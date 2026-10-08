/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — UAC isochronous audio capture implementation.
 *
 * Mirrors the proven bulk transfer discipline in usb_transfer.c (helper-owned
 * transfer state, USB_FS_START -> USB_FS_COMPLETE wait with session-FIFO
 * record validation, bounded cancel-and-reap, STOP/CLOSE/UNINIT teardown) but
 * framed for isochronous per the ugen kernel contract (sys/dev/usb/usb_generic.c,
 * ugen_fs_copy_in/ugen_fs_copy_out, FreeBSD releng/11.4):
 *   - ppBuffer[i] / pLength[i] are PER-USB-FRAME arrays; nFrames is the number
 *     of frames in this transfer and must not exceed the max_frames read back
 *     from USB_FS_OPEN.
 *   - the KERNEL buffers all frames in one DMA buffer ("isochronous USB
 *     transfer only use one buffer, but can have multiple frame lengths!"),
 *     and on copy-out packet i is delivered to ppBuffer[i]. Session frame_ptrs
 *     therefore stride the user buffer by maxpkt: packet i lands at
 *     buffer + i * maxpkt (VD_UAC_LAYOUT_STRIDED_MAXPKT, now confirmed).
 *
 * CRITICAL ORDERING RULE (root cause of the historical "isoc IN START fail
 * errno=22"): USB_SET_ALTINTERFACE is handled by ugen_set_interface(), whose
 * first action is ugen_fs_uninit(f) — it tears down the whole usbfs session on
 * that fd (f->fs_xfer = NULL, f->fs_ep_max = 0). After any SET_INTERFACE the
 * USB_FS_START / USB_FS_STOP handlers fail their "ep_index >= fs_ep_max" /
 * "fs_xfer[ep_index] == NULL" checks and return EINVAL. So the alternate
 * setting is selected BEFORE USB_FS_INIT, and never again afterwards.
 *
 * hardware-verified on a PS5 (VitaUSBStream, 2026-10-05): 56 transfers /
 * 1792 packets / 344,064 PCM bytes in 2.02 s with zero read errors; see
 * docs/STREAMING.md "Audio (UAC)".
 */
#include "uac_audio.h"
#include "usb_transfer.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_endian.h>

#define VD_LOG(...)                                                                                \
    do                                                                                             \
    {                                                                                              \
        fprintf(stderr, "[uac] " __VA_ARGS__);                                                     \
    } while (0)

#define VD_UAC_XFER_TIMEOUT_MS 250u
#define VD_UAC_WALL_GRACE_MS 200u
#define VD_UAC_DRAIN_MS 250u

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    struct timeval tv;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    if (gettimeofday(&tv, NULL) == 0)
        return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
    return (uint64_t)time(NULL) * 1000u;
}

static int wait_slice(int fd, uint64_t deadline_ms)
{
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

static void clear_transfer_fields(struct usb_fs_endpoint *ep)
{
    ep->ppBuffer = NULL;
    ep->pLength = NULL;
    ep->nFrames = 0;
    ep->aFrames = 0;
    ep->flags = 0;
    ep->timeout = 0;
    ep->isoc_time_complete = 0;
    ep->status = 0;
}

static void stop_ep(int fd, uint8_t ep_index, const char *tag)
{
    struct usb_fs_stop stop;
    memset(&stop, 0, sizeof(stop));
    stop.ep_index = ep_index;
    if (ioctl(fd, USB_FS_STOP, &stop) != 0 && errno != ENXIO && errno != ENOTTY && errno != EBADF)
        VD_LOG("%s STOP fail errno=%d\n", tag, errno);
}

/* Wait for THIS session's completion record (the audio session has exactly
 * one endpoint slot, so an unexpected record is drained and logged, never
 * mistaken for ours — same session-FIFO discipline as usb_transfer.c). */
static int wait_completion(int fd, struct usb_fs_endpoint *ep, uint8_t ep_index,
                           uint32_t queued_frames, uint64_t deadline_ms, const char *tag,
                           int *reaped)
{
    for (;;)
    {
        struct usb_fs_complete complete;
        enum UsbOutCompletionCheck check;
        int result;

        if (monotonic_ms() >= deadline_ms)
            return -ETIMEDOUT;

        memset(&complete, 0, sizeof(complete));
        result = ioctl(fd, USB_FS_COMPLETE, &complete);
        if (result == 0)
        {
            check = vd_uac_check_completion(complete.ep_index, ep_index, ep->status, ep->aFrames,
                                            queued_frames);
            if (check == USB_OUT_COMPLETION_OTHER)
            {
                VD_LOG("%s observed unexpected completion ep=%u while waiting for ep=%u\n", tag,
                       (unsigned)complete.ep_index, (unsigned)ep_index);
                continue;
            }
            *reaped = 1;
            if (check == USB_OUT_COMPLETION_STATUS_ERROR)
            {
                VD_LOG("%s completion status=%d\n", tag, ep->status);
                return usb_status_to_errno(ep->status);
            }
            if (check == USB_OUT_COMPLETION_FRAME_ERROR)
            {
                VD_LOG("%s invalid completed frames=%u (queued %u)\n", tag, (unsigned)ep->aFrames,
                       (unsigned)queued_frames);
                return -EIO;
            }
            return 0;
        }
        int complete_error = errno;
        if (complete_error != EBUSY && complete_error != EINTR)
        {
            VD_LOG("USB_FS_COMPLETE fd=%d ep=%u rc=%d errno=%d raw_status=%d reaped=0\n", fd,
                   (unsigned)ep_index, result, complete_error, ep->status);
            return -complete_error;
        }
        result = wait_slice(fd, deadline_ms);
        if (result <= 0)
            return result == 0 ? -ETIMEDOUT : result;
    }
}

/* STOP is asynchronous: keep the endpoint framing alive until its cancel (or
 * raced normal) completion is copied out. If the device never produces that
 * record, USB_FS_CLOSE synchronously unsets the transfer; when even that
 * fails, the session is marked dead and only fd close may release memory. */
static int cancel_and_reap(VdUacSession *s, const char *tag)
{
    struct usb_fs_close close_ep;
    int reaped = 0;
    int result;

    stop_ep(s->fd, VD_UAC_EP_INDEX_ISOC_IN, tag);
    result =
        wait_completion(s->fd, &s->eps[VD_UAC_EP_INDEX_ISOC_IN], (uint8_t)VD_UAC_EP_INDEX_ISOC_IN,
                        s->frames_per_xfer, monotonic_ms() + VD_UAC_DRAIN_MS, tag, &reaped);
    if (reaped)
        return 0;

    memset(&close_ep, 0, sizeof(close_ep));
    close_ep.ep_index = (uint8_t)VD_UAC_EP_INDEX_ISOC_IN;
    if (ioctl(s->fd, USB_FS_CLOSE, &close_ep) == 0)
        return 0;
    if (errno == EBADF || errno == ENXIO || errno == ENOTTY || errno == EINVAL)
        return 0;
    VD_LOG("%s could not reap cancellation or close ep=%u errno=%d\n", tag,
           (unsigned)VD_UAC_EP_INDEX_ISOC_IN, errno);
    return result < 0 ? result : -EIO;
}

/* Select an alternate setting: USB_SET_ALTINTERFACE ioctl (the ugen-supported
 * path, same as uvc_stream.c), falling back to a raw SET_INTERFACE control
 * request (USB 2.0 table 9-4). Both may fail on ugen; USB_FS_OPEN also selects
 * the alt that owns the endpoint, so callers treat failure as non-fatal and
 * record the errno in s->last_errno.
 *
 * MUST be called only while NO USB_FS session exists on this fd: the
 * USB_SET_ALTINTERFACE ioctl is dispatched to ugen_set_interface(), which
 * begins with ugen_fs_uninit(f) ("make sure all FIFO's are gone") and thereby
 * destroys f->fs_xfer / f->fs_ep_max / f->fs_ep_ptr — every later
 * USB_FS_START or USB_FS_STOP on the fd then fails with EINVAL. The raw
 * control fallback (USB_DO_REQUEST) does not touch usbfs state at all. */
static int set_interface(int fd, uint8_t interface_index, uint8_t alt)
{
    int iface = (int)interface_index;
    struct usb_alt_interface a;

    ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface);
    memset(&a, 0, sizeof(a));
    a.uai_interface_index = interface_index;
    a.uai_alt_index = alt;
    if (ioctl(fd, USB_SET_ALTINTERFACE, &a) == 0)
        return 0;
    return usb_ctrl_xfer(fd, 0x01, UR_SET_INTERFACE, alt, interface_index, NULL, 0, 0);
}

int vd_uac_session_open(VdUacSession *s, const VdUsbVitaDevice *dev)
{
    if (!s || !dev || !dev->has_audio || dev->audio_isoc_ep == 0)
        return -1;
    memset(s, 0, sizeof(*s));
    s->fd = open(dev->path, O_RDWR | O_NONBLOCK);
    if (s->fd < 0)
    {
        VD_LOG("open %s failed\n", dev->path);
        return -1;
    }
    s->as_interface = dev->audio_stream_interface;
    s->as_alt = dev->audio_isoc_alt;
    s->isoc_in_ep = dev->audio_isoc_ep;
    s->maxpkt = dev->audio_isoc_maxpkt;
    s->layout = VD_UAC_LAYOUT_STRIDED_MAXPKT;
    s->frames_per_xfer = VD_UAC_DEFAULT_FRAMES_PER_XFER;
    VD_LOG("session open: as_iface=%u alt=%u isoc_in=0x%02x maxpkt=%u\n", s->as_interface,
           s->as_alt, s->isoc_in_ep, s->maxpkt);
    return 0;
}

int vd_uac_start(VdUacSession *s, uint32_t frames_per_xfer)
{
    struct usb_fs_init init;
    struct usb_fs_open open;
    uint16_t maxpkt;
    int detach_iface;

    if (!s || s->fd < 0 || s->isoc_in_ep == 0)
        return -EINVAL;
    if (s->streaming || s->ep_opened)
        return -EBUSY;
    if (frames_per_xfer == 0)
        frames_per_xfer = VD_UAC_DEFAULT_FRAMES_PER_XFER;
    if (frames_per_xfer > VD_UAC_MAX_FRAMES)
        return -EINVAL;
    if (s->maxpkt == 0)
        return -EINVAL;

    /* Claim the interface from any kernel driver (Ghostcontrol's claim
     * sequence; best-effort like the bulk path). */
    detach_iface = (int)s->as_interface;
    ioctl(s->fd, USB_IFACE_DRIVER_DETACH, &detach_iface);

    /* Select the active alt BEFORE creating any USB_FS state. The ordering is
     * mandatory: ugen_set_interface() (the USB_SET_ALTINTERFACE handler) runs
     * ugen_fs_uninit() on this fd and destroys any usbfs session, so a
     * SET_INTERFACE after USB_FS_INIT/OPEN makes every later USB_FS_START and
     * USB_FS_STOP fail with EINVAL ("isoc IN START fail errno=22"). Selecting
     * alt here also guarantees USB_FS_OPEN sees the alt that owns the isoc
     * endpoint (usbd_get_ep_by_addr() only resolves the endpoint descriptor of
     * the currently active alt). Failure is non-fatal and recorded: the
     * fallback raw SET_INTERFACE may also fail on ugen, and USB_FS_OPEN will
     * loudly fail if the endpoint is really unreachable. */
    if (set_interface(s->fd, s->as_interface, s->as_alt) != 0)
    {
        s->last_errno = errno;
        VD_LOG("alt %u select failed errno=%d (USB_FS_OPEN requires the owning alt)\n", s->as_alt,
               s->last_errno);
    }
    else
    {
        s->last_errno = 0;
    }
    /* ugen_set_interface() ends in usb_probe_and_attach(); detach again so no
     * kernel driver owns the interface going forward (USB_IFACE_DRIVER_DETACH
     * only detaches drivers and pins the parent iface — it never touches
     * usbfs state, so it is safe at any time). */
    ioctl(s->fd, USB_IFACE_DRIVER_DETACH, &detach_iface);

    memset(&init, 0, sizeof(init));
    init.pEndpoints = s->eps;
    init.ep_index_max = 1u;
    if (ioctl(s->fd, USB_FS_INIT, &init) != 0)
    {
        int e = errno;
        VD_LOG("USB_FS_INIT fail errno=%d\n", e);
        return -e;
    }

    /* max_bufsize must be a sane multiple of the endpoint max packet (an
     * oversized value is rejected with EINVAL — see the bulk pitfall), so use
     * exactly packets * maxpkt.
     *
     * USB_FS_MAX_FRAMES_PRE_SCALE declares max_frames in units of 1 ms USB
     * frames; the kernel converts it to 125 us micro-frame slots itself
     * (usbd_transfer_setup_sub: nframes <<= (3 - fps_shift), fps_shift =
     * bInterval - 1 for HS isoc). For the Vita's 1 ms endpoint (bInterval 4)
     * that scaling is a no-op; for sub-millisecond endpoints it raises the
     * per-transfer frame cap up to 8x, which is why libusb20 allocates its
     * per-frame arrays 8x large when using it (our frame_ptrs already holds
     * VD_UAC_MAX_FRAMES = 256 = 8 * 32 entries and the kernel only ever
     * touches pLength/ppBuffer[0..nFrames-1]). The kernel reads max_frames
     * back as the actual (scaled) frame count — see below. */
    memset(&open, 0, sizeof(open));
    open.ep_index = (uint8_t)VD_UAC_EP_INDEX_ISOC_IN;
    open.ep_no = s->isoc_in_ep;
    open.max_bufsize = frames_per_xfer * (uint32_t)s->maxpkt;
    open.max_frames = frames_per_xfer | USB_FS_MAX_FRAMES_PRE_SCALE;
    if (ioctl(s->fd, USB_FS_OPEN, &open) != 0)
    {
        int e = errno;
        struct usb_fs_uninit uninit;
        VD_LOG("USB_FS_OPEN ep_no=0x%02x fail errno=%d\n", s->isoc_in_ep, e);
        memset(&uninit, 0, sizeof(uninit));
        ioctl(s->fd, USB_FS_UNINIT, &uninit);
        return -e;
    }
    maxpkt = open.max_packet_length ? open.max_packet_length : s->maxpkt;
    /* Kernel contract (usb_generic.c USB_FS_OPEN): max_frames is read back as
     * the transfer's actual frame count; a START with nFrames above it is
     * rejected ("security check"). Pre-scaling only ever increases it, so a
     * smaller read-back means a divergent kernel clamped us — obey it. */
    if (open.max_frames != 0 && open.max_frames < frames_per_xfer)
    {
        VD_LOG("kernel clamped max_frames %u -> %u\n", frames_per_xfer, open.max_frames);
        frames_per_xfer = open.max_frames;
    }

    s->frames_per_xfer = frames_per_xfer;
    s->maxpkt = maxpkt;
    s->buffer_cap = frames_per_xfer * (uint32_t)maxpkt;
    s->buffer = (uint8_t *)malloc(s->buffer_cap);
    s->lengths = (uint32_t *)malloc((size_t)frames_per_xfer * sizeof(uint32_t));
    if (!s->buffer || !s->lengths)
    {
        struct usb_fs_close close_ep;
        struct usb_fs_uninit uninit;
        free(s->buffer);
        free(s->lengths);
        s->buffer = NULL;
        s->lengths = NULL;
        memset(&close_ep, 0, sizeof(close_ep));
        close_ep.ep_index = (uint8_t)VD_UAC_EP_INDEX_ISOC_IN;
        ioctl(s->fd, USB_FS_CLOSE, &close_ep);
        memset(&uninit, 0, sizeof(uninit));
        ioctl(s->fd, USB_FS_UNINIT, &uninit);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < frames_per_xfer; ++i)
        s->frame_ptrs[i] = s->buffer + (size_t)i * s->maxpkt;
    s->ep_opened = 1;

    /* NOTE: the alt is selected BEFORE USB_FS_INIT above and must not be
     * re-selected here — ugen_set_interface() would uninit the just-opened
     * session and every USB_FS_START would then fail with EINVAL. */
    s->streaming = 1;
    VD_LOG("streaming: isoc IN 0x%02x maxpkt=%u frames/xfer=%u (%u bytes/xfer)\n", s->isoc_in_ep,
           s->maxpkt, frames_per_xfer, s->buffer_cap);
    return 0;
}

int vd_uac_read(VdUacSession *s, uint8_t *pcm, uint32_t pcm_cap, uint32_t *pcm_len,
                uint32_t *packet_lengths, uint32_t packet_cap, uint32_t *packet_count)
{
    struct usb_fs_start start;
    struct usb_fs_endpoint *ep;
    uint64_t deadline_ms;
    uint32_t frames_done;
    uint32_t copy_frames;
    int reaped = 0;
    int result;

    if (pcm_len)
        *pcm_len = 0;
    if (packet_count)
        *packet_count = 0;
    if (!s || s->fd < 0 || !pcm || !pcm_len)
        return -EINVAL;
    if (s->dead)
        return -EIO;
    if (!s->streaming || !s->ep_opened || !s->buffer || !s->lengths)
        return -EINVAL;

    ep = &s->eps[VD_UAC_EP_INDEX_ISOC_IN];
    /* Never overwrite the framing of a possibly in-flight transfer. */
    if (ep->ppBuffer || ep->pLength || ep->nFrames != 0)
        return -EBUSY;

    /* One buffer, nFrames packet slots: request maxpkt bytes per USB frame
     * (device may deliver less per packet — MULTI_SHORT_OK). */
    vd_uac_init_frame_lengths(s->lengths, s->frames_per_xfer, s->maxpkt);
    ep->ppBuffer = s->frame_ptrs;
    ep->pLength = s->lengths;
    ep->nFrames = s->frames_per_xfer;
    ep->aFrames = 0;
    ep->flags = USB_FS_FLAG_MULTI_SHORT_OK;
    ep->timeout = (uint16_t)VD_UAC_XFER_TIMEOUT_MS;
    ep->isoc_time_complete = 0;
    ep->status = 0;

    memset(&start, 0, sizeof(start));
    start.ep_index = (uint8_t)VD_UAC_EP_INDEX_ISOC_IN;
    if (ioctl(s->fd, USB_FS_START, &start) != 0)
    {
        int e = errno;
        VD_LOG("isoc IN START fail errno=%d\n", e);
        if (e == EBUSY)
        {
            /* An OLDER transfer is still queued and the kernel may still
             * copy out through this framing (same persistent buffers).
             * Never clear the framing while it could be addressed: keep the
             * in-flight guard and cancel/reap the old transfer first. Only a
             * reaped cancel makes the framing safe to clear; otherwise the
             * session is marked dead and fd close is the teardown (same
             * discipline as usb_transfer.c's EBUSY START path). */
            if (cancel_and_reap(s, "isoc") == 0)
                clear_transfer_fields(ep);
            else
                s->dead = 1;
            return -e;
        }
        stop_ep(s->fd, (uint8_t)VD_UAC_EP_INDEX_ISOC_IN, "isoc");
        clear_transfer_fields(ep);
        return -e;
    }

    deadline_ms = monotonic_ms() + VD_UAC_XFER_TIMEOUT_MS + VD_UAC_WALL_GRACE_MS;
    result = wait_completion(s->fd, ep, (uint8_t)VD_UAC_EP_INDEX_ISOC_IN, ep->nFrames, deadline_ms,
                             "isoc", &reaped);
    if (result != 0)
    {
        VD_LOG("isoc IN %s error=%d\n", result == -ETIMEDOUT ? "timeout" : "completion", -result);
        if (reaped)
        {
            stop_ep(s->fd, (uint8_t)VD_UAC_EP_INDEX_ISOC_IN, "isoc");
        }
        else if (cancel_and_reap(s, "isoc") != 0)
        {
            /* The kernel may still address the framing; refuse further reads
             * and let fd close in vd_uac_session_close() be the teardown. */
            s->dead = 1;
            return result;
        }
        clear_transfer_fields(ep);
        return result;
    }

    frames_done = ep->aFrames;
    copy_frames = frames_done;
    if (packet_lengths && copy_frames > packet_cap)
        copy_frames = packet_cap;
    if (packet_lengths && copy_frames != 0)
        memcpy(packet_lengths, s->lengths, (size_t)copy_frames * sizeof(uint32_t));
    if (packet_count)
        *packet_count = copy_frames;

    result = vd_uac_reassemble(s->buffer, s->buffer_cap, s->lengths, frames_done, s->maxpkt,
                               s->layout, pcm, pcm_cap, pcm_len, NULL);
    clear_transfer_fields(ep);
    return result;
}

void vd_uac_stop(VdUacSession *s)
{
    struct usb_fs_close close_ep;
    struct usb_fs_uninit uninit;

    if (!s || s->fd < 0)
        return;
    if (!s->streaming && !s->ep_opened)
        return;

    if (!s->dead)
    {
        if (s->eps[VD_UAC_EP_INDEX_ISOC_IN].nFrames != 0)
        {
            /* Defensive: an in-flight transfer must be reaped before its
             * framing is released. */
            if (cancel_and_reap(s, "stop") != 0)
                s->dead = 1;
            else
                clear_transfer_fields(&s->eps[VD_UAC_EP_INDEX_ISOC_IN]);
        }
        /* Close the FS session BEFORE any SET_INTERFACE: the alt-0 select runs
         * ugen_fs_uninit() on this fd (see set_interface()), so doing it first
         * would turn these teardown ioctls into EINVAL noise. */
        memset(&close_ep, 0, sizeof(close_ep));
        close_ep.ep_index = (uint8_t)VD_UAC_EP_INDEX_ISOC_IN;
        ioctl(s->fd, USB_FS_CLOSE, &close_ep);
        memset(&uninit, 0, sizeof(uninit));
        ioctl(s->fd, USB_FS_UNINIT, &uninit);
        /* alt 0 = zero-bandwidth: the Vita stops sending isoc packets. */
        set_interface(s->fd, s->as_interface, 0);
    }
    s->ep_opened = 0;
    s->streaming = 0;
    if (!s->dead)
    {
        free(s->buffer);
        free(s->lengths);
        s->buffer = NULL;
        s->lengths = NULL;
    }
    VD_LOG("stream stopped\n");
}

void vd_uac_session_close(VdUacSession *s)
{
    if (!s)
        return;
    if (s->streaming || s->ep_opened)
        vd_uac_stop(s);
    if (s->fd >= 0)
    {
        /* Closing the ugen fd synchronously tears down every queued
         * transfer, so any deferred buffer is safe to release now. */
        close(s->fd);
        s->fd = -1;
    }
    free(s->buffer);
    free(s->lengths);
    s->buffer = NULL;
    s->lengths = NULL;
    s->dead = 0;
}
