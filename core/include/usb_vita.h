/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — USB host enumeration + PS Vita UVC/UAC device detection.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "uvc_protocol.h"

#define VD_USB_MAX_ENDPOINTS 15

typedef struct {
    char path[32];
    uint16_t vid, pid;
    uint8_t device_class;
    int is_vita;

    /* Video */
    int has_video;                  /* a VS interface with formats parsed */
    int has_video_control;
    uint8_t video_control_interface;
    VdUvcStreaming uvc;             /* parsed VS interface (formats/frames) */
    uint8_t video_in_eps[VD_USB_MAX_ENDPOINTS];
    uint8_t video_out_eps[VD_USB_MAX_ENDPOINTS];
    unsigned video_in_count, video_out_count;

    /* Audio (VitaUSBStream only) */
    int has_audio;
    VdUvcStreaming uac;             /* reuse interface_number field */
    uint8_t audio_in_eps[VD_USB_MAX_ENDPOINTS];
    uint8_t audio_out_eps[VD_USB_MAX_ENDPOINTS];
    unsigned audio_in_count, audio_out_count;

    /* Active audio isoc stream (VitaUSBStream): the isochronous IN endpoint
     * lives under the AudioStreaming interface's non-zero alt setting (alt 1;
     * alt 0 is zero-bandwidth), so it is NOT in audio_in_eps above. */
    uint8_t audio_stream_interface; /* AudioStreaming (0x01/0x02) iface */
    uint8_t audio_isoc_alt;         /* alt setting owning the isoc endpoint */
    uint8_t audio_isoc_ep;          /* isoc IN bEndpointAddress, or 0 */
    uint16_t audio_isoc_maxpkt;     /* wMaxPacketSize of the isoc IN endpoint */

    /* Pad input (vita-side/input-receiver): vendor-specific interface
     * (class 0xFF, subclass 0x50 'P', protocol 0x01) with one bulk OUT
     * endpoint carrying 28-byte pad wire reports. Matched by class/subclass,
     * never by interface number (the plugin appends it after the gadget's own
     * interfaces, so the number depends on the stream plugin build). */
    int has_pad;
    uint8_t pad_interface;          /* vendor pad interface number */
    uint8_t pad_out_ep;             /* bulk OUT bEndpointAddress, or 0 */
} VdUsbVitaDevice;

/* Inspect a single ugen node. Returns 0 and fills `dev` when the node exposes
 * a VideoStreaming interface with a bulk IN endpoint (i.e. a Vita UVC device).
 * Descriptor reads only: no driver detach, no endpoint open, no USB writes. */
int vd_usb_inspect_vita(const char *path, VdUsbVitaDevice *dev);

/* Scan /dev/ugen*. Returns the number of Vita UVC devices found (up to capacity). */
size_t vd_usb_scan_vita(VdUsbVitaDevice *devices, size_t capacity);
