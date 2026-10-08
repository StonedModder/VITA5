/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — UVC streaming session implementation.
 *
 * Uses FreeBSD ugen control requests (USB_DO_REQUEST) and USB FS endpoint
 * transfers, following the same ioctl surface as the Ghostcontrol core
 * (reference only).
 */
#include "uvc_stream.h"
#include "usb_transfer.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_endian.h>

#define VD_LOG(...) do { fprintf(stderr, "[uvc] " __VA_ARGS__); } while (0)

/* Class-specific interface control requests (SET_CUR/GET_CUR probe/commit)
 * and SET_INTERFACE are issued through usb_transfer.c. */

/* Select an alternate setting via the ugen USB_SET_ALTINTERFACE ioctl, NOT a
 * raw SET_INTERFACE control transfer. On FreeBSD ugen the alt setting is chosen
 * through this ioctl using the interface's ordinal index; the raw control
 * transfer is not the supported path (it failed on hardware). Detaches any
 * kernel driver first, matching Ghostcontrol's endpoint-claim sequence. */
static int set_interface(int fd, uint8_t interface_index, uint8_t alt)
{
    int iface = (int)interface_index;
    ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface);
    struct usb_alt_interface a;
    memset(&a, 0, sizeof(a));
    a.uai_interface_index = interface_index;
    a.uai_alt_index = alt;
    return ioctl(fd, USB_SET_ALTINTERFACE, &a);
}

int vd_uvc_session_open(VdUvcSession *s, const VdUsbVitaDevice *dev)
{
    if (!s || !dev || !dev->has_video || dev->video_in_count == 0) return -1;
    memset(s, 0, sizeof(*s));
    s->fd = open(dev->path, O_RDWR | O_NONBLOCK);
    if (s->fd < 0) {
        VD_LOG("open %s failed\n", dev->path);
        return -1;
    }
    s->vs_interface = dev->uvc.interface_number;
    s->bulk_in_ep = dev->video_in_eps[0];
    s->vs_alt_active = 0;
    VD_LOG("session open: iface=%u bulk_in=0x%02x\n",
           s->vs_interface, s->bulk_in_ep);
    return 0;
}

int vd_uvc_start_stream(VdUvcSession *s,
                        uint8_t format_index, uint8_t frame_index,
                        uint32_t frame_interval,
                        uint32_t max_frame_size)
{
    if (!s || s->fd < 0) return -1;

    uint32_t max_payload = max_frame_size + UVC_PAYLOAD_HEADER_SIZE;

    /* SAFE ORDERING: open the bulk endpoint BEFORE starting the stream. The
     * endpoint lives on alt 0 (always present), so USB_FS_OPEN binds it without
     * touching stream state. If it fails we return WITHOUT ever committing, so
     * the Vita never starts streaming and cannot be left mid-stream (which is
     * what degraded it on earlier attempts).
     *
     * The Vita sends each frame as ONE bulk request: 12-byte UVC header +
     * the raw NV12 frame (vita-udcd-uvc uvc_frame_req_submit_phycont). So the
     * endpoint buffer and the read must cover a whole frame, or the transfer
     * stalls (ETIMEDOUT). */
    {
        uint32_t maxpkt = 0;
        uint32_t frame_bytes = max_frame_size + UVC_PAYLOAD_HEADER_SIZE;
        uint32_t max_bufsize = frame_bytes + 4096u; /* slack */
        int orc = usb_transfer_open_endpoint(s->fd, s->eps, 2,
                                             (uint8_t)VD_USB_EP_INDEX_VIDEO_IN,
                                             s->bulk_in_ep,
                                             (int)s->vs_interface,
                                             max_bufsize, &maxpkt);
        if (orc != 0) {
            VD_LOG("endpoint OPEN failed rc=%d (bulk IN 0x%02x) — not starting stream\n",
                   orc, s->bulk_in_ep);
            return -6;
        }
        s->ep_opened = 1;
        s->maxpkt = maxpkt;
        s->frame_bytes = frame_bytes;
    }

    /* 1. SET_CUR on VS_PROBE_CONTROL */
    VdUvcStreamingControl probe;
    vd_uvc_default_probe(&probe, format_index, frame_index,
                         frame_interval, max_frame_size, max_payload);
    /* EP0 OUT data stages are flaky on this firmware (they can fail or carry
     * no data at all): a failed PROBE/COMMIT SET_CUR is tolerated - the gadget
     * streams on its default (fmt1/frame1) parameters regardless, and the
     * caller re-commits periodically while idle. */
    if (usb_uvc_ctrl_xfer(s->fd, s->vs_interface, UVC_SET_CUR,
                          UVC_VS_PROBE_CONTROL,
                          &probe, sizeof(probe), NULL, 0) != 0) {
        VD_LOG("PROBE SET_CUR failed (tolerated)\n");
    }

    /* 2. GET_CUR on VS_PROBE_CONTROL — read back negotiated parameters */
    VdUvcStreamingControl negotiated;
    memset(&negotiated, 0, sizeof(negotiated));
    if (usb_uvc_ctrl_xfer(s->fd, s->vs_interface, UVC_GET_CUR,
                          UVC_VS_PROBE_CONTROL,
                          NULL, 0, &negotiated, sizeof(negotiated)) != 0) {
        VD_LOG("PROBE GET_CUR failed (tolerated, using requested params)\n");
        negotiated = probe;
    }
    s->probe = negotiated;

    /* 3. SET_CUR on VS_COMMIT_CONTROL — apply and start the stream. Only reached
     * once the endpoint is confirmed open, so the stream always has a live bulk
     * reader and is torn down cleanly on close. */
    if (usb_uvc_ctrl_xfer(s->fd, s->vs_interface, UVC_SET_CUR,
                          UVC_VS_COMMIT_CONTROL,
                          &negotiated, sizeof(negotiated), NULL, 0) != 0) {
        VD_LOG("COMMIT SET_CUR failed (tolerated)\n");
    }

    /* Diagnostic: read back the committed parameters to confirm the Vita
     * accepted the commit (frame_index must be non-zero for frames to flow). */
    {
        VdUvcStreamingControl committed;
        memset(&committed, 0, sizeof(committed));
        if (usb_uvc_ctrl_xfer(s->fd, s->vs_interface, UVC_GET_CUR,
                              UVC_VS_COMMIT_CONTROL,
                              NULL, 0, &committed, sizeof(committed)) == 0) {
            VD_LOG("COMMIT readback: fmt=%u frame=%u interval=%u\n",
                   committed.bFormatIndex, committed.bFrameIndex,
                   committed.dwFrameInterval);
            s->probe = committed;
        } else {
            VD_LOG("COMMIT GET_CUR readback failed\n");
        }
    }

    s->last_errno = 0;
    s->vs_alt_active = 0;
    s->streaming = 1;
    VD_LOG("streaming: fmt=%u frame=%u interval=%u frame_size=%u maxpkt=%u\n",
           negotiated.bFormatIndex, negotiated.bFrameIndex,
           negotiated.dwFrameInterval, negotiated.dwMaxVideoFrameSize, s->maxpkt);
    return 0;
}

