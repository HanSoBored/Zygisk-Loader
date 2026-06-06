TMPDIR_FOR_VERIFY="$TMPDIR/.vunzip"
mkdir -p "$TMPDIR_FOR_VERIFY" || abort_verify "Failed to create $TMPDIR_FOR_VERIFY"

abort_verify() {
  ui_print "*********************************************************"
  ui_print "! $1"
  ui_print "! This zip may be corrupted, please try downloading again"
  abort "*********************************************************"
}

verify_file_hash() {
  local file_path="$1" hash_path="$2" display_name="$3"
  local hash
  read -r hash _ < "$hash_path"

  # Validate hash format before comparison
  if ! echo "$hash" | grep -qE '^[0-9a-f]{64}$'; then
    abort_verify "Invalid hash format in $hash_path"
  fi

  for cmd_pair in "sha256sum|1" "toybox sha256sum|1" "openssl dgst -sha256|2"; do
    local cmd="${cmd_pair%|*}"
    local field="${cmd_pair#*|}"
    if command -v ${cmd%% *} >/dev/null 2>&1; then
      local actual_hash
      actual_hash=$($cmd "$file_path" | cut -d' ' -f"$field")
      if [ "$hash" != "$actual_hash" ]; then
        abort_verify "Failed to verify $display_name"
      fi
      ui_print "- Verified $display_name" >&1
      return 0
    fi
  done
  ui_print "- SHA-256 verification not available, skipping verification for $display_name"
}

# extract <zip> <file> <target dir> <junk paths>
extract() {
  zip=$1
  file=$2
  dir=$3
  junk_paths=$4
  [ -z "$junk_paths" ] && junk_paths=false
  opts="-o"
  [ "$junk_paths" = true ] && opts="-oj"

  file_path=""
  hash_path=""
  if [ "$junk_paths" = true ]; then
    file_path="$dir/$(basename "$file")"
    hash_path="$TMPDIR_FOR_VERIFY/$(basename "$file").sha256"
  else
    file_path="$dir/$file"
    hash_path="$TMPDIR_FOR_VERIFY/$file.sha256"
  fi

  unzip "$opts" "$zip" "$file" -d "$dir" >&2
  [ -f "$file_path" ] || abort_verify "$file not exists"

  unzip "$opts" "$zip" "$file.sha256" -d "$TMPDIR_FOR_VERIFY" >&2
  [ -f "$hash_path" ] || abort_verify "$file.sha256 not exists"

  verify_file_hash "$file_path" "$hash_path" "$file"
}

file="META-INF/com/google/android/update-binary"
file_path="$TMPDIR_FOR_VERIFY/$file"
hash_path="$file_path.sha256"
unzip -o "$ZIPFILE" "META-INF/com/google/android/*" -d "$TMPDIR_FOR_VERIFY" >&2
[ -f "$file_path" ] || abort_verify "$file not exists"
if [ -f "$hash_path" ]; then
  verify_file_hash "$file_path" "$hash_path" "$file"
else
  ui_print "- Download from Magisk app"
fi
