// VITA5 app — the connection / waiting screen.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Three designed states:
//   absent    no dock service and no Vita: say exactly what to do, with the
//             numbered steps and a retry action
//   busy      a request is in flight: attach attempt, or stream start
//   connected the Vita is streaming: confirm it and move to the live view
//
// The hero element is a drawn handheld silhouette with signal arcs that
// breathe while waiting and lock on when the stream is live.

#include "screens.hpp"

#include "core/tween.hpp"

#include <cstdio>

namespace vd5
{

namespace
{

using gfx::Color;
using gfx::Rect;

class ConnectScreen final : public Screen
{
  public:
    explicit ConnectScreen(AppServices &services) : services_(services) {}

    const char *name() const override
    {
        return "connect";
    }

    void enter() override
    {
        age_ = 0.0f;
        announced_ = false;
        live_hold_ = 0.0f;
    }

    void update(const hui::InputFrame &input, float dt, Feedback &feedback) override
    {
        age_ += dt;
        const DockState state = services_.dock.connect_state();
        state_mix_ = state == DockState::connected   ? 1.0f
                     : state == DockState::busy      ? 0.5f
                                                     : 0.0f;
        state_mix_smoothed_ += (state_mix_ - state_mix_smoothed_) * dt * 4.0f;

        if (state == DockState::connected && !announced_)
        {
            announced_ = true;
            feedback.play(hui::audio::Cue::connect);
        }
        if (state == DockState::connected)
        {
            live_hold_ += dt;
            if (live_hold_ > 1.4f)
                services_.go_live = true;
        }
        else
        {
            live_hold_ = 0.0f;
        }

        if (input.is_pressed(Action::confirm))
        {
            if (state == DockState::connected)
            {
                services_.go_live = true;
            }
            else
            {
                services_.retry_attach = true;
                feedback.play(hui::audio::Cue::select);
            }
        }
        if (input.is_pressed(Action::menu))
        {
            services_.go_settings = true;
            feedback.play(hui::audio::Cue::open);
        }
    }

