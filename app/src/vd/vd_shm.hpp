// VITA5 app — bounded local IPC adapter and process-owned ring consumer.
// SPDX-License-Identifier: GPL-3.0-or-later
// Socket records populate app-owned RAM; no sandbox files or shared mappings.
// attach_memory() retains the direct, caller-owned ring mode for host tests.

#pragma once
#include <cstddef>
#include <cstdint>

extern "C"
{
#include "pad_passthrough.h"
#include "vd_ipc.h"
}

namespace vd5
{

class Shm
{
  public:
    Shm() = default;
    Shm(const Shm &) = delete;
    Shm &operator=(const Shm &) = delete;
    ~Shm();

    // Starts/progresses a nonblocking loopback handshake. poll() must run
    // every frame, including while attach() returns false.
    bool attach(std::uint16_t port = VD_IPC_PORT);
    // Consumes a region owned by the caller (tests, future hand-offs).
    bool attach_memory(void *base, std::size_t bytes);
    void detach();
    bool attached() const
    {
        return base_ != nullptr && (fd_ < 0 || hello_ok_);
    }
    const char *last_error() const
    {
        return error_;
    }

    // ---- contract state (local header populated by IPC or the test producer) ----
    bool contract_ok() const; // magic + version match the header contract
    bool vita_detected() const;
    bool stream_active() const;
    std::uint32_t service_error() const; // VD_ERR_* from the header

    // ---- video (kernel -> app) ----
    // Copies the newest complete NV12 slot into `out` (>= VD_VIDEO_MAX_FRAME
    // bytes). Returns true only when a slot not seen before was copied;
    // width/height/format then describe the frame. Torn copies are dropped
    // and counted, never returned.
    bool read_video(std::uint8_t *out, std::size_t cap, std::uint32_t *width,
                    std::uint32_t *height);

    // ---- audio (kernel -> app) ----
    // Copies the oldest retained chunk (VD_AUDIO_CHUNK_BYTES of interleaved
    // 16-bit stereo @ 48 kHz). A full local PCM ring drops its oldest chunk
    // and counts the overrun; playback cannot block video/status or health.
    bool read_audio(void *out, std::size_t cap);

    // ---- pad (app -> kernel) ----
    // Stages exact wire records for IPC; neutral pad and per-target touch
    // releases cannot be replaced by newer held state. True means retained,
    // not necessarily sent yet. Direct attach_memory mode publishes into the
    // supplied ring, preserving the original producer contract.
    bool publish_pad(const VdPadReport &report);
    bool publish_touch(const VdTouchReport &report);

    // ---- observability for the on-screen readouts ----
    struct Stats
    {
        std::uint64_t video_frames = 0; // frames copied to the app
        std::uint64_t video_torn = 0;   // torn slots dropped
        std::uint64_t audio_chunks = 0;
        std::uint64_t audio_overruns = 0;
        std::uint64_t pad_reports = 0;
        std::uint32_t captured = 0; // header frames_captured at last poll
        std::uint32_t dropped = 0;  // header frames_dropped at last poll
        double video_fps = 0.0;     // from frames_captured deltas
    };
    const Stats &stats() const
    {
        return stats_;
    }
    // Call unconditionally once per frame, even before attached(): advances
    // nonblocking connect/HELLO, partial I/O, heartbeat and capture readouts.
    void poll(float dt);

  private:
    void init_audio_cursor();
    const volatile std::uint32_t *slot_seq_video(int slot) const;
    const volatile std::uint32_t *slot_seq_audio(int slot) const;
    volatile std::uint32_t *slot_seq_pad(int slot);

    std::uint8_t *base_ = nullptr;
    std::size_t bytes_ = 0;
    bool owns_memory_ = false; // calloc-owned; never shared with the payload
    bool connected_ = false;
    bool hello_ok_ = false;
    std::uint64_t deadline_ = 0;
    std::uint64_t last_receive_ = 0;
    std::uint64_t last_ping_ = 0;
    std::uint8_t *incoming_ = nullptr;
    VdIpcRx rx_{};
    bool rx_ready_ = false;
    std::uint8_t outgoing_[VD_IPC_HEADER_BYTES + VD_IPC_HELLO_BYTES] = {};
    std::size_t outgoing_length_ = 0;
    std::size_t outgoing_offset_ = 0;
    struct PendingInput
    {
        std::uint8_t body[VD_PAD_WIRE_BYTES] = {};
        std::uint32_t type = 0;
        std::size_t length = 0;
        bool ready = false;
    };
    // Protected neutral pad and per-target touch releases precede latest state.
    PendingInput pending_[6]{};
    void transport_poll();
    void transport_fail(const char *error);
    bool receive_record();
    bool queue_input(std::uint32_t type, const std::uint8_t *body, std::size_t length,
                     unsigned slot);
    int fd_ = -1;
    const char *error_ = "";
    VdSharedHeader *header_ = nullptr;

    std::uint32_t video_seen_[VD_VIDEO_SLOTS] = {};
    std::uint32_t audio_seen_[VD_AUDIO_SLOTS] = {};
    int audio_cursor_ = 0;
    bool audio_cursor_valid_ = false;
    std::uint32_t pad_next_ = 0;
    Stats stats_{};
    float fps_window_ = 0.0f;
    std::uint32_t fps_base_ = 0;
};

} // namespace vd5
