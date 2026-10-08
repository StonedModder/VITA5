# VITA5 — Vita-side USB input receiver

Vita-side companion for **VITA5**: the PS Vita is docked to the PS5 over a
single USB cable. The PS5 is the USB **host**, the Vita is the USB
**device/gadget** (`SceUdcd`). Video flows Vita→PS5 through the UVC gadget
(xerpi's [vita-udcd-uvc](https://github.com/xerpi/vita-udcd-uvc): VideoControl
interface 0 + VideoStreaming interface 1, bulk IN `0x81`). This plugin adds the
opposite direction: **controller input PS5→Vita over the same USB cable** —
no Bluetooth anywhere in the input path.

It adds a **third USB interface** (interface number 2, vendor-specific class
`0xFF`) with one **bulk OUT endpoint `0x02`** to the existing UVC
configuration. The PS5 host writes 28-byte pad reports to that endpoint; the
plugin decodes them with `include/pad_wire.h` and injects them into the Vita
input system so games see a controller.

```
        USB (one cable)
PS5 host ================================ PS Vita (SceUdcd gadget)
  |  UVC video   bulk IN  0x81  <--------- VideoStreaming iface 1 (unchanged)
  |  pad reports bulk OUT 0x02  ---------> vendor iface 2 (this plugin)
```

## Contents

| Path | What |
|---|---|
| `include/pad_wire.h` | Exact wire-format decoder API (pure C99, host-tested) |
| `src/pad_wire.c` | Decoder implementation |
| `tests/pad_wire_test.c` | Host round-trip unit test (run with WSL clang-18) |
| `src/input_receiver.c` | taiHEN kernel plugin (skprx): USB descriptors + receive loop + SceCtrl injection |
| `include/input_receiver.h` | `input_receiver_init()/term()` (integrated-mode API) |
| `Makefile`, `input_receiver.yml` | vitasdk build for the skprx |
| `integration/vita-udcd-uvc-input.patch` | Verified patch extending vita-udcd-uvc itself (recommended mode) |

## Pad wire format (28 bytes, little-endian)

| offset | type | field |
|---|---|---|
| 0 | u32 | `report_id` |
| 4 | u64 | `timestamp_us` |
| 12 | u32 | `buttons` |
| 16 | i16 | `left_x` |
| 18 | i16 | `left_y` |
| 20 | i16 | `right_x` |
| 22 | i16 | `right_y` |
| 24 | u8 | `l2` |
| 25 | u8 | `r2` |
| 26 | u16 | `reserved` (0) |

Sticks are full-range signed (−32768..32767, 0 = centered); `l2`/`r2` are
0..255. Button bits: `SELECT=1<<0, L3=1<<1, R3=1<<2, START=1<<3, UP=1<<4,
RIGHT=1<<5, DOWN=1<<6, LEFT=1<<7, L2=1<<8, R2=1<<9, L1=1<<10, R1=1<<11,
TRIANGLE=1<<12, CIRCLE=1<<13, CROSS=1<<14, SQUARE=1<<15, HOME=1<<16,
TOUCH=1<<17`.

The bit layout for bits 0..16 is **numerically identical** to `SceCtrlButtons`
(asserted at compile time in `src/input_receiver.c`), so the mapping is a mask:
`ctrl = wire & 0x1FFFF`. `TOUCH` (bit 17) has no `SceCtrlButtons` equivalent and
is dropped. Analog triggers `l2`/`r2` above `PAD_WIRE_TRIGGER_THRESHOLD` (32)
additionally force the digital `SCE_CTRL_LTRIGGER`/`RTRIGGER` bits. Stick
conversion is `(v + 32768) >> 8` (0..255, `0x80` centered).

`pad_wire_decode()` rejects any length other than exactly 28 bytes.

## USB integration — the exact SceUdcd additions

