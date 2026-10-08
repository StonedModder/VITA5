# VITA5 test-build + FTP-deploy tooling

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

One-shot tooling for testing on a real PS5 with a **fresh random title ID
per test**, uploaded over the authorized FTP. The stock build (root
Makefile, `tools/build.sh`) is used unchanged.

For the day-to-day **app test loop** — fresh-ID build, deploy, run, input
reference tests, Vita plugin install and plugin log — see
[The app test loop](#the-app-test-loop) below.

---

## The app test loop

Rig addresses in this loop: PS5 `<ps5-ip>` (FTP `:2120`, elfldr
`:9021`), PS Vita `<vita-ip>` (FTP `:1337`). Builds run in WSL Ubuntu;
the tools run from the repo root. Never run more than one payload sender at
a time — sending a payload **executes it on the console immediately**.

### 1. Build the app (WSL)

```bash
make -C app deps     # once per checkout: fetch + verify pinned ps5-opengl SDK
make -C app test     # host-side contract tests, no console needed
bash tools/build_test.sh  # prints FRESH_TITLE_ID and DIST_PATH
```

### 2. Deploy the app

Set `FRESH_TITLE_ID` to the value printed by the build and `PS5_HOST` to
the console address verified for this run.

```bash
bash tools/deploy_app.sh "$PS5_HOST" "$FRESH_TITLE_ID"
```

Deploys `dist/<fresh-title-id>/` to `/data/homebrew/<fresh-title-id>/`.
**Every changed app binary requires a new title ID**; reusing an old
identity can leave the console launching cached code. The script deploys
only; launch the newly named title after readback verification completes.
Dock-only payload changes do not require rebuilding or redeploying the app.

### 3. Run the app + smoke test

Launch the title manually from the PS5 home screen (Games section). While
it runs:

```bash
python3 tools/app_smoketest.py --title-id "$FRESH_TITLE_ID"
```

All checks green + the app live means the transport is ready for the
hands-on input test (see
[`app_smoketest.py`](#app_smoketestpy--pc-side-smoke-test) for exactly
what is and is not verified).

### 4. Deploy a payload

Payloads are built in WSL and sent to elfldr's raw-TCP interface:

```bash
# build (WSL):
export PS5_PAYLOAD_SDK=/mnt/c/ps5-payload-sdk/ps5-payload-sdk
make -C payload                      # all six VITA5-*.elf

# send + capture the payload's console output (--wait seconds, default 45):
python3 tools/payload_send.py payload/VITA5-streamdiag.elf --wait 60
# Run the dock only after the diagnostic exits and no old dock is active:
python3 tools/payload_send.py payload/VITA5-payload.elf --wait 1500
```

### 5. Input reference tests

```bash
python3 tools/payload_send.py payload/VITA5-padtest.elf
python3 tools/payload_send.py payload/VITA5-touchswipe.elf
```

- `VITA5-padtest.elf` — scripted pad-report sender (no app needed):
  neutral → D-pad → face buttons → shoulders/triggers → START/SELECT →
  stick sweeps → neutral, each step ~600 ms at 10 Hz. Whatever the Vita is
  showing should visibly react to every step.
- `VITA5-touchswipe.elf` — front-touchscreen swipes: 10 up, pause, 10
  down, pause, 10 up. If injection works, the LiveArea list ends 10 swipes
  up from where it started. **The Vita must be showing the LiveArea or a
  game** — see the pitfalls below.
- Also available: `VITA5-touchtest.elf` (touch channel), `VITA5-eptest.elf`
  (UVC control-path probe).

Watch the run's progress in the sender's captured console output and in the
Vita plugin log (step 7).

### 6. Install the Vita input plugin

The Vita's FTP app is **its own step** in the loop: it cannot be open while
a game/LiveArea input test is running, so do this before/after the tests,
never during.

```bash
# 1. on the Vita, open the FTP app (VitaShell/FTPVita)
# 2. from the repo root (set VITA_FTP_HOST to the Vita):
VITA_FTP_HOST=<vita-ip> python3 tools/vita_install_input_receiver.py
# 3. reboot the Vita: taiHEN must load the plugin before the stream gadget
#    registers, and it loads at boot
```

The installer uploads `vita-side/input-receiver/input_receiver.skprx` to
`ur0:tai/`, adds `ur0:tai/input_receiver.skprx` under `*KERNEL` of
`ur0:tai/config.txt` (backup first, no other lines touched), and verifies
everything by readback.

### 7. Read the plugin log

```bash
VITA_FTP_HOST=<vita-ip> python3 tools/app_smoketest.py
```

The `vita-plugin`/`vita-log` section prints the last records of
`ur0:/tai/input_receiver.log` (and verifies the plugin line in
`config.txt`). The full file is readable over the Vita FTP at
`ur0:/tai/input_receiver.log` — the plugin appends open-append-close per
event. Log lines look like `decode=... buttons=0x...`,
`emu: buttons=0x...`, `touch counters: chunks=... ok=... bad=...
injected=... reads=...`; `chunks`/`ok`/`bad`/`injected` are the touch
channel's cumulative counters. Note the plugin appends some records
(notably `touch counters:`) **without a trailing newline**, so naive
line-based tails see run-on lines. Detail and semantics:
[`vita-side/input-receiver/README.md`](../vita-side/input-receiver/README.md).

### Two hard-won pitfalls (from `docs/VITA_INPUT_API.md`)

1. **elfldr is raw TCP, not HTTP.** Payloads go to `<ps5-ip>:9021` as
   a raw ELF byte stream — the documented ps5-payload-dev/elfldr protocol
   (`nc -q0 $PS5_HOST 9021 < payload.elf`). **HTTP PUT does not work.**
   `tools/payload_send.py` speaks the right protocol; anything that opens
   an HTTP client against `:9021` will fail or wedge.
2. **The Vita FTP app does not consume injected touch.** VitaShell/FTPVita
   in the foreground ignores synthetic `SceTouchReport`s, so touch tests
   (touchswipe, touchtest) must run while the Vita shows the **LiveArea or
   a game** — with the FTP app open you will see nothing and wrongly
   conclude injection is broken.

---

## `deploy_app.sh` — dev-loop app deploy

```bash
bash tools/deploy_app.sh [host] [title-id]     # defaults: <ps5-ip> PPSA39410
```

Deploys `dist/<title-id>/` to `/data/homebrew/<title-id>/` over the PS5's
FTP — same flow as `deploy_ftp.sh` (hidden temp name + rename into place,
`eboot.bin` published first and `sce_sys/param.json` **last**, explicit
modes `0755`/`0644` set via `SITE CHMOD` and verified against MLSD, every
file read back and SHA-256-hashed) with one deliberate difference: the
low-level tool can update existing remote files. **Do not use that
capability for the app test loop: always pass a fresh identity from
`build_test.sh`.** Remote-only leftovers are reported and left alone
(never deleted). Deploys only; it never launches anything.

| Env | Default | Meaning |
| --- | --- | --- |
| `PS5_HOST` | `<ps5-ip>` | PS5 host (first argument overrides) |
| `PS5_FTP_PORT` | `2120` | FTP port |
| `PS5_FTP_USER` | `anonymous` | FTP user |
| `PS5_FTP_PASSWORD` | template anonymous value | keep real credentials in the environment, never in the files |

Proven on hardware (2026-10-06): 87-file update of `PPSA39410` over the
real ftpsrv — the server accepts `RNTO` onto an existing entry, so the
update path is a straight rename; the delete-and-rename fallback only
exists for servers that refuse that.

## `app_smoketest.py` — PC-side smoke test

```bash
python3 tools/app_smoketest.py [--title-id PPSA39410] [--host H] [--skip-vita] [--vita-log-lines N]
```

Run while the app is up on the PS5. What it **does** verify:

- `ps5-reachable` — the host resolves and answers TCP;
- `elfldr-tcp` — a listener on `:9021` accepts a connection (the probe
  sends **zero payload bytes**: an ELF stream would execute, and HTTP is
  not elfldr's protocol);
- `ftp-login` / `ftp-root` — FTP login works and `/data/homebrew` lists;
- `local-eboot` — `dist/<title-id>/eboot.bin` exists locally; size +
  SHA-256 reported (also checks `sce_module/libc.prx`,
  `sce_sys/param.json`, and that `param.json` carries the title ID);
- `remote-deploy` — the deployed `/data/homebrew/<title-id>/eboot.bin`
  matches the local build byte for byte (readback + hash, same as
  `deploy_ftp.sh`); "not deployed yet" is reported as a warning;
- `vita-plugin` / `vita-log` (advisory, `--skip-vita` to disable) — over
  the Vita FTP: `input_receiver.skprx` under `*KERNEL` of `config.txt`,
  and the tail of `ur0:/tai/input_receiver.log`. Unreachable Vita is
  `SKIP`, never a failure — the FTP app can only run between input tests.

What it **cannot** do: press buttons. Input effects are proven by the
padtest/touchswipe reference payloads plus the Vita log — the smoke test
only clears the transport and artifacts for that hands-on step.

Exit status: `0` when all required checks pass (WARN/SKIP never fail),
`1` on any required failure. Env overrides: `PS5_HOST`, `PS5_FTP_PORT`,
`PS5_FTP_USER`, `PS5_FTP_PASSWORD`, `PS5_ELFLDR_PORT` (default `9021`),
`VITA_FTP_HOST`, `VITA_FTP_PORT`, `VITA_FTP_USER`, `VITA_FTP_PASSWORD`.

## `payload_send.py` — payloads via elfldr (raw TCP)

```bash
python3 tools/payload_send.py payload/VITA5-touchswipe.elf
python3 tools/payload_send.py --host <ps5-ip> --port 9021 --wait 45 file.elf
```

Streams the ELF to elfldr and keeps the socket open to capture the
payload's console output (elfldr accepts a raw ELF stream or a one-line
`file:`/`https:` URI — see the ps5-payload-dev/elfldr README). Not HTTP.

## `vita_install_input_receiver.py` — Vita plugin install

```bash
VITA_FTP_HOST=<vita-ip> python3 tools/vita_install_input_receiver.py [path/to/input_receiver.skprx]
```

Uploads the plugin to `ur0:tai/` and adds it under `*KERNEL` of
`ur0:tai/config.txt`, with backup and readback verification. Env:
`VITA_FTP_HOST` (required), `VITA_FTP_PORT` (`1337`),
`VITA_FTP_USER`, `VITA_FTP_PASSWORD`.

## Platform footguns (measured on this rig)

- **550 wording on the PS5 ftpsrv**: CWD into a missing path is
  `550 Not a directory.` (the only unambiguous "absent" reply), RETR of a
  missing file is `550 Cannot open file.`, DELE of a missing file is
  `550 Cannot delete file.` The `is_missing` handling in `deploy_ftp.sh` /
  `deploy_app.sh` matches only the first; existence is confirmed by MLSD
  listing, never by error text.
- **MSYS/git-bash + Windows Python**: bash hands `/c/Users/...` paths that
  a Windows `python3` cannot open. `deploy_app.sh` remaps them internally;
  `deploy_ftp.sh` expects a POSIX `python3` (run it from WSL).

---

## `fresh_id.py` — fresh random title ID

```bash
python3 tools/fresh_id.py            # prints one fresh ID, e.g. PPSA04231
python3 tools/fresh_id.py --fail PPSA04231   # exclude a failed ID forever
```

Draws `PPSA` + 5 random digits with Python's `secrets`, excluding
`PPSA99999` and every ID already recorded in `tools/.used_ids`
(reserved or failed). Each generated ID is appended to the ledger
(`PPSA#####  # reserved|failed`) before it is printed, so no title ID is
ever reused across test builds or deploys.

## `build_test.sh` — one-shot fresh-ID test build

```bash
bash tools/build_test.sh
```

1. Restores LF line endings on the runtime digest-gate inputs
   (`runtime/libc.prx.sha256`, `tooling/native/runtime/*.txt`) if any CRLF
   drift is found — the digest gate fails on CRLF. `.gitattributes` already
   pins `*.sha256`/`*.txt` to `eol=lf` and is never modified.
2. Draws a fresh title ID via `fresh_id.py`.
3. Runs the stock identity init
   (`make -C app init TITLE_ID=<fresh> APP_NAME="VITA5 <fresh>"`, which is
   the stock `make init` pointed at `app/sce_sys/param.json` — the file the
   app build consumes; see `app/README.md`).
4. Builds the app folder via `make -C app` into `dist/<fresh>/`.
5. Verifies `titleId`/`conceptId`/`contentId` agree in `param.json`, the
   built `sce_sys/param.json` carries the fresh ID, and `eboot.bin` +
   `sce_module/libc.prx` exist and are non-empty.

Prints `FRESH_TITLE_ID=<id>` and `DIST_PATH=<path>` on success.

## `deploy_ftp.sh` — FTP deploy of a built app folder

```bash
bash tools/deploy_ftp.sh <host> <title-id>
# example:
bash tools/deploy_ftp.sh <ps5-ip> PPSA04231
```

Deploys `dist/<title-id>/` to `/data/homebrew/<title-id>/` on the PS5's
FTP server. It **deploys only** — it never launches anything.

| Env | Default | Meaning |
| --- | --- | --- |
| `PS5_FTP_PORT` | `2120` | FTP port |
| `PS5_FTP_USER` | `anonymous` | FTP user |
| `PS5_FTP_PASSWORD` | template anonymous value | keep real credentials in the environment, never in the files |

Staging/publish rules implemented:

- **Never overwrite**: refuses to stage if *any* files already exist for the
  candidate (`/data/homebrew/<id>` or `/data/homebrew/<id>.*`); fresh IDs
  make this a collision, and a fresh ID + rebuild is the recovery path.
  (`deploy_app.sh` can update in place at the transport layer, but fresh
  app identities are still mandatory for console testing.)
- **Hidden temp + rename**: every file is STORed as
  `.<name>.<random>.upload`, the transfer is completed and byte-checked
  (`SIZE` where supported), then renamed into place — final names only ever
  appear complete.
- **Publish order**: all other files first, then `eboot.bin`, then
  `sce_sys/param.json` **last**.
- **Explicit modes** (FTP creates 0666): `0755` on `eboot.bin` and every
  `*.prx`, `0755` on the title dir and every subdirectory, `0644` on all
  data. Set via `SITE CHMOD` and **verified** against MLSD mode facts;
  verification failure aborts the deploy.
- **No suppressed errors**: only an explicit "no such file/directory" 550 is
  treated as absence; arbitrary 550 errors propagate and abort.
- **Readback**: every file is retrieved and SHA-256-hashed against the
  local copy; remote file count and per-file sizes must match before the
  deploy is declared complete.

## Licence

GPL-3.0-or-later (same as the repository).
