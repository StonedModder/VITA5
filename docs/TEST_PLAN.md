# VITA5 — End-to-end hardware test plan

A gated checklist for the owner: build the app with a fresh title ID, deploy it
to the PS5 over FTP, and watch for five observable results (build,
registration, video, audio, input) plus the display check. **Do not merge
gates** — each gate has its own pass criteria, and a later gate proves nothing
about an earlier one. Record the result of each gate (pass / fail / N/A) with
the firmware, loader, and artifact digest, per
[TESTING.md](TESTING.md).

## Test-cycle entry points

| Step | Entry point | Verified equivalent (committed tools) |
|------|-------------|---------------------------------------|
| Fresh-title-ID build | `tools/build_test.sh` | `make -C app init TITLE_ID=PPSA##### APP_NAME="VITA5"` then `make -C app` |
| FTP deploy to `<ps5-ip>:2120` | `tools/deploy_ftp.sh` | `make deploy PS5_HOST=<ps5-ip> FTP_PORT=2120` (or `PS5_HOST=… FTP_PORT=… tools/deploy.sh`) |

> The `tools/build_test.sh` / `tools/deploy_ftp.sh` wrappers are the test
> cycle's named entry points; at the time of writing they are not in the tree,
> so every gate below also carries the equivalent committed command. The
> checklist runs either way.

## Before you start

| Requirement | Detail |
|-------------|--------|
| PS5 | Homebrew environment with elfldr on `<ps5-ip>` port **9021**, FTP on port **2120**, klog on 3232 (confirmed probe endpoints). Payloads reach elfldr over **raw TCP** on 9021 (the `nc -q0 $PS5_HOST 9021 < payload.elf` protocol / `tools/payload_send.py`) — **not HTTP**. Keep loader/FTP on a trusted LAN. |
| Vita | Fully set up per [VITA_SETUP.md](VITA_SETUP.md): stream gadget + input receiver installed under `ur0:tai`, `config.txt` updated, Vita rebooted. |
| Host | Linux/WSL build environment per [GETTING_STARTED.md](GETTING_STARTED.md); run `make doctor` first. |
| Cable | One USB cable, Vita → PS5 USB port. |
| Coordination | Take the shared console lock only for the test window ([TESTING.md](TESTING.md) milestone procedure). |

**Vita state during the whole run:** screen on, a game or app running (so
input injection has a consumer), Vita awake. For the **touch cases** the
foreground consumer must be the LiveArea or a game — the Vita FTP app
(VitaShell/FTPVita) does not consume injected touch. The plugins are silent
kernel modules — if the Vita boots normally after the `config.txt` edit, that
part is already proven.

---

## Gate 1 — Build (host only, no console)

1. Set a **fresh** application identity (title IDs are never reused):
   ```bash
   make -C app init TITLE_ID=PPSA##### APP_NAME="VITA5"   # pick a new PPSA + 5 digits
   ```
2. Build:
   ```bash
   tools/build_test.sh        # test-cycle wrapper
   # equivalent:
   make -C app
   ```
3. Optional host confidence before touching hardware: `make -C app test`
   (ring protocol, NV12 conversion, audio ordering, pad wire format).

**Pass:** the build finishes with `Build complete. App folder: dist/<TITLE_ID>`
containing `eboot.bin` and `sce_sys/param.json`; the converter self-check
reports `container: signed, plaintext … integrity: valid`. Record the title ID
and the artifact digest.

**Fail:** fix on the host; nothing has touched the console yet.

## Gate 2 — Registration (deploy + launch)

1. Deploy the folder over FTP (the app must **not** be running on the PS5
   during upload):
   ```bash
   tools/deploy_ftp.sh        # test-cycle wrapper, targets <ps5-ip>:2120
   # equivalent:
   make deploy PS5_HOST=<ps5-ip> FTP_PORT=2120
   ```
   The deploy publishes `/data/homebrew/<TITLE_ID>/` atomically
   (temp-name + rename), `eboot.bin` and `param.json` last, and verifies both
   appear on the remote side.
2. On the PS5: launch the title from the home screen via the loader.
3. On the Vita: plug the USB cable into the PS5 (do this before or during
   launch — order does not matter; the payload re-detects).

**Pass (owner observation):**

- The title appears on the PS5 home screen with its icon/name and launches.
- The app reaches its normal screens (splash → connect). With the dock service
  absent it shows the designed "waiting for the dock service" states — that is
  a **pass** for this gate (it proves the app runs and fails cleanly).

**Fail:** re-check the FTP target/port, that the app was closed during deploy,
and that the title ID in `param.json` matches what you deployed.

## Gate 3 — Rendering (video appears on the TV)

Vita: plugged in, screen on, showing something moving (a game or the LiveArea).

