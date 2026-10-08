/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — pad forwarder implementation: shared pad ring -> USB bulk OUT.
 *
 * The native app publishes each normalized pad report as the 28-byte
 * little-endian wire image into the pad ring (single-producer /
 * single-consumer slot ring, release/acquire seq per slot; see
 * core/include/vd_shared.h). This worker copies each fresh slot in publish
 * order, validates it with vd_pad_deserialize, and forwards it through
 * usb_send_pad_report() — which re-serializes with vd_pad_serialize and
 * issues exactly one synchronous bulk OUT to the Vita (proven usb_fs
 * START -> COMPLETE pattern of core/src/usb_transfer.c).
 *
 * USB session ownership: this module discovers the Vita (vd_usb_scan_vita)
 * and owns its own ugen fd + usb_fs endpoint session for the pad OUT slot
 * (VD_USB_EP_INDEX_PAD_OUT). In-flight OUT transfers are serialized by the
 * synchronous helper; a failed transfer tears the whole session down and
 * rebuilds it through discovery, because usb_bulk_write() may have
 * quarantined its backing after an unbounded failure.
 */
#include "pad_forwarder.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pad_passthrough.h"
#include "usb_transfer.h"
#include "usb_vita.h"
#include "vd_shared.h"

#define VD_LOG(...)                                                                                \
    do                                                                                             \
    {                                                                                              \
        fprintf(stderr, "[pad] " __VA_ARGS__);                                                     \
    } while (0)

/* Pad OUT endpoint per vita-side/input-receiver: the vendor pad interface
 * (class 0xFF / subclass 0x50 'P' / protocol 0x01) with one bulk OUT endpoint
 * carrying 28-byte wire reports. Both the interface number and the endpoint
 * address are DISCOVERED by vd_usb_scan_vita() (the plugin appends the
 * interface after the stream gadget's own ones, so they are build-dependent;
 * matching is by class/subclass, never by number). The usb_fs endpoint slot
 * layout is fixed by usb_transfer.h (slot 1 = pad OUT). */
#define VD_PAD_OUT_EP_COUNT (VD_USB_EP_INDEX_PAD_OUT + 1u)

/* One pad report is 28 bytes; 4 KiB covers the endpoint max packet (512 on
 * hi-speed) with slack so the transfer can never stall on max_bufsize. */
#define VD_PAD_USB_MAX_BUF 4096u

/* Poll cadence: sub-millisecond while forwarding (input-lag-critical), a
 * slower idle when the Vita is absent. Discovery retries back off from
 * VD_PAD_BACKOFF_MIN_MS to VD_PAD_BACKOFF_MAX_MS. */
#define VD_PAD_IDLE_NS 250000L    /* 250 us */
#define VD_PAD_NO_DEV_NS 5000000L /* 5 ms */
#define VD_PAD_BACKOFF_MIN_MS 250u
#define VD_PAD_BACKOFF_MAX_MS 1000u

/* Consumer cursor over the pad slot ring (in publish order). */
typedef struct
{
    uint32_t seen[VD_PAD_SLOTS];
    unsigned int cursor;
    int primed;
} PadRingCursor;

/* The forwarder-owned ugen session for the pad OUT endpoint. */
typedef struct
{
    int fd;
    struct usb_fs_endpoint eps[VD_PAD_OUT_EP_COUNT];
    uint32_t backoff_ms;
    uint64_t next_try_ms;
} PadUsb;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_started;
static int g_stop;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    return 0u;
}

static void wait_ns(long ns)
{
    struct timespec req;

    req.tv_sec = 0;
    req.tv_nsec = ns;
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        ;
}

/* Acquire load on a ring seq counter (mirrors the app-side seq_load; the
 * producer releases with an even seq after filling the slot). */
static uint32_t pad_seq_load(const VdSharedHeader *hdr, unsigned int slot)
{
    return __atomic_load_n(&hdr->pad_slot_seq[slot], __ATOMIC_ACQUIRE);
}

static void pad_ring_prime(const VdSharedHeader *hdr, PadRingCursor *cur)
{
    unsigned int i;

    for (i = 0; i < VD_PAD_SLOTS; ++i)
        cur->seen[i] = (pad_seq_load(hdr, i) + 1u) & ~1u;
    /* pad_producer is the next slot the producer will write: everything
     * before it is stale at attach time. */
    cur->cursor = __atomic_load_n(&hdr->pad_producer, __ATOMIC_ACQUIRE) % VD_PAD_SLOTS;
    cur->primed = 1;
}

