// VITA5 app — persisted player settings (output, audio, input, overlays).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stored as key=value text under /download0/vd5/settings.cfg with the kit's
// write-then-rename discipline (core/save_file.hpp). The app applies values
// the frame they change and stores them once, a moment after the last edit —
// never on the frame.

#pragma once
#include <string>
#include <string_view>

namespace vd5
{

// Spatial upscale for the Vita's NV12 stream. Off keeps the proven CPU path;
// every other mode runs the GPU chain (bilinear or FSR EASU + RCAS) at the
// chosen integer scale. 5x–9x would exceed 4K from 960x544 and is not useful
// on a TV — 2x/3x/4x covers 1080p through 4K-class output.
//
// Values 0..2 keep the old Off/Sharp/FSR save-file encoding (fixed 2x); 3..6
// add the higher factors. Do not renumber 0..2.
enum class UpscaleMode : int
{
    off = 0,
    sharp2 = 1, // was "sharp" (2x)
    fsr2 = 2,   // was "fsr" (2x)
    sharp3 = 3,
    fsr3 = 4,
    sharp4 = 5,
    fsr4 = 6,
};

constexpr int kUpscaleModeCount = 7;
constexpr int kUpscaleModeMax = kUpscaleModeCount - 1;

// Integer scale factor for a mode (1 for off).
inline int vd_settings_upscale_factor(UpscaleMode mode)
{
    switch (mode)
    {
    case UpscaleMode::sharp2:
    case UpscaleMode::fsr2:
        return 2;
    case UpscaleMode::sharp3:
    case UpscaleMode::fsr3:
        return 3;
    case UpscaleMode::sharp4:
    case UpscaleMode::fsr4:
        return 4;
    case UpscaleMode::off:
    default:
        return 1;
    }
}

// True when the mode uses AMD FSR EASU (vs plain bilinear).
inline bool vd_settings_upscale_is_fsr(UpscaleMode mode)
{
    return mode == UpscaleMode::fsr2 || mode == UpscaleMode::fsr3 || mode == UpscaleMode::fsr4;
}

// True when the GPU chain should run (anything except off).
inline bool vd_settings_upscale_active(UpscaleMode mode)
{
    return mode != UpscaleMode::off;
}

// Where the Vita's touch input is sent.
enum class TouchTarget : int
{
    rear = 0,  // rear touchpad (default)
    front = 1, // front touchscreen
};

struct VdSettings
{
    // Output: an index into the kit's resolution table (1080p / 1440p / 4K).
    // Applies at the next launch (the EGL display is opened once at start).
    int resolution = 0;
    // Frame pacing: 60 or 30 presented frames per second.
    int fps_cap = 60;
    // Vita audio through the TV.
    bool stream_audio = true;
    int stream_volume = 8; // 0..10
    // Interface sounds.
    bool ui_sounds = true;
    int ui_volume = 7; // 0..10
    // On-screen readouts.
    bool show_fps = true;
    bool show_hud = true;
    // Controller feedback.
    bool haptics = true;
    bool light_bar = true;
    // Input passthrough.
    bool input_passthrough = true;
    TouchTarget touch_target = TouchTarget::front;
    // Video upscaling (owner default: off). Cycled live with L3+D-Pad.
    UpscaleMode upscale = UpscaleMode::off;

    static float gain(int volume) // 0..10 -> 0..1, matching the kit's curve
    {
        const float t = (float)volume / 10.0f;
        return t * t;
    }
};

std::string encode_settings(const VdSettings &settings);
// Clamps out-of-range values; malformed lines are ignored. Returns false
// (leaving *settings untouched) only when the text is not a settings file.
bool decode_settings(std::string_view data, VdSettings *settings);

bool load_settings(const std::string &path, VdSettings *settings);
bool save_settings(const std::string &path, const VdSettings &settings);

// Input getters — consumed by pad_bridge/touch_bridge via app wiring.
bool vd_settings_input_passthrough(const VdSettings &settings);
TouchTarget vd_settings_touch_target(const VdSettings &settings);
UpscaleMode vd_settings_upscale(const VdSettings &settings);
// Short label for the settings row and the L3 toast ("Off" / "Sharp 2x" / …).
const char *vd_settings_upscale_label(UpscaleMode mode);
// Advance/retreat the upscale enum with wrap (for L3 chords / settings).
UpscaleMode vd_settings_cycle_upscale(UpscaleMode mode, int direction);

} // namespace vd5
