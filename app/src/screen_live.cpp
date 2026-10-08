// VITA5 app — the live view: video, audio and input status overlay.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Three designed states:
//   absent    no signal: an animated placeholder with what to do about it
//   busy      the stream is starting: waiting for the first frame
//   connected the Vita's screen, scaled to the output, with a HUD
//
// The video path is real: shared ring -> NV12 -> RGBA -> GL texture. When
// the dock service is not there, the same code draws the states above and
// no picture is invented.
//
// The picture has two layouts, both animated (fullscreen.hpp): the HUD
// view fits the picture into its stage, full screen fits it to the whole
// canvas and hides the chrome. Cross toggles between them, Circle leaves
// full screen, and a fading hint row keeps the way out discoverable.

#include "screens.hpp"

#include "core/tween.hpp"
#include "fullscreen.hpp"

#include <cstdio>

namespace vd5
{

namespace
{
using gfx::Color;
using gfx::Rect;

// The video stage: fixed, the picture letterboxes inside it.
const Rect kStage{120.0f, 150.0f, 1680.0f, 790.0f};

class LiveScreen final : public Screen
{
  public:
    explicit LiveScreen(AppServices &services) : services_(services)
    {
    }

    const char *name() const override
    {
        return "live";
    }

    void enter() override
    {
        age_ = 0.0f;
    }

    void update(const hui::InputFrame &input, float dt, Feedback &feedback) override
    {
        age_ += dt;
        if (services_.dock.take_new_frame())
        {
            frame_pulse_.trigger(1.0f);
            if (!got_first_frame_)
            {
                got_first_frame_ = true;
                feedback.play(hui::audio::Cue::resume);
            }
        }
        frame_pulse_.update(dt, 6.0f);
        if (!services_.dock.stream_active())
            got_first_frame_ = false;

        // The picture went away (the stream dropped): fall back to the HUD
        // view so the status states are readable again.
        if (fullscreen_.active() && !video_showing())
            fullscreen_.exit();
        fullscreen_.update(dt, input.pressed != 0);

        // ---- L3 hotkeys: work in both modes, straight from the dock
        // model's chord events (the Vita can never produce stick clicks).
        const int cycle = services_.dock.take_upscale_cycle();
        if (cycle != 0)
        {
            services_.settings.upscale =
                vd5::vd_settings_cycle_upscale(services_.settings.upscale, cycle);
            services_.settings_changed = true;
            services_.toast_text = vd5::vd_settings_upscale_label(services_.settings.upscale);
            services_.toast_time = 1.6f;
        }
        if (services_.dock.take_fullscreen_toggle())
        {
            fullscreen_.toggle();
            feedback.play(fullscreen_.active() ? hui::audio::Cue::open : hui::audio::Cue::back);
            services_.toast_text = fullscreen_.active() ? "Full screen" : "Windowed";
            services_.toast_time = 1.2f;
        }
        if (services_.dock.take_touch_target_toggle())
        {
            VdSettings &s = services_.settings;
            s.touch_target =
                s.touch_target == TouchTarget::front ? TouchTarget::rear : TouchTarget::front;
            services_.settings_changed = true;
            services_.toast_text =
                s.touch_target == TouchTarget::front ? "Front touchscreen" : "Rear touchpad";
            services_.toast_time = 1.4f;
            feedback.play(hui::audio::Cue::toggle);
        }
        if (services_.dock.take_home_press())
        {
            services_.toast_text = "Vita HOME";
            services_.toast_time = 1.0f;
            feedback.play(hui::audio::Cue::notify);
        }
        if (services_.toast_time > 0.0f)
            services_.toast_time -= dt;

        // While input capture is on, every button goes to the Vita and this
        // screen stays quiet — the L3 chord family (handled by the dock
        // model) is the way out. In app mode the normal shortcuts work again.
        if (!services_.dock.capture_active())
        {
            if (input.is_pressed(Action::confirm) && video_showing())
            {
                fullscreen_.toggle();
                feedback.play(fullscreen_.active() ? hui::audio::Cue::open : hui::audio::Cue::back);
            }
            else if (input.is_pressed(Action::back))
            {
                if (fullscreen_.active())
                {
                    fullscreen_.exit();
                    feedback.play(hui::audio::Cue::back);
                }
                else
                {
                    services_.go_connect = true;
                    feedback.play(hui::audio::Cue::back);
                }
            }
            if (input.is_pressed(Action::menu))
            {
                services_.go_settings = true;
                feedback.play(hui::audio::Cue::open);
            }
        }
    }

