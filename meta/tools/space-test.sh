#!/usr/bin/env bash
# Builds and runs the host-side unit test for vrcore/ue3_space (needs a prior
# `cmake --preset mingw-i686-release` so the OpenXR headers are fetched).
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
deps="$repo/build/mingw-i686-release/_deps"
g++ -std=c++20 -Wall -DXR_NO_PROTOTYPES -I"$repo/src/core/include" -I"$deps/openxr_sdk-src/include" \
  "$repo/src/tests/space_test.cpp" "$repo/src/core/src/ue3/space.cpp" -o "$repo/build/space_test"
"$repo/build/space_test"
