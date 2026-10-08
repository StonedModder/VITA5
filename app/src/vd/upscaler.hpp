// VITA5 app — the GPU upscale chain for the Vita's NV12 video frames.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Chain: NV12 -> RGBA (shader), then either a bilinear stretch ("sharp"
// first pass) or AMD FSR 1.0 EASU ("fsr"), then RCAS sharpening. All passes
// are single-frame spatial filters: zero added latency. The output texture
// is factorx the source (e.g. 960x544 -> 1920x1080 at 2x, 2880x1632 at 3x,
// 3840x2176 at 4x) and the live view letterboxes that output into the stage.
//
// mode: 0 = off (never reaches this class), 1 = bilinear, 2 = FSR EASU.
// factor: integer 2..4.

#pragma once
#include <cstdint>

namespace vd5
{

class Upscaler
{
  public:
    Upscaler() = default;
    Upscaler(const Upscaler &) = delete;
    Upscaler &operator=(const Upscaler &) = delete;
    ~Upscaler();

    // Runs one NV12 frame through the chain. Returns the output texture
    // (RGBA8, factorx source) or 0 when the chain is unavailable — callers
    // then fall back to the CPU path and keep the last good picture.
    std::uint32_t run(const std::uint8_t *nv12, int width, int height, int mode, int factor);

    // The current output texture (0 until run() succeeds).
    std::uint32_t output_texture() const
    {
        return out_;
    }
    int output_width() const
    {
        return out_w_;
    }
    int output_height() const
    {
        return out_h_;
    }

    void shutdown();

  private:
    bool ensure_programs();
    bool ensure_targets(int width, int height, int factor);
    void destroy_targets();

    bool programs_failed_ = false; // retry only after explicit shutdown/reinitialization
    std::uint32_t prog_yuv_ = 0;
    std::uint32_t prog_blit_ = 0;
    std::uint32_t prog_easu_ = 0;
    std::uint32_t prog_rcas_ = 0;
    std::uint32_t vao_ = 0;

    std::uint32_t y_tex_ = 0;
    std::uint32_t uv_tex_ = 0;
    std::uint32_t work_ = 0; // RGBA8 at source size (yuv output)
    std::uint32_t tmp_ = 0;  // RGBA8 at factorx (blit/easu output)
    std::uint32_t out_ = 0;  // RGBA8 at factorx (rcas output)
    std::uint32_t fbo_ = 0;
    int src_w_ = 0;
    int src_h_ = 0;
    int factor_ = 0;
    int out_w_ = 0;
    int out_h_ = 0;
};

} // namespace vd5
