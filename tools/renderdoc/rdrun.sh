#!/usr/bin/env bash
# Runs a RenderDoc replay script against a capture from the terminal.
#
#   tools/renderdoc/rdrun.sh <script.py> <capture.rdc> [args...]
#
# The distro's renderdoc has no standalone Python module, but qrenderdoc embeds
# one: `qrenderdoc --python` runs bootstrap.py, which opens the capture and runs
# the script with `rd`, `ctl` (ReplayController) and `args` as globals, then
# exits before the UI appears. The script's output is printed here. Needs a
# display session (the Qt app starts) and the GPU (replay). RD_TIMEOUT sets the
# timeout in seconds (default 600).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ $# -ge 2 ]] || { sed -n '2,11p' "$0"; exit 2; }
script="$(realpath "$1")"
capture="$(realpath "$2")"
shift 2
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
args=""
for a in "$@"; do args+="$a"$'\x1f'; done
RD_TOOLS="$here" RD_SCRIPT="$script" RD_CAPTURE="$capture" RD_OUT="$tmp/out" RD_ARGS="$args" \
  timeout "${RD_TIMEOUT:-600}" qrenderdoc --python "$here/bootstrap.py" >"$tmp/qrenderdoc.log" 2>&1 || true
if [[ ! -s "$tmp/out" ]]; then
  echo "no output from qrenderdoc; its log:" >&2
  tail -n 20 "$tmp/qrenderdoc.log" >&2
  exit 1
fi
cat "$tmp/out"
! grep -q '^RDRUN-FAILED$' "$tmp/out"
