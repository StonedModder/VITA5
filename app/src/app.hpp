// VITA5 app — the screen manager and frame composer.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "screens.hpp"

#include "gfx/renderer.hpp"

#include <cstdint>
#include <memory>

namespace vd5
{

class App
{
  public:
    App(const hui::ui::Fonts &fonts, std::uint32_t glass_texture, DockModel &dock,
        VdSettings &settings, const char *settings_path);

    // One update: screens advance, requests are honoured, transitions move.
    void update(const hui::InputFrame &input, float dt, Feedback &feedback);
    // Queues the frame (backdrop + scene) on the renderer.
    void compose(hui::gfx::Renderer &renderer);

    // Flushes the input bridges (closing neutral report) at exit.
    ~App();

    void set_clock(double seconds)
    {
        services_.time = seconds;
    }
    void set_app_fps(double fps)
    {
        services_.app_fps = fps;
    }
    void set_pad_connected(bool connected)
    {
        services_.pad_connected = connected;
    }
    // Called when the settings change outside the settings screen (none
    // today) or after each applied change: returns true when the app should
    // re-apply gains/flags this frame.
    bool take_settings_changed()
    {
        const bool changed = services_.settings_changed;
        services_.settings_changed = false;
        return changed;
    }
    const VdSettings &settings() const
    {
        return settings_;
    }

  private:
    void go(Screen *next);

    DockModel &dock_;
    VdSettings &settings_;
    AppServices services_;
    const hui::ui::Fonts *fonts_;
    std::uint32_t glass_;

    std::unique_ptr<Screen> splash_;
    std::unique_ptr<Screen> connect_;
    std::unique_ptr<Screen> live_;
    std::unique_ptr<Screen> settings_screen_;
    std::unique_ptr<Screen> inputtest_;
    Screen *current_ = nullptr;
    Screen *outgoing_ = nullptr;

    float transition_ = 1.0f; // 0..1: incoming screen's entrance progress
    static constexpr float kTransitionSeconds = 0.42f;

    hui::gfx::DrawList scene_;
    hui::gfx::BackdropSpec backdrop_{};
};

} // namespace vd5
