/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — audio producer implementation: continuous UAC isoc capture loop
 * publishing PCM into the shared audio ring.
 *
 * Worker flow (runs until vd_audio_producer_stop()):
 *   1. find a Vita exposing the UAC isoc IN endpoint (vd_usb_scan_vita);
 *      when none is present, retry with capped exponential back-off
 *   2. open + start the capture session via core/src/uac_audio.c
 *      (hardware-verified: 48 kHz stereo int16 interleaved, 192 bytes per
 *      1 ms USB frame, isoc IN under the AudioStreaming alt 1)
 *   3. vd_uac_read() one multi-frame transfer at a time and copy the PCM into
 *      the next audio ring slot using the per-slot release/acquire sequence
 *      protocol of core/include/vd_shared.h: bump the slot's seq to an ODD
 *      value before filling, then to the NEXT EVEN value as the release
 *      (a consumer that observes its seq change mid-copy drops the torn copy)
 *   4. on persistent read failure tear the session down and return to (1)
 *
 * Slots hold VD_AUDIO_CHUNK_BYTES (4096 = 256 stereo frames) each, so PCM is
 * accumulated across reads until a slot fills; any trailing partial chunk is
 * dropped at teardown. Shared-header writes owned here: audio_sample_rate /
 * audio_channels / audio_bits (fixed capture format), audio_chunks (published
 * chunk counter), and last_error (VD_ERR_* on failure paths only — never
 * cleared here). vita_detected / stream_active / frame counters belong to the
 * payload main and the video path and are left untouched.
 */
#include "audio_producer.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uac_audio.h"
#include "usb_vita.h"
#include "vd_shared.h"

#define VD_AP_XFER_FRAMES 32u      /* ~32 ms of audio per isoc transfer */
#define VD_AP_MAX_READ_ERRORS 4u   /* bail before abusing the Vita UDCD */
#define VD_AP_BACKOFF_MIN_MS 100u  /* first retry delay when no Vita */
#define VD_AP_BACKOFF_MAX_MS 2000u /* retry delay cap */
#define VD_AP_SLEEP_SLICE_MS 20u   /* stop() wake-up granularity while idle */
#define VD_AP_SCAN_CAPACITY 8u     /* Vita devices per scan */

#define VD_AP_LOG(...)                                                                             \
    do                                                                                             \
    {                                                                                              \
        fprintf(stderr, "[audio] " __VA_ARGS__);                                                   \
    } while (0)

typedef struct
{
    VdSharedHeader *hdr;
    pthread_t thread;
    volatile int stop; /* set by vd_audio_producer_stop() */
    int started;
} VdAudioProducer;

static VdAudioProducer g_ap;

static int vd_ap_stopping(void)
{
    return __atomic_load_n(&g_ap.stop, __ATOMIC_RELAXED) != 0;
}

static void vd_ap_set_error(uint32_t code)
{
    if (g_ap.hdr != NULL)
        __atomic_store_n(&g_ap.hdr->last_error, code, __ATOMIC_RELAXED);
}

/* Sleep in short slices so stop() is honored promptly even while backing off
 * with no device present. */
static void vd_ap_sleep_ms(uint32_t total_ms)
{
    while (total_ms != 0u && !vd_ap_stopping())
    {
        uint32_t slice = total_ms > VD_AP_SLEEP_SLICE_MS ? VD_AP_SLEEP_SLICE_MS : total_ms;
        struct timespec ts;
        ts.tv_sec = (time_t)(slice / 1000u);
        ts.tv_nsec = (long)(slice % 1000u) * 1000000L;
        (void)nanosleep(&ts, NULL);
        total_ms -= slice;
    }
}

static uint32_t vd_ap_next_backoff(uint32_t current)
{
    return current >= VD_AP_BACKOFF_MAX_MS ? VD_AP_BACKOFF_MAX_MS : current * 2u;
}

/* First device exposing the audio isoc IN endpoint (a base vita-udcd-uvc build
 * is video-only, so a bare Vita hit is not enough). */
static int vd_ap_find_vita(VdUsbVitaDevice *dev)
{
    VdUsbVitaDevice list[VD_AP_SCAN_CAPACITY];
    size_t count = vd_usb_scan_vita(list, VD_AP_SCAN_CAPACITY);
    size_t i;

    for (i = 0; i < count; ++i)
    {
        if (list[i].has_audio && list[i].audio_isoc_ep != 0u)
        {
            *dev = list[i];
            return 0;
        }
    }
    return -1;
}

/* Publish one full chunk with the vd_shared.h slot protocol. The producer
 * owns the ring (single producer), so an unread slot is overwritten in place;
 * a consumer that was mid-copy detects the change via its seq re-read and
 * drops the torn copy (treated as a drop, not corruption). */
static void vd_ap_publish_chunk(const uint8_t *chunk)
{
    VdSharedHeader *hdr = g_ap.hdr;
    uint32_t slot;
    uint32_t base;

    slot = __atomic_load_n(&hdr->audio_producer, __ATOMIC_RELAXED) % VD_AUDIO_SLOTS;
    base = __atomic_load_n(&hdr->audio_slot_seq[slot], __ATOMIC_RELAXED) & ~1u;

    /* ODD seq = "being filled". */
    __atomic_store_n(&hdr->audio_slot_seq[slot], base + 1u, __ATOMIC_RELAXED);
    memcpy(VD_SHM_AUDIO_PTR(hdr, slot), chunk, VD_AUDIO_CHUNK_BYTES);
    /* NEXT EVEN seq = the release; all payload stores happen before it. */
    __atomic_store_n(&hdr->audio_slot_seq[slot], base + 2u, __ATOMIC_RELEASE);
    __atomic_store_n(&hdr->audio_producer, (slot + 1u) % VD_AUDIO_SLOTS, __ATOMIC_RELEASE);
    (void)__atomic_fetch_add(&hdr->audio_chunks, 1u, __ATOMIC_RELAXED);
}

