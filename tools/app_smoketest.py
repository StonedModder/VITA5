#!/usr/bin/env python3
# VITA5 - quick app input-loop smoke test, from the PC side of the rig.
# Copyright (C) 2026 VITA5 contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run AFTER launching the app on the PS5. Verifies everything the PC side can
# honestly verify before the hands-on input test:
#   * the PS5 answers TCP on the FTP port (network reachability),
#   * elfldr (:9021) accepts a TCP connection,
#   * FTP (:2120) logs in and serves /data/homebrew,
#   * dist/<title-id>/eboot.bin exists locally (size + sha256 reported),
#   * the deployed /data/homebrew/<title-id>/eboot.bin matches the local
#     build byte for byte (deploy_ftp.sh-style readback + hash compare),
#   * best-effort, when the Vita's FTP app happens to be open: the
#     input_receiver plugin line is in ur0:tai/config.txt and the tail of
#     ur0:/tai/input_receiver.log is printed.
#
# It cannot press buttons for you: input effects are verified by the
# padtest/touchswipe reference payloads (tools/payload_send.py) and the Vita
# plugin log. The elfldr probe sends ZERO payload bytes - a raw ELF stream
# would be executed, and HTTP PUT is not elfldr's protocol (raw TCP only).
#
# Usage: python3 tools/app_smoketest.py [--title-id PPSA39410] [--host H]
#   PS5_HOST          PS5 host             (required, or pass --host)
#   PS5_FTP_PORT      FTP port             (default 2120)
#   PS5_FTP_USER      FTP user             (default anonymous)
#   PS5_FTP_PASSWORD  FTP password         (default the stock template's
#                      anonymous value; keep real credentials in the
#                      environment, never in this file)
#   PS5_ELFLDR_PORT   elfldr port          (default 9021)
#   VITA_FTP_HOST     Vita host            (required for Vita checks)
#   VITA_FTP_PORT     Vita FTP port        (default 1337)
#   VITA_FTP_USER     Vita FTP user        (default anonymous)
#   VITA_FTP_PASSWORD Vita FTP password    (default anonymous)
#
# Exit status: 0 when every required check passes (WARN/SKIP never fail),
# 1 when any required check fails, 2 on usage errors.

import argparse
import hashlib
import json
import os
import re
import socket
import sys
from ftplib import FTP, error_perm

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PS5_HOST = os.environ.get("PS5_HOST", "")
PS5_FTP_PORT = int(os.environ.get("PS5_FTP_PORT", "2120"))
PS5_FTP_USER = os.environ.get("PS5_FTP_USER", "anonymous")
PS5_FTP_PASSWORD = os.environ.get("PS5_FTP_PASSWORD", "codex")
PS5_ELFLDR_PORT = int(os.environ.get("PS5_ELFLDR_PORT", "9021"))

VITA_FTP_HOST = os.environ.get("VITA_FTP_HOST", "")
VITA_FTP_PORT = int(os.environ.get("VITA_FTP_PORT", "1337"))
VITA_FTP_USER = os.environ.get("VITA_FTP_USER", "anonymous")
VITA_FTP_PASSWORD = os.environ.get("VITA_FTP_PASSWORD", "anonymous")

PLUGIN_LINE = "ur0:tai/input_receiver.skprx"
LOG_NAME = "input_receiver.log"

PASS, FAIL, WARN, SKIP = "PASS", "FAIL", "WARN", "SKIP"


class Results:
    """Ordered check results; required failures drive the exit status."""

    def __init__(self):
        self.rows = []

    def add(self, status, name, detail, required=True):
        self.rows.append((status, name, detail, required))
        print(f"[{status}] {name}: {detail}")
        return status

    def summary(self):
        counts = {status: 0 for status in (PASS, FAIL, WARN, SKIP)}
        for status, _, _, _ in self.rows:
            counts[status] += 1
        print(
            "==> [smoke] summary: "
            + ", ".join(f"{counts[s]} {s.lower()}" for s in (PASS, FAIL, WARN, SKIP))
        )
        hard = any(status == FAIL and required for status, _, _, required in self.rows)
        return 1 if hard else 0


