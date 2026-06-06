#include <jni.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <errno.h>
#include <android/dlext.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <elf.h>
#include <ctype.h>
#include <android/log.h>
#include "zygisk.h"

void zygisk_module_entry(struct zygisk_api *api, JNIEnv *env);
void zygisk_companion_entry(int client);

// --- CONFIGURATION ---
#define LOG_TAG "Zygisk-Loader"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

// JSON configuration path
#define CONFIG_PATH "/data/adb/modules/zygisk-loader/config/target.json"

// Maximum payload .so file size (16 MB)
#define MAX_PAYLOAD_SIZE ((size_t)16 * 1024 * 1024)
// Maximum JSON config file size (64 KB)
#define MAX_CONFIG_SIZE  ((size_t)64 * 1024)

// Maximum nesting depth for recursive JSON value skipping.
// Prevents stack overflow on deeply nested or malformed JSON input.
// Standard JSON parsers (cJSON, yyjson) use similar limits (100-1000).
#define MAX_JSON_NESTING_DEPTH 100

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

// --- GLOBAL VARIABLES ---
// These globals communicate state between preAppSpecialize() and
// postAppSpecialize() within the same forked child process.
// The Zygote fork+COW model ensures each app gets its own copy --
// no cross-app contamination, no locking needed.
// If threading is added in the future, these WILL need synchronization.
//
static JavaVM *g_jvm = NULL;
static uint8_t *g_payload_buffer = NULL;
static size_t g_payload_size = 0;
static bool g_target_app_detected = false;

// Re-entry guard for postAppSpecialize. The Zygisk lifecycle guarantees
// a single call per process, but this prevents resource leaks (second
// memfd_create, fd leak, double dlopen) if behavior changes.
static bool g_payload_injected = false;

// --- UTILITY FUNCTIONS ---

// Portable secure memory zeroing — prevents compiler elision.
// explicit_bzero() requires __ANDROID_API__ >= 28 (Android 9 / API 28),
// which is our minSdkVersion — safe for all supported devices.
// memset_s() is not reliably available in NDK across all target levels.
// __attribute__((noinline)) guards against LTO-based elision of the
// volatile-store fallback (API < 28).
static __attribute__((noinline)) void secure_zero(void *ptr, size_t n) {
    if (!ptr || n == 0) return;
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 28
    explicit_bzero(ptr, n);
#else
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    for (size_t i = 0; i < n; i++) p[i] = 0;
#endif
}

// Check if a character is valid in a JSON primitive token (number, bool, null).
// Accepts alphanumeric characters and the sign/exponent/decimal chars
// used in JSON number literals. Used to advance past non-string values
// (numbers, true, false, null) when scanning for the next key-value pair.
static bool is_json_primitive_char(char c) {
    return isalnum((unsigned char)c) || c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E';
}

// Shared read loop: reads file_size bytes from an open fd into a newly allocated buffer.
// Buffer is null-terminated if requested. On success, *out points to buffer, *out_size = file_size.
static bool read_file_content(int fd, size_t file_size, uint8_t **out, size_t *out_size,
                               bool null_terminate) {
    bool success = false;
    uint8_t *buf = NULL;

    if (file_size == 0) {
        LOGE("File is empty");
        return false;
    }

    buf = (uint8_t *)malloc(file_size + (null_terminate ? 1 : 0));
    if (!buf) {
        LOGE("Failed to allocate %zu bytes for file read", file_size);
        return false;
    }

    size_t total_read = 0;
    while (total_read < file_size) {
        ssize_t n = read(fd, buf + total_read, file_size - total_read);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("Failed to read file (errno=%d)", errno);
            goto cleanup;
        }
        if (n == 0) {
            LOGE("Short read: %zu of %zu bytes", total_read, file_size);
            goto cleanup;
        }
        total_read += (size_t)n;
    }

    if (null_terminate) {
        buf[file_size] = '\0';
    }

    *out = buf;
    *out_size = file_size;
    success = true;

cleanup:
    if (!success) {
        secure_zero(buf, file_size);
        free(buf);
    }
    return success;
}

// Shared file reader: open, validate, read, close.
// Uses fstat for TOCTOU-safe size detection. Handles both binary and text
// files via parameters:
//   - max_size: maximum allowed file size (16MB for payload, 64KB for config)
//   - null_terminate: whether to add a NUL terminator after file content
//   - check_regular: if true, rejects non-regular files via S_ISREG
static bool read_file_generic(const char *path, uint8_t **out, size_t *out_size,
                               size_t max_size, bool null_terminate, bool check_regular) {
    if (!path) { LOGE("NULL path"); return false; }

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        LOGE("Failed to open: %s (errno=%d)", path, errno);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        LOGE("Failed to fstat: %s (errno=%d)", path, errno);
        close(fd); return false;
    }
    if (check_regular && !S_ISREG(st.st_mode)) {
        LOGE("Not a regular file: %s", path);
        close(fd); return false;
    }
    if (st.st_size < 0) {
        LOGE("fstat returned negative size");
        close(fd);
        return false;
    }
    if (st.st_size == 0) {
        LOGE("File is empty: %s", path);
        close(fd); return false;
    }
    if ((uint64_t)st.st_size > max_size) {
        LOGE("File exceeds maximum size (%zu bytes): %s", max_size, path);
        close(fd); return false;
    }

    bool ok = read_file_content(fd, (size_t)st.st_size, out, out_size, null_terminate);
    close(fd);
    return ok;
}

