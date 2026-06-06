#ifndef ZYGISK_H
#define ZYGISK_H

#include <jni.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

// Minimum Zygisk API version required by this module.
// Compile-time constant; the runtime version is checked via
// api->getFlags() for compatibility during registration.
#define ZYGISK_API_VERSION 5

// Feature detection flags for Zygisk API runtime feature detection.
// Check api->getFlags() & ZYGISK_API_FLAG_* before using the
// corresponding union members in struct zygisk_api (see SAFE USAGE below).
#define ZYGISK_API_FLAG_PLT_HOOK_V4      (1u << 0)
#define ZYGISK_API_FLAG_PLT_HOOK_V3      (1u << 1)

typedef struct zygisk_api zygisk_api;
typedef struct zygisk_module_abi zygisk_module_abi;

typedef enum {
    ZYGISK_OPTION_FORCE_DENYLIST_UNMOUNT = 0,
    ZYGISK_OPTION_DLCLOSE_MODULE_LIBRARY = 1,
} zygisk_option;

// #define used instead of enum because (1u << 31) as an enum value
// would be INT_MIN (implementation-defined for signed enum types).
// uint32_t allows arbitrary bitwise combinations for multi-state queries.
typedef uint32_t zygisk_state_flag;

#define ZYGISK_STATE_PROCESS_GRANTED_ROOT          (1u << 0)
#define ZYGISK_STATE_PROCESS_ON_DENYLIST           (1u << 1)
#define ZYGISK_STATE_PROCESS_IS_MANAGER            (1u << 27)
#define ZYGISK_STATE_PROCESS_ROOT_IS_APATCH        (1u << 28)
#define ZYGISK_STATE_PROCESS_ROOT_IS_KSU           (1u << 29)
#define ZYGISK_STATE_PROCESS_ROOT_IS_MAGISK        (1u << 30)
#define ZYGISK_STATE_PROCESS_IS_FIRST_STARTED      (1u << 31)

struct zygisk_app_specialize_args {
    jint *uid;
    jint *gid;
    jintArray *gids;
    jint *runtime_flags;
    jobjectArray *rlimits;
    jint *mount_external;
    jstring *se_info;
    jstring *nice_name;
    jstring *instruction_set;
    jstring *app_data_dir;

    jintArray *fds_to_ignore;
    jboolean *is_child_zygote;
    jboolean *is_top_app;
    jobjectArray *pkg_data_info_list;
    jobjectArray *whitelisted_data_info_list;
    jboolean *mount_data_dirs;
    jboolean *mount_storage_dirs;
    jboolean *mount_sysprop_overrides;
};

struct zygisk_server_specialize_args {
    jint *uid;
    jint *gid;
    jintArray *gids;
    jint *runtime_flags;
    jlong *permitted_capabilities;
    jlong *effective_capabilities;
};

struct zygisk_module_abi {
    long api_version;
    void *impl;

    void (*preAppSpecialize)(void *impl, struct zygisk_app_specialize_args *args);
    void (*postAppSpecialize)(void *impl, const struct zygisk_app_specialize_args *args);
    void (*preServerSpecialize)(void *impl, struct zygisk_server_specialize_args *args);
    void (*postServerSpecialize)(void *impl, const struct zygisk_server_specialize_args *args);
};

struct zygisk_api {
    void *impl;
    bool (*registerModule)(struct zygisk_api *api, const struct zygisk_module_abi *abi);

    void (*hookJniNativeMethods)(JNIEnv *env, const char *className, JNINativeMethod *methods, int numMethods);
    // Anonymous unions for Zygisk API backward compatibility.
    // Check getFlags() at runtime to determine which variant is valid.
    // v1/v2: use exemptFd; v3+: use plt_hook_exclude; v4+: use plt_hook_register_v4.
    // Calling the wrong union member corrupts the ABI.
    //
    // SAFE USAGE:
    //   uint32_t flags = api->getFlags();
    //   if (flags & ZYGISK_API_FLAG_PLT_HOOK_V4) {
    //       api->plt_hook_register_v4(dev, inode, symbol, newFunc, oldFunc);
    //   } else {
    //       api->plt_hook_register(path, symbol, newFunc, oldFunc);
    //   }
    union {
        void (*plt_hook_register)(const char *path, const char *symbol, void *newFunc, void **oldFunc);
        void (*plt_hook_register_v4)(dev_t dev, ino_t inode, const char *symbol, void *newFunc, void **oldFunc);
    };
    union {
        void (*plt_hook_exclude)(const char *path, const char *symbol);
        void (*exemptFd)(int fd);
    };

    // NOTE: (void) parameter lists are intentional — in C, empty parentheses ()
    // mean "unspecified parameters" (no type checking), while (void) explicitly
    // declares "no parameters" with compiler type checking. This is a correctness
    // fix from the previous () style to match the actual Zygisk API signature.
    bool (*plt_hook_commit)(void);
    int (*connectCompanion)(void *impl);
    void (*setOption)(void *impl, zygisk_option opt);
    int (*getModuleDir)(void *impl);
    uint32_t (*getFlags)(void);
};

#endif