def reply_code(error):
    return str(error).split(maxsplit=1)[0]


def is_missing(error):
    """Only an explicit 'no such entry' 550 counts as absence (same rule as
    tools/deploy_ftp.sh). Anything else propagates. Measured wording on the
    PS5 ftpsrv: CWD into a missing path is '550 Not a directory.' (matched
    here), while RETR/DELE of a missing file say 'Cannot open/delete file.'
    (NOT matched - ambiguous, so file existence is checked by listing)."""
    text = str(error).lower()
    return reply_code(error) == "550" and (
        "no such" in text or "not found" in text or "not a directory" in text
    )


def check_ps5_reachable(results, host, port):
    try:
        socket.getaddrinfo(host, port)
    except OSError as error:
        results.add(FAIL, "ps5-reachable", f"cannot resolve {host}: {error}")
        return False
    try:
        with socket.create_connection((host, port), timeout=5):
            pass
    except OSError as error:
        results.add(FAIL, "ps5-reachable", f"{host} answers no TCP on :{port}: {error}")
        return False
    results.add(PASS, "ps5-reachable", f"{host} accepts TCP connections")
    return True


def check_elfldr(results, host, port):
    """TCP connect only. An ELF sent here would EXECUTE on the console, and
    HTTP PUT is not elfldr's protocol - so this probe deliberately sends
    zero payload bytes and closes."""
    try:
        with socket.create_connection((host, port), timeout=5):
            pass
    except OSError as error:
        results.add(FAIL, "elfldr-tcp", f"no listener on {host}:{port}: {error}")
        return False
    results.add(
        PASS,
        "elfldr-tcp",
        f"raw-TCP elfldr listening on {host}:{port} (0 bytes sent; "
        "deploy payloads with tools/payload_send.py, never HTTP)",
    )
    return True


def local_artifact(results, artifact, title_id):
    """Verify the built app folder; report eboot.bin size + sha256."""
    eboot = os.path.join(artifact, "eboot.bin")
    if not os.path.isfile(eboot) or os.path.getsize(eboot) == 0:
        results.add(FAIL, "local-eboot", f"missing or empty: {eboot}")
        return None
    with open(eboot, "rb") as handle:
        payload = handle.read()
    digest = hashlib.sha256(payload).hexdigest()
    results.add(
        PASS,
        "local-eboot",
        f"{eboot}: {len(payload)} bytes sha256={digest}",
    )
    for relative in ("sce_module/libc.prx", "sce_sys/param.json"):
        path = os.path.join(artifact, *relative.split("/"))
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            results.add(FAIL, "local-artifact", f"missing or empty: {path}")
    param = os.path.join(artifact, "sce_sys", "param.json")
    if os.path.isfile(param):
        try:
            with open(param, encoding="utf-8") as handle:
                built_id = json.load(handle).get("titleId", "")
            if built_id != title_id:
                results.add(
                    WARN,
                    "local-artifact",
                    f"param.json titleId {built_id!r} != expected {title_id!r}",
                )
        except (OSError, ValueError) as error:
            results.add(WARN, "local-artifact", f"cannot parse param.json: {error}")
    return payload


def ftp_session(host, port, user, password):
    ftp = FTP()
    ftp.connect(host, port, timeout=15)
    ftp.login(user, password)
    return ftp


def list_entries(ftp, path):
    """MLSD listing {name: facts}; {} only when the directory is provably
    absent (550-tolerant handling from tools/deploy_ftp.sh)."""
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


