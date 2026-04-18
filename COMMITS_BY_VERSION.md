# Commit Messages by Version

## v0.1.0
- docs(readme): Add credit section for Zygisk-Rust-ModuleTemplate
- docs(readme): visualize architecture with monochrome mermaid diagram
- feat: Initialized Project

## v0.1.1
- chore: bump version 0.1.1
- refactor(lib): improve logging and error handling in Zygisk module
- chore(ci): rename from geoink to the project

## v0.2.0
- chore: bump version to 0.2.0
- feat: Improve process name detection in Zygisk module

## v0.2.1
- bump version 0.2.1
- refactor(module): simplify process matching logic and update logging
- chore: bump `android_logger` v0.15.x`
- chore: bump `rust-android-gradle` 0.9.6

## v0.2.2
- chore: bump version 0.2.2
- refactor: improve payload injection using file descriptors

## v0.2.3
- chore: bump version 0.2.3
- refactor: implement memory-buffered payload injection

## v0.3.0
- chore: bump version 0.3.0
- refactor(module): update ZygiskModule trait for jni v0.21.0
- refactor(macros): adapt `module_entry_impl` for jni v0.21.0
- refactor(lib): adapt to jni v0.21.0 breaking changes
- refactor(api): adapt `hook_jni_native_methods` for jni v0.21.0
- build(deps): bump jni crate to 0.21.0

## v0.3.1
- chore(docs): revise README for improved clarity and detail
- chore: bump version 0.3.1
- refactor: Relocate module configs and enhance stealth
- chore: refining path and permission

## v0.3.2
- chore(bump): versione ZL to 0.3.2 fix patch
- fix(module): replace attach_current_thread_as_daemon with get_env to prevent race condition
- fix(build): fix library paths native library paths in build script to use rustJniLibs directory
- chore(ci):Update build tools to 36.1.0, NDK to 29.0.14206865 nad JDK to 21
- fix(build): python command reference in cargo configuration
- chore(gradle): update NDK version and improve build configuration
- chore(gradle): bump version 8.5
- chore(docs): Add Telegram support to README

## v0.3.3
- chore(bump): versione ZL to 0.3.3 refactor patch
- chore(docs): Update Readme for more clarity information
- refactor(module): improve injection stealth with random filenames and immediate cleanup
- feat(build): add optimization on release profile

## v1.0.0
- feat(init): rewrite to c

## v1.1.0
- chore(release): standardize zip naming and restore git-cliff with full history
- fix(gradle): resolve verName ambiguity in root build.gradle
- feat(docs): update documentation and release metadata for v1.1.0
- chore(bump): transition to version-driven release and v1.1.0 bump
- feat(module): implement multi-app support with native JSON parser
- feat(docs): update LICENSE, and Globalization The Commentary
