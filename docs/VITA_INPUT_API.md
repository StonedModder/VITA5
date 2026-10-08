# VITA5 — Vita Input API: buttons, sticks & triggers

**How to make a docked PS Vita press buttons** — the complete wire protocol,
field by field, so you can write your own sender (a macro script, a turbo
tool, a game-specific automation, anything) against the
`vita-side/input-receiver` plugin.

The Vita is the USB **device**, the sender is the USB **host** (in the VITA5
rig that's the PS5 — a root payload or the native app — but the protocol is
plain USB, so any host works). Hardware-proven end to end on 2026-10-05:
163/163 scripted reports delivered and injected on a PS Vita running
VitaUSBStream + taiHEN.

---

## 1. TL;DR

A pad report is **28 little-endian bytes**. Send it as **seven no-data
control OUTs** on EP0, 4 bytes per request:

| field | value |
|-------|-------|
| `bmRequestType` | `0x40` (host→device, vendor, **device** recipient) |
| `bRequest` | `0x50 + chunk` where `chunk` = 0..6 |
| `wValue` | bytes `[4*chunk .. 4*chunk+1]` of the report, little-endian |
| `wIndex` | bytes `[4*chunk+2 .. 4*chunk+3]` of the report, little-endian |
| `wLength` | 0 — **no data stage** |

Send chunks 0→6 back to back; the plugin reassembles and injects. Chunk `0`
always starts a fresh report (torn state is dropped), so a failed report can
never corrupt the next one.

Reference implementations in this repo:
`core/src/usb_transfer.c` → `usb_send_pad_report()` (C) and
`payload/padtest.c` (a complete sender).

---

## 2. The 28-byte wire report

Little-endian throughout. This layout is the single source of truth shared
by the PS5 encoder (`vd_pad_serialize`, `core/src/pad_passthrough.c`) and
the Vita decoder (`pad_wire_decode`, `vita-side/input-receiver/src/pad_wire.c`).

| offset | type | field | meaning |
|--------|------|-------|---------|
| 0 | u32 | `report_id` | monotonic counter, starts anywhere, +1 per report |
| 4 | u64 | `timestamp_us` | sender timestamp, microseconds (informational) |
| 12 | u32 | `buttons` | button bit mask — see §3 |
| 16 | i16 | `left_x` | left stick X, full range (see §4) |
| 18 | i16 | `left_y` | left stick Y |
| 20 | i16 | `right_x` | right stick X |
| 22 | i16 | `right_y` | right stick Y |
| 24 | u8 | `l2` | left trigger analog, 0..255 (see §5) |
| 25 | u8 | `r2` | right trigger analog, 0..255 |
| 26 | u16 | `reserved` | must be 0 |

Length must be exactly 28 — the decoder rejects anything else.

---

## 3. Buttons

`buttons` is a plain bit mask. **Bits 0..16 are numerically identical to
`SceCtrlButtons`** on the Vita — what you set is what the Vita sees.

| bit | value | button | notes |
|-----|-------|--------|-------|
| 0 | `0x00000001` | SELECT | |
| 1 | `0x00000002` | L3 (left stick click) | |
| 2 | `0x00000004` | R3 (right stick click) | |
| 3 | `0x00000008` | START | |
| 4 | `0x00000010` | D-PAD UP | |
| 5 | `0x00000020` | D-PAD RIGHT | |
| 6 | `0x00000040` | D-PAD DOWN | |
| 7 | `0x00000080` | D-PAD LEFT | |
| 8 | `0x00000100` | L2 (digital) | also auto-set by analog l2 > 32 |
| 9 | `0x00000200` | R2 (digital) | also auto-set by analog r2 > 32 |
| 10 | `0x00000400` | L1 | |
| 11 | `0x00000800` | R1 | |
| 12 | `0x00001000` | TRIANGLE | |
| 13 | `0x00002000` | CIRCLE | |
| 14 | `0x00004000` | CROSS | |
| 15 | `0x00008000` | SQUARE | |
| 16 | `0x00010000` | HOME (PS button) | injected as `SCE_CTRL_PSBUTTON`; emulation is firmware-dependent |
| 17 | `0x00020000` | TOUCH | **not injected** (no `SceCtrl` concept) |

Bits above 17 are ignored (the Vita side masks with `0x1FFFF`).

