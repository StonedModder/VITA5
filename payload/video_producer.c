/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — video producer: continuous UVC capture into the shared video ring.
 *
 * A worker thread scans /dev/ugen* for the Vita, runs the UVC probe/commit
 * handshake through core/src/uvc_stream.c (hardware-verified bulk capture on
 * IN 0x81: one bulk request carries a 12-byte UVC header + the raw NV12
 * frame) and copies every complete frame into the next video ring slot.
 *
 * Ring protocol (single producer, per-slot release/acquire counters):
 *   seq = before + 1   (odd  = filling)
 *   memcpy frame into the slot
 *   seq = before + 2   (even = released)
 *   video_producer = (slot + 1) % VD_VIDEO_SLOTS
 *
 * The Vita may be absent or hot-plugged: the producer backs off and retries,
 * and repeated read failures tear the session down for a rescan. Stop is
 * cooperative: the worker notices g_stop, closes the UVC session itself and
 * exits; the stop call joins it.
 */
#include "video_producer.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "uvc_stream.h"
#include "vd_shared.h"

/* Negotiation target: the Vita's 960x544 NV12 mode (frame index 1). */
#define VP_TARGET_WIDTH 960u
#define VP_TARGET_HEIGHT 544u

/* Payload buffer: one whole UVC frame payload (frame + 12-byte header) plus
 * the same slack the endpoint buffer carries. */
#define VP_PAYLOAD_CAP (VD_VIDEO_MAX_FRAME + UVC_PAYLOAD_HEADER_SIZE + 4096u)

/* Absent-device retry back-off. */
#define VP_BACKOFF_MIN_MS 100u
#define VP_BACKOFF_MAX_MS 2000u

/* Consecutive payload read failures before the session is torn down and the
 * device is rescanned (repeated USB errors risk degrading the Vita's UDCD). */
#define VP_MAX_CONSEC_ERR 8u

/* Devices inspected per scan. */
#define VP_SCAN_SLOTS 4u

typedef struct
{
    uint8_t format_index;
    uint8_t frame_index;
    uint32_t interval;
    uint32_t max_frame_size;
    uint32_t width;
    uint32_t height;
    uint32_t publish_size; /* NV12 bytes copied per frame */
} VdStreamParams;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_started;
static volatile int g_stop;
static VdSharedHeader *g_hdr;
static uint8_t g_payload[VP_PAYLOAD_CAP];
static VdUsbVitaDevice g_scan[VP_SCAN_SLOTS];

static volatile uint32_t *slot_seq(VdSharedHeader *hdr, uint32_t slot)
{
    return (volatile uint32_t *)&hdr->video_slot_seq[slot];
}

/* Single video writer: retire every retained frame before changing global
 * geometry. An in-flight reader must observe a different sequence on its
 * final check; a fresh reader must see odd, never stale ready bytes. Do not
 * reset counters on restart (which would permit sequence ABA). */
static void retire_video(VdSharedHeader *hdr)
{
    for (unsigned i = 0; i < VD_VIDEO_SLOTS; ++i)
    {
        volatile uint32_t *seq = slot_seq(hdr, i);
        uint32_t before = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        __atomic_store_n(seq, (before + 1u) | 1u, __ATOMIC_RELEASE);
    }
    /* Retirement must precede metadata mutation, not just frame release. */
    __sync_synchronize();
}

/* Prompt-yieldable sleep so stop() does not wait out a whole back-off. */
static void wait_ms(unsigned ms)
{
    while (ms-- > 0 && !g_stop)
    {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000L;
        (void)nanosleep(&ts, NULL);
    }
}

static void backoff_grow(unsigned *backoff)
{
    if (*backoff < VP_BACKOFF_MAX_MS)
    {
        *backoff *= 2u;
        if (*backoff > VP_BACKOFF_MAX_MS)
        {
            *backoff = VP_BACKOFF_MAX_MS;
        }
    }
}

/* First scanned device that exposes a usable VideoStreaming bulk IN. */
static const VdUsbVitaDevice *find_vita(void)
{
    size_t n = vd_usb_scan_vita(g_scan, VP_SCAN_SLOTS);
    for (size_t i = 0; i < n; ++i)
    {
        if (g_scan[i].has_video && g_scan[i].video_in_count > 0)
        {
            return &g_scan[i];
        }
    }
    return NULL;
}

