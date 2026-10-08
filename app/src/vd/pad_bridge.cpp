// VITA5 app — pad mapping + live forwarding bridge. See pad_bridge.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pad_bridge.hpp"

namespace vd5
{

std::uint32_t map_pad_buttons(std::uint32_t scepad_buttons)
{
    if ((scepad_buttons & scepad::kIntercepted) != 0)
        return 0;

    struct Entry
    {
        std::uint32_t from;
        std::uint32_t to;
    };
    static constexpr Entry kTable[] = {
        {scepad::kCreate, VD_PAD_SELECT},
        {scepad::kL3, VD_PAD_L3},
        {scepad::kR3, VD_PAD_R3},
        {scepad::kOptions, VD_PAD_START},
        {scepad::kUp, VD_PAD_UP},
        {scepad::kRight, VD_PAD_RIGHT},
        {scepad::kDown, VD_PAD_DOWN},
        {scepad::kLeft, VD_PAD_LEFT},
        {scepad::kL2, VD_PAD_L2},
        {scepad::kR2, VD_PAD_R2},
        {scepad::kL1, VD_PAD_L1},
        {scepad::kR1, VD_PAD_R1},
        {scepad::kTriangle, VD_PAD_TRIANGLE},
        {scepad::kCircle, VD_PAD_CIRCLE},
        {scepad::kCross, VD_PAD_CROSS},
        {scepad::kSquare, VD_PAD_SQUARE},
        {scepad::kTouchpad, VD_PAD_TOUCH},
    };
    std::uint32_t out = 0;
    for (const Entry &entry : kTable)
    {
        if ((scepad_buttons & entry.from) != 0)
            out |= entry.to;
    }
    return out;
}

ChordEvents chord_events(std::uint32_t buttons, std::uint32_t prev_buttons)
{
    ChordEvents events;
    if ((buttons & scepad::kL3) == 0)
        return events;
    const std::uint32_t risen = buttons & ~prev_buttons;
    if ((risen & scepad::kR3) != 0)
        events.toggle_capture = true;
    if ((risen & scepad::kRight) != 0)
        events.upscale_cycle = +1;
    if ((risen & scepad::kLeft) != 0)
        events.upscale_cycle = -1;
    if ((risen & scepad::kUp) != 0)
        events.toggle_fullscreen = true;
    if ((risen & scepad::kDown) != 0)
        events.toggle_touch_target = true;
    // L3 + DualSense touchpad click → Vita HOME (PS button). The DualSense
    // has no separate PS bit exposed to the app pad reader, so this is the
    // free modifier chord that stands in for it.
    if ((risen & scepad::kTouchpad) != 0)
        events.press_home = true;
    return events;
}

std::uint32_t mask_modifiers(std::uint32_t buttons)
{
    // No chord in progress: the Vita sees the report as-is.
    if ((buttons & scepad::kL3) == 0)
        return buttons;
    // Chording: L3/R3, D-pad, and the touchpad click belong to the app.
    return buttons & ~(scepad::kL3 | scepad::kR3 | scepad::kUp | scepad::kDown |
                       scepad::kLeft | scepad::kRight | scepad::kTouchpad);
}

std::int16_t map_axis(std::uint8_t value)
{
    // (v - 128) * 257 with 128 centred at 0; the bottom end clamps to the
    // exact int16 minimum (-32896 would otherwise wrap).
    int out = (int)value * 257 - 32896;
    if (out < -32768)
        out = -32768;
    if (out > 32767)
        out = 32767;
    return (std::int16_t)out;
}

void fill_report(VdPadReport *out, std::uint32_t report_id, std::uint64_t timestamp_us,
                 std::uint32_t scepad_buttons, std::uint8_t left_x, std::uint8_t left_y,
                 std::uint8_t right_x, std::uint8_t right_y, std::uint8_t l2, std::uint8_t r2)
{
    if (out == nullptr)
        return;
    vd_pad_make_report(out, report_id, timestamp_us, map_pad_buttons(scepad_buttons),
                       map_axis(left_x), map_axis(left_y), map_axis(right_x), map_axis(right_y), l2,
                       r2);
}

} // namespace vd5

// ---------------------------------------------------------------------------
// Live passthrough bridge. Console build only: the host test build
// (VD5_HOST_TESTS) has neither scePad nor the USB transport and gets inert
// stubs at the bottom of the file.
// ---------------------------------------------------------------------------
#ifndef VD5_HOST_TESTS

#include "platform/ps5/system.hpp" // hui::sys::monotonic_us / log

#include <cstddef>
#include <cstdint>

