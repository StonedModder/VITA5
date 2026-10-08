#!/usr/bin/env python3
# VITA5 - install the input-receiver kernel plugin on the PS Vita over FTP.
# Copyright (C) 2026 VITA5 contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Uploads vita-side/input-receiver/input_receiver.skprx to ur0:tai/ and adds
# `ur0:tai/input_receiver.skprx` under the *KERNEL section of ur0:tai/config.txt
# (creating the section when missing). No other config.txt lines are touched -
# in particular any existing streaming plugin entries are left alone. A backup
# of config.txt is written to ur0:tai/config.txt.bak before the first edit.
#
# Usage: python3 tools/vita_install_input_receiver.py [path/to/input_receiver.skprx]
#   VITA_FTP_HOST      Vita host           (required)
#   VITA_FTP_PORT      FTP port            (default 1337)
#   VITA_FTP_USER      FTP user            (default anonymous)
#   VITA_FTP_PASSWORD  FTP password        (default anonymous; keep real
#                      credentials in the environment, never in this file)

import hashlib
import io
import os
import sys
from ftplib import FTP, error_perm

HOST = os.environ.get("VITA_FTP_HOST", "")
PORT = int(os.environ.get("VITA_FTP_PORT", "1337"))
USER = os.environ.get("VITA_FTP_USER", "anonymous")
PASSWORD = os.environ.get("VITA_FTP_PASSWORD", "anonymous")

PLUGIN_LINE = "ur0:tai/input_receiver.skprx"
# FTPVita (VitaShell) exposes volumes as colon-suffixed directories at "/"
# and only accepts one path component per CWD, so navigation is stepwise.
TAI_PARTS = ("ur0:", "tai")
CONFIG_NAME = "config.txt"


def remote_read(ftp, path):
    chunks = []
    ftp.retrbinary(f"RETR {path}", chunks.append)
    return b"".join(chunks)


def find_tai_dir(ftp):
    for parts in (("ur0:", "tai"), ("ur0", "tai")):
        try:
            ftp.cwd("/")
            for part in parts:
                ftp.cwd(part)
            return "/".join(parts)
        except error_perm:
            continue
    raise SystemExit("cannot find ur0:tai over FTP (tried ur0:/tai, ur0/tai)")


def patch_config(text):
    """Return (new_text, changed) with PLUGIN_LINE under *KERNEL."""
    eol = "\r\n" if "\r\n" in text else "\n"
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    if any(line.strip() == PLUGIN_LINE for line in lines):
        return text, False
    for index, line in enumerate(lines):
        if line.strip() == "*KERNEL":
            lines.insert(index + 1, PLUGIN_LINE)
            return eol.join(lines), True
    # No *KERNEL section: add one at the top (taiHEN convention), keep the rest.
    return "*KERNEL" + eol + PLUGIN_LINE + eol + eol.join(lines), True


def kernel_section(text):
    lines = text.replace("\r\n", "\n").split("\n")
    out, active = [], False
    for line in lines:
        if line.strip().startswith("*"):
            active = line.strip() == "*KERNEL"
        if active:
            out.append(line)
    return "\n".join(out).rstrip("\n")


def main():
    plugin = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "vita-side", "input-receiver", "input_receiver.skprx",
    )
    with open(plugin, "rb") as handle:
        payload = handle.read()
    local_sha = hashlib.sha256(payload).hexdigest()
    print(f"==> local {plugin}: {len(payload)} bytes sha256={local_sha}")

    with FTP() as ftp:
        ftp.connect(HOST, PORT, timeout=15)
        ftp.login(USER, PASSWORD)
        print(f"==> connected to ftp://{HOST}:{PORT} as {USER}")
        tai = find_tai_dir(ftp)  # leaves cwd inside ur0:tai
        print(f"==> working directory: {ftp.pwd()}")
        original = remote_read(ftp, CONFIG_NAME).decode("utf-8", "replace")
        print(f"==> read {tai}/{CONFIG_NAME} ({len(original)} bytes)")

        updated, changed = patch_config(original)
        if changed:
            try:
                remote_read(ftp, f"{CONFIG_NAME}.bak")
                print(f"==> backup already exists: {tai}/{CONFIG_NAME}.bak")
            except error_perm:
                ftp.storbinary(f"STOR {CONFIG_NAME}.bak", io.BytesIO(original.encode("utf-8")))
                print(f"==> backup written: {tai}/{CONFIG_NAME}.bak")

        # Upload the plugin (atomic-ish: temp name + rename when supported).
        remote_name = "input_receiver.skprx"
        temp = "input_receiver.skprx.upload"
        ftp.storbinary(f"STOR {temp}", io.BytesIO(payload))
        try:
            ftp.rename(temp, remote_name)
        except error_perm:
            ftp.storbinary(f"STOR {remote_name}", io.BytesIO(payload))
            ftp.delete(temp)
        print(f"==> uploaded {tai}/{remote_name}")

        if changed:
            ftp.storbinary(f"STOR {CONFIG_NAME}", io.BytesIO(updated.encode("utf-8")))
            print(f"==> updated {tai}/{CONFIG_NAME} (added {PLUGIN_LINE} under *KERNEL)")
        else:
            print(f"==> {PLUGIN_LINE} already present in {tai}/{CONFIG_NAME}; config untouched")

        # Read back and verify.
        back = remote_read(ftp, remote_name)
        back_sha = hashlib.sha256(back).hexdigest()
        print(f"==> readback {tai}/{remote_name}: {len(back)} bytes sha256={back_sha}")
        if back_sha != local_sha:
            raise SystemExit("ERROR: uploaded plugin hash mismatch on readback")
        final_cfg = remote_read(ftp, CONFIG_NAME).decode("utf-8", "replace")
        section = kernel_section(final_cfg)
        print("==> config.txt *KERNEL section after edit:")
        print(section)
        if PLUGIN_LINE not in section:
            raise SystemExit("ERROR: plugin line missing from *KERNEL section on readback")
        print("==> INSTALL VERIFIED")
        try:
            ftp.quit()
        except (EOFError, OSError):
            pass


if __name__ == "__main__":
    main()
