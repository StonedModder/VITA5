// VITA5 app — the loading transition screen.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shown between launch and the connect screen: the wordmark assembles
// letter by letter over the animated backdrop while the display, fonts and
// audio thread come up, then hands over. This is the "loading/splash
// transition between screens" and it is also where the app waits out the
// system splash picture.

#include "screens.hpp"

#include "core/tween.hpp"

#include <cstdio>

namespace vd5
{

namespace
{

using gfx::Color;
using gfx::Rect;

class SplashScreen final : public Screen
{
  public:
    explicit SplashScreen(AppServices &services) : services_(services) {}

    const char *name() const override
    {
        return "splash";
    }

    void enter() override
    {
        age_ = 0.0f;
    }

    void update(const hui::InputFrame &input, float dt, Feedback &feedback) override
    {
        age_ += dt;
        if (age_ > kDuration && !done_)
        {
            done_ = true;
            feedback.play(hui::audio::Cue::welcome);
            services_.go_connect = true;
        }
        // A press skips the wait (the transition is loading, not a gate).
        if (!done_ && input.pressed != 0 && age_ > 0.6f)
            age_ = kDuration;
    }

    void draw(Frame &frame) const override
    {
        fill_backdrop(frame, hui::gfx::BackdropMode::grid);

        hui::gfx::DrawList &list = frame.scene;
        const hui::ui::Fonts &fonts = frame.fonts;
        const float cx = 960.0f;
        const float cy = 500.0f;

        // The wordmark assembles: each glyph eases up into place.
        const char *word = "VITA5";
        const float size = 118.0f;
        float total = 0.0f;
        for (const char *c = word; *c != '\0'; ++c)
        {
            char one[2] = {*c, '\0'};
            total += fonts.display.measure(one, size);
        }
        float x = cx - total * 0.5f;
        int index = 0;
        for (const char *c = word; *c != '\0'; ++c, ++index)
        {
            const float in = hui::tween::stagger(age_, index, 0.055f, 0.5f);
            char one[2] = {*c, '\0'};
            const Color color =
                index < 4 ? Color::rgb(color::kInk) : Color::rgb(color::kSignal);
            const float dy = 26.0f * (1.0f - in);
            list.push_opacity(in);
            x += hui::ui::text(list, fonts.display, one, x, cy + dy, size, color);
            list.pop_opacity();
        }

        // A turning ring and a real progress bar underneath.
        draw_spinner(list, cx, cy + 130.0f, 30.0f, Color::rgb(color::kSignal, 0.9f),
                     (double)age_, 6.0f);
        const Rect track{cx - 220.0f, cy + 196.0f, 440.0f, 10.0f};
        list.rounded_rect(track, 5.0f, Color::rgb(color::kPanelHigh, 0.8f));
        const float progress = hui::tween::clamp01(age_ / kDuration);
        if (progress > 0.01f)
        {
            Rect fill = track;
            fill.w = track.w * hui::tween::cubic_out(progress);
            list.rounded_rect(fill, 5.0f, Color::rgb(color::kSignal));
        }
        hui::ui::text(list, fonts.regular, "starting the dock", cx, cy + 254.0f, 26.0f,
                      Color::rgb(color::kMuted), gfx::Align::center, 2.0f);

        draw_brand(frame, hui::tween::clamp01(age_ * 1.5f));
    }

  private:
    static constexpr float kDuration = 2.2f;
    AppServices &services_;
    float age_ = 0.0f;
    bool done_ = false;
};

} // namespace

std::unique_ptr<Screen> make_splash_screen(AppServices &services)
{
    return std::make_unique<SplashScreen>(services);
}

} // namespace vd5