// Read binary file (.so payload) — wraps read_file_generic with payload constraints.
static bool read_binary_file(const char *path, uint8_t **out_buffer, size_t *out_size) {
    return read_file_generic(path, out_buffer, out_size,
                             MAX_PAYLOAD_SIZE, false, true);
}

// Read text file (JSON config) — wraps read_file_generic with config constraints.
// null_terminated for string-safe parsing. Regular-file check skipped because
// config files may be managed by root tools that use special file types.
static bool read_text_file(const char *path, uint8_t **out_buffer, size_t *out_size) {
    return read_file_generic(path, out_buffer, out_size,
                             MAX_CONFIG_SIZE, true, false);
}

// Escape-aware JSON string scanner.
// Scans from the opening quote and finds the matching closing quote,
// correctly handling all JSON escape sequences:
//   \" \\ \/ \b \f \n \r \t \uXXXX
// Returns pointer to the closing quote on success, NULL on error.
static const char* scan_json_string(const char *start, const char *end_limit) {
    if (!start || start >= end_limit || *start != '"') return NULL;
    const char *p = start + 1;
    while (p < end_limit && *p) {
        if (*p == '\\') {
            p++;
            if (p >= end_limit || !*p) return NULL;
            switch (*p) {
                case '"': case '\\': case '/':
                case 'b': case 'f': case 'n': case 'r': case 't':
                    p++;
                    break;
                case 'u':
                    // \uXXXX is included for full JSON spec compliance (RFC 8259).
                    // Config files are typically ASCII, but programmatic generators
                    // (manager apps, scripts) may emit \uXXXX escapes.
                    p++;
                    for (int i = 0; i < 4; i++) {
                        if (p >= end_limit || !*p) return NULL;
                        if (!((*p >= '0' && *p <= '9') ||
                              (*p >= 'a' && *p <= 'f') ||
                              (*p >= 'A' && *p <= 'F')))
                            return NULL;
                        p++;
                    }
                    break;
                default:
                    return NULL;  // Invalid escape sequence
            }
        } else if (*p == '"') {
            return p;  // Found closing quote
        } else {
            p++;
        }
    }
    return NULL;  // Unterminated string
}

// Advance past whitespace, commas, and colons (JSON structural separators).
// Returns pointer to the first non-separator character.
// Logs a warning if multiple consecutive structural separators are found
// (e.g., ",," or ".:"), which indicates malformed JSON in target.json.
// The permissive behavior is kept for compatibility, but the warning helps
// users debug misconfigured files.
static const char* skip_json_separators(const char *p, const char *end_limit) {
    int commas = 0, colons = 0;
    while (p < end_limit && (*p == ' ' || *p == '\t' || *p == '\n' ||
           *p == '\r' || *p == ',' || *p == ':')) {
        if (*p == ',') commas++;
        else if (*p == ':') colons++;
        p++;
    }
    if (commas + colons >= 2) {
        LOGW("Malformed JSON: %d comma(s) and %d colon(s) at consecutive positions in target.json",
             commas, colons);
    }
    return p;
}

// Forward declaration for mutual recursion with skip_bracketed.
static const char* skip_json_value(const char *p, const char *end_limit, int depth);

// Parameterized helper for skipping bracketed JSON values ({}/[]).
// Eliminates the duplicated object/array skip logic — both follow the
// same depth-tracking pattern differing only by bracket characters.
static const char* skip_bracketed(const char *p, const char *end_limit,
                                    char open_bracket, char close_bracket, int depth) {
    int nested_depth = 1;
    p++;
    while (nested_depth > 0 && p < end_limit && *p) {
        if (*p == open_bracket) nested_depth++;
        else if (*p == close_bracket) nested_depth--;
        else if (*p == '"') {
            const char *se = scan_json_string(p, end_limit);
            if (!se) return NULL;
            p = se + 1;
            continue;
        } else if (*p == '{' || *p == '[') {
            p = skip_json_value(p, end_limit, depth);
            if (!p) return NULL;
            continue;
        }
        if (nested_depth > 0) p++;
    }
    return (nested_depth == 0) ? p + 1 : NULL;
}

