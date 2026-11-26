#!/usr/bin/env bash
# Smoke-tests a built mod under plain Wine, without the game:
# loads the proxy DLL into tests/smoke_host.exe (renamed to the game's exe name),
# then prints the host output and the mod's log.
#
#   tools/smoke-test.sh [--game rememberme] [--preset mingw-i686-release] [--dxvk <32-bit d3d9.dll>]
#
# Plain Wine uses wined3d, so the DXVK interop path is only exercised when a
# DXVK d3d9.dll is given, e.g. Proton's from the game prefix:
#   --dxvk ~/.local/share/Steam/steamapps/compatdata/228300/pfx/drive_c/windows/syswow64/d3d9.dll
#
# Uses a throwaway WINEPREFIX under ${XDG_CACHE_HOME:-~/.cache}/vrmod-smoke
# (created on first run, which takes a little while).
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
game="rememberme"
preset="mingw-i686-release"
dxvk=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --game)   game="$2"; shift 2 ;;
    --preset) preset="$2"; shift 2 ;;
    --dxvk)   dxvk="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# shellcheck source=/dev/null
source "$repo/games/$game/smoke.env"   # HOST_EXE_NAME, LOG_FILE
build="$repo/build/$preset"
cache="${XDG_CACHE_HOME:-$HOME/.cache}/vrmod-smoke"
run_dir="$cache/run-$game"
export WINEPREFIX="$cache/prefix"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="dinput8=n,b;mscoree=;mshtml="

rm -rf "$run_dir" && mkdir -p "$run_dir"
cp "$build/tests/smoke_host.exe" "$run_dir/$HOST_EXE_NAME"
cp "$build/games/$game/"*.dll "$build/games/$game/"*.ini "$run_dir/"
if [[ -n "$dxvk" ]]; then
  cp "$dxvk" "$run_dir/d3d9.dll"
  WINEDLLOVERRIDES="$WINEDLLOVERRIDES;d3d9=n"
fi

[[ -d "$WINEPREFIX" ]] || { echo "creating Wine prefix in $WINEPREFIX ..."; wineboot -i >/dev/null 2>&1; }

cd "$run_dir"
status=0
wine "$HOST_EXE_NAME" || status=$?
echo "---- $LOG_FILE ----"
cat "$run_dir/$LOG_FILE" 2>/dev/null || echo "(no log written)"
exit "$status"
