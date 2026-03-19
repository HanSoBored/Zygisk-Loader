#include <jni.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <time.h>
#include <dlfcn.h>
#include <android/log.h>
#include "zygisk.h"

// --- CONFIGURATION ---
#define LOG_TAG "Zygisk_Loader"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// JSON configuration path
#define CONFIG_PATH "/data/adb/modules/zygisk-loader/config/target.json"

// --- GLOBAL VARIABLES ---
static JavaVM *g_jvm = NULL;
static uint8_t *g_payload_buffer = NULL;
static size_t g_payload_size = 0;
static bool g_target_app_detected = false;

// --- UTILITY FUNCTIONS ---

static uint32_t rand_int() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_nsec ^ ts.tv_sec);
}

// Write buffer to file
static bool write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, size, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return written == size;
}

// Reading the entire file into memory (with null_terminate option for JSON/String parsing)
static bool read_file_to_memory(const char *path, uint8_t **out_buffer, size_t *out_size, bool null_terminate) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        return false;
    }

    size_t alloc_size = null_terminate ? fsize + 1 : fsize;
    uint8_t *buf = (uint8_t *)malloc(alloc_size);
    if (!buf) {
        fclose(f);
        return false;
    }

    size_t read_sz = fread(buf, 1, fsize, f);
    fclose(f);

    if (read_sz != (size_t)fsize) {
        free(buf);
        return false;
    }

    if (null_terminate) {
        buf[fsize] = '\0';
    }

    *out_buffer = buf;
    *out_size = fsize;
    return true;
}

// Native lightweight JSON parser with strict key-value matching.
// Searches for "app" key, validates the value, then extracts corresponding "lib" path.
static bool get_payload_path(const char *json_data, const char *app_name, char *out_path, size_t max_len) {
    const char *ptr = json_data;
    while ((ptr = strstr(ptr, "\"app\"")) != NULL) {
        ptr += 5; // skip "\"app\""

        // Skip whitespaces and colon
        while (*ptr == ' ' || *ptr == '\t' || *ptr == ':' || *ptr == '\n' || *ptr == '\r') ptr++;

        if (*ptr == '"') {
            ptr++; // skip opening quote
            const char *val_end = strchr(ptr, '"');
            if (val_end) {
                size_t val_len = val_end - ptr;
                // Strict comparison: ensure exact match of package name
                if (strncmp(ptr, app_name, val_len) == 0 && strlen(app_name) == val_len) {
                    // Match found! Now find "lib" within this object scope
                    const char *lib_ptr = strstr(val_end, "\"lib\"");
                    if (lib_ptr) {
                        lib_ptr += 5;
                        while (*lib_ptr == ' ' || *lib_ptr == '\t' || *lib_ptr == ':') lib_ptr++;
                        if (*lib_ptr == '"') {
                            lib_ptr++;
                            const char *lib_end = strchr(lib_ptr, '"');
                            if (lib_end) {
                                size_t lib_len = lib_end - lib_ptr;
                                if (lib_len >= max_len) lib_len = max_len - 1;
                                strncpy(out_path, lib_ptr, lib_len);
                                out_path[lib_len] = '\0';
                                return true;
                            }
                        }
                    }
                }
                ptr = val_end;
            }
        }
    }
    return false;
}

// Helper for reading jstrings into a safe C buffer
static void get_jstring_utf(JNIEnv *env, jstring *jstr_ptr, char *out_buf, size_t buf_size) {
    out_buf[0] = '\0';
    if (jstr_ptr && *jstr_ptr) {
        const char *str = (*env)->GetStringUTFChars(env, *jstr_ptr, NULL);
        if (str) {
            strncpy(out_buf, str, buf_size - 1);
            out_buf[buf_size - 1] = '\0';
            (*env)->ReleaseStringUTFChars(env, *jstr_ptr, str);
        }
    }
}

// --- ZYGISK LIFECYCLE ---

static void onLoad(struct zygisk_api *api, JNIEnv *env) {
    (void)api;
    if ((*env)->GetJavaVM(env, &g_jvm) != JNI_OK) {
        LOGE("Failed to get JavaVM");
    }
    LOGI("Zygisk-Loader Initialized");
}

