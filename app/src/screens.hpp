// VITA5 app — screen interface and shared drawing helpers.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Four screens, each designed in three states up front:
//
//   splash      the loading transition (always its own animated state)
//   connect     absent: waiting for the dock service / the Vita
//               busy:   a request is in flight (attach, stream start)
//               connected: the Vita is streaming (moves on to live view)
//   live        absent: no signal, with what to do about it
//               busy:   the stream is starting
//               connected: video + audio + input status overlay
//   settings    absent:  dock offline banner over the same settings
//               busy:    a change is being saved
//               connected: settings while the stream is live
//
// Screens never talk to the rings directly: they read DockModel's facts.

#pragma once
#include "core/input.hpp"
#include "dock_model.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "vd_settings.hpp"
#include "vd_theme.hpp"

#include "audio/cues.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace vd5
{

// Shorthand for the kit's graphics namespace (the screens draw with it
// constantly).
namespace gfx = hui::gfx;

// The kit's input vocabulary, unqualified, for the screens.
using hui::Action;
using hui::Direction;

// Sounds (and rumble) a screen asked for this frame.
struct Feedback
{
    std::vector<hui::audio::CueEvent> cues;
    float rumble_strength = 0.0f;
    float rumble_seconds = 0.0f;

    void play(hui::audio::Cue cue, float gain = 1.0f, float pan = 0.0f, float pitch = 1.0f)
    {
        hui::audio::CueEvent event{};
        event.cue = cue;
        event.gain = gain;
        event.pan = pan;
        event.pitch = pitch;
        cues.push_back(event);
    }
    void clear()
    {
        cues.clear();
        rumble_strength = 0.0f;
        rumble_seconds = 0.0f;
    }
};

// Everything a screen can reach from the app.
struct AppServices
{
    DockModel &dock;
    VdSettings &settings;
    const hui::ui::Fonts *fonts = nullptr;
    std::uint32_t glass = 0;
    const char *settings_path = nullptr;
    double time = 0.0;    // free-running seconds, for idle motion
    double app_fps = 0.0; // presented frames per second
    bool pad_connected = false;
    // Screens communicate with the app through these (read once per frame).
    bool settings_changed = false; // apply + persist the settings
    bool go_live = false;          // connect screen: open the live view
    bool go_connect = false;       // live screen: back to the connect view
    bool go_settings = false;      // open settings
    bool close_settings = false;   // leave settings
    bool go_inputtest = false;     // settings: open the input test screen
    bool close_inputtest = false;  // input test: back to settings
    bool retry_attach = false;     // ask the dock model for a fresh attempt
    bool toggle_fullscreen = false; // L3+D-Pad UP chord (live view)
    // Transient on-screen toast (L3 hotkey feedback); the app decays the
    // timer and the live view draws it. Display state, not a request.
    const char *toast_text = nullptr;
    float toast_time = 0.0f;

    void clear_requests()
    {
        settings_changed = false;
        toggle_fullscreen = false;
        go_live = false;
        go_connect = false;
        go_settings = false;
        close_settings = false;
        go_inputtest = false;
        close_inputtest = false;
        retry_attach = false;
    }
};

// One frame handed to a screen's draw().
struct Frame
{
    hui::gfx::DrawList &scene;
    hui::gfx::BackdropSpec &backdrop;
    const hui::ui::Fonts &fonts;
    std::uint32_t glass = 0;
    double time = 0.0;
    AppServices *services = nullptr;
};

class Screen
{
  public:
    virtual ~Screen() = default;
    virtual const char *name() const = 0;
    virtual void enter() {}
    virtual void leave() {}
    virtual void update(const hui::InputFrame &input, float dt, Feedback &feedback) = 0;
    virtual void draw(Frame &frame) const = 0;
};

// Factories (screen_*.cpp).
std::unique_ptr<Screen> make_splash_screen(AppServices &services);
std::unique_ptr<Screen> make_connect_screen(AppServices &services);
std::unique_ptr<Screen> make_live_screen(AppServices &services);
std::unique_ptr<Screen> make_settings_screen(AppServices &services);
std::unique_ptr<Screen> make_inputtest_screen(AppServices &services);

// ---- shared drawing helpers (screen_common.cpp) ---------------------------

// Top-right status pill: dock state with a breathing indicator, plus the
// live FPS readout when the settings ask for it.
void draw_status_pill(Frame &frame, const char *label, DockState state, float breathe);

// One-line brand mark, top-left.
void draw_brand(Frame &frame, float alpha);

// Bottom-right controller hints.
void draw_hints_row(Frame &frame, const hui::ui::Hint *hints, int count);

// The animated backdrop shared by every screen (frozen when time stops).
void fill_backdrop(Frame &frame, hui::gfx::BackdropMode mode);

// A small centred spinner arc turning `seconds` into rotation.
void draw_spinner(hui::gfx::DrawList &list, float cx, float cy, float radius,
                  hui::gfx::Color color, double seconds, float thickness = 6.0f);

// Numbered "what to do" step row (used by the absent states).
void draw_step_row(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, int index,
                   const char *title, const char *detail, float x, float y, float width,
                   float appear);

} // namespace vd5
