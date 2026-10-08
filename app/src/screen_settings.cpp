// VITA5 app — the settings screen.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Three designed states:
//   absent    the dock is offline: the same settings, with a banner saying
//             video settings apply on the next connection
//   busy      a change is being written (the save is real, see save_timer_)
//   connected settings while the stream runs live behind the panel
//
// Values apply the frame they change; they are written once, a moment after
// the last edit (never on the frame).

#include "screens.hpp"

#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "core/tween.hpp"
#include "platform/ps5/system.hpp"
#include "vd_settings.hpp"

#include <cstdio>

namespace vd5
{

namespace
{

using gfx::Color;
using gfx::Rect;

constexpr int kRowCount = 14;
constexpr int kVisibleRows = 10; // the ten-row band the panel was designed for
constexpr int kRowInputTest = 13;

// The rows keep their original look; when they outgrow the band the list
// scrolls under it (hui::ui::Scroller + a clip), instead of moving.
constexpr float kRowsLeft = 88.0f;
constexpr float kRowsTop = 336.0f;
constexpr float kRowWidth = 1220.0f;
constexpr float kRowHeight = 58.0f;
constexpr float kRowPitch = 62.0f;
constexpr float kViewHeight = (kVisibleRows - 1) * kRowPitch + kRowHeight;
constexpr float kContentHeight = (kRowCount - 1) * kRowPitch + kRowHeight;

struct Row
{
    const char *label;
    const char *hint;
};

constexpr Row kRows[kRowCount] = {
    {"Output resolution", "applies at the next launch"},
    {"Frame cap", "presented frames per second"},
    {"Stream audio", "Vita audio through the TV"},
    {"Stream volume", "Vita audio level"},
    {"Interface sounds", "menu feedback"},
    {"Interface volume", "menu sound level"},
    {"Show FPS readout", "in the status pill"},
    {"Show stream HUD", "over the video"},
    {"Controller rumble", "on events"},
    {"Light bar", "follows the dock accent"},
    {"Controller passthrough", "the PS5 pad drives the Vita"},
    {"Touch target", "where touch input is sent"},
    {"Upscale", "picture scaling for the stream"},
    {"Input test...", "check buttons and touch"},
};

constexpr const char *kTouchTargets[2] = {"Rear touchpad", "Front touchscreen"};
// Labels must match vd_settings_upscale_label / UpscaleMode order
// (0 off, 1 sharp2, 2 fsr2, 3 sharp3, 4 fsr3, 5 sharp4, 6 fsr4).
constexpr const char *kUpscaleLabels[kUpscaleModeCount] = {
    "Off", "Sharp 2x", "FSR 2x", "Sharp 3x", "FSR 3x", "Sharp 4x", "FSR 4x",
};

class SettingsScreen final : public Screen
{
  public:
    explicit SettingsScreen(AppServices &services) : services_(services) {}

    const char *name() const override
    {
        return "settings";
    }

    void enter() override
    {
        age_ = 0.0f;
        saving_ = 0.0f;
        saved_flash_ = 0.0f;
        // Park the list wherever the focus last was, then line the ring up.
        reveal_focus();
        scroll_.position.snap(scroll_.position.target);
        highlight_.snap(row_rect(focus_));
    }

    void update(const hui::InputFrame &input, float dt, Feedback &feedback) override
    {
        age_ += dt;
        if (saved_flash_ > 0.0f)
            saved_flash_ -= dt;

        // Deferred save: quiet for a moment, then write once.
        if (saving_ > 0.0f)
        {
            saving_ -= dt;
            if (saving_ <= 0.0f)
            {
                saving_ = 0.0f;
                if (vd5::save_settings(services_.settings_path, services_.settings))
                {
                    saved_flash_ = 1.4f;
                    feedback.play(hui::audio::Cue::saved);
                }
                else
                {
                    save_failed_ = true;
                    feedback.play(hui::audio::Cue::error);
                }
            }
        }

        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = focus_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < kRowCount)
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

