#!/usr/bin/env bash
# Lets 32-bit OpenXR clients (like a 32-bit game under Proton) use SteamVR on Linux.
#
# Under Proton, a 32-bit game's wineopenxr uses Proton's 32-bit native OpenXR
# loader. That loader first looks for ~/.config/openxr/1/active_runtime.i686.json,
# then falls back to active_runtime.json, which SteamVR points at its 64-bit
# runtime ("wrong ELF class: ELFCLASS64"). SteamVR also ships a 32-bit client
# (bin/vrclient.so); this writes the i686 manifest pointing at it. 64-bit apps
# keep using active_runtime.json.
#
#   meta/tools/setup-openxr-i686.sh            # create (refuses to replace a different file)
#   meta/tools/setup-openxr-i686.sh --remove   # delete it again
set -euo pipefail

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/openxr/1"
manifest="$config_dir/active_runtime.i686.json"
steamvr="${STEAMVR_DIR:-${STEAM_ROOT:-$HOME/.local/share/Steam}/steamapps/common/SteamVR}"
marker='"_written_by": "remember-me-vr meta/tools/setup-openxr-i686.sh"'

if [[ "${1:-}" == "--remove" ]]; then
  if [[ -f "$manifest" ]] && grep -qF "$marker" "$manifest"; then
    rm -f "$manifest" && echo "removed $manifest"
  else
    echo "nothing to remove (missing, or not written by this script): $manifest"
  fi
  exit 0
fi

lib="$steamvr/bin/vrclient.so"
[[ -f "$lib" ]] || { echo "SteamVR 32-bit client not found: $lib (set STEAMVR_DIR)" >&2; exit 1; }
file -L "$lib" | grep -q "32-bit" || { echo "$lib is not a 32-bit library" >&2; exit 1; }

content="{
  \"file_format_version\": \"1.0.0\",
  $marker,
  \"runtime\": {
    \"name\": \"SteamVR\",
    \"VALVE_runtime_is_steamvr\": true,
    \"library_path\": \"$lib\"
  }
}"

if [[ -e "$manifest" ]] && ! grep -qF "$marker" "$manifest"; then
  echo "refusing to replace existing $manifest (not written by this script)" >&2
  exit 1
fi
mkdir -p "$config_dir"
printf '%s\n' "$content" > "$manifest"
echo "wrote $manifest -> $lib"
