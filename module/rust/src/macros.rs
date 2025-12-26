pub use jni::JNIEnv;

use crate::{
    binding::{ModuleAbi, RawApiTable},
    module::RawModule,
    ZygiskApi, ZygiskModule,
};

#[inline(always)]
pub fn module_entry_impl(module: &'static dyn ZygiskModule, table: *const (), env: *mut ()) {
    // Cast arguments to their concrete types
    let table: &'static RawApiTable = unsafe { &*table.cast() };
    let mut env: JNIEnv = unsafe { JNIEnv::from_raw(env.cast()).unwrap() };

    // Currently a Zygisk module doesn't have a destructor, so we just have to
    // leak some heap memory. (And yes, we have to do `Box::leak` TWICE: one'
    // for the module, and the other for the `ModuleAbi`.)
    // Note that the original version also leaks memory, but it saves one leak
    // compared to us, thanks to C++ not using fat pointers. Lucky them :(
    let raw_module = Box::leak(Box::new(RawModule {
        inner: module,
        api_table: table,
    }));
    let module_abi = Box::leak(Box::new(ModuleAbi::from_module(raw_module)));
    if table.register_module.unwrap()(table, module_abi) {
        let api = ZygiskApi::from_raw(table);
        module.on_load(api, &mut env);
    }
}

/// Register a static variable as a Zygisk module.
///
/// ## Example
///
/// ```
/// struct DummyModule;
/// impl ZygiskModule for DummyModule {}
///
/// static MODULE: DummyModule = DummyModule;
/// zygisk_module!(&MODULE);
/// ```
#[macro_export]
macro_rules! zygisk_module {
    ($module: expr) => {
        #[no_mangle]
        extern "C" fn zygisk_module_entry(table: *const (), env: *mut ()) {
            if let Err(_) = std::panic::catch_unwind(|| {
                $crate::macros::module_entry_impl($module, table, env);
            }) {
                // Panic messages should be displayed by the default panic hook.
                std::process::abort();
            }
        }
    };
}

/// Register a root companion request handler function for your module.
///
/// The function runs in a superuser daemon process and handles a root companion request from
/// your module running in a target process. The function has to accept an integer value,
/// which is a socket that is connected to the target process.
/// See [ZygiskApi::connect_companion()] for more info.
///
/// Note: the function may be run concurrently on multiple threads.
#[macro_export]
macro_rules! zygisk_companion {
    ($func:path) => {
        #[no_mangle]
        extern "C" fn zygisk_companion_entry(client: ::std::os::unix::io::RawFd) {
            // Type check
            let _type_check: fn(::std::os::unix::io::RawFd) = $func;
            $func(client);
        }
    };
}
