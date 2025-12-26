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
use std::fs::{self, File};
use std::io::{BufRead, BufReader, Read, Write};
use std::sync::OnceLock;

pub use api::ZygiskApi;
pub use binding::{AppSpecializeArgs, ServerSpecializeArgs, StateFlags, ZygiskOption, API_VERSION};
use jni::{JNIEnv, JavaVM};
pub use module::ZygiskModule;

// Config & Source Payload path
const CONFIG_PATH: &str = "/data/adb/modules/zygisk-loader/config/target";
const SOURCE_PAYLOAD_PATH: &str = "/data/adb/modules/zygisk-loader/config/payload.so";
const TARGET_FILENAME: &str = "lib_ghost_payload.so";

static MODULE: ZygiskLoaderModule = ZygiskLoaderModule {};
crate::zygisk_module!(&MODULE);

struct ZygiskLoaderModule {}

static JAVA_VM: OnceLock<JavaVM> = OnceLock::new();
static TARGET_CONFIG: OnceLock<String> = OnceLock::new();
static PAYLOAD_BUFFER: OnceLock<Vec<u8>> = OnceLock::new();
static TARGET_APP_DETECTED: OnceLock<bool> = OnceLock::new();

impl ZygiskModule for ZygiskLoaderModule {
    fn on_load(&self, _api: ZygiskApi, env: &mut JNIEnv) {
        #[cfg(target_os = "android")]
        android_logger::init_once(
            Config::default()
                .with_max_level(LevelFilter::Debug)
                .with_tag("Zygisk_Loader"),
        );

        let vm = env.get_java_vm().expect("Failed to get JavaVM");
        let _ = JAVA_VM.set(vm);
        info!("Zygisk-Loader Initialized");
    }

    fn pre_app_specialize(&self, _api: ZygiskApi, args: &mut AppSpecializeArgs) {
        // 1. Read Config (As Root/Zygote)
        if let Ok(target) = read_target_config() {
            let _ = TARGET_CONFIG.set(target);
        }

        let current_process = get_process_name_from_args_safe(args);
        let target_package = TARGET_CONFIG.get().map(|s| s.as_str()).unwrap_or("");

        if !target_package.is_empty() && current_process.contains(target_package) {
            info!("Target Detected: {}", current_process);
            let _ = TARGET_APP_DETECTED.set(true);

            // 2. Read Payload to RAM
            match read_file_to_memory(SOURCE_PAYLOAD_PATH) {
                Ok(buffer) => {
                    info!("Payload buffered to RAM: {} bytes", buffer.len());
                    let _ = PAYLOAD_BUFFER.set(buffer);
                },
                Err(e) => {
                    error!("Failed to buffer payload from {}: {}", SOURCE_PAYLOAD_PATH, e);
                }
            }
        }
    }

    fn post_app_specialize(&self, _api: ZygiskApi, args: &AppSpecializeArgs) {
        if TARGET_APP_DETECTED.get() != Some(&true) {
            return;
        }

        let app_data_dir = get_app_data_dir_from_args(args);
        if app_data_dir.is_empty() { return; }

        if let Some(buffer) = PAYLOAD_BUFFER.get() {
            let cache_dir = format!("{}/cache", app_data_dir);
            let dest_path = format!("{}/{}", cache_dir, TARGET_FILENAME);

            let _ = fs::create_dir_all(&cache_dir);
            
            // Write file
            match write_memory_to_file(&dest_path, buffer) {
                Ok(_) => {
                    let c_dest = CString::new(dest_path.clone()).unwrap();
                    
                    unsafe {
                        libc::chmod(c_dest.as_ptr(), 0o700);
                        
                        // Injection
                        info!("Injecting...");
                        let handle = libc::dlopen(c_dest.as_ptr(), libc::RTLD_NOW);
                        
                        if handle.is_null() {
                            let err = CStr::from_ptr(libc::dlerror()).to_string_lossy();
                            error!("dlopen failed: {}", err);
                        } else {
                            info!("Injection success! Handle: {:p}", handle);
                            info!("Payload is active.");
                            
                            if libc::unlink(c_dest.as_ptr()) == 0 {
                                info!("Artifact removed from disk.");
                            } else {
                                error!("Failed to remove artifact.");
                            }
                        }
                    }
                },
                Err(e) => error!("Write failed: {}", e)
            }
        }
    }
}

// IO HELPERS

fn read_file_to_memory(path: &str) -> std::io::Result<Vec<u8>> {
    let mut f = File::open(path)?;
    let mut buffer = Vec::new();
    f.read_to_end(&mut buffer)?;
    Ok(buffer)
}

fn write_memory_to_file(path: &str, data: &[u8]) -> std::io::Result<()> {
    let mut f = File::create(path)?;
    f.write_all(data)?;
    f.sync_all()?;
    Ok(())
}

fn read_target_config() -> std::io::Result<String> {
    let f = File::open(CONFIG_PATH)?;
    let mut reader = BufReader::new(f);
    let mut line = String::new();
    reader.read_line(&mut line)?;
    Ok(line.trim().to_string())
}

// ARGS PARSING HELPERS

fn get_process_name_from_args_safe(args: &AppSpecializeArgs) -> String {
    if let Some(vm) = JAVA_VM.get() {
        if let Ok(mut env) = vm.attach_current_thread_as_daemon() {
            if let Ok(s) = env.get_string(args.nice_name) {
                let s_rust: String = s.into();
                if !s_rust.is_empty() { return s_rust; }
            }
        }
    }
    let dir = get_app_data_dir_from_args(args);
    if !dir.is_empty() { return extract_package_from_path(&dir); }
    String::new()
}

fn get_app_data_dir_from_args(args: &AppSpecializeArgs) -> String {
    if let Some(vm) = JAVA_VM.get() {
        if let Ok(mut env) = vm.attach_current_thread_as_daemon() {
            if let Ok(j_str) = env.get_string(args.app_data_dir) {
                return j_str.into();
            }
        }
    }
    String::new()
}

fn extract_package_from_path(path: &str) -> String {
    let parts: Vec<&str> = path.split('/').collect();
    if parts.len() >= 3 {
        for part in parts.iter().rev() {
            if !part.is_empty() && *part != "cache" {
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