        const bool left = input.nav == Direction::left ||
                          input.is_pressed(Action::page_prev);
        const bool right = input.nav == Direction::right ||
                           input.is_pressed(Action::page_next);
        if (left || right)
        {
            if (change(focus_, right ? 1 : -1))
            {
                feedback.play(is_toggle(focus_) ? hui::audio::Cue::toggle
                                                : hui::audio::Cue::slider);
                services_.settings_changed = true;
                saving_ = 0.7f;
            }
        }
        if (input.is_pressed(Action::confirm))
        {
            if (focus_ == kRowInputTest)
            {
                open_input_test();
                feedback.play(hui::audio::Cue::open);
            }
            else if (change(focus_, 1))
            {
                feedback.play(is_toggle(focus_) ? hui::audio::Cue::toggle
                                                : hui::audio::Cue::slider);
                services_.settings_changed = true;
                saving_ = 0.7f;
            }
        }
        if (input.is_pressed(Action::back))
        {
            services_.close_settings = true;
            feedback.play(hui::audio::Cue::back);
        }

        reveal_focus();
        scroll_.update(dt, 14.0f);
        highlight_.target(row_rect(focus_));
        highlight_.update(dt, 16.0f);
        refusal_.update(dt, 9.0f);
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
        paint.heading("Settings", 88.0f, 220.0f, 56.0f);
        list.pop_opacity();

        // State banner (absent) or live chip (connected).
        if (state == DockState::absent)
        {
            const Rect banner{88.0f, 252.0f, 1220.0f, 56.0f};
            list.rounded_rect(banner, 18.0f, Color::rgb(color::kWaiting, 0.13f));
            list.bordered_rect(banner, 18.0f, Color(0, 0, 0, 0), 1.5f,
                               Color::rgb(color::kWaiting, 0.5f));
            hui::ui::text(list, fonts.regular,
                          "Dock offline: video and audio settings apply on the next connection.",
                          banner.x + 26.0f, banner.cy() + 8.0f, 23.0f,
                          Color::rgb(color::kWaiting));
        }
        else if (state == DockState::connected)
        {
            const Rect chip{88.0f, 258.0f, 220.0f, 44.0f};
            list.rounded_rect(chip, 22.0f, Color::rgb(color::kSignal, 0.14f));
            list.circle(chip.x + 24.0f, chip.cy(), 7.0f,
                        Color::rgb(color::kSignal, 0.6f + 0.4f * hui::ui::breathe((float)frame.time)));
            hui::ui::text(list, fonts.semibold, "STREAM LIVE", chip.x + 44.0f, chip.cy() + 8.0f,
                          20.0f, Color::rgb(color::kSignal), gfx::Align::left, 1.5f);
        }
        else
        {
            const Rect chip{88.0f, 258.0f, 260.0f, 44.0f};
            list.rounded_rect(chip, 22.0f, Color::rgb(color::kWaiting, 0.13f));
            hui::ui::text(list, fonts.semibold, "CONNECTING", chip.x + 24.0f, chip.cy() + 8.0f,
                          20.0f, Color::rgb(color::kWaiting), gfx::Align::left, 1.5f);
        }

        // ---- the rows (a scrolling list, clipped to the band) ----
        list.push_clip({kRowsLeft, kRowsTop, kRowWidth, kViewHeight});
        for (int i = 0; i < kRowCount; ++i)
        {
            const Rect r = row_rect(i);
            if (r.y + r.h <= kRowsTop || r.y >= kRowsTop + kViewHeight)
                continue;
            const float in = hui::tween::stagger(age_, i + 1, 0.05f, 0.45f);
            if (in <= 0.0f)
                continue;
            list.push_opacity(in);
            draw_row(paint, r, i, i == focus_);
            list.pop_opacity();
        }
        list.pop_clip();

        // The focus ring glides between rows (spring, not a teleport).
        Rect ring = highlight_.value();
        ring.x += hui::ui::shake(refusal_.value, (float)frame.time);
        paint.focus_ring(ring, 18.0f, 0.55f + 0.45f * hui::ui::breathe((float)frame.time, 1.8f));