// Skip a complete JSON value (object, array, or single character).
// Handles nested objects/arrays, escaped strings, and returns pointer
// past the end of the value on success, NULL on malformed input.
static const char* skip_json_value(const char *p, const char *end_limit, int depth) {
    if (++depth > MAX_JSON_NESTING_DEPTH) {
        LOGE("JSON nesting depth exceeded (%d)", depth);
        return NULL;
    }
    if (p >= end_limit) { return NULL; }
    const char *result = NULL;
    if (*p == '{') result = skip_bracketed(p, end_limit, '{', '}', depth);
    else if (*p == '[') result = skip_bracketed(p, end_limit, '[', ']', depth);
    if (!result) { return NULL; }
    return result;
}

// Returns true if a STRING value was read (val_start/val_len are set).
// Returns false if the value was a non-string (number, bool, null, object, or array).
// In BOTH cases, *cursor is advanced past whatever value was present.
// Callers MUST continue scanning when false is returned; the key was
// consumed but the value was not a string.
static bool try_read_json_string_key_value(const char **cursor, const char *end_limit,
                         const char **key_start, size_t *key_len,
                         const char **val_start, size_t *val_len) {
    const char *p = *cursor;

    // Read key string
    const char *key_end = scan_json_string(p, end_limit);
    if (!key_end) {
        *cursor = p + 1;  // Advance past opening quote to prevent infinite loop
        return false;
    }
    *key_start = p + 1;
    *key_len = (size_t)(key_end - *key_start);
    p = key_end + 1;

    // Skip to value
    p = skip_json_separators(p, end_limit);

    // Read string value (non-string values are skipped)
    if (p < end_limit && *p == '"') {
        const char *val_end = scan_json_string(p, end_limit);
        if (!val_end) return false;
        *val_start = p + 1;
        *val_len = (size_t)(val_end - *val_start);
        p = val_end + 1;
        *cursor = p;
        return true;
    }
    // Non-string value — skip it and continue scanning
    if (p < end_limit && *p != '"' && *p != '{' && *p != '[') {
        LOGW("Non-string value for key '%.*s', expected a string path",
             (int)*key_len, *key_start);
        while (p < end_limit && is_json_primitive_char(*p)) {
            p++;
        }
    } else if (p < end_limit && (*p == '{' || *p == '[')) {
        p = skip_json_value(p, end_limit, 0);
        if (!p) {
            *cursor = *cursor + 1;  // Advance past bad char to prevent desync
            return false;
        }
    } else if (p < end_limit) {
        p++;
    }
    *val_start = NULL;
    *val_len = 0;
    *cursor = p;
    return false; // did not read a string value
}

// Try to match the next key in a JSON object against a target name.
// Reads the key-value pair at *cursor via try_read_json_string_key_value.
// If the key matches and the value is a string, sets outputs and returns true.
// On non-match (key differs or value is non-string), *cursor is still advanced
// past the value for the caller to continue scanning.
static bool try_match_object_key(const char **cursor, const char *end_limit,
                                  const char *key_name, size_t key_len,
                                  const char **out_val_start, size_t *out_val_len) {
    const char *p = *cursor;
    const char *key_start = NULL, *val_start = NULL;
    size_t kv_key_len = 0, val_len = 0;

    if (!try_read_json_string_key_value(&p, end_limit, &key_start, &kv_key_len,
                                  &val_start, &val_len)) {
        *cursor = p;
        return false;
    }

    if (kv_key_len == key_len && memcmp(key_start, key_name, key_len) == 0) {
        *out_val_start = val_start;
        *out_val_len = val_len;
        *cursor = p;
        return true;
    }

    *cursor = p;
    return false;
}

// Shared helper: scan a JSON object body for a key with a given name.
// If found and the value is a string, return pointers via out_val_start/out_val_len.
// Nested objects/arrays inside values are skipped via skip_json_value()
// instead of depth tracking — this correctly handles any nesting level
// without premature exit from the enclosing object.
// cursor: on entry, points to the character after '{'.
//         on exit,  points past the matching '}'.
static bool scan_json_object_for_key(const char **cursor, const char *end_limit,
                                      const char *key_name, size_t key_len,
                                      const char **out_val_start, size_t *out_val_len) {
    const char *p = *cursor;
    int depth = 1;

    while (depth > 0 && p < end_limit) {
        p = skip_json_separators(p, end_limit);
        if (p >= end_limit || !*p) return false;

        if (*p == '}') {
            depth--;
            if (depth == 0) break;
            p++;
            continue;
        }
        if (*p == ']') {
            LOGE("Unmatched ']'");
            return false;
        }

        if (*p == '"') {
            if (try_match_object_key(&p, end_limit, key_name, key_len,
                                      out_val_start, out_val_len)) {
                *cursor = p;
                return true;
            }
            // p already advanced past non-matching key
        } else {
            LOGW("Unexpected non-string character '%c' in JSON object body", *p);
            p++;
        }
    }

    *cursor = (p < end_limit) ? (p + 1) : p;
    return false;
}

