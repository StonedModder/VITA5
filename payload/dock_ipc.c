/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dock_ipc.h"
#include "vd_ipc.h"
#include "vd_shm_prod.h"
#include "pad_passthrough.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef VD_BUILD_REV
#define VD_BUILD_REV "unknown"
#endif

#define VD_TOUCH_PENDING 32u
_Static_assert(sizeof(VdSharedHeader) <= VD_SHM_HEADER_SIZE, "RAM header overflow");

struct VdDockIpc
{
    VdSharedHeader *ring;
    int listener, peer, hello, hello_sent, status_sent;
    VdIpcRx rx;
    uint8_t input[VD_IPC_HELLO_BYTES];
    /* Exactly one pending output packet; reused for all message kinds. */
    uint8_t *packet;
    size_t length, offset;
    uint64_t last_input, last_status, tx_started;
    uint32_t audio_seen[VD_AUDIO_SLOTS], video_seen[VD_VIDEO_SLOTS];
    unsigned audio_cursor, prefer_video;
    uint32_t last_pad_id;
    uint8_t touch_pending[VD_TOUCH_PENDING][VD_TOUCH_WIRE_BYTES];
    unsigned touch_head, touch_count;
};

static uint32_t load(const volatile uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void store(volatile uint32_t *p, uint32_t n)
{
    __atomic_store_n(p, n, __ATOMIC_RELEASE);
}
static void publish_pad(VdDockIpc *s, const uint8_t *wire)
{
    VdSharedHeader *h = s->ring;
    unsigned slot = load(&h->pad_producer) % VD_PAD_SLOTS;
    uint32_t seq = load(&h->pad_slot_seq[slot]);
    store(&h->pad_slot_seq[slot], seq + 1u);
    memset(VD_SHM_PAD_PTR(h, slot), 0, VD_PAD_REPORT_BYTES);
    memcpy(VD_SHM_PAD_PTR(h, slot), wire, VD_PAD_WIRE_BYTES);
    store(&h->pad_slot_seq[slot], seq + 2u);
    store(&h->pad_producer, (slot + 1u) % VD_PAD_SLOTS);
    s->last_pad_id = vd_ipc_get_u32(wire);
}
/* Bounded FIFO behind one immutable, USB-acknowledged mailbox. Full FIFO
 * backpressures socket reads (including pads/pings); the normal peer timeout
 * then cancels queued holds and reserves two disconnect releases. No heap
 * growth and no assumption that USB is present or fast. */
static void pump_touch(VdDockIpc *s)
{
    VdSharedHeader *h = s->ring;
    uint32_t seq = load(&h->touch_seq);
    if (!s->touch_count || (seq & 1u) || load(&h->touch_ack_seq) != seq)
        return;
    store(&h->touch_seq, seq + 1u);
    memcpy(h->touch_wire, s->touch_pending[s->touch_head], VD_TOUCH_WIRE_BYTES);
    store(&h->touch_seq, seq + 2u);
    s->touch_head = (s->touch_head + 1u) % VD_TOUCH_PENDING;
    --s->touch_count;
}
static void publish_touch(VdDockIpc *s, const uint8_t *wire)
{
    unsigned tail = (s->touch_head + s->touch_count) % VD_TOUCH_PENDING;
    memcpy(s->touch_pending[tail], wire, VD_TOUCH_WIRE_BYTES);
    ++s->touch_count;
    pump_touch(s);
}
static void reset_peer(VdDockIpc *s, uint64_t now)
{
    if (s->peer >= 0)
    {
        uint8_t pad[VD_PAD_WIRE_BYTES], touch[VD_TOUCH_WIRE_BYTES];
        VdPadReport r;
        vd_pad_make_report(&r, s->last_pad_id + 1u, now * 1000u, 0, 0, 0, 0, 0, 0, 0);
        vd_pad_serialize(&r, pad, sizeof(pad));
        uint32_t epoch = load(&s->ring->pad_reset_epoch);
        store(&s->ring->pad_reset_epoch, epoch + 1u);
        /* Invalidate EVERY old held slot before publishing reset completion.
         * A paused worker must never send neutral then replay older holds. */
        for (unsigned i = 0; i < VD_PAD_SLOTS; ++i)
            publish_pad(s, pad);
        store(&s->ring->pad_reset_epoch, epoch + 2u);
        /* Keep the outstanding mailbox immutable, but discard queued holds.
         * Releases for BOTH ports precede all input from the next peer. */
        s->touch_head = s->touch_count = 0;
        memset(touch, 0, sizeof(touch));
        publish_touch(s, touch);
        touch[0] = 1;
        publish_touch(s, touch);
        close(s->peer);
    }
    s->peer = -1;
    s->hello = s->hello_sent = s->status_sent = 0;
    s->length = s->offset = 0;
    vd_ipc_rx_reset(&s->rx);
}
static void prime_audio(VdDockIpc *s)
{
    for (unsigned i = 0; i < VD_AUDIO_SLOTS; ++i)
        /* Also skip a chunk already being produced at attachment. */
        s->audio_seen[i] = (load(&s->ring->audio_slot_seq[i]) + 1u) & ~1u;
    s->audio_cursor = load(&s->ring->audio_producer) % VD_AUDIO_SLOTS;
}
static int queue(VdDockIpc *s, uint32_t type, const void *body, size_t n, uint64_t now)
{
    s->length = vd_ipc_packet(s->packet, VD_IPC_MAX_PACKET, type, body, n);
    s->offset = 0;
    s->tx_started = now;
    return s->length != 0;
}
static int queue_status(VdDockIpc *s, uint64_t now)
{
    VdSharedHeader *h = s->ring;
    uint8_t body[VD_IPC_STATUS_BYTES];
    const uint32_t words[VD_IPC_STATUS_WORDS] = {
        load(&h->vita_detected),  load(&h->stream_active),      load(&h->video_width),
        load(&h->video_height),   load(&h->video_pixel_format), load(&h->audio_sample_rate),
        load(&h->audio_channels), load(&h->audio_bits),         load(&h->frames_captured),
        load(&h->frames_dropped), load(&h->audio_chunks),       load(&h->pad_reports),
        load(&h->last_error)};
    for (unsigned i = 0; i < VD_IPC_STATUS_WORDS; ++i)
        vd_ipc_put_u32(body + 4u * i, words[i]);
    s->last_status = now;
    s->status_sent = 1;
    return queue(s, VD_IPC_STATUS, body, sizeof(body), now);
}
static int queue_audio(VdDockIpc *s, uint64_t now)
{
    VdSharedHeader *h = s->ring;
    unsigned slot = s->audio_cursor;
    uint32_t seq = load(&h->audio_slot_seq[slot]);
    /* An overwritten unread slot means the retained oldest record is now
     * at the producer cursor. Rebase only the read cursor, never storage. */
    if (!(seq & 1u) && seq != s->audio_seen[slot] && (uint32_t)(seq - s->audio_seen[slot]) > 2u)
    {
        slot = load(&h->audio_producer) % VD_AUDIO_SLOTS;
        s->audio_cursor = slot;
        seq = load(&h->audio_slot_seq[slot]);
    }
    /* At most a ring scan: skip stale slots (including the in-flight chunk
     * skipped during prime), but never pass a currently writing slot. */
    for (unsigned i = 0; i < VD_AUDIO_SLOTS; ++i)
    {
        if (seq & 1u)
            return 0;
        if (seq != s->audio_seen[slot])
            break;
        slot = (slot + 1u) % VD_AUDIO_SLOTS;
        seq = load(&h->audio_slot_seq[slot]);
        if (i + 1u == VD_AUDIO_SLOTS)
            return 0;
    }
    uint8_t *body = s->packet + VD_IPC_HEADER_BYTES;
    memcpy(body, VD_SHM_AUDIO_PTR(h, slot), VD_AUDIO_CHUNK_BYTES);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (seq != load(&h->audio_slot_seq[slot]))
        return 0;
    s->audio_seen[slot] = seq;
    s->audio_cursor = (slot + 1u) % VD_AUDIO_SLOTS;
    return queue(s, VD_IPC_AUDIO, body, VD_AUDIO_CHUNK_BYTES, now);
}
static int queue_video(VdDockIpc *s, uint64_t now)
{
    VdSharedHeader *h = s->ring;
    unsigned next = load(&h->video_producer) % VD_VIDEO_SLOTS;
    unsigned slot = (next + VD_VIDEO_SLOTS - 1u) % VD_VIDEO_SLOTS;
    uint32_t seq = load(&h->video_slot_seq[slot]);
    if (!seq || (seq & 1u) || seq == s->video_seen[slot])
        return 0;
    uint32_t w = load(&h->video_width), ht = load(&h->video_height);
    uint32_t fmt = load(&h->video_pixel_format);
    size_t bytes = vd_ipc_video_bytes(w, ht, fmt);
    if (!bytes)
        return 0;
    uint8_t *body = s->packet + VD_IPC_HEADER_BYTES;
    vd_ipc_put_u32(body, w);
    vd_ipc_put_u32(body + 4, ht);
    vd_ipc_put_u32(body + 8, fmt);
    memcpy(body + VD_IPC_VIDEO_META_BYTES, VD_SHM_VIDEO_PTR(h, slot), bytes);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (seq != load(&h->video_slot_seq[slot]) ||
        next != load(&h->video_producer) % VD_VIDEO_SLOTS || w != load(&h->video_width) ||
        ht != load(&h->video_height) || fmt != load(&h->video_pixel_format))
        return 0;
    s->video_seen[slot] = seq;
    return queue(s, VD_IPC_VIDEO, body, VD_IPC_VIDEO_META_BYTES + bytes, now);
}

VdDockIpc *vd_dock_ipc_open(void *ring, size_t bytes, uint16_t port, uint16_t *actual_port)
{
    if (!ring || bytes < VD_SHM_TOTAL_BYTES || !vd_shm_prod_contract_ok(ring))
    {
        errno = EINVAL;
        return NULL;
    }
    VdDockIpc *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->ring = ring;
    s->peer = s->listener = -1;
    s->packet = malloc(VD_IPC_MAX_PACKET);
    if (!s->packet)
    {
        free(s);
        return NULL;
    }
    int fd = vd_ipc_listen(port, actual_port);
    if (fd < 0)
    {
        free(s->packet);
        free(s);
        errno = -fd;
        return NULL;
    }
    s->listener = fd;
    return s;
}
int vd_dock_ipc_connected(const VdDockIpc *s)
{
    return s && s->peer >= 0 && s->hello;
}
int vd_dock_ipc_tick(VdDockIpc *s, uint64_t now)
{
    if (!s || s->listener < 0)
        return -EINVAL;
    /* Releases continue draining even with no IPC peer attached. */
    pump_touch(s);
    if (s->peer < 0)
    {
        int fd = vd_ipc_accept(s->listener);
        if (fd < 0)
            return 0; /* Transient accept failure never stops USB. */
        s->peer = fd;
        s->last_input = now;
        s->prefer_video = 0;
        memset(s->video_seen, 0, sizeof(s->video_seen));
        prime_audio(s);
    }
    if (now - s->last_input >= VD_IPC_PEER_IDLE_MS ||
        (s->length && now - s->tx_started >= VD_IPC_PEER_IDLE_MS))
    {
        reset_peer(s, now);
        return 0;
    }
    for (unsigned i = 0; i < 8u; ++i)
    {
        pump_touch(s);
        if (s->touch_count == VD_TOUCH_PENDING)
            break; /* Reserve storage BEFORE receiving any next record. */
        int rc = vd_ipc_receive(s->peer, &s->rx, s->input, sizeof(s->input));
        if (rc < 0)
        {
            reset_peer(s, now);
            return 0;
        }
        if (!rc)
            break;
        uint32_t type = s->rx.type;
        if (!s->hello)
        {
            if (type != VD_IPC_HELLO)
            {
                reset_peer(s, now);
                return 0;
            }
            s->hello = 1;
            printf("[vd5-ipc] peer hello RAM=%u wire=%u pid=%u rev=%.31s\n",
                   vd_ipc_get_u32(s->input), VD_IPC_VERSION, vd_ipc_get_u32(s->input + 4),
                   (const char *)s->input + 8);
        }
        else if (type == VD_IPC_PAD)
            publish_pad(s, s->input);
        else if (type == VD_IPC_TOUCH)
            publish_touch(s, s->input);
        else if (type != VD_IPC_PING)
        {
            reset_peer(s, now);
            return 0;
        }
        s->last_input = now;
        vd_ipc_rx_reset(&s->rx);
    }
    for (unsigned i = 0; i < 4u; ++i)
    {
        if (!s->length)
        {
            if (!s->hello_sent)
            {
                uint8_t hello[VD_IPC_HELLO_BYTES];
                vd_ipc_make_hello(hello, VD_BUILD_REV, (uint32_t)getpid());
                queue(s, VD_IPC_HELLO, hello, sizeof(hello), now);
                s->hello_sent = 1;
            }
            else if (!s->hello)
                break;
            else if (!s->status_sent || now - s->last_status >= 250u)
                queue_status(s, now);
            else
            {
                int queued;
                if (s->prefer_video)
                {
                    queued = queue_video(s, now);
                    if (!queued)
                        queued = queue_audio(s, now);
                }
                else
                {
                    queued = queue_audio(s, now);
                    if (!queued)
                        queued = queue_video(s, now);
                }
                s->prefer_video ^= 1u;
                if (!queued)
                    break;
            }
        }
        int rc = vd_ipc_send(s->peer, s->packet, s->length, &s->offset);
        if (rc < 0)
        {
            reset_peer(s, now);
            return 0;
        }
        if (!rc)
            break;
        s->length = s->offset = 0;
    }
    return 0;
}
void vd_dock_ipc_close(VdDockIpc *s)
{
    if (!s)
        return;
    reset_peer(s, vd_ipc_now_ms());
    if (s->listener >= 0)
        close(s->listener);
    free(s->packet);
    free(s);
}
