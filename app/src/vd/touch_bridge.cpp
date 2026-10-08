// VITA5 app — touch bridge implementation. See touch_bridge.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "touch_bridge.hpp"

#include <cstddef>
#include <cstdint>
#include <chrono>

// Exact prototype from core/include/usb_transfer.h (usb_send_touch_report:
// 0 on success, negative errno otherwise). That header is payload-side — it
// pulls in FreeBSD kernel USB headers (<dev/usb/usb.h>) that the app and
// host builds do not have — so the link contract is declared here instead.
// The signature is the reviewed one; do not change it without the header.
extern "C"
{
    int usb_send_touch_report(int fd, const VdTouchReport *t);
}

namespace vd5
{
namespace
{

// Low-rate refresh while a finger is down. Records replace state, so this
// only guards against a lost record — static touches are not streamed.
constexpr std::uint64_t kKeepaliveUs = 250000; // 250 ms

// DualSense touchpad native resolution (coordinates 0..1919 x 0..941).
constexpr std::uint16_t kDefaultResolutionX = 1920;
constexpr std::uint16_t kDefaultResolutionY = 942;

// Vita raw touch space (docs/VITA_INPUT_API.md section 10).
constexpr std::uint32_t kVitaXMax = 1919;
constexpr std::uint32_t kVitaYMax = 1087;

struct BridgeState
{
    bool enabled = true;
    int port = kTouchPortFront;
    std::uint16_t resolution_x = kDefaultResolutionX;
    std::uint16_t resolution_y = kDefaultResolutionY;
    int fd = -1;
    TouchSender sender = nullptr;
    VdTouchReport last_sent{};
    bool last_sent_valid = false;
    std::uint64_t last_send_us = 0;
    bool have_send_time = false;
};

BridgeState g;

// Largest coordinate the pad delivers for a given resolution.
std::uint32_t source_max(std::uint16_t resolution)
{
    return resolution > 0 ? static_cast<std::uint32_t>(resolution - 1) : 0u;
}

// Round-and-clamp scale between two raw coordinate spaces.
std::uint16_t scale_axis(std::uint16_t value, std::uint32_t src_max, std::uint32_t dst_max)
{
    if (src_max == 0)
        return 0;
    std::uint32_t out = (static_cast<std::uint32_t>(value) * dst_max + src_max / 2) / src_max;
    if (out > dst_max)
        out = dst_max;
    return static_cast<std::uint16_t>(out);
}

bool reports_equal(const VdTouchReport &a, const VdTouchReport &b)
{
    return a.port == b.port && a.count == b.count && a.f0_x == b.f0_x && a.f0_y == b.f0_y &&
           a.f1_x == b.f1_x && a.f1_y == b.f1_y && a.f0_active == b.f0_active &&
           a.f1_active == b.f1_active;
}

// Hands one record to the installed transport. Returns 0 on success.
int send_report(const VdTouchReport &report)
{
    if (g.fd < 0)
        return -1;
    const TouchSender sender = g.sender != nullptr ? g.sender : &usb_send_touch_report;
    return sender(g.fd, &report);
}

// Best-effort release record for a port (disable / reset / port switch).
void send_release(int port)
{
    if (g.fd < 0)
        return;
    VdTouchReport release{};
    release.port = static_cast<std::uint8_t>(port);
    send_report(release);
}

} // namespace

bool touch_bridge_finger_active(std::uint8_t finger)
{
    return (finger & kTouchFingerUp) == 0;
}

std::uint16_t touch_bridge_scale_x(std::uint16_t pad_x)
{
    return scale_axis(pad_x, source_max(g.resolution_x), kVitaXMax);
}

std::uint16_t touch_bridge_scale_y(std::uint16_t pad_y)
{
    return scale_axis(pad_y, source_max(g.resolution_y), kVitaYMax);
}

VdTouchReport touch_bridge_map(const TouchPadSample &sample)
{
    VdTouchReport out{};
    out.port = static_cast<std::uint8_t>(g.port);

    int slot = 0;
    for (int i = 0; i < 2 && slot < 2; ++i)
    {
        if (!touch_bridge_finger_active(sample.finger[i]))
            continue;
        const std::uint16_t x = touch_bridge_scale_x(sample.x[i]);
        const std::uint16_t y = touch_bridge_scale_y(sample.y[i]);
        if (slot == 0)
        {
            out.f0_x = x;
            out.f0_y = y;
            out.f0_active = 1;
        }
        else
        {
            out.f1_x = x;
            out.f1_y = y;
            out.f1_active = 1;
        }
        ++slot;
    }
    out.count = static_cast<std::uint8_t>(slot);
    return out;
}

void touch_bridge_set_port(int port)
{
    const int next = (port == kTouchPortFront) ? kTouchPortFront : kTouchPortRear;
    if (next == g.port)
        return;

    const int previous = g.port;
    g.port = next;

    if (g.last_sent_valid && g.last_sent.count > 0)
    {
        // Records replace state per port: release the old target or its
        // last fingers would stay down forever.
        send_release(previous);
        g.last_sent = VdTouchReport{};
        g.last_sent.port = static_cast<std::uint8_t>(next);
        g.last_sent_valid = true;
    }
    else if (g.last_sent_valid)
    {
        // Nothing down: retarget the quiet state without a pointless record.
        g.last_sent.port = static_cast<std::uint8_t>(next);
    }
}

int touch_bridge_get_port()
{
    return g.port;
}

void touch_bridge_set_enabled(bool enabled)
{
    if (enabled == g.enabled)
        return;
    if (!enabled && g.last_sent_valid && g.last_sent.count > 0)
    {
        send_release(g.port);
        g.last_sent = VdTouchReport{};
        g.last_sent.port = static_cast<std::uint8_t>(g.port);
    }
    g.enabled = enabled;
    g.last_sent_valid = false; // re-enabling re-announces the current state
}

bool touch_bridge_enabled()
{
    return g.enabled;
}

void touch_bridge_set_touch_resolution(std::uint16_t resolution_x, std::uint16_t resolution_y)
{
    g.resolution_x = resolution_x;
    g.resolution_y = resolution_y;
}

void touch_bridge_set_transport(int fd, TouchSender sender)
{
    g.fd = fd;
    g.sender = sender;
}

void touch_bridge_reset()
{
    if (g.last_sent_valid && g.last_sent.count > 0)
        send_release(g.port);
    g.last_sent = VdTouchReport{};
    g.last_sent.port = static_cast<std::uint8_t>(g.port);
    g.last_sent_valid = false;
    g.have_send_time = false;
}

bool touch_bridge_update(const TouchPadSample &sample, std::uint64_t now_us)
{
    if (!g.enabled)
        return false;

    const VdTouchReport report = touch_bridge_map(sample);

    if (!g.last_sent_valid && report.count == 0)
    {
        // Nothing down and nothing announced yet: start silent instead of
        // sending a startup release record.
        g.last_sent = report;
        g.last_sent_valid = true;
        return false;
    }

    const bool changed = !g.last_sent_valid || !reports_equal(report, g.last_sent);
    const bool keepalive = report.count > 0 && g.have_send_time && !changed &&
                           (now_us - g.last_send_us) >= kKeepaliveUs;
    if (!changed && !keepalive)
        return false;

    if (send_report(report) != 0)
        return false; // transport unavailable/failed: retry on the next update

    g.last_sent = report;
    g.last_sent_valid = true;
    g.last_send_us = now_us;
    g.have_send_time = true;
    return true;
}

#ifndef VD5_HOST_TESTS

namespace
{

// scePadRead sample layout (120 bytes), per the reviewed Pad ABI
// (blackbearreloaded/ps5-native-gamepad-input-research): the touch block at
// 0x34 is a count, 7 reserved bytes and two 8-byte contacts.
struct RawTouch
{
    std::uint16_t x;
    std::uint16_t y;
    std::uint8_t id;
    std::uint8_t reserved[3];
};

struct RawTouchPad
{
    std::uint8_t count;
    std::uint8_t reserved[7];
    RawTouch points[2];
};

struct RawPadData
{
    std::uint32_t buttons;
    std::uint8_t left_x;
    std::uint8_t left_y;
    std::uint8_t right_x;
    std::uint8_t right_y;
    std::uint8_t l2;
    std::uint8_t r2;
    std::uint8_t reserved0[42];
    RawTouchPad touch_pad;
    std::int32_t connected;
    std::uint64_t timestamp_us;
    std::uint8_t extension[16];
    std::uint8_t connected_count;
    std::uint8_t reserved1[15];
};
static_assert(sizeof(RawTouch) == 8);
static_assert(sizeof(RawTouchPad) == 24);
static_assert(sizeof(RawPadData) == 120);
static_assert(offsetof(RawPadData, touch_pad) == 0x34);
static_assert(offsetof(RawPadData, connected) == 0x4c);
static_assert(offsetof(RawPadData, timestamp_us) == 0x50);
static_assert(offsetof(RawPadData, connected_count) == 0x68);

// scePad queue drain: one call covers the handle's queued samples.
constexpr int kPollBatch = 16;

void decode_touch(const RawTouchPad &touch_pad, TouchPadSample *out)
{
    int count = touch_pad.count;
    if (count > 2)
        count = 2; // the DualSense reports at most two contacts
    for (int i = 0; i < 2; ++i)
    {
        if (i < count)
        {
            out->x[i] = touch_pad.points[i].x;
            out->y[i] = touch_pad.points[i].y;
            out->finger[i] = touch_pad.points[i].id;
        }
        else
        {
            out->x[i] = 0;
            out->y[i] = 0;
            out->finger[i] = kTouchFingerUp;
        }
    }
}

} // namespace

extern "C"
{
    int scePadRead(int handle, RawPadData *samples, int count);
    int sceUserServiceInitialize(const void *params);
    int sceUserServiceGetInitialUser(int *user);
    int scePadInit(void);
    int scePadOpen(int user, int type, int index, const void *parameters);
    int scePadClose(int handle);
}

void touch_bridge_poll(int pad_handle)
{
    if (!g.enabled || pad_handle < 0)
        return;

    RawPadData raw[kPollBatch];
    const int count = scePadRead(pad_handle, raw, kPollBatch);
    if (count <= 0)
        return;

    // Batch drain to the newest record (input-lag contract of
    // pad_passthrough.h): only the latest touch state matters.
    const RawPadData &newest = raw[count - 1];
    TouchPadSample sample{};
    decode_touch(newest.touch_pad, &sample);
    touch_bridge_update(sample, newest.timestamp_us);
}

// Self-managed variant: lazily opens and keeps our own pad handle (the
// pad_bridge open/retry policy), so the app wiring is one call per frame.
// If the OS refuses the handle, touch stays silent and retries after the
// same 500 ms backoff.
void touch_bridge_poll()
{
    static int s_handle = -1;
    static std::uint64_t s_last_open_attempt_us = 0;

    if (!g.enabled)
        return;

    if (s_handle < 0)
    {
        using namespace std::chrono;
        const std::uint64_t now = static_cast<std::uint64_t>(
            duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
        if (s_last_open_attempt_us != 0 && now - s_last_open_attempt_us < 500000)
            return;
        s_last_open_attempt_us = now;

        (void)sceUserServiceInitialize(nullptr);
        int user = -1;
        if (sceUserServiceGetInitialUser(&user) != 0)
            return;
        scePadInit();
        s_handle = scePadOpen(user, 0, 0, nullptr);
        if (s_handle < 0)
            return;
    }

    RawPadData raw[kPollBatch];
    const int count = scePadRead(s_handle, raw, kPollBatch);
    if (count <= 0)
        return;

    const RawPadData &newest = raw[count - 1];
    TouchPadSample sample{};
    decode_touch(newest.touch_pad, &sample);
    touch_bridge_update(sample, newest.timestamp_us);
}

#endif // !VD5_HOST_TESTS

} // namespace vd5