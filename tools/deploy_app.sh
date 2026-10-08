#!/usr/bin/env bash
# VITA5 - dev-loop deployment of the built app folder to the PS5 over FTP.
# Copyright (C) 2026 VITA5 contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Same FTP staging/publish flow as tools/deploy_ftp.sh - hidden temporary
# names + rename into place, eboot.bin published first and sce_sys/param.json
# LAST, every path set to 0777 via SITE CHMOD (console wants open-to-all;
# 0644/0666 data or non-exec eboot causes "Can't start the game or app" /
# CE-107750-0). Verified against MLSD; every file read back and SHA-256-
# hashed - with
# one deliberate difference for the edit-test loop: the title is FIXED and
# redeployed, so existing remote files are UPDATED IN PLACE instead of
# refused. Only an explicit "no such entry" 550 counts as absence
# (is_missing below); arbitrary 550s propagate and abort. Remote files with
# no local counterpart are reported and left alone (never deleted).
#
# This script only DEPLOYS; it never launches anything on the console.
# Fully close the running app before deploying over it (docs/DEPLOYMENT.md).
#
# Usage: bash tools/deploy_app.sh [host] [title-id]
#   defaults: host = $PS5_HOST (required if the argument is omitted), title-id = PPSA39410
#   PS5_FTP_PORT      FTP port            (default 2120)
#   PS5_FTP_USER      FTP user            (default anonymous)
#   PS5_FTP_PASSWORD  FTP password        (default the stock template's
#                     anonymous value; keep real credentials in the
#                     environment, never in this file)

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

usage() {
    echo "usage: tools/deploy_app.sh [host] [title-id]" >&2
    exit 2
}

