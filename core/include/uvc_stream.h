/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — UVC streaming session: probe/commit handshake and bulk frame
 * capture. Talks to the Vita's VideoStreaming interface over an open ugen fd.
 *
 * Control flow (from vita-udcd-uvc):
 *   1. SET_CUR  on VS_PROBE_CONTROL   -> send desired streaming parameters
 *   2. GET_CUR  on VS_PROBE_CONTROL   -> read back negotiated parameters
 *   3. SET_CUR  on VS_COMMIT_CONTROL  -> apply, the Vita starts streaming
 *   4. SET_INTERFACE alt 1 on the streaming iface -> enable the bulk endpoint
 *   5. bulk IN reads -> NV12 payloads with 12-byte UVC headers
 *   6. SET_INTERFACE alt 0 -> stop (Vita aborts streaming)
 */
#pragma once
#include <stdint.h>
#include <dev/usb/usb_ioctl.h>
#include "uvc_protocol.h"
#include "usb_vita.h"

typedef struct {
    int fd;                        /* open ugen fd for the Vita device */
    uint8_t vs_interface;          /* bInterfaceNumber of VideoStreaming */
    uint8_t vs_alt_active;         /* current alternate setting (0/1) */
    uint8_t bulk_in_ep;            /* endpoint address, e.g. 0x81 */
    VdUvcStreamingControl probe;   /* last negotiated parameters */
    int streaming;
    int last_errno;                /* last transfer errno (0 after a successful read) */
    /* usb_fs endpoint slots (registered with USB_FS_INIT). Slot
     * VD_USB_EP_INDEX_VIDEO_IN (0) = bulk IN 0x81 (UVC NV12 payloads);
     * slot VD_USB_EP_INDEX_PAD_OUT (1) = vendor OUT (pad reports). Persistent
     * across reads so the transfer helper's in-flight guard and its post-UNINIT
     * quarantine release stay reachable. */
    struct usb_fs_endpoint eps[2];
    int ep_opened;
    uint32_t maxpkt;
    uint32_t frame_bytes;          /* negotiated frame_size + 12-byte header */
} VdUvcSession;

/* Open the device node and pick the VideoStreaming interface + bulk IN endpoint
 * from the already-parsed device. Returns 0 on success. */
int vd_uvc_session_open(VdUvcSession *s, const VdUsbVitaDevice *dev);

/* Run the full probe/commit handshake for the chosen format/frame/interval.
 * `format_index`, `frame_index` are 1-based UVC indices. `frame_interval` is
 * in 100ns units (use a parsed frame's intervals[] or default_interval).
 * On success the session is ready to read frames (alt setting active). */
int vd_uvc_start_stream(VdUvcSession *s,
                        uint8_t format_index, uint8_t frame_index,
                        uint32_t frame_interval,
                        uint32_t max_frame_size);

/* Re-send the probe/commit SET_CUR pair without touching the endpoint
 * session. The gadget starts streaming when a COMMIT data stage lands; on
 * this firmware those stages are flaky, so a session that goes idle retries
 * the commit periodically. Cheap and always safe. */
void vd_uvc_recommit(VdUvcSession *s);

/* Stop streaming: SET_INTERFACE alt 0 (Vita aborts). Safe to call when idle. */
int vd_uvc_stop_stream(VdUvcSession *s);

/* Read one UVC payload into `buf` (capacity `cap`). The buffer retains the
 * UVC header at buf[0]; return value is PIXEL bytes EXCLUDING that header,
 * or <0 on error. Copy exactly the returned count from buf + buf[0] (do not
 * subtract the header length again). *frame_done reflects the EOF bit.
 * Transport failures preserve their negative errno (including -ETIMEDOUT
 * and -ENXIO) and set last_errno; transport failures leave EOF/FID clear. */
int vd_uvc_read_payload(VdUvcSession *s, uint8_t *buf, unsigned cap,
                        int *frame_done, int *fid);

void vd_uvc_session_close(VdUvcSession *s);