// Scan a single JSON object body, looking for the "app" key matching app_name
// and extracting the corresponding "lib" value.
// Uses a two-pass approach (first find app, then extract lib) — re-scans the
// object body from the start for each key. This is O(2n) but is simpler and
// safer than a single-pass approach; config files are small enough that
// performance is irrelevant.
// Updates cursor to point past the closing '}'.
// Returns true if app found and lib path extracted.
static bool scan_app_entry(const char **cursor, const char *end_limit,
                           const char *app_name, size_t app_len,
                           const char **out_lib_start, size_t *out_lib_len,
                           size_t max_len) {
    const char *obj_start = *cursor;
    const char *val_start = NULL;
    size_t val_len = 0;

    // Pass 1: find the matching "app" key
    const char *scan = obj_start;
    if (!scan_json_object_for_key(&scan, end_limit, "app", sizeof("app") - 1, &val_start, &val_len))
        return false;
    if (val_len != app_len || memcmp(val_start, app_name, app_len) != 0)
        return false;

    // Pass 2: re-scan from object body start to find "lib"
    scan = obj_start;
    if (!scan_json_object_for_key(&scan, end_limit,
                                  "lib", sizeof("lib") - 1,
                                  out_lib_start, &val_len))
        return false;
    if (max_len == 0) {
        LOGE("max_len is 0, cannot store lib path");
        return false;
    }
    if (val_len >= max_len) {
        LOGE("Lib path too long (%zu chars, max %zu)", val_len, max_len);
        return false;
    }
    *out_lib_len = val_len;

    *cursor = scan;
    return true;
}

// Forward-only JSON parser that scans one object at a time.
// For each object, matches the "app" key and extracts the corresponding "lib" path.
// Handles escaped quotes and braces inside strings by using escape-aware
// string scanning (does not use strchr for string boundary detection).
static bool get_payload_path(const char *json, size_t json_size, const char *app_name,
                              char *out_path, size_t max_len) {
    const char *p = json;
    const char *end_limit = json + json_size;
    size_t app_len = strlen(app_name);
    if (app_len == 0) {
        LOGE("Empty app_name in get_payload_path");
        return false;
    }

    while (p < end_limit && *p) {
        // Skip string content before looking for '{' to avoid
        // matching braces inside string values
        while (p < end_limit && *p && *p != '{') {
            if (*p == '"') {
                const char *str_end = scan_json_string(p, end_limit);
                if (!str_end) return false;
                p = str_end + 1;
                // Defense-in-depth: catches scan_json_string() false positives
                // by verifying the character after a quoted string is a valid
                // JSON structural separator (,:}]). If not, the quote was premature.
                const char *next = skip_json_separators(p, end_limit);
                if (next && next < end_limit && *next != ',' && *next != ':' &&
                    *next != '}' && *next != ']') {
                    LOGE("Unexpected character after JSON string: '%c'", *next);
                    return false;
                }
            } else {
                p++;  // skip non-string character
            }
        }
        if (p >= end_limit || !*p) return false;

        // Found a JSON object — scan it for app/lib entries
        const char *obj_body = p + 1;
        const char *lib_val_start = NULL;
        size_t lib_val_len = 0;

        if (scan_app_entry(&obj_body, end_limit, app_name, app_len,
                           &lib_val_start, &lib_val_len, max_len)) {
            memcpy(out_path, lib_val_start, lib_val_len);
            out_path[lib_val_len] = '\0';
            return true;
        }
        p = obj_body;
    }
    return false;
}

// Helper: safely copy a jstring (via pointer) into a fixed-size C buffer.
// Returns true on success, false if the string was NULL, truncated, or JNI failed.
// On truncation, the buffer is set to empty and false is returned — callers should
// check the return value and handle truncation as a hard failure rather than using
// silently truncated data for sensitive operations (process matching, paths).
static bool jstring_to_cstr(JNIEnv *env, jstring *jstr_ptr, char *out_buf, size_t buf_size) {
    out_buf[0] = '\0';
    if (jstr_ptr && *jstr_ptr) {
        const char *str = (*env)->GetStringUTFChars(env, *jstr_ptr, NULL);
        if (!str) return false;
        size_t src_len = strlen(str);
        if (src_len >= buf_size) {
            LOGW("JNI string truncated: %zu chars into %zu buffer", src_len, buf_size);
            (*env)->ReleaseStringUTFChars(env, *jstr_ptr, str);
            return false;
        }
        memcpy(out_buf, str, src_len + 1);
        (*env)->ReleaseStringUTFChars(env, *jstr_ptr, str);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            LOGW("JNI exception cleared after ReleaseStringUTFChars");
        }
    }
    return true;
}