A USB gadget has exactly **one** configuration descriptor, owned by **one**
registered `SceUdcdDriver` (the UVC plugin's). A separately registered second
driver cannot append interfaces to it, so interface 2 must live in the same
configuration/endpoint tables as the video interfaces. There are two ways to
get there; both keep the video path byte-identical.

### Mode A (recommended): extend the UVC gadget itself

`integration/vita-udcd-uvc-input.patch` (verified: `git apply --check` passes
against a pristine xerpi/vita-udcd-uvc checkout) makes exactly these additions
to `include/usb_descriptors.h` / `src/main.c` / `Makefile`:

* `struct SceUdcdEndpoint endpoints[2]` → `[3]`, adding
  `{USB_ENDPOINT_OUT, 2, 0, 0}` — the pad bulk OUT endpoint. SceUdcd fills in
  `endpointNumber` like it does for EP0 and video IN `0x81`.
* `endpdesc_hi[2]` → `[3]` and `endpdesc_full[2]` → `[3]`: one
  `SceUdcdEndpointDescriptor` for `bEndpointAddress = USB_ENDPOINT_OUT | 0x02`,
  `bmAttributes = USB_ENDPOINT_TYPE_BULK`, `wMaxPacketSize` 0x200 (hi-speed) /
  0x40 (full-speed), `bInterval = 0`.
* `interdesc_hi[3]` → `[4]` and `interdesc_full[3]` → `[4]`: one
  `SceUdcdInterfaceDescriptor` with `bInterfaceNumber = 2`,
  `bAlternateSetting = 0`, `bNumEndpoints = 1`, `bInterfaceClass =
  USB_CLASS_VENDOR_SPEC (0xFF)`, subclass `0x50`, protocol `0x01`,
  `endpoints = &endpdesc_hi[1]` (resp. `&endpdesc_full[1]`).
* `settings_hi/full[2]` → `[3]`: `{&interdesc_hi[2], 0, 1}` entry.
* `SceUdcdConfigDescriptor` (hi + full): `wTotalLength` grows by exactly
  `USB_DT_INTERFACE_SIZE + USB_DT_ENDPOINT_SIZE = 9 + 7 = 16`
  (`2 * USB_DT_INTERFACE_SIZE + 1 * USB_DT_ENDPOINT_SIZE` becomes
  `3 * USB_DT_INTERFACE_SIZE + 2 * USB_DT_ENDPOINT_SIZE`), `bNumInterfaces`
  2 → 3.
* `SceUdcdDriver.numEndpoints` 2 → 3.
* `uvc_start()` calls `input_receiver_init(&endpoints[2])` after
  `ksceUdcdActivate()` (failure only logs — video keeps working);
  `uvc_stop()` calls `input_receiver_term()` first.

The 8-byte Interface Association Descriptor still covers only the two video
interfaces (`bInterfaceCount = 2`) — correct USB semantics: interface 2 is a
separate vendor function in the same configuration. The UVC class-request
handling (`uvc_udcd_process_request`) never sees interface 2 traffic because
that interface defines no class requests; the video EP `0x81` transfers are
untouched.

To use Mode A: copy `src/input_receiver.c`, `src/pad_wire.c`,
`include/pad_wire.h`, `include/input_receiver.h` into your vita-udcd-uvc
checkout and apply the patch:

```
cp src/input_receiver.c src/pad_wire.c   <vita-udcd-uvc>/src/
cp include/pad_wire.h include/input_receiver.h <vita-udcd-uvc>/include/
cd <vita-udcd-uvc> && git apply /path/to/vita-udcd-uvc-input.patch && make
```

### Mode B (standalone skprx, experimental)

`make` in this directory builds `input_receiver.skprx`, a self-contained
taiHEN kernel plugin that never registers its own `SceUdcdDriver` (that would
take over the USB configuration and kill video). Instead it hooks
`ksceUdcdRegister` (SceUdcdForDriver NID `0x4E55244D`) and, when the UVC
gadget driver (`"VITAUVC00"`) registers, replaces `driver->configuration_hi`,
`driver->configuration` and `driver->interface` with **superset copies**
built at runtime: the existing VideoControl/VideoStreaming
`SceUdcdInterfaceDescriptor` entries are copied by value (their `extra` and
`endpoints` pointers keep referencing the UVC plugin's class descriptors and
endpoint descriptors, which stay in place), a third interface entry is
appended, and `wTotalLength`/`bNumInterfaces` are bumped as in Mode A. The
UVC plugin's own `endpoints[]` array is left untouched, so its video
`ksceUdcdReqSend(&endpoints[1], …)` calls keep working.

The one structural difference from Mode A: the UVC driver's `endpoints[]`
array has exactly 2 slots (EP0 + video IN) and cannot be extended from
outside, so the pad `SceUdcdEndpoint` is a plugin-local struct with
`endpointNumber` **pre-filled** to 2 (matching the descriptor) instead of
being filled by SceUdcd. See "Unverified" below.

Load order matters (taiHEN loads kplugins in `config.txt` order):
`input_receiver.skprx` must appear **above** `udcd_uvc.skprx` so the hook is
installed before the UVC plugin registers.

## Receive pipeline (v1.9: chunked control channel)

The report rides **seven SETUP-only vendor control OUTs** on EP0
(`bmRequestType 0x40`, `bRequest 0x50..0x56` = chunk index, 4 bytes per
request in `wValue`/`wIndex`, `wLength` 0). The plugin wraps the UVC
gadget's `processRequest` (installed when the gadget registers) and
reassembles the 28 bytes from the chunks; chunk `0` resets the reassembly
state so torn transfers never mix two reports. The complete report goes
into a single-slot mailbox and a worker thread decodes (`pad_wire_decode`)
and injects. If no report arrives for 500 ms, the injection is explicitly
reset so no button can stay stuck (the button emulation also self-expires,
see below).

This shape is measured, not chosen: **EP0 OUT data stages do not deliver on
this firmware at all** (`payload/eptest.c` proves it with the gadget's own
UVC `SET_CUR` → `GET_CUR` round trip), while SETUP-only transfers reach
`processRequest` reliably. See
[docs/VITA_INPUT_API.md](../../docs/VITA_INPUT_API.md) for the full wire
protocol, button/stick/trigger encoding and macro-authoring guide.

## Input-injection API (and its evidence)

**Chosen: `ksceCtrlSetButtonEmulation` / `ksceCtrlSetAnalogEmulation`**
(`psp2kern/ctrl.h`, link `-lSceCtrlForDriver_stub`).

Evidence:

* vitasdk
  [`psp2kern/ctrl.h`](https://github.com/vitasdk/vita-headers/blob/master/include/psp2kern/ctrl.h)
  documents them as kernel exports for exactly this purpose: "Emulate buttons
  for the digital pad" (`port`, `slot` 0–3, `userButtons`, `kernelButtons`,
  `uiMake` duration in sampling counts) and "Emulate values for the analog
  pad's X- and Y-axis" (0..0xFF per axis). NIDs (vita-headers
  `db/360/SceCtrl.yml`): `ksceCtrlSetButtonEmulation = 0x1E750326`,
  `ksceCtrlSetAnalogEmulation = 0x06577FE8`.
