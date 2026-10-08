# VITA5 — Integration: payload + app

How the two PS5 halves fit together: a **root payload** (full `ugen` USB
access) reads the Vita's streams and a **native app** (sandboxed, no USB
access) renders them and forwards the controller. They meet at one
shared-memory region.

```
 PS Vita ── USB ──▶ payload (ugen) ── shared ring ──▶ native app ──▶ TV
 (UVC + UAC)        read streams        /dev/vdshm     render + audio
      ▲                                                      │
      └── SETUP-only control OUTs ◀── pad + touch ◀── scePadRead / touchpad
```

The PS5 SDK exposes no USB APIs to sandboxed apps, which is why the split
exists: USB lives in the ELF-loaded root payload, presentation lives in the
app.

## 1. Data flow, forward path (Vita → TV)

| Stage | Component | Contract |
|-------|-----------|----------|
| Detect the Vita | `core/src/usb_vita.c` | scan `/dev/ugen*`; match VID `054c` / PID `1338` (fast pre-filter) or, as proof, a VideoStreaming interface (class `0x0E`/sub `0x02`) with a bulk IN endpoint and the NV12 format GUID. Confirmed device: `/dev/ugen2.2`. |
| UVC handshake | `core/src/uvc_stream.c`, `core/include/uvc_stream.h` | `SET_CUR` → `GET_CUR` on `VS_PROBE_CONTROL`, then `SET_CUR` on `VS_COMMIT_CONTROL` with the 34-byte `uvc_streaming_control` (format index 1, frame index 1 = 960×544, interval 166666 = 60 fps nominal). Verified working on hardware. |
| Activate video endpoint | `core/src/usb_transfer.c` | `USB_FS_INIT` + `USB_FS_OPEN` on bulk IN `0x81` — this is what activates the transfer (the bulk endpoint sits on alt 0 of the VideoStreaming interface and is always present; no `SET_INTERFACE` is required to start video). Confirmed: `maxpkt=512`. |
| Read frames | `uvc_stream.h` `vd_uvc_read_payload()` | **Chunked bulk reads** (e.g. 64 KiB), reassembled into `12 + frame_bytes` per frame (783,372 bytes for 960×544). A single full-frame transfer stalls on `ugen` — chunking is mandatory. First 12 bytes are the UVC payload header (`0x0c 0x82` = EOH+EOF on a complete frame), the rest is raw NV12. Verified: **137 frames reassembled, 0 read errors, ~45 fps, 34 MiB/s** over a 3 s window. |
| Audio capture | `core/src/uac_audio.c`, `core/include/uac_audio.h` | Isochronous IN under the AudioStreaming interface's **alt 1**. Ordering is load-bearing: select alt 1 **before** `USB_FS_INIT`, never while the session is live (`USB_SET_ALTINTERFACE` destroys the usbfs session; selecting it later made every `USB_FS_START` fail with `EINVAL`). Then `USB_FS_OPEN` with `max_frames = frames_per_xfer | USB_FS_MAX_FRAMES_PRE_SCALE` (default 32 frames/xfer ≈ 32 ms), one multi-frame transfer per read (`ppBuffer[i]`/`pLength[i]`), reassembled by `vd_uac_reassemble()` (`uac_audio_state.h`, host-tested). Confirmed: **56 transfers, 0 errors, 344,064 PCM bytes = 56 × 32 × 192 exactly**, real 48 kHz stereo int16. |
| Publish | payload → shared ring | Frames/chunks are written into the video and audio rings of `core/include/vd_shared.h` (see §2). |
| Consume + present | `app/src/vd/vd_shm.cpp`, `nv12.cpp`, `audio_pipe.cpp` | App maps the region, reads video newest-slot-first (dropping torn slots), converts NV12 → RGBA with BT.601, scales 960×544 to the output and presents; audio is read strictly in order and played through `sceAudioOut` at 48 kHz. |

## 2. The shared ring (`core/include/vd_shared.h`)