1. Ensure the dock service (root payload) is running — it is what opens
   `/dev/ugen2.2`, runs the UVC probe/commit handshake and publishes
   `/dev/vdshm`. If the app still shows "waiting for the dock service", the
   payload side is missing or `/dev/vdshm` was not created (see
   [INTEGRATION.md](INTEGRATION.md) §3).
2. Watch the app's connect screen: "Starting the video stream" → "Stream
   established", then the live view.

**Pass (owner observation):**

- The **Vita's screen appears on the TV** at 960×544 content, scaled to the
  output, in motion (not a still, not a colour bar, not invented graphics).
- The HUD's fps/drop readouts move; the picture survives at least a minute of
  continuous play.

Vita check at the same time: the Vita itself keeps running normally — its own
screen is unchanged, no app crashes.

**Fail pointers:** `VD_ERR_PROBE_FAILED`/`COMMIT_FAILED`/`BULK_READ_FAILED`
in the header health fields; a device that is not at `/dev/ugen2.2` with
VID `054c` / PID `1338`; frame reads that stall usually mean full-frame
instead of chunked bulk reads ([STREAMING.md](STREAMING.md) §2).

## Gate 4 — Audio (sound plays)

Vita: same as Gate 3, with system or game audio actually playing (VitaUSBStream
mixes system + game audio).

1. Watch the app's audio meter on the live view (HUD).
2. Listen on the TV.

**Pass (owner observation):**

- **TV speakers play the Vita's audio**, in sync-ish with the picture, no
  continuous buzzing/stuttering, and the meter moves.
- Toggling the `stream audio` setting off/on in the app's settings changes
  what you hear on the next connection (settings apply on next connection).

**Fail pointers:** audio needs the isoc endpoint with alt 1 selected **before**
`USB_FS_INIT` (the historical failure was every transfer `EINVAL`; see
[INTEGRATION.md](INTEGRATION.md) §6). The confirmed healthy capture rate is
~1000 packets/s (192 B per 1 ms USB frame) at 48 kHz stereo int16.

## Gate 5 — Input (the controller drives the Vita)

