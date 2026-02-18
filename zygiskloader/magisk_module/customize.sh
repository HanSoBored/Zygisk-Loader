# Extract verify.sh

enforce_install_from_magisk_app() {
  if $BOOTMODE; then
    ui_print "- Installing from Magisk app"
  else
    ui_print "*********************************************************"
    ui_print "! Install from recovery is NOT supported"
    ui_print "! Please install from Magisk app"
    abort "*********************************************************"
  fi
}

VERSION=$(grep_prop version "${TMPDIR}/module.prop")
ui_print "- Zygisk-Loader version ${VERSION}"


# Extract verify.sh
ui_print "- Extracting verify.sh"
unzip -o "$ZIPFILE" 'verify.sh' -d "$TMPDIR" >&2
if [ ! -f "$TMPDIR/verify.sh" ]; then
  ui_print    "*********************************************************"
  ui_print    "! Unable to extract verify.sh!"
  ui_print    "! This zip may be corrupted, please try downloading again"
  abort "*********************************************************"
fi
. $TMPDIR/verify.sh

extract "$ZIPFILE" 'customize.sh' "$TMPDIR"
extract "$ZIPFILE" 'verify.sh' "$TMPDIR"
extract "$ZIPFILE" 'util_functions.sh' "$TMPDIR"
. "$TMPDIR/util_functions.sh"

check_android_version
enforce_install_from_magisk_app

# Check architecture
if [ "$ARCH" != "arm" ] && [ "$ARCH" != "arm64" ] && [ "$ARCH" != "x86" ] && [ "$ARCH" != "x64" ]; then
  abort "! Unsupported platform: $ARCH"
else
  ui_print "- Device platform: $ARCH"
fi

# Extract libs
ui_print "- Extracting module files"

extract "$ZIPFILE" 'module.prop' "$MODPATH"
extract "$ZIPFILE" 'uninstall.sh' "$MODPATH"

mkdir -p "$MODPATH/zygisk"
ui_print "- Extracting daemon libraries"

if [ "$ARCH" = "arm" ] || [ "$ARCH" = "arm64" ]; then
  extract "$ZIPFILE" "lib/armeabi-v7a/libzygiskloader.so" "$MODPATH/zygisk" true
  mv "$MODPATH/zygisk/libzygiskloader.so" "$MODPATH/zygisk/armeabi-v7a.so"

  if [ "$IS64BIT" = true ]; then
    extract "$ZIPFILE" "lib/arm64-v8a/libzygiskloader.so" "$MODPATH/zygisk" true
    mv "$MODPATH/zygisk/libzygiskloader.so" "$MODPATH/zygisk/arm64-v8a.so"
  fi
fi

if [ "$ARCH" = "x86" ] || [ "$ARCH" = "x64" ]; then
  extract "$ZIPFILE" "lib/x86_64/libzygiskloader.so" "$MODPATH/zygisk" true
  mv "$MODPATH/zygisk/libzygiskloader.so" "$MODPATH/zygisk/x86_64.so"

  if [ "$IS64BIT" = true ]; then
    extract "$ZIPFILE" "lib/x86/libzygiskloader.so" "$MODPATH/zygisk" true
    mv "$MODPATH/zygisk/libzygiskloader.so" "$MODPATH/zygisk/x86.so"
  fi
fi

mkdir -p "$MODPATH/config"
echo "com.tencent.rmcn" > "$MODPATH/config/target"

set_perm_recursive $MODPATH 0 0 0755 0644
set_perm_recursive $MODPATH/config 0 0 0755 0644