namespace
{

// scePadRead sample layout (120 bytes), as typed in ProsperoLight and used
// by the vendored kit reader (app/vendor/kit/platform/ps5/pad.cpp).
struct RawPadSample
{
    std::uint32_t buttons;
    std::uint8_t left_x;
    std::uint8_t left_y;
    std::uint8_t right_x;
    std::uint8_t right_y;
    std::uint8_t l2;
    std::uint8_t r2;
    std::uint8_t reserved0[66];
    std::int32_t connected;
    std::uint64_t timestamp_us;
    std::uint8_t extension[16];
    std::uint8_t connected_count;
    std::uint8_t reserved1[15];
};
static_assert(sizeof(RawPadSample) == 120);
static_assert(offsetof(RawPadSample, connected) == 0x4c);
static_assert(offsetof(RawPadSample, timestamp_us) == 0x50);

// Forwarding policy (docs/VITA_INPUT_API.md §6): reports REPLACE state, so a
// held input must be refreshed well inside the Vita's 500 ms silence reset.
constexpr std::uint64_t kHoldRefreshUs = 100000; // >= 10 Hz while anything held
constexpr std::uint64_t kSampleStaleUs = 300000; // no sample -> assume lost
constexpr std::uint64_t kPadReopenUs = 500000;   // scePadOpen retry spacing

// Logical controller state (the raw scePad shape; mapped on send).
struct PadState
{
    std::uint32_t buttons = 0;
    std::uint8_t left_x = 128;
    std::uint8_t left_y = 128;
    std::uint8_t right_x = 128;
    std::uint8_t right_y = 128;
    std::uint8_t l2 = 0;
    std::uint8_t r2 = 0;
    bool connected = false;
    bool operator==(const PadState &) const = default;
};
constexpr PadState kNeutral{}; // all-zero wire report: nothing held

bool g_enabled = true;
PadState g_current = kNeutral; // newest known controller state
PadState g_sent = kNeutral;    // last state the transport accepted
std::uint32_t g_report_id = 0;
std::uint64_t g_last_send_us = 0;
std::uint64_t g_last_sample_us = 0;
std::uint64_t g_reports_sent = 0;

int g_handle = -1; // scePad handle owned by the bridge
std::uint64_t g_next_open_us = 0;
bool g_open_warned = false;

} // namespace

extern "C"
{
    // scePad / user service ABI (same reviewed declarations as the vendored kit
    // reader); linked against the payload SDK stubs like vendor/kit's pad.cpp.
    int sceUserServiceInitialize(const void *params);
    int sceUserServiceGetInitialUser(int *user);
    int scePadInit(void);
    int scePadOpen(int user, int type, int index, const void *parameters);
    int scePadRead(int handle, RawPadSample *samples, int count);
    int scePadClose(int handle);

    // Transport session over core/src/usb_transfer.c + core/src/usb_vita.c,
    // bridged into the app by app/src/vd/core_pad.c (signatures kept in sync
    // with that file).
    int vd_pad_usb_send(const VdPadReport *report);
    int vd_pad_usb_connected(void);
    void vd_pad_usb_close(void);
}

