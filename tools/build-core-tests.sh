#!/usr/bin/env bash
# VITA5 - build and run the core host unit tests (pure logic only, no PS5).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Covers tests/test_host.c (UVC descriptor parse, NV12 math, pad wire),
# tests/test_usb_transfer.c (completion validation, pad round-trip) and
# tests/test_uac_audio.c (UAC PCM math, isoc framing + packet reassembly).
# Run under WSL Ubuntu; git-bash 'cc' is broken devkitPro MSYS2 gcc.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cc=${HOST_CC:-clang}
read -r -a cflags <<<"${HOST_TEST_CFLAGS:--std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -ffunction-sections -fdata-sections}"
read -r -a ldflags <<<"${HOST_TEST_LDFLAGS:--Wl,--gc-sections}"
build="$root/build/tests"
mkdir -p "$build"

build_and_run() {
    local name=$1
    shift
    echo "==> [test-core] build $name"
    "$cc" "${cflags[@]}" -I"$root/core/include" "$@" "${ldflags[@]}" -o "$build/$name"
    echo "==> [test-core] run $name"
    "$build/$name"
}

build_and_run test_host \
    "$root/tests/test_host.c" \
    "$root/core/src/uvc_protocol.c" \
    "$root/core/src/pad_passthrough.c"

build_and_run test_usb_transfer \
    "$root/tests/test_usb_transfer.c" \
    "$root/core/src/pad_passthrough.c"

build_and_run test_uac_audio \
    "$root/tests/test_uac_audio.c"

bash "$root/tools/test-video-producer.sh"

bash "$root/tools/test-uac-framing.sh"
bash "$root/tools/test-pad-forwarder-ipc.sh"

echo "core host tests: ALL SUITES PASS"