[[ $# -le 2 ]] || usage
host=${1:-${PS5_HOST:-}}
[[ -n $host ]] || { echo "pass the PS5 host or set PS5_HOST" >&2; exit 2; }
title_id=${2:-PPSA39410}
[[ $host =~ ^[A-Za-z0-9][A-Za-z0-9.-]*$ ]] || {
    echo "host must be an IPv4 address or hostname" >&2
    exit 2
}
[[ $title_id =~ ^PPSA[0-9]{5}$ ]] || {
    echo "title-id must use PPSA followed by five digits" >&2
    exit 2
}
port=${PS5_FTP_PORT:-2120}
[[ $port =~ ^[0-9]+$ ]] && (( 10#$port >= 1 && 10#$port <= 65535 )) || {
    echo "PS5_FTP_PORT must be between 1 and 65535" >&2
    exit 2
}
user=${PS5_FTP_USER:-anonymous}
# The stock template's anonymous-login default (root Makefile). Real
# credentials belong in PS5_FTP_USER/PS5_FTP_PASSWORD, never in this file.
password=${PS5_FTP_PASSWORD:-codex}

artifact="$root/dist/$title_id"
[[ -d $artifact ]] || { echo "missing build output folder: $artifact" >&2; exit 2; }
[[ -s $artifact/eboot.bin ]] || { echo "missing or empty: $artifact/eboot.bin" >&2; exit 2; }
[[ -s $artifact/sce_module/libc.prx ]] || { echo "missing or empty: $artifact/sce_module/libc.prx" >&2; exit 2; }
[[ -s $artifact/sce_sys/param.json ]] || { echo "missing or empty: $artifact/sce_sys/param.json" >&2; exit 2; }

command -v python3 >/dev/null || { echo "missing required command: python3" >&2; exit 2; }

printf '==> [deploy] Source: %s\n' "$artifact"
printf '==> [deploy] Target: ftp://%s:%s/data/homebrew/%s/ (update in place)\n' "$host" "$port" "$title_id"

python3 - "$host" "$port" "$user" "$password" "$title_id" "$artifact" <<'PY'
from ftplib import FTP, error_perm
from hashlib import sha256
from posixpath import dirname, join
import re
import secrets
import sys
from pathlib import Path

host, port, user, password, title_id, artifact_name = sys.argv[1:7]
homebrew_root = "/data/homebrew"


def native_path(path):
    """MSYS/bash hands POSIX-style drive paths (/c/...) that a Windows
    Python cannot open; remap to drive form. A POSIX Python path (including
    WSL's /mnt/c/...) passes through unchanged."""
    match = re.fullmatch(r"/([A-Za-z])/(.*)", path)
    if match:
        return f"{match.group(1).upper()}:/{match.group(2)}"
    return path


artifact = Path(native_path(artifact_name))
if not artifact.is_dir():
    raise RuntimeError(f"build output folder not readable from python3: {artifact}")
CANDIDATE_DIR = join(homebrew_root, title_id)

# Publish order: eboot.bin, then sce_sys/param.json LAST.
CRITICAL = ["eboot.bin", "sce_sys/param.json"]


def reply_code(error):
    return str(error).split(maxsplit=1)[0]


def is_missing(error):
    """Only an explicit 'no such entry' 550 counts as absence. Anything else
    (including arbitrary 550s) must propagate. Measured wording on the PS5
    ftpsrv: CWD into a missing path is '550 Not a directory.', RETR of a
    missing file is '550 Cannot open file.', DELE of a missing file is
    '550 Cannot delete file.' - only the first is unambiguous, so existence
    is always confirmed by listing (MLSD), never by error text."""
    text = str(error).lower()
    return reply_code(error) == "550" and (
        "no such" in text or "not found" in text or "not a directory" in text
    )


def validate_name(name):
    if not name or name in {".", ".."} or not re.fullmatch(r"[A-Za-z0-9._-]+", name):
        raise RuntimeError(f"unsafe FTP entry name: {name!r}")


def list_entries(ftp, path):
    """MLSD listing {name: facts}; {} only when the directory is provably absent."""
    previous = ftp.pwd()
    try:
        ftp.cwd(path)
    except error_perm as error:
        if is_missing(error):
            return {}
        raise
    try:
        return {name: facts for name, facts in ftp.mlsd() if name not in {".", ".."}}
    finally:
        ftp.cwd(previous)


def ensure_directory(ftp, path, mode="0777"):
    """Create path (and any missing parents) under the FTP CWD. After return the
    directory verifiably exists; mode=None skips chmod (shared non-title dirs)."""
    current = ""
    for component in path.strip("/").split("/"):
        validate_name(component)
        current += f"/{component}"
        try:
            ftp.mkd(current)
        except error_perm as error:
            try:
                previous = ftp.pwd()
                ftp.cwd(current)
                ftp.cwd(previous)
            except error_perm:
                raise error
        if mode is not None:
            site_chmod(ftp, current, mode)


def site_chmod(ftp, path, mode):
    last = None
    for command in (f"SITE CHMOD {mode} {path}", f"SITE chmod {mode} {path}"):
        try:
            ftp.sendcmd(command)
            return
        except error_perm as error:
            last = error
    raise RuntimeError(f"cannot set mode {mode} on {path}: {last}")


def remote_mode(facts):
    raw = facts.get("unix.mode") or facts.get("mode")
    if raw is None:
        return None
    if re.fullmatch(r"[0-7]{3,4}", raw):
        return raw[-4:].zfill(4)
    return None


def verify_modes(ftp, directory, expected):
    entries = list_entries(ftp, directory)
    problems = []
    for name, mode in sorted(expected.items()):
        facts = entries.get(name)
        if facts is None:
            problems.append(f"{join(directory, name)}: missing from MLSD")
            continue
        actual = remote_mode(facts)
        if actual is None:
            problems.append(f"{join(directory, name)}: server reports no mode fact")
        elif actual != mode:
            problems.append(f"{join(directory, name)}: mode {actual} != {mode}")
    if problems:
        raise RuntimeError("permission verification failed: " + "; ".join(problems))


def upload_atomic(ftp, local, remote, mode):
    directory = dirname(remote)
    name = remote.rsplit("/", 1)[-1]
    temporary = join(directory, f".{name}.{secrets.token_hex(6)}.upload")
    with local.open("rb") as source:
        ftp.storbinary(f"STOR {temporary}", source, blocksize=256 * 1024)
    # storbinary raises on an incomplete transfer; confirm the byte count
    # when the server supports SIZE before the rename publishes the file.
    try:
        size = ftp.size(temporary)
    except error_perm as error:
        text = str(error).lower()
        if reply_code(error) in {"500", "502", "504"} or any(
            word in text for word in ("support", "implement", "syntax")
        ):
            size = None  # SIZE unsupported; the readback pass still verifies.
        else:
            raise
    if size is not None and size != local.stat().st_size:
        raise RuntimeError(
            f"incomplete transfer for {remote}: {size} != {local.stat().st_size}"
        )
    site_chmod(ftp, temporary, mode)
    # Publish via rename so the final name only ever appears complete. On a
    # redeploy the final name already exists; the PS5 ftpsrv accepts RNTO
    # onto an existing entry (measured 2026-10-06), but a server that refuses
    # it gets the old copy removed and the rename retried. DELE error text is
    # ambiguous here ('550 Cannot delete file.' also answers for 'absent'),
    # so the old copy's removal is confirmed by listing, never by error text.
    try:
        ftp.rename(temporary, remote)
    except error_perm as error:
        try:
            ftp.delete(remote)
        except error_perm:
            pass
        if name in list_entries(ftp, directory):
            raise error  # old copy survived the delete; surface the rename error
        try:
            ftp.rename(temporary, remote)
        except error_perm:
            raise error


def remote_files(ftp, directory):
    """Every regular file below directory as {relpath: (size, mode)}."""
    found = {}
    for name, facts in sorted(list_entries(ftp, directory).items()):
        validate_name(name)
        kind = facts.get("type", "file")
        path = join(directory, name)
        if kind == "dir":
            found.update(
                {
                    join(name, sub): value
                    for sub, value in remote_files(ftp, path).items()
                }
            )
        elif kind == "file":
            size = int(facts.get("size", -1))
            found[name] = (size, remote_mode(facts))
        else:
            raise RuntimeError(f"unexpected remote entry type {kind!r} at {path}")
    return found


with FTP() as ftp:
    ftp.connect(host, int(port), timeout=15)
    ftp.login(user, password)

    # Local file plan: everything else first, then eboot.bin, then param.json.
    local_files = sorted(
        path for path in artifact.rglob("*") if path.is_file()
    )
    order = {name: len(CRITICAL) + index for index, name in enumerate(CRITICAL)}
    plan = []
    for local in local_files:
        relative = local.relative_to(artifact).as_posix()
        for part in relative.split("/"):
            validate_name(part)
        plan.append((order.get(relative, 0), relative, local))
    plan.sort(key=lambda item: (item[0], item[1]))
    published = [relative for _, relative, _ in plan]
    for name in CRITICAL:
        if published.count(name) != 1:
            raise RuntimeError(f"critical file {name} missing from the app folder")

    def file_mode(relative):
        # Open-to-all for the whole title tree. Manual FTP defaults to 0666
        # (no +x) and the old 0755/0644 split still fails some loaders.
        return "0777"

    # Every ancestor directory of a published file; dirs are always 0777.
    parent_dirs = sorted(
        {
            "/".join(relative.split("/")[:index])
            for _, relative, _ in plan
            for index in range(1, len(relative.split("/")))
        }
    )
    ensure_directory(ftp, homebrew_root, mode=None)
    for directory in parent_dirs:
        ensure_directory(ftp, join(CANDIDATE_DIR, directory))
    ensure_directory(ftp, CANDIDATE_DIR)

    # Dev-loop divergence from deploy_ftp.sh: existing files are expected and
    # updated in place. Remote-only files are leftovers from an earlier build;
    # report them, never delete (FTP removal is deliberately out of scope).
    existing = remote_files(ftp, CANDIDATE_DIR) if list_entries(ftp, CANDIDATE_DIR) else {}
    stale = sorted(set(existing) - set(published))
    if stale:
        print(f"==> [deploy] note: {len(stale)} remote-only file(s) left in place:")
        for name in stale:
            print(f"==> [deploy]   stale {join(CANDIDATE_DIR, name)}")

    print(f"==> [deploy] Publishing {len(plan)} files; eboot.bin, then param.json LAST")
    for index, (_, relative, local) in enumerate(plan, 1):
        print(f"==> [deploy] [{index}/{len(plan)}] {relative}")
        upload_atomic(ftp, local, join(CANDIDATE_DIR, relative), file_mode(relative))

    # Explicit modes were set per file/dir above; verify every one via MLSD.
    expected_by_dir = {}
    for relative in published:
        directory, _, name = relative.rpartition("/")
        expected_by_dir.setdefault(directory, {})[name] = file_mode(relative)
    for directory in parent_dirs:
        parent, _, name = directory.rpartition("/")
        expected_by_dir.setdefault(parent, {})[name] = "0777"
    for directory, expected in sorted(expected_by_dir.items()):
        verify_modes(
            ftp,
            join(CANDIDATE_DIR, directory) if directory else CANDIDATE_DIR,
            expected,
        )
    print("==> [deploy] Verified remote modes (0777 open-to-all on every path)")

    # Readback: retrieve + hash every file, compare count and sizes.
    remote = remote_files(ftp, CANDIDATE_DIR)
    if len(remote) < len(plan):
        raise RuntimeError(
            f"remote file count {len(remote)} < local file count {len(plan)}"
        )
    for _, relative, local in plan:
        entry = remote.get(relative)
        if entry is None:
            raise RuntimeError(f"missing after upload: {relative}")
        size, _mode = entry
        if size != local.stat().st_size:
            raise RuntimeError(
                f"size mismatch for {relative}: remote {size} != local {local.stat().st_size}"
            )
        digest = sha256()
        ftp.retrbinary(f"RETR {join(CANDIDATE_DIR, relative)}", digest.update)
        local_digest = sha256(local.read_bytes()).hexdigest()
        if digest.hexdigest() != local_digest:
            raise RuntimeError(f"hash mismatch after readback: {relative}")
        print(f"==> [deploy] verified {relative} ({size} bytes)")

    print(f"Deployment complete: ftp://{host}:{port}{CANDIDATE_DIR}/")
    print(f"Deployed title: {title_id} ({len(plan)} files, updated in place)")
    try:
        ftp.quit()
    except (EOFError, OSError):
        pass
PY
