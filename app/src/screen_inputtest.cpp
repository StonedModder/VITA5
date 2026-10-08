// VITA5 app — in-app input test harness screen.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Menu of scripted input tests, each run on a background thread that speaks
// the hardware-verified input protocol (docs/VITA_INPUT_API.md):
//   pad reports    28-byte LE wire, 7 SETUP-only control OUTs (bRequest 0x50..0x56)
//   touch records  16-byte LE wire, 4 SETUP-only control OUTs (bRequest 0x58..0x5B)
// Reports latch (each fully replaces the previous state), ~10 Hz while held,
// and EVERY run ends with a neutral pad report plus touch releases — also on
// stop, on screen exit and on destruction. The scripts mirror the proven
// payload senders step for step:
//   Button tour          payload/padtest.c     (its exact step table)
//   Front tap + swipe    payload/touchtest.c   (phases 1-2)
//   Two-finger tap       payload/touchtest.c   (phase 3)
//   Rear swipe           payload/touchtest.c   (phase 4)
//   LiveArea swipe       payload/touchswipe.c  (10 up / 10 down / 10 up)

#include "screen_inputtest.hpp"

#include "core/tween.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

extern "C"
{
#include "pad_passthrough.h"
#include "usb_vita.h"

    // ---- core transport contract (core/include/usb_transfer.h) -------------
    // Declared here instead of #include "usb_transfer.h": that header pulls in
    // the FreeBSD dev/usb kernel headers, which are not C++-safe. These are its
    // exact public signatures (C linkage, matching how the app already wraps the
    // core headers in extern "C" — see app/src/vd/pad_bridge.hpp). The touch
    // record itself (VdTouchReport + vd_touch_serialize) comes from
    // pad_passthrough.h above: the 16-byte record of docs/VITA_INPUT_API.md §10.
    struct usb_fs_endpoint;
    int usb_send_pad_report(int fd, struct usb_fs_endpoint *ep, const VdPadReport *report);
    int usb_send_touch_report(int fd, const VdTouchReport *t);
}

