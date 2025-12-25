mod api;
mod binding;
#[doc(hidden)]
pub mod macros;
mod module;

#[macro_use]
extern crate log;
#[cfg(target_os = "android")]
extern crate android_logger;

#[cfg(target_os = "android")]
use android_logger::Config;
#[cfg(target_os = "android")]
use log::LevelFilter;

use std::ffi::{CStr, CString};
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::sync::OnceLock;

pub use api::ZygiskApi;
pub use binding::{AppSpecializeArgs, ServerSpecializeArgs, StateFlags, ZygiskOption, API_VERSION};
use jni::{JNIEnv, JavaVM};
pub use module::ZygiskModule;

// Path Config & Loader
const CONFIG_PATH: &str = "/data/adb/modules/zygisk-loader/active_config.txt";
const PAYLOAD_PATH: &str = "/data/adb/modules/zygisk-loader/payload.so";

static MODULE: ZygiskLoaderModule = ZygiskLoaderModule {};
crate::zygisk_module!(&MODULE);

struct ZygiskLoaderModule {}

// Static variable to store JavaVM
static JAVA_VM: OnceLock<JavaVM> = OnceLock::new();

impl ZygiskModule for ZygiskLoaderModule {
    fn on_load(&self, _api: ZygiskApi, env: JNIEnv) {
        #[cfg(target_os = "android")]
        android_logger::init_once(
            Config::default()
                .with_max_level(LevelFilter::Debug) // Changed to Debug for more detailed logs
                .with_tag("Zygisk_Loader"),
        );

        // Store the JavaVM and not JNIEnv (JNIEnv is thread/process specific)
        let vm = env.get_java_vm().expect("Failed to get JavaVM");
        let _ = JAVA_VM.set(vm); // Set once at startup

        info!("Zygisk-Loader Loaded (on_load). JavaVM stored.");
    }

    fn post_app_specialize(&self, _api: ZygiskApi, args: &AppSpecializeArgs) {
        // Get process name from AppSpecializeArgs (nice_name is guaranteed to exist)
        let current_process = get_process_name_from_args_safe(args);

        if current_process.is_empty() {
            error!("Failed to get process name from AppSpecializeArgs!");
            return;
        }

        debug!("Checking process: '{}'", current_process);

        // Read Target Config
        let target_package = match read_target_config() {
            Ok(target) => target,
            Err(e) => {
                error!("Failed to read config in {}: {:?}", CONFIG_PATH, e);
                return;
            }
        };

        if current_process.contains(target_package.trim()) {
            info!("Target Match! Process: '{}' matches Target: '{}'", current_process, target_package);
            info!("Attempting Injection: {}", PAYLOAD_PATH);

            unsafe {
                inject_payload(PAYLOAD_PATH);
            }
        }
    }
}

// Helper Functions

fn read_target_config() -> std::io::Result<String> {
    let f = File::open(CONFIG_PATH)?;
    let mut reader = BufReader::new(f);
    let mut line = String::new();

    reader.read_line(&mut line)?;

    Ok(line.trim().to_string())
}

unsafe fn inject_payload(path: &str) {
    let c_path = CString::new(path).unwrap();

    let handle = libc::dlopen(c_path.as_ptr(), libc::RTLD_NOW);

    if handle.is_null() {
        let err_ptr = libc::dlerror();
        if !err_ptr.is_null() {
            let err_msg = CStr::from_ptr(err_ptr).to_string_lossy();
            error!("Fail Load Payload: {}", err_msg);
        } else {
            error!("Fail Load Payload: Unknown error");
        }
    } else {
        info!("Payload successfully loaded! Handle: {:p}", handle);
    }
}

// Helper function to extract process name from AppSpecializeArgs
// This function gets a valid JNIEnv for the current process and extracts the string
fn get_process_name_from_args_safe(args: &AppSpecializeArgs) -> String {
    // Try to get JavaVM and attach to current thread to get valid JNIEnv
    if let Some(vm) = JAVA_VM.get() {
        match vm.attach_current_thread_as_daemon() {
            Ok(env) => {
                // First try to get the process name from nice_name (usually the package name or process name)
                // args.nice_name is &mut JString<'a>. We need to dereference it (*args.nice_name)
                // to get JString that can be owned by env.get_string.
                let nice_name_jstring = *args.nice_name;
                if let Ok(nice_name_str) = env.get_string(nice_name_jstring) {
                    let nice_name_rust: String = nice_name_str.into();
                    if !nice_name_rust.is_empty() {
                        debug!("Got process name from nice_name: '{}'", nice_name_rust);
                        return nice_name_rust;
                    }
                }

                // If nice_name is empty, try to get from app_data_dir
                // args.app_data_dir is &mut JString<'a>. We need to dereference it (*args.app_data_dir)
                // to get JString that can be owned by env.get_string.
                let app_data_dir_jstring = *args.app_data_dir;
                if let Ok(app_data_dir_str) = env.get_string(app_data_dir_jstring) {
                    // Perbaikan: Tambahkan anotasi tipe String juga di sini
                    let app_data_dir_rust: String = app_data_dir_str.into();
                    if !app_data_dir_rust.is_empty() {
                        // Extract package name from app_data_dir path
                        // Format is typically: /data/user/0/com.android.chrome or /data/data/com.example.package
                        let package_name = extract_package_from_path(&app_data_dir_rust);
                        if !package_name.is_empty() {
                            debug!("Got process name from app_data_dir: '{}'", package_name);
                            return package_name;
                        }
                    }
                }
            },
            Err(e) => {
                error!("Failed to attach JVM in post_app_specialize: {:?}", e);
            }
        }
    } else {
        error!("JavaVM not initialized");
    }

    // If both fail, return empty string (fallback will use /proc/self/cmdline)
    String::new()
}

// Helper function to extract package name from app data directory path
fn extract_package_from_path(path: &str) -> String {
    // Path format: /data/user/0/com.android.chrome or /data/data/com.example.package
    let parts: Vec<&str> = path.split('/').collect();
    if parts.len() >= 3 {
        // Get the last non-empty part which should be the package name
        for part in parts.iter().rev() {
            if !part.is_empty() {
                return part.to_string();
            }
        }
    }
    String::new()
}

#[cfg(test)]
mod test {
    use std::os::unix::io::RawFd;
    fn companion(_socket: RawFd) {}
    crate::zygisk_companion!(companion);
}
