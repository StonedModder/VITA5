#!/usr/bin/env bash
# VITA5 - one-shot fresh-ID test build.
# Copyright (C) 2026 VITA5 contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Draws a fresh random title ID (tools/fresh_id.py), configures the stock
# application identity with it, builds the PS5 app folder through the stock
# build, and verifies the built output. Prints the fresh ID and the dist path.
#
# Usage: bash tools/build_test.sh

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

command -v python3 >/dev/null || { echo "missing required command: python3" >&2; exit 2; }
command -v make >/dev/null || { echo "missing required command: make" >&2; exit 2; }

# The runtime digest gate reads runtime/libc.prx.sha256 and
# tooling/native/runtime/*.txt as exact LF-terminated bytes; CRLF worktree
# bytes make the gate fail ("runtime checksum manifest does not match").
# .gitattributes already pins *.sha256/*.txt to eol=lf; never edit that file.
# Restore any drifted files to their indexed LF form before compiling.
ensure_lf() {
    local file fixed=0
    while IFS= read -r file; do
        if grep -q $'\r' "$file"; then
            sed -i 's/\r$//' "$file"
            printf '==> [lf] Restored LF line endings: %s\n' "$file"
            fixed=$((fixed + 1))
        fi
    done < <(find runtime tooling/native/runtime -type f \
        \( -name '*.sha256' -o -name '*.txt' \) 2>/dev/null | sort)
    printf '==> [lf] Digest-gate inputs are LF (%d file(s) repaired)\n' "$fixed"
}

ensure_lf

fresh=$(python3 tools/fresh_id.py)
[[ $fresh =~ ^PPSA[0-9]{5}$ && $fresh != PPSA99999 ]] || {
    echo "fresh_id.py produced an invalid title ID: $fresh" >&2
    exit 1
}
printf '==> [build] Fresh title ID: %s\n' "$fresh"

# The stock init target (tools/init-project.sh) writes titleId, conceptId
# and contentId coherently into the param.json the build actually consumes.
printf '==> [build] Configuring application identity\n'
make -C app init TITLE_ID="$fresh" APP_NAME="VITA5 $fresh"

printf '==> [build] Building the PS5 application folder\n'
make -C app

dist="$root/dist/$fresh"
param="$root/app/sce_sys/param.json"
[[ -d $dist ]] || { echo "missing build output folder: $dist" >&2; exit 1; }

python3 - "$param" "$fresh" <<'PY'
import json
import re
import sys
from pathlib import Path

param_path, title_id = sys.argv[1], sys.argv[2]
with Path(param_path).open(encoding="utf-8") as source:
    param = json.load(source)

errors = []
if param.get("titleId") != title_id:
    errors.append(f"titleId {param.get('titleId')!r} != {title_id!r}")
if param.get("conceptId") != title_id[4:]:
    errors.append(f"conceptId {param.get('conceptId')!r} != {title_id[4:]!r}")
content_id = param.get("contentId", "")
if f"-{title_id}_00-" not in content_id:
    errors.append(f"contentId {content_id!r} does not contain -{title_id}_00-")
if not re.fullmatch(r"[A-Z]{2}\d{4}-PPSA\d{5}_00-[A-Z0-9]{16}", content_id):
    errors.append(f"contentId {content_id!r} is malformed")
if errors:
    raise SystemExit("identity mismatch in param.json: " + "; ".join(errors))
print(f"Identity coherent: titleId={title_id} conceptId={title_id[4:]} contentId={content_id}")
PY

for required in eboot.bin sce_module/libc.prx sce_sys/param.json; do
    [[ -s $dist/$required ]] || {
        echo "missing or empty build output: $dist/$required" >&2
        exit 1
    }
done

built_param=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["titleId"])' "$dist/sce_sys/param.json")
[[ $built_param == "$fresh" ]] || {
    echo "built sce_sys/param.json carries titleId $built_param, expected $fresh" >&2
    exit 1
}

printf '==> [build] Verified eboot.bin + sce_module/libc.prx in dist output\n'
printf 'FRESH_TITLE_ID=%s\n' "$fresh"
printf 'DIST_PATH=%s\n' "$dist"
