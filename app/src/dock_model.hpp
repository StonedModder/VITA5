// VITA5 app — the live dock model: shared region, video, audio, pad.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One object that owns the attach/poll cycle against the kernel dock
// service and turns the rings into what the screens draw: a video texture,
// an audio stream, forwarded pad reports and honest status flags. Every
// flag is computed from the contract header — nothing is assumed to work.

#pragma once
#include "audio_pipe.hpp"
#include "vd/vd_shm.hpp"
#include "vd_settings.hpp"
#include "video_surface.hpp"

#include "core/input.hpp"

#include <cstdint>
#include <vector>

namespace vd5
{

// The three states every screen is designed around.
enum class DockState : std::uint8_t
{
    absent,   // nothing to talk to yet (no service, or no Vita)
    busy,     // a request is in flight (attach, or stream start)
    connected // streaming
};

class DockModel
{
  public:
    void init(VideoSurface *video, AudioPipe *audio);
    // Runs one update: attach polling, ring pumps, pad forwarding.
    // `sample` is the newest pad sample of the frame (may be null).
    void update(float dt, std::uint64_t now_us, const hui::PadSample *sample,
                const VdSettings &settings);

    // The user asked for a retry (Cross on the connect screen). While the
    // attempt runs the connect screen shows its request-in-flight state.
    void request_attach();
    bool attach_in_flight() const
    {
        return attach_timer_ > 0.0f;
    }
    // Service present, Vita seen, but the stream has not started yet.
    bool stream_pending() const
    {
        return shm_.attached() && shm_.vita_detected() && !shm_.stream_active();
    }

    DockState connect_state() const;
    DockState live_state() const;

    // ---- facts for the screens and the HUD ----
    bool service_present() const
    {
        return shm_.attached();
    }
    bool vita_detected() const
    {
        return shm_.attached() && shm_.vita_detected();
    }
    bool stream_active() const
    {
        return shm_.attached() && shm_.stream_active();
    }
    std::uint32_t service_error() const
    {
        return shm_.service_error();
    }
    const char *service_error_text() const;
    const Shm::Stats &ring_stats() const
    {
        return shm_.stats();
    }
    float audio_level() const;
    // The video stage facts for the live view.
    bool video_ready() const
    {
        return video_ != nullptr && video_->ready();
    }
    std::uint32_t video_texture() const
    {
        return video_ != nullptr ? video_->texture() : 0;
    }
    // Source NV12 geometry.
    int video_width() const
    {
        return video_width_;
    }
    int video_height() const
    {
        return video_height_;
    }
    // What the live view should letterbox (chain output when upscaling).
    int present_width() const
    {
        return video_ != nullptr ? video_->present_width() : video_width_;
    }
    int present_height() const
    {
        return video_ != nullptr ? video_->present_height() : video_height_;
    }
    // True once per freshly uploaded video frame (then clears): the live
    // view's pulse indicator tracks it.
    bool take_new_frame()
    {
        const bool fresh = have_new_frame_;
        have_new_frame_ = false;
        return fresh;
    }
    std::uint64_t pad_reports() const
    {
        return pad_reports_;
    }
    // Input capture: while ON every controller input goes to the Vita and
    // the app UI stays quiet; L3+R3 (stick clicks — unpressable on a Vita)
    // toggles it, so full screen / settings / dock status stay reachable.
    bool capture_active() const
    {
        return capture_active_;
    }
    // Touch records ride the shared header (used as the touch bridge's
    // transport sink).
    bool publish_touch(const VdTouchReport &report);
    // L3 hotkey events (consumed once by the app).
    int take_upscale_cycle()
    {
        const int cycle = upscale_cycle_;
        upscale_cycle_ = 0;
        return cycle;
    }
    bool take_fullscreen_toggle()
    {
        const bool toggle = fullscreen_toggle_;
        fullscreen_toggle_ = false;
        return toggle;
    }
    bool take_touch_target_toggle()
    {
        const bool toggle = touch_target_toggle_;
        touch_target_toggle_ = false;
        return toggle;
    }
    bool take_home_press()
    {
        const bool press = home_press_;
        home_press_ = false;
        return press;
    }
    double app_fps = 0.0; // presented frames per second (filled by the app)
    double video_fps = 0.0;

  private:
    void pump_video(UpscaleMode mode);
    void pump_audio();
    void publish_pad(std::uint64_t now_us, const hui::PadSample *sample);
    void publish_input(std::uint64_t now_us, const hui::PadSample *sample,
                       const VdSettings &settings);

    Shm shm_;
    VideoSurface *video_ = nullptr;
    AudioPipe *audio_ = nullptr;
    std::vector<std::uint8_t> nv12_;
    std::vector<std::uint8_t> rgba_;
    int video_width_ = 0;
    int video_height_ = 0;
    float attach_timer_ = 0.0f; // > 0 while an attach attempt is in flight
    float attach_retry_ = 0.0f; // seconds until the next automatic attempt
    std::uint32_t report_id_ = 0;
    std::uint64_t pad_reports_ = 0;
    bool have_new_frame_ = false;
    bool capture_active_ = true;
    bool capture_neutral_sent_ = true;
    std::uint32_t prev_buttons_ = 0;
    int upscale_cycle_ = 0;
    bool fullscreen_toggle_ = false;
    bool touch_target_toggle_ = false;
    bool home_press_ = false;
    // Frames remaining to hold VD_PAD_HOME after L3+touchpad (one-shot).
    int home_hold_frames_ = 0;
};

} // namespace vd5
