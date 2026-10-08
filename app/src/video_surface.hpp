// VITA5 app — the GL texture that carries the Vita's video frames.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frames arrive as NV12 (vd_shm.hpp). The off path converts to RGBA on the
// CPU (nv12.hpp) and uploads here; the upscale modes (off by default) run
// the GPU chain in vd/upscaler.hpp instead (NV12->RGB, then EASU or a
// bilinear stretch, then RCAS). The draw list paints whichever texture
// texture() reports and the GPU scales it to the picture rect. Uploads use
// glTexSubImage2D into a single mip level (the kit's performance rules).

#pragma once
#include "vd/upscaler.hpp"

#include <cstdint>

namespace vd5
{

class VideoSurface
{
  public:
    VideoSurface() = default;
    VideoSurface(const VideoSurface &) = delete;
    VideoSurface &operator=(const VideoSurface &) = delete;
    ~VideoSurface();

    // Creates (or re-creates) the texture at width x height.
    bool create(int width, int height);
    void destroy();
    // Uploads a tightly packed RGBA8 frame (top row first).
    void upload(const std::uint8_t *rgba);
    // Runs the GPU upscale chain on a raw NV12 frame (chain_mode 1 = sharp
    // bilinear, 2 = FSR EASU; factor 2..4; chain_mode 0 clears the chain).
    // When the chain is unavailable the caller falls back to upload().
    void process_nv12(const std::uint8_t *nv12, int chain_mode, int factor);
    bool chain_ready() const
    {
        return chain_valid_;
    }

    // The texture to draw: the chain output when active, else the CPU path.
    std::uint32_t texture() const
    {
        return (chain_valid_ && upscaler_.output_texture() != 0) ? upscaler_.output_texture()
                                                                 : texture_;
    }
    // Source geometry (the NV12 frame). Prefer present_width/height for the
    // picture rect so an active upscale actually changes on-screen size.
    int width() const
    {
        return width_;
    }
    int height() const
    {
        return height_;
    }
    int present_width() const
    {
        return (chain_valid_ && upscaler_.output_width() > 0) ? upscaler_.output_width() : width_;
    }
    int present_height() const
    {
        return (chain_valid_ && upscaler_.output_height() > 0) ? upscaler_.output_height()
                                                              : height_;
    }
    bool ready() const
    {
        return texture_ != 0;
    }

  private:
    std::uint32_t texture_ = 0;
    int width_ = 0;
    int height_ = 0;
    Upscaler upscaler_;
    bool chain_valid_ = false;
};

} // namespace vd5