One physical region, mapped by both sides at fixed offsets. Synchronization is
per-slot release/acquire sequence counters — single-producer/single-consumer,
no locks, no syscalls.

| Region | Offset | Size |
|--------|--------|------|
| `VdSharedHeader` | 0 | 4,096 B |
| Video ring (2 slots × 1,382,400 B, max 720p NV12) | `VD_SHM_VIDEO_OFFSET` = 4,096 | 2,764,800 B |
| Audio ring (16 slots × 4,096 B PCM chunks) | `VD_SHM_AUDIO_OFFSET` | 65,536 B |
| Pad ring (8 slots × 64 B) | `VD_SHM_PAD_OFFSET` | 512 B |
| **Total** (`VD_SHM_TOTAL_BYTES`) | | **2,834,944 B** |

- **Slot protocol:** the producer bumps `seq` to odd, fills the slot, then bumps
  it to the next even value as the release. A consumer reads `seq`, copies the
  slot, re-reads `seq`, and drops the slot if it changed mid-copy (torn
  frame). The app's host tests exercise exactly this, including a 200 ms
  producer-race run (`race: 643 frames delivered, 0 torn frames escaped,
  2998 torn drops`).
- **Header state:** `video_width/height/pixel_format` (`VD_PIX_NV12`),
  `audio_sample_rate/channels/bits` (48000 / 2 / 16), per-ring `seq` arrays
  and `producer` cursors, plus health counters (`vita_detected`,
  `stream_active`, `frames_captured`, `frames_dropped`, `audio_chunks`,
  `pad_reports`, `last_error`).
- **Error codes** (`VD_ERR_*`): `NO_VITA`, `PROBE_FAILED`, `COMMIT_FAILED`,
  `BULK_READ_FAILED`, `BAD_FRAME_HEADER`, `VITA_DISCONNECTED`.
- Video slots hold raw NV12 (`w*h*3/2` bytes: full Y plane then interleaved
  half-resolution UV); the confirmed 960×544 frame is 783,360 B and fits the
  1,382,400 B slot. Audio slots hold tightly-packed interleaved int16 PCM.
  Pad slots hold the 28-byte wire report (see §4) zero-padded to 64 bytes.
- Pointer helpers: `VD_SHM_VIDEO_PTR/AUDIO_PTR/PAD_PTR(base, slot)`.

## 3. The payload → app hand-off (`/dev/vdshm`)

`vd_shared.h` fixes the **layout**; the mapping mechanism is a separate
contract. The committed app side (`app/src/vd/vd_shm.cpp`) attaches by
`open("/dev/vdshm", O_RDWR)` + `mmap`:

```
/dev/vdshm   mmap(2)-able, at least VD_SHM_TOTAL_BYTES (2,834,944), RDWR
```

