/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — USB transfer implementation (bulk IN/OUT + control).
 *
 * Direct port of Ghostcontrol's proven usb_helpers.c transfer pattern
 * (synchronous usb_fs START -> COMPLETE wait, helper-owned packet copies,
 * bounded cancel-and-reap, post-UNINIT quarantine release), generalized from
 * one OUT endpoint to VITA5's two directions. The completion-validation
 * semantics (usb_out_check_completion) are reused verbatim; see
 * include/usb_transfer_state.h.
 */
#include "usb_transfer.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <dev/usb/usbdi.h> /* USB_SHORT_XFER_OK and transfer flags */

#define VD_LOG(...) do { fprintf(stderr, "[usb] " __VA_ARGS__); } while (0)

/* Bounded deadlines. OUT (pad reports) is latency-critical and short; IN
 * (video payloads) may legitimately wait for the next chunk. */
#define VD_USB_OUT_TIMEOUT_MS    150u
#define VD_USB_IN_TIMEOUT_MS     1500u
#define VD_USB_WALL_GRACE_MS     200u
#define VD_USB_DRAIN_MS          250u

/* Direction-tagged backing magics so release-after-uninit can recognize a
 * quarantined helper-owned buffer and nothing else. */
#define VD_USB_XFER_MAGIC_IN  UINT64_C(0x5644494E42554646) /* "VDINBUFF" */
#define VD_USB_XFER_MAGIC_OUT UINT64_C(0x56444F5542554646) /* "VDOUTBUF" */

struct UsbXferBacking {
    uint64_t magic;
    void *buffers[1];
    uint32_t lengths[1];
    uint8_t packet[];
};

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    struct timeval tv;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u +
               (uint64_t)ts.tv_nsec / 1000000u;
    /* A failed monotonic clock must not turn bounded cancellation into an
     * infinite loop. Wall clock remains a safe fallback deadline source. */
    if (gettimeofday(&tv, NULL) == 0)
        return (uint64_t)tv.tv_sec * 1000u +
               (uint64_t)tv.tv_usec / 1000u;
    return (uint64_t)time(NULL) * 1000u;
}