    void draw(Frame &frame) const override
    {
        fill_backdrop(frame, hui::gfx::BackdropMode::aurora);
        hui::gfx::DrawList &list = frame.scene;
        const hui::ui::Fonts &fonts = frame.fonts;
        const DockState state = services_.dock.live_state();
        const bool video = video_showing();
        const float zoom = fullscreen_.zoom();
        const float chrome = fullscreen_.hud_alpha();

        // Full screen: the letterbox behind the picture goes black and
        // hides the aurora as the picture grows into it.
        if (zoom > 0.004f)
            list.rounded_rect({0.0f, 0.0f, Fullscreen::kScreenW, Fullscreen::kScreenH}, 8.0f,
                              Color::rgb(0x000000, 0.94f * zoom));

        if (chrome > 0.004f)
        {
            list.push_opacity(chrome);
            draw_brand(frame, hui::tween::clamp01(age_ * 2.0f));

            // ---- the video stage (fixed; the picture letterboxes inside) ----
            list.shadow(kStage, 30.0f, 50.0f, Color::rgb(0x000000, 0.5f));
            list.rounded_rect(kStage, 26.0f, Color::rgb(color::kDeep, 0.92f));
            if (!video)
            {
                // No frame from the ring yet: keep the designed states and
                // invent no picture.
                if (state == DockState::absent)
                    draw_no_signal(list, fonts, kStage, frame);
                else
                    draw_waiting(list, fonts, kStage, frame.time);
            }
            list.pop_opacity();
        }

        // ---- the picture itself: the newest frame off the shared ring ----
        if (video)
        {
            const Rect picture = fullscreen_.video_rect(kStage, services_.dock.present_width(),
                                                        services_.dock.present_height());
            const float radius = fullscreen_.corner_radius();
            if (services_.dock.video_texture() != 0)
                list.image(services_.dock.video_texture(), picture, gfx::kFullUv,
                           Color::rgb(0xffffff), radius);
            // A bright edge pulse with every delivered frame keeps the
            // picture feeling live even on a still frame.
            list.bordered_rect(
                picture, radius, Color(0, 0, 0, 0), 2.0f,
                Color::rgb(color::kSignal, (0.10f + 0.22f * frame_pulse_.value) * chrome));
        }

        if (chrome > 0.004f)
        {
            list.push_opacity(chrome);
            if (video)
                draw_hud(frame, kStage);
            draw_status_pill(frame, status_pill_label(state), state,
                             hui::ui::breathe((float)frame.time));
            if (video && !services_.dock.capture_active())
            {
                const hui::ui::Hint hints[] = {
                    {hui::ui::Button::cross, "Full screen"},
                    {hui::ui::Button::options, "Settings"},
                    {hui::ui::Button::circle, "Dock status"},
                    {hui::ui::Button::left_stick, "L3+R3: back to the Vita"},
                };
                draw_hints_row(frame, hints, 4);
            }
            else if (video)
            {
                const hui::ui::Hint hints[] = {
                    {hui::ui::Button::left_stick, "L3+R3: app menu"},
                };
                draw_hints_row(frame, hints, 1);
            }
            else
            {
                const hui::ui::Hint hints[] = {
                    {hui::ui::Button::options, "Settings"},
                    {hui::ui::Button::circle, "Dock status"},
                };
                draw_hints_row(frame, hints, 2);
            }
            list.pop_opacity();
        }

        // ---- the full-screen exit affordance (fades while the player idles)
        const float hint = fullscreen_.hint_alpha() * zoom;
        if (video && !services_.dock.capture_active() && hint > 0.004f)
        {
            list.push_opacity(hint);
            const hui::ui::Hint hints[] = {
                {hui::ui::Button::circle, "Exit full screen"},
                {hui::ui::Button::options, "Settings"},
            };
            draw_hints_row(frame, hints, 2);
            list.pop_opacity();
        }
    }

