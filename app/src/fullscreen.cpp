// VITA5 app — fullscreen enter/exit for the live view. See fullscreen.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fullscreen.hpp"

namespace vd5
{

namespace
{
// The picture inside the stage, and inside the full canvas: the largest
// aspect-preserving fit (thin letterbox bars when the ratios differ).
hui::gfx::Rect fit_within(const hui::gfx::Rect &bounds, int video_w, int video_h)
{
    if (video_w <= 0 || video_h <= 0)
        return bounds;
    const float scale_x = bounds.w / (float)video_w;
    const float scale_y = bounds.h / (float)video_h;
    const float scale = scale_x < scale_y ? scale_x : scale_y;
    const float w = (float)video_w * scale;
    const float h = (float)video_h * scale;
    return {bounds.x + (bounds.w - w) * 0.5f, bounds.y + (bounds.h - h) * 0.5f, w, h};
}

hui::gfx::Rect lerp_rect(const hui::gfx::Rect &a, const hui::gfx::Rect &b, float t)
{
    const float u = 1.0f - t;
    return {a.x * u + b.x * t, a.y * u + b.y * t, a.w * u + b.w * t, a.h * u + b.h * t};
}

constexpr float kHintFadeIn = 0.22f; // seconds the exit hint takes to appear
constexpr float kHintHold = 2.6f;    // seconds it stays while the player idles
constexpr float kHintFadeOut = 0.7f; // seconds it takes to leave again
constexpr float kZoomOmega = 11.0f;  // slightly soft, like the kit's panels
} // namespace

void Fullscreen::update(float dt, bool activity)
{
    zoom_.target = active_ ? 1.0f : 0.0f;
    zoom_.update(dt, kZoomOmega);
    if (activity)
        hint_idle_ = 0.0f;
    else
        hint_idle_ += dt;
}

float Fullscreen::hint_alpha() const
{
    if (!active_)
        return 1.0f; // the HUD view's hints are permanent chrome
    if (hint_idle_ < kHintFadeIn)
        return hint_idle_ / kHintFadeIn;
    if (hint_idle_ < kHintHold)
        return 1.0f;
    return hui::tween::clamp01(1.0f - (hint_idle_ - kHintHold) / kHintFadeOut);
}

hui::gfx::Rect Fullscreen::video_rect(const hui::gfx::Rect &stage, int video_w, int video_h) const
{
    const hui::gfx::Rect in_hud = fit_within(stage, video_w, video_h);
    const hui::gfx::Rect on_screen = fit_within({0.0f, 0.0f, kScreenW, kScreenH}, video_w, video_h);
    return lerp_rect(in_hud, on_screen, zoom());
}

float Fullscreen::corner_radius() const
{
    return 8.0f * (1.0f - zoom());
}

} // namespace vd5