        // ---- footer: save state (this is the settings "request" state) ----
        const Rect footer{88.0f, 986.0f, 720.0f, 56.0f};
        if (saving_ > 0.0f)
        {
            draw_spinner(list, footer.x + 26.0f, footer.cy(), 16.0f,
                         Color::rgb(color::kWaiting), frame.time, 4.0f);
            hui::ui::text(list, fonts.regular, "Saving settings", footer.x + 58.0f,
                          footer.cy() + 8.0f, 24.0f, Color::rgb(color::kWaiting));
        }
        else if (saved_flash_ > 0.0f)
        {
            list.circle(footer.x + 26.0f, footer.cy(), 12.0f, Color::rgb(color::kSuccess));
            hui::ui::text(list, fonts.regular, "Settings saved", footer.x + 58.0f,
                          footer.cy() + 8.0f, 24.0f, Color::rgb(color::kSuccess));
        }
        else if (save_failed_)
        {
            list.circle(footer.x + 26.0f, footer.cy(), 12.0f, Color::rgb(color::kAlert));
            hui::ui::text(list, fonts.regular, "Could not write settings", footer.x + 58.0f,
                          footer.cy() + 8.0f, 24.0f, Color::rgb(color::kAlert));
        }
        else
        {
            hui::ui::text(list, fonts.regular, kRows[focus_].hint, footer.x + 4.0f,
                          footer.cy() + 8.0f, 24.0f, Color::rgb(color::kMuted));
        }

        draw_status_pill(frame, "SETTINGS", state, hui::ui::breathe((float)frame.time));

        const hui::ui::Hint hints[] = {
            {hui::ui::Button::cross, focus_ == kRowInputTest ? "Open" : "Change"},
            {hui::ui::Button::l2, "Less", hui::ui::Button::r2},
            {hui::ui::Button::circle, "Back"},
        };
        draw_hints_row(frame, hints, 3);
    }

  private:
    static bool is_toggle(int row)
    {
        return row == 2 || row == 4 || row == 6 || row == 7 || row == 8 || row == 9 ||
               row == 10;
    }

    Rect row_rect(int index) const
    {
        // Rows keep a fixed pitch so the focus springs (driven from update(),
        // which knows no draw layout) line up with them; the list scrolls
        // under the band instead of moving the rows.
        return {kRowsLeft, kRowsTop + (float)index * kRowPitch - scroll_.offset(),
                kRowWidth, kRowHeight};
    }

    // Keeps the focused row inside the scrolling band.
    void reveal_focus()
    {
        scroll_.reveal((float)focus_ * kRowPitch, (float)focus_ * kRowPitch + kRowHeight,
                       kViewHeight, 6.0f, kContentHeight);
    }

    // The test-harness screen is owned by the app; this hands over via a
    // screen request (see screens.hpp AppServices).
    void open_input_test()
    {
        services_.go_inputtest = true;
    }

    // Returns true when the value actually moved.
    bool change(int row, int direction)
    {
        VdSettings &s = services_.settings;
        switch (row)
        {
            case 0:
            {
                const int next = s.resolution + direction;
                if (next < 0 || next > 2)
                    return false;
                s.resolution = next;
                return true;
            }
            case 1:
                s.fps_cap = s.fps_cap == 60 ? 30 : 60;
                return true;
            case 2:
                s.stream_audio = !s.stream_audio;
                return true;
            case 3:
                return step(s.stream_volume, direction);
            case 4:
                s.ui_sounds = !s.ui_sounds;
                return true;
            case 5:
                return step(s.ui_volume, direction);
            case 6:
                s.show_fps = !s.show_fps;
                return true;
            case 7:
                s.show_hud = !s.show_hud;
                return true;
            case 8:
                s.haptics = !s.haptics;
                return true;
            case 9:
                s.light_bar = !s.light_bar;
                return true;
            case 10:
                s.input_passthrough = !s.input_passthrough;
                return true;
            case 11:
            {
                const int next = (int)s.touch_target + direction;
                if (next < 0 || next > 1)
                    return false;
                s.touch_target = (TouchTarget)next;
                return true;
            }
            case 12:
            {
                const int next = (int)s.upscale + direction;
                if (next < 0 || next > kUpscaleModeMax)
                    return false;
                s.upscale = (UpscaleMode)next;
                return true;
            }
            default:
                return false;
        }
    }

