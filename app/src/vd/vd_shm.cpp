// VITA5 app — shared region reader/writer. See vd_shm.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vd_shm.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string.h>
#include <unistd.h>

#ifndef VD_APP_REV
#define VD_APP_REV unknown
#endif
#define VD_STRINGIFY_INNER(x) #x
#define VD_STRINGIFY(x) VD_STRINGIFY_INNER(x)

namespace vd5
{

namespace
{

// Acquire/release on the contract's volatile seq fields. The protocol is
// plain C on the kernel side; C11/C++ atomic builtins operate on the same
// storage without changing the layout.
inline std::uint32_t seq_load(const volatile std::uint32_t *seq)
{
    return __atomic_load_n(seq, __ATOMIC_ACQUIRE);
}

inline void seq_store(volatile std::uint32_t *seq, std::uint32_t value)
{
    __atomic_store_n(seq, value, __ATOMIC_RELEASE);
}

inline void copy_slot(void *dst, const void *src, std::size_t bytes)
{
    memcpy(dst, src, bytes);
}

} // namespace

Shm::~Shm()
{
    detach();
}

bool Shm::attach(std::uint16_t port)
{
    if (attached())
        return true;
    if (fd_ >= 0)
    {
        transport_poll();
        return attached();
    }
    detach();
    base_ = static_cast<std::uint8_t *>(calloc(1, VD_SHM_TOTAL_BYTES));
    incoming_ = static_cast<std::uint8_t *>(malloc(VD_IPC_MAX_BODY));
    owns_memory_ = true;
    if (base_ == nullptr || incoming_ == nullptr)
    {
        transport_fail("cannot allocate the local IPC ring; retry attachment");
        return false;
    }
    bytes_ = VD_SHM_TOTAL_BYTES;
    header_ = reinterpret_cast<VdSharedHeader *>(base_);
    header_->magic = VD_SHM_MAGIC;
    header_->version = VD_SHM_VERSION;
    init_audio_cursor();
    fd_ = vd_ipc_connect(port);
    if (fd_ < 0)
    {
        transport_fail("dock IPC unavailable on loopback; start the matching dock and retry");
        return false;
    }
    deadline_ = vd_ipc_now_ms() + 2000u;
    error_ = "waiting for dock IPC connection and matching HELLO";
    transport_poll();
    return attached();
}

bool Shm::attach_memory(void *base, std::size_t bytes)
{
    detach();
    if (base == nullptr || bytes < VD_SHM_TOTAL_BYTES)
    {
        error_ = "shared region is smaller than the contract";
        return false;
    }
    base_ = static_cast<std::uint8_t *>(base);
    bytes_ = bytes;
    owns_memory_ = false;
    header_ = reinterpret_cast<VdSharedHeader *>(base_);
    init_audio_cursor();
    error_ = "";
    return true;
}

// The audio ring is consumed strictly in order; everything produced before
// attach is stale sound and is skipped by marking those chunks consumed and
// starting the cursor where the producer is writing.
void Shm::init_audio_cursor()
{
    for (std::uint32_t i = 0; i < VD_AUDIO_SLOTS; ++i)
        audio_seen_[i] = seq_load(slot_seq_audio((int)i));
    audio_cursor_ = (int)(header_->audio_producer % VD_AUDIO_SLOTS);
    audio_cursor_valid_ = true;
}

void Shm::detach()
{
    if (owns_memory_)
        free(base_);
    free(incoming_);
    incoming_ = nullptr;
    connected_ = false;
    hello_ok_ = false;
    deadline_ = last_receive_ = last_ping_ = 0;
    vd_ipc_rx_reset(&rx_);
    rx_ready_ = false;
    outgoing_length_ = outgoing_offset_ = 0;
    for (auto &pending : pending_)
        pending = PendingInput{};
    if (fd_ >= 0)
        close(fd_);
    base_ = nullptr;
    bytes_ = 0;
    owns_memory_ = false;
    fd_ = -1;
    header_ = nullptr;
    for (std::uint32_t &seen : video_seen_)
        seen = 0;
    for (std::uint32_t &seen : audio_seen_)
        seen = 0;
    audio_cursor_ = 0;
    audio_cursor_valid_ = false;
    pad_next_ = 0;
    stats_ = Stats{};
    fps_window_ = 0.0f;
    fps_base_ = 0;
}

bool Shm::contract_ok() const
{
    return attached() && header_->magic == VD_SHM_MAGIC && header_->version == VD_SHM_VERSION;
}

bool Shm::vita_detected() const
{
    return attached() && header_->vita_detected != 0;
}

bool Shm::stream_active() const
{
    return attached() && header_->stream_active != 0;
}

std::uint32_t Shm::service_error() const
{
    return header_ != nullptr ? header_->last_error : VD_ERR_NONE;
}

const volatile std::uint32_t *Shm::slot_seq_video(int slot) const
{
    return &header_->video_slot_seq[slot];
}

const volatile std::uint32_t *Shm::slot_seq_audio(int slot) const
{
    return &header_->audio_slot_seq[slot];
}

volatile std::uint32_t *Shm::slot_seq_pad(int slot)
{
    return &header_->pad_slot_seq[slot];
}

bool Shm::read_video(std::uint8_t *out, std::size_t cap, std::uint32_t *width,
                     std::uint32_t *height)
{
    if (!attached() || out == nullptr || cap < VD_VIDEO_MAX_FRAME)
        return false;

    // Newest first: the producer writes into video_producer, so the slot
    // before it is the one most recently released.
    const std::uint32_t producer = header_->video_producer;
    for (int attempt = 0; attempt < (int)VD_VIDEO_SLOTS; ++attempt)
    {
        const int slot =
            (int)((producer + VD_VIDEO_SLOTS - 1u - (std::uint32_t)attempt) % VD_VIDEO_SLOTS);
        const std::uint32_t before = seq_load(slot_seq_video(slot));
        if ((before & 1u) != 0)
            continue; // producer is filling this slot
        if (before == video_seen_[slot])
            continue; // nothing new in this slot
        const void *src = VD_SHM_VIDEO_PTR(base_, (std::uint32_t)slot);
        copy_slot(out, src, VD_VIDEO_MAX_FRAME);
        const std::uint32_t after = seq_load(slot_seq_video(slot));
        if (after != before)
        {
            ++stats_.video_torn; // torn copy: drop and retry next frame
            continue;
        }
        video_seen_[slot] = before;
        ++stats_.video_frames;
        if (width != nullptr)
            *width = header_->video_width;
        if (height != nullptr)
            *height = header_->video_height;
        return true;
    }
    return false;
}

bool Shm::read_audio(void *out, std::size_t cap)
{
    if (!attached() || out == nullptr || cap < VD_AUDIO_CHUNK_BYTES)
        return false;

    if (!audio_cursor_valid_)
    {
        // Defensive: attach_memory()/attach() normally set the cursor.
        for (std::uint32_t i = 0; i < VD_AUDIO_SLOTS; ++i)
            audio_seen_[i] = seq_load(slot_seq_audio((int)i));
        audio_cursor_ = (int)(header_->audio_producer % VD_AUDIO_SLOTS);
        audio_cursor_valid_ = true;
    }

    const int slot = audio_cursor_;
    const std::uint32_t before = seq_load(slot_seq_audio(slot));
    if ((before & 1u) != 0 || before == audio_seen_[slot])
        return false; // producer mid-write, or nothing new yet
    copy_slot(out, VD_SHM_AUDIO_PTR(base_, (std::uint32_t)slot), VD_AUDIO_CHUNK_BYTES);
    const std::uint32_t after = seq_load(slot_seq_audio(slot));
    if (after != before)
    {
        ++stats_.audio_overruns;
        return false;
    }
    audio_seen_[slot] = before;
    audio_cursor_ = (slot + 1) % (int)VD_AUDIO_SLOTS;
    ++stats_.audio_chunks;
    return true;
}

bool Shm::publish_pad(const VdPadReport &report)
{
    if (!attached())
        return false;
    if (fd_ >= 0)
    {
        std::uint8_t body[VD_PAD_WIRE_BYTES];
        vd_pad_serialize(&report, body, sizeof(body));
        const bool neutral = report.buttons == 0 && report.left_x == 0 && report.left_y == 0 &&
                             report.right_x == 0 && report.right_y == 0 && report.l2 == 0 &&
                             report.r2 == 0;
        // A neutral must not be replaced by newer held input under backpressure.
        if (neutral)
            pending_[3].ready = false;
        const bool queued = queue_input(VD_IPC_PAD, body, sizeof(body), neutral ? 0u : 3u);
        if (queued)
            ++stats_.pad_reports;
        return queued;
    }

    const std::uint32_t slot = pad_next_;
    volatile std::uint32_t *seq = slot_seq_pad((int)slot);
    const std::uint32_t before = seq_load(seq);
    seq_store(seq, before + 1u); // odd: filling
    std::uint8_t *dst = VD_SHM_PAD_PTR(base_, slot);
    memset(dst, 0, VD_PAD_REPORT_BYTES);
    vd_pad_serialize(&report, dst, VD_PAD_REPORT_BYTES);
    seq_store(seq, before + 2u); // even: released
    pad_next_ = (slot + 1u) % VD_PAD_SLOTS;
    header_->pad_producer = pad_next_;
    ++stats_.pad_reports;
    return true;
}

bool Shm::publish_touch(const VdTouchReport &report)
{
    if (!attached())
        return false;
    if (fd_ >= 0)
    {
        std::uint8_t body[VD_TOUCH_WIRE_BYTES];
        vd_touch_serialize(&report, body);
        if (vd_ipc_validate_body(VD_IPC_TOUCH, body, sizeof(body)) != 0)
            return false;
        const bool release = report.count == 0;
        const unsigned port = report.port;
        if (release)
            pending_[4u + port].ready = false;
        return queue_input(VD_IPC_TOUCH, body, sizeof(body), release ? 1u + port : 4u + port);
    }

    // Latest-wins single record (touch records REPLACE state on the Vita):
    // fill the wire bytes, then release with an even sequence so the kernel
    // forwarder can spot a torn fill and retry.
    volatile std::uint32_t *seq = &header_->touch_seq;
    const std::uint32_t before = seq_load(seq);
    seq_store(seq, before + 1u); // odd: filling
    vd_touch_serialize(&report, base_ + offsetof(VdSharedHeader, touch_wire));
    seq_store(seq, before + 2u); // even: released
    return true;
}

void Shm::transport_fail(const char *error)
{
    detach();
    error_ = error;
}

bool Shm::queue_input(std::uint32_t type, const std::uint8_t *body, std::size_t length,
                      unsigned slot)
{
    if (!attached() || slot >= 6u || length > VD_PAD_WIRE_BYTES ||
        vd_ipc_validate_body(type, body, length) != 0)
        return false;
    auto &pending = pending_[slot];
    memcpy(pending.body, body, length);
    pending.type = type;
    pending.length = length;
    pending.ready = true;
    // Accepted means retained in bounded staging, not necessarily sent yet.
    // In-flight bytes are separate and cannot be overwritten here.
    return true;
}

bool Shm::receive_record()
{
    if (!hello_ok_)
    {
        if (rx_.type != VD_IPC_HELLO)
        {
            transport_fail("dock IPC requires server HELLO first; install the matching dock");
            return false;
        }
        hello_ok_ = true;
        error_ = "";
        std::printf("[VITA5 IPC] dock revision=%.31s wire=%u RAM=%u app=%s\n",
                    reinterpret_cast<const char *>(incoming_ + 8), VD_IPC_VERSION,
                    vd_ipc_get_u32(incoming_), VD_STRINGIFY(VD_APP_REV));
        return true;
    }
    switch (rx_.type)
    {
    case VD_IPC_STATUS:
    {
        const auto word = [this](unsigned field) { return vd_ipc_get_u32(incoming_ + field * 4u); };
        header_->vita_detected = word(VD_IPC_VITA);
        header_->stream_active = word(VD_IPC_STREAM);
        // Frame metadata must stay with the unread frame, not a later status.
        bool unread = false;
        for (unsigned i = 0; i < VD_VIDEO_SLOTS; ++i)
            unread = unread || seq_load(slot_seq_video((int)i)) != video_seen_[i];
        if (!unread)
        {
            header_->video_width = word(VD_IPC_WIDTH);
            header_->video_height = word(VD_IPC_HEIGHT);
            header_->video_pixel_format = word(VD_IPC_FORMAT);
        }
        header_->audio_sample_rate = word(VD_IPC_AUDIO_RATE);
        header_->audio_channels = word(VD_IPC_AUDIO_CHANNELS);
        header_->audio_bits = word(VD_IPC_AUDIO_BITS);
        header_->frames_captured = word(VD_IPC_CAPTURED);
        header_->frames_dropped = word(VD_IPC_DROPPED);
        header_->audio_chunks = word(VD_IPC_AUDIO_CHUNKS);
        header_->pad_reports = word(VD_IPC_PAD_REPORTS);
        header_->last_error = word(VD_IPC_ERROR);
        return true;
    }
    case VD_IPC_VIDEO:
    {
        // Retire older local frames: the socket video adapter is latest-wins.
        for (unsigned i = 0; i < VD_VIDEO_SLOTS; ++i)
            video_seen_[i] = seq_load(slot_seq_video((int)i));
        const unsigned slot = header_->video_producer % VD_VIDEO_SLOTS;
        auto *seq = &header_->video_slot_seq[slot];
        const auto before = seq_load(seq);
        seq_store(seq, before + 1u);
        const std::size_t bytes = rx_.length - VD_IPC_VIDEO_META_BYTES;
        auto *destination = VD_SHM_VIDEO_PTR(base_, slot);
        memcpy(destination, incoming_ + VD_IPC_VIDEO_META_BYTES, bytes);
        // Existing readers copy a whole fixed-size slot; clear stale tail bytes.
        memset(destination + bytes, 0, VD_VIDEO_MAX_FRAME - bytes);
        header_->video_width = vd_ipc_get_u32(incoming_);
        header_->video_height = vd_ipc_get_u32(incoming_ + 4);
        header_->video_pixel_format = vd_ipc_get_u32(incoming_ + 8);
        seq_store(seq, before + 2u);
        header_->video_producer = (slot + 1u) % VD_VIDEO_SLOTS;
        return true;
    }
    case VD_IPC_AUDIO:
    {
        const unsigned slot = header_->audio_producer % VD_AUDIO_SLOTS;
        auto *seq = &header_->audio_slot_seq[slot];
        const auto before = seq_load(seq);
        if (before != audio_seen_[slot])
        {
            // Playback stalled long enough to fill local PCM storage. Retire
            // the oldest chunk instead of blocking the shared socket stream:
            // STATUS/video and peer health must remain independent of playback.
            audio_seen_[slot] = before;
            audio_cursor_ = (static_cast<int>(slot) + 1) % static_cast<int>(VD_AUDIO_SLOTS);
            ++stats_.audio_overruns;
        }
        seq_store(seq, before + 1u);
        memcpy(VD_SHM_AUDIO_PTR(base_, slot), incoming_, VD_AUDIO_CHUNK_BYTES);
        seq_store(seq, before + 2u);
        header_->audio_producer = (slot + 1u) % VD_AUDIO_SLOTS;
        return true;
    }
    default:
        transport_fail("invalid dock IPC direction or repeated HELLO; install the matching dock");
        return false;
    }
}

void Shm::transport_poll()
{
    if (fd_ < 0)
        return; // attach_memory is a direct local ring, not a socket peer
    const auto now = vd_ipc_now_ms();
    if ((!hello_ok_ && now >= deadline_) || (hello_ok_ && now - last_receive_ >= 2000u))
    {
        transport_fail(hello_ok_ ? "dock IPC health timed out; retry the dock connection"
                                 : "dock IPC connect/HELLO timed out; start the matching dock");
        return;
    }
    if (!connected_)
    {
        const int result = vd_ipc_connected(fd_);
        if (result < 0)
        {
            transport_fail("dock IPC connection failed; start the matching dock and retry");
            return;
        }
        if (result == 0)
            return;
        connected_ = true;
        std::uint8_t hello[VD_IPC_HELLO_BYTES];
        vd_ipc_make_hello(hello, VD_STRINGIFY(VD_APP_REV), static_cast<std::uint32_t>(getpid()));
        outgoing_length_ =
            vd_ipc_packet(outgoing_, sizeof(outgoing_), VD_IPC_HELLO, hello, sizeof(hello));
        outgoing_offset_ = 0;
    }
    // Bounded outgoing work; PING is independent of input passthrough.
    for (unsigned work = 0; work < 8u; ++work)
    {
        if (outgoing_length_ == 0)
        {
            if (!hello_ok_)
                break;
            if (now - last_ping_ >= VD_IPC_PING_MS)
            {
                outgoing_length_ =
                    vd_ipc_packet(outgoing_, sizeof(outgoing_), VD_IPC_PING, nullptr, 0);
                last_ping_ = now;
            }
            else
            {
                for (auto &pending : pending_)
                {
                    if (!pending.ready)
                        continue;
                    outgoing_length_ = vd_ipc_packet(outgoing_, sizeof(outgoing_), pending.type,
                                                     pending.body, pending.length);
                    pending.ready = false;
                    break;
                }
            }
            outgoing_offset_ = 0;
            if (outgoing_length_ == 0)
                break;
        }
        const int result = vd_ipc_send(fd_, outgoing_, outgoing_length_, &outgoing_offset_);
        if (result < 0)
        {
            transport_fail("dock IPC send lost the peer; retry the dock connection");
            return;
        }
        if (result == 0)
            break;
        outgoing_length_ = outgoing_offset_ = 0;
    }
    for (unsigned work = 0; work < 32u; ++work)
    {
        // Complete validated packets are dispatched without PCM consumer stalls.
        const bool complete = rx_ready_;
        const int result = complete ? 1 : vd_ipc_receive(fd_, &rx_, incoming_, VD_IPC_MAX_BODY);
        if (result < 0)
        {
            transport_fail(
                result == -EPROTO
                    ? "dock IPC wire/RAM mismatch or malformed record; install matching app/dock"
                    : "dock IPC read lost the peer; retry the dock connection");
            return;
        }
        if (result == 0)
            break;
        if (!complete)
            last_receive_ = now;
        rx_ready_ = true;
        if (!receive_record())
            break;
        vd_ipc_rx_reset(&rx_);
        rx_ready_ = false;
    }
}

void Shm::poll(float dt)
{
    transport_poll();
    if (!attached() || dt <= 0.0f)
        return;
    const std::uint32_t captured = header_->frames_captured;
    const std::uint32_t dropped = header_->frames_dropped;
    fps_window_ += dt;
    if (fps_window_ >= 0.5f)
    {
        // Counter wrap is harmless: the delta stays correct in unsigned math.
        const std::uint32_t delta = captured - fps_base_;
        stats_.video_fps = (double)delta / (double)fps_window_;
        fps_base_ = captured;
        fps_window_ = 0.0f;
    }
    stats_.captured = captured;
    stats_.dropped = dropped;
}

} // namespace vd5
