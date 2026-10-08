// VITA5 app — DualSense touchpad to the Vita touch surfaces.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turns scePad touchpad samples (ScePadData.touchPad: up to two fingers,
// x[2]/y[2]/finger[2]) into VdTouchReport records for the Vita's front
// touchscreen / rear touchpad. Source and mapping per
// docs/TOUCHPAD_PASSTHROUGH.md sections 2-4; the 16-byte wire record is
// docs/VITA_INPUT_API.md section 10 (built by vd_touch_serialize in
// core/src/pad_passthrough.c, the single source of truth).
//
// - Coordinates are normalized from the DualSense pad space and scaled into
//   the Vita raw space (x 0..1919, y 0..1087), clamped to range.
// - One finger maps to slot 0, two fingers to slots 0/1 (active fingers are
//   compacted in sample order; inactive slots are zeroed).
// - Records replace state: exactly one record per change, ending with
//   count=0 on release. A low-rate keepalive refreshes while a finger is
//   down (guards a lost record; no steady-state streaming).
// - The target port is selectable at runtime; the default is the REAR
//   touchpad (TOUCHPAD_PASSTHROUGH.md section 4).
//
// Transport: usb_send_touch_report(fd, &report) — the exact
// core/include/usb_transfer.h signature. That header is payload-side (it
// pulls in FreeBSD kernel USB headers foreign to the app and host builds),
// so touch_bridge.cpp declares the prototype instead of including it; a
// test harness can stub the same symbol or install its own sink with
// touch_bridge_set_transport().

#pragma once
#include <cstdint>

extern "C"
{
#include "pad_passthrough.h"
}

namespace vd5
{

// Vita touch ports carried by VdTouchReport.port.
constexpr int kTouchPortFront = 0; // front touchscreen
constexpr int kTouchPortRear = 1;  // rear touchpad

// scePad contact-id bit set while no finger occupies the slot (the
// DualSense HID "finger up" flag carried through ScePadData.touchPad).
constexpr std::uint8_t kTouchFingerUp = 0x80;

// One DualSense touchpad sample: ScePadData.touchPad's fields for up to two
// fingers. A slot is touching while (finger[i] & kTouchFingerUp) == 0.
struct TouchPadSample
{
    std::uint16_t x[2] = {0, 0};
    std::uint16_t y[2] = {0, 0};
    std::uint8_t finger[2] = {kTouchFingerUp, kTouchFingerUp};
};

// Touch-report sink with usb_send_touch_report's exact signature
// (core/include/usb_transfer.h): returns 0 on success, negative errno
// otherwise.
using TouchSender = int (*)(int fd, const VdTouchReport *report);

// ---- mapping (pure; host-testable) ------------------------------------

// A slot carries a finger while bit 7 of its scePad contact id is clear.
bool touch_bridge_finger_active(std::uint8_t finger);

// Pad -> Vita raw space (x 0..1919, y 0..1087): scaled from the configured
// touch resolution and clamped (TOUCHPAD_PASSTHROUGH.md section 4).
std::uint16_t touch_bridge_scale_x(std::uint16_t pad_x);
std::uint16_t touch_bridge_scale_y(std::uint16_t pad_y);

// Normalized record for one sample on the current target port: active
// fingers compacted into slots 0/1 (1 finger -> slot 0), inactive slots
// zeroed. count is the active finger count (0..2).
VdTouchReport touch_bridge_map(const TouchPadSample &sample);

// ---- runtime controls (settings UI) ----------------------------------

// 0 = front touchscreen, 1 = rear touchpad; any other value selects the
// rear touchpad. Default: rear. Switching while a touch is held releases
// the old port first so the Vita never keeps a stuck finger; the new target
// takes effect on the next report (TOUCHPAD_PASSTHROUGH.md section 5).
void touch_bridge_set_port(int port);
int touch_bridge_get_port();

// Disabled bridges send nothing. Disabling while a touch is held releases
// it first; re-enabling re-announces the current state on the next update.
void touch_bridge_set_enabled(bool enabled);
bool touch_bridge_enabled();

// DualSense touchpad resolution used for the normalization (scePad
// TouchpadInformation.resolution_x / resolution_y), default 1920x942.
void touch_bridge_set_touch_resolution(std::uint16_t resolution_x,
                                       std::uint16_t resolution_y);

// Report sink + transfer fd. sender == nullptr selects the default
// usb_send_touch_report. fd < 0 disables sending (state tracking continues,
// so a later install sends the current state). Default: fd -1, default
// sender. Pass a persistent fd; the touch path is 4 SETUP-only control OUTs
// and must be serialized with other transfers on the same ugen session.
void touch_bridge_set_transport(int fd, TouchSender sender);

// Release any held touch and forget the change state. Call on pad
// disconnect / connected_count generation changes and stream restarts.
void touch_bridge_reset();

// ---- driving ---------------------------------------------------------

// Feed one pad sample with a microsecond timestamp (e.g. the sample's
// timestamp_us). Sends exactly one record when the normalized state changed
// since the last send, plus the low-rate keepalive while fingers are down.
// Returns true when a record was handed to the transport (0 from the
// sender); an unavailable or failing transport keeps the state dirty so the
// next update retries.
bool touch_bridge_update(const TouchPadSample &sample, std::uint64_t now_us);

#ifndef VD5_HOST_TESTS
// scePadRead glue: batch-drains the handle's queue, keeps the newest sample
// and feeds touch_bridge_update. Host tests drive touch_bridge_update
// directly (this entry point needs the PS5 scePad runtime).
void touch_bridge_poll(int pad_handle);

// Self-managed variant: owns its own pad handle (opened lazily, retried
// every 500 ms). This is the one the app calls per frame.
void touch_bridge_poll();
#endif

} // namespace vd5