  private:
    // The shared ring delivered a picture the view can show right now.
    bool video_showing() const
    {
        return services_.dock.live_state() == DockState::connected &&
               services_.dock.video_ready() && services_.dock.video_texture() != 0;
    }

    static const char *status_pill_label(DockState state)
    {
        switch (state)
        {
        case DockState::connected:
            return "LIVE";
        case DockState::busy:
            return "STARTING";
        case DockState::absent:
        default:
            return "NO SIGNAL";
        }
    }

    void draw_waiting(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Rect &stage,
                      double time) const
    {
        draw_spinner(list, stage.cx(), stage.cy() - 60.0f, 44.0f, Color::rgb(color::kWaiting), time,
                     8.0f);
        hui::ui::text(list, fonts.semibold, "Starting the stream", stage.cx(), stage.cy() + 46.0f,
                      34.0f, Color::rgb(color::kInk), gfx::Align::center);
        hui::ui::text(list, fonts.regular, "The Vita is connected; waiting for video frames.",
                      stage.cx(), stage.cy() + 92.0f, 25.0f, Color::rgb(color::kMuted),
                      gfx::Align::center);
    }

    void draw_no_signal(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Rect &stage,
                        Frame &frame) const
    {
        // An animated scan placeholder: a sweep line and soft grid, so the
        // stage is alive even with nothing to show.
        const float sweep = (float)(frame.time * 0.35 - (int)(frame.time * 0.35));
        list.rounded_rect(
            {stage.x + 8.0f, stage.y + 8.0f + sweep * (stage.h - 40.0f), stage.w - 16.0f, 28.0f},
            12.0f, Color::rgb(color::kSignal, 0.07f));
        for (int i = 1; i < 6; ++i)
        {
            const float gx = stage.x + stage.w * (float)i / 6.0f;
            list.line(gx, stage.y + 12.0f, gx, stage.y + stage.h - 12.0f, 1.5f,
                      Color::rgb(0x1c2c4c, 0.55f));
        }

        hui::ui::text(list, fonts.display, "No signal", stage.cx(), stage.cy() - 96.0f, 58.0f,
                      Color::rgb(color::kInk), gfx::Align::center);
        const char *why = services_.dock.service_present()
                              ? "The dock service is running but no video is arriving."
                              : "The dock service is not reachable right now.";
        hui::ui::text(list, fonts.regular, why, stage.cx(), stage.cy() - 40.0f, 26.0f,
                      Color::rgb(color::kMuted), gfx::Align::center);

        const float y = stage.cy() + 24.0f;
        const float width = 900.0f;
        const float x = stage.cx() - width * 0.5f;
        draw_step_row(list, fonts, 1, "Connect the Vita over USB",
                      "The dock service watches the USB bus for the camera function.", x, y, width,
                      hui::tween::stagger(age_, 0, 0.1f, 0.6f));
        draw_step_row(list, fonts, 2, "Start USB streaming on the Vita",
                      "Frames arrive as NV12 960x544 at up to 60 fps.", x, y + 108.0f, width,
                      hui::tween::stagger(age_, 1, 0.1f, 0.6f));
    }

