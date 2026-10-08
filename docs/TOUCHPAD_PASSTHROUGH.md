# DualSense touchpad → Vita touch passthrough

How to pass the PS5 controller's touchpad through to the PS Vita as touch input
(front touchscreen / rear touchpad). This documents the approach and the exact
injection mechanism, distilled from working Vita plugins, so the step is ready
when we build it. The Vita-side touch-injection mechanism below is **confirmed
from real plugin source** (vitacompanion); the PS5→Vita transport and mapping
are our design.

**Status (2026-10-06):** **front touchscreen injection is CONFIRMED WORKING
on hardware** — scripted swipes from the PS5 drove the Vita's LiveArea pages
(10 up / 10 down / 10 up, visually observed). The transport + Vita-side
injection are documented in
[VITA_INPUT_API.md §10](VITA_INPUT_API.md) (16-byte touch records over the
chunked control channel) and implemented in the `SceTouch*` hooks in
`vita-side/input-receiver/src/input_receiver.c` (verified counters:
271/271 records decoded, 0 errors, 7024 reports injected). What remains is
the DualSense-side app integration (§2/§3), the rear-touchpad confirmation
with a game (§4), and the front/rear overlay toggle (§4/§6).

## Overview

```
 DualSense touchpad ──▶ PS5 app ── USB ──▶ Vita kernel plugin ──▶ SceTouchData ──▶ game
 (2 fingers, x/y)       scePadRead  OUT     (inject SceTouchReport)   sceTouchPeek
```

The DualSense touchpad reports finger contacts + 2D coordinates. The PS5 app
forwards them over USB; a Vita kernel plugin injects them as touch so the game
sees a touchscreen/touchpad press.

## 1. Vita-side injection (the key mechanism)

Touch injection on the Vita is done in a **taiHEN kernel plugin** that hooks the
`SceTouch*` driver exports and injects synthetic touch reports into the data
games read. This is exactly what `vitacompanion` does (its
`kernel/touch_patch.c` is the reference implementation).

### Hook the touch driver

Hook these exports with `taiHookFunctionExportForKernel` and call the original,
then append synthetic reports to the returned `SceTouchData`:

- `sceTouchPeek`, `sceTouchRead`
- `sceTouchPeekRegion`, `sceTouchReadRegion`
- the `_ext` variants (`sceTouchPeekRegionExt`, `sceTouchReadRegionExt`)

Each hook runs `patch_touch_data(port, data, count)` after the real read.

### Inject a synthetic touch report

`SceTouchData` holds `reportNum` (active count) and `report[]`, an array of
`SceTouchReport`. To add a touch, append to `report[]` and bump `reportNum`:

```c
SceTouchReport *r = &data->report[data->reportNum++];
r->id    = 0x70 + slot;   // synthetic touch id base (0x70), one per slot
r->force = 0x80;
r->x     = (int16_t)x;    // raw touch coordinate
r->y     = (int16_t)y;
// r->reserved[8] = 0; r->info = 0;
```

### Facts and limits (from the source)

| Item | Value |
|------|-------|
| Touch ports | `FRONT = 0`, `REAR = 1` |
| Max reports per sample | **front = 6**, **rear = 4** |
| Synthetic id base | `0x70` (+ slot index) |
| `force` for synthetic touch | `0x80` |
| Touch coordinate space | `x: 0–1919`, `y: 0–1087` (raw 1920×1088) |
| Independent touch slots | 4 |
| `SceTouchReport` fields | `id, force, x(i16), y(i16), reserved[8], info` |

### Exposing it to the input layer

vitacompanion exposes a tiny kernel syscall API its user module calls (adapt
this for our USB receive path):

```c
int vitaCompanionKernelSetTouch(int port, int slot_and_active, int x, int y);
// slot_and_active = slot_index | 0x100 (0x100 = active flag)
int vitaCompanionKernelReset(void);
```

The kernel stores the 4 slot points; the touch hook merges them into every
`SceTouchData` the game reads. For VITA5, the USB receive loop sets the slots
from incoming DualSense touch data instead of a TCP command.

