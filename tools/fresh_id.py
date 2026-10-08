#!/usr/bin/env python3
# VITA5 - fresh test title ID generator.
# Copyright (C) 2026 VITA5 contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints a fresh random PS5 title ID (PPSA + 5 random digits) and reserves it
# in tools/.used_ids so no ID is ever reused across test builds/deploys.
# PPSA99999 and every previously reserved/failed ID are excluded.

import argparse
import os
import re
import secrets
import sys
from pathlib import Path

USED_FILE = Path(__file__).resolve().parent / ".used_ids"
RESERVED = "PPSA99999"
ID_PATTERN = re.compile(r"PPSA\d{5}")
MAX_ATTEMPTS = 10000


def load_used(path):
    """Return the set of title IDs already recorded (used or failed)."""
    used = set()
    if not path.exists():
        return used
    with path.open(encoding="utf-8") as source:
        for line in source:
            token = line.split("#", 1)[0].split()[0:1]
            if token and ID_PATTERN.fullmatch(token[0]):
                used.add(token[0])
    return used


def append_record(path, title_id, status):
    """Append one '<id>  # <status>' record to the used-ID ledger."""
    with path.open("a", encoding="utf-8", newline="\n") as ledger:
        ledger.write(f"{title_id}  # {status}\n")


def lock_ledger(path):
    """Best-effort exclusive lock on the ledger file (POSIX flock)."""
    handle = path.open("a", encoding="utf-8")
    try:
        import fcntl

        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
    except ImportError:
        pass  # Non-POSIX host: single-user tooling, no lock available.
    return handle


def generate(used):
    """Generate one fresh ID with secrets and reserve it in the ledger."""
    with lock_ledger(USED_FILE) as locked:
        # Re-read under the lock: another process may have reserved meanwhile.
        used |= load_used(USED_FILE)
        for _ in range(MAX_ATTEMPTS):
            digits = "".join(secrets.choice("0123456789") for _ in range(5))
            candidate = f"PPSA{digits}"
            if candidate == RESERVED or candidate in used:
                continue
            append_record(USED_FILE, candidate, "reserved")
            return candidate
    raise SystemExit("could not draw a fresh title ID; the ledger is exhausted")


def main():
    parser = argparse.ArgumentParser(
        description="Generate a fresh random PS5 title ID (PPSA + 5 digits)."
    )
    parser.add_argument(
        "--fail",
        metavar="PPSA#####",
        help="mark an ID as failed so it is never generated again",
    )
    args = parser.parse_args()

    if args.fail:
        if not ID_PATTERN.fullmatch(args.fail):
            raise SystemExit("--fail expects an ID of the form PPSA#####")
        with lock_ledger(USED_FILE) as locked:
            used = load_used(USED_FILE)
            if args.fail not in used:
                append_record(USED_FILE, args.fail, "failed")
        print(args.fail)
        return

    print(generate(load_used(USED_FILE)))


if __name__ == "__main__":
    sys.exit(main())
