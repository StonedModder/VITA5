// VITA5 app — shared screen chrome. See screens.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screens.hpp"

#include "ui/fonts.hpp"

#include <cstdio>

namespace vd5
{

using gfx::Color;
using gfx::Rect;

void fill_backdrop(Frame &frame, hui::gfx::BackdropMode mode)
{
    frame.backdrop.mode = mode;
    frame.backdrop.colors[0] = Color::rgb(0x050a14);
    frame.backdrop.colors[1] = Color::rgb(0x0b1b33);
    frame.backdrop.colors[2] = Color::rgb(0x155e63);
    frame.backdrop.colors[3] = Color::rgb(color::kSignal, 0.85f);
    frame.backdrop.params[0] = 0.0f;
    frame.backdrop.params[1] = 0.0f;
    frame.backdrop.params[2] = 0.0f;
    frame.backdrop.params[3] = 0.0f;
    frame.backdrop.time = (float)frame.time;
}

void draw_brand(Frame &frame, float alpha)
{
    hui::gfx::DrawList &list = frame.scene;
    const hui::ui::Fonts &fonts = frame.fonts;
    list.push_opacity(alpha);
    // "vita" in ink, "Dock" in the signal colour, "5" as a badge.
    const float x = 88.0f;
    const float baseline = 118.0f;
    const float size = 46.0f;
    const float w1 = hui::ui::text(list, fonts.display, "vita", x, baseline, size,
                                   Color::rgb(color::kInk));
    const float w2 = hui::ui::text(list, fonts.display, "Dock", x + w1, baseline, size,
                                   Color::rgb(color::kSignal));
    const Rect badge{x + w1 + w2 + 12.0f, baseline - 38.0f, 42.0f, 42.0f};
    list.rounded_rect(badge, 10, Color::rgb(color::kSignal, 0.18f));
    hui::ui::text(list, fonts.semibold, "5", badge.cx(), baseline - 8.0f, 28.0f,
                  Color::rgb(color::kSignal), gfx::Align::center);
    list.pop_opacity();
}

void draw_status_pill(Frame &frame, const char *label, DockState state, float breathe)
{
    hui::gfx::DrawList &list = frame.scene;
    const hui::ui::Fonts &fonts = frame.fonts;

    Color dot;
    switch (state)
    {
        case DockState::connected:
            dot = Color::rgb(color::kSignal);
            break;
        case DockState::busy:
            dot = Color::rgb(color::kWaiting);
            break;
        case DockState::absent:
        default:
            dot = Color::rgb(color::kAlert);
            break;
    }

    char fps[32];
    if (frame.services != nullptr && frame.services->settings.show_fps)
        std::snprintf(fps, sizeof(fps), "%2.0f FPS", frame.services->app_fps);
    else
        fps[0] = '\0';

    const float text_size = 26.0f;
    const float label_w = fonts.semibold.measure(label, text_size);
    const float fps_w = fps[0] != '\0' ? fonts.mono.measure(fps, text_size) + 28.0f : 0.0f;
    const float width = label_w + fps_w + 96.0f;
    const Rect pill{1832.0f - width, 72.0f, width, 56.0f};

    list.shadow(pill, 20, 22, Color::rgb(0x000000, 0.35f));
    list.rounded_rect(pill, 28, Color::rgb(color::kPanel, 0.85f));
    list.rounded_rect(pill.inset(-1.5f), 29, Color(0, 0, 0, 0)); // spacer keeps radius
    list.bordered_rect(pill, 28, Color(0, 0, 0, 0), 1.5f, Color::rgb(0x2a3c5e, 0.9f));

    // The dot breathes while the state is not settled, and pulses with each
    // fresh video frame when streaming.
    const float glow = 0.55f + 0.45f * breathe;
    list.glow({pill.x + 26.0f, pill.cy() - 9.0f, 18.0f, 18.0f}, 9, 14, dot.with_alpha(0.35f * glow));
    list.circle(pill.x + 35.0f, pill.cy(), 9.0f, dot);
    hui::ui::text(list, fonts.semibold, label, pill.x + 60.0f, pill.cy() + 9.0f, text_size,
                  Color::rgb(color::kInk));
    if (fps[0] != '\0')
        hui::ui::text(list, fonts.mono, fps, pill.x + width - 28.0f, pill.cy() + 8.0f, text_size,
                      Color::rgb(color::kMuted), gfx::Align::right);
}

void draw_hints_row(Frame &frame, const hui::ui::Hint *hints, int count)
{
    hui::gfx::DrawList &list = frame.scene;
    const float width = hui::ui::measure_hints(frame.fonts, hints, count);
    const float x = 1832.0f;
    // A soft plate behind the hints keeps them readable over video.
    list.rounded_rect({x - width - 36.0f, 964.0f, width + 60.0f, 72.0f}, 26.0f,
                      Color::rgb(color::kDeep, 0.55f));
    hui::ui::draw_hints(list, frame.fonts, hui::ui::GlyphStyle::dark(), hints, count, x, true);
}

void draw_spinner(hui::gfx::DrawList &list, float cx, float cy, float radius,
                  hui::gfx::Color color, double seconds, float thickness)
{
    // Two arcs chasing each other: one solid, one fading.
    const float base = (float)(seconds * 2.2);
    list.arc(cx, cy, radius, thickness, base, 1.9f, color);
    list.arc(cx, cy, radius, thickness, base + 3.14159f, 1.1f, color.with_alpha(0.45f));
}

void draw_step_row(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, int index,
                   const char *title, const char *detail, float x, float y, float width,
                   float appear)
{
    const float in = appear;
    if (in <= 0.0f)
        return;
    const Rect row{x, y, width, 92.0f};
    list.push_opacity(in);
    list.rounded_rect(row, 18.0f, Color::rgb(color::kPanel, 0.72f));
    list.circle(x + 46.0f, y + 46.0f, 22.0f, Color::rgb(color::kSignal, 0.16f));
    char number[8];
    std::snprintf(number, sizeof(number), "%d", index);
    hui::ui::text(list, fonts.semibold, number, x + 46.0f, y + 55.0f, 26.0f,
                  Color::rgb(color::kSignal), gfx::Align::center);
    hui::ui::text(list, fonts.semibold, title, x + 88.0f, y + 40.0f, 28.0f,
                  Color::rgb(color::kInk));
    hui::ui::text(list, fonts.regular, detail, x + 88.0f, y + 72.0f, 23.0f,
                  Color::rgb(color::kMuted));
    list.pop_opacity();
}

} // namespace vd5