If the device node is missing, `attach()` fails cleanly ("dock service not
present") and the app runs its designed not-connected states — no fake frames
are ever drawn. If the payload lands with a different hand-off, only
`Shm::attach()` changes.

**Status:** the ring layout and the app-side consumption are host-tested; the
`/dev/vdshm` hand-off itself is an assumption agreed between the two
workstreams and is **not yet verified end to end on hardware** (the payload
that publishes the region is still being built).

## 4. Controller return path (PS5 → Vita)

| Stage | Component | Contract |
|-------|-----------|----------|
| Read pad | app (`scePadRead`) | One read per update, batch-drained to the newest record (`pad_passthrough.h` input-lag contract). |
| Normalize | `core/include/pad_passthrough.h` | `VdPadReport` — `report_id` (monotonic), `timestamp_us`, `buttons`, four int16 stick axes (full range, 0 = centered), `l2`/`r2` 0..255. Buttons use the `VD_PAD_*` bit layout, which is **numerically identical to `SceCtrlButtons` for bits 0..16**; `vd_pad_translate_buttons()` converts the scePad mask (table in `app/src/vd/pad_bridge.cpp`), `vd_pad_make_report()` builds the report. |
| Serialize | `vd_pad_serialize()` | 28-byte little-endian wire format, published into the pad ring (64-byte slots, zero-padded): `0 u32 report_id, 4 u64 timestamp_us, 12 u32 buttons, 16 i16 lx, 18 i16 ly, 20 i16 rx, 22 i16 ry, 24 u8 l2, 25 u8 r2, 26 u16 reserved=0`. The Vita-side parser mirrors this exactly and rejects any length other than 28. |
| Forward | payload | Consumes pad-ring slots (copy + immediate forward, no syscall on the hot path beyond the USB transfer) and keeps the LAST-GOOD report so held state survives. |
| USB | vendor control OUT `0x40`, EP0, **SETUP-only** | The 28-byte report goes out as **seven no-data control OUTs** (`usb_send_pad_report()`): `bRequest 0x50 + chunk` (chunk 0..6), 4 bytes per request carried in `wValue`/`wIndex`, `wLength` 0. Chunk 0 (`0x50`) resets the plugin's reassembly, so a torn report can never corrupt the next one. This shape is forced: EP0 OUT *data stages* are dead firmware-wide (proof `payload/eptest.c`), while SETUP-only control transfers reach the gadget's `processRequest` hook 100%. No extra USB interface is published — the stream configuration stays pristine. |
| Inject | `input_receiver.c` | `pad_wire_decode()` → `ksceCtrlSetButtonEmulation` / `ksceCtrlSetAnalogEmulation` (the ds4vita call pattern): `ctrl = wire & 0x1FFFF`, sticks converted `(v + 32768) >> 8`, triggers > 32 force the digital L/R trigger bits. 500 ms no-report reset + ~0.5 s `uiMake` expiry prevent stuck input. |

Not on the pad channel: **touch** rides its own chunk family (see §8) and
**motion** is not forwarded. The wire `TOUCH` bit (bit 17) is never injected —
there is no `SceCtrl` concept for it. The original design note is
[TOUCHPAD_PASSTHROUGH.md](TOUCHPAD_PASSTHROUGH.md); the implemented touch
wire format is [VITA_INPUT_API.md](VITA_INPUT_API.md) §10.

**Status (2026-10-05):** **controller input is working end to end** — the
pad channel is a chunked SETUP-only control transport (v1.9 of the plugin;
see [VITA_INPUT_API.md](VITA_INPUT_API.md) for the wire protocol and macro
guide). The earlier bulk-OUT/extra-interface approach was abandoned because
SceUdcd kept serving the gadget's *original* configuration tables no matter
how the driver's pointers were rewritten (wire `wTotalLength`/
`bNumInterfaces` stayed at the originals), and EP0 OUT *data stages* were
proven dead firmware-wide (`payload/eptest.c`). What remains is the full
app → ring → payload → USB path (live DualSense → Vita). The verified
VitaUSBStream gadget layout (driver `VITAUVC00`, ifaces 0–3, video
`0x81`/audio `0x83`) and the full plugin bring-up history live in
[vita-side/input-receiver/README.md](../vita-side/input-receiver/README.md)
("Verified on hardware").

## 5. The committed contracts

| Header | Owns |
|--------|------|
| `core/include/vd_shared.h` | Shared-memory region layout, slot protocol, pixel/audio/pad sizing, error codes |
| `core/include/pad_passthrough.h` | `VdPadReport`, `VD_PAD_*` bits, 28-byte pad wire format, `VdTouchReport` + the 16-byte touch wire format (`vd_touch_serialize`), serialize/deserialize |
| `core/include/uvc_stream.h` | UVC session: probe/commit handshake, bulk payload reads, `eps[2]` endpoint slots (video IN = slot 0, pad OUT = slot 1) |
| `core/include/uac_audio.h` | UAC isoc session: alt-1-before-`USB_FS_INIT` ordering, one-buffer multi-frame transfers, `VD_UAC_DEFAULT_FRAMES_PER_XFER 32` |
| `core/include/uac_audio_state.h` | `vd_uac_reassemble()` packet reassembly (host-tested) |
| `core/include/usb_transfer.h`, `usb_vita.h`, `uvc_protocol.h` | ugen `USB_FS_*` discipline, `usb_send_pad_report()` / `usb_send_touch_report()` (SETUP-only chunked control OUTs), device/descriptor parsing, UVC protocol structs (34-byte `uvc_streaming_control`) |