// Helper: get the JNI environment for the current Zygote thread.
// Wraps GetEnv with a NULL check on g_jvm. Called from preAppSpecialize and
// postAppSpecialize — only 2 call sites, but the wrapper keeps the null-check
// logic in one place. Returns NULL if g_jvm is not set or GetEnv fails.
static JNIEnv* get_jni_env(void) {
    JNIEnv *local_env = NULL;
    if (!g_jvm || (*g_jvm)->GetEnv(g_jvm, (void**)&local_env, JNI_VERSION_1_6) != JNI_OK)
        return NULL;
    return local_env;
}

// --- ZYGISK LIFECYCLE ---

static void onLoad(struct zygisk_api *api, JNIEnv *env) {
    (void)api;
    if ((*env)->GetJavaVM(env, &g_jvm) != JNI_OK) {
        LOGE("Failed to get JavaVM");
        // g_jvm remains NULL; all subsequent get_jni_env() calls will fail.
        // Early return -- injection cannot proceed without a valid JVM.
        return;
    }
    LOGI("Zygisk-Loader Initialized");
}

// Resolve the process name from Zygisk specialization args.
// Returns true if a valid process name was resolved.
static bool resolve_process_name(JNIEnv *env,
                                 struct zygisk_app_specialize_args *args,
                                 char *out_name, size_t name_size,
                                 char *out_base, size_t base_size) {
    if (!args) { LOGE("resolve_process_name: NULL args"); return false; }
    // Try extracting from nice_name (Fast-path)
    if (!jstring_to_cstr(env, args->nice_name, out_name, name_size)) {
        // jstring_to_cstr logged a warning on truncation; out_name is "".
        // Fallback path below handles isolated processes.
    }

    // Fallback for isolated processes if nice_name is empty
    if (strlen(out_name) == 0) {
        char app_data_dir[512] = {0};
        if (!jstring_to_cstr(env, args->app_data_dir, app_data_dir, sizeof(app_data_dir))) {
            LOGW("Failed to read app_data_dir for fallback name resolution (nice_name='%s')", out_name);
        }
        const char *last_slash = strrchr(app_data_dir, '/');
        if (last_slash) {
            size_t sub_len = strlen(last_slash + 1);
            size_t copy_len = (sub_len < name_size - 1) ? sub_len : name_size - 1;
            if (sub_len >= name_size - 1) {
                LOGW("Process name truncated from %zu to %zu chars in fallback path",
                     sub_len, name_size - 1);
            }
            memcpy(out_name, last_slash + 1, copy_len);
            out_name[copy_len] = '\0';
        }
        secure_zero(app_data_dir, sizeof(app_data_dir));
    }

    if (strlen(out_name) == 0) return false;

    // Get Base Package Name (strip service postfix like ":bg", ":remote", etc.)
    size_t name_len = strlen(out_name);
    size_t copy_len = (name_len < base_size - 1) ? name_len : base_size - 1;
    memcpy(out_base, out_name, copy_len);
    out_base[copy_len] = '\0';
    char *colon = strchr(out_base, ':');
    if (colon) *colon = '\0';

    return true;
}

// Phase 1: Read config JSON + find matching app entry → payload path
static bool resolve_payload_path(const char *base_process_name,
                                  char *payload_path_out, size_t payload_path_max) {
    uint8_t *json_data = NULL;
    size_t json_size = 0;
    if (!read_text_file(CONFIG_PATH, &json_data, &json_size))
        return false;
    bool found = get_payload_path((const char*)json_data, json_size,
                                   base_process_name,
                                   payload_path_out, payload_path_max);
    if (!found) {
        LOGD("App '%s' not found in config", base_process_name);
    }
    free(json_data);
    return found;
}

// Release payload buffer with secure zeroing.
// Called from postAppSpecialize (always) and on validation failure.
// The buffer is zeroed before freeing to prevent sensitive .so content
// from lingering in the heap after the injection decision is complete.
static void release_payload(void) {
    if (g_payload_buffer) {
        secure_zero(g_payload_buffer, g_payload_size);
        free(g_payload_buffer);
        g_payload_buffer = NULL;
        g_payload_size = 0;
    }
    g_target_app_detected = false;
}

// Compile-time ELF type aliases — eliminates 32/64-bit code duplication.
// The module is compiled per-ABI, so the payload must match the host bitness.
#if defined(__LP64__)
    #define VALIDATE_EHDR Elf64_Ehdr
    #define VALIDATE_PHDR Elf64_Phdr
    #define VALIDATE_PHDR_SIZE sizeof(Elf64_Phdr)
    #define VALIDATE_ELFCLASS ELFCLASS64
#else
    #define VALIDATE_EHDR Elf32_Ehdr
    #define VALIDATE_PHDR Elf32_Phdr
    #define VALIDATE_PHDR_SIZE sizeof(Elf32_Phdr)
    #define VALIDATE_ELFCLASS ELFCLASS32
