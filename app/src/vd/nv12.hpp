// VITA5 app — NV12 to RGBA conversion (the Vita's UVC frame format).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The confirmed wire format (docs/PS5_PROBE_FINDINGS.md) is NV12 at
// 960x544: a width*height Y plane followed by interleaved UV at half
// resolution. Conversion uses BT.601 limited-range coefficients, the same
// matrix the PC capture path used to decode the Vita's stream.

#pragma once
#include <cstdint>

namespace vd5
{

// Converts `width` x `height` NV12 (even dimensions) into tightly packed
// RGBA8 (top row first). `rgba` must hold width*height*4 bytes.
void nv12_to_rgba(const std::uint8_t *nv12, int width, int height, std::uint8_t *rgba);

// The same for one 2x2 block group starting at (x, y) — exported for tests.
void nv12_pixel(const std::uint8_t *y_plane, const std::uint8_t *uv_plane, int width,
                int x, int y, std::uint8_t out_rgba[4]);

} // namespace vd5
