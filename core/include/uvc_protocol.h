/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — USB Video Class / USB Audio Class protocol definitions.
 *
 * Reference: xerpi/vita-udcd-uvc (Vita-side UVC device) and the UVC 1.1 spec.
 * The PS Vita running vita-udcd-uvc / VitaUSBStream enumerates as:
 *   Device class 0xEF (Miscellaneous), subclass 0x02, protocol 0x01 (IAD)
 *   VideoControl iface 0 (class 0x0E, subclass 0x01)
 *   VideoStreaming  iface 1 (class 0x0E, subclass 0x02), bulk IN 0x81
 *   NV12 uncompressed, 12 bpp
 *
 * This is the HOST side: we parse descriptors, send probe/commit, and read
 * bulk frames. We do NOT generate descriptors (that is the Vita's job).
 */
#pragma once
#include <stdint.h>

/* ---- USB base ------------------------------------------------------ */
#define USB_CLASS_VIDEO                  0x0E
#define USB_CLASS_AUDIO                  0x01
#define USB_DEVICE_CLASS_MISCELLANEOUS   0xEF
#define USB_SUBCLASS_COMMON              0x02
#define USB_PROTOCOL_IAD                 0x01

#define USB_DT_DEVICE                    0x01
#define USB_DT_CONFIG                    0x02
#define USB_DT_STRING                    0x03
#define USB_DT_INTERFACE                 0x04
#define USB_DT_ENDPOINT                  0x05
#define USB_DT_CS_INTERFACE              0x24
#define USB_DT_CS_ENDPOINT               0x25
#define USB_DT_IAD                       0x0B

/* ---- UVC VideoControl / VideoStreaming subclass --------------------- */
#define UVC_SC_VIDEOCONTROL              0x01
#define UVC_SC_VIDEOSTREAMING            0x02

/* VS interface descriptor subtypes */
#define UVC_VS_INPUT_HEADER              0x01
#define UVC_VS_FORMAT_UNCOMPRESSED       0x04
#define UVC_VS_FRAME_UNCOMPRESSED        0x05
#define UVC_VS_FORMAT_MJPEG              0x06
#define UVC_VS_FRAME_MJPEG               0x07
#define UVC_VS_COLORFORMAT               0x0D

/* ---- UVC request codes -------------------------------------------- */
#define UVC_SET_CUR                      0x01
#define UVC_GET_CUR                      0x81
#define UVC_GET_MIN                      0x82
#define UVC_GET_MAX                      0x83
#define UVC_GET_RES                      0x84
#define UVC_GET_LEN                      0x85
#define UVC_GET_INFO                     0x86
#define UVC_GET_DEF                      0x87

/* VS control selectors */
#define UVC_VS_PROBE_CONTROL             0x01
#define UVC_VS_COMMIT_CONTROL            0x02
#define UVC_VS_STILL_PROBE_CONTROL       0x03
#define UVC_VS_STILL_COMMIT_CONTROL      0x04

/* ---- UVC payload header ------------------------------------------- */
#define UVC_PAYLOAD_HEADER_SIZE          12
#define UVC_STREAM_EOH                   (1u << 7)
#define UVC_STREAM_ERR                   (1u << 6)
#define UVC_STREAM_STI                   (1u << 5)
#define UVC_STREAM_RES                   (1u << 4)
#define UVC_STREAM_SCR                   (1u << 3)
#define UVC_STREAM_PTS                   (1u << 2)
#define UVC_STREAM_EOF                   (1u << 1)
#define UVC_STREAM_FID                   (1u << 0)

/* NV12 frame helper: 12 bits per pixel packed as w*h*3/2 */
#define VD_NV12_FRAME_SIZE(w, h)         ((unsigned)(w) * (unsigned)(h) * 3u / 2u)

/* Vita-udcd-uvc NV12 GUID */
#define VD_UVC_GUID_NV12 \
    { 'N','V','1','2',0x00,0x00,0x10,0x00,0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71 }

/* ---- Video Probe and Commit control (36 bytes, UVC 1.1) ----------- */
typedef struct __attribute__((packed)) {
    uint16_t bmHint;
    uint8_t  bFormatIndex;
    uint8_t  bFrameIndex;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate;
    uint16_t wPFrameRate;
    uint16_t wCompQuality;
    uint16_t wCompWindowSize;
    uint16_t wDelay;
    uint32_t dwMaxVideoFrameSize;
    uint32_t dwMaxPayloadTransferSize;
    uint32_t dwClockFrequency;
    uint8_t  bmFramingInfo;
    uint8_t  bPreferedVersion;
    uint8_t  bMinVersion;
    uint8_t  bMaxVersion;
} VdUvcStreamingControl;

/* ---- Parsed VS frame descriptor (subset we care about) ------------- */
typedef struct {
    uint8_t  frame_index;
    uint16_t width;
    uint16_t height;
    uint32_t max_frame_size;
    uint32_t default_interval;
    uint8_t  interval_type;
    uint32_t intervals[8];
    uint8_t  interval_count;
} VdUvcFrame;

#define VD_UVC_MAX_FRAMES 8

/* ---- Parsed VS format descriptor ---------------------------------- */
typedef struct {
    uint8_t format_index;
    uint8_t format_subtype;   /* UVC_VS_FORMAT_UNCOMPRESSED (0x04) or _MJPEG (0x06) */
    uint8_t bits_per_pixel;
    uint8_t guid[16];
    uint8_t frame_count;
    VdUvcFrame frames[VD_UVC_MAX_FRAMES];
    int      is_nv12;
} VdUvcFormat;

#define VD_UVC_MAX_FORMATS 4

/* ---- Fully parsed UVC streaming interface ------------------------- */
typedef struct {
    uint8_t interface_number;
    uint8_t endpoint_address;   /* bulk IN, e.g. 0x81 */
    uint8_t format_count;
    VdUvcFormat formats[VD_UVC_MAX_FORMATS];
} VdUvcStreaming;

/* Default probe: 960x544 @ 60 fps NV12 (Vita frame index 1). */
void vd_uvc_default_probe(VdUvcStreamingControl *ctrl,
                          uint8_t format_index, uint8_t frame_index,
                          uint32_t frame_interval,
                          uint32_t max_frame_size, uint32_t max_payload);

/* Parse a VS interface's class-specific descriptors into VdUvcStreaming.
 * `data`/`len` is the raw descriptor blob for the VideoStreaming interface
 * (starting at its Input Header). Returns 0 on success. */
int vd_uvc_parse_streaming(const uint8_t *data, unsigned len,
                           uint8_t interface_number,
                           VdUvcStreaming *out);