#endif

// Uses %zu / (size_t) for e_phoff since all values are bounds-checked
// against g_payload_size (size_t) before printing.
#define VALIDATE_EHDR_FMT "%zu"

// ELF identification check: magic, class, and endianness.
static bool elf_check_ident(void) {
    if (g_payload_size < EI_NIDENT) {
        LOGE("Payload too small for ELF identification (%zu bytes)", g_payload_size);
        return false;
    }

    unsigned char *ident = (unsigned char *)g_payload_buffer;
    if (memcmp(ident, "\x7f" "ELF", 4) != 0) {
        LOGE("Payload missing ELF magic"); return false;
    }
    if (ident[EI_CLASS] != VALIDATE_ELFCLASS) {
        LOGE("Invalid ELF class: %d (expected %d)", ident[EI_CLASS], VALIDATE_ELFCLASS);
        return false;
    }
    if (ident[EI_DATA] != ELFDATA2LSB) {
        LOGE("Payload is not little-endian"); return false;
    }
    return true;
}

// ELF architecture match — extracted as a standalone helper matching
// AOSP linker's GetTargetElfMachine() pattern.
static bool elf_check_architecture(VALIDATE_EHDR *ehdr) {
#if defined(__aarch64__)
    if (ehdr->e_machine != EM_AARCH64) {
        LOGE("Payload arch mismatch: expected AARCH64, got %d", (int)ehdr->e_machine);
        return false;
    }
#elif defined(__ARM_ARCH)
    if (ehdr->e_machine != EM_ARM) {
        LOGE("Payload arch mismatch: expected ARM, got %d", (int)ehdr->e_machine);
        return false;
    }
#elif defined(__x86_64__)
    if (ehdr->e_machine != EM_X86_64) {
        LOGE("Payload arch mismatch: expected x86_64, got %d", (int)ehdr->e_machine);
        return false;
    }
#elif defined(__i386__)
    if (ehdr->e_machine != EM_386) {
        LOGE("Payload arch mismatch: expected i386, got %d", (int)ehdr->e_machine);
        return false;
    }
#else
#error "Unsupported target architecture"
#endif  // architecture
    return true;
}

// ELF header validation: type, program headers, architecture match.
// Each bound is checked individually against different constraints
// (header overlap, file bounds, overflow), so no abstraction is needed.
static bool elf_check_header(void) {
    if (g_payload_size < sizeof(VALIDATE_EHDR)) {
        LOGE("Payload too small for ELF header (%zu bytes)", g_payload_size);
        return false;
    }

    VALIDATE_EHDR ehdr;
    memcpy(&ehdr, g_payload_buffer, sizeof(ehdr));
    _Static_assert(sizeof(VALIDATE_EHDR) <= 128,
                   "VALIDATE_EHDR exceeds expected ELF header size");

    if (ehdr.e_type != ET_DYN) {
        LOGE("Payload is not a shared library (e_type=%d)", ehdr.e_type);
        return false;
    }
    if (ehdr.e_phnum == 0) {
        LOGE("Payload has no program headers");
        return false;
    }
    if (ehdr.e_ehsize != sizeof(VALIDATE_EHDR)) {
        LOGE("ELF header size mismatch: %d (expected %zu)",
             ehdr.e_ehsize, sizeof(VALIDATE_EHDR));
        return false;
    }
    if (ehdr.e_phentsize != sizeof(VALIDATE_PHDR)) {
        LOGE("Program header entry size mismatch: %d (expected %zu)",
             ehdr.e_phentsize, sizeof(VALIDATE_PHDR));
        return false;
    }
    if (ehdr.e_phoff < sizeof(VALIDATE_EHDR)) {
        LOGE("Program header offset (" VALIDATE_EHDR_FMT ") overlaps ELF header",
             (size_t)ehdr.e_phoff);
        return false;
    }
    if (ehdr.e_phoff >= g_payload_size) {
        LOGE("Program header offset beyond file bounds (" VALIDATE_EHDR_FMT ")",
             (size_t)ehdr.e_phoff);
        return false;
    }

    // Overflow-safe bounds check matching AOSP linker's safe_add pattern.
    size_t phdr_size;
    if (__builtin_mul_overflow((size_t)ehdr.e_phnum, VALIDATE_PHDR_SIZE, &phdr_size)) {
        LOGE("Program header size overflow"); return false;
    }
    size_t phdr_end;
    if (__builtin_add_overflow(ehdr.e_phoff, phdr_size, &phdr_end)) {
        LOGE("Program header end overflow"); return false;
    }
    if (phdr_end > g_payload_size) {
        LOGE("Program headers extend beyond file bounds"); return false;
    }

    if (!elf_check_architecture(&ehdr)) return false;

    return true;
}

// Orchestrator: ELF validation in two phases.
static bool validate_elf_header(void) {
    return elf_check_ident() && elf_check_header();
}