The Vita-side mirror of the pad wire format is
`vita-side/input-receiver/include/pad_wire.h` (host-tested decoder:
`ALL TESTS PASSED (100098 checks)`).

## 6. Load-bearing rules (do not reorder)

1. **Video reads must be chunked.** A single 783,372-byte bulk transfer stalls
   and times out on `ugen`; bounded reads (≤ `max_bufsize` set at
   `USB_FS_OPEN`) complete on buffer-full and the last chunk lands on the
   device's short packet. Confirmed on hardware.
2. **Audio alt 1 before `USB_FS_INIT`, never after.** `USB_SET_ALTINTERFACE`
   calls `ugen_fs_uninit()` and destroys the fd's usbfs session; selecting the
   alt after opening makes `USB_FS_START`/`USB_FS_STOP` fail with `EINVAL`
   forever. `USB_IFACE_DRIVER_DETACH` is safe at any time. Confirmed by
   fixing exactly this (56/56 clean transfers afterwards).
3. **Endpoint activation is `USB_FS_INIT` + `USB_FS_OPEN`**, for video and
   audio alike.
4. **One USB configuration per registered `SceUdcdDriver`.** The Vita-side
   input interface must live in the stream gadget's own configuration (Mode A
   patch or Mode B hook) — a separately registered second driver cannot
   append interfaces and would kill video. (The shipped channel needs no
   interface at all — SETUP-only control OUTs on EP0, §4 — but any future
   extra interface must follow this rule.)
5. **`USB_FS_MAX_FRAMES_PRE_SCALE`** declares `max_frames` in 1 ms USB frames
   and the kernel scales to micro-frames itself; for the Vita's 1 ms isoc
   endpoint the scaling is a no-op. A START with more frames than the scaled
   count is rejected.

## 7. Verified vs not yet verified

Confirmed on real hardware (PS5 + live Vita):

- Device `/dev/ugen2.2`, VID `054c` / PID `1338`, class-match detection.
- UVC probe/commit handshake; bulk IN `0x81` opened via `USB_FS_INIT` +
  `USB_FS_OPEN`; 137 NV12 frames reassembled from chunked reads at ~45 fps
  with 0 errors.
- UAC isoc IN (alt 1, selected before `USB_FS_INIT`): 56 transfers, 0 errors,
  real 48 kHz stereo int16 PCM.
- Host-side tests: SPSC ring protocol (race run, 0 torn escapes), NV12 →
  RGBA against hand-computed BT.601 values, audio in-order delivery, pad
  wire-format round-trip and scePad → Vita button table (`vd_tests: all
  checks passed`); pad decoder (`pad_wire_test`, 100,098 checks).
- **Input injection (2026-10-05):** 163/163 scripted pad reports delivered
  and injected over the chunked SETUP-only control transport (plugin v1.9)
  — the Vita visibly pressed the scripted buttons
  (`payload/padtest.c`).
- **Front touchscreen injection (2026-10-06):** scripted LiveArea page
  swipes visibly observed by the console owner — 1084 chunks = 271/271
  touch records decoded, 0 bad decodes, 7024 reports injected
  (`payload/touchswipe.c`, `payload/touchtest.c`).

Not yet verified on hardware (treat as contracts awaiting proof):

- The `/dev/vdshm` hand-off end to end (payload publishing the region).
- Rendering, `sceAudioOut` playback, and frame pacing on the PS5.
- The live app input chain (§8): live DualSense → `scePadRead`/touchpad →
  bridges → ring → payload → USB → Vita. The scripted payloads prove the
  transport and the Vita-side injection, not the app-side chain.
- Rear-touchpad injection against a game that reads the rear pad (port 1 is
  implemented but pending a game test).
- Input forwarding while the full UVC/UAC stream runs under the native app
  (the combined run — see the Gate 5 cases in
  [TEST_PLAN.md](TEST_PLAN.md)).

