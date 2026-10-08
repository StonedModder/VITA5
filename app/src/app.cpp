// VITA5 app — the screen manager and frame composer. See app.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.hpp"

#include "core/tween.hpp"
#include "vd/pad_bridge.hpp"
#include "vd/touch_bridge.hpp"

namespace vd5
{

App::App(const hui::ui::Fonts &fonts, std::uint32_t glass_texture, DockModel &dock,
         VdSettings &settings, const char *settings_path)
    : dock_(dock), settings_(settings), services_{dock, settings}, fonts_(&fonts),
      glass_(glass_texture)
{
    services_.fonts = &fonts;
    services_.glass = glass_texture;
    services_.settings_path = settings_path;

    splash_ = make_splash_screen(services_);
    connect_ = make_connect_screen(services_);
    live_ = make_live_screen(services_);
    settings_screen_ = make_settings_screen(services_);
    inputtest_ = make_inputtest_screen(services_);

    current_ = splash_.get();
    current_->enter();
    transition_ = 1.0f;
}

void App::go(Screen *next)
{
    if (next == nullptr || next == current_)
        return;
    if (current_ != nullptr)
        current_->leave();
    outgoing_ = current_;
    current_ = next;
    current_->enter();
    transition_ = 0.0f;
}

void App::update(const hui::InputFrame &input, float dt, Feedback &feedback)
{
    services_.clear_requests();
    if (current_ != nullptr)
        current_->update(input, dt, feedback);

    if (services_.retry_attach)
        dock_.request_attach();
    // Settings are live: the screens edit the shared struct directly and
    // main() re-applies gains and flags when take_settings_changed() fires.
    if (services_.go_settings)
        go(settings_screen_.get());
    else if (services_.go_inputtest)
        go(inputtest_.get());
    else if (services_.close_inputtest)
        go(settings_screen_.get());
    else if (services_.go_live)
        go(live_.get());
    else if (services_.go_connect)
        go(connect_.get());
    else if (services_.close_settings)
        go(dock_.stream_active() ? live_.get() : connect_.get());

    // Live input passthrough (DualSense -> Vita) is driven by the dock model
    // from the frame's pad sample — the single scePad reader — including the
    // touch bridge, the settings toggles and the L3+R3 capture chord. The
    // input test screen drives the transport itself.

    if (transition_ < 1.0f)
    {
        transition_ += dt / kTransitionSeconds;
        if (transition_ >= 1.0f)
        {
            transition_ = 1.0f;
            outgoing_ = nullptr;
        }
    }
}

void App::compose(hui::gfx::Renderer &renderer)
{
    scene_.clear();
    backdrop_ = hui::gfx::BackdropSpec{};
    Frame frame{scene_, backdrop_, *fonts_, glass_, services_.time, &services_};

    renderer.begin();
    if (outgoing_ != nullptr && transition_ < 1.0f)
    {
        // The leaving screen slides up and fades; the entering one rises
        // into place (eased, never teleported).
        const float t = hui::tween::quint_out(transition_);
        hui::gfx::BackdropSpec old_backdrop{};
        Frame old_frame{scene_, old_backdrop, *fonts_, glass_, services_.time, &services_};
        scene_.push_opacity(1.0f - t);
        scene_.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -70.0f * t);
        outgoing_->draw(old_frame);
        scene_.pop_transform();
        scene_.pop_opacity();
        if (outgoing_ != current_)
        {
            scene_.push_opacity(t);
            scene_.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 70.0f * (1.0f - t));
            current_->draw(frame);
            scene_.pop_transform();
            scene_.pop_opacity();
        }
        renderer.backdrop(backdrop_);
    }
    else
    {
        current_->draw(frame);
        renderer.backdrop(backdrop_);
    }
    renderer.draw(scene_);
}

App::~App()
{
    // Closing neutral report so nothing stays held on the Vita.
    pad_bridge_shutdown();
}

} // namespace vd5
