# Streaming video and audio from a PS Vita over USB

This is the reference for capturing the PS Vita's screen and audio over a single
USB cable on a PlayStation 5. It is written from confirmed device research (live
enumeration on both a PC and a PS5) and the `vita-udcd-uvc` device source. It
describes the working method only.

## 1. The device

A hacked Vita running `vita-udcd-uvc` / **VitaUSBStream** enumerates as a
standard USB video + audio gadget. The host (PC or PS5) treats it as a webcam
and a USB microphone.

| Property | Value |
|----------|-------|
| Vendor ID | `0x054C` (Sony) |
| Product ID | `0x1338` |
| Device class | `0xEF` Miscellaneous, subclass `0x02`, protocol `0x01` (IAD) |
| UVC version | 1.10 (`bcdUVC 0x0110`) |

Interface map (from a live descriptor dump):

| Interface | Class/Sub | Endpoint | Role |
|-----------|-----------|----------|------|
| 0 | `0x0E` / `0x01` | — | VideoControl (UVC) |
| 1 | `0x0E` / `0x02` | **bulk IN `0x81`** | VideoStreaming (UVC) |
| 2 | `0x01` / `0x01` | — | AudioControl (UAC) |
| 3 (alt 0) | `0x01` / `0x02` | — | AudioStreaming (zero-bandwidth) |
| 3 (alt 1) | `0x01` / `0x02` | **isoc IN** | AudioStreaming (active) |

The video bulk endpoint lives on **alt 0** of interface 1 (always present), so
no `SET_INTERFACE` is required to enable it — opening the endpoint is enough.
Only the audio interface uses alt settings (alt 1 activates the isoc stream).

## 2. Video (UVC)

### Format

- **Pixel format: NV12**, 12 bits per pixel
- Format GUID `4e 56 31 32 00 00 10 00 80 00 00 aa 00 38 9b 71` ("NV12")
- Format index 1, subtype `0x04` (Uncompressed)

Frame descriptors (frame index → resolution; every frame offers 60 fps and 30 fps
intervals, `dwDefaultFrameInterval = 166666` = 60 fps):

| Idx | Resolution | NV12 frame bytes |
|-----|-----------|------------------|
| 1 | 960 × 544 (Vita native) | 783,360 |
| 2 | 896 × 504 | 677,376 |
| 3 | 864 × 488 | 632,448 |
| 4 | 480 × 272 | 195,840 |
| 5 | 640 × 480 | 460,800 |

Frame index 1 (960×544) is the right default.

### How the Vita sends a frame

The device sends **each frame as a single bulk IN request**:

```
[ 12-byte UVC header ][ raw NV12 frame data ]
```

- The 12-byte header: `header[0] = 12` (header length), `header[1]` = flags
  (`UVC_STREAM_EOH | UVC_STREAM_FID | UVC_STREAM_EOF` for a complete frame).
- The NV12 data is the whole frame contiguously (Y plane then interleaved UV).
- The request size is `12 + frame_bytes` (e.g. 783,372 for 960×544).
- `dwMaxPayloadTransferSize` advertised in the probe control is the full frame
  size (12 + the largest frame).

### Streaming sequence (host side)

1. **Probe** — `SET_CUR` on `VS_PROBE_CONTROL` (`wValue = 0x0100`) with the
   desired `uvc_streaming_control` (format index, frame index, frame interval).
2. **Probe readback** — `GET_CUR` on `VS_PROBE_CONTROL` returns the negotiated
   parameters. `bFrameIndex` must be non-zero for frames to flow.
3. **Commit** — `SET_CUR` on `VS_COMMIT_CONTROL` (`wValue = 0x0200`) with the
   negotiated control. The Vita sets its `stream` flag and starts sending frames
   on its display vblank.
4. **Open the endpoint** — on FreeBSD `ugen` (the PS5 payload path) bind the
   bulk IN endpoint `0x81` with `USB_FS_INIT` + `USB_FS_OPEN`. This is what
   activates the data transfer; a raw `SET_INTERFACE` is not the mechanism.
5. **Read frames** — pull `12 + frame_bytes` per frame off the bulk endpoint.

Control request shape: `bmRequestType = 0x21` (host-to-device, class, interface),
`bRequest = UVC_SET_CUR (0x01)`, `wValue = control_selector << 8`,
`wIndex = interface_number`, `wLength = 34` (the packed
`uvc_streaming_control` is **34 bytes** in UVC 1.1). `GET_CUR` uses
`bmRequestType = 0xA1`.

### Reading a frame on the PS5 (ugen)

