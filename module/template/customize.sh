# shellcheck disable=SC2034
SKIPUNZIP=1

# TMPDIR is set by Magisk/KernelSU during module installation,
# but guard against unset variable just in case.
: "${TMPDIR:=/dev/tmp}"

DEBUG='@DEBUG@'
SONAME='@SONAME@'
SUPPORTED_ABIS='@SUPPORTED_ABIS@'

if [ "$BOOTMODE" ] && { [ "$KSU" ] || [ "$KSU_VER_CODE" ] || [ -d /data/adb/ksu/ ]; }; then
	ui_print "- Installing from KernelSU app"
	ui_print "- KernelSU version: $KSU_KERNEL_VER_CODE (kernel) + $KSU_VER_CODE (ksud)"
	if [ "$(command -v magisk)" ]; then
		ui_print "*********************************************************"
		ui_print "! Multiple root implementation is NOT supported!"
		ui_print "! Please uninstall Magisk before installing $SONAME"
		abort "*********************************************************"
	fi
elif [ "$BOOTMODE" ] && [ "$MAGISK_VER_CODE" ]; then
	ui_print "- Installing from Magisk app"
else
	ui_print "*********************************************************"
	ui_print "! Install from recovery is not supported"
	ui_print "! Please install from KernelSU or Magisk app"
	abort "*********************************************************"
fi

MODULE_VERSION=$(grep_prop version "${TMPDIR}/module.prop")
ui_print "- Installing $SONAME $MODULE_VERSION"

# check architecture
SUPPORTED=false
for abi in $SUPPORTED_ABIS; do
	if [ "$ARCH" == "$abi" ]; then
		SUPPORTED=true
	fi
done
if [ "$SUPPORTED" == "false" ]; then
	abort "! Unsupported platform: $ARCH"
else
	ui_print "- Device platform: $ARCH"
fi

ui_print "- Extracting verify.sh"
unzip -o "$ZIPFILE" 'verify.sh' -d "$TMPDIR" >/dev/null 2>&1
if [ ! -f "$TMPDIR/verify.sh" ]; then
	ui_print "*********************************************************"
	ui_print "! Unable to extract verify.sh!"
	ui_print "! This zip may be corrupted, please try downloading again"
	abort "*********************************************************"
fi
. "$TMPDIR/verify.sh"
extract "$ZIPFILE" 'customize.sh' "$TMPDIR/.vunzip"
extract "$ZIPFILE" 'verify.sh' "$TMPDIR/.vunzip"
if unzip -l "$ZIPFILE" | grep -qE '[[:space:]]sepolicy\.rule$'; then
  extract "$ZIPFILE" 'sepolicy.rule' "$TMPDIR"
fi

ui_print "- Extracting module files"
extract "$ZIPFILE" 'module.prop' "$MODPATH"
extract "$ZIPFILE" 'post-fs-data.sh' "$MODPATH"
extract "$ZIPFILE" 'service.sh' "$MODPATH"
if [ -f "$TMPDIR/sepolicy.rule" ]; then
  mv "$TMPDIR/sepolicy.rule" "$MODPATH"
fi

HAS32BIT=false
[ -n "$(getprop ro.product.cpu.abilist32)" ] && HAS32BIT=true
[ -n "$(getprop ro.system.product.cpu.abilist32)" ] && HAS32BIT=true

mkdir "$MODPATH/zygisk" || abort "Failed to create $MODPATH/zygisk"

# Helper: extract and rename a single ABI library
extract_and_rename() {
	local dir="$1" name="$2"
	extract "$ZIPFILE" "lib/$dir/lib$SONAME.so" "$MODPATH/zygisk" true
	mv "$MODPATH/zygisk/lib$SONAME.so" "$MODPATH/zygisk/$name.so"
}

# Helper: extract ABI-specific libraries with renaming
extract_abi_libs() {
	local arch32_dir="$1" arch64_dir="$2"
	if [ "$HAS32BIT" = true ]; then
		ui_print "- Extracting ${arch32_dir##*/} libraries"
		extract_and_rename "$arch32_dir" "${arch32_dir##*/}"
	fi
	ui_print "- Extracting ${arch64_dir##*/} libraries"
	extract_and_rename "$arch64_dir" "${arch64_dir##*/}"
}

if [ "$ARCH" = "x86" ] || [ "$ARCH" = "x64" ]; then
	extract_abi_libs "x86" "x86_64"
else
	extract_abi_libs "armeabi-v7a" "arm64-v8a"
fi

ui_print "- Setting permissions"
mkdir -p "$MODPATH/config" || abort "Failed to create $MODPATH/config"
cat > "$MODPATH/config/target.json" << 'EOF'
[
  {
    "app": "com.target.application",
    "lib": "/data/adb/modules/zygisk-loader/config/payload.so"
  }
]
EOF
# For multiple target applications, add more entries. Example:
# [
#   { "app": "com.example.app1", "lib": "/data/adb/modules/zygisk-loader/config/payload1.so" },
#   { "app": "com.example.app2", "lib": "/data/adb/modules/zygisk-loader/config/payload2.so" }
# ]
ui_print "*********************************************************"
ui_print "! IMPORTANT: Edit /data/adb/modules/zygisk-loader/config/target.json"
ui_print "! Replace the placeholder values with your actual"
ui_print "! target app package names and payload library paths."
ui_print "! Without proper configuration, the module will not"
ui_print "! inject into any application."
ui_print "*********************************************************"
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm_recursive "$MODPATH/config" 0 0 0755 0644