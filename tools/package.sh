#!/usr/bin/env bash
# Zips a built mod so it can be extracted straight into the game's install folder.
#
#   tools/package.sh [--game rememberme] [--preset mingw-i686-release] [--version <v>]
#
# Output: dist/<PACKAGE_NAME>-<version>.zip. The version defaults to
# `git describe --tags --always --dirty`. Prints the zip path on the last line.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
game="rememberme"
preset="mingw-i686-release"
version=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --game)    game="$2"; shift 2 ;;
    --preset)  preset="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

game_dir="$repo/games/$game"
# shellcheck source=/dev/null
source "$game_dir/package.env"   # PACKAGE_NAME, PACKAGE_DIR, PACKAGE_FILES, PACKAGE_DOCS
build_dir="$repo/build/$preset/games/$game"
[[ -n "$version" ]] || version="$(git -C "$repo" describe --tags --always --dirty 2>/dev/null || echo unknown)"

stage="$repo/build/package/$PACKAGE_NAME"
rm -rf "$stage" && mkdir -p "$stage/$PACKAGE_DIR"
for f in $PACKAGE_FILES; do
  [[ -f "$build_dir/$f" ]] || { echo "missing build output: $build_dir/$f (build first)" >&2; exit 1; }
  cp "$build_dir/$f" "$stage/$PACKAGE_DIR/"
  # Ship without debug info; the unstripped DLL stays in the build tree for debugging.
  if [[ "$f" == *.dll ]] && command -v i686-w64-mingw32-strip >/dev/null; then
    i686-w64-mingw32-strip --strip-debug "$stage/$PACKAGE_DIR/$f"
  fi
done
for f in ${PACKAGE_DOCS:-}; do
  cp "$game_dir/$f" "$stage/"
done
cp "$repo/LICENSE" "$stage/LICENSE.txt"
echo "$version" > "$stage/VERSION.txt"

mkdir -p "$repo/dist"
zip="$repo/dist/$PACKAGE_NAME-$version.zip"
rm -f "$zip"
# cmake's built-in archiver: no dependency on a zip binary.
(cd "$stage" && cmake -E tar cf "$zip" --format=zip -- *)
echo "$zip"
