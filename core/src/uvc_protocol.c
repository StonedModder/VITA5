/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — UVC streaming descriptor parsing and probe/commit helpers.
 */
#include "uvc_protocol.h"
#include <string.h>

void vd_uvc_default_probe(VdUvcStreamingControl *ctrl,
                          uint8_t format_index, uint8_t frame_index,
                          uint32_t frame_interval,
                          uint32_t max_frame_size, uint32_t max_payload)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->bmHint = 0;
    ctrl->bFormatIndex = format_index;
    ctrl->bFrameIndex = frame_index;
    ctrl->dwFrameInterval = frame_interval;
    ctrl->dwMaxVideoFrameSize = max_frame_size;
    ctrl->dwMaxPayloadTransferSize = max_payload;
    ctrl->bPreferedVersion = 1;
}

/* Walk the class-specific descriptors that follow a VideoStreaming interface.
 * The blob begins at the VS Input Header (0x24 / subtype 0x01). */
int vd_uvc_parse_streaming(const uint8_t *data, unsigned len,
                           uint8_t interface_number,
                           VdUvcStreaming *out)
{
    if (!data || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->interface_number = interface_number;
    out->endpoint_address = 0;

    unsigned pos = 0;
    VdUvcFormat *cur = NULL;

    while (pos + 3 <= len) {
        uint8_t blen = data[pos];
        uint8_t type = data[pos + 1];
        uint8_t sub  = data[pos + 2];
        if (blen < 3 || pos + blen > len) return -1;

        if (type == USB_DT_CS_INTERFACE) {
            switch (sub) {
            case UVC_VS_INPUT_HEADER:
                if (blen >= 9) {
                    out->endpoint_address = data[pos + 6];
                }
                break;
            case UVC_VS_FORMAT_UNCOMPRESSED:
            case UVC_VS_FORMAT_MJPEG:
                if (out->format_count >= VD_UVC_MAX_FORMATS) break;
                cur = &out->formats[out->format_count++];
                memset(cur, 0, sizeof(*cur));
                if (blen >= 8) {
                    cur->format_index = data[pos + 3];
                    cur->is_nv12 = (sub == UVC_VS_FORMAT_UNCOMPRESSED);
                }
                cur->format_subtype = sub;
                if (sub == UVC_VS_FORMAT_UNCOMPRESSED && blen >= 27) {
                    /* uvc_format_uncompressed packed layout:
                     * offset 5 = guidFormat[16], offset 21 = bBitsPerPixel. */
                    memcpy(cur->guid, data + pos + 5, 16);
                    cur->bits_per_pixel = data[pos + 21];
                    /* NV12 GUID check */
                    static const uint8_t nv12[16] = VD_UVC_GUID_NV12;
                    cur->is_nv12 = (memcmp(cur->guid, nv12, 16) == 0);
                } else if (sub == UVC_VS_FORMAT_MJPEG && blen >= 11) {
                    cur->bits_per_pixel = 0;
                }
                break;
            case UVC_VS_FRAME_UNCOMPRESSED:
            case UVC_VS_FRAME_MJPEG:
                if (cur && cur->frame_count < VD_UVC_MAX_FRAMES && blen >= 26) {
                    VdUvcFrame *f = &cur->frames[cur->frame_count++];
                    f->frame_index = data[pos + 3];
                    f->width  = (uint16_t)(data[pos + 5] | (data[pos + 6] << 8));
                    f->height = (uint16_t)(data[pos + 7] | (data[pos + 8] << 8));
                    /* Packed uvc_frame_uncompressed offsets:
                     * 17 = dwMaxVideoFrameBufferSize, 21 = dwDefaultFrameInterval,
                     * 25 = bFrameIntervalType, 26 = dwFrameInterval[]. */
                    f->max_frame_size =
                        (uint32_t)data[pos + 17] |
                        ((uint32_t)data[pos + 18] << 8) |
                        ((uint32_t)data[pos + 19] << 16) |
                        ((uint32_t)data[pos + 20] << 24);
                    f->default_interval =
                        (uint32_t)data[pos + 21] |
                        ((uint32_t)data[pos + 22] << 8) |
                        ((uint32_t)data[pos + 23] << 16) |
                        ((uint32_t)data[pos + 24] << 24);
                    f->interval_type = data[pos + 25];
                    unsigned n = 0;
                    if (f->interval_type == 0) {
                        /* continuous range: min/max — store min as default */
                        n = 1;
                        f->intervals[0] = f->default_interval;
                    } else {
                        unsigned max = f->interval_type;
                        if (max > 8) max = 8;
                        for (unsigned i = 0; i < max && (pos + 26 + (i + 1) * 4) <= (unsigned)(pos + blen); ++i) {
                            f->intervals[i] =
                                (uint32_t)data[pos + 26 + i * 4] |
                                ((uint32_t)data[pos + 27 + i * 4] << 8) |
                                ((uint32_t)data[pos + 28 + i * 4] << 16) |
                                ((uint32_t)data[pos + 29 + i * 4] << 24);
                            n = i + 1;
                        }
                    }
                    f->interval_count = (uint8_t)n;
                }
                break;
            default:
                break;
            }
        }
        pos += blen;
    }
    return out->format_count > 0 ? 0 : -1;
}