void vd_uvc_recommit(VdUvcSession *s)
{
    if (!s || s->fd < 0)
        return;
    (void)usb_uvc_ctrl_xfer(s->fd, s->vs_interface, UVC_SET_CUR,
                            UVC_VS_PROBE_CONTROL,
                            &s->probe, sizeof(s->probe), NULL, 0);
    (void)usb_uvc_ctrl_xfer(s->fd, s->vs_interface, UVC_SET_CUR,
                            UVC_VS_COMMIT_CONTROL,
                            &s->probe, sizeof(s->probe), NULL, 0);
}

int vd_uvc_stop_stream(VdUvcSession *s)
{
    if (!s || s->fd < 0 || !s->streaming) return 0;
    /* SET_INTERFACE alt 0 -> the Vita aborts streaming (per vita-udcd-uvc). */
    set_interface(s->fd, s->vs_interface, 0);
    s->vs_alt_active = 0;
    s->streaming = 0;
    VD_LOG("stream stopped\n");
    return 0;
}

int vd_uvc_read_payload(VdUvcSession *s, uint8_t *buf, unsigned cap,
                        int *frame_done, int *fid)
{
    if (!s || s->fd < 0 || !buf) return -1;
    if (frame_done) *frame_done = 0;
    if (fid) *fid = 0;

    /* Read a whole frame in bounded chunks. The Vita sends header+NV12 as one
     * bulk request (frame_bytes), but a single transfer sized to the full frame
     * never completes on ugen (the exact-fit case stalls) while a chunk that
     * fills its buffer completes on buffer-full. So pull the frame in <=64 KiB
     * chunks and reassemble; the final chunk lands on the device's short packet. */
    uint32_t total = s->frame_bytes ? s->frame_bytes : (uint32_t)cap;
    if (total > (uint32_t)cap) total = (uint32_t)cap;
    enum { CHUNK = 65536 };
    uint32_t got_total = 0;
    while (got_total < total) {
        uint32_t want = total - got_total;
        if (want > CHUNK) want = CHUNK;
        uint32_t got = 0;
        int rc = usb_bulk_read(s->fd, &s->eps[VD_USB_EP_INDEX_VIDEO_IN],
                               buf + got_total, want, &got);
        if (rc != 0) {
            /* Preserve idle/disconnect errno for the producer's recovery
             * policy instead of collapsing every transfer failure to -2. */
            s->last_errno = -rc;
            return rc;
        }
        s->last_errno = 0;
        if (got == 0) break;
        got_total += got;
    }
    if (got_total < UVC_PAYLOAD_HEADER_SIZE) return -2;

    /* Parse the 12-byte UVC payload header. */
    uint8_t header_len = buf[0];
    uint8_t info = buf[1];
    if (header_len < 2 || header_len > got_total) return -3;

    if (fid) *fid = (info & UVC_STREAM_FID) ? 1 : 0;
    if (frame_done) *frame_done = (info & UVC_STREAM_EOF) ? 1 : 0;

    if (info & UVC_STREAM_ERR) {
        VD_LOG("payload ERR bit set, dropping\n");
        return -4;
    }
    /* Return payload length excluding header so caller writes only pixel data.
     * Callers that need raw bytes can use header_len to skip. */
    return (int)(got_total - header_len);
}

void vd_uvc_session_close(VdUvcSession *s)
{
    if (!s) return;
    if (s->streaming) vd_uvc_stop_stream(s);
    if (s->ep_opened) {
        usb_transfer_close_endpoint(s->fd, s->eps, 2,
                                    (uint8_t)VD_USB_EP_INDEX_VIDEO_IN);
        s->ep_opened = 0;
    }
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    /* Closing the ugen fd synchronously tears down every queued transfer, so
     * any quarantined helper-owned buffer is safe to release now. */
    usb_transfer_release_after_uninit(&s->eps[VD_USB_EP_INDEX_VIDEO_IN]);
}
