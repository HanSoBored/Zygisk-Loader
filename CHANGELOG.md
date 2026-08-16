## [2.0.2] - 2026-08-16

### <!-- 1 -->🐛 Bug Fixes

- Scan past non-matching config entries ([cd4d90c](cd4d90cfb4491347a687573ce4f2ce960574be1e))


### Build

- Pin NDK r30 — r29 install lacks linux-arm64 prebuilts ([291204e](291204ef3d5cfffa27f8f42153fad2480b710824))


### <!-- 7 -->⚙️ Miscellaneous Tasks

- Bump version to 2.0.2 ([43d5e13](43d5e13b2cd4f37695e59c20d4650ea563e6d7d5))


### Ci

- Install NDK r30 on CI runner ([d918ab8](d918ab84bff1f6d1d62ad1b18e51cc05a4f0c0e2))


## [2.0.1] - 2026-06-06

### <!-- 0 -->🚀 Features

- Implement memfd-based RAM injection with ELF validation ([4a0c94c](4a0c94cdd4aaccfd8e6f5e1b362dab89f69c0137))


### <!-- 1 -->🐛 Bug Fixes

- Harden template scripts with SHA-256 verification and config warnings ([1bf0a28](1bf0a2810a6053b4f039bd68fb60b11a89ca9e56))

- Replace heredoc with printf to fix YAML syntax error ([989fe8e](989fe8e983815a11cbeac558d65bb4fa0fd41279))


### <!-- 2 -->🚜 Refactor

- Modular module build system with SHA-256 and ADB validation ([468e0a0](468e0a0c4e1080e0417e4878c55bab1f1fe9ee76))

- Root build system with versionCode from VERSION file ([ba2f0c0](ba2f0c02b455acb614a6eeae2f83576e0208ddc2))

- Switch to C23, fix NDK 29 compat, and resolve review findings ([c6a45a8](c6a45a8fa91b72eb3ae022660ffb2e30f7bb1442))


### <!-- 3 -->📚 Documentation

- Update README for RAM-only injection strategy ([e3dcc83](e3dcc83d3ceb9c2bde6d1f9d7df641bb1b523467))


### <!-- 7 -->⚙️ Miscellaneous Tasks

- Update Gradle wrapper to latest version ([a163715](a1637154b4d30f9406d9d6d8c4d7da0b075482cb))

- Bump version to 2.0.1 ([6f01354](6f0135465e5b135daad30dd08c98b54433445689))


### Ci

- Add workflow_dispatch, update.json generation, and changelog validation ([4d13289](4d13289c152b3ceaee33328bdded7887d516f8b7))

- Commit update.json and CHANGELOG.md to main after release ([00a0062](00a0062da5ff0e1409cd1685c20e45c09ec78b40))