    void draw(Frame &frame) const override
    {
        fill_backdrop(frame, hui::gfx::BackdropMode::aurora);
        hui::gfx::DrawList &list = frame.scene;
        const hui::ui::Fonts &fonts = frame.fonts;
        const DockState state = services_.dock.connect_state();

        draw_brand(frame, hui::tween::clamp01(age_ * 2.0f));

        // ---- hero: the handheld silhouette with signal arcs ----
        const float hero_cx = 560.0f;
        const float hero_cy = 540.0f;
        draw_handheld(list, hero_cx, hero_cy, state);

        // ---- right column: title, state line, steps or progress ----
        const float x = 980.0f;
        const float width = 780.0f;
        float y = 250.0f;

        const float title_in = hui::tween::stagger(age_, 0, 0.05f, 0.6f);
        list.push_opacity(title_in);
        hui::ui::text(list, fonts.display, "Dock your PS Vita", x, y, 64.0f,
                      Color::rgb(color::kInk));
        list.pop_opacity();
        y += 54.0f;

        const char *status_line = "";
        Color status_color = Color::rgb(color::kMuted);
        switch (state)
        {
            case DockState::connected:
                status_line = "Vita detected - stream is live";
                status_color = Color::rgb(color::kSignal);
                break;
            case DockState::busy:
                status_line = services_.dock.stream_pending() ? "Starting the video stream"
                                                              : "Contacting the dock service";
                status_color = Color::rgb(color::kWaiting);
                break;
            case DockState::absent:
            default:
                status_line = services_.dock.service_present()
                                  ? "Waiting for the Vita on USB"
                                  : "Waiting for the dock service";
                status_color = Color::rgb(color::kWaiting);
                break;
        }
        const float status_in = hui::tween::stagger(age_, 1, 0.05f, 0.6f);
        list.push_opacity(status_in);
        list.glow({x - 12.0f, y - 12.0f, 14.0f, 14.0f}, 7, 16,
                  status_color.with_alpha(0.4f + 0.3f * hui::ui::breathe((float)frame.time)));
        list.circle(x - 5.0f, y - 5.0f, 7.0f, status_color);
        hui::ui::text(list, fonts.semibold, status_line, x + 24.0f, y + 4.0f, 30.0f,
                      status_color);
        list.pop_opacity();
        y += 70.0f;

        if (state == DockState::absent)
        {
            draw_step_row(list, fonts, 1, "Plug the Vita in over USB",
                          "Use a data cable; the console sees the Vita as a camera.", x, y,
                          width, hui::tween::stagger(age_, 2, 0.09f, 0.7f));
            y += 108.0f;
            draw_step_row(list, fonts, 2, "Enable USB streaming on the Vita",
                          "The udcd_uvc plugin starts the video and audio stream.", x, y, width,
                          hui::tween::stagger(age_, 3, 0.09f, 0.7f));
            y += 108.0f;
            draw_step_row(list, fonts, 3, "Wait for the handshake",
                          "The dock service probes the stream and this screen updates.", x, y,
                          width, hui::tween::stagger(age_, 4, 0.09f, 0.7f));
            y += 128.0f;

            if (services_.dock.service_present() && services_.dock.service_error() != VD_ERR_NONE)
            {
                list.rounded_rect({x, y, width, 62.0f}, 16.0f,
                                  Color::rgb(color::kAlert, 0.14f));
                char error_line[160];
                std::snprintf(error_line, sizeof(error_line), "Last dock error: %s",
                              services_.dock.service_error_text());
                hui::ui::text(list, fonts.regular, error_line, x + 24.0f, y + 40.0f, 24.0f,
                              Color::rgb(color::kAlert));
            }
        }
        else if (state == DockState::busy)
        {
            // Request in flight: spinner + honest line + what is being waited on.
            list.rounded_rect({x, y, width, 190.0f}, 22.0f, Color::rgb(color::kPanel, 0.75f));
            draw_spinner(list, x + 74.0f, y + 95.0f, 30.0f, Color::rgb(color::kWaiting),
                         frame.time, 7.0f);
            hui::ui::text(list, fonts.semibold,
                          services_.dock.stream_pending() ? "Starting the stream"
                                                          : "Contacting the dock service",
                          x + 134.0f, y + 82.0f, 30.0f, Color::rgb(color::kInk));
            hui::ui::text(list, fonts.regular,
                          services_.dock.stream_pending()
                              ? "The Vita is connected; waiting for video frames."
                              : "Mapping the shared region and checking the contract.",
                          x + 134.0f, y + 124.0f, 23.0f, Color::rgb(color::kMuted));
            // Determinate-looking activity: bars that shimmer in sequence.
            for (int i = 0; i < 5; ++i)
            {
                const float phase = hui::ui::breathe((float)frame.time + i * 0.22f, 1.1f);
                list.rounded_rect({x + 134.0f + i * 56.0f, y + 146.0f, 42.0f, 8.0f}, 4.0f,
                                  Color::rgb(color::kWaiting, 0.25f + 0.55f * phase));
            }
        }
        else
        {
            // Connected: a confirmation card, then the live view takes over.
            list.rounded_rect({x, y, width, 190.0f}, 22.0f, Color::rgb(color::kPanel, 0.75f));
            list.glow({x, y, width, 190.0f}, 22.0f, 26.0f,
                      Color::rgb(color::kSignal, 0.16f + 0.1f * hui::ui::breathe((float)frame.time)));
            hui::ui::text(list, fonts.semibold, "Stream established", x + 40.0f, y + 82.0f,
                          32.0f, Color::rgb(color::kSignal));
            hui::ui::text(list, fonts.regular, "NV12 960x544 video, 48 kHz stereo audio",
                          x + 40.0f, y + 126.0f, 24.0f, Color::rgb(color::kMuted));
            hui::ui::text(list, fonts.regular, "Opening the live view", x + 40.0f, y + 162.0f,
                          23.0f, Color::rgb(color::kInk));
        }

        draw_status_pill(frame, status_pill_label(state),
                         state, hui::ui::breathe((float)frame.time));

        const hui::ui::Hint hints[] = {
            {state == DockState::connected ? hui::ui::Button::cross : hui::ui::Button::cross,
             state == DockState::connected ? "Live view" : "Retry"},
            {hui::ui::Button::options, "Settings"},
        };
        draw_hints_row(frame, hints, 2);
    }

