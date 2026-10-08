// VITA5 app — Vita audio into the mixer.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dock service publishes 48 kHz interleaved stereo S16 chunks
// (VD_AUDIO_CHUNK_BYTES = 256 frames). This feeds them into the kit's
// StreamRing, which the mixer plays on the music bus and the AudioOut
// thread pushes through sceAudioOut (48 kHz, 256-frame grains).

#pragma once
#include "audio/stream_ring.hpp"

#include <atomic>
#include <cstdint>

namespace vd5
{

class AudioPipe
{
  public:
    AudioPipe() = default;
    AudioPipe(const AudioPipe &) = delete;
    AudioPipe &operator=(const AudioPipe &) = delete;

    // One VD_AUDIO_CHUNK_BYTES chunk of interleaved stereo S16.
    void feed(const void *s16_chunk, std::size_t bytes);
    void set_muted(bool muted)
    {
        muted_ = muted;
    }
    bool muted() const
    {
        return muted_;
    }

    hui::audio::StreamRing *stream()
    {
        return &ring_;
    }

    // Level metering for the HUD: peak of the last fed chunk, 0..1.
    float level() const
    {
        return level_.load(std::memory_order_relaxed);
    }
    std::uint64_t chunks_fed() const
    {
        return chunks_.load(std::memory_order_relaxed);
    }

  private:
    hui::audio::StreamRing ring_{1u << 14};
    bool muted_ = false;
    std::atomic<float> level_{0.0f};
    std::atomic<std::uint64_t> chunks_{0};
};

} // namespace vd5