// Phase 2: Read binary .so from disk + validate ELF header
static bool load_and_validate_payload(const char *payload_path) {
    // read_binary_file() uses O_NOFOLLOW, so symlinks are rejected
    // at the open() stage — no need for a separate lstat check here.

    if (!read_binary_file(payload_path, &g_payload_buffer, &g_payload_size))
        return false;

    bool valid = validate_elf_header();
    if (!valid) {
        release_payload();
        return false;
    }
    return true;
}

// Orchestrator: resolve payload path then load and validate
static bool load_payload_for_process(const char *base_process_name,
                                      char *payload_path_out, size_t payload_path_max) {
    if (!resolve_payload_path(base_process_name, payload_path_out, payload_path_max))
        return false;
    return load_and_validate_payload(payload_path_out);
}

static void preAppSpecialize(void *impl, struct zygisk_app_specialize_args *args) {
    (void)impl;
    if (!args) { LOGE("preAppSpecialize: NULL args"); return; }

    JNIEnv *env = get_jni_env();
    if (!env) return;

    char process_name[256] = {0};
    char base_process_name[256] = {0};

    if (!resolve_process_name(env, args, process_name, sizeof(process_name),
                              base_process_name, sizeof(base_process_name)))
        return;

    char payload_path[512] = {0};
    if (load_payload_for_process(base_process_name, payload_path, sizeof(payload_path))) {
        LOGI("Target Detected: %s (Base: %s)", process_name, base_process_name);
        LOGD("Dynamic Payload Path: %s", payload_path);
        g_target_app_detected = true;
    }
}

// Shared write helper: writes a full buffer to an fd with partial-write
// handling and EINTR retry. Replaces the identical write loops that
// were duplicated in load_via_memfd() and load_via_tempfile().
static bool write_buffer_to_fd(int fd, const uint8_t *buf, size_t size) {
    while (size > 0) {
        ssize_t n = write(fd, buf, size);
        if (n < 0) {
            if (errno == EINTR) {
                LOGW("write() interrupted by signal (EINTR) — retrying");
                continue;
            }
            LOGE("Failed to write payload (errno=%d)", errno);
            return false;
        }
        if (n == 0) {
            LOGE("write returned 0 (fd may have reached EOF)");
            return false;
        }
        buf += (size_t)n;
        size -= (size_t)n;
    }
    return true;
}

// Shared helper: dlopen from an fd after lseek. Eliminates the identical
// lseek → android_dlextinfo setup → android_dlopen_ext → error handling
// sequence duplicated in load_via_memfd() and load_via_tempfile().
static int dlopen_from_fd(int fd, const char *log_label) {
    if (lseek(fd, 0, SEEK_SET) == -1) {
        LOGE("lseek for %s failed: %s", log_label, strerror(errno));
        close(fd);
        return -errno;
    }
    // ANDROID_DLEXT_USE_LIBRARY_FD requires API 26+ (Android 8.0).
    // The module's minSdkVersion is 28 so this is always available,
    // but guard against backports to older platforms.
#if defined(__ANDROID_API__) && __ANDROID_API__ < 26
#error "ANDROID_DLEXT_USE_LIBRARY_FD requires API 26+"
#endif
    android_dlextinfo extinfo = {0};
    extinfo.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    extinfo.library_fd = fd;
    void *handle = android_dlopen_ext("libpayload.so", RTLD_NOW, &extinfo);
    if (!handle) {
        LOGE("%s injection failed: %s", log_label, dlerror());
        int saved_errno = errno;
        close(fd);
        return -saved_errno;
    }
    close(fd);
    LOGI("Injection successful — loaded via %s", log_label);
    return 0;
}

// Tier 1: load payload via memfd + android_dlopen_ext (no disk writes)
// Tier 2: fallback to temp file + dlopen when memfd unavailable
// Tier 3: log error and return false
static int load_via_memfd(void) {
    int memfd = (int)syscall(SYS_memfd_create, "zygisk-payload", MFD_CLOEXEC);
    if (memfd < 0) {
        int saved_errno = errno;
        LOGE("memfd_create failed: %s", strerror(saved_errno));
        return -saved_errno;
    }

    if (!write_buffer_to_fd(memfd, g_payload_buffer, g_payload_size)) {
        int saved_errno = errno;
        close(memfd);
        return -saved_errno;
    }
    return dlopen_from_fd(memfd, "memfd");
}