* **xerpi's [ds4vita](https://github.com/xerpi/ds4vita)** — a shipped kernel
  plugin feeding an external DualShock 4 into the Vita — injects with the
  exact call pattern this plugin uses (ds4vita `main.c:196–261`):

  ```c
  ksceCtrlSetButtonEmulation(0, 0, buttons, buttons, 32);
  ksceCtrlSetAnalogEmulation(0, 0, lx, ly, rx, ry, lx, ly, rx, ry, 1);
  /* reset: */
  ksceCtrlSetButtonEmulation(0, 0, 0, 0, 32);
  ksceCtrlSetAnalogEmulation(0, 0, 0x80, 0x80, 0x80, 0x80,
                             0x80, 0x80, 0x80, 0x80, 0);
  ```

  ds4vita maps the PS button to `SCE_CTRL_INTERCEPTED` (= `0x10000` = our
  `HOME`) and passes the same button word as both `userButtons` and
  `kernelButtons`; this plugin does the same. ds4vita also calls
  `ksceKernelPowerTick(0)` while input is active — copied here so the Vita
  does not sleep mid-game.

`uiMake = 32` sampling counts (~0.5 s at 60 Hz sampling) is a natural
stuck-input failsafe: every report refreshes the emulation, and if reports
stop it expires on its own (plus the explicit 500 ms reset).

