// VITA5 app — the dock theme implementation. See vd_theme.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vd_theme.hpp"

namespace vd5
{

const hui::ui::Theme &dock_theme()
{
    using hui::ui::Theme;
    using hui::gfx::Color;
    static const Theme kTheme = [] {
        Theme t{};
        t.id = "dock";
        t.name = "Docklight";
        t.family = "VITA5";
        t.summary = "OLED-dark dock glass with a cyan signal accent";

        // Page: animated aurora in deep indigo/teal, frozen per screen by the
        // caller through spec.time.
        t.backdrop.mode = hui::gfx::BackdropMode::aurora;
        t.backdrop.colors[0] = Color::rgb(0x050a14);
        t.backdrop.colors[1] = Color::rgb(0x0b1b33);
        t.backdrop.colors[2] = Color::rgb(0x155e63);
        t.backdrop.colors[3] = Color::rgb(0x2fe0c9);
        t.page = Color::rgb(color::kPage);
        t.page_text = Color::rgb(color::kInk);
        t.page_text_muted = Color::rgb(color::kMuted);

        t.surface = Color::rgb(color::kPanel, 0.92f);
        t.surface_high = Color::rgb(color::kPanelHigh, 0.92f);
        t.text = Color::rgb(color::kInk);
        t.text_muted = Color::rgb(color::kMuted);
        t.primary = Color::rgb(color::kSignal);
        t.on_primary = Color::rgb(0x03211f);
        t.secondary = Color::rgb(color::kPanelHigh);
        t.on_secondary = Color::rgb(color::kInk);
        t.accent = Color::rgb(color::kSignal);
        t.outline = Color::rgb(0x2a3c5e);
        t.focus = Color::rgb(color::kSignal, 0.9f);
        t.shadow = Color::rgb(0x000000, 0.55f);
        t.light = Color::rgb(0x9fd8ff, 0.35f);
        t.danger = Color::rgb(color::kAlert);
        t.success = Color::rgb(0x3ddc84);
        t.warning = Color::rgb(color::kWaiting);

        t.style = hui::ui::SurfaceStyle::soft;
        t.corner = hui::ui::Corner::round;
        t.radius = 14.0f;
        t.radius_card = 24.0f;
        t.border = 1.5f;
        t.shadow_offset = 8.0f;
        t.shadow_blur = 22.0f;
        t.focus_width = 3.0f;
        t.focus_gap = 5.0f;

        t.heading = hui::ui::FontRole::display;
        t.label = hui::ui::FontRole::semibold;
        t.caps = false;
        t.tracking = 0.5f;

        t.omega = 15.0f;
        t.damping = 1.0f;
        t.sounds = hui::audio::SoundSet::glass;
        t.dark = true;
        return t;
    }();
    return kTheme;
}

} // namespace vd5
