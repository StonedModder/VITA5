# VITA5

**2 consoles, 1 cable. 0 emulation. All real hardware**

VITA5 docks a PlayStation Vita to a PS5. The Vita's picture and sound play on
your TV through the PS5, and your DualSense controller sends input back to the
Vita. One cable does it all — no Bluetooth, no capture card, no PC in the
middle.

---

## Table of contents

- [What it does](#what-it-does)
- [Installation](#installation)
- [How to use it](#how-to-use-it)
- [Controls](#controls)
- [Documentation](#documentation)
- [Building from source](#building-from-source)
- [Status](#status)
- [License & disclaimer](#license--disclaimer)

---

## What it does


https://github.com/user-attachments/assets/6320cb02-66a4-4c69-8712-9765f024f6c2


```
   PS Vita ──── one USB cable ────▶ PS5 ────▶ TV
 (your game)                     (VITA5 app)   (picture + sound)
     ▲                                          │
     └────────── DualSense controller ──────────┘
```

- **Video** — the Vita's screen shows on your TV, up to 60 fps
- **Audio** — the Vita's sound plays through the PS5
- **Input** — your DualSense controls the game; the touchpad maps to the Vita's
  touchscreen
- **One cable** — power, video, audio and input all travel over the same USB
  cable

---

## Installation

> **Before you start:** both devices must already run homebrew. The Vita needs
> a hacked (CFW) system with **VitaUSBStream** installed, and the PS5 needs an
> active jailbreak with a payload loader (elfldr). If those are not set up yet,
> do that first — VITA5 will not work without them.

You will install three things. Every release on the
[Releases page](https://github.com/StonedModder/VITA5/releases) ships all three
as ready-to-use files.

| # | File | Goes on | What it is |
| - | ---- | ------- | ---------- |
| 1 | `input_receiver.skprx` | PS Vita | the plugin that receives controller input |
| 2 | `VITA5-payload.elf` | PS5 | the dock service that reads the Vita's stream |
| 3 | `PPSA*.zip` | PS5 | the VITA5 app you launch |

### Step 1 — Install the Vita plugin

1. Copy `input_receiver.skprx` to `ur0:tai/` on the Vita (use VitaShell).
2. Open `ur0:tai/config.txt` and add this line under `*KERNEL`, **above**
   `VitaUSBStream.skprx`:

   ```
   *KERNEL
   ur0:tai/input_receiver.skprx
   ur0:tai/VitaUSBStream.skprx
   ```

3. Reboot the Vita.

> The order matters. The input plugin must load *before* the stream plugin, or
> the controller will not be picked up.

### Step 2 — Start the dock service on the PS5

1. Make sure your PS5's jailbreak / payload loader is running.
2. Send `VITA5-payload.elf` to the PS5 over the loader (port `9021`). With the
   included helper: `python tools/payload_send.py VITA5-payload.elf`.
3. Leave it running — it stays active in the background.

### Step 3 — Install the app

1. Extract `PPSA*.zip`. You get one folder (the title ID is the folder name).
2. Copy that whole folder to `data/homebrew/` on the PS5 (FTP, usually port 2120).
3. Every file and directory in that folder must be mode **0777**. If the app
   will not start, that is the first thing to fix.
4. Launch it from the PS5 home screen.

That's it. Connect the Vita with a USB cable and the picture appears.

---

## How to use it

1. Connect the PS Vita to the PS5 with one USB cable.
2. Start the dock service ([Step 2](#step-2--start-the-dock-service-on-the-ps5))
   if it is not already running.
3. Launch **VITA5** from the PS5 home screen.
4. Play. The Vita's screen is on your TV and the DualSense controls the game.

To stop, close the VITA5 app on the PS5.

---

## Controls

Everything is driven from the DualSense. The Vita has no stick clicks, so
**L3 is a free modifier** — hold Left Stick click, then press a second button.
A short on-screen toast confirms each hotkey. The same upscale and touch
options also appear under **Settings**.

| Combination | Action |
| ----------- | ------ |
| **L3 + R3** | switch control between the Vita and the app (so you can open settings / full screen while a game is running) |
| **L3 + D-Pad ◀ / ▶** | cycle picture upscaling: **Off → Sharp 2x → FSR 2x → Sharp 3x → FSR 3x → Sharp 4x → FSR 4x → Off** |
| **L3 + D-Pad ▲** | toggle full screen |
| **L3 + D-Pad ▼** | swap DualSense touchpad target: Vita **front touchscreen** ↔ **rear touch pad** |
| **L3 + Touchpad click** | press the Vita **HOME** (PS) button |

While L3 is held, those chord buttons are **not** sent to the Vita — hotkeys
never leak into the game. With L3 released, the DualSense passes through
normally (buttons, sticks, triggers, and touch).

---

## Documentation

Deeper reference material lives in [`docs/`](docs/):

- [Getting started](docs/GETTING_STARTED.md) — toolchain setup and first build
- [Vita setup](docs/VITA_SETUP.md) — preparing the Vita side in detail
- [Streaming](docs/STREAMING.md) — how the USB video/audio stream works
- [Touchpad passthrough](docs/TOUCHPAD_PASSTHROUGH.md) — touchpad → touchscreen mapping
- [Troubleshooting](docs/TROUBLESHOOTING.md) — build and install problems

---

## Building from source

Only needed if you want to modify VITA5. A normal install uses the prebuilt
files from the [Releases page](https://github.com/StonedModder/VITA5/releases).

- **App and payload** — `tools/build_test.sh` (builds a fresh title ID each time)
- **Host tests** — `make -C app test` under WSL/Linux
- Full toolchain notes: [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md)

---

## Status

Video, audio, controller, touch, upscaling, and the L3 hotkeys work.

Known issue: the VitaUSBStream gadget leaks frame memory, so if the stream dies
right after starting, reboot the Vita to clear it (an upstream bug, not
something VITA5 can fix).

---

## License & disclaimer

GPL-3.0-or-later. Derived from
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
(© 2026 BlackBearReloaded). Third-party components keep their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Independent homebrew project, not affiliated with or endorsed by Sony
Interactive Entertainment. "PlayStation", "PS5" and "PS Vita" are trademarks of
Sony Interactive Entertainment Inc. No proprietary Sony material is included.
Use at your own risk; running homebrew requires a modified console.
