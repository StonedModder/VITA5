// VITA5 app — Vita audio feed. See audio_pipe.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio_pipe.hpp"

namespace vd5
{

void AudioPipe::feed(const void *s16_chunk, std::size_t bytes)
{
    if (s16_chunk == nullptr || bytes == 0)
        return;
    const std::size_t frames = bytes / 4; // stereo S16
    if (frames == 0)
        return;

    float peak = 0.0f;
    float converted[2 * 1024];
    const std::int16_t *in = static_cast<const std::int16_t *>(s16_chunk);
    std::size_t at = 0;
    while (at < frames)
    {
        const std::size_t batch = frames - at > 1024 ? 1024 : frames - at;
        for (std::size_t i = 0; i < batch * 2; ++i)
        {
            const float sample = (float)in[at * 2 + i] / 32768.0f;
            converted[i] = sample;
            const float magnitude = sample < 0.0f ? -sample : sample;
            if (magnitude > peak)
                peak = magnitude;
        }
        if (!muted_)
            ring_.write(converted, batch);
        at += batch;
    }
    level_.store(peak, std::memory_order_relaxed);
    chunks_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace vd5
