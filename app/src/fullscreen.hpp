// VITA5 app — fullscreen enter/exit for the live view.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The live view has two layouts: the HUD view (the Vita's picture in its
// stage, with the status overlays) and full screen (the picture filling the
// whole 1920x1080 canvas, the HUD hidden). Cross (confirm) toggles between
// them, Circle (back) leaves full screen. The picture glides between the
// two layouts on the kit's critically damped spring (core/tween.hpp) and
// the chrome fades with it, so the change is animated, never a teleport.
//
// The exit affordance is a hint row that fades in on entry and again on
// every input, then fades out while the player idles, so the picture stays
// clean but the way out is never far.

#pragma once
#include "core/tween.hpp"
#include "gfx/draw_list.hpp"

namespace vd5
{

class Fullscreen
{
  public:
    // The design canvas: full screen means all of it.
    static constexpr float kScreenW = 1920.0f;
    static constexpr float kScreenH = 1080.0f;

    void toggle()
    {
        active_ = !active_;
        hint_idle_ = 0.0f;
    }
    void enter()
    {
        active_ = true;
        hint_idle_ = 0.0f;
    }
    void exit()
    {
        active_ = false;
        hint_idle_ = 0.0f;
    }
    bool active() const
    {
        return active_;
    }

    // One frame: chases the zoom spring toward the active layout and ages
    // the exit-hint timer. `activity` is true when the player touched a
    // control this frame (it resurfaces the hint).
    void update(float dt, bool activity);

    // Spring-settled progress of the change: 0 = HUD view, 1 = full screen.
    float zoom() const
    {
        return hui::tween::clamp01(zoom_.value);
    }
    // Alpha for the HUD chrome (brand, stage plate, overlays, hints).
    float hud_alpha() const
    {
        return 1.0f - zoom();
    }
    // Alpha for the full-screen exit hint (see the header note).
    float hint_alpha() const;
    // The picture's rect this frame: the HUD view fits it inside `stage`,
    // full screen fits it to the whole canvas; the spring glides between
    // the two. Letterboxing preserves the aspect ratio in both.
    hui::gfx::Rect video_rect(const hui::gfx::Rect &stage, int video_w, int video_h) const;
    // The picture's corner radius: 8 in the stage, square in full screen.
    float corner_radius() const;

  private:
    hui::tween::Spring zoom_{};
    bool active_ = false;
    float hint_idle_ = 0.0f;
};

} // namespace vd5