def check_ftp(results, host, port, user, password, title_id, local_eboot):
    """FTP login + deployed-tree verification via readback + hash compare."""
    homebrew_root = "/data/homebrew"
    candidate = f"{homebrew_root}/{title_id}"
    try:
        with ftp_session(host, port, user, password) as ftp:
            results.add(PASS, "ftp-login", f"logged in to ftp://{host}:{port} as {user}")
            entries = list_entries(ftp, homebrew_root)
            results.add(
                PASS,
                "ftp-root",
                f"{homebrew_root} lists {len(entries)} entries",
            )
            deployed = list_entries(ftp, candidate)
            if not deployed:
                results.add(
                    WARN,
                    "remote-deploy",
                    f"{candidate}/ not deployed yet - run: bash tools/deploy_app.sh",
                )
                return
            remote_names = set(deployed)
            for critical in ("eboot.bin", "sce_sys"):
                if critical not in remote_names:
                    results.add(
                        FAIL,
                        "remote-deploy",
                        f"{candidate}/{critical} missing on the console",
                    )
                    return
            if local_eboot is None:
                results.add(
                    WARN,
                    "remote-deploy",
                    f"{candidate}/ present, but no local eboot.bin to compare against",
                )
                return
            digest = hashlib.sha256()
            size = 0

            def accumulate(chunk):
                nonlocal size
                size += len(chunk)
                digest.update(chunk)

            ftp.retrbinary(f"RETR {candidate}/eboot.bin", accumulate)
            local_digest = hashlib.sha256(local_eboot).hexdigest()
            if digest.hexdigest() != local_digest or size != len(local_eboot):
                results.add(
                    FAIL,
                    "remote-deploy",
                    f"{candidate}/eboot.bin does not match the local build "
                    f"(remote {size} bytes sha256={digest.hexdigest()}, "
                    f"local {len(local_eboot)} bytes sha256={local_digest}) - "
                    "redeploy: bash tools/deploy_app.sh",
                )
                return
            results.add(
                PASS,
                "remote-deploy",
                f"{candidate}/eboot.bin matches local build "
                f"({size} bytes sha256={local_digest}); "
                f"{len(deployed)} top-level entries on the console",
            )
    except (error_perm, OSError, EOFError) as error:
        results.add(FAIL, "ftp-login", f"FTP error on {host}:{port}: {error}")


# Vita-side helpers: stepwise navigation over FTPVita, same shape as
# tools/vita_install_input_receiver.py.
def find_tai_dir(ftp):
    for parts in (("ur0:", "tai"), ("ur0", "tai")):
        try:
            ftp.cwd("/")
            for part in parts:
                ftp.cwd(part)
            return "/".join(parts)
        except error_perm:
            continue
    return None


def remote_read(ftp, path):
    chunks = []
    ftp.retrbinary(f"RETR {path}", chunks.append)
    return b"".join(chunks)


def log_tail(log, lines):
    """Last N display records. The plugin appends some records (notably the
    'touch counters:' line) WITHOUT a trailing newline, so run-on lines are
    split at each such boundary before the tail is taken."""
    records = []
    for chunk in log.splitlines():
        records.extend(re.split(r"(?<=\d)(?=touch counters:)", chunk))
    records = [record for record in (r.strip() for r in records) if record]
    return records[-lines:]