/* Copy the next published report (28-byte wire image) in publish order.
 * Returns 1 with `wire` filled on success, 0 when nothing new is available or
 * the copy raced a producer rewrite (torn -> dropped, retried next poll). */
static int pad_ring_next(const VdSharedHeader *hdr, PadRingCursor *cur, uint8_t *wire)
{
    unsigned int slot;
    uint32_t before;
    uint32_t after;

    uint32_t epoch = __atomic_load_n(&hdr->pad_reset_epoch, __ATOMIC_ACQUIRE);
    if (epoch & 1u)
        return 0;
    if (!cur->primed)
        pad_ring_prime(hdr, cur);
    slot = cur->cursor;
    before = pad_seq_load(hdr, slot);
    if (!(before & 1u) && before != cur->seen[slot] && (uint32_t)(before - cur->seen[slot]) > 2u)
    {
        /* An unread slot was overwritten. Producer points to the oldest
         * retained slot, NOT the old consumer cursor (which can be newest). */
        slot = __atomic_load_n(&hdr->pad_producer, __ATOMIC_ACQUIRE) % VD_PAD_SLOTS;
        before = pad_seq_load(hdr, slot);
    }
    for (unsigned i = 0; i < VD_PAD_SLOTS; ++i)
    {
        if (before & 1u)
            return 0;
        if (before != cur->seen[slot])
            break;
        slot = (slot + 1u) % VD_PAD_SLOTS;
        before = pad_seq_load(hdr, slot);
        if (i + 1u == VD_PAD_SLOTS)
            return 0;
    }
    memcpy(wire, VD_SHM_PAD_PTR(hdr, slot), VD_PAD_WIRE_BYTES);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    after = pad_seq_load(hdr, slot);
    if (after != before || epoch != __atomic_load_n(&hdr->pad_reset_epoch, __ATOMIC_ACQUIRE))
        return 0; /* torn copy or disconnect invalidation: retry */
    cur->seen[slot] = before;
    cur->cursor = (slot + 1u) % VD_PAD_SLOTS;
    return 1;
}

static void pad_backoff_grow(PadUsb *usb)
{
    uint32_t next = usb->backoff_ms * 2u;

    usb->backoff_ms = next < VD_PAD_BACKOFF_MAX_MS ? next : VD_PAD_BACKOFF_MAX_MS;
}

/* Tear the FS session down synchronously, then release any helper-owned
 * transfer buffer usb_bulk_write() may have quarantined (safe only after
 * USB_FS_CLOSE + USB_FS_UNINIT have run — see usb_transfer.h). */
static void pad_usb_close(PadUsb *usb)
{
    if (usb->fd < 0)
        return;
    close(usb->fd);
    usb->fd = -1;
}

/* Discover the Vita and open its ugen node. Pad reports ride a vendor
 * control OUT on EP0 (see usb_send_pad_report), so no bulk endpoint
 * session is needed. Returns 0 or -errno; -ENODEV when no Vita is attached
 * (or it vanished mid-open). */
static int pad_usb_open(PadUsb *usb)
{
    VdUsbVitaDevice dev;

    memset(&dev, 0, sizeof(dev));
    if (vd_usb_scan_vita(&dev, 1u) == 0)
        return -ENODEV;
    usb->fd = open(dev.path, O_RDWR | O_NONBLOCK);
    if (usb->fd < 0)
        return -errno;
    VD_LOG("vita at %s (vid=0x%04x pid=0x%04x); pad reports via EP0 control OUT\n", dev.path,
           dev.vid, dev.pid);
    return 0;
}

