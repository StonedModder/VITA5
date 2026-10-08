// VITA5 app — DualSense state to the Vita-facing pad report, and the live
// forwarding bridge that drives it (scePadRead -> VdPadReport -> USB).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The translation table follows the reviewed public Pad ABI
// (blackbearreloaded/ps5-native-gamepad-input-research): scePad's button
// word with L3 = 0x2 ... Square = 0x8000, plus the touchpad click. The
// reviewed ABI lives in the ps5-homebrew-ui platform pad reader
// (app/vendor/kit/platform/ps5/pad.cpp, vendored unchanged); this file is
// the VITA5 mapping from that mask into pad_passthrough.h's Vita-facing
// bits (numerically identical to SceCtrlButtons for bits 0..17).
//
// The bridge below is the live half: it reads the DualSense with scePadRead,
// maps each sample with fill_report() and forwards it through the
// hardware-proven pad transport (core/src/usb_transfer.c
// usb_send_pad_report: the 28-byte wire report as seven SETUP-only control
// OUTs — docs/VITA_INPUT_API.md). Reports REPLACE state on the Vita
// (docs/VITA_INPUT_API.md §6), so the bridge keeps re-sending while anything
// is held (>= 10 Hz refresh) and ALWAYS finishes with a neutral all-zero
// report on passthrough disable or controller disconnect so nothing stays
// held. The Vita side force-resets after 500 ms of silence regardless.

#pragma once
#include <cstdint>

extern "C"
{
#include "pad_passthrough.h"
}

namespace vd5
{

// scePad button bits (the reviewed ABI's button word).
namespace scepad
{
constexpr std::uint32_t kCreate = 0x00000001; // Create/Share (Vita SELECT)
constexpr std::uint32_t kL3 = 0x00000002;
constexpr std::uint32_t kR3 = 0x00000004;
constexpr std::uint32_t kOptions = 0x00000008;
constexpr std::uint32_t kUp = 0x00000010;
constexpr std::uint32_t kRight = 0x00000020;
constexpr std::uint32_t kDown = 0x00000040;
constexpr std::uint32_t kLeft = 0x00000080;
constexpr std::uint32_t kL2 = 0x00000100;
constexpr std::uint32_t kR2 = 0x00000200;
constexpr std::uint32_t kL1 = 0x00000400;
constexpr std::uint32_t kR1 = 0x00000800;
constexpr std::uint32_t kTriangle = 0x00001000;
constexpr std::uint32_t kCircle = 0x00002000;
constexpr std::uint32_t kCross = 0x00004000;
constexpr std::uint32_t kSquare = 0x00008000;
constexpr std::uint32_t kTouchpad = 0x00100000;
constexpr std::uint32_t kIntercepted = 0x80000000;
} // namespace scepad

// scePad mask -> VD_PAD_* mask (pad_passthrough.h). Intercepted input is
// dropped: when the system owns the pad nothing is forwarded to the Vita.
std::uint32_t map_pad_buttons(std::uint32_t scepad_buttons);

// 0..255 axis with 128 centred -> signed 16-bit full range.
std::int16_t map_axis(std::uint8_t value);

// L3 chord family (the app's hotkeys; a Vita has no stick clicks, so nothing
// is lost): L3+R3 toggles input capture, L3+D-Pad LEFT/RIGHT cycles the
// upscale mode, L3+D-Pad UP toggles full screen, L3+D-Pad DOWN swaps the
// DualSense touchpad between Vita front/rear touch, L3+touchpad click sends
// a one-shot Vita HOME (PS) press. Chords fire on the rising edge of the
// second button while L3 is held.
struct ChordEvents
{
    bool toggle_capture = false;
    int upscale_cycle = 0; // -1 / 0 / +1
    bool toggle_fullscreen = false;
    bool toggle_touch_target = false;
    bool press_home = false;
};

ChordEvents chord_events(std::uint32_t buttons, std::uint32_t prev_buttons);

// What the Vita may see from a raw mask: while L3 is held the chord buttons
// are the app's (L3/R3/D-pad/touchpad click dropped); otherwise everything
// passes. HOME is never taken from the DualSense PS bit here — only the
// synthetic L3+touchpad chord injects it.
std::uint32_t mask_modifiers(std::uint32_t buttons);

// Fills a normalized report from one pad sample (scePad mask, 0..255 axes).
void fill_report(VdPadReport *out, std::uint32_t report_id, std::uint64_t timestamp_us,
                 std::uint32_t scepad_buttons, std::uint8_t left_x, std::uint8_t left_y,
                 std::uint8_t right_x, std::uint8_t right_y, std::uint8_t l2, std::uint8_t r2);

} // namespace vd5

// ---------------------------------------------------------------------------
// Live passthrough bridge (console build: real scePad + USB transport; host
// test builds compile inert stubs). Single-threaded — call from the UI/frame
// loop only; sends are synchronous (seven SETUP control OUTs on EP0). Poll at
// 10 Hz or faster; the 60 Hz frame loop is plenty.
// ---------------------------------------------------------------------------

// One controller sample in the raw shape the vendored kit reader produces
// (scePad button word, 0..255 axes with 128 centred).
struct PadBridgeSample
{
    std::uint32_t buttons = 0;
    std::uint8_t left_x = 128;
    std::uint8_t left_y = 128;
    std::uint8_t right_x = 128;
    std::uint8_t right_y = 128;
    std::uint8_t l2 = 0;
    std::uint8_t r2 = 0;
    bool connected = false;
};

// Master switch for forwarding input to the Vita (the UI's passthrough
// toggle). Starts ENABLED. Disabling emits one neutral all-zero report so
// nothing stays held, then releases the controller handle and USB session.
void pad_bridge_set_enabled(bool enabled);
bool pad_bridge_enabled();

// Forward one frame of controller state to the Vita. The no-argument form
// reads the DualSense itself with scePadRead (draining the batch to the
// newest sample); the overload forwards a sample the caller already read
// (the frame's newest sample) instead of reading again. Holds are refreshed
// every 100 ms, changes are sent immediately, and disconnects / system-owned
// (intercepted) input force a neutral report. Sends happen only while
// passthrough is enabled and the Vita is on the USB bus; failed sends are
// retried through discovery with a quiet backoff. Cheap when idle.
void pad_bridge_poll();
void pad_bridge_poll(const PadBridgeSample *sample);

// Send the closing neutral report and release the controller handle and the
// USB session. Call before the app exits (disable already does this).
void pad_bridge_shutdown();

// HUD status: true while a USB session to the Vita is open, and the number
// of reports successfully handed to the transport since start.
bool pad_bridge_transport_up();
std::uint64_t pad_bridge_reports_sent();