def check_vita(results, log_lines):
    """Advisory only: the Vita FTP app is its own step in the loop (it cannot
    be open during a LiveArea/game input test), so unreachability is SKIP,
    never FAIL."""
    try:
        with ftp_session(
            VITA_FTP_HOST, VITA_FTP_PORT, VITA_FTP_USER, VITA_FTP_PASSWORD
        ) as ftp:
            tai = find_tai_dir(ftp)
            if tai is None:
                results.add(
                    SKIP,
                    "vita-plugin",
                    "ur0:tai not reachable over the Vita FTP "
                    f"(ftp://{VITA_FTP_HOST}:{VITA_FTP_PORT})",
                    required=False,
                )
                return
            config = remote_read(ftp, "config.txt").decode("utf-8", "replace")
            section, active = [], False
            for line in config.replace("\r\n", "\n").split("\n"):
                if line.strip().startswith("*"):
                    active = line.strip() == "*KERNEL"
                if active:
                    section.append(line)
            if any(line.strip() == PLUGIN_LINE for line in section):
                results.add(
                    PASS,
                    "vita-plugin",
                    f"{PLUGIN_LINE} present under *KERNEL of {tai}/config.txt",
                    required=False,
                )
            else:
                results.add(
                    WARN,
                    "vita-plugin",
                    f"{PLUGIN_LINE} NOT in {tai}/config.txt - install with: "
                    "python3 tools/vita_install_input_receiver.py",
                    required=False,
                )
            try:
                log = remote_read(ftp, LOG_NAME).decode("utf-8", "replace")
            except error_perm as error:
                # 'Absent' and 'unreadable' are indistinguishable from the
                # 550 text alone (this server says 'Cannot open file.' for
                # both a missing file and worse); report both possibilities
                # and the server's actual reply instead of guessing.
                results.add(
                    WARN,
                    "vita-log",
                    f"cannot read {tai}/{LOG_NAME} ({error}) - not written "
                    "yet (the plugin logs after the first input event) or "
                    "unreadable",
                    required=False,
                )
                return
            tail = log_tail(log, log_lines)
            results.add(
                PASS,
                "vita-log",
                f"{tai}/{LOG_NAME}: {len(log)} bytes, last {len(tail)} records:",
                required=False,
            )
            for line in tail:
                shown = line if len(line) <= 160 else line[:160] + " ..."
                print(f"       | {shown}")
    except (error_perm, OSError, EOFError) as error:
        results.add(
            SKIP,
            "vita-plugin",
            f"Vita FTP not reachable at {VITA_FTP_HOST}:{VITA_FTP_PORT} ({error}) - "
            "open the Vita's FTP app as its own step to check the plugin/log",
            required=False,
        )


def main():
    parser = argparse.ArgumentParser(
        description="VITA5 app input-loop smoke test (PC-side checks)"
    )
    parser.add_argument("--host", default=PS5_HOST, help="PS5 host (default %(default)s)")
    parser.add_argument(
        "--title-id", default="PPSA39410", help="app title id (default %(default)s)"
    )
    parser.add_argument(
        "--skip-vita", action="store_true", help="skip the advisory Vita FTP checks"
    )
    parser.add_argument(
        "--vita-log-lines",
        type=int,
        default=12,
        help="lines of ur0:/tai/input_receiver.log to print (default %(default)s)",
    )
    args = parser.parse_args()
    if not re.fullmatch(r"PPSA[0-9]{5}", args.title_id):
        parser.error("title-id must use PPSA followed by five digits")

    artifact = os.path.join(REPO_ROOT, "dist", args.title_id)
    print(f"==> [smoke] PS5 target: {args.host} (ftp :{PS5_FTP_PORT}, elfldr :{PS5_ELFLDR_PORT})")
    print(f"==> [smoke] app folder: {artifact}")
    print("==> [smoke] run this while the app is up on the PS5; input effects "
          "themselves are proven by padtest/touchswipe + the Vita log")

    results = Results()
    if check_ps5_reachable(results, args.host, PS5_FTP_PORT):
        check_elfldr(results, args.host, PS5_ELFLDR_PORT)
    else:
        results.add(FAIL, "elfldr-tcp", "cannot check: PS5 unreachable")
    local_eboot = local_artifact(results, artifact, args.title_id)
    check_ftp(
        results,
        args.host,
        PS5_FTP_PORT,
        PS5_FTP_USER,
        PS5_FTP_PASSWORD,
        args.title_id,
        local_eboot,
    )
    if not args.skip_vita:
        check_vita(results, args.vita_log_lines)
    return results.summary()


if __name__ == "__main__":
    sys.exit(main())
