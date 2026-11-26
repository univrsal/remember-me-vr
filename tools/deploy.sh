#!/usr/bin/env bash
# Install/uninstall a built mod into the game's binary folder.
#
#   tools/deploy.sh install   [--game rememberme] [--preset mingw-i686-release]
#   tools/deploy.sh uninstall [--game rememberme]
#   tools/deploy.sh status    [--game rememberme]
#
# Installed files are recorded in <bin dir>/.vrmod-installed. The script never
# overwrites a file it didn't install, and uninstall only removes recorded files.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
action="${1:-}"; shift || true
game="rememberme"
preset="mingw-i686-release"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --game)   game="$2"; shift 2 ;;
    --preset) preset="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

env_file="$repo/games/$game/deploy.env"
[[ -f "$env_file" ]] || { echo "no $env_file" >&2; exit 1; }
# shellcheck source=/dev/null
source "$env_file"

bin_dir="$repo/$GAME_BIN_DIR"
build_dir="$repo/build/$preset/games/$game"
manifest="$bin_dir/.vrmod-installed"
[[ -d "$bin_dir" ]] || { echo "game folder not found: $bin_dir" >&2; exit 1; }

recorded() { [[ -f "$manifest" ]] && grep -qxF "$1" "$manifest"; }

case "$action" in
  install)
    for f in $MOD_FILES; do
      [[ -f "$build_dir/$f" ]] || { echo "missing build output: $build_dir/$f (build first)" >&2; exit 1; }
      if [[ -e "$bin_dir/$f" ]] && ! recorded "$f"; then
        echo "refusing to overwrite $bin_dir/$f: not installed by this script" >&2; exit 1
      fi
    done
    for f in $MOD_FILES; do
      # Keep the user's edited config; only install the default if absent.
      if [[ "$f" == *.ini && -e "$bin_dir/$f" ]]; then
        echo "kept existing $f"
      else
        cp "$build_dir/$f" "$bin_dir/$f"
        echo "installed $f"
      fi
      recorded "$f" || echo "$f" >> "$manifest"
    done
    echo
    echo "Steam launch options (app $STEAM_APP_ID) need the DLL override, e.g.:"
    echo "  WINEDLLOVERRIDES=\"dinput8=n,b\" %command%"
    ;;
  uninstall)
    [[ -f "$manifest" ]] || { echo "nothing installed"; exit 0; }
    while IFS= read -r f; do
      [[ -n "$f" ]] || continue
      rm -f "$bin_dir/$f" && echo "removed $f"
    done < "$manifest"
    rm -f "$manifest"
    ;;
  status)
    if [[ -f "$manifest" ]]; then
      echo "installed in $bin_dir:"; sed 's/^/  /' "$manifest"
    else
      echo "not installed"
    fi
    [[ -n "${LOG_FILE:-}" && -f "$bin_dir/$LOG_FILE" ]] && { echo; echo "last log lines:"; tail -n 15 "$bin_dir/$LOG_FILE"; }
    ;;
  *)
    sed -n '2,10p' "$0"; exit 2 ;;
esac
