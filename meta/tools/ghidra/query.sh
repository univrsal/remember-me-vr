#!/usr/bin/env bash
# Read-only queries against the analysed Ghidra project (see Decompile.java):
#   meta/tools/ghidra/query.sh fn:0x00c8e0d0 vt:0x01300000:40 xref:0x00c8e0d0 str:SomeText
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$repo/re/ghidra/query.out"
rm -f "$out"
GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-8G}" "${GHIDRA_HOME:-/opt/ghidra}/support/analyzeHeadless" \
  "$repo/re/ghidra" RememberMe -process RememberMe.exe.unpacked.exe -noanalysis -readOnly \
  -scriptPath "$repo/tools/ghidra" -postScript Decompile.java "out:$out" "$@" > "$repo/re/ghidra/query.log" 2>&1 || true
if [[ -f "$out" ]]; then cat "$out"; else echo "query failed; see re/ghidra/query.log" >&2; grep -E "ERROR|Exception" "$repo/re/ghidra/query.log" | head >&2; exit 1; fi
