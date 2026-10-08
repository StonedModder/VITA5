// VITA5 app — NV12 to RGBA conversion. See nv12.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nv12.hpp"

#include <cstddef>

namespace vd5
{

namespace
{

inline std::uint8_t clamp_u8(int value)
{
    return (std::uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value));
}

} // namespace

void nv12_pixel(const std::uint8_t *y_plane, const std::uint8_t *uv_plane, int width, int x,
                int y, std::uint8_t out_rgba[4])
{
    // BT.601 limited range: Y 16..235, UV 16..240, the range the Vita's
    // UVC encoder produced in the PC capture.
    const int yv = (int)y_plane[y * width + x] - 16;
    const int uv_index = (y / 2) * width + (x & ~1);
    const int u = (int)uv_plane[uv_index + 0] - 128;
    const int v = (int)uv_plane[uv_index + 1] - 128;

    const int c = 298 * yv;
    out_rgba[0] = clamp_u8((c + 409 * v + 128) >> 8); // R
    out_rgba[1] = clamp_u8((c - 100 * u - 208 * v + 128) >> 8); // G
    out_rgba[2] = clamp_u8((c + 516 * u + 128) >> 8); // B
    out_rgba[3] = 255;
}

void nv12_to_rgba(const std::uint8_t *nv12, int width, int height, std::uint8_t *rgba)
{
    if (nv12 == nullptr || rgba == nullptr || width <= 0 || height <= 0)
        return;
    const std::uint8_t *y_plane = nv12;
    const std::uint8_t *uv_plane = nv12 + (std::size_t)width * height;
    for (int y = 0; y < height; ++y)
    {
        std::uint8_t *row = rgba + (std::size_t)y * width * 4;
        for (int x = 0; x < width; x += 2)
        {
            nv12_pixel(y_plane, uv_plane, width, x + 0, y, row + x * 4);
            if (x + 1 < width)
                nv12_pixel(y_plane, uv_plane, width, x + 1, y, row + (x + 1) * 4);
        }
    }
}

} // namespace vd5