Multiple buttons combine with OR: `UP | CROSS` = `0x000010 | 0x4000` =
`0x4010`. Releasing everything = `buttons = 0` (combined with neutral
sticks/triggers, that's the "no input" report).

---

## 4. Sticks

Four `i16` fields: `left_x`, `left_y`, `right_x`, `right_y`.

- Range: **-32768 .. 32767**, **0 = centered**
- The Vita converts each axis with `(value + 32768) >> 8` → its native
  **0..255** scale (0 → 0, centered → 128, 32767 → 255)
- Y axis is "up is negative" (standard gamepad convention); if your macro
  wants "up" on the left stick, send `left_y = -32768`

Examples: full left `left_x = -32768`; half right `right_x = 16384`;
centered everything `0,0,0,0`.

---

## 5. Triggers

`l2` and `r2` are **analog 0..255**.

- Any value **> 32** (`PAD_WIRE_TRIGGER_THRESHOLD`) also sets the **digital**
  L2/R2 button bits on the Vita — so games that only read digital triggers
  still see the press
- To press the trigger digitally only: send `l2 = 255` (and don't bother
  with bit 8 — it gets set automatically)
- To release: `l2 = 0` (and clear bit 8 if you set it manually)

---

## 6. Timing & latching (important for macros)

- **Reports latch, they don't persist.** Each report fully replaces the
  previous state (buttons + sticks + triggers at once). To *hold* a button,
  keep sending reports with that bit set.
- **Emulation window ≈ 0.5 s.** The Vita injects each report with
  `uiMake = 32` sampling counts; if reports stop, the press **expires on its
  own** and the plugin also force-resets after **500 ms of silence**. A
  stuck button is therefore impossible — but it also means one single
  report only presses for half a second.
- **Recommended cadence: 10 Hz or faster** while anything is pressed. A
  macro step of "hold CROSS for 600 ms" = 6 reports at 100 ms spacing.
- **Always finish with a neutral report** (everything zero) so nothing is
  left held.
- `report_id` should increase monotonically; `timestamp_us` is informational
  (the injection path doesn't use either today).

---

## 7. Transport details & why it looks like this

The report goes out as **seven SETUP-only vendor control transfers**. That
is unusual, and deliberate — measured on the real target:

1. **EP0 OUT *data stages* do not deliver on this firmware at all.** Proof:
   `payload/eptest.c` sends a UVC `VS_PROBE_CONTROL` `SET_CUR` (the exact
   shape vita-udcd-uvc's own handshake uses) and reads it back with
   `GET_CUR` — the values never arrive. The stream runs on defaults; nobody
   noticed. So no control-OUT *data stage* can carry the report.
2. **SETUP-only control transfers do reach the gadget reliably** (the
   plugin's `processRequest` hook sees 100% of them).
3. `bmRequestType` must be **`0x40`** (device recipient). The
   interface-recipient shape `0x21` validates `wIndex` as an interface
   number and returns `EIO` when `wIndex` carries arbitrary data. The
   plugin accepts both `0x40` and `0x21` (`0x21` only safe with
   `wIndex` = a real interface number).
4. Chunk `0` (`bRequest 0x50`) resets the plugin's reassembly state — send
   it first in every report.

The plugin (`vita-side/input-receiver`) wraps the UVC gadget driver's
`processRequest` when the gadget registers (driver name `VITAUVC00`), so
this channel requires **no extra USB interface and no descriptor changes** —
the video/audio stream is untouched.

---

## 8. Minimal C sender (PS5 payload or any libusb-style host)

```c
/* Send one 28-byte report; fd = open /dev/ugen* of the Vita. */
static int send_report(int fd, const uint8_t wire[28])
{
    for (unsigned i = 0; i < 7; i++) {
        uint16_t wValue = (uint16_t)(wire[i*4] | ((uint16_t)wire[i*4+1] << 8));
        uint16_t wIndex = (uint16_t)(wire[i*4+2] | ((uint16_t)wire[i*4+3] << 8));
        /* bmRequestType 0x40, bRequest 0x50+i, wLength 0 */
        int rc = vendor_control_out(fd, 0x40, (uint8_t)(0x50 + i),
                                    wValue, wIndex);
        if (rc != 0)
            return rc;
    }
    return 0;
}
```

In this repo `vendor_control_out` is `usb_ctrl_xfer()` and the full helper is
`usb_send_pad_report(fd, NULL, &report)` — use `vd_pad_make_report()` to
build the `VdPadReport`, `vd_pad_serialize()` to get the 28 bytes.

Macro example — hold CROSS for 600 ms, then release:

```c
uint8_t wire[28];
VdPadReport r;

for (int i = 0; i < 6; i++) {            /* 6 x 100 ms */
    vd_pad_make_report(&r, ++id, now_us(), VD_PAD_CROSS,
                       0, 0, 0, 0, 0, 0);
    vd_pad_serialize(&r, wire, sizeof wire);
    send_report(fd, wire);
    sleep_ms(100);
}
vd_pad_make_report(&r, ++id, now_us(), 0, 0, 0, 0, 0, 0, 0);  /* release */
vd_pad_serialize(&r, wire, sizeof wire);
send_report(fd, wire);
```

## 9. Python sketch (any USB host with pyusb)

```python
import struct, time, usb.core

dev = usb.core.find(idVendor=0x054c, idProduct=0x1338)  # VitaUSBStream gadget

def report(buttons=0, lx=0, ly=0, rx=0, ry=0, l2=0, r2=0, rid=[0]):
    rid[0] += 1
    wire = struct.pack('<IQIhhhhBBH', rid[0], int(time.time()*1e6) & (2**64-1),
                       buttons, lx, ly, rx, ry, l2, r2, 0)
    for i in range(7):
        w = wire[i*4:i*4+4]
        dev.ctrl_transfer(0x40, 0x50 + i,
                          w[0] | (w[1] << 8), w[2] | (w[3] << 8))

CROSS, CIRCLE = 0x4000, 0x2000

for _ in range(6):              # hold CROSS 600 ms
    report(buttons=CROSS); time.sleep(0.1)
report()                        # release
```

---

## 10. Touch records (touchpad / touchscreen)

Touch rides its **own** chunk family alongside the pad report: a **16-byte**
record, little-endian, sent as **4** no-data control OUTs
(`bmRequestType 0x40`, `bRequest 0x58..0x5B`, 4 bytes/request in
`wValue`/`wIndex`, same scheme as §1). Chunk `0` (`0x58`) resets
reassembly.

| offset | type | field | meaning |
|--------|------|-------|---------|
| 0 | u8 | `port` | 0 = front touchscreen, 1 = rear touchpad |
| 1 | u8 | `count` | active fingers (0..2) |
| 2 | u16 | `reserved` | 0 |
| 4 | u16 | `f0_x` | finger 0 X, raw Vita space 0..1919 |
| 6 | u16 | `f0_y` | finger 0 Y, raw Vita space 0..1087 |
| 8 | u16 | `f1_x` | finger 1 X |
| 10 | u16 | `f1_y` | finger 1 Y |
| 12 | u8 | `f0_active` | 0/1 |
| 13 | u8 | `f1_active` | 0/1 |
| 14 | u16 | `reserved` | 0 |

The plugin appends synthetic `SceTouchReport`s to every sample the game
reads (ids `0x70`/`0x71`, `force 0x80`, up to 6 reports on front / 4 on
rear — the mechanism proven by vitacompanion's `kernel/touch_patch.c`).

**VERIFIED ON HARDWARE (2026-10-06):** scripted swipes from the PS5 drove
the Vita's LiveArea — 10 up / 10 down / 10 up page swipes, visibly observed
by the console owner. The plugin's counters from that run: **1084 chunks =
271 records (exactly what the sender emitted), 0 bad decodes, 7024 reports
injected** into live `sceTouch*` reads (the shell polls at ~16 buffers/call,
continuously — no chicken-and-egg).

**Front-port taps visibly drive the LiveArea.** Records replace state like
pad reports do; send every change, end with `count=0` to release. Finger
0/1 map to injection slots 0/1; inactive fingers are ignored. Validation:
`port <= 1`, `count <= 2`, active flags 0/1, coords within range — anything
else is rejected before the touch driver sees it.

Debugging pitfall (learned the hard way): `ir_log()` writes **silently
drop when called from the USB `processRequest` hook context** — counters
and record logs must be emitted from the touch-read hook (game thread)
context instead. Also: touch tests must target the LiveArea or a game —
the FTP app (VitaShell/FTPVita) does not consume injected touch.

C sender loop from §8: same function, but `bmRequestType 0x40`,
`bRequest 0x58+i`, `i < 4`, over your 16-byte touch record.

Caveat: the DualSense side (reading its touchpad via `scePadRead` →
`ScePadData.touchPad`, normalizing, and choosing front vs rear at runtime)
is the app-side integration; the transport and Vita injection are what this
document covers.

## 11. Caveats

* **HOME** (`0x10000`) maps to `SCE_CTRL_PSBUTTON` = `SCE_CTRL_INTERCEPTED`;
  whether it can be emulated depends on firmware (the ctrl header warns
  values above `0x10000` are interceptable only with shell privileges). Test
  before relying on it in a macro.
* **TOUCH** (bit 17) is never injected — there is no `SceCtrl` concept for
  it. Touch passthrough is a separate design
  ([TOUCHPAD_PASSTHROUGH.md](TOUCHPAD_PASSTHROUGH.md)).
* Emulation targets port 0 / slot 0 with the ds4vita call pattern
  (`ksceCtrlSetButtonEmulation` + `ksceCtrlSetAnalogEmulation`), verified
  working on the tested firmware.
* The sender must be the **USB host**. In the VITA5 rig that means code
  running on the PS5 (root payload or the native app); if the Vita is
  plugged into a PC instead, the same Python/C code works from there.
* The channel exists only while the stream gadget is registered (stream
  enabled in VUS Control). Senders should tolerate `EIO`/disconnects and
  retry.
