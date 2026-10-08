# VITA5 — Vita setup (what the owner does on the Vita)

This is the hands-on guide for preparing the PS Vita side of the dock. When
you are done, a single USB cable to the PS5 carries video (Vita → PS5), audio
(Vita → PS5), and controller input (PS5 → Vita). No Bluetooth is involved
anywhere in the path.

```
        USB (one cable)
PS5 host ================================ PS Vita (SceUdcd gadget)
  |  UVC video   bulk IN  0x81  <--------- VideoStreaming interface
  |  UAC audio   isoc IN  (alt 1) <------- AudioStreaming interface
  |  pad reports bulk OUT 0x02  ---------> vendor interface (input receiver)
```

## What runs on the Vita

| Piece | What it is | Source |
|-------|-----------|--------|
| Stream gadget | `vita-udcd-uvc` / **VitaUSBStream** build: adds UVC video (NV12 960×544, bulk IN `0x81`) and — in the VitaUSBStream build — UAC audio (48 kHz stereo int16, isoc IN) to the `SceUdcd` USB gadget | xerpi [vita-udcd-uvc](https://github.com/xerpi/vita-udcd-uvc); audio requires the VitaUSBStream build (base `vita-udcd-uvc` is video-only) |
| Input receiver | VITA5 taiHEN kernel plugin (`.skprx`) that accepts 28-byte controller reports on USB **bulk OUT `0x02`** and injects them as `SceCtrl` input | [`vita-side/input-receiver/`](../vita-side/input-receiver/) |

Requirements before you start:

- A hacked Vita running a taiHEN-based CFW (enso or equivalent) so kernel
  plugins load at boot. The SceCtrl emulation NIDs used by the input receiver
  are from the 3.60 firmware database, and the reference implementation
  (xerpi's ds4vita) runs on 3.60–3.68-era CFW. On newer firmware, confirm the
  same exports exist before relying on input.
- A file manager with `ur0:` access (e.g. VitaShell) to copy files and edit
  `config.txt`.
- vitasdk (`arm-vita-eabi-gcc`, `vita-elf-create`, `vita-make-fself`) if you
  build the plugins yourself.

## 1. Get the two plugins

You need one `.skprx` for streaming and input. There are two ways to combine
them; both keep the video path byte-identical.

### Mode A (recommended): input receiver built into the stream gadget

`vita-side/input-receiver/integration/vita-udcd-uvc-input.patch` extends
`vita-udcd-uvc` itself with a third USB interface (vendor class `0xFF`, one
bulk OUT endpoint `0x02`). Copy the four sources into your `vita-udcd-uvc`
(or VitaUSBStream) checkout and apply the patch:

```bash
cp src/input_receiver.c src/pad_wire.c            <vita-udcd-uvc>/src/
cp include/pad_wire.h include/input_receiver.h    <vita-udcd-uvc>/include/
cd <vita-udcd-uvc> && git apply /path/to/vita-udcd-uvc-input.patch && make
```

The patch applies cleanly to a pristine checkout (`git apply --check`
verified). The result is a single `.skprx` (the gadget's, e.g.
`udcd_uvc.skprx`) carrying video + audio + input.

### Mode B (standalone `input_receiver.skprx`, experimental)

```bash
cd vita-side/input-receiver && make
```

This builds a self-contained kernel plugin that hooks `ksceUdcdRegister` and
appends the vendor interface to the UVC gadget's configuration at registration
time. It must load **before** the stream gadget (see step 3).

> **Build status:** the wire-format decoder is host-tested
> (`pad_wire_test: ALL TESTS PASSED (100098 checks)`), but neither plugin has
> been compiled with vitasdk on the development machine. Expect to be the
> first to run `make` end to end.

## 2. Copy the plugins to `ur0:tai`

Over USB (VitaShell `USB connection`) or FTP, copy the built `.skprx` files
into `ur0:tai/` on the Vita memory card. Typical result:

```
ur0:tai/udcd_uvc.skprx          # the stream gadget (Mode A: includes input)
ur0:tai/input_receiver.skprx    # Mode B only
```

## 3. Register them in `ur0:tai/config.txt`

Kernel plugins go under the `*KERNEL` section. taiHEN loads kernel plugins in
**file order**, and for Mode B that order is load-bearing: the input receiver
installs a hook that must be in place before the stream gadget registers its
USB driver.

```
*KERNEL
ur0:tai/input_receiver.skprx    # Mode B only — must be ABOVE the gadget
ur0:tai/udcd_uvc.skprx
```

- **Mode A:** only the gadget line is needed (input is compiled in).
- **Mode B:** `input_receiver.skprx` **above** `udcd_uvc.skprx`.
- If `config.txt` already has a `ur0:tai/udcd_uvc.skprx` (or `ur0:tai/henkaku.skprx`)
  under `*KERNEL`, add your lines to that existing section — do not create a
  second `*KERNEL` block.

## 4. Reboot

Fully reboot the Vita (or power-cycle) so taiHEN loads the kernel plugins at
boot. A crash or a Vita that hangs at boot after this step means a plugin
failed to load — remove the line you added, reboot again, and re-check which
plugin is at fault.

## 5. Connect and stream

1. Plug the Vita into a USB port on the PS5 with a normal USB cable.
2. The gadget enumerates automatically as a USB **webcam + microphone** —
   there is nothing to press or enable on the Vita. VID `0x054C`, PID `0x1338`.
3. Video frames start flowing only after the host runs the UVC probe/commit
   handshake (the VITA5 payload does this when the app starts the stream). The
   Vita screen keeps running normally; the gadget captures it on display
   vblank.
4. Audio is mixed by VitaUSBStream (system + game audio) into 48 kHz stereo
   16-bit PCM and streams whenever the host opens the isoc endpoint.
5. Controller reports arrive on bulk OUT `0x02` whenever the PS5 side is
   forwarding input; the plugin injects them so games see a controller.

## 6. Verify the Vita is streaming

The device-side check is enumeration: a streaming Vita is indistinguishable
from a USB webcam and USB microphone to any host.

**Quickest check — plug the Vita into a PC first** (this exact behaviour is
confirmed on Windows with the VitaUSBStream build):

- Video: a `USB Video Device` / **"Vita USB Stream"** camera appears and shows
  the Vita screen at **960 × 544**.
- Audio: a recording device **"Line (Vita USB Stream)"** appears at **48 kHz**.
- USB identity: VID `0x054C`, PID `0x1338`, Windows instance `UDCD_UAV`,
  composite device (IAD: interfaces 0–1 UVC, 2+ UAC).

**On the PS5:** the VITA5 payload finds the device at `/dev/ugen2.2`
(VID `054c`, PID `1338`; confirmed on hardware). The payload's probe log is
the PS5-side confirmation.

**On the Vita itself:** the plugins loaded if the Vita boots normally and the
screen behaves as usual — both plugins are silent background kernel modules
with no UI.

## Input behaviour (what to expect)

- Reports are 28 bytes, little-endian; bits 0–16 map 1:1 onto `SceCtrlButtons`
  (the plugin asserts this at compile time), so buttons pass straight through.
  Sticks are full-range signed, converted to `0..255` (`0x80` centered).
  Analog `L2`/`R2` above threshold 32 also force the digital trigger bits.
- The `TOUCH` bit has no `SceCtrl` equivalent and is dropped. `HOME` maps to
  `SCE_CTRL_PSBUTTON`/`SCE_CTRL_INTERCEPTED`; whether it is accepted depends
  on firmware behaviour.
- Failsafes against stuck input: every report refreshes the emulation with a
  ~0.5 s `uiMake` window, and if no report arrives for 500 ms the injection is
  explicitly reset. The plugin also calls `ksceKernelPowerTick` while input is
  active so the Vita does not sleep mid-game.

## Known caveats and unverified points

- **Interface numbering when combining audio + input.** The input-receiver
  patch is written against the video-only `vita-udcd-uvc` configuration, where
  the vendor input interface is number 2. The confirmed VitaUSBStream
  enumeration puts audio on interfaces 2–3. Combining UAC audio **and** the
  input interface in one configuration therefore shifts the interface numbers
  and is **not yet verified** on hardware; if you build that combination,
  check the descriptor dump before expecting the PS5 to find bulk OUT `0x02`.
- **Mode B's endpoint tracking** (a plugin-local endpoint struct with a
  pre-filled endpoint number instead of an entry in the registered driver's
  table) is unverified on hardware. Mode A avoids it entirely — use Mode A.
- **Input injection on real firmware** (`ksceCtrlSetButtonEmulation` /
  `ksceCtrlSetAnalogEmulation`, called exactly as xerpi's ds4vita calls them)
  has not yet been exercised against the PS5 host side.
- **PS5 enumeration of the vendor interface** alongside UVC on the same USB
  configuration is USB-legal (the Interface Association Descriptor covers only
  the video interfaces) but not yet tested with the actual PS5 USB stack.
- The base gadget's own firmware caveat is inherited: vita-udcd-uvc inserts
  its Interface Association Descriptor through a firmware-specific hook
  tested by xerpi on his firmware. If UVC binding fails on your firmware,
  video will not appear regardless of VITA5.

If something does not enumerate, the host-side debugging reference is
[STREAMING.md](STREAMING.md) (the full device profile) and
[PC_CAPTURE_NOTES.md](PC_CAPTURE_NOTES.md) (confirmed PC behaviour).
