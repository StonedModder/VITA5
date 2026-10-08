#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Real upscale control flow with artificial GL/shader compilation failures.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cxx=${HOST_CXX:-clang++-18}
mkdir -p "$root/build/tests"
build=$(mktemp -d "$root/build/tests/upscaler-fallback.XXXXXX")
trap 'rm -rf "$build"' EXIT
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -DVD5_HOST_UPSCALER_TEST -DGL_GLEXT_PROTOTYPES \
    -I"$root/app/src" -I"$root/app/vendor/kit" \
    "$root/tests/test_upscaler_fallback.cpp" "$root/app/src/vd/upscaler.cpp" \
    -o "$build/test_upscaler_fallback"
timeout --signal=TERM --kill-after=2s 15s "$build/test_upscaler_fallback"
