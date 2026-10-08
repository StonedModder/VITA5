/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — synchronous FreeBSD usb_fs bulk/control transfers over an open
 * /dev/ugen* fd (PS5 root payload). Ported from Ghostcontrol's proven
 * usb_helpers.c transfer pattern, generalized to both directions.
 *
 * SESSION-WIDE FIFO DISCIPLINE (kept from the reference implementation):
 *   USB_FS_COMPLETE dequeues the oldest completion record for the whole ugen
 *   session — it is NOT an endpoint-selective wait. The caller must serialize
 *   IN and OUT transfers on a session: never leave one transfer outstanding
 *   while starting another, or the waiters steal each other's completion
 *   records (the wait loops drain foreign records, so a concurrent transfer
 *   would starve until timeout).
 *
 * BUFFER OWNERSHIP (kept from the reference implementation):
 *   Every transfer helper owns a private copy of its packet for the whole
 *   lifetime of the kernel transfer (OUT: copied in before USB_FS_START; IN:
 *   copied out after completion), so caller stack buffers are safe to use. If
 *   a transfer can neither complete nor be synchronously cancelled and reaped,
 *   its backing allocation is QUARANTINED — never freed while the kernel could
 *   still address it — and stays alive until usb_transfer_release_after_uninit()
 *   after USB_FS_UNINIT (or closing the ugen fd) succeeds.
 *
 * ENDPOINT SLOTS:
 *   struct usb_fs_endpoint carries no endpoint index; the index is the slot
 *   the caller registers with USB_FS_INIT / USB_FS_OPEN (usb_fs_open.ep_index).
 *   VITA5 fixes the slots as:
 *     VD_USB_EP_INDEX_VIDEO_IN 0 — bulk IN 0x81, UVC NV12 payloads (Vita->PS5)
 *     VD_USB_EP_INDEX_PAD_OUT  1 — vendor OUT, pad reports    (PS5->Vita)
 *   usb_bulk_read() is bound to the video-IN slot and usb_bulk_write() /
 *   usb_send_pad_report() to the pad-OUT slot. Pass the SAME persistent
 *   endpoint struct to every call (e.g. one per session, not per call): it
 *   must stay alive across transfers so the in-flight guard (-EBUSY) and the
 *   post-UNINIT release can do their jobs.
 *
 * All functions return 0 on success or a negative errno value.
 */
#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include "pad_passthrough.h"
#include "usb_transfer_state.h"

#define VD_USB_EP_INDEX_VIDEO_IN 0u
#define VD_USB_EP_INDEX_PAD_OUT  1u

/* Synchronous bulk IN (video hot path): read one UVC payload from the Vita's
 * endpoint 0x81 into `buf` (capacity `cap`). Short reads are legal and return
 * success with *got < cap. On success *got holds the bytes delivered. */
int usb_bulk_read(int fd, struct usb_fs_endpoint *ep,
                  uint8_t *buf, uint32_t cap, uint32_t *got);

/* Synchronous bulk OUT: send `len` bytes to the Vita's vendor OUT endpoint
 * (pad reports). The helper owns a copy until completion, so `data` may point
 * at a stack buffer. */
int usb_bulk_write(int fd, struct usb_fs_endpoint *ep,
                   const uint8_t *data, uint32_t len);

/* Control transfer via USB_DO_REQUEST. `flags` is ucr_flags (use
 * USB_SHORT_XFER_OK for IN transfers that may legally be short). */
int usb_ctrl_xfer(int fd, uint8_t bmRequestType, uint8_t bRequest,
                  uint16_t wValue, uint16_t wIndex,
                  void *data, uint16_t len, uint16_t flags);

/* UVC class-specific interface control request (SET_CUR/GET_CUR probe/commit
 * on VS_PROBE_CONTROL / VS_COMMIT_CONTROL): cs is the selector, sent in
 * wValue's high byte, wIndex is the VideoStreaming interface number. Exactly
 * one of the out/in buffers is used per direction (see uvc_stream.c). */
int usb_uvc_ctrl_xfer(int fd, uint8_t interface_number, uint8_t bRequest,
                      uint8_t cs, const void *out_data, uint16_t out_len,
                      void *in_data, uint16_t in_len);

/* Pad OUT path: serialize `report` with the shared 28-byte wire format
 * (vd_pad_serialize, the single source of truth for the Vita-side layout) and
 * send it as one bulk OUT packet. */
int usb_send_pad_report(int fd, struct usb_fs_endpoint *ep,
                        const VdPadReport *report);

/* Touch OUT path: serialize `t` with the shared 16-byte touch wire format
 * (vd_touch_serialize, the single source of truth for the Vita-side layout)
 * and send it as FOUR no-data control OUTs (bmRequestType 0x40,
 * bRequest 0x58..0x5B = chunk index, 4 bytes per request in wValue/wIndex,
 * wLength 0) — same SETUP-only chunk scheme as usb_send_pad_report. Chunk 0
 * (0x58) resets Vita-side reassembly. */
int usb_send_touch_report(int fd, const VdTouchReport *t);

/* Open (register + activate) a ugen FS endpoint slot: USB_IFACE_DRIVER_DETACH ->
 * USB_FS_INIT(eps array) -> USB_FS_OPEN(ep_index -> ep_no). Opening the endpoint
 * selects the alternate setting that owns it (Vita VideoStreaming bulk IN 0x81
 * is alt 1). Must run before any usb_bulk_read/write. `eps` is a persistent
 * usb_fs_endpoint[ep_count]; `ep_index` is the slot for `ep_no`. Returns 0 or
 * -errno. Optional *maxpkt_out receives the endpoint max packet length. */
int usb_transfer_open_endpoint(int fd, struct usb_fs_endpoint *eps,
                               unsigned ep_count, uint8_t ep_index,
                               uint8_t ep_no, int iface_ordinal,
                               uint32_t max_bufsize, uint32_t *maxpkt_out);
/* Close a slot and tear down the FS endpoint session (USB_FS_CLOSE + UNINIT). */
void usb_transfer_close_endpoint(int fd, struct usb_fs_endpoint *eps,
                                 unsigned ep_count, uint8_t ep_index);

/* Free a quarantined helper-owned transfer buffer. Call ONLY after
 * USB_FS_UNINIT (or an equivalent synchronous teardown such as closing the
 * ugen fd) has succeeded — never while a kernel transfer could still address
 * the buffer. */
void usb_transfer_release_after_uninit(struct usb_fs_endpoint *ep);
