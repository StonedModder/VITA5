/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — host-side unit test for the pure parsing/serialization logic.
 *
 * These run on the dev machine (no PS5, no USB): UVC VS descriptor parsing,
 * NV12 frame-size math, and pad-report wire round-trip. The ugen/ioctl paths
 * are NOT covered here — those need hardware. Build: see tests/Makefile.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "uvc_protocol.h"
#include "pad_passthrough.h"

/* Build a minimal VideoStreaming descriptor blob matching vita-udcd-uvc's
 * NV12 format with one 960x544 frame, two intervals (60fps, 30fps). */
static unsigned build_vs_blob(uint8_t *out)
{
    unsigned pos = 0;

    /* Input Header (UVC_VS_INPUT_HEADER), bLength=14, 1 format, ep 0x81 */
    uint8_t hdr[14] = {14,
                       USB_DT_CS_INTERFACE,
                       UVC_VS_INPUT_HEADER,
                       1, /* bNumFormats */
                       0x4B,
                       0x00, /* wTotalLength (filled loosely, not validated) */
                       0x81, /* bEndpointAddress */
                       0,    /* bmInfo */
                       2,    /* bTerminalLink */
                       0,    /* bStillCaptureMethod */
                       0,
                       0, /* bTriggerSupport, bTriggerUsage */
                       1, /* bControlSize */
                       /* bmaControls[1][1] */ 0};
    memcpy(out + pos, hdr, sizeof(hdr));
    pos += sizeof(hdr);

    /* Format Uncompressed (NV12), bLength=27
     * uvc_format_uncompressed packed: offset 5 = guidFormat[16],
     * offset 21 = bBitsPerPixel, offset 22 = bDefaultFrameIndex. */
    uint8_t fmt[27] = {0};
    fmt[0] = 27;
    fmt[1] = USB_DT_CS_INTERFACE;
    fmt[2] = UVC_VS_FORMAT_UNCOMPRESSED;
    fmt[3] = 1; /* bFormatIndex */
    fmt[4] = 1; /* bNumFrameDescriptors */
    static const uint8_t nv12[16] = VD_UVC_GUID_NV12;
    memcpy(fmt + 5, nv12, 16);
    fmt[21] = 12; /* bBitsPerPixel */
    fmt[22] = 1;  /* bDefaultFrameIndex */
    memcpy(out + pos, fmt, sizeof(fmt));
    pos += sizeof(fmt);

    /* Frame Uncompressed (n=2 intervals), bLength = 26 + 4*2 = 34 */
    uint8_t frame[34] = {0};
    frame[0] = 34;
    frame[1] = USB_DT_CS_INTERFACE;
    frame[2] = UVC_VS_FRAME_UNCOMPRESSED;
    frame[3] = 1; /* bFrameIndex */
    frame[4] = 0; /* bmCapabilities */
    frame[5] = 0xC0;
    frame[6] = 0x03; /* wWidth = 960 */
    frame[7] = 0x20;
    frame[8] = 0x02; /* wHeight = 544 */
    uint32_t frame_size = VD_NV12_FRAME_SIZE(960, 544);
    frame[17] = (uint8_t)(frame_size); /* dwMaxVideoFrameBufferSize */
    frame[18] = (uint8_t)(frame_size >> 8);
    frame[19] = (uint8_t)(frame_size >> 16);
    frame[20] = (uint8_t)(frame_size >> 24);
    /* dwDefaultFrameInterval = 166666 (60fps) */
    frame[21] = 0x0A;
    frame[22] = 0x8B;
    frame[23] = 0x02;
    frame[24] = 0x00;
    frame[25] = 2; /* bFrameIntervalType = 2 */
    /* interval[0] = 166666 (60fps) */
    frame[26] = 0x0A;
    frame[27] = 0x8B;
    frame[28] = 0x02;
    frame[29] = 0x00;
    /* interval[1] = 333333 (30fps) */
    frame[30] = 0x15;
    frame[31] = 0x16;
    frame[32] = 0x05;
    frame[33] = 0x00;
    memcpy(out + pos, frame, sizeof(frame));
    pos += sizeof(frame);

    return pos;
}

static void test_vs_parse(void)
{
    uint8_t blob[256];
    unsigned len = build_vs_blob(blob);

    VdUvcStreaming s;
    int rc = vd_uvc_parse_streaming(blob, len, 1, &s);
    assert(rc == 0);
    assert(s.format_count == 1);
    assert(s.endpoint_address == 0x81);
    assert(s.formats[0].is_nv12 == 1);
    assert(s.formats[0].bits_per_pixel == 12);
    assert(s.formats[0].frame_count == 1);

    const VdUvcFrame *f = &s.formats[0].frames[0];
    assert(f->width == 960);
    assert(f->height == 544);
    assert(f->frame_index == 1);
    assert(f->max_frame_size == VD_NV12_FRAME_SIZE(960, 544));
    assert(f->interval_count == 2);
    printf("  VS parse OK: %ux%u nv12, frame_size=%u, intervals=%u\n", f->width, f->height,
           f->max_frame_size, f->interval_count);
}

static void test_nv12_math(void)
{
    assert(VD_NV12_FRAME_SIZE(960, 544) == 783360);
    assert(VD_NV12_FRAME_SIZE(1280, 720) == 1382400);
    printf("  NV12 math OK\n");
}

static void test_probe(void)
{
    VdUvcStreamingControl c;
    vd_uvc_default_probe(&c, 1, 1, 166666, VD_NV12_FRAME_SIZE(960, 544),
                         VD_NV12_FRAME_SIZE(960, 544) + UVC_PAYLOAD_HEADER_SIZE);
    assert(c.bFormatIndex == 1);
    assert(c.bFrameIndex == 1);
    assert(c.dwFrameInterval == 166666);
    /* UVC 1.1 Video Probe and Commit Control is 34 bytes (packed), matching
     * vita-udcd-uvc's uvc_streaming_control exactly. UVC 1.0 was 26 bytes. */
    assert(sizeof(VdUvcStreamingControl) == 34);
    printf("  probe struct OK (34 bytes)\n");
}

static void test_pad_roundtrip(void)
{
    VdPadReport in, out;
    vd_pad_make_report(&in, 42, 123456789ull, VD_PAD_CROSS | VD_PAD_UP, -32768, 32767, 100, -100,
                       128, 255);
    uint8_t wire[64];
    size_t n = vd_pad_serialize(&in, wire, sizeof(wire));
    assert(n == 28);
    int rc = vd_pad_deserialize(wire, n, &out);
    assert(rc == 0);
    assert(out.report_id == 42);
    assert(out.timestamp_us == 123456789ull);
    assert(out.buttons == (VD_PAD_CROSS | VD_PAD_UP));
    assert(out.left_x == -32768);
    assert(out.left_y == 32767);
    assert(out.right_x == 100);
    assert(out.right_y == -100);
    assert(out.l2 == 128);
    assert(out.r2 == 255);
    printf("  pad round-trip OK (28-byte wire)\n");
}

int main(void)
{
    printf("VITA5 host tests:\n");
    test_nv12_math();
    test_vs_parse();
    test_probe();
    test_pad_roundtrip();
    printf("ALL PASS\n");
    return 0;
}