static int wait_slice(int fd, uint64_t deadline_ms) {
    struct pollfd pfd;
    uint64_t now = monotonic_ms();
    uint64_t remaining;
    int timeout_ms;
    int result;

    if (now >= deadline_ms)
        return 0;
    remaining = deadline_ms - now;
    timeout_ms = (int)(remaining > 20u ? 20u : remaining);
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    result = poll(&pfd, 1, timeout_ms);
    if (result < 0)
        return errno == EINTR ? 1 : -errno;
    if (result > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return -ENODEV;
    return 1;
}

/* Wait for THIS endpoint's completion record specifically. USB_FS_COMPLETE
 * overwrites ep_index with whichever queued completion is oldest. Unexpected
 * records are drained rather than mistaken for our own completion; call sites
 * serialize IN/OUT, so seeing one indicates stale session state and is
 * logged. `expected_length` is the exact OUT length or the IN buffer
 * capacity, depending on `is_in`. */
static int wait_completion(int fd, struct usb_fs_endpoint *ep,
                           uint8_t ep_index, int is_in,
                           uint32_t expected_length, uint64_t deadline_ms,
                           const char *tag, int *reaped) {
    const char *dir = is_in ? "IN" : "OUT";

    for (;;) {
        struct usb_fs_complete complete;
        enum UsbOutCompletionCheck check;
        int result;

        if (monotonic_ms() >= deadline_ms)
            return -ETIMEDOUT;

        memset(&complete, 0, sizeof(complete));
        result = ioctl(fd, USB_FS_COMPLETE, &complete);
        if (result == 0) {
            uint32_t actual_length = ep->pLength ? ep->pLength[0] : 0u;

            if (is_in)
                check = usb_in_check_completion(complete.ep_index, ep_index,
                                                ep->status, ep->aFrames,
                                                actual_length,
                                                expected_length);
            else
                check = usb_out_check_completion(complete.ep_index, ep_index,
                                                 ep->status, ep->aFrames,
                                                 actual_length,
                                                 expected_length);
            if (check == USB_OUT_COMPLETION_OTHER) {
                VD_LOG("%s %s observed unexpected completion ep=%u while waiting for ep=%u\n",
                       dir, tag, (unsigned)complete.ep_index,
                       (unsigned)ep_index);
                continue;
            }

            *reaped = 1;
            if (check == USB_OUT_COMPLETION_STATUS_ERROR) {
                VD_LOG("%s %s completion status=%d\n", dir, tag, ep->status);
                return usb_status_to_errno(ep->status);
            }
            if (check == USB_OUT_COMPLETION_FRAME_ERROR) {
                VD_LOG("%s %s invalid completed frames=%u\n", dir, tag,
                       (unsigned)ep->aFrames);
                return -EIO;
            }
            if (check == USB_OUT_COMPLETION_LENGTH_ERROR) {
                if (is_in)
                    VD_LOG("%s %s overrun read=%u capacity=%u\n", dir, tag,
                           (unsigned)actual_length,
                           (unsigned)expected_length);
                else
                    VD_LOG("%s %s short/invalid write=%u expected=%u\n", dir,
                           tag, (unsigned)actual_length,
                           (unsigned)expected_length);
                return -EIO;
            }
            return 0;
        }

        int complete_error = errno;
        if (complete_error != EBUSY && complete_error != EINTR) {
            VD_LOG("USB_FS_COMPLETE fd=%d ep=%u rc=%d errno=%d raw_status=%d reaped=0\n",
                   fd, (unsigned)ep_index, result, complete_error, ep->status);
            return -complete_error;
        }

        result = wait_slice(fd, deadline_ms);
        if (result <= 0)
            return result == 0 ? -ETIMEDOUT : result;
    }
}

static void stop_ep(int fd, uint8_t ep_index, const char *tag) {
    struct usb_fs_stop stop;
    memset(&stop, 0, sizeof(stop));
    stop.ep_index = ep_index;
    if (ioctl(fd, USB_FS_STOP, &stop) != 0 &&
        errno != ENXIO && errno != ENOTTY && errno != EBADF)
        VD_LOG("%s STOP fail errno=%d\n", tag, errno);
}

/* STOP is asynchronous: keep the user endpoint/backing alive until its cancel
 * (or raced normal) completion has been copied out. If a broken device never
 * produces that record, closing the endpoint synchronously unsets the
 * transfer. */
static int cancel_and_reap(int fd, struct usb_fs_endpoint *ep,
                           uint8_t ep_index, int is_in,
                           uint32_t expected_length, const char *tag) {
    struct usb_fs_close close_ep;
    int reaped = 0;
    int result;

    stop_ep(fd, ep_index, tag);
    result = wait_completion(fd, ep, ep_index, is_in, expected_length,
                             monotonic_ms() + VD_USB_DRAIN_MS, tag, &reaped);
    if (reaped)
        return 0;

    memset(&close_ep, 0, sizeof(close_ep));
    close_ep.ep_index = ep_index;
    if (ioctl(fd, USB_FS_CLOSE, &close_ep) == 0)
        return 0;

    /* These errors mean the descriptor/device or endpoint transfer no longer
     * exists, so there is no kernel operation left that can address backing. */
    if (errno == EBADF || errno == ENXIO || errno == ENOTTY || errno == EINVAL)
        return 0;

    VD_LOG("%s could not reap cancellation or close ep=%u errno=%d\n",
           tag, (unsigned)ep_index, errno);
    return result < 0 ? result : -EIO;
}

static void clear_endpoint(struct usb_fs_endpoint *ep) {
    ep->ppBuffer = NULL;
    ep->pLength = NULL;
    ep->nFrames = 0;
    ep->aFrames = 0;
    ep->flags = 0;
    ep->timeout = 0;
    ep->isoc_time_complete = 0;
    ep->status = 0;
}

/* Synchronous one-frame transfer in either direction. For OUT, `src` is
 * copied into helper-owned storage before START; for IN the received bytes are
 * copied out to `dst` after completion. `expected_length` semantics for
 * validation: OUT must complete exactly `len`, IN may complete short but not
 * over `len` (the capacity). */
static int xfer_sync(int fd, struct usb_fs_endpoint *ep, uint8_t ep_index,
                     int is_in, uint8_t *dst, const uint8_t *src,
                     uint32_t len, uint32_t *transferred, const char *tag) {
    struct UsbXferBacking *backing;
    struct usb_fs_start start;
    uint64_t deadline_ms;
    size_t allocation_size;
    int reaped = 0;
    int result;
    int start_error;

    if (fd < 0 || !ep || (!src && !is_in && len != 0) ||
        (!dst && is_in && len != 0))
        return -EINVAL;
    if (transferred)
        *transferred = 0;
    /* A prior catastrophic cancel path deliberately leaves these pointers
     * alive for synchronous session teardown. Never overwrite them: the old
     * transfer could copy out through replacement pointers and corrupt/free
     * the wrong allocation. */
    if (ep->ppBuffer || ep->pLength || ep->nFrames != 0)
        return -EBUSY;
    if (!tag)
        tag = is_in ? "in" : "out";
    if ((size_t)len > SIZE_MAX - sizeof(*backing))
        return -EOVERFLOW;

    allocation_size = sizeof(*backing) + (size_t)len;
    backing = (struct UsbXferBacking *)malloc(allocation_size);
    if (!backing)
        return -ENOMEM;
    backing->magic = is_in ? VD_USB_XFER_MAGIC_IN : VD_USB_XFER_MAGIC_OUT;
    if (!is_in && len != 0)
        memcpy(backing->packet, src, len);
    backing->buffers[0] = backing->packet;
    backing->lengths[0] = len;

    ep->ppBuffer = backing->buffers;
    ep->pLength  = backing->lengths;
    ep->nFrames  = 1;
    ep->timeout  = (uint16_t)(is_in ? VD_USB_IN_TIMEOUT_MS
                                    : VD_USB_OUT_TIMEOUT_MS);
    /* IN reads must terminate at short packets: UVC payloads are chunks that
     * legitimately end short of the buffer. Without SINGLE_SHORT_OK the
     * kernel fails such completions with USB_ERR_SHORT_XFER (status 21) -
     * the exact bug behind intermittent video frames. */
    ep->flags    = is_in ? USB_FS_FLAG_SINGLE_SHORT_OK : 0;
    ep->aFrames  = 0;
    ep->status   = 0;

    memset(&start, 0, sizeof(start));
    start.ep_index = ep_index;
    if (ioctl(fd, USB_FS_START, &start) != 0) {
        start_error = errno;
        VD_LOG("%s %s START fail errno=%d\n",
               is_in ? "IN" : "OUT", tag, start_error);
        if (start_error == EBUSY) {
            /* The queued record belongs to an older transfer. Its copy-out
             * metadata cannot safely be associated with this packet. Keep
             * our replacement backing alive, cancel best-effort, and force
             * the caller down the synchronous session-reinit path. */
            stop_ep(fd, ep_index, tag);
            return -start_error;
        }
        stop_ep(fd, ep_index, tag);
        clear_endpoint(ep);
        free(backing);
        return -start_error;
    }

    deadline_ms = monotonic_ms() +
                  (is_in ? VD_USB_IN_TIMEOUT_MS : VD_USB_OUT_TIMEOUT_MS) +
                  VD_USB_WALL_GRACE_MS;
    result = wait_completion(fd, ep, ep_index, is_in, len, deadline_ms, tag,
                             &reaped);
    if (result != 0) {
        VD_LOG("%s %s %s error=%d\n", is_in ? "IN" : "OUT", tag,
               result == -ETIMEDOUT ? "timeout" : "completion", -result);
        /* Even a validation/status error gets a defensive STOP. If the
         * expected completion was already reaped, there is nothing to drain. */
        if (reaped)
            stop_ep(fd, ep_index, tag);
        else if (cancel_and_reap(fd, ep, ep_index, is_in, len, tag) != 0) {
            /* Do not free storage that a still-live kernel transfer could
             * address. This rare tiny backing is quarantined until
             * usb_transfer_release_after_uninit() runs after synchronous
             * session teardown (or until the payload process exits) rather
             * than risking a kernel UAF. */
            return result;
        }
    } else if (is_in && transferred && dst && len != 0) {
        uint32_t got = backing->lengths[0];
        if (got > len)
            got = len; /* defensive; wait_completion already validated */
        *transferred = got;
        if (got != 0)
            memcpy(dst, backing->packet, got);
    }

    clear_endpoint(ep);
    backing->magic = 0;
    free(backing);
    return result;
}

/* Open (register + activate) a ugen FS endpoint slot. This is the setup step
 * the transfer path requires before USB_FS_START: FreeBSD ugen needs
 * USB_FS_INIT (register the endpoint-state array) then USB_FS_OPEN (bind a slot
 * to a physical endpoint number). Opening the endpoint is also what selects the
 * alternate setting that owns it — for the Vita's VideoStreaming bulk IN 0x81
 * that is alt 1 — so this replaces the failed raw SET_INTERFACE attempt.
 * `eps`/`ep_count` is the persistent endpoint-state array (registered once per
 * ugen session); `ep_index` is the slot to bind to `ep_no`. Detaches any kernel
 * driver on the interface first (Ghostcontrol's claim sequence). */
int usb_transfer_open_endpoint(int fd, struct usb_fs_endpoint *eps,
                               unsigned ep_count, uint8_t ep_index,
                               uint8_t ep_no, int iface_ordinal,
                               uint32_t max_bufsize, uint32_t *maxpkt_out) {
    struct usb_fs_init init;
    struct usb_fs_open open;
    int detach_iface = iface_ordinal;

    if (fd < 0 || !eps || ep_count == 0 || ep_index >= ep_count)
        return -EINVAL;

    /* Claim the interface from any kernel driver. Best-effort: not fatal if it
     * is already detached. */
    ioctl(fd, USB_IFACE_DRIVER_DETACH, &detach_iface);

    memset(&init, 0, sizeof(init));
    init.pEndpoints = eps;
    init.ep_index_max = (uint8_t)ep_count;
    if (ioctl(fd, USB_FS_INIT, &init) != 0) {
        int e = errno;
        VD_LOG("USB_FS_INIT fail errno=%d\n", e);
        return -e;
    }

    memset(&open, 0, sizeof(open));
    open.ep_index = ep_index;
    open.ep_no = ep_no;
    /* max_bufsize is the max single-transfer buffer. For the Vita's UVC bulk IN
     * this must cover one whole frame (the device sends header+frame as a single
     * request): frame_size + 12-byte UVC header. An undersized max_bufsize (or a
     * read larger than max_bufsize) makes the transfer stall -> ETIMEDOUT. */
    if (max_bufsize == 0)
        max_bufsize = 64u * 1024u;
    open.max_bufsize = max_bufsize;
    open.max_frames = 1;
    if (ioctl(fd, USB_FS_OPEN, &open) != 0) {
        int e = errno;
        VD_LOG("USB_FS_OPEN ep_no=0x%02x slot=%u fail errno=%d\n",
               ep_no, ep_index, e);
        struct usb_fs_uninit uninit;
        memset(&uninit, 0, sizeof(uninit));
        ioctl(fd, USB_FS_UNINIT, &uninit);
        return -e;
    }
    if (maxpkt_out)
        *maxpkt_out = open.max_packet_length;
    VD_LOG("USB_FS_OPEN ok ep_no=0x%02x slot=%u maxpkt=%u\n",
           ep_no, ep_index, open.max_packet_length);
    return 0;
}

/* Close a slot and tear down the FS endpoint session. Call at stream end. */
void usb_transfer_close_endpoint(int fd, struct usb_fs_endpoint *eps,
                                 unsigned ep_count, uint8_t ep_index) {
    struct usb_fs_close close_ep;
    struct usb_fs_uninit uninit;
    if (fd < 0) return;
    memset(&close_ep, 0, sizeof(close_ep));
    close_ep.ep_index = ep_index;
    ioctl(fd, USB_FS_CLOSE, &close_ep);
    memset(&uninit, 0, sizeof(uninit));
    ioctl(fd, USB_FS_UNINIT, &uninit);
    (void)eps;
    (void)ep_count;
}

int usb_bulk_read(int fd, struct usb_fs_endpoint *ep,
                  uint8_t *buf, uint32_t cap, uint32_t *got) {
    return xfer_sync(fd, ep, (uint8_t)VD_USB_EP_INDEX_VIDEO_IN, 1,
                     buf, NULL, cap, got, "video");
}

int usb_bulk_write(int fd, struct usb_fs_endpoint *ep,
                   const uint8_t *data, uint32_t len) {
    return xfer_sync(fd, ep, (uint8_t)VD_USB_EP_INDEX_PAD_OUT, 0,
                     NULL, data, len, NULL, "pad");
}

int usb_ctrl_xfer(int fd, uint8_t bmRequestType, uint8_t bRequest,
                  uint16_t wValue, uint16_t wIndex,
                  void *data, uint16_t len, uint16_t flags) {
    struct usb_ctl_request ctrl;

    if (fd < 0 || (!data && len != 0))
        return -EINVAL;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.ucr_request.bmRequestType = bmRequestType;
    ctrl.ucr_request.bRequest = bRequest;
    USETW(ctrl.ucr_request.wValue, wValue);
    USETW(ctrl.ucr_request.wIndex, wIndex);
    USETW(ctrl.ucr_request.wLength, len);
    ctrl.ucr_data = data;
    ctrl.ucr_flags = flags;
    if (ioctl(fd, USB_DO_REQUEST, &ctrl) != 0)
        return -errno;
    return 0;
}

int usb_uvc_ctrl_xfer(int fd, uint8_t interface_number, uint8_t bRequest,
                      uint8_t cs, const void *out_data, uint16_t out_len,
                      void *in_data, uint16_t in_len) {
    /* UVC 1.1 class-specific interface request: wValue high byte carries the
     * control selector (VS_PROBE_CONTROL / VS_COMMIT_CONTROL). */
    if (out_data && out_len) {
        /* host-to-device, class, interface = 0x21 (SET_CUR) */
        return usb_ctrl_xfer(fd, 0x21, bRequest,
                             (uint16_t)((uint16_t)cs << 8), interface_number,
                             (void *)out_data, out_len, 0);
    }
    /* device-to-host, class, interface = 0xA1 (GET_CUR); short reads are
     * legal for these variable-length blocks. */
    return usb_ctrl_xfer(fd, 0xA1, bRequest,
                         (uint16_t)((uint16_t)cs << 8), interface_number,
                         in_data, in_len, USB_SHORT_XFER_OK);
}

int usb_send_pad_report(int fd, struct usb_fs_endpoint *ep,
                        const VdPadReport *report) {
    uint8_t wire[VD_PAD_WIRE_BYTES];
    size_t n;
    unsigned i;

    /* Pad reports ride SEVEN no-data control OUTs (bRequest 0x50..0x56 =
     * chunk index, 4 bytes per request in wValue/wIndex). EP0 OUT data
     * stages do not deliver on the Vita firmware (proven by eptest.c: even
     * the gadget's own UVC SET_CUR data never arrives), but SETUP-only
     * control transfers demonstrably reach the gadget's processRequest.
     * `ep` is unused. */
    (void)ep;
    if (!report)
        return -EINVAL;
    /* The pad path is wired to the exact 28-byte little-endian wire
     * format; vd_pad_serialize is the single source of truth shared with the
     * Vita-side parser. */
    n = vd_pad_serialize(report, wire, sizeof(wire));
    if (n != VD_PAD_WIRE_BYTES)
        return -EINVAL;
    for (i = 0; i < 7; i++) {
        uint16_t wValue = (uint16_t)(wire[i * 4] | ((uint16_t)wire[i * 4 + 1] << 8));
        uint16_t wIndex = (uint16_t)(wire[i * 4 + 2] | ((uint16_t)wire[i * 4 + 3] << 8));
        /* Device-recipient (0x40): wValue/wIndex are free-form vendor
         * fields. The interface-recipient shape (0x21) validates wIndex as
         * an interface number and EIOs on arbitrary data. */
        int rc = usb_ctrl_xfer(fd, 0x40, (uint8_t)(0x50 + i),
                               wValue, wIndex, NULL, 0, 0);
        if (rc != 0)
            return rc;
    }
    return 0;
}

int usb_send_touch_report(int fd, const VdTouchReport *t) {
    uint8_t wire[VD_TOUCH_WIRE_BYTES];
    size_t n;
    unsigned i;

    /* Touch records ride FOUR no-data control OUTs (bRequest 0x58..0x5B =
     * chunk index, 4 bytes per request in wValue/wIndex) — the same
     * SETUP-only chunk scheme as the pad path; see usb_send_pad_report for
     * why EP0 OUT data stages cannot be used. Chunk 0 (0x58) resets the
     * Vita-side reassembly state. */
    if (!t)
        return -EINVAL;
    /* vd_touch_serialize is the single source of truth for the 16-byte
     * little-endian touch wire format shared with the Vita-side parser. */
    n = vd_touch_serialize(t, wire);
    if (n != VD_TOUCH_WIRE_BYTES)
        return -EINVAL;
    for (i = 0; i < 4; i++) {
        uint16_t wValue = (uint16_t)(wire[i * 4] | ((uint16_t)wire[i * 4 + 1] << 8));
        uint16_t wIndex = (uint16_t)(wire[i * 4 + 2] | ((uint16_t)wire[i * 4 + 3] << 8));
        /* Device-recipient (0x40): wValue/wIndex are free-form vendor
         * fields. The interface-recipient shape (0x21) validates wIndex as
         * an interface number and EIOs on arbitrary data. */
        int rc = usb_ctrl_xfer(fd, 0x40, (uint8_t)(0x58 + i),
                               wValue, wIndex, NULL, 0, 0);
        if (rc != 0)
            return rc;
    }
    return 0;
}

void usb_transfer_release_after_uninit(struct usb_fs_endpoint *ep) {
    struct UsbXferBacking *backing;

    if (!ep || !ep->ppBuffer)
        return;
    backing = (struct UsbXferBacking *)((uint8_t *)ep->ppBuffer -
        offsetof(struct UsbXferBacking, buffers));
    if ((backing->magic != VD_USB_XFER_MAGIC_IN &&
         backing->magic != VD_USB_XFER_MAGIC_OUT) ||
        ep->ppBuffer != backing->buffers ||
        ep->pLength != backing->lengths)
        return;
    clear_endpoint(ep);
    backing->magic = 0;
    free(backing);
}
