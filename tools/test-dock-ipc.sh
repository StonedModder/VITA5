#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cc=${HOST_CC:-clang}
mkdir -p "$root/build/tests"
build=$(mktemp -d "$root/build/tests/dock-ipc.XXXXXX")
trap 'rm -rf "$build"' EXIT
flags=(-std=gnu11 -D_GNU_SOURCE -DVD5_HOST_IPC_TEST -O1 -g -Wall -Wextra -Wpedantic -Werror)
if [[ ${VD_SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"$cc" "${flags[@]}" -I"$root/core/include" \
    "$root/tests/test_dock_ipc.c" "$root/core/src/vd_ipc.c" \
    "$root/core/src/pad_passthrough.c" -o "$build/test_dock_ipc"
timeout --signal=TERM --kill-after=2s 15s "$build/test_dock_ipc"
