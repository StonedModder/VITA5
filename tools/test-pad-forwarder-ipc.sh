#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cc=${HOST_CC:-clang}
read -r -a cflags <<<"${HOST_TEST_CFLAGS:--std=c11 -O2 -Wall -Wextra -Wpedantic -Werror}"
read -r -a ldflags <<<"${HOST_TEST_LDFLAGS:-}"
if [[ ${VD_SANITIZE:-0} == 1 ]]; then
    cflags+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    ldflags+=(-fsanitize=address,undefined)
fi
mkdir -p "$root/build/tests"
build=$(mktemp -d "$root/build/tests/pad-forwarder-ipc.XXXXXX")
trap 'rm -rf "$build"' EXIT
"$cc" "${cflags[@]}" -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -pthread \
    -I"$root/tests/host_usb" -I"$root/core/include" -I"$root/payload" \
    "$root/tests/test_pad_forwarder_ipc.c" "$root/payload/pad_forwarder.c" \
    "$root/payload/dock_ipc.c" "$root/core/src/vd_ipc.c" \
    "$root/core/src/pad_passthrough.c" "${ldflags[@]}" -o "$build/test_pad_forwarder_ipc"
result=0
for mode in ${PAD_TEST_MODES:-overrun reset touch slow absent saturation failure reconnect}; do
    timeout --signal=TERM --kill-after=2s 10s "$build/test_pad_forwarder_ipc" "$mode" || result=1
done
exit "$result"