A single host transfer sized to the whole frame (783,372 bytes) **does not
complete** on `ugen` — the exact-fit transfer stalls and times out. A transfer
that **fills its buffer** completes on buffer-full. So read each frame in
bounded chunks (e.g. 64 KiB) and reassemble:

```
total = 12 + frame_bytes
got = 0
while got < total:
    want = min(65536, total - got)
    usb_bulk_read(buf + got, want)   # each completes on buffer-full
    got += returned
```

The final chunk lands on the device's short packet (the device request ends
there). After reassembly: the first 12 bytes are the UVC header, the rest is the
raw NV12 frame.

- `USB_FS_OPEN` `max_bufsize` must be ≥ the largest read (frame size + slack).
- Read size must be ≤ `max_bufsize`; a read larger than `max_bufsize` stalls.

### NV12 → display

NV12 is 12 bpp: a full-resolution Y plane (`w*h` bytes) followed by an
interleaved half-resolution UV plane (`w*h/2` bytes). Convert to RGBA with
BT.601 coefficients, scale `960×544` to the output resolution, and present.

### Verified on hardware

The chunked-read path is confirmed on a PS5 reading a live Vita
(960×544 NV12, 60 fps nominal):

```
endpoint open (USB_FS_INIT+FS_OPEN): OK maxpkt=512
capture window 3.01 s
  payloads read : 137        frames (EOF) : 137
  read errors   : 0          payload bytes : 107,320,320
  frame rate    : 45.59 fps  throughput : 34.06 MiB/s
  first payload head: 0c82 ...  (12-byte UVC header, EOH | EOF)
```

137 frames reassembled with zero read errors at ~45 fps. The first payload's
header bytes `0c 82` confirm a 12-byte UVC header with `UVC_STREAM_EOH |
UVC_STREAM_EOF` (a complete frame), followed by the raw NV12 plane.

## 3. Audio (UAC)

- Interface 2 = AudioControl (`0x01`/`0x01`). Interface 3 = AudioStreaming
  (`0x01`/`0x02`), activated by selecting **alt 1** (alt 0 is zero-bandwidth,
  no endpoints).
- One **isochronous IN** endpoint (under interface 3 alt 1) carries the audio
  stream. Its `bEndpointAddress` and `wMaxPacketSize` are parsed from the
  endpoint descriptor (`core/src/usb_vita.c`, `find_isoc_in_endpoint()`) into
  `VdUsbVitaDevice.audio_isoc_ep` / `audio_isoc_maxpkt` (plus
  `audio_stream_interface` / `audio_isoc_alt`). The older `audio_in_eps[]`
  list only covers alt 0 and therefore never contains this endpoint.
- Format: **48 kHz, stereo, 16-bit PCM** (interleaved). On Windows the same
  device appears as "Line (Vita USB Stream)" at 48 kHz; `VitaUSBStream` mixes
  system/game audio and converts to 48 kHz stereo 16-bit. Expected payload:
  **192 bytes per 1 ms USB frame** (24 per 125 µs micro-frame).
- The PS5 app reads PCM grains (e.g. 256 frames) and plays them through
  `sceAudioOut` at 48 kHz.

The base `vita-udcd-uvc` is video-only; audio requires the **VitaUSBStream**
build, which adds the USB Audio Class function alongside UVC.

### Isochronous read method (hardware-verified)

`core/src/uac_audio.c` (`VdUacSession`) mirrors the proven bulk discipline of
`usb_transfer.c` (START → COMPLETE wait with session-FIFO record validation,
bounded cancel-and-reap, STOP/CLOSE/UNINIT teardown) framed for isoc per
`struct usb_fs_endpoint` in `dev/usb/usb_ioctl.h` ("isochronous USB transfer
only use one buffer, but can have multiple frame lengths!" — the one buffer is
the kernel-side DMA buffer; userland is a per-frame `ppBuffer[i]`/`pLength[i]`
contract, see `uac_audio_state.h`):

1. `USB_IFACE_DRIVER_DETACH` → **select alt 1** (`USB_SET_ALTINTERFACE`, raw
   `UR_SET_INTERFACE` control as fallback; failure is non-fatal and recorded in
   `last_errno`) → `USB_IFACE_DRIVER_DETACH` again → `USB_FS_INIT` (1 slot) →
   `USB_FS_OPEN` on the isoc IN `ep_no` with
   `max_bufsize = frames_per_xfer * maxpkt` and
   `max_frames = frames_per_xfer | USB_FS_MAX_FRAMES_PRE_SCALE`.
