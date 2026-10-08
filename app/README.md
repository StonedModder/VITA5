# VITA5 — native display app (`app/`)

The display/UX half of **VITA5**: a native PS5 application that docks a
PS Vita over USB and shows its screen and audio on the TV, with controller
input forwarded back to the Vita.

The kernel payload (built separately) captures the Vita's UVC video (NV12
960x544) and UAC audio (48 kHz stereo) over USB and publishes them into a
shared-memory ring defined by the committed contract in
[`core/include/vd_shared.h`](../core/include/vd_shared.h). This app is the
consumer: it maps that region, converts and renders the frames, plays the
audio through `sceAudioOut`, reads the controller with `scePadRead` and
publishes pad reports into the pad ring for passthrough.

## What is in here

| Path | What it is |
| --- | --- |
| `src/` | the application: screens, dock model, settings, video texture, audio feed |
| `src/vd/` | the contract layer: shared region reader/writer (`vd_shm`), NV12 → RGBA (`nv12`), scePad → Vita pad report (`pad_bridge`, built on `core/src/pad_passthrough.c`) |
| `src/screen_*.cpp` | the four screens (splash, connect, live view, settings), each designed in three states: connected / not-connected / request-in-flight |
| `vendor/kit/` | [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) vendored unchanged at revision in `vendor/kit/REVISION.txt` (draw list, components, themes, sounds, PS5 platform layer, runtime shims) — GPL-3.0-or-later, third-party terms in `vendor/kit/THIRD_PARTY_NOTICES.md` |
| `assets/` | the kit's baked fonts and sound sets, shipped as `/app0/assets` |
| `sce_sys/` | presentation assets + `param.json` (identity `PPSA39410`) |
| `tests/vd_tests.cpp` | host tests for the ring protocol, NV12 conversion and pad mapping |
| `tools/prepare-opengl.sh` | fetches the pinned ps5-opengl SDK and stages the link inputs (adapted from the kit's script) |

The UI is built on the kit's animated draw list: springs and easing for all
motion (focus rings, screen transitions, entrance staggers), animated
backdrops, breathing status indicators, live FPS/audio/input readouts, and
sound cues on every interaction.

## Building

Builds run on **Linux/WSL Ubuntu** (the repository's stock toolchain; the
root build script is unchanged):

```bash
# 1. host checks for the contract layer (no console needed)
make -C app test

# 2. fetch and verify the pinned ps5-opengl SDK, stage link inputs
make -C app deps

# 3. build the PS5 application folder (via the repo's stock root build)
make -C app
```

Output lands in `dist/PPSA39410/` (`eboot.bin`, `sce_module/libc.prx`,
`sce_sys/`, `assets/`) exactly as the boilerplate's build produces it.

An application identity is set with a fresh title ID (never reused):

```bash
make -C app init TITLE_ID=PPSA##### APP_NAME="VITA5"
```

`app/Makefile` drives the stock root build with `APP_SOURCE_DIR=app`,
`APP_PARAM`/`APP_SCE_SYS=app/sce_sys`, `APP_ASSETS=app/assets` and
`APP_STATIC_ARCHIVES=app/.generated/libps5opengl-group.a`. Two link-time
needs of the statically linked OpenGL runtime are staged without touching
the stock build script (see `tools/prepare-opengl.sh`): the AGC import
stubs are copied into the payload SDK's stub directory (which the build
links and hands to the converter), and the process-lifetime heap + splash
hold from `vendor/kit/runtime/` are wired with `APP_WRAP_SYMBOLS`, exactly
as the kit's own build wires them.

## The shared-memory contract

`src/vd/vd_shm.{hpp,cpp}` consumes `core/include/vd_shared.h` exactly:
fixed offsets, per-slot release/acquire sequence counters (odd = filling,
next even = released; a consumer that sees the seq change mid-copy drops the
torn slot). Video is read newest-slot-first, audio strictly in order, and
pad reports are published as the documented 28-byte wire format
(`core/include/pad_passthrough.h`) into the 64-byte slots, zero-padded.

**Mapping interface (assumption, not yet verified):** `vd_shared.h` fixes
the layout but not the mapping mechanism, and the kernel payload is being
built in parallel. This app attaches by `mmap`-ing a character device that
publishes the region:

```
/dev/vdshm   mmap(2)-able, at least VD_SHM_TOTAL_BYTES, RDWR
```

If the payload lands with a different hand-off, only `Shm::attach()`
changes. Until the payload exists, `attach()` fails cleanly and the app
runs its "waiting for the dock service" states. No fake frames are ever
drawn: the render path is real code against the contract.

