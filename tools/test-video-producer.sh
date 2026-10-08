#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux/WSL integration test: real UVC reader + real producer, artificial USB.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cc=${HOST_CC:-clang}
read -r -a cflags <<<"${HOST_TEST_CFLAGS:--std=c11 -O2 -Wall -Wextra -Wpedantic -Werror}"
read -r -a ldflags <<<"${HOST_TEST_LDFLAGS:-}"
if [[ ${VD_SANITIZE:-0} == 1 ]]; then
    cflags+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    ldflags+=(-fsanitize=address,undefined)
fi
# Keep generated artifacts in the ignored test build tree, not system temp.
# Preserve timeout's exit status and give concurrent runs private directories.
mkdir -p "$root/build/tests"
build=$(mktemp -d "$root/build/tests/video-producer.XXXXXX")
trap 'rm -rf "$build"' EXIT
printf '%s\n' '==> [test-video] build real uvc_stream.c + video_producer.c'
"$cc" "${cflags[@]}" -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -pthread \
    -I"$root/tests/host_usb" -I"$root/core/include" -I"$root/payload" \
    "$root/tests/test_video_producer.c" \
    "$root/core/src/uvc_protocol.c" "$root/core/src/uvc_stream.c" \
    "$root/payload/video_producer.c" "$root/payload/vd_shm_prod.c" \
    "$root/payload/dock_ipc.c" "$root/core/src/vd_ipc.c" \
    "$root/core/src/pad_passthrough.c" \
    -Wl,--wrap=ioctl "${ldflags[@]}" -o "$build/test_video_producer"
printf '%s\n' '==> [test-video] run artificial USB fixture (NOT hardware output)'
timeout --signal=TERM --kill-after=2s 10s "$build/test_video_producer"
timeout --signal=TERM --kill-after=2s 10s "$build/test_video_producer" reopen
timeout --signal=TERM --kill-after=2s 10s "$build/test_video_producer" partial-eof
timeout --signal=TERM --kill-after=2s 10s "$build/test_video_producer" partial-error
timeout --signal=TERM --kill-after=2s 10s "$build/test_video_producer" helper