/* Pick the NV12 format and the 960x544 frame (falling back to frame 0). */
static int pick_stream(const VdUsbVitaDevice *dev, VdStreamParams *p)
{
    const VdUvcFormat *fmt = NULL;
    for (unsigned i = 0; i < dev->uvc.format_count; ++i)
    {
        const VdUvcFormat *f = &dev->uvc.formats[i];
        if (f->frame_count == 0)
        {
            continue;
        }
        if (f->is_nv12)
        {
            fmt = f;
            break;
        }
    }
    if (fmt == NULL)
    {
        return -1;
    }

    const VdUvcFrame *fr = &fmt->frames[0];
    for (unsigned i = 0; i < fmt->frame_count; ++i)
    {
        const VdUvcFrame *g = &fmt->frames[i];
        if (g->width == VP_TARGET_WIDTH && g->height == VP_TARGET_HEIGHT)
        {
            fr = g;
            break;
        }
    }

    uint32_t publish = VD_NV12_FRAME_SIZE(fr->width, fr->height);
    if (publish == 0 || publish > VD_VIDEO_MAX_FRAME || fr->max_frame_size == 0)
    {
        return -1;
    }

    p->format_index = fmt->format_index;
    p->frame_index = fr->frame_index;
    p->interval = fr->default_interval;
    p->max_frame_size = fr->max_frame_size;
    p->width = fr->width;
    p->height = fr->height;
    p->publish_size = publish;
    return 0;
}

/* Partial overwrite destroys the previous ready bytes. Keep the slot odd
 * until a subsequent complete frame replaces it; never restore readiness. */
static void frame_abandon(volatile uint32_t *seq, uint32_t seq_before)
{
    VdSharedHeader *hdr = g_hdr;
    __atomic_store_n(seq, seq_before + 1u, __ATOMIC_RELEASE);
    ++hdr->frames_dropped;
}

/* Read payloads until stop or a fatal run of read errors, publishing every
 * complete frame into the video ring. Returns 0 on stop request or fatal
 * error, 1 when the session went persistently idle and should be re-opened
 * from scratch. */
static int capture_loop(VdUvcSession *s, const VdStreamParams *p)
{
    VdSharedHeader *hdr = g_hdr;
    uint32_t slot = hdr->video_producer % VD_VIDEO_SLOTS;
    volatile uint32_t *seq = slot_seq(hdr, slot);
    uint32_t seq_before = *seq;
    uint32_t filled = 0;
    int filling = 0;
    unsigned consec_err = 0;
    unsigned idle_ticks = 0;

    while (!g_stop)
    {
        int frame_done = 0;
        int fid = 0;
        int got = vd_uvc_read_payload(s, g_payload, VP_PAYLOAD_CAP, &frame_done, &fid);
        if (got < 0)
        {
            if (filling)
            {
                frame_abandon(seq, seq_before);
                filling = 0;
            }
            if (got == -ETIMEDOUT)
            {
                /* Idle: the gadget is not sending (stream never started or
                 * was aborted). NOT an error - re-commit periodically (a
                 * COMMIT data stage that lands restarts the stream). If
                 * re-commits keep timing out, the session itself is stale:
                 * return 1 so the caller re-opens from scratch (the proven
                 * unwedge path - streamdiag's fresh-open streamed clean). */
                if (++idle_ticks >= 8)
                {
                    printf("[video] idle after re-commits; reopening session\n");
                    return 1;
                }
                if ((idle_ticks & 1u) == 0u)
                    vd_uvc_recommit(s);
                continue;
            }
            hdr->last_error = (got == -3) ? VD_ERR_BAD_FRAME_HEADER : VD_ERR_BULK_READ_FAILED;
            if (++consec_err >= VP_MAX_CONSEC_ERR)
            {
                printf("[video] %u consecutive read errors; reopening session\n", consec_err);
                return 1;
            }
            continue;
        }
        consec_err = 0;
        idle_ticks = 0;

        /* Pixel data starts after the UVC payload header (buf[0] = length). */
        unsigned hdr_len = g_payload[0];
        if (hdr_len < 2 || hdr_len + (unsigned)got > VP_PAYLOAD_CAP)
        {
            hdr->last_error = VD_ERR_BAD_FRAME_HEADER;
            if (filling)
            {
                frame_abandon(seq, seq_before);
                filling = 0;
            }
            continue;
        }

        if (!filling)
        {
            /* Odd seq = filling; a consumer skips this slot until release. */
            seq_before = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
            /* A retired/abandoned odd slot needs a fresh odd generation. */
            seq_before += seq_before & 1u;
            __atomic_store_n(seq, seq_before + 1u, __ATOMIC_RELEASE);
            __sync_synchronize();
            filled = 0;
            filling = 1;
        }

        /* The reader returns PIXEL bytes, with the header still at buf[0].
         * Skip the header in the source pointer, not in the returned length:
         * subtracting it twice makes every full frame look 12 bytes short. */
        unsigned pix = (unsigned)got;
        if (filled + pix > VD_VIDEO_MAX_FRAME)
        {
            frame_abandon(seq, seq_before);
            filling = 0;
            continue;
        }
        memcpy(VD_SHM_VIDEO_PTR(hdr, slot) + filled, g_payload + hdr_len, pix);
        filled += pix;

        if (frame_done)
        {
            if (filled >= p->publish_size)
            {
                /* Even seq = released; then announce the next slot to write. */
                __sync_synchronize();
                *seq = seq_before + 2u;
                __sync_synchronize();
                hdr->video_producer = (slot + 1u) % VD_VIDEO_SLOTS;
                ++hdr->frames_captured;
                hdr->last_error = VD_ERR_NONE;
            }
            else
            {
                /* EOF before a full frame: partial frame, drop it. */
                frame_abandon(seq, seq_before);
            }
            filling = 0;
            slot = hdr->video_producer % VD_VIDEO_SLOTS;
            seq = slot_seq(hdr, slot);
        }
    }

    /* Partial bytes remain invalid (odd), including after cooperative stop. */
    if (filling)
    {
        frame_abandon(seq, seq_before);
    }
    return 0;
}

