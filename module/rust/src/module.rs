use jni::JNIEnv;

use crate::{
    binding::{ModuleAbi, RawApiTable},
    AppSpecializeArgs, ServerSpecializeArgs, ZygiskApi,
};

// Note: in stub implementations, all the arguments are unused.
#[allow(unused_variables)]
pub trait ZygiskModule {
    /// This function is called when the module is loaded into the target process.
    ///
    /// A Zygisk API handle will be sent as an argument; call utility functions or interface
    /// with Zygisk through this handle.
    fn on_load(&self, api: ZygiskApi, env: &mut JNIEnv) {}

    /// This function is called before the app process is specialized.
    /// At this point, the process just got forked from zygote, but no app specific specialization
    /// is applied. This means that the process does not have any sandbox restrictions and
    /// still runs with the same privilege of zygote.
    ///
    /// All the arguments that will be sent and used for app specialization is passed as a mutable
    /// reference to a single [AppSpecializeArgs] object. You can read and overwrite these arguments
    /// to change how the app process will be specialized.
    ///
    /// If you need to run some operations as superuser, you can call `ZygiskApi::connect_companion()`
    /// to get a socket to do IPC calls with a root companion process.
    /// See [ZygiskApi::connect_companion] for more info.
    fn pre_app_specialize(&self, api: ZygiskApi, args: &mut AppSpecializeArgs) {}

    /// This function is called after the app process is specialized.
    /// At this point, the process has all sandbox restrictions enabled for this application.
    /// This means that this function runs as the same privilege of the app's own code.
    fn post_app_specialize(&self, api: ZygiskApi, args: &AppSpecializeArgs) {}

    /// This function is called before the system server process is specialized.
    /// See [Self::pre_app_specialize] for more info.
    fn pre_server_specialize(&self, api: ZygiskApi, args: &mut ServerSpecializeArgs) {}

    /// This function is called after the system server process is specialized.
    /// At this point, the process runs with the privilege of `system_server`.
    fn post_server_specialize(&self, api: ZygiskApi, args: &ServerSpecializeArgs) {}
}

/// Information about a registered module, for use in FFI functions.
///
/// This exists since the Zygisk API binding requires any `this` pointers to be thin,
/// while Rust's `dyn` pointers are not.
pub(crate) struct RawModule {
    pub inner: &'static dyn ZygiskModule,
    pub api_table: *const RawApiTable,
}

impl crate::binding::ModuleAbi {
    pub(crate) fn from_module(module: &'static mut RawModule) -> ModuleAbi {
        macro_rules! def_func {
            ($name: ident, $arg_type: ty) => {
                extern "C" fn $name(module: &mut RawModule, args: $arg_type) {
                    let api = unsafe { ZygiskApi::from_raw(&*module.api_table) };
                    module.inner.$name(api, args);
                }
            };
        }
        def_func!(pre_app_specialize, &mut AppSpecializeArgs);
        def_func!(post_app_specialize, &AppSpecializeArgs);
        def_func!(pre_server_specialize, &mut ServerSpecializeArgs);
        def_func!(post_server_specialize, &ServerSpecializeArgs);

        ModuleAbi {
            api_version: crate::API_VERSION,
            this: module,
            pre_app_specialize,
            post_app_specialize,
            pre_server_specialize,
            post_server_specialize,
        }
    }
}