    static bool step(int &value, int direction)
    {
        const int next = value + direction;
        if (next < 0 || next > 10)
            return false;
        value = next;
        return true;
    }

    void draw_row(hui::ui::Painter &paint, const Rect &r, int index, bool focused) const
    {
        const VdSettings &s = services_.settings;
        const hui::ui::Look look{focused ? 1.0f : 0.0f, 0.0f, false};
        paint.panel(r);
        paint.body(kRows[index].label, r.x + 28.0f, r.cy() + 9.0f, 27.0f, paint.theme().text);

        char value[64];
        bool toggle = false;
        float numeric = 0.0f;
        switch (index)
        {
            case 0:
                std::snprintf(value, sizeof(value), "%s",
                              hui::Settings::kResolutions[s.resolution].label);
                break;
            case 1:
                std::snprintf(value, sizeof(value), "%d fps", s.fps_cap);
                break;
            case 2:
                toggle = s.stream_audio;
                std::snprintf(value, sizeof(value), "%s", toggle ? "On" : "Off");
                break;
            case 3:
                numeric = (float)s.stream_volume / 10.0f;
                std::snprintf(value, sizeof(value), "%d", s.stream_volume);
                break;
            case 4:
                toggle = s.ui_sounds;
                std::snprintf(value, sizeof(value), "%s", toggle ? "On" : "Off");
                break;
            case 5:
                numeric = (float)s.ui_volume / 10.0f;
                std::snprintf(value, sizeof(value), "%d", s.ui_volume);
                break;
            case 6:
                toggle = s.show_fps;
                std::snprintf(value, sizeof(value), "%s", toggle ? "On" : "Off");
                break;
            case 7:
                toggle = s.show_hud;
                std::snprintf(value, sizeof(value), "%s", toggle ? "On" : "Off");
                break;
            case 8:
                toggle = s.haptics;
                std::snprintf(value, sizeof(value), "%s", toggle ? "On" : "Off");
                break;
            case 9:
                toggle = s.light_bar;
                std::snprintf(value, sizeof(value), "%s", toggle ? "On" : "Off");
                break;
            case 10:
                toggle = s.input_passthrough;
                std::snprintf(value, sizeof(value), "%s", toggle ? "On" : "Off");
                break;
            case 11:
                std::snprintf(value, sizeof(value), "%s", kTouchTargets[(int)s.touch_target]);
                break;
            case 12:
                std::snprintf(value, sizeof(value), "%s", kUpscaleLabels[(int)s.upscale]);
                break;
            case 13:
            default:
                std::snprintf(value, sizeof(value), "Open");
                break;
        }

        const float right = r.x + r.w - 28.0f;
        if (index == 3 || index == 5)
        {
            const Rect slider{right - 320.0f, r.cy() - 7.0f, 220.0f, 14.0f};
            paint.slider(slider, numeric, look);
            paint.body(value, right, r.cy() + 9.0f, 26.0f, paint.theme().text_muted,
                       gfx::Align::right);
        }
        else if (is_toggle(index))
        {
            const Rect toggle_rect{right - 92.0f, r.cy() - 21.0f, 92.0f, 42.0f};
            paint.toggle(toggle_rect, toggle ? 1.0f : 0.0f, look);
        }
        else
        {
            paint.body(value, right, r.cy() + 9.0f, 27.0f,
                       focused ? paint.theme().accent : paint.theme().text_muted,
                       gfx::Align::right);
        }
    }

    AppServices &services_;
    float age_ = 0.0f;
    float saving_ = 0.0f;
    float saved_flash_ = 0.0f;
    bool save_failed_ = false;
    int focus_ = 0;
    mutable hui::ui::SpringRect highlight_;
    hui::ui::Scroller scroll_;
    hui::ui::Pulse refusal_;
};

} // namespace

std::unique_ptr<Screen> make_settings_screen(AppServices &services)
{
    return std::make_unique<SettingsScreen>(services);
}

} // namespace vd5
