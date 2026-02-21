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

// --- KONFIGURASI ---
#define LOG_TAG "Zygisk_Loader"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CONFIG_PATH "/data/adb/modules/zygisk-loader/config/target"
#define SOURCE_PAYLOAD_PATH "/data/adb/modules/zygisk-loader/config/payload.so"

// --- GLOBAL VARIABLES ---
static JavaVM *g_jvm = NULL;
static char g_target_package[256] = {0};
static uint8_t *g_payload_buffer = NULL;
static size_t g_payload_size = 0;
static bool g_target_app_detected = false;

// --- UTILITY FUNCTIONS ---

// Pengganti rand_int()
static uint32_t rand_int() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_nsec ^ ts.tv_sec);
}

// Menulis buffer ke file
static bool write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, size, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return written == size;
}

// Membaca file utuh ke memori (RAM Buffer)
static bool read_file_to_memory(const char *path, uint8_t **out_buffer, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fsize <= 0) {
        fclose(f);
        return false;
    }
    
    uint8_t *buf = (uint8_t *)malloc(fsize);
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
    
    *out_buffer = buf;
    *out_size = fsize;
    return true;
}

// Membaca file konfigurasi (Mendapatkan Target)
static void read_target_config() {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (f) {
        if (fgets(g_target_package, sizeof(g_target_package), f)) {
            // Hilangkan karakter newline / spasi di akhir
            g_target_package[strcspn(g_target_package, "\r\n")] = 0;
        }
        fclose(f);
    }
}

// Helper untuk membaca jstring ke buffer C yang aman
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
    LOGI("Zygisk-Loader Initialized (Pure C)");
}

static void preAppSpecialize(void *impl, struct zygisk_app_specialize_args *args) {
    (void)impl;
    
    // 1. Baca Konfigurasi Target (Sebagai Root/Zygote)
    read_target_config();
    
    if (strlen(g_target_package) == 0) return;

    JNIEnv *env = NULL;
    if (g_jvm && (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        char process_name[256] = {0};
        
        // Coba dapatkan dari nice_name (Fast-path)
        get_jstring_utf(env, args->nice_name, process_name, sizeof(process_name));

        // Jika kosong, ekstrak dari app_data_dir (Fallback untuk isolated process)
        if (strlen(process_name) == 0) {
            char app_data_dir[256] = {0};
            get_jstring_utf(env, args->app_data_dir, app_data_dir, sizeof(app_data_dir));
            const char *last_slash = strrchr(app_data_dir, '/');
            if (last_slash) {
                strncpy(process_name, last_slash + 1, sizeof(process_name) - 1);
            }
        }

        // Cek apakah target sesuai dengan konfigurasi
        if (strstr(process_name, g_target_package) != NULL) {
            LOGI("Target Detected: %s", process_name);
            g_target_app_detected = true;

            // 2. Baca file Payload .so ke dalam RAM
            if (read_file_to_memory(SOURCE_PAYLOAD_PATH, &g_payload_buffer, &g_payload_size)) {
                LOGI("Payload buffered to RAM: %zu bytes", g_payload_size);
            } else {
                LOGE("Failed to buffer payload from: %s", SOURCE_PAYLOAD_PATH);
            }
        }
    }
}

static void postAppSpecialize(void *impl, const struct zygisk_app_specialize_args *args) {
    (void)impl;
    
    // Cegah eksekusi di aplikasi yang bukan target atau jika gagal load buffer
    if (!g_target_app_detected || !g_payload_buffer) return;

    JNIEnv *env = NULL;
    if (g_jvm && (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        
        char app_data_dir[512] = {0};
        get_jstring_utf(env, args->app_data_dir, app_data_dir, sizeof(app_data_dir));

        if (strlen(app_data_dir) == 0) {
            LOGE("Could not determine app data directory");
            goto cleanup;
        }

        // Generate nama file random /cache/.res_[rand].so
        char file_name[1024];
        snprintf(file_name, sizeof(file_name), "%s/cache/.res_%u.so", app_data_dir, rand_int());

        LOGI("Attempting injection to: %s", file_name);

        // Tulis buffer ke Cache -> Dlopen -> Unlink
        if (write_file(file_name, g_payload_buffer, g_payload_size)) {
            
            void *handle = dlopen(file_name, RTLD_NOW);
            
            // Hapus file secara instan untuk stealth
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
    // Hapus memori buffer agar tidak menjadi memory leak di sisi target aplikasi
    if (g_payload_buffer) {
        free(g_payload_buffer);
        g_payload_buffer = NULL;
        g_payload_size = 0;
    }
}

// Kita tidak membutuhkan method server
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
    // Companion tidak digunakan, cukup di-close
    close(client);
}