static void preAppSpecialize(void *impl, struct zygisk_app_specialize_args *args) {
    (void)impl;

    JNIEnv *env = NULL;
    if (g_jvm && (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        char process_name[256] = {0};

        // Try extracting from nice_name (Fast-path)
        get_jstring_utf(env, args->nice_name, process_name, sizeof(process_name));

        // Fallback for isolated processes if nice_name is empty
        if (strlen(process_name) == 0) {
            char app_data_dir[256] = {0};
            get_jstring_utf(env, args->app_data_dir, app_data_dir, sizeof(app_data_dir));
            const char *last_slash = strrchr(app_data_dir, '/');
            if (last_slash) {
                strncpy(process_name, last_slash + 1, sizeof(process_name) - 1);
            }
        }

        if (strlen(process_name) == 0) return;

        // SAFE: Get Base Package Name (strip service postfix like ":bg", ":remote", etc.)
        char base_process_name[256] = {0}; // Initialize with zeros for safety
        strncpy(base_process_name, process_name, sizeof(base_process_name) - 1);
        base_process_name[sizeof(base_process_name) - 1] = '\0'; // Ensure null-termination
        char *colon = strchr(base_process_name, ':');
        if (colon) *colon = '\0';

        // 1. Read target.json config to memory as null-terminated string
        uint8_t *json_data = NULL;
        size_t json_size = 0;
        if (!read_file_to_memory(CONFIG_PATH, &json_data, &json_size, true)) {
            return; // Config file not found or access denied
        }

        char payload_path[512] = {0};

        // 2. Check if base_process_name exists in JSON and get its payload path
        if (get_payload_path((const char*)json_data, base_process_name, payload_path, sizeof(payload_path))) {
            LOGI("Target Detected: %s (Base: %s)", process_name, base_process_name);
            LOGI("Dynamic Payload Path: %s", payload_path);
            g_target_app_detected = true;

            // 3. Read app-specific payload .so to RAM
            if (read_file_to_memory(payload_path, &g_payload_buffer, &g_payload_size, false)) {
                LOGI("Payload buffered to RAM: %zu bytes", g_payload_size);
            } else {
                LOGE("Failed to buffer payload from: %s", payload_path);
            }
        }

        free(json_data);
    }
}

static void postAppSpecialize(void *impl, const struct zygisk_app_specialize_args *args) {
    (void)impl;

    // Prevent execution in non-target apps or if payload failed to load
    if (!g_target_app_detected || !g_payload_buffer) return;

    JNIEnv *env = NULL;
    if (g_jvm && (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) {

        char app_data_dir[512] = {0};
        get_jstring_utf(env, args->app_data_dir, app_data_dir, sizeof(app_data_dir));

        if (strlen(app_data_dir) == 0) {
            LOGE("Could not determine app data directory");
            goto cleanup;
        }

        // Generate random file name /cache/.res_[rand].so
        char file_name[1024];
        snprintf(file_name, sizeof(file_name), "%s/cache/.res_%u.so", app_data_dir, rand_int());

        LOGI("Attempting injection to: %s", file_name);

        // Write buffer to Cache -> Dlopen -> Unlink
        if (write_file(file_name, g_payload_buffer, g_payload_size)) {

            void *handle = dlopen(file_name, RTLD_NOW);

            // Delete files instantly for stealth
            unlink(file_name);

            if (!handle) {
                LOGE("Injection failed: %s", dlerror());
            } else {
                LOGI("Injection success! Handle: %p", handle);
            }
        } else {
            LOGE("Failed to write payload to cache");
        }
    }

cleanup:
    // Free RAM buffer to prevent memory leak
    if (g_payload_buffer) {
        free(g_payload_buffer);
        g_payload_buffer = NULL;
        g_payload_size = 0;
    }
}

// Method server tidak diperlukan
static void preServerSpecialize(void *impl, struct zygisk_server_specialize_args *args) { (void)impl; (void)args; }
static void postServerSpecialize(void *impl, const struct zygisk_server_specialize_args *args) { (void)impl; (void)args; }

static struct zygisk_module_abi module_abi;

__attribute__((visibility("default")))
void zygisk_module_entry(struct zygisk_api *api, JNIEnv *env) {
    module_abi.api_version = ZYGISK_API_VERSION;
    module_abi.impl = api;
    module_abi.preAppSpecialize = preAppSpecialize;
    module_abi.postAppSpecialize = postAppSpecialize;
    module_abi.preServerSpecialize = preServerSpecialize;
    module_abi.postServerSpecialize = postServerSpecialize;

    if (api->registerModule(api, &module_abi)) {
        onLoad(api, env);
    }
}

__attribute__((visibility("default")))
void zygisk_companion_entry(int client) {
    close(client);
}