  private:
    static const char *status_pill_label(DockState state)
    {
        switch (state)
        {
            case DockState::connected:
                return "STREAM LIVE";
            case DockState::busy:
                return "CONNECTING";
            case DockState::absent:
            default:
                return "NO VITA";
        }
    }

    void draw_handheld(hui::gfx::DrawList &list, float cx, float cy, DockState state) const
    {
        const Color body = Color::rgb(0x17223c);
        const Color edge = Color::rgb(0x2b3f66);
        const Color screen = Color::rgb(state == DockState::connected ? 0x0d3b3a : 0x0b1427);
        const float bob = 8.0f * hui::ui::breathe((float)services_.time, 3.2f);

        // Signal arcs: they breathe while waiting and settle when connected.
        const int arcs = 3;
        for (int i = 0; i < arcs; ++i)
        {
            const float phase =
                hui::ui::breathe((float)services_.time - i * 0.35f, 2.0f);
            const float radius = 220.0f + i * 66.0f + phase * 18.0f;
            const float alpha =
                state == DockState::connected ? 0.42f - i * 0.1f : 0.30f * (1.0f - phase) + 0.06f;
            list.arc(cx, cy + bob, radius, 7.0f, 5.0f + i * 0.25f, 2.1f,
                     Color::rgb(color::kSignal, alpha), true);
            list.arc(cx, cy + bob, radius, 7.0f, 11.2f + i * 0.25f, 2.1f,
                     Color::rgb(color::kSignal, alpha), true);
        }

        // The handheld: body, screen inset, sticks and buttons.
        const Rect body_rect{cx - 150.0f, cy - 220.0f + bob, 300.0f, 440.0f};
        list.shadow(body_rect, 34.0f, 46.0f, Color::rgb(0x000000, 0.45f));
        list.rounded_rect(body_rect, 34.0f, body);
        list.bordered_rect(body_rect, 34.0f, Color(0, 0, 0, 0), 2.5f, edge);
        const Rect screen_rect{cx - 110.0f, cy - 170.0f + bob, 220.0f, 220.0f};
        list.rounded_rect(screen_rect, 14.0f, screen);
        if (state == DockState::connected)
        {
            // A live screen: scrolling scan shimmer across the display.
            const float sweep = (float)(services_.time - (int)services_.time);
            list.rounded_rect({screen_rect.x, screen_rect.y + sweep * screen_rect.h,
                               screen_rect.w, 26.0f},
                              12.0f, Color::rgb(color::kSignal, 0.18f));
        }
        else
        {
            draw_spinner(list, screen_rect.cx(), screen_rect.cy(), 34.0f,
                         Color::rgb(color::kSignal, 0.65f), services_.time * 0.8, 6.0f);
        }
        // Sticks and face buttons.
        list.circle(cx - 74.0f, cy + 128.0f + bob, 30.0f, Color::rgb(0x223151));
        list.circle(cx + 74.0f, cy + 128.0f + bob, 30.0f, Color::rgb(0x223151));
        const float button_y = cy + 20.0f + bob;
        list.circle(cx + 84.0f, button_y - 34.0f, 15.0f, Color::rgb(0x2c3f66));
        list.circle(cx + 84.0f, button_y + 34.0f, 15.0f, Color::rgb(0x2c3f66));
        list.circle(cx + 50.0f, button_y, 15.0f, Color::rgb(0x2c3f66));
        list.circle(cx + 118.0f, button_y, 15.0f, Color::rgb(0x2c3f66));
    }

    AppServices &services_;
    float age_ = 0.0f;
    float state_mix_ = 0.0f;
    float state_mix_smoothed_ = 0.0f;
    float live_hold_ = 0.0f;
    bool announced_ = false;
};

} // namespace

std::unique_ptr<Screen> make_connect_screen(AppServices &services)
{
    return std::make_unique<ConnectScreen>(services);
}

} // namespace vd5
