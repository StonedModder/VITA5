// VITA5 app — application entry point.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and audio, then runs the dock app every
// frame: read input, pump the shared rings, update the screens, play the
// sounds they asked for, draw, present. Lifecycle markers and pacing are
// logged for hardware runs. main() never returns: the shell closes the
// title (the runtime holds the process; see the kit's runtime shims).

#include "app.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "dock_model.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "vd_settings.hpp"
#include "vd_theme.hpp"

#include <GL/glcorearb.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>

namespace
{

constexpr const char *kAssets = "/app0/assets";
constexpr const char *kDataRoot = "/download0/vd5";
constexpr const char *kSettingsPath = "/download0/vd5/settings.cfg";

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[VD5] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

bool open_display(hui::ps5::Display &display, int *resolution)
{
    using hui::Settings;
    const Settings::Resolution &mode = Settings::kResolutions[*resolution];
    if (display.open(mode.width, mode.height))
    {
        hui::sys::log("[VD5] display mode %s %dx%d", mode.label, display.width(),
                      display.height());
        return true;
    }
    if (*resolution == 0)
        return false;
    hui::sys::log("[VD5] display mode %s failed, using 1080p", mode.label);
    *resolution = 0;
    return display.open(Settings::kResolutions[0].width, Settings::kResolutions[0].height);
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[VD5] entry");
    save::ensure_directory(kDataRoot);

    // ---- settings ----
    vd5::VdSettings settings;
    vd5::load_settings(kSettingsPath, &settings);
    sys::log("[VD5] settings resolution=%d fps=%d audio=%d", settings.resolution,
             settings.fps_cap, settings.stream_audio ? 1 : 0);

    // ---- display at the saved resolution ----
    int resolution = ps5::Display::supports_display_modes() ? settings.resolution : 0;
    ps5::Display display;
    if (!open_display(display, &resolution))
    {
        sys::log("[VD5] fatal: display open failed");
        sys::park();
    }
    settings.resolution = resolution;

    // ---- renderer + fonts ----
    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "montserrat-medium.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono))
    {
        sys::log("[VD5] fatal: renderer init failed");
        sys::park();
    }

    // ---- controller (reviewed public Pad ABI, vendored reader) ----
    ps5::Pad pad;
    const bool pad_open = pad.open();
    sys::log("[VD5] pad open=%d", pad_open ? 1 : 0);
    InputTracker tracker;

    // ---- audio: UI cues + the Vita stream through sceAudioOut ----
    audio::Mixer mixer;
    vd5::AudioPipe audio_pipe;
    mixer.attach_stream(0, audio_pipe.stream());
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(std::string(kAssets) + "/audio/sfx");
    sys::log("[VD5] sounds files=%d rejected=%d", bank.files, bank.rejected);

    // ---- dock model: shared region + video texture + pad forwarding ----
    vd5::VideoSurface video;
    vd5::DockModel dock;
    dock.init(&video, &audio_pipe);
    sys::log("[VD5] dock model ready (waiting for the dock service)");

    vd5::App app(fonts, renderer.glass_texture(), dock, settings, kSettingsPath);

    const auto apply_settings = [&] {
        mixer.set_bus_gain(audio::Bus::ui, Settings::gain(settings.ui_volume));
        mixer.set_stream_gain(0, settings.stream_audio ? Settings::gain(settings.stream_volume)
                                                       : 0.0f,
                             0.0f);
        audio_pipe.set_muted(!settings.stream_audio);
    };
    apply_settings();

    PadSample samples[64];
    PadSample newest{};
    bool have_sample = false;
    FrameStats stats;
    std::int64_t previous = sys::monotonic_us();
    std::int64_t last_frame_start = previous;
    std::uint64_t frames = 0;
    double fps_seconds = 0.0;
    int fps_frames = 0;
    double app_fps = 0.0;

    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        // Animation time is start-to-start, clamped so a hitch never
        // teleports the animations.
        float dt = frames == 0 ? 1.0f / 60.0f : (float)(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f;

        // ---- input: batch-drain to the newest sample ----
        const std::size_t count = pad.read(samples);
        if (count > 0)
        {
            newest = samples[count - 1];
            have_sample = true;
        }
        InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                          (std::uint64_t)now);

        // ---- dock rings + screens ----
        dock.update(dt, (std::uint64_t)now, have_sample ? &newest : nullptr, settings);
        vd5::Feedback feedback;
        app.set_clock((double)now / 1e6);
        app.set_app_fps(app_fps);
        app.set_pad_connected(input.connected);
        app.update(input, dt, feedback);
        if (app.take_settings_changed())
        {
            apply_settings();
            vd5::save_settings(kSettingsPath, settings);
        }

        // ---- feedback: cues and rumble ----
        for (const audio::CueEvent &event : feedback.cues)
        {
            if (settings.ui_sounds || audio::cue_bus(event.cue) != audio::Bus::ui)
                sounds.play(mixer, vd5::dock_theme().sounds, event);
        }
        if (settings.haptics && feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
        if (settings.light_bar)
        {
            const gfx::Color accent = vd5::dock_theme().accent;
            pad.set_light_bar((std::uint8_t)(accent.r * 255.0f), (std::uint8_t)(accent.g * 255.0f),
                              (std::uint8_t)(accent.b * 255.0f));
        }

        // ---- draw + present ----
        app.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[VD5] fatal: swap failed frame=%llu error=%s",
                     (unsigned long long)frames, ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        ++frames;
        if (frames == 1)
        {
            sys::log("[VD5] first-swap ok shapes=%zu draws=%zu", renderer.last_instances(),
                     renderer.last_draw_calls());
            const bool hidden = sys::hide_splash_screen();
            sys::log("[VD5] ready splash_hidden=%d", hidden ? 1 : 0);
        }

        // ---- pacing: honour the frame cap ----
        const std::int64_t after_swap = sys::monotonic_us();
        const long long frame_us = (long long)(after_swap - last_frame_start);
        const long long budget_us = 1000000LL / (settings.fps_cap == 30 ? 30 : 60);
        if (frame_us > 0 && frame_us < budget_us)
            sys::sleep_us((std::uint32_t)(budget_us - frame_us));

        // ---- telemetry ----
        const float frame_ms = (float)(after_swap - previous) / 1000.0f;
        previous = after_swap;
        stats.add((double)frame_ms);
        fps_seconds += (double)frame_ms / 1000.0;
        ++fps_frames;
        if (fps_seconds >= 0.5)
        {
            app_fps = (double)fps_frames / fps_seconds;
            fps_seconds = 0.0;
            fps_frames = 0;
        }
        if (stats.count() == 600)
        {
            char summary[160];
            stats.format(summary, sizeof(summary));
            sys::log("[VD5] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                     renderer.last_instances());
            sys::log("[VD5] dock attached=%d vita=%d stream=%d video_fps=%.1f torn=%llu "
                     "audio_chunks=%llu pad_reports=%llu audio_voices=%d underruns=%llu",
                     dock.service_present() ? 1 : 0, dock.vita_detected() ? 1 : 0,
                     dock.stream_active() ? 1 : 0, dock.video_fps,
                     (unsigned long long)dock.ring_stats().video_torn,
                     (unsigned long long)dock.ring_stats().audio_chunks,
                     (unsigned long long)dock.pad_reports(), mixer.active_voices(),
                     (unsigned long long)mixer.stream_underruns());
            stats.reset();
        }
    }
}