## 2. PS5-side source (DualSense touchpad)

Read the controller with `scePadRead`. `ScePadData` carries the touchpad state:
up to **2 fingers**, each with `x`, `y` and a touch/id field
(`ScePadData.touchPad.x[2] / y[2] / finger[2]`). This is the data to forward.

## 3. Transport (PS5 → Vita over the USB cable)

Extend the existing input path: the pad reports already flow PS5→Vita over the
USB bulk OUT endpoint (`0x02`, the 28-byte `pad_passthrough.c` wire format).
Add a touch record alongside the button/stick report, e.g.:

```
touch record:  [ finger_count u8 ][ per finger: id u8, x u16, y u16, active u8 ]
```

Keep it in the same wire-format decoder family as `pad_wire.h` (pure C99,
host-tested).

## 4. Coordinate mapping + target

- **Default target**: the DualSense pad maps to the Vita **rear touchpad**
  (natural fit — both are back/touchpad-style inputs).
- **Toggle**: the player can switch the target to the **front touchscreen** at
  runtime via an in-game overlay setting (see §6). The default is rear.
- **Scale**: normalize DualSense pad coords (0..1 × 0..1) then multiply to the
  Vita space (`x: 0–1919`, `y: 0–1087`). Clamp to range.
- **Fingers**: DualSense supports 2; the Vita accepts more, but map 1:1 (slot 0,
  slot 1). Set `active` only while a finger is down.

## 5. In-game overlay toggle (front/rear)

The touch target must be switchable **while a game is running**, like
[PS5CEMU-HAR](https://github.com/premohq/PS5CEMU-HAR)'s in-game settings panel
(`port/app/side_menu.h` + `port/frontend/settings.h`). Its pattern, to reuse in
our native app's UI (`app/`):

- **SideMenu overlay** — a panel down the **left** of the screen over the
  **dimmed** game (the rest of the game stays in view). Rows are settings,
  actions, or categories (a category opens in place; one open at a time).
- **Controller-driven** — Up/Down move focus, **Left/Right change a setting
  live**, Cross chooses/opens, Circle closes. No mouse.
- **Settings model** — a struct persisted to a JSON file (PS5CEMU-HAR uses
  `/data/ps5cemu/ps5cemu.json`); the touch-target setting applies **immediately**
  (re-maps the touch port/coords on the next report).
- **Rendered via ImGui** (`menu_canvas.h` draws the panel) on a fixed 1920×1080
  layout; help text under the list, controller hints at the bottom.

For VITA5 this is one row: **"Touch target" — Rear touchpad / Front
touchscreen** — changed with Left/Right in the overlay, applied live. It belongs
to the app's settings screen/overlay (the `app/` UI built on the
ps5-homebrew-ui kit).

## 6. References (cloned in `tmp/research/`)

- **[devnoname120/vitacompanion](https://github.com/devnoname120/vitacompanion)**
  — `kernel/touch_patch.c` (the `SceTouchReport` injection),
  `kernel/main.c` (the `sceTouch*` taiHEN hooks),
  `include/vitacompanion_kernel.h` (the `SetTouch` syscall API). The primary
  reference for this whole mechanism.
- **[MERLev/reVita](https://github.com/MERLev/reVita)** — kernel plugin that
  emulates touch **presses and swipes** (useful for gesture/swipe injection),
  plus "swap touchpads" and touch-pointer display. Supersedes remaPSV.
- **[premohq/PS5CEMU-HAR](https://github.com/premohq/PS5CEMU-HAR)** —
  `port/app/side_menu.h` + `port/frontend/settings.h`: the in-game settings
  overlay pattern (left panel over the dimmed game, controller-driven live
  settings, JSON persistence) to reuse for the front/rear touch-target toggle.

## Status

The Vita-side **injection mechanism is confirmed** (real, working plugin code).
What remains to build: the PS5 touchpad read + USB touch transport + the
coordinate mapping + wiring the USB receive loop to the kernel `SetTouch`-style
API. None of this has run on hardware yet.