namespace vd5
{

namespace
{

using gfx::Color;
using gfx::Rect;

// ---- menu ---------------------------------------------------------------

struct TestEntry
{
    const char *label;
    const char *detail;
};

constexpr TestEntry kTests[] = {
    {"Button tour", "d-pad, faces, shoulders/triggers, START/SELECT, stick sweeps"},
    {"Front tap + swipe", "front: tap (960, 544), swipe x 200-1700 at y 400"},
    {"Two-finger tap", "front: (600, 544) + (1300, 544) together"},
    {"Rear swipe", "rear: x 200-1700 at y 544"},
    {"LiveArea swipe 10/10/10", "10 up, 10 down, 10 up (proven page pattern)"},
    {"Neutral reset", "release pad + touch right now"},
};
constexpr int kTestCount = static_cast<int>(sizeof(kTests) / sizeof(kTests[0]));

// ---- status log ---------------------------------------------------------

constexpr int kLogCap = 160;    // ring capacity (older lines drop)
constexpr int kLogVisible = 13; // lines drawn at once
constexpr int kLogText = 128;

struct LogLine
{
    char text[kLogText];
    bool error;
};

// ---- pacing (docs/VITA_INPUT_API.md §6) ---------------------------------

constexpr unsigned kReportHz = 10;   // recommended cadence while held
constexpr unsigned kPadHoldMs = 600; // padtest.c: 6 reports per step
constexpr unsigned kPadGapMs = 1000 / kReportHz;
constexpr unsigned kTouchStepMs = 100;   // touchtest.c: 10 Hz touch steps
constexpr int kTapSteps = 6;             // touchtest.c hold_touch
constexpr int kSwipeSteps = 30;          // touchtest.c swipe
constexpr unsigned kLiveAreaStepMs = 30; // touchswipe.c SWIPE_STEP_US
constexpr unsigned kLiveAreaGapMs = 400; // touchswipe.c SWIPE_GAP_US
constexpr unsigned kLiveAreaBatchGapMs = 1000;
constexpr int kLiveAreaSteps = 8;
constexpr int kLiveAreaSwipes = 10;

// ---- scripted pad steps: the payload/padtest.c table, exactly ------------

struct PadStep
{
    const char *name;
    std::uint32_t buttons;
    std::int16_t lx, ly, rx, ry;
    std::uint8_t l2, r2;
};

constexpr PadStep kPadSteps[] = {
    {"CROSS", VD_PAD_CROSS, 0, 0, 0, 0, 0, 0},
    {"CIRCLE", VD_PAD_CIRCLE, 0, 0, 0, 0, 0, 0},
    {"neutral", 0, 0, 0, 0, 0, 0, 0},
    {"D-PAD UP", VD_PAD_UP, 0, 0, 0, 0, 0, 0},
    {"D-PAD DOWN", VD_PAD_DOWN, 0, 0, 0, 0, 0, 0},
    {"D-PAD LEFT", VD_PAD_LEFT, 0, 0, 0, 0, 0, 0},
    {"D-PAD RIGHT", VD_PAD_RIGHT, 0, 0, 0, 0, 0, 0},
    {"CROSS", VD_PAD_CROSS, 0, 0, 0, 0, 0, 0},
    {"CIRCLE", VD_PAD_CIRCLE, 0, 0, 0, 0, 0, 0},
    {"SQUARE", VD_PAD_SQUARE, 0, 0, 0, 0, 0, 0},
    {"TRIANGLE", VD_PAD_TRIANGLE, 0, 0, 0, 0, 0, 0},
    {"L1", VD_PAD_L1, 0, 0, 0, 0, 0, 0},
    {"R1", VD_PAD_R1, 0, 0, 0, 0, 0, 0},
    {"L2", VD_PAD_L2, 0, 0, 0, 0, 0, 0},
    {"R2", VD_PAD_R2, 0, 0, 0, 0, 0, 0},
    {"START", VD_PAD_START, 0, 0, 0, 0, 0, 0},
    {"SELECT", VD_PAD_SELECT, 0, 0, 0, 0, 0, 0},
    {"L-STICK X sweep", 0, -32768, 0, 0, 0, 0, 0},
    {"L-STICK X sweep +", 0, 32767, 0, 0, 0, 0, 0},
    {"L-STICK Y sweep", 0, 0, -32768, 0, 0, 0, 0},
    {"L-STICK Y sweep +", 0, 0, 32767, 0, 0, 0, 0},
    {"R-STICK X sweep", 0, 0, 0, -32768, 0, 0, 0},
    {"R-STICK X sweep +", 0, 0, 0, 32767, 0, 0, 0},
    {"R-STICK Y sweep", 0, 0, 0, 0, -32768, 0, 0},
    {"R-STICK Y sweep +", 0, 0, 0, 0, 32767, 0, 0},
    {"triggers half", 0, 0, 0, 0, 0, 128, 128},
    {"neutral (reset)", 0, 0, 0, 0, 0, 0, 0},
};
constexpr int kPadStepCount = static_cast<int>(sizeof(kPadSteps) / sizeof(kPadSteps[0]));

// ---- touch geometry (payload/touchtest.c + payload/touchswipe.c) --------

constexpr std::uint8_t kPortFront = 0;
constexpr std::uint8_t kPortRear = 1;
constexpr std::uint16_t kTapX = 960, kTapY = 544;
constexpr std::uint16_t kTwoFingerX0 = 600, kTwoFingerX1 = 1300;
constexpr std::uint16_t kSwipeXFrom = 200, kSwipeXTo = 1700;
constexpr std::uint16_t kFrontSwipeY = 400;
constexpr std::uint16_t kRearSwipeY = 544;
constexpr std::uint16_t kLiveAreaX = 960;
constexpr std::uint16_t kLiveAreaYBottom = 880, kLiveAreaYTop = 180;

// The single place that maps the scripts' coordinates onto VdTouchReport.
VdTouchReport make_touch_report(std::uint8_t port, std::uint8_t count, std::uint16_t x0,
                                std::uint16_t y0, std::uint8_t a0, std::uint16_t x1,
                                std::uint16_t y1, std::uint8_t a1)
{
    VdTouchReport r;
    std::memset(&r, 0, sizeof(r));
    r.port = port;
    r.count = count;
    r.f0_x = x0;
    r.f0_y = y0;
    r.f1_x = x1;
    r.f1_y = y1;
    r.f0_active = a0;
    r.f1_active = a1;
    return r;
}

std::uint64_t now_us()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000u +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000u;
}

class InputTestScreen final : public Screen
{
  public:
    explicit InputTestScreen(AppServices &services) : services_(services)
    {
        pthread_mutex_init(&lock_, nullptr);
    }