Vita: a game or app open that responds to buttons and sticks. This gate proves
the whole return path: `scePadRead` → `pad_bridge` → pad ring → payload → 7
SETUP-only control OUTs (`bRequest 0x50..0x56`) → input receiver → `SceCtrl`
— and the touch path (DualSense touchpad → `touch_bridge` → 16-byte record →
4 control OUTs (`0x58..0x5B`) → the plugin's `SceTouch*` hooks). See
[INTEGRATION.md](INTEGRATION.md) §8 for the chains.

1. Pick up the **PS5 controller** (connected to the PS5 as usual — no
   Bluetooth pairing with the Vita anywhere).
2. Press D-pad/buttons, move both sticks, pull L2/R2.

**Pass (owner observation):**

- The **Vita reacts to the PS5 controller**: menus navigate, the character
  moves, triggers fire. Sticks track in all directions; no button stays held
  after release.
- Let go of everything for ~1 s: nothing keeps repeating on the Vita (the
  receiver's 500 ms reset and ~0.5 s emulation expiry must clear held state).
- HUD input-report readout advances while you play.

**Fail pointers:** if video works but input does not, suspect the Vita side
first — wrong `config.txt` load order (input receiver must be **above** the
stream gadget in Mode B), or the stream not enabled (the channel exists only
while the stream gadget is registered). The transport is **SETUP-only chunked
control OUTs** — an EP0 OUT data stage delivers nothing on this firmware
(proof `payload/eptest.c`), so a sender using one is silently dead.

### Gate 5 input cases (record each separately)

Each case has a scripted sender (committed, hardware-proven) so the Vita side
can be isolated from the app side. Build with `make -C payload
VITA5-<name>.elf` (WSL), then send to elfldr over **raw TCP** on 9021 —
`tools/payload_send.py --host <ps5-ip> --port 9021 --wait 45 file.elf` or
`nc -q0 $PS5_HOST 9021 < file.elf` (not HTTP). Once the build under test
ships the native input integration ([INTEGRATION.md](INTEGRATION.md) §8),
re-run each case **live** from the DualSense (pad bridge / touch bridge, or
the in-app test harness) — same expected results.

**5.1 — Button / stick / trigger tour** (scripted: `payload/padtest.c`)

- Do: run the scripted sequence — neutral → D-pad (up, down, left, right) →
  face buttons (CROSS, CIRCLE, SQUARE, TRIANGLE) → shoulders/triggers (L1,
  R1, L2, R2) → START/SELECT → stick sweeps (LX, LY, RX, RY) → neutral. Each
  step is held ~600 ms at 10 Hz (above the receiver's 500 ms reset and its
  ~0.5 s emulation window). Live equivalent: press each in turn.
- Expected: **every listed button visibly presses.** On the LiveArea the
  D-pad moves the selection, CROSS activates the page/bubble, CIRCLE backs
  out; in a game each face button and L1/R1/L2/R2 act, and both sticks sweep
  in all four directions (analog L2/R2 also set the digital trigger bits past
  32). After the neutral report nothing stays held. HOME is deliberately not
  in the tour (firmware-dependent emulation, would kick the user out).
- Reference result: 163/163 scripted reports delivered and injected with
  visible button presses (2026-10-05).

**5.2 — Front touch: LiveArea swipe pattern** (scripted:
`payload/touchswipe.c`)

- Do: swipe **UP 10 times, pause, DOWN 10 times, pause, UP 10 times** (a
  swipe is a fast flick along x=960, y 880→180 or 180→880 in 8 steps of
  30 ms, 400 ms between swipes). Live equivalent: the same pattern on the
  DualSense touchpad.
- Expected: the LiveArea visibly pages with each flick, and ends **net 10
  swipes up** from where it started — a different spot than the start.
- Reference result: visible page swipes observed on 2026-10-06 (see the
  counters case below).

**5.3 — Front touch: single tap** (scripted: `payload/touchtest.c` phase 1)

- Do: one front-port tap at (960, 544) on the LiveArea.
- Expected: the **LiveArea page/bubble under the finger activates** (page
  selection or bubble open) — the tap lands as a real touch. Records replace
  state; end with `count=0`, and nothing stays "stuck down" afterwards.

**5.4 — Front touch: two-finger tap** (scripted: `payload/touchtest.c`
phase 3)

- Do: two front fingers down together at (600, 544) + (1300, 544), then
  release.
- Expected: both fingers land as **one simultaneous two-finger sample**
  (injection slots 0/1). The LiveArea has no standard two-finger gesture, so
  the pass here is counter-based (below) plus no stuck touch and no crash; in
  a game that reads two fingers, both touches register.

**5.5 — Rear touchpad (port 1)** — needs a **game that reads the rear pad**

- Do: open a game with rear-pad controls; run a rear swipe/tap (scripted:
  `payload/touchtest.c` phase 4, rear swipe x 200→1700 at y 544; live: rear
  half of the DualSense touchpad).
- Expected: the **game responds to the rear swipe/tap**, and the counters
  (below) advance with `bad=0`. The LiveArea does not consume rear touch and
  the FTP app consumes no injected touch at all — a LiveArea-only run proves
  nothing for this case.
- Status note: rear touch (port 1) is implemented but **pending a game
  test** — this is the one input case still open.

### Reading the touch counters

Every touch case is verified twice: on screen (expected results above) and in
the plugin's counters at `ur0:/tai/input_receiver.log` (read over VitaShell
FTP). The plugin dumps the line
`touch counters: chunks=… ok=… bad=… injected=… reads=…` every 64 touch
reads, from the touch-read hook (log writes drop silently in the USB hook
context — that is why the dump lives there).
**Pass:** `chunks` = 4 × records sent, `ok` = records sent, `bad` = 0, and
`injected` climbs (reports appended to the game/shell reads). The verified
2026-10-06 run ended at **1084 chunks = 271 records, 0 bad, 7024 injected**.

## Gate 6 — Display / fullscreen toggle

While the stream is live:

1. Exercise the live view's presentation control on the build under test
   (fullscreen toggle / display-mode change), and the `resolution` setting in
   Settings (1080p / 1440p / 4K; display modes apply on the next connection on
   consoles that support them).

**Pass (owner observation):**

- The picture re-scales correctly for each presentation (no stretched aspect,
  no black screen), and **the stream keeps running** across the toggle — video
  continues, audio continues, input continues.
- Returning to the previous presentation restores it exactly.

> Status note: the committed live view fits the video to the output with a
> HUD and reaches Settings with Options (`app/src/screen_live.cpp`); it has no
> in-stream fullscreen toggle at the time of writing. Run this gate against
> whichever presentation control the build under test ships, and mark it
> **N/A** if the build has none.

---

## Result record (copy per run)

```
date / operator:
firmware (PS5 / Vita):            loader / elfldr:
commit + artifact digest:         title ID:
Gate 1 build:                     pass / fail
Gate 2 registration:              pass / fail
Gate 3 rendering:                 pass / fail
Gate 4 audio:                     pass / fail
Gate 5 input:                     pass / fail
  5.1 button/stick/trigger tour:  pass / fail
  5.2 LiveArea swipes (net 10 up): pass / fail
  5.3 front tap:                  pass / fail
  5.4 two-finger tap:             pass / fail
  5.5 rear touchpad (game):       pass / fail / N/A
Gate 6 fullscreen toggle:         pass / fail / N/A
notes:
```

## Cleanup

Close the app on the PS5 before any re-deploy. To remove the staged title
exactly: `make undeploy PS5_HOST=<ps5-ip> FTP_PORT=2120`. To stop the
Vita side, remove the plugin lines from `ur0:tai/config.txt` and reboot the
Vita ([VITA_SETUP.md](VITA_SETUP.md)).