/* Run one UVC session against `dev` until stop or failure. Returns 0 when the
 * stop was requested, 1 when the session needs a fresh re-open (persistently
 * idle or repeated read errors - the proven unwedge path), -1 when it never
 * became active. */
static int run_session(const VdUsbVitaDevice *dev, const VdStreamParams *p)
{
    VdSharedHeader *hdr = g_hdr;
    VdUvcSession s;
    int rc = -1;
    int loop_rc;

    if (vd_uvc_session_open(&s, dev) != 0)
    {
        hdr->last_error = VD_ERR_PROBE_FAILED;
        return -1;
    }
    if (vd_uvc_start_stream(&s, p->format_index, p->frame_index, p->interval, p->max_frame_size) !=
        0)
    {
        hdr->last_error = VD_ERR_COMMIT_FAILED;
        vd_uvc_session_close(&s);
        return -1;
    }

    retire_video(hdr);
    hdr->video_width = p->width;
    hdr->video_height = p->height;
    hdr->video_pixel_format = VD_PIX_NV12;
    hdr->stream_active = 1;
    hdr->last_error = VD_ERR_NONE;

    loop_rc = capture_loop(&s, p);
    if (g_stop)
    {
        rc = 0;
    }
    else if (loop_rc != 0 || hdr->last_error == VD_ERR_NONE)
    {
        rc = 1; /* re-open from scratch (idle wedge) or ended cleanly */
    }

    vd_uvc_stop_stream(&s);
    vd_uvc_session_close(&s);
    hdr->stream_active = 0;
    return rc;
}

static void *producer_thread(void *arg)
{
    (void)arg;
    VdSharedHeader *hdr = g_hdr;
    unsigned backoff = VP_BACKOFF_MIN_MS;

    while (!g_stop)
    {
        const VdUsbVitaDevice *dev = find_vita();
        VdStreamParams p;
        if (dev == NULL || pick_stream(dev, &p) != 0)
        {
            hdr->vita_detected = 0;
            hdr->stream_active = 0;
            hdr->last_error = (dev == NULL) ? VD_ERR_NO_VITA : VD_ERR_PROBE_FAILED;
            wait_ms(backoff);
            backoff_grow(&backoff);
            continue;
        }

        hdr->vita_detected = 1;
        int rc = run_session(dev, &p);
        if (rc == 0)
        {
            break; /* stop requested */
        }
        if (rc > 0)
        {
            backoff = VP_BACKOFF_MIN_MS; /* was streaming: retry promptly */
        }
        else
        {
            backoff_grow(&backoff);
        }
        if (hdr->last_error == VD_ERR_NONE)
        {
            hdr->last_error = VD_ERR_VITA_DISCONNECTED;
        }
        wait_ms(backoff);
    }

    hdr->stream_active = 0;
    return NULL;
}

int vd_video_producer_start(void *shm, unsigned long shm_bytes)
{
    if (shm == NULL || shm_bytes < (unsigned long)VD_SHM_TOTAL_BYTES)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_lock);
    if (g_started)
    {
        pthread_mutex_unlock(&g_lock);
        return -EBUSY;
    }

    VdSharedHeader *hdr = (VdSharedHeader *)shm;
    g_hdr = hdr;

    retire_video(hdr);
    hdr->video_width = 0;
    hdr->video_height = 0;
    hdr->video_pixel_format = VD_PIX_NONE;
    hdr->video_producer = 0;
    hdr->frames_captured = 0;
    hdr->frames_dropped = 0;
    hdr->vita_detected = 0;
    hdr->stream_active = 0;
    hdr->last_error = VD_ERR_NONE;

    g_stop = 0;
    int rc = pthread_create(&g_thread, NULL, producer_thread, NULL);
    if (rc != 0)
    {
        g_hdr = NULL;
        pthread_mutex_unlock(&g_lock);
        return -rc;
    }
    g_started = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void vd_video_producer_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    if (g_started)
    {
        (void)pthread_join(g_thread, NULL);
        g_started = 0;
    }
    if (g_hdr != NULL)
    {
        g_hdr->stream_active = 0;
    }
    pthread_mutex_unlock(&g_lock);
}
