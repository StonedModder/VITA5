// VITA5 app — the live dock model. See dock_model.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dock_model.hpp"

#include "vd/nv12.hpp"
#include "vd/pad_bridge.hpp"
#include "vd/touch_bridge.hpp"

#include <cstdio>

namespace vd5
{

namespace
{
// The touch bridge's transport sink: records ride the shared header
// (latest-wins), consumed by the kernel pad forwarder. `fd` is unused —
// the sink signature matches usb_send_touch_report's for compatibility.
DockModel *g_touch_model = nullptr;

int touch_send_thunk(int fd, const VdTouchReport *report)
{
    (void)fd;
    return (g_touch_model != nullptr && report != nullptr && g_touch_model->publish_touch(*report))
               ? 0
               : -1;
}
} // namespace


namespace
{
constexpr float kAttachWindow = 0.7f;  // seconds an attach attempt stays in flight
constexpr float kRetryPeriod = 2.0f;   // seconds between automatic attach attempts
constexpr int kMaxAudioChunksPerFrame = 8;
} // namespace

void DockModel::init(VideoSurface *video, AudioPipe *audio)
{
    video_ = video;
    audio_ = audio;
    nv12_.assign(VD_VIDEO_MAX_FRAME, 0);
    rgba_.assign((std::size_t)1280 * 720 * 4, 0);
}

void DockModel::request_attach()
{
    if (attach_timer_ > 0.0f)
        return; // one attempt at a time
    attach_timer_ = kAttachWindow;
    attach_retry_ = kRetryPeriod;
}

DockState DockModel::connect_state() const
{
    if (stream_active())
        return DockState::connected;
    if (attach_in_flight() || stream_pending())
        return DockState::busy;
    return DockState::absent;
}

DockState DockModel::live_state() const
{
    if (stream_active())
        return DockState::connected;
    if (attach_in_flight() || stream_pending() || (service_present() && !vita_detected()))
        return DockState::busy;
    return DockState::absent;
}

const char *DockModel::service_error_text() const
{
    switch (shm_.service_error())
    {
        case VD_ERR_NONE:
            return "";
        case VD_ERR_NO_VITA:
            return "no Vita on the USB bus";
        case VD_ERR_PROBE_FAILED:
            return "the Vita refused the video probe";
        case VD_ERR_COMMIT_FAILED:
            return "the stream settings were not accepted";
        case VD_ERR_BULK_READ_FAILED:
            return "the USB video transfer failed";
        case VD_ERR_BAD_FRAME_HEADER:
            return "a video frame arrived malformed";
        case VD_ERR_VITA_DISCONNECTED:
            return "the Vita was unplugged";
        default:
            return "the dock service reported an error";
    }
}

float DockModel::audio_level() const
{
    return audio_ != nullptr ? audio_->level() : 0.0f;
}

void DockModel::update(float dt, std::uint64_t now_us, const hui::PadSample *sample,
                       const VdSettings &settings)
{
    // ---- attach polling: retry forever, patiently ----
    if (attach_timer_ > 0.0f)
    {
        attach_timer_ -= dt;
        if (attach_timer_ <= 0.0f)
        {
            attach_timer_ = 0.0f;
            if (!shm_.attached())
                shm_.attach(); // fails cleanly ("waiting for the dock service")
        }
    }
    else if (!shm_.attached())
    {
        attach_retry_ -= dt;
        if (attach_retry_ <= 0.0f)
        {
            attach_retry_ = kRetryPeriod;
            attach_timer_ = kAttachWindow;
        }
    }
    else if (settings.stream_audio)
    {
        // The ring is healthy only while the service is there; a region that
        // lost its magic means the service went away.
        if (!shm_.contract_ok())
            shm_.detach();
    }

    shm_.poll(dt);
    if (shm_.attached())
    {
        pump_video(settings.upscale);
        pump_audio();
        video_fps = shm_.stats().video_fps;
    }
    publish_input(now_us, sample, settings);
}

void DockModel::pump_video(UpscaleMode mode)
{
    if (video_ == nullptr)
        return;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (shm_.read_video(nv12_.data(), nv12_.size(), &width, &height))
    {
        if (width == 0 || height == 0 || width > 1280 || height > 720)
            return; // nonsense geometry: keep the last good picture
        video_width_ = (int)width;
        video_height_ = (int)height;
        if (!video_->ready() || video_->width() != video_width_ || video_->height() != video_height_)
        {
            if (!video_->create(video_width_, video_height_))
                return;
        }
        // The GPU chain (off by default) takes the raw NV12; when it is
        // unavailable the proven CPU path keeps the picture alive.
        if (vd_settings_upscale_active(mode))
        {
            const int factor = vd_settings_upscale_factor(mode);
            const int chain_mode = vd_settings_upscale_is_fsr(mode) ? 2 : 1;
            video_->process_nv12(nv12_.data(), chain_mode, factor);
        }
        else
        {
            video_->process_nv12(nv12_.data(), 0, 1); // clears chain_valid_
        }
        if (!video_->chain_ready())
        {
            const std::size_t needed = (std::size_t)video_width_ * video_height_ * 4;
            if (rgba_.size() < needed)
                rgba_.assign(needed, 0);
            nv12_to_rgba(nv12_.data(), video_width_, video_height_, rgba_.data());
            video_->upload(rgba_.data());
        }
        have_new_frame_ = true;
    }
}

void DockModel::pump_audio()
{
    if (audio_ == nullptr)
        return;
    std::uint8_t chunk[VD_AUDIO_CHUNK_BYTES];
    for (int i = 0; i < kMaxAudioChunksPerFrame; ++i)
    {
        if (!shm_.read_audio(chunk, sizeof(chunk)))
            break;
        audio_->feed(chunk, sizeof(chunk));
    }
}

void DockModel::publish_pad(std::uint64_t now_us, const hui::PadSample *sample)
{
    if (!shm_.attached() || sample == nullptr)
        return;
    VdPadReport report{};
    fill_report(&report, ++report_id_, now_us, sample->buttons, sample->left_x, sample->left_y,
                sample->right_x, sample->right_y, sample->l2, sample->r2);
    if (shm_.publish_pad(report))
        ++pad_reports_;
}

void DockModel::publish_input(std::uint64_t now_us, const hui::PadSample *sample,
                              const VdSettings &settings)
{
    // Install the touch bridge transport once: touch records ride the shared
    // header (latest-wins), consumed by the kernel pad forwarder.
    static bool transport_installed = false;
    if (!transport_installed)
    {
        transport_installed = true;
        g_touch_model = this;
        touch_bridge_set_transport(0, &touch_send_thunk);
    }

    // L3 chord family (the app's hotkeys; a Vita has no stick clicks):
    // L3+R3 toggles input capture, L3+D-Pad LEFT/RIGHT cycles the upscale
    // mode, L3+D-Pad UP toggles full screen, L3+D-Pad DOWN swaps touch
    // target, L3+touchpad click fires a one-shot Vita HOME.
    if (sample != nullptr)
    {
        const ChordEvents events = chord_events(sample->buttons, prev_buttons_);
        if (events.toggle_capture)
            capture_active_ = !capture_active_;
        if (events.upscale_cycle != 0)
            upscale_cycle_ += events.upscale_cycle;
        if (events.toggle_fullscreen)
            fullscreen_toggle_ = true;
        if (events.toggle_touch_target)
            touch_target_toggle_ = true;
        if (events.press_home)
        {
            home_press_ = true;
            // Hold HOME for a few frames so the Vita sees a real press, not
            // a single-sample blip the plugin might drop.
            home_hold_frames_ = 6;
        }
        prev_buttons_ = sample->buttons;
    }

    const bool forward = settings.input_passthrough && capture_active_ && sample != nullptr;
    touch_bridge_set_port(vd_settings_touch_target(settings) == TouchTarget::front
                              ? kTouchPortFront
                              : kTouchPortRear);
    touch_bridge_set_enabled(forward);
    if (forward)
    {
        capture_neutral_sent_ = false;
        // While L3 is held the chord buttons belong to the app, never to the
        // Vita: mask L3/R3/D-pad/touchpad out of the forwarded report.
        hui::PadSample masked = *sample;
        masked.buttons = mask_modifiers(sample->buttons);
        if (home_hold_frames_ > 0)
        {
            // Inject Vita HOME via the mapped report path: set the scePad
            // bit that map_pad_buttons does not cover by OR-ing after map
            // through fill_report's buttons argument — easiest is to build
            // the report ourselves once HOME is active.
            VdPadReport report{};
            fill_report(&report, ++report_id_, now_us, masked.buttons, masked.left_x, masked.left_y,
                        masked.right_x, masked.right_y, masked.l2, masked.r2);
            report.buttons |= VD_PAD_HOME;
            if (shm_.publish_pad(report))
                ++pad_reports_;
            --home_hold_frames_;
        }
        else
        {
            publish_pad(now_us, &masked);
        }
        vd5::TouchPadSample touch{};
        for (int i = 0; i < 2; ++i)
        {
            touch.x[i] = sample->touch_x[i];
            touch.y[i] = sample->touch_y[i];
            touch.finger[i] = sample->touch_finger[i];
        }
        touch_bridge_update(touch, now_us);
    }
    else if (!capture_neutral_sent_)
    {
        // Leaving capture (or disabling passthrough): exactly one neutral
        // report so nothing stays held on the Vita.
        const hui::PadSample neutral{};
        publish_pad(now_us, &neutral);
        capture_neutral_sent_ = true;
        home_hold_frames_ = 0;
    }
}

bool DockModel::publish_touch(const VdTouchReport &report)
{
    return shm_.publish_touch(report);
}

} // namespace vd5