static void *pad_worker(void *arg)
{
    VdSharedHeader *hdr = (VdSharedHeader *)arg;
    PadRingCursor cursor;
    PadUsb usb;
    uint32_t seen_touch_seq = 0;

    memset(&cursor, 0, sizeof(cursor));
    memset(&usb, 0, sizeof(usb));
    usb.fd = -1;
    usb.backoff_ms = VD_PAD_BACKOFF_MIN_MS;

    while (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) == 0)
    {
        VdPadReport report;
        uint8_t wire[VD_PAD_WIRE_BYTES];
        uint64_t now;
        int rc;

        now = monotonic_ms();
        if (usb.fd < 0 && now >= usb.next_try_ms)
        {
            rc = pad_usb_open(&usb);
            if (rc == 0)
            {
                usb.backoff_ms = VD_PAD_BACKOFF_MIN_MS;
            }
            else
            {
                if (rc != -ENODEV)
                    VD_LOG("session open failed (%d), retry in %u ms\n", rc,
                           (unsigned)usb.backoff_ms);
                usb.next_try_ms = now + usb.backoff_ms;
                pad_backoff_grow(&usb);
            }
        }

        /* Adapter mailbox is immutable until synchronous USB success. Never
         * drain/ack while absent, or discard a failed release on reconnect. */
        {
            uint32_t seq = __atomic_load_n(&hdr->touch_seq, __ATOMIC_ACQUIRE);

            if ((seq & 1u) == 0u && seq != seen_touch_seq)
            {
                uint8_t tw[VD_TOUCH_WIRE_BYTES];
                VdTouchReport touch;

                memcpy(tw, hdr->touch_wire, sizeof(tw));
                if (__atomic_load_n(&hdr->touch_seq, __ATOMIC_ACQUIRE) != seq)
                {
                    /* Torn fill: retry on the next poll. */
                }
                else if (usb.fd < 0)
                {
                    /* Retain unacknowledged state until USB returns. */
                }
                else if (vd_touch_deserialize(tw, &touch) != 0 &&
                         usb_send_touch_report(usb.fd, &touch) == 0)
                {
                    hdr->touch_reports++;
                    seen_touch_seq = seq;
                    __atomic_store_n(&hdr->touch_ack_seq, seq, __ATOMIC_RELEASE);
                }
                else
                {
                    /* Retry this exact record after rebuilding. A failed
                     * release is not superseded by a later active report. */
                    VD_LOG("touch send failed; rebuilding session\n");
                    pad_usb_close(&usb);
                    usb.next_try_ms = monotonic_ms() + usb.backoff_ms;
                    pad_backoff_grow(&usb);
                }
            }
        }

        if (!pad_ring_next(hdr, &cursor, wire))
        {
            wait_ns(usb.fd >= 0 ? VD_PAD_IDLE_NS : VD_PAD_NO_DEV_NS);
            continue;
        }

        /* Drain-and-drop while the Vita is absent so a reconnect starts from
         * the live edge of the ring, not a stale backlog. */
        if (usb.fd < 0)
            continue;

        if (vd_pad_deserialize(wire, sizeof(wire), &report) != 0)
        {
            VD_LOG("dropping malformed ring report\n");
            continue;
        }

        /* Fast send path: one serialize + one synchronous control OUT. */
        rc = usb_send_pad_report(usb.fd, NULL, &report);
        if (rc != 0)
        {
            /* Transient failures happen while the gadget is mid-state; the
             * control path allocates nothing, so retrying the same report is
             * safe. Only a persistent failure rebuilds the session (the old
             * eager rebuild churned the shared USB device and starved video). */
            int tries;
            for (tries = 0; tries < 3 && rc != 0; ++tries)
            {
                usleep(2000);
                rc = usb_send_pad_report(usb.fd, NULL, &report);
            }
        }
        if (rc == 0)
        {
            hdr->pad_reports++;
            continue;
        }

        VD_LOG("pad send failed (%d), rebuilding session\n", rc);
        pad_usb_close(&usb);
        usb.next_try_ms = monotonic_ms() + usb.backoff_ms;
        pad_backoff_grow(&usb);
    }

    pad_usb_close(&usb);
    return NULL;
}

int vd_pad_forwarder_start(void *shm, unsigned long shm_bytes)
{
    VdSharedHeader *hdr = (VdSharedHeader *)shm;
    int rc = 0;

    if (shm == NULL || shm_bytes < VD_SHM_TOTAL_BYTES)
        return -EINVAL;
    if (hdr->magic != VD_SHM_MAGIC || hdr->version != VD_SHM_VERSION)
        return -EINVAL;

    pthread_mutex_lock(&g_lock);
    if (g_started)
    {
        rc = -EBUSY;
    }
    else
    {
        int err;

        __atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE);
        err = pthread_create(&g_thread, NULL, pad_worker, hdr);
        if (err != 0)
            rc = -err;
        else
            g_started = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void vd_pad_forwarder_stop(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_started)
    {
        __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
        pthread_join(g_thread, NULL);
        g_started = 0;
    }
    pthread_mutex_unlock(&g_lock);
}