/* Copy a PCM burst into the ring, splitting it on VD_AUDIO_CHUNK_BYTES slot
 * boundaries. *fill carries the partial-chunk offset across reads. */
static void vd_ap_feed_pcm(const uint8_t *pcm, uint32_t len, uint8_t *staging, uint32_t *fill)
{
    while (len != 0u)
    {
        uint32_t space = VD_AUDIO_CHUNK_BYTES - *fill;
        uint32_t take = len < space ? len : space;

        memcpy(staging + *fill, pcm, take);
        *fill += take;
        pcm += take;
        len -= take;
        if (*fill == VD_AUDIO_CHUNK_BYTES)
        {
            vd_ap_publish_chunk(staging);
            *fill = 0u;
        }
    }
}

/* One full capture cycle: device present -> stream until stop() or persistent
 * read failure -> teardown. Returns 1 when the stream bailed on read errors
 * (the caller applies a re-scan back-off). */
static int vd_ap_stream_cycle(void)
{
    VdUsbVitaDevice dev;
    VdUacSession session;
    uint8_t staging[VD_AUDIO_CHUNK_BYTES];
    uint8_t *pcm;
    uint32_t fill = 0u;
    uint32_t read_errors = 0u;

    if (vd_ap_find_vita(&dev) != 0)
    {
        vd_ap_set_error(VD_ERR_NO_VITA);
        return 1;
    }
    if (vd_uac_session_open(&session, &dev) != 0)
    {
        VD_AP_LOG("session open on %s failed\n", dev.path);
        vd_ap_set_error(VD_ERR_PROBE_FAILED);
        return 1;
    }
    if (vd_uac_start(&session, VD_AP_XFER_FRAMES) != 0)
    {
        VD_AP_LOG("isoc stream start failed on %s\n", dev.path);
        vd_ap_set_error(VD_ERR_PROBE_FAILED);
        vd_uac_session_close(&session);
        return 1;
    }

    /* One read can deliver more than a ring slot (32 packets * 192 bytes =
     * 6144 > 4096), so the PCM buffer is sized from the live session. */
    pcm = (uint8_t *)malloc(session.buffer_cap);
    if (pcm == NULL)
    {
        VD_AP_LOG("pcm buffer alloc (%u bytes) failed\n", session.buffer_cap);
        vd_uac_stop(&session);
        vd_uac_session_close(&session);
        return 1;
    }
    VD_AP_LOG("streaming from %s\n", dev.path);

    while (!vd_ap_stopping() && read_errors < VD_AP_MAX_READ_ERRORS)
    {
        uint32_t got = 0u;
        int rc = vd_uac_read(&session, pcm, session.buffer_cap, &got, NULL, 0u, NULL);

        if (rc != 0)
        {
            ++read_errors;
            continue;
        }
        read_errors = 0u;
        if (got != 0u)
            vd_ap_feed_pcm(pcm, got, staging, &fill);
    }

    if (read_errors >= VD_AP_MAX_READ_ERRORS && !vd_ap_stopping())
    {
        VD_AP_LOG("persistent read errors on %s, re-scanning\n", dev.path);
        vd_ap_set_error(VD_ERR_VITA_DISCONNECTED);
    }

    free(pcm);
    vd_uac_stop(&session);
    vd_uac_session_close(&session);
    return read_errors >= VD_AP_MAX_READ_ERRORS && !vd_ap_stopping();
}

static void *vd_ap_worker(void *arg)
{
    uint32_t backoff_ms = VD_AP_BACKOFF_MIN_MS;

    (void)arg;
    while (!vd_ap_stopping())
    {
        int bailed = vd_ap_stream_cycle();

        if (bailed && !vd_ap_stopping())
        {
            vd_ap_sleep_ms(backoff_ms);
            backoff_ms = vd_ap_next_backoff(backoff_ms);
        }
        else if (!bailed)
        {
            backoff_ms = VD_AP_BACKOFF_MIN_MS;
        }
    }
    return NULL;
}

int vd_audio_producer_start(void *shm, unsigned long shm_bytes)
{
    unsigned long need = (unsigned long)VD_SHM_AUDIO_OFFSET + (unsigned long)VD_SHM_AUDIO_BYTES;
    int rc;

    if (shm == NULL || shm_bytes < need)
        return -EINVAL;
    if (g_ap.started)
        return -EBUSY;

    memset(&g_ap, 0, sizeof(g_ap));
    g_ap.hdr = (VdSharedHeader *)shm;
    __atomic_store_n(&g_ap.stop, 0, __ATOMIC_RELAXED);

    /* Fixed capture format, read by the app before consuming slots. */
    g_ap.hdr->audio_sample_rate = VD_UAC_SAMPLE_RATE;
    g_ap.hdr->audio_channels = VD_UAC_CHANNELS;
    g_ap.hdr->audio_bits = VD_UAC_BITS;

    rc = pthread_create(&g_ap.thread, NULL, vd_ap_worker, NULL);
    if (rc != 0)
    {
        g_ap.hdr = NULL;
        return -rc;
    }
    g_ap.started = 1;
    return 0;
}

void vd_audio_producer_stop(void)
{
    if (!g_ap.started)
        return;
    __atomic_store_n(&g_ap.stop, 1, __ATOMIC_RELAXED);
    pthread_join(g_ap.thread, NULL);
    g_ap.started = 0;
    g_ap.hdr = NULL;
}
