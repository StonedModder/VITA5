#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux/WSL regression: real core/src/uac_audio.c transfer framing lifetime
# under an EBUSY USB_FS_START with an older pending transfer (artificial
# USB via -Wl,--wrap). Usage:
#   bash tools/test-uac-framing.sh                    # current production code
#   bash tools/test-uac-framing.sh build/old_uac.c    # e.g. a pre-fix revision
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cc=${HOST_CC:-clang}
read -r -a cflags <<<"${HOST_TEST_CFLAGS:--std=c11 -O2 -Wall -Wextra -Wpedantic -Werror}"
read -r -a ldflags <<<"${HOST_TEST_LDFLAGS:-}"
mkdir -p "$root/build/tests"
build=$(mktemp -d "$root/build/tests/uac-framing.XXXXXX")
trap 'rm -rf "$build"' EXIT
src=${1:-$root/core/src/uac_audio.c}
printf '%s\n' "==> [test-uac-framing] build real uac_audio.c from: $src"
"$cc" "${cflags[@]}" -D_POSIX_C_SOURCE=200809L \
    -I"$root/tests/host_usb" -I"$root/core/include" \
    "$root/tests/test_uac_framing.c" "$src" \
    -Wl,--wrap=ioctl,--wrap=open,--wrap=close,--wrap=poll \
    "${ldflags[@]}" -o "$build/test_uac_framing"
printf '%s\n' '==> [test-uac-framing] run (artificial USB, NOT hardware)'
timeout --signal=TERM --kill-after=2s 30s "$build/test_uac_framing"
