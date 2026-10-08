/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — UAC isochronous audio capture session (PS5 root payload).
 *
 * Reads the VitaUSBStream UAC audio stream: the isochronous IN endpoint under
 * the AudioStreaming interface's active alternate setting (interface 3 alt 1
 * on the confirmed device; alt 0 is zero-bandwidth). Format: 48 kHz stereo
 * 16-bit interleaved PCM ('Line (Vita USB Stream)').
 *
 * ugen isoc flow (mirrors the bulk pattern in usb_transfer.c / uvc_stream.c):
 *   USB_IFACE_DRIVER_DETACH -> select alt 1 -> USB_FS_INIT(1 slot) ->
 *   USB_FS_OPEN(isoc IN, max_frames = packets per transfer) -> per read:
 *   one multi-frame transfer (ppBuffer[i]/pLength[i] per USB frame) ->
 *   USB_FS_START -> USB_FS_COMPLETE wait -> reassemble packets into PCM.
 * Teardown: USB_FS_STOP, USB_FS_CLOSE + USB_FS_UNINIT, alt back to 0.
 *
 * ORDERING IS LOAD-BEARING: USB_SET_ALTINTERFACE (ugen_set_interface()) calls
 * ugen_fs_uninit() and destroys the fd's usbfs session, so selecting the alt
 * after USB_FS_INIT/OPEN makes USB_FS_START/USB_FS_STOP fail with EINVAL (the
 * historical "isoc IN START fail errno=22"). The alt is therefore selected
 * BEFORE USB_FS_INIT and never again while the session is live.
 *
 * All functions return 0 on success or a negative errno value (open/start
 * and read), or -1 (session_open, matching vd_uvc_session_open).
 */
#pragma once
#include <stdint.h>
#include <dev/usb/usb_ioctl.h>

#include "uac_audio_state.h"
#include "usb_vita.h"

/* Endpoint slot inside this session's USB_FS_INIT array (a dedicated audio
 * session has exactly one: the isoc IN endpoint). */
#define VD_UAC_EP_INDEX_ISOC_IN 0u

/* 32 USB frames per transfer. HS isoc frames are 1 ms, so one transfer covers
 * ~32 ms of audio (32 * 192 = 6144 PCM bytes at 48 kHz stereo int16). */
#define VD_UAC_DEFAULT_FRAMES_PER_XFER 32u
#define VD_UAC_MAX_FRAMES 256u

typedef struct
{
    int fd;               /* open ugen fd for the Vita device */
    uint8_t as_interface; /* AudioStreaming bInterfaceNumber (3) */
    uint8_t as_alt;       /* alt owning the isoc endpoint (1) */
    uint8_t isoc_in_ep;   /* isoc IN bEndpointAddress, e.g. 0x82 */
    uint16_t maxpkt;      /* wMaxPacketSize, kernel-filled at FS_OPEN */
    uint32_t frames_per_xfer;
    enum VdUacFrameLayout layout; /* in-buffer packet placement */
    int streaming;
    int ep_opened;
    int dead;       /* an un-reaped transfer exists: fd close only */
    int last_errno; /* errno from the alt-select attempt (0 = ok) */
    /* usb_fs endpoint slot, persistent across reads (the in-flight guard and
     * the post-UNINIT release need it to stay alive). */
    struct usb_fs_endpoint eps[1];
    /* One-buffer isoc framing: frame_ptrs[i] -> buffer + i*maxpkt (frames_per_xfer
     * * maxpkt bytes total), lengths holds the per-frame byte counts. The kernel
     * reads ppBuffer[0..nFrames-1], so every slot must point into the buffer. */
    void *frame_ptrs[VD_UAC_MAX_FRAMES];
    uint32_t *lengths;
    uint8_t *buffer;
    uint32_t buffer_cap;
} VdUacSession;

/* Open the device node and copy the audio isoc endpoint parameters from the
 * already-parsed device (needs dev->audio_isoc_ep != 0). Returns 0 or -1. */
int vd_uac_session_open(VdUacSession *s, const VdUsbVitaDevice *dev);

/* Open the isoc endpoint (USB_FS_INIT + USB_FS_OPEN) and select the active
 * alternate setting. `frames_per_xfer` is the packets-per-transfer framing
 * (0 = VD_UAC_DEFAULT_FRAMES_PER_XFER). On success the session is ready for
 * vd_uac_read() calls. Returns 0 or -errno. */
int vd_uac_start(VdUacSession *s, uint32_t frames_per_xfer);

/* Run one multi-frame isoc IN transfer and reassemble its packets.
 *   pcm/pcm_cap           destination for tightly-packed interleaved PCM
 *   *pcm_len              PCM bytes written
 *   packet_lengths/cap    optional: the per-packet actual lengths (USB frames)
 *   *packet_count         optional: packets delivered in this transfer
 * Returns 0 or -errno (-ETIMEDOUT when the transfer never completes). */
int vd_uac_read(VdUacSession *s, uint8_t *pcm, uint32_t pcm_cap, uint32_t *pcm_len,
                uint32_t *packet_lengths, uint32_t packet_cap, uint32_t *packet_count);

/* Stop the stream: cancel any in-flight transfer, select alt 0 (the Vita
 * stops sending isoc data), and close/uninit the FS endpoint session. */
void vd_uac_stop(VdUacSession *s);

/* Full teardown including the ugen fd. Safe to call at any point. */
void vd_uac_session_close(VdUacSession *s);