## 8. Native app input integration

The app drives the Vita with the **live DualSense**: buttons/sticks/triggers
through the pad chain, the DualSense touchpad through a touch chain. Both end
at the same transport — **SETUP-only chunked control OUTs on EP0** (EP0 OUT
*data stages* are dead firmware-wide, proof `payload/eptest.c`;
[VITA_INPUT_API.md](VITA_INPUT_API.md) §7) — sent by the root payload on the
app's behalf (the app is sandboxed and has no USB access).

**Pad chain:** DualSense `scePadRead` (one read per update, batch-drained to
the newest record) → `app/src/vd/pad_bridge` (`map_pad_buttons()` scePad mask
→ `VD_PAD_*`, `fill_report()` 0..255 axes / 128-centred → i16 full range) →
28-byte wire report (`vd_pad_serialize()`) → pad ring (§2) → payload forwards
it as **7 control OUTs** (`bmRequestType 0x40`, `bRequest 0x50..0x56`, 4
bytes per request in `wValue`/`wIndex`, `wLength` 0) → plugin
`pad_wire_decode()` → `ksceCtrlSetButtonEmulation` /
`ksceCtrlSetAnalogEmulation`. Field-by-field contract:
[VITA_INPUT_API.md](VITA_INPUT_API.md) §2 (the 28-byte report), §3 (button
bits), §4 (sticks), §5 (triggers), §6 (timing — 10 Hz+ while anything is
held, always finish with a neutral report).

**Touch chain:** DualSense touchpad (`ScePadData.touchPad`) →
`app/src/vd/touch_bridge` (fingers normalized into raw Vita touch space
0..1919 × 0..1087, front port 0 / rear port 1 chosen at runtime) → 16-byte
touch record (`vd_touch_serialize()`) → payload sends it as **4 control OUTs**
(`bmRequestType 0x40`, `bRequest 0x58..0x5B`, same chunk scheme; chunk 0 =
`0x58` resets reassembly) → plugin reassembles → `SceTouch*` hooks append
synthetic `SceTouchReport`s (ids `0x70`/`0x71`, `force 0x80`) to every sample
the game/shell reads. Record layout and validation rules:
[VITA_INPUT_API.md](VITA_INPUT_API.md) §10.

The integration also adds the matching app settings and an in-app test
harness for exercising both bridges against a live stream.

### Verified status

| Input path | Status | Evidence |
|------------|--------|----------|
| Buttons, sticks, triggers (pad channel) | **Verified on hardware, 2026-10-05** | 163/163 scripted reports delivered and injected; the Vita visibly pressed the scripted buttons (`payload/padtest.c`). Wire contract: [VITA_INPUT_API.md](VITA_INPUT_API.md) §2–§6. |
| Front touchscreen (port 0) | **Verified on hardware, 2026-10-06** | scripted LiveArea page swipes visibly observed: 1084 chunks = 271/271 touch records decoded, 0 bad decodes, 7024 reports injected (`payload/touchswipe.c`, `payload/touchtest.c`). Record contract: [VITA_INPUT_API.md](VITA_INPUT_API.md) §10. |
| Rear touchpad (port 1) | Implemented, **pending a game test** | same record format and injection path ([VITA_INPUT_API.md](VITA_INPUT_API.md) §10); needs a game that reads the rear pad — the LiveArea does not consume it |
| Live DualSense → app → Vita (the two chains above) | **Integration in progress** | the scripted payloads prove the transport and the Vita-side injection; the live app chain has not run end to end yet |

Touch testing rule (learned on hardware): target the **LiveArea or a game** —
the Vita FTP app (VitaShell/FTPVita) does not consume injected touch, so a
touch case run against it can never show a visible pass. Diagnostics counters
live in `ur0:/tai/input_receiver.log` (see
[VITA_INPUT_API.md](VITA_INPUT_API.md) §10 and the Gate 5 cases in
[TEST_PLAN.md](TEST_PLAN.md)).
