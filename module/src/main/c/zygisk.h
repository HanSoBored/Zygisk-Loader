#ifndef ZYGISK_H
#define ZYGISK_H

#include <jni.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

#define ZYGISK_API_VERSION 5

typedef struct zygisk_api zygisk_api;
typedef struct zygisk_module_abi zygisk_module_abi;

typedef enum {
    ZYGISK_OPTION_FORCE_DENYLIST_UNMOUNT = 0,
    ZYGISK_OPTION_DLCLOSE_MODULE_LIBRARY = 1,
} zygisk_option;

typedef enum {
    ZYGISK_STATE_PROCESS_GRANTED_ROOT = (1u << 0),
    ZYGISK_STATE_PROCESS_ON_DENYLIST = (1u << 1),
    ZYGISK_STATE_PROCESS_IS_MANAGER = (1u << 27),
    ZYGISK_STATE_PROCESS_ROOT_IS_APATCH = (1u << 28),
    ZYGISK_STATE_PROCESS_ROOT_IS_KSU = (1u << 29),
    ZYGISK_STATE_PROCESS_ROOT_IS_MAGISK = (1u << 30),
    ZYGISK_STATE_PROCESS_IS_FIRST_STARTED = (1u << 31),
} zygisk_state_flag;

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
    union {
        void (*plt_hook_register)(const char *path, const char *symbol, void *newFunc, void **oldFunc);
        void (*plt_hook_register_v4)(dev_t dev, ino_t inode, const char *symbol, void *newFunc, void **oldFunc);
    };
    union {
        void (*plt_hook_exclude)(const char *path, const char *symbol);
        void (*exemptFd)(int fd);
    };

    bool (*plt_hook_commit)();
    int (*connectCompanion)(void *impl);
    void (*setOption)(void *impl, zygisk_option opt);
    int (*getModuleDir)(void *impl);
    uint32_t (*getFlags)();
};

#endif