namespace
{

void close_pad()
{
    if (g_handle >= 0)
        scePadClose(g_handle);
    g_handle = -1;
}

bool open_pad(std::uint64_t now)
{
    // The kit reader owns the user service; a second initialize reports
    // "already initialized", which is fine. Never terminated here.
    (void)sceUserServiceInitialize(nullptr);
    int user = -1;
    if (sceUserServiceGetInitialUser(&user) != 0)
        return false;
    (void)scePadInit();
    g_handle = scePadOpen(user, 0, 0, nullptr);
    g_next_open_us = now + kPadReopenUs;
    if (g_handle < 0)
    {
        if (!g_open_warned)
        {
            hui::sys::log("[VD5] pad_bridge scePadOpen failed; will retry");
            g_open_warned = true;
        }
        return false;
    }
    hui::sys::log("[VD5] pad_bridge pad open user=0x%x handle=%d", static_cast<unsigned>(user),
                  g_handle);
    g_open_warned = false;
    g_last_sample_us = now;
    return true;
}

// Convert one controller sample into logical state. A disconnected or
// system-owned (intercepted) pad maps to neutral: nothing may stay held.
PadState state_from_sample(std::uint32_t buttons, std::uint8_t left_x, std::uint8_t left_y,
                           std::uint8_t right_x, std::uint8_t right_y, std::uint8_t l2,
                           std::uint8_t r2, bool connected)
{
    PadState s;
    if (connected && (buttons & vd5::scepad::kIntercepted) == 0)
    {
        s.buttons = buttons;
        s.left_x = left_x;
        s.left_y = left_y;
        s.right_x = right_x;
        s.right_y = right_y;
        s.l2 = l2;
        s.r2 = r2;
        s.connected = true;
    }
    return s;
}

// Self-read path: drain the scePad batch to the newest sample. Also owns
// stuck-input safety for a silent pad: if no sample has arrived for a while
// the state is forced to neutral and the handle is rebound.
void poll_pad(std::uint64_t now)
{
    if (g_handle < 0)
    {
        if (now >= g_next_open_us)
            open_pad(now);
        return;
    }
    RawPadSample raw[64];
    const int count = scePadRead(g_handle, raw, 64);
    if (count > 0)
    {
        const RawPadSample &r = raw[count - 1]; // batch drain to the newest
        g_last_sample_us = now;
        g_current = state_from_sample(r.buttons, r.left_x, r.left_y, r.right_x, r.right_y, r.l2,
                                      r.r2, r.connected != 0);
        return;
    }
    if (now - g_last_sample_us > kSampleStaleUs)
    {
        // The pad stopped reporting (or reads keep failing): release
        // everything and rebind the handle.
        g_current = kNeutral;
        close_pad();
        g_next_open_us = now + kPadReopenUs;
    }
}

// Send policy: every change goes out immediately; a non-neutral state is
// re-sent at >= 10 Hz so a hold cannot expire on the Vita. A failed send
// leaves g_sent untouched, so the pending state retries on the next poll.
void pump(std::uint64_t now)
{
    const bool non_neutral = !(g_current == kNeutral);
    const bool due =
        !(g_current == g_sent) || (non_neutral && now - g_last_send_us >= kHoldRefreshUs);
    if (!due)
        return;
    VdPadReport report;
    vd5::fill_report(&report, ++g_report_id, static_cast<std::uint64_t>(hui::sys::monotonic_us()),
                     g_current.buttons, g_current.left_x, g_current.left_y, g_current.right_x,
                     g_current.right_y, g_current.l2, g_current.r2);
    if (vd_pad_usb_send(&report) == 0)
    {
        g_sent = g_current;
        g_last_send_us = now;
        ++g_reports_sent;
    }
}

// The one report that always gets through on the way out: all-zero, so
// nothing is left held on the Vita (docs/VITA_INPUT_API.md §6).
void send_neutral_now()
{
    VdPadReport report;
    vd5::fill_report(&report, ++g_report_id, static_cast<std::uint64_t>(hui::sys::monotonic_us()),
                     0, 128, 128, 128, 128, 0, 0);
    if (vd_pad_usb_send(&report) == 0)
    {
        g_sent = kNeutral;
        g_last_send_us = static_cast<std::uint64_t>(hui::sys::monotonic_us());
        ++g_reports_sent;
    }
}

void poll_impl(const PadBridgeSample *sample)
{
    if (!g_enabled)
        return;
    const std::uint64_t now = static_cast<std::uint64_t>(hui::sys::monotonic_us());
    if (sample != nullptr)
    {
        g_current =
            state_from_sample(sample->buttons, sample->left_x, sample->left_y, sample->right_x,
                              sample->right_y, sample->l2, sample->r2, sample->connected);
        g_last_sample_us = now;
    }
    else
    {
        poll_pad(now);
    }
    pump(now);
}

} // namespace

void pad_bridge_set_enabled(bool enabled)
{
    if (enabled == g_enabled)
        return;
    g_enabled = enabled;
    if (!enabled)
    {
        // Finish with a neutral all-zero report so nothing stays held.
        if (!(g_sent == kNeutral))
            send_neutral_now();
        g_current = kNeutral;
        close_pad();
        vd_pad_usb_close();
    }
    else
    {
        g_next_open_us = 0; // bind the pad on the very next poll
    }
}

bool pad_bridge_enabled()
{
    return g_enabled;
}

void pad_bridge_poll()
{
    poll_impl(nullptr);
}

void pad_bridge_poll(const PadBridgeSample *sample)
{
    poll_impl(sample);
}

void pad_bridge_shutdown()
{
    if (g_enabled && !(g_sent == kNeutral))
        send_neutral_now();
    g_enabled = false;
    g_current = kNeutral;
    g_sent = kNeutral;
    close_pad();
    vd_pad_usb_close();
}

bool pad_bridge_transport_up()
{
    return vd_pad_usb_connected() != 0;
}

std::uint64_t pad_bridge_reports_sent()
{
    return g_reports_sent;
}

#else // VD5_HOST_TESTS: no pad, no USB — inert stubs.

void pad_bridge_set_enabled(bool enabled)
{
    (void)enabled;
}

bool pad_bridge_enabled()
{
    return false;
}

void pad_bridge_poll()
{
}

void pad_bridge_poll(const PadBridgeSample *sample)
{
    (void)sample;
}

void pad_bridge_shutdown()
{
}

bool pad_bridge_transport_up()
{
    return false;
}

std::uint64_t pad_bridge_reports_sent()
{
    return 0;
}

#endif // VD5_HOST_TESTS