    void draw_hud(Frame &frame, const Rect &stage) const
    {
        if (!services_.settings.show_hud)
            return;
        hui::gfx::DrawList &list = frame.scene;
        const hui::ui::Fonts &fonts = frame.fonts;

        // Top overlay bar over the video.
        const Rect bar{stage.x + 18.0f, stage.y + 18.0f, stage.w - 36.0f, 64.0f};
        list.rounded_rect(bar, 18.0f, Color::rgb(color::kDeep, 0.55f));

        const float pulse = 0.45f + 0.55f * hui::ui::breathe((float)frame.time, 1.2f);
        list.circle(bar.x + 34.0f, bar.cy(), 9.0f, Color::rgb(color::kAlert, pulse));
        hui::ui::text(list, fonts.semibold, "LIVE", bar.x + 56.0f, bar.cy() + 9.0f, 26.0f,
                      Color::rgb(color::kInk));

        char readout[160];
        std::snprintf(readout, sizeof(readout), "NV12 %dx%d  |  %2.0f fps in  |  %2.0f fps out",
                      services_.dock.video_width(), services_.dock.video_height(),
                      services_.dock.video_fps, services_.app_fps);
        hui::ui::text(list, fonts.mono, readout, bar.cx(), bar.cy() + 8.0f, 24.0f,
                      Color::rgb(color::kMuted), gfx::Align::center);

        // Audio meter: bars driven by the real chunk peak, with a gentle
        // idle shimmer so the meter never looks frozen.
        const float level = services_.dock.audio_level();
        const float meter_x = bar.x + bar.w - 260.0f;
        for (int i = 0; i < 12; ++i)
        {
            const float idle = 0.12f + 0.12f * hui::ui::breathe((float)frame.time + i * 0.23f);
            const float value = level > idle ? level : idle;
            const float h = 8.0f + value * 36.0f * (0.6f + 0.4f * (float)(i % 3) / 2.0f);
            list.rounded_rect({meter_x + i * 17.0f, bar.cy() + 22.0f - h, 11.0f, h}, 4.0f,
                              Color::rgb(color::kSignal, 0.35f + 0.6f * value));
        }
        const char *audio_label = services_.settings.stream_audio ? "audio 48 kHz" : "audio muted";
        hui::ui::text(list, fonts.mono, audio_label, bar.x + bar.w - 22.0f, bar.cy() + 8.0f, 22.0f,
                      Color::rgb(color::kMuted), gfx::Align::right);

        // ---- transient toast (L3 hotkey feedback) ----
        if (services_.toast_time > 0.004f && services_.toast_text != nullptr)
        {
            const float alpha = hui::tween::clamp01(services_.toast_time / 0.35f);
            const float w = 320.0f;
            const Rect pill{stage.cx() - w * 0.5f, stage.y - 96.0f, w, 68.0f};
            list.rounded_rect(pill, 26.0f, Color::rgb(0x000000, 0.62f * alpha));
            hui::ui::text(list, fonts.semibold, services_.toast_text, pill.cx(), pill.cy() - 11.0f,
                          22.0f, Color::rgb(0xffffff, 1.0f * alpha), gfx::Align::center);
        }

        // Bottom overlay: input passthrough status.
        const Rect foot{stage.x + 18.0f, stage.y + stage.h - 78.0f, stage.w - 36.0f, 58.0f};
        list.rounded_rect(foot, 16.0f, Color::rgb(color::kDeep, 0.5f));
        char input_line[128];
        std::snprintf(input_line, sizeof(input_line), "input: %llu reports forwarded",
                      (unsigned long long)services_.dock.pad_reports());
        hui::ui::text(list, fonts.mono, input_line, foot.x + 22.0f, foot.cy() + 8.0f, 23.0f,
                      services_.pad_connected ? Color::rgb(color::kSignal)
                                              : Color::rgb(color::kWaiting));
        char dropped[64];
        std::snprintf(dropped, sizeof(dropped), "dropped: %llu",
                      (unsigned long long)services_.dock.ring_stats().dropped);
        hui::ui::text(list, fonts.mono, dropped, foot.x + foot.w - 22.0f, foot.cy() + 8.0f, 23.0f,
                      Color::rgb(color::kMuted), gfx::Align::right);
    }

    AppServices &services_;
    Fullscreen fullscreen_;
    float age_ = 0.0f;
    mutable hui::ui::Pulse frame_pulse_;
    bool got_first_frame_ = false;
};

} // namespace

std::unique_ptr<Screen> make_live_screen(AppServices &services)
{
    return std::make_unique<LiveScreen>(services);
}

} // namespace vd5