static bool load_via_tempfile(const char *app_data_dir) {
    char temp_path[512];

    // Create the cache directory if it doesn't exist yet.
    // On fresh installs or cleared data, /cache/ may not exist.
    char cache_dir[512];
    int cache_dir_written = snprintf(cache_dir, sizeof(cache_dir),
                                     "%s/cache", app_data_dir);
    if (cache_dir_written < 0 || (size_t)cache_dir_written >= sizeof(cache_dir)) {
        LOGE("Cache dir path too long: %s", app_data_dir);
        return false;
    }
    mkdir(cache_dir, 0700);  // Best-effort, may already exist

    int written = snprintf(temp_path, sizeof(temp_path),
                           "%s/zygisk-payload-XXXXXX", cache_dir);
    if (written < 0) {
        LOGE("snprintf encoding error for temp path");
        return false;
    }
    if ((size_t)written >= sizeof(temp_path)) {
        LOGE("Temp path too long for app_data_dir: %s", app_data_dir);
        return false;
    }

    int fd = mkstemp(temp_path);
    if (fd < 0) {
        LOGE("Failed to create temp file (errno=%d)", errno);
        return false;
    }

    // Immediately unlink — the fd keeps the inode alive.
    // Kernel auto-reclaims when fd is closed or process exits.
    unlink(temp_path);

    if (!write_buffer_to_fd(fd, g_payload_buffer, g_payload_size)) {
        close(fd);
        return false;
    }
    return dlopen_from_fd(fd, "tempfile") == 0;
}

// Inject payload using memfd + android_dlopen_ext (Tier 1).
// Falls back to temp file + dlopen on ENOSYS/EPERM (Tier 2).
// No file is ever written to disk in the common path (forensic stealth).
static bool inject_payload(const char *app_data_dir) {
    int ret = load_via_memfd();
    if (ret == 0) return true;

    if (ret == -ENOSYS || ret == -EPERM || ret == -EACCES) {
        LOGW("memfd_create unavailable (errno=%d), falling back to temp file injection", -ret);
        return load_via_tempfile(app_data_dir);
    }

    return false;
}

static void postAppSpecialize(void *impl, const struct zygisk_app_specialize_args *args) {
    (void)impl;

    // Re-entry guard: prevent double injection if called more than once.
    // Zygisk API guarantees one call per process — if this fires,
    // check Zygisk API contract changes.
    if (g_payload_injected) {
        LOGW("postAppSpecialize called again (re-entry)");
        return;
    }

    if (!g_target_app_detected || !g_payload_buffer) {
        return;
    }

    if (!args) {
        LOGE("postAppSpecialize: NULL args");
        release_payload();
        return;
    }

    char app_data_dir[512] = {0};

    JNIEnv *env = get_jni_env();
    if (!env) {
        release_payload();
        return;
    }

    if (!jstring_to_cstr(env, args->app_data_dir, app_data_dir, sizeof(app_data_dir))) {
        LOGE("Could not determine app data directory (truncated or JNI error)");
        release_payload();
        return;
    }

    if (!inject_payload(app_data_dir)) {
        LOGE("inject_payload failed for: %s (len=%zu)", app_data_dir, strlen(app_data_dir));
    }

    release_payload();
    g_payload_injected = true;
}

// Server specialization not needed for this module.
// LOGD calls intentionally omitted to avoid log spam on every
// system_server fork. The original empty-body pattern is restored.
static void preServerSpecialize(void *impl, struct zygisk_server_specialize_args *args) {
    (void)impl;
    (void)args;
}
static void postServerSpecialize(void *impl, const struct zygisk_server_specialize_args *args) {
    (void)impl;
    (void)args;
}

static struct zygisk_module_abi module_abi;

// Note: ZYGISK_OPTION_DLCLOSE_MODULE_LIBRARY is intentionally NOT set.
// The module .so stays resident after zygote specialization because:
//   1. No need to hide module presence (Zygisk modules are visible anyway)
//   2. Leaving it loaded avoids potential issues with dlclose on shared
//      linker namespaces in Android's Zygote process model
// If stealth is desired in the future, add:
//   api->setOption(api->impl, ZYGISK_OPTION_DLCLOSE_MODULE_LIBRARY);
__attribute__((visibility("default")))
void zygisk_module_entry(struct zygisk_api *api, JNIEnv *env) {
    if (!api || !env) {
        LOGE("zygisk_module_entry: NULL api or env");
        return;
    }
    module_abi.api_version = ZYGISK_API_VERSION;
    module_abi.impl = api;
    module_abi.preAppSpecialize = preAppSpecialize;
    module_abi.postAppSpecialize = postAppSpecialize;
    module_abi.preServerSpecialize = preServerSpecialize;
    module_abi.postServerSpecialize = postServerSpecialize;

    bool registered = api->registerModule(api, &module_abi);
    if (registered) {
        onLoad(api, env);
    } else {
        LOGE("Module registration FAILED — check Zygisk API version compatibility");
    }
}

// Companion not needed — close immediately per Zygisk API contract.
// Module operates entirely in-process via preAppSpecialize/postAppSpecialize.
__attribute__((visibility("default")))
void zygisk_companion_entry(int client) {
    if (client >= 0) close(client);
}