**Fallback (documented, not implemented): the MiniVitaTV hook approach.**
If the emulation APIs turn out to be insufficient on some firmware (e.g. for
kernel-mode input consumers, or HOME-button quirks — the header notes
`userButtons` "cannot emulate kernel buttons" and only applies to user-mode
applications, while `kernelButtons` only applies to kernel-mode ones), the
alternative used by
[MiniVitaTV](https://github.com/TheOfficialFloW/MiniVitaTV) (`kernel.c`) is a
taiHEN offset hook on SceCtrl's internal input setter
(`taiHookFunctionOffsetForKernel(..., SceCtrl modid, seg 0, offset 0x107C, …)`,
with the `SceCtrlDataInternal` layout `{timeStamp, buttons, lx, ly, rx, ry,
lx_wide, …}` and SceCtrl module-data offsets such as the input buffer at
`data + 0xA84`). Those offsets are firmware-specific and were not ported here;
they are the documented plan B.

## Building

### Host-tested decoder (verified)

The decoder is pure C99 and tested on this machine with clang-18 in WSL
(not the broken devkitPro gcc in git-bash):

```
cd vita-side/input-receiver
clang-18 -std=c99 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -Iinclude \
  -o /tmp/pad_wire_test tests/pad_wire_test.c src/pad_wire.c && /tmp/pad_wire_test
```

Real output:

```
pad_wire_test: ALL TESTS PASSED (100098 checks)
```

(100 098 checks: golden-byte vector, independent encode→decode round-trip
over 100 000 deterministic pseudo-random reports, per-bit button mapping,
stick conversion endpoints, trigger threshold boundaries, length/NULL
rejection. `make host-test` runs the same test without sanitizers.)

The kernel plugin was additionally **syntax/type-checked** against the real
SDK headers (`vitasdk/vita-headers` master + `taihen.h`) with clang-18
`-fsyntax-only -Wall -Wextra`, clean in both build modes. That validates
struct field names, function signatures and the SceCtrl mapping asserts — it
is not a build.

### Kernel plugin (built with VitaSDK)

```
make            # standalone Mode B: input_receiver.skprx
make host-test  # host build + run of the wire decoder tests
```

Requires vitasdk (`arm-vita-eabi-gcc`, `vita-elf-create`, `vita-make-fself`).
Built and hardware-tested in WSL with `arm-vita-eabi-gcc` 15.x; the Makefile
forces `-std=gnu17` because `taihen.h`'s `TAI_CONTINUE` casts through
unprototyped function pointers (a C23 error). Mode A is built by the patched
vita-udcd-uvc Makefile (`make` in that tree, with the four files copied in).

The taiHEN kernel stubs (`libtaihenForKernel_stub.a`) are generated from
taiHEN's real `exports.yml` with the official pipeline
(`vita-elf-export kernel …` + `vita-libs-gen`); the NIDs were verified
byte-for-byte against the export table `vita-elf-create` builds from that
same yml.

## Firmware caveats

* The emulation NIDs above are from the 3.60 firmware database; ds4vita runs
  on 3.60–3.68-era CFW. Newer homebrew enso/firmware stacks should be
  checked for the same exports.
* Mode B relies on vita-udcd-uvc's own firmware-specific hook
  (`SceUdcd` internal config-builder at module offset `0x01E1128C - 0x01E10000`)
  to insert the Interface Association Descriptor. That is a pre-existing
  vita-udcd-uvc caveat (it targets xerpi's tested firmware); Mode A inherits
  it, Mode B inherits it, and if it fails UVC video binding may fail on that
  firmware regardless of this plugin.
* `HOME` maps to `SCE_CTRL_PSBUTTON` = `SCE_CTRL_INTERCEPTED` (`0x10000`); the
  ctrl header warns values above `0x10000` are interceptable only with shell
  privileges. Emulation of HOME therefore depends on firmware behavior.
* `TOUCH` (wire bit 17) is intentionally not injected (no SceCtrl concept).

## Verified on hardware (2026-10-05)

Live-tested against the real target: PS Vita running **VitaUSBStream**
(UVC+UAC stream gadget) docked to a PS5 (USB host), plugin loaded via taiHEN
from `ur0:tai/config.txt` (`*KERNEL`, listed **above** `VitaUSBStream.skprx`
— that order is required so the hook is installed before the gadget
registers).

**Gadget layout (from the live descriptor probe and the plugin's own log):**

* The stream gadget's driver name really is **`VITAUVC00`** (VitaUSBStream is
  a fork of xerpi's vita-udcd-uvc but kept the name); it registers at **stream
  start** (VUS Control), not at boot.
* 5 interface descriptors (hi- and full-speed identical):
  `0` VideoControl (0x0E/0x01), `1` VideoStreaming (0x0E/0x02, bulk IN
  `0x81`), `2` AudioControl (0x01/0x01), `3` alt 0 AudioStreaming
  zero-bandwidth, `3` alt 1 AudioStreaming (isoc IN **`0x83`**, 192 B
  maxpkt).
* Endpoint numbers in use: **0** (EP0), **1** (video 0x81), **3** (audio
  0x83). The pad endpoint must avoid all of them → **EP2 OUT (`0x02`)** is
  the free slot.
* The plugin appends the pad interface as **interface 4** (next free number =
  `max(bInterfaceNumber)+1`, *not* the descriptor count — interface 3 has two
  alt settings).
* PS5-side: the composite device enumerates cleanly with the appended
  interface (`uaudio0` records 48 kHz/2ch; UVC unaffected).

**Diagnostics that work on hardware:**

* The Vita's kernel log is **not** remotely reachable (the PS5's klog is a
  different machine). The plugin appends breadcrumbs to
  `ur0:/tai/input_receiver.log` (open-append-close per event; read it over
  VitaShell FTP).
* `ksceDebugPrintf` called from the `ksceUdcdRegister` hook correlated with a
  kernel panic at stream start (v1.4) — **never kprintf from that hook**.
* `tools/klog_capture.py` records the PS5-side klog (USB attach/detach
  events) to `tools/klog_capture.txt` for host-side correlation.

**Version history (each fix informed by the hardware feedback loop):**

* **v1.0** — config corruption: the superset builder capped the table at 2
  interfaces and hardcoded interface 2. VitaUSBStream has 4 interfaces + an
  audio alt setting, so the audio interfaces were silently dropped and the
  numbering collided → the whole device failed to enumerate.
* **v1.1** — full copy + next-free interface number. Still panicked at stream
  start: the receive request was armed at boot and bound to a live endpoint
  at stream start.
* **v1.2** — RX lifecycle gated on the gadget being configured (the driver's
  `attach`/`detach` callbacks are wrapped; arm on attach, cancel on detach).
  Endpoint moved to EP3 OUT — bad call, that shares number 3 with the audio
  isoc IN (0x83).
* **v1.3** — gadget matched by its Video-class descriptors (fork-proof),
  endpoint back to 0x02, interface number = max+1. But its settings table
  copy (reading the gadget's own `settings[]`, whose length is unknown)
  correlated with the appended interface never reaching the wire.
* **v1.4** — kprintf breadcrumbs; panicked at stream start (see above).
* **v1.5** — **observe-only** diagnostics (no patch, no RX): proved the
  plugin/hook are healthy, captured the real gadget layout, and exonerated
  everything except the patch path.
* **v1.6** — current: patch rebuilt from the observed layout with
  **synthesized** xerpi-style settings entries (`{&iface, its own
  bAlternateSetting, 1}`), never reading the gadget's settings table.
* **v1.7–v1.8** — the descriptor patch never reaches the wire (SceUdcd
  serves the gadget's original tables) and EP0 OUT *data stages* deliver
  zeroed buffers; byte-level logging + `payload/eptest.c` (UVC probe
  round-trip) proved data stages are dead firmware-wide.
* **v1.9** — **WORKING**: chunked SETUP-only transport (7 control OUTs,
  4 bytes per request in `wValue`/`wIndex`). Verified end to end on
  hardware: 163/163 reports delivered, received bytes mirror the sender's
  exactly, `ksceCtrlSetButtonEmulation` accepts them — **the Vita visibly
  presses the scripted buttons**.
* **v2.0** — **touch injection WORKING**: `SceTouch*` hooks (vitacompanion's
  proven NIDs) append synthetic reports (ids `0x70/0x71`) to every sample
  the game/shell reads; touch records ride their own chunk family
  (`0x58..0x5B`, 4 chunks). Verified on hardware 2026-10-06: scripted
  swipes from the PS5 drove the LiveArea pages — 271/271 records decoded,
  0 errors, 7024 reports injected. Live counters log to
  `ur0:/tai/input_receiver.log`. (Pitfall: `ir_log()` drops silently in the
  USB hook context — telemetry must log from the touch-read hook.)

## Still open (be honest)

1. **PS5 pad forwarding integration**: `payload/pad_forwarder.c` sends via
   the chunked control channel and the transport + injection are proven
   end to end (`payload/padtest.c`, 2026-10-05), but the full
   app → ring → payload → USB path (live DualSense → Vita) has not run.

## References

- **[isage/vixen](https://github.com/isage/vixen)** — *ViXEn, Vita X-input
  Enabler*: a PS Vita kernel driver (`.skprx`) that injects USB gamepad input
  (Xbox/x-input, Logitech, DS3-alike via `vixen_ds3.skprx`) as controller input
  on the Vita. Use it as the reference for the **input-injection** half of this
  plugin — it already does USB-gamepad → Vita-controller injection in a taiHEN
  kernel module, so the injection mechanism (and DS3-style presentation) can be
  adapted rather than built from scratch. Our added value is the **transport**:
 pad reports arrive over the same USB cable from the PS5 (bulk OUT `0x02`)
 instead of from a pad plugged into the Vita.
 - **Touch injection** (for DualSense touchpad → Vita touch passthrough):
 - **[devnoname120/vitacompanion](https://github.com/devnoname120/vitacompanion)**
   — its kernel module injects **front-touch and rear-touch** with 4
   independent slots in the Vita's raw `1920×1088` touch space
   (`press front-touch <slot> <x> <y>`). The cleanest reference for touch
   injection.
 - **[reVita](https://www.gamebrew.org/wiki/ReVita)** — kernel plugin that
   emulates **touch presses and swipes** (supersedes remaPSV2); also "swap
   touchpads" and native/emulated touch pointer display.
 - `vixen` itself does **not** handle touch (it is x-input gamepads only).

 Design notes: map the DualSense touchpad to the Vita **rear touchpad**
 (natural fit) or **front touchscreen**; scale/clamp DualSense pad coords to
 the Vita's 1920×1088 touch space (different aspect).

## License notes

Structure and USB patterns follow xerpi's `vita-udcd-uvc` and injection
patterns follow xerpi's `ds4vita`; see those projects for their licenses
before redistribution.