    ~InputTestScreen() override
    {
        stop_and_join();
        pthread_mutex_destroy(&lock_);
    }

    const char *name() const override
    {
        return "inputtest";
    }

    void enter() override
    {
        age_ = 0.0f;
        highlight_.snap(row_rect(focus_));
        log_line("input test harness ready");
    }

    void leave() override
    {
        // Leaving always costs a neutral report/release first.
        stop_and_join();
    }

    void update(const hui::InputFrame &input, float dt, Feedback &feedback) override
    {
        age_ += dt;

        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = focus_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < kTestCount)
            {
                focus_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else if (!input.nav_repeat)
            {
                refusal_.trigger();
                feedback.play(hui::audio::Cue::error, 0.8f, 0.0f, 0.7f);
            }
        }

        if (input.is_pressed(Action::confirm))
        {
            if (running_.load())
            {
                refusal_.trigger();
                feedback.play(hui::audio::Cue::error, 0.8f, 0.0f, 0.7f);
            }
            else
            {
                feedback.play(hui::audio::Cue::select);
                start_test(focus_);
            }
        }

        // Square is the stop button; Circle stops a run before leaving.
        const bool stop_asked =
            input.is_pressed(Action::west) || (input.is_pressed(Action::back) && running_.load());
        if (stop_asked && running_.load())
        {
            request_stop();
            feedback.play(hui::audio::Cue::back);
        }
        else if (input.is_pressed(Action::back))
        {
            services_.close_inputtest = true; // back to settings
            feedback.play(hui::audio::Cue::back);
        }

        highlight_.target(row_rect(focus_));
        highlight_.update(dt, 16.0f);
        refusal_.update(dt, 9.0f);

        snapshot();
    }

    void draw(Frame &frame) const override
    {
        fill_backdrop(frame, hui::gfx::BackdropMode::waves);
        hui::gfx::DrawList &list = frame.scene;
        const hui::ui::Fonts &fonts = frame.fonts;
        const DockState state = services_.dock.connect_state();

        draw_brand(frame, hui::tween::clamp01(age_ * 2.0f));

        hui::ui::Painter paint(list, fonts, dock_theme(), frame.glass);

        const float title_in = hui::tween::stagger(age_, 0, 0.05f, 0.5f);
        list.push_opacity(title_in);
        paint.heading("Input test", 88.0f, 220.0f, 56.0f);
        list.pop_opacity();

        // Run-state chip: what the worker thread is doing right now.
        {
            const Rect chip{88.0f, 258.0f, running_view_ ? 320.0f : 180.0f, 44.0f};
            list.rounded_rect(chip, 22.0f,
                              Color::rgb(running_view_ ? color::kSignal : color::kMuted,
                                         running_view_ ? 0.14f : 0.12f));
            list.circle(chip.x + 24.0f, chip.cy(), 7.0f,
                        Color::rgb(running_view_ ? color::kSignal : color::kMuted,
                                   running_view_ ? 0.6f + 0.4f * hui::ui::breathe((float)frame.time)
                                                 : 0.8f));
            hui::ui::text(list, fonts.semibold, running_view_ ? "TEST RUNNING" : "IDLE",
                          chip.x + 44.0f, chip.cy() + 8.0f, 20.0f,
                          Color::rgb(running_view_ ? color::kSignal : color::kMuted),
                          gfx::Align::left, 1.5f);
        }

        // ---- the test rows (left column) ----
        for (int i = 0; i < kTestCount; ++i)
        {
            const Rect r = row_rect(i);
            const float in = hui::tween::stagger(age_, i + 1, 0.05f, 0.45f);
            if (in <= 0.0f)
                continue;
            list.push_opacity(in);
            draw_row(paint, r, i, i == focus_);
            list.pop_opacity();
        }

        Rect ring = highlight_.value();
        ring.x += hui::ui::shake(refusal_.value, (float)frame.time);
        paint.focus_ring(ring, 18.0f, 0.55f + 0.45f * hui::ui::breathe((float)frame.time, 1.8f));

        // ---- status log (right column) ----
        const Rect panel{748.0f, 336.0f, 560.0f, 650.0f};
        paint.panel(panel);
        paint.label("STATUS LOG", panel.x + 26.0f, panel.y + 46.0f, 20.0f,
                    paint.theme().text_muted);
        hui::ui::text(list, fonts.mono, running_view_ ? step_view_ : "idle", panel.x + 26.0f,
                      panel.y + 84.0f, 21.0f,
                      Color::rgb(running_view_ ? color::kWaiting : color::kMuted));

        const float line_y = panel.y + 128.0f;
        for (int i = 0; i < snapshot_count_; ++i)
        {
            const LogLine &line = snapshot_[i];
            const bool fresh = (i == snapshot_count_ - 1);
            const Color tint = line.error
                                   ? Color::rgb(color::kAlert)
                                   : (fresh ? Color::rgb(color::kInk) : Color::rgb(color::kMuted));
            hui::ui::text(list, fonts.mono, line.text, panel.x + 26.0f, line_y + (float)i * 34.0f,
                          21.0f, tint);
        }
        if (snapshot_count_ == 0)
        {
            hui::ui::text(list, fonts.mono, "(no steps yet)", panel.x + 26.0f, line_y, 21.0f,
                          Color::rgb(color::kMuted, 0.7f));
        }

        // ---- footer: the focused test's detail ----
        const Rect footer{88.0f, 986.0f, 620.0f, 56.0f};
        hui::ui::text(list, fonts.regular, kTests[focus_].detail, footer.x + 4.0f,
                      footer.cy() + 8.0f, 24.0f, Color::rgb(color::kMuted));

        draw_status_pill(frame, "INPUT TEST", state, hui::ui::breathe((float)frame.time));

        const hui::ui::Hint hints[] = {
            {hui::ui::Button::cross, "Run"},
            {hui::ui::Button::square, "Stop"},
            {hui::ui::Button::circle, "Back"},
        };
        draw_hints_row(frame, hints, 3);
    }

