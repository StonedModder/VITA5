// VITA5 app — persisted settings. See vd_settings.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vd_settings.hpp"

#include "core/save_file.hpp"

#include <cstdio>
#include <cstdlib>

namespace vd5
{

namespace
{

int clamp_int(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

// Older builds stored upscale as 0=off / 1=sharp / 2=fsr (fixed 2x). Map
// those three values onto the new enum so a friend's save file still works.
// Values 3..6 already match the expanded enum and pass through.
UpscaleMode migrate_upscale(int value)
{
    if (value >= 0 && value <= kUpscaleModeMax)
        return (UpscaleMode)value;
    return UpscaleMode::off;
}

} // namespace

std::string encode_settings(const VdSettings &s)
{
    char line[128];
    std::string out;
    out.reserve(256);
    std::snprintf(line, sizeof(line), "resolution=%d\n", s.resolution);
    out += line;
    std::snprintf(line, sizeof(line), "fps_cap=%d\n", s.fps_cap);
    out += line;
    std::snprintf(line, sizeof(line), "stream_audio=%d\n", s.stream_audio ? 1 : 0);
    out += line;
    std::snprintf(line, sizeof(line), "stream_volume=%d\n", s.stream_volume);
    out += line;
    std::snprintf(line, sizeof(line), "ui_sounds=%d\n", s.ui_sounds ? 1 : 0);
    out += line;
    std::snprintf(line, sizeof(line), "ui_volume=%d\n", s.ui_volume);
    out += line;
    std::snprintf(line, sizeof(line), "show_fps=%d\n", s.show_fps ? 1 : 0);
    out += line;
    std::snprintf(line, sizeof(line), "show_hud=%d\n", s.show_hud ? 1 : 0);
    out += line;
    std::snprintf(line, sizeof(line), "haptics=%d\n", s.haptics ? 1 : 0);
    out += line;
    std::snprintf(line, sizeof(line), "light_bar=%d\n", s.light_bar ? 1 : 0);
    out += line;
    std::snprintf(line, sizeof(line), "input_passthrough=%d\n", s.input_passthrough ? 1 : 0);
    out += line;
    std::snprintf(line, sizeof(line), "touch_target=%d\n", (int)s.touch_target);
    out += line;
    std::snprintf(line, sizeof(line), "upscale=%d\n", (int)s.upscale);
    out += line;
    return out;
}

bool decode_settings(std::string_view data, VdSettings *settings)
{
    if (settings == nullptr)
        return false;
    if (data.find("resolution=") == std::string_view::npos)
        return false; // not a settings file

    VdSettings out = *settings;
    std::size_t at = 0;
    while (at < data.size())
    {
        std::size_t end = data.find('\n', at);
        if (end == std::string_view::npos)
            end = data.size();
        const std::string_view line = data.substr(at, end - at);
        at = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = line.substr(0, eq);
        const int value = (int)std::strtol(std::string(line.substr(eq + 1)).c_str(), nullptr, 10);
        if (key == "resolution")
            out.resolution = clamp_int(value, 0, 2);
        else if (key == "fps_cap")
            out.fps_cap = value >= 45 ? 60 : 30;
        else if (key == "stream_audio")
            out.stream_audio = value != 0;
        else if (key == "stream_volume")
            out.stream_volume = clamp_int(value, 0, 10);
        else if (key == "ui_sounds")
            out.ui_sounds = value != 0;
        else if (key == "ui_volume")
            out.ui_volume = clamp_int(value, 0, 10);
        else if (key == "show_fps")
            out.show_fps = value != 0;
        else if (key == "show_hud")
            out.show_hud = value != 0;
        else if (key == "haptics")
            out.haptics = value != 0;
        else if (key == "light_bar")
            out.light_bar = value != 0;
        else if (key == "input_passthrough")
            out.input_passthrough = value != 0;
        else if (key == "touch_target")
            out.touch_target =
                clamp_int(value, 0, 1) == 0 ? TouchTarget::rear : TouchTarget::front;
        else if (key == "upscale")
            out.upscale = migrate_upscale(value);
    }
    *settings = out;
    return true;
}

bool load_settings(const std::string &path, VdSettings *settings)
{
    std::string data;
    if (!hui::save::read_file(path, &data, 4096))
        return false;
    return decode_settings(data, settings);
}

bool save_settings(const std::string &path, const VdSettings &settings)
{
    // write_atomic returns an error string on failure, empty on success.
    return hui::save::write_atomic(path, encode_settings(settings)).empty();
}

bool vd_settings_input_passthrough(const VdSettings &settings)
{
    return settings.input_passthrough;
}

TouchTarget vd_settings_touch_target(const VdSettings &settings)
{
    return settings.touch_target;
}

UpscaleMode vd_settings_upscale(const VdSettings &settings)
{
    return settings.upscale;
}

const char *vd_settings_upscale_label(UpscaleMode mode)
{
    switch (mode)
    {
    case UpscaleMode::sharp2:
        return "Sharp 2x";
    case UpscaleMode::fsr2:
        return "FSR 2x";
    case UpscaleMode::sharp3:
        return "Sharp 3x";
    case UpscaleMode::fsr3:
        return "FSR 3x";
    case UpscaleMode::sharp4:
        return "Sharp 4x";
    case UpscaleMode::fsr4:
        return "FSR 4x";
    case UpscaleMode::off:
    default:
        return "Off";
    }
}

UpscaleMode vd_settings_cycle_upscale(UpscaleMode mode, int direction)
{
    int next = (int)mode + direction;
    if (next > kUpscaleModeMax)
        next = 0;
    if (next < 0)
        next = kUpscaleModeMax;
    return (UpscaleMode)next;
}

} // namespace vd5