## What is verified (host evidence, Linux/WSL)

- `make -C app test` — the SPSC ring protocol (including a 200 ms producer
  race run where torn copies must never reach the app), the NV12 → RGBA
  converter against hand-computed BT.601 values, the audio ring's in-order
  delivery, pad publish/round-trip through the committed wire format and
  the scePad → Vita button table. Verbatim:

  ```
  race: 643 frames delivered, 0 torn frames escaped, 2998 torn drops
  vd_tests: all checks passed
  ```

- `make doctor` — `Required Linux/WSL build prerequisites are available.`
  (every tool `[OK]`, PS5 SDK and `libc.prx` `[CACHED]`).
- `make test` (root, stock suites) — `EXIT_TEST=0`, ending in `Ran 10
  tests in 7.152s` / `OK` for the tooling integration suite plus the
  GoogleTest unit suite, Lapy, update-check and self-update checks.
- `make -C app` — `EXIT=0`, ending in the stock converter's self check
  (`container: signed, plaintext ... integrity: valid`) and `Build
  complete. App folder: dist/PPSA39410`. The PS5 compile used the pinned
  payload SDK headers and the ps5-opengl SDK; the output is a native FE10
  `eboot.bin` plus the verified `sce_module/libc.prx`. This is
  compile/package evidence only.
- `make lint` (root) — **fails, and the failures are pre-existing**:
  `EXIT_LINT=2`, with 93 clang-format violations in `tests/test_host.c`
  (52) and `tests/test_usb_transfer.c` (41) — files owned by the kernel
  payload workstream, untouched by this app. No `app/` file appears in the
  lint output.

Two stock-tooling facts worth recording:

1. The checkout had CRLF worktree bytes in `runtime/libc.prx.sha256` and
   `tooling/native/runtime/*.txt`, which failed the runtime digest gate
   (`runtime checksum manifest does not match the release digest`).
   `.gitattributes` now pins `*.sha256` and `*.txt` to LF and the three
   files were restored to their exact indexed (LF) bytes — the expected
   hashes are unchanged (`libc.prx: OK`, both reproduction passes match).
2. `tooling/native/ps5-pie.ld` gained the four `PROVIDE(__eh_frame_…)`
   bounds that the OpenGL runtime's `libunwind` resolves its unwind tables
   through. These lines are verbatim from the newer upstream
   ps5-native-app-boilerplate (the kit's own copy of the script) and are
   `PROVIDE`s: they define nothing unless referenced, so non-OpenGL apps
   built from the template are unaffected. Without them the OpenGL link
   fails on `undefined symbol: __eh_frame_start`.

## What is NOT hardware-verified

Nothing in this list has run on a PS5; treat all of it as unproven until a
console cycle is run:

- rendering the video (shared ring → NV12 → RGBA → GL texture → screen),
- audio playback through `sceAudioOut`,
- pad forwarding end to end (PS5 → shared ring → kernel → USB → Vita),
- the `/dev/vdshm` mapping interface (the payload side is still being
  built; see above),
- display modes other than the boot default, frame pacing under load,
  and the on-screen behaviour of every state at 60 fps,
- the packaged presentation assets on the home screen.

The PS5 build compiling and the host tests passing are not evidence that
any of the above works on hardware.

## Screens and states

| Screen | not connected | request in flight | connected |
| --- | --- | --- | --- |
| `splash` | — (the loading transition: wordmark assembly, progress, spinner) | | |
| `connect` | what to do (three numbered steps), retry action, last dock error | "Contacting the dock service" / "Starting the video stream" with live activity | "Stream established", moves to the live view |
| `live` | animated no-signal stage + steps to fix it | waiting for the first frame | the Vita's screen + HUD (fps in/out, audio meter, input reports, drops) |
| `settings` | banner: settings apply on the next connection | "Saving settings" while the write is in flight | the same settings, live over the stream |

Controls: Cross confirms/retries, Circle goes back, Options opens settings,
L2/R2 step values in settings.

## Licence

GPL-3.0-or-later (same as the repository). The vendored kit keeps its own
licence and third-party notices (`vendor/kit/`). Fonts and sounds belong to
their respective authors; see `vendor/kit/THIRD_PARTY_NOTICES.md`.

Presentation assets: the icon and 4K backgrounds are the project's own
masters (`app/sce_sys/*.png`, regenerated into `icon0.png` / `pic0.dds` /
`pic1.dds` with `tools/prepare-assets.sh --output-directory app/sce_sys`).
`sce_sys/snd0.at9` is still the boilerplate's selection music; replace it
with the project's own ATRAC9 track before shipping.