  private:
    static Rect row_rect(int index)
    {
        return {88.0f, 336.0f + (float)index * 62.0f, 620.0f, 58.0f};
    }

    void draw_row(hui::ui::Painter &paint, const Rect &r, int index, bool focused) const
    {
        const hui::ui::Look look{focused ? 1.0f : 0.0f, 0.0f, false};
        paint.panel(r);
        paint.body(kTests[index].label, r.x + 28.0f, r.cy() + 9.0f, 27.0f, paint.theme().text);
        const bool running_this = running_view_ && index == run_view_;
        paint.body(running_this ? "running" : "run", r.x + r.w - 28.0f, r.cy() + 9.0f, 24.0f,
                   running_this ? paint.theme().accent : paint.theme().text_muted,
                   gfx::Align::right);
        (void)look;
    }

    // ---- logging (worker + UI thread) -----------------------------------

    void log_line(const char *fmt, ...)
    {
        LogLine line;
        line.error = false;
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line.text, sizeof(line.text), fmt, args);
        va_end(args);
        push_line(line);
    }

    void log_error(const char *fmt, ...)
    {
        LogLine line;
        line.error = true;
        std::snprintf(line.text, sizeof(line.text), "error: ");
        const size_t used = std::strlen(line.text);
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line.text + used, sizeof(line.text) - used, fmt, args);
        va_end(args);
        push_line(line);
    }

    // A script step: shows in the log and in the panel header.
    void log_step(const char *fmt, ...)
    {
        LogLine line;
        line.error = false;
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line.text, sizeof(line.text), fmt, args);
        va_end(args);
        pthread_mutex_lock(&lock_);
        std::snprintf(step_, sizeof(step_), "%s", line.text);
        pthread_mutex_unlock(&lock_);
        push_line(line);
    }

    void push_line(const LogLine &line)
    {
        pthread_mutex_lock(&lock_);
        log_[log_head_] = line;
        log_head_ = (log_head_ + 1) % kLogCap;
        if (log_count_ < kLogCap)
            ++log_count_;
        pthread_mutex_unlock(&lock_);
    }

    // Copy the newest lines (and run state) out for the draw() thread.
    void snapshot()
    {
        pthread_mutex_lock(&lock_);
        running_view_ = running_.load();
        run_view_ = test_;
        std::snprintf(step_view_, sizeof(step_view_), "%s", step_);
        snapshot_count_ = 0;
        const int first = log_count_ <= kLogVisible ? 0 : log_count_ - kLogVisible;
        for (int i = first; i < log_count_; ++i)
        {
            const int slot = (log_head_ - log_count_ + i + kLogCap * 2) % kLogCap;
            snapshot_[snapshot_count_++] = log_[slot];
        }
        pthread_mutex_unlock(&lock_);
    }

    // ---- transport (worker thread only) ---------------------------------

    int send_pad(std::uint32_t buttons, std::int16_t lx, std::int16_t ly, std::int16_t rx,
                 std::int16_t ry, std::uint8_t l2, std::uint8_t r2)
    {
        VdPadReport report;
        vd_pad_make_report(&report, ++pad_id_, now_us(), buttons, lx, ly, rx, ry, l2, r2);
        if (!wire_checked_)
        {
            // Contract check on the first report of the run (byte-level
            // evidence, like payload/padtest.c).
            std::uint8_t wire[VD_PAD_WIRE_BYTES];
            const size_t n = vd_pad_serialize(&report, wire, sizeof(wire));
            if (n != VD_PAD_WIRE_BYTES)
            {
                log_error("pad wire format: %u bytes, expected %u", (unsigned)n,
                          (unsigned)VD_PAD_WIRE_BYTES);
                return -1;
            }
            wire_checked_ = true;
        }
        const int rc = usb_send_pad_report(fd_, nullptr, &report);
        if (rc != 0)
            log_error("pad send failed (rc=%d)", rc);
        return rc;
    }

    int send_touch(std::uint8_t port, std::uint8_t count, std::uint16_t x0, std::uint16_t y0,
                   std::uint8_t a0, std::uint16_t x1, std::uint16_t y1, std::uint8_t a1)
    {
        const VdTouchReport report = make_touch_report(port, count, x0, y0, a0, x1, y1, a1);
        if (!wire_checked_)
        {
            std::uint8_t wire[VD_TOUCH_WIRE_BYTES];
            const size_t n = vd_touch_serialize(&report, wire);
            if (n != VD_TOUCH_WIRE_BYTES)
            {
                log_error("touch wire format: %u bytes, expected %u", (unsigned)n,
                          (unsigned)VD_TOUCH_WIRE_BYTES);
                return -1;
            }
            wire_checked_ = true;
        }
        const int rc = usb_send_touch_report(fd_, &report);
        if (rc != 0)
            log_error("touch send failed (rc=%d)", rc);
        return rc;
    }

    // Cancellable pacing: false once a stop is asked for.
    bool wait_ms(unsigned ms)
    {
        unsigned left = ms;
        while (left > 0 && !stop_.load(std::memory_order_relaxed))
        {
            const unsigned slice = left < 25u ? left : 25u;
            struct timespec req;
            req.tv_sec = 0;
            req.tv_nsec = (long)slice * 1000000L;
            nanosleep(&req, nullptr);
            left -= slice;
        }
        return !stop_.load(std::memory_order_relaxed);
    }

    bool open_vita()
    {
        VdUsbVitaDevice dev;
        std::memset(&dev, 0, sizeof(dev));
        if (vd_usb_scan_vita(&dev, 1u) == 0)
        {
            log_error("no PS Vita UVC device found on /dev/ugen*");
            return false;
        }
        log_line("vita: %s (vid=0x%04x pid=0x%04x)", dev.path, dev.vid, dev.pid);
        fd_ = open(dev.path, O_RDWR | O_NONBLOCK);
        if (fd_ < 0)
        {
            log_error("open %s failed (%d)", dev.path, fd_);
            return false;
        }
        return true;
    }

    // ---- script primitives (the payload scripts' exact shapes) ----------

    // One pad report held ~`ms` at 10 Hz (reports latch: repeat to hold).
    bool pad_hold(const char *name, std::uint32_t buttons, std::int16_t lx, std::int16_t ly,
                  std::int16_t rx, std::int16_t ry, std::uint8_t l2, std::uint8_t r2, unsigned ms)
    {
        int reps = (int)((ms * kReportHz) / 1000u);
        if (reps < 1)
            reps = 1;
        for (int r = 0; r < reps; ++r)
        {
            if (stop_.load())
                return false;
            if (send_pad(buttons, lx, ly, rx, ry, l2, r2) != 0)
                return false;
            if (!wait_ms(kPadGapMs))
                return false;
        }
        (void)name;
        return true;
    }

    // touchtest.c hold_touch(): `steps` records at 10 Hz, then a release.
    bool touch_hold(const char *name, std::uint8_t port, int steps, std::uint16_t x0,
                    std::uint16_t y0, std::uint8_t a0, std::uint16_t x1, std::uint16_t y1,
                    std::uint8_t a1)
    {
        for (int i = 0; i < steps; ++i)
        {
            if (stop_.load())
                return false;
            if (send_touch(port, (std::uint8_t)(a0 + a1), x0, y0, a0, x1, y1, a1) != 0)
                return false;
            log_step("%s: hold %d/%d", name, i + 1, steps);
            if (!wait_ms(kTouchStepMs))
                return false;
        }
        return send_touch(port, 0, 0, 0, 0, 0, 0, 0) == 0 && wait_ms(kTouchStepMs);
    }

    // touchtest.c swipe(): finger travels x_from -> x_to at height y.
    bool touch_swipe_x(const char *name, std::uint8_t port, std::uint16_t y, std::uint16_t x_from,
                       std::uint16_t x_to, int steps)
    {
        for (int i = 0; i < steps; ++i)
        {
            if (stop_.load())
                return false;
            const std::uint16_t x =
                (std::uint16_t)(x_from + ((int)(x_to - x_from) * i) / (steps - 1));
            if (send_touch(port, 1, x, y, 1, 0, 0, 0) != 0)
                return false;
            log_step("%s: %d/%d", name, i + 1, steps);
            if (!wait_ms(kTouchStepMs))
                return false;
        }
        return send_touch(port, 0, 0, 0, 0, 0, 0, 0) == 0 && wait_ms(kTouchStepMs);
    }

    // touchswipe.c swipe(): a fast flick, x=960, y 880->180 (dir > 0) in 8
    // steps of 30 ms, release, 400 ms gap. Logs "up: swipe 3/10 sent".
    bool livearea_swipe(const char *batch, int dir, int index, int total)
    {
        const std::uint16_t y_from = dir > 0 ? kLiveAreaYBottom : kLiveAreaYTop;
        const std::uint16_t y_to = dir > 0 ? kLiveAreaYTop : kLiveAreaYBottom;
        for (int i = 0; i < kLiveAreaSteps; ++i)
        {
            if (stop_.load())
                return false;
            const std::uint16_t y =
                (std::uint16_t)(y_from + ((int)(y_to - y_from) * i) / (kLiveAreaSteps - 1));
            if (send_touch(kPortFront, 1, kLiveAreaX, y, 1, 0, 0, 0) != 0)
                return false;
            if (!wait_ms(kLiveAreaStepMs))
                return false;
        }
        if (send_touch(kPortFront, 0, 0, 0, 0, 0, 0, 0) != 0)
            return false;
        log_step("%s: swipe %d/%d sent", batch, index, total);
        return wait_ms(kLiveAreaGapMs);
    }

    // ---- the scripts ----------------------------------------------------

    bool script_button_tour()
    {
        for (int i = 0; i < kPadStepCount; ++i)
        {
            const PadStep &step = kPadSteps[i];
            log_step("pad step %d/%d: %s", i + 1, kPadStepCount, step.name);
            if (!pad_hold(step.name, step.buttons, step.lx, step.ly, step.rx, step.ry, step.l2,
                          step.r2, kPadHoldMs))
                return false;
        }
        return true;
    }

    bool script_front_tap_swipe()
    {
        return touch_hold("front tap", kPortFront, kTapSteps, kTapX, kTapY, 1, 0, 0, 0) &&
               touch_swipe_x("front swipe", kPortFront, kFrontSwipeY, kSwipeXFrom, kSwipeXTo,
                             kSwipeSteps);
    }

    bool script_two_finger_tap()
    {
        return touch_hold("two-finger tap", kPortFront, kTapSteps, kTwoFingerX0, kTapY, 1,
                          kTwoFingerX1, kTapY, 1);
    }

    bool script_rear_swipe()
    {
        return touch_swipe_x("rear swipe", kPortRear, kRearSwipeY, kSwipeXFrom, kSwipeXTo,
                             kSwipeSteps);
    }

    bool script_livearea_swipe()
    {
        const struct
        {
            const char *name;
            int dir;
        } batches[] = {{"up", 1}, {"down", -1}, {"up", 1}};
        for (int b = 0; b < 3; ++b)
        {
            log_step("batch %d: %s x%d", b + 1, batches[b].name, kLiveAreaSwipes);
            for (int i = 0; i < kLiveAreaSwipes; ++i)
            {
                if (!livearea_swipe(batches[b].name, batches[b].dir, i + 1, kLiveAreaSwipes))
                    return false;
            }
            if (b < 2 && !wait_ms(kLiveAreaBatchGapMs))
                return false;
        }
        return true;
    }

    bool script_neutral_reset()
    {
        log_step("neutral: releasing pad + touch");
        return true; // run_script() sends the neutral reports right after
    }

    // ---- worker ---------------------------------------------------------

    static void *worker_main(void *arg)
    {
        static_cast<InputTestScreen *>(arg)->run_script();
        return nullptr;
    }

    void run_script()
    {
        if (open_vita())
        {
            switch (test_)
            {
            case 0:
                script_button_tour();
                break;
            case 1:
                script_front_tap_swipe();
                break;
            case 2:
                script_two_finger_tap();
                break;
            case 3:
                script_rear_swipe();
                break;
            case 4:
                script_livearea_swipe();
                break;
            case 5:
            default:
                script_neutral_reset();
                break;
            }

            // ALWAYS leave the Vita neutral: released pad + released fingers.
            send_pad(0, 0, 0, 0, 0, 0, 0);
            send_touch(kPortFront, 0, 0, 0, 0, 0, 0, 0);
            send_touch(kPortRear, 0, 0, 0, 0, 0, 0, 0);
            log_line(stop_.load() ? "stopped: neutral pad + touch released"
                                  : "done: neutral pad + touch released");
            close(fd_);
            fd_ = -1;
        }
        running_.store(false);
    }

    void start_test(int index)
    {
        stop_and_join(); // reap a previous worker, if any
        test_ = index;
        pad_id_ = 0;
        wire_checked_ = false;
        stop_.store(false);
        running_.store(true);
        log_line("run: %s", kTests[index].label);
        if (pthread_create(&thread_, nullptr, &InputTestScreen::worker_main, this) != 0)
        {
            running_.store(false);
            thread_valid_ = false;
            log_error("worker thread did not start");
            return;
        }
        thread_valid_ = true;
    }

    void request_stop()
    {
        if (running_.load())
        {
            stop_.store(true);
            log_line("stop requested");
        }
    }

    void stop_and_join()
    {
        stop_.store(true);
        if (thread_valid_)
        {
            pthread_join(thread_, nullptr);
            thread_valid_ = false;
        }
        running_.store(false);
    }

    AppServices &services_;
    float age_ = 0.0f;
    int focus_ = 0;
    hui::ui::SpringRect highlight_;
    hui::ui::Pulse refusal_;

    // Worker state.
    pthread_mutex_t lock_{};
    pthread_t thread_{};
    bool thread_valid_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    int test_ = 0;
    int fd_ = -1;
    std::uint32_t pad_id_ = 0;
    bool wire_checked_ = false;

    // Status log (ring, mutex-guarded).
    LogLine log_[kLogCap] = {};
    int log_head_ = 0;
    int log_count_ = 0;
    char step_[kLogText] = "idle";

    // Snapshot the draw() thread reads without locks.
    LogLine snapshot_[kLogVisible] = {};
    int snapshot_count_ = 0;
    char step_view_[kLogText] = "idle";
    bool running_view_ = false;
    int run_view_ = 0;
};

} // namespace

std::unique_ptr<Screen> make_inputtest_screen(AppServices &services)
{
    return std::make_unique<InputTestScreen>(services);
}

} // namespace vd5