2. Per read: `ppBuffer[i] = buffer + i * maxpkt`, `pLength[i] = maxpkt` for
   `i < nFrames` (`USB_FS_FLAG_MULTI_SHORT_OK`), one `USB_FS_START`, wait on
   `USB_FS_COMPLETE`. On completion `pLength[i]` holds packet *i*'s actual
   byte count and `aFrames` the packets completed (partial completion is
   legal). `vd_uac_reassemble()` (`uac_audio_state.h`, host tested) reassembles
   the packets into tightly-packed interleaved PCM and returns the per-packet
   lengths.
3. Teardown: `USB_FS_STOP`, `USB_FS_CLOSE` + `USB_FS_UNINIT`, then alt back to
   0 (the Vita stops sending isoc data).

**Ordering is load-bearing (this was the `USB_FS_START` EINVAL root cause):**
`USB_SET_ALTINTERFACE` is dispatched to `ugen_set_interface()`
(`sys/dev/usb/usb_generic.c`), whose first action is `ugen_fs_uninit(f)` —
it destroys the fd's whole usbfs session (`fs_xfer = NULL`, `fs_ep_max = 0`).
The `USB_FS_START`/`USB_FS_STOP` handlers then fail their
`ep_index >= fs_ep_max` / `fs_xfer[ep_index] == NULL` checks and return
`EINVAL` (errno 22) forever. Selecting the alt *after* `USB_FS_OPEN` (the old
"belt-and-braces" order) therefore killed the session before the first read;
the alt must be selected **before** `USB_FS_INIT` and never while the session
is live. (`USB_IFACE_DRIVER_DETACH` does not touch usbfs state and is safe at
any time.)

`USB_FS_MAX_FRAMES_PRE_SCALE` semantics (resolved from `usbd_transfer_setup_sub`
in `sys/dev/usb/usb_transfer.c`): it declares `max_frames` in 1 ms USB frames
and the kernel converts to 125 µs micro-frame slots itself
(`nframes <<= (3 - fps_shift)`, `fps_shift = bInterval - 1` for HS isoc), up to
8× for 125 µs endpoints. The kernel reads `max_frames` back as the actual
scaled slot count, and a START with `nFrames` above it is rejected. For the
Vita's 1 ms endpoint (bInterval 4) the scaling is a no-op. Select the
interface alt **before** `USB_FS_INIT`. Doing it after `USB_FS_OPEN` kills
the session before the first read.

## 4. Architecture on the PS5

The PS5 SDK exposes no USB APIs, and the sandboxed native app cannot open USB
devices. USB access lives in a **root payload** (via the ELF loader), which has
full FreeBSD `ugen` access (`/dev/ugen*`, `USB_*` ioctls). The native app
renders.

```
PS5 payload (ugen USB)                native app (sandboxed)
-----------------------               -----------------------
scan /dev/ugen*, match VID/PID        mmap shared-memory region
UVC probe/commit handshake            read NV12 frames from the ring
USB_FS_OPEN bulk IN 0x81              NV12 -> RGBA -> VideoOut / GL
chunked bulk reads -> frame           audio ring -> sceAudioOut
  reassembly -> shared-mem ring       scePadRead -> pad reports
UAC isoc reads -> audio ring
```

The two halves communicate through a shared-memory region with
single-producer/single-consumer rings (per-slot sequence counters, no locks).

## 5. Where the code lives

| Concern | Location |
|---------|----------|
| USB device detection + descriptor parsing | `core/src/usb_vita.c`, `core/src/uvc_protocol.c` |
| UVC probe/commit + chunked frame read | `core/src/uvc_stream.c` |
| ugen bulk/control transfers (`USB_FS_*`) | `core/src/usb_transfer.c` |
| UAC isoc audio capture + PCM reassembly | `core/src/uac_audio.c`, `core/include/uac_audio_state.h` |
| Shared-memory ring contract | `core/include/vd_shared.h` |
| Pad-report wire format | `core/src/pad_passthrough.c` |
| Native display app | `app/` |
| Vita-side input receiver | `vita-side/input-receiver/` |

## 6. Quick reference (constants)

```
UVC payload header size      12
uvc_streaming_control size   34        (UVC 1.1, packed)
NV12 frame size              w*h*3/2
Default frame interval       166666    (60 fps; 333333 = 30 fps)
Video bulk endpoint          0x81 (IN, alt 0 of interface 1)
Audio isoc endpoint          interface 3, alt 1
Audio isoc framing           1 buffer + pLength[nFrames]; 192 B per 1 ms frame
USB VID/PID                  0x054C / 0x1338
```
