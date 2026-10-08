/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — host-side unit tests for the pure UAC isochronous audio logic.
 *
 * These run on the dev machine (no PS5, no USB): PCM interleave math, the
 * isoc framing layout (one buffer + per-frame length array), packet
 * reassembly under both in-buffer layout assumptions, and multi-frame isoc
 * completion validation. The ugen/ioctl isoc transfer paths in
 * core/src/uac_audio.c are NOT covered here — those need PS5 hardware.
 * Build: tools/build-core-tests.sh (or the test-core make target).
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uac_audio_state.h"

static void test_pcm_math(void)
{
    /* One interleaved sample frame: stereo int16 = 4 bytes. */
    assert(vd_uac_frame_bytes(2, 16) == 4);
    assert(vd_uac_frame_bytes(1, 16) == 2);
    assert(vd_uac_frame_bytes(2, 24) == 6);

    /* 48 kHz stereo int16: 192 bytes per 1 ms USB frame, 24 per 125 us
     * micro-frame (HS isoc). This is the confirmed device format. */
    assert(vd_uac_packet_bytes(48000, 2, 16, 1000) == 192);
    assert(vd_uac_packet_bytes(48000, 2, 16, 125) == 24);

    /* Interleaved little-endian int16 accessor: frame f, channel c lives at
     * (f * channels + c) * 2. */
    const uint8_t pcm[8] = {0x34, 0x12, 0x00, 0x80, 0xff, 0x7f, 0xd0, 0xfd};
    assert(vd_uac_pcm_sample(pcm, 0, 0, 2) == 0x1234);
    assert(vd_uac_pcm_sample(pcm, 0, 1, 2) == (int16_t)0x8000);
    assert(vd_uac_pcm_sample(pcm, 1, 0, 2) == 0x7fff);
    assert(vd_uac_pcm_sample(pcm, 1, 1, 2) == (int16_t)0xfdd0);
    printf("  PCM interleave math OK\n");
}

static void test_framing_layout(void)
{
    /* The isoc framing contract (usb_ioctl.h: one buffer, many frame
     * lengths): every USB frame is REQUESTED at maxpkt bytes, so the
     * strided-by-maxpkt and packed-by-requested buffer layouts coincide. */
    uint32_t lengths[8];
    unsigned i;
    memset(lengths, 0xAA, sizeof(lengths));
    vd_uac_init_frame_lengths(lengths, 8, 192);
    for (i = 0; i < 8; ++i)
        assert(lengths[i] == 192);
    /* Shorter framings only touch their own prefix. */
    vd_uac_init_frame_lengths(lengths, 3, 200);
    assert(lengths[0] == 200 && lengths[2] == 200 && lengths[3] == 192);
    printf("  isoc framing layout OK\n");
}

/* Build one completed isoc transfer: 4 USB frames, maxpkt 8, packet lengths
 * [8, 6, 0, 4]. Packet payloads are 0xN0 + i for packet N. The strided
 * layout places packet i at i * maxpkt; the packed layout at the cumulative
 * sum of earlier actual lengths. Both must reassemble to the same PCM. */
static unsigned build_strided_buffer(uint8_t *buffer)
{
    unsigned i;
    memset(buffer, 0xEE, 32);
    for (i = 0; i < 8; ++i)
        buffer[i] = (uint8_t)(0x10 + i);
    for (i = 0; i < 6; ++i)
        buffer[8 + i] = (uint8_t)(0x20 + i);
    buffer[16] = 0xEE; /* frame 2 delivers nothing */
    for (i = 0; i < 4; ++i)
        buffer[24 + i] = (uint8_t)(0x40 + i);
    return 32;
}

static unsigned build_packed_buffer(uint8_t *buffer)
{
    unsigned i;
    memset(buffer, 0xEE, 32);
    for (i = 0; i < 8; ++i)
        buffer[i] = (uint8_t)(0x10 + i);
    for (i = 0; i < 6; ++i)
        buffer[8 + i] = (uint8_t)(0x20 + i);
    for (i = 0; i < 4; ++i)
        buffer[14 + i] = (uint8_t)(0x40 + i);
    return 32;
}

static void expect_pcm(const uint8_t *out, uint32_t out_len)
{
    static const uint8_t want[18] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x20,
                                     0x21, 0x22, 0x23, 0x24, 0x25, 0x40, 0x41, 0x42, 0x43};
    assert(out_len == sizeof(want));
    assert(memcmp(out, want, sizeof(want)) == 0);
}

static void test_reassemble_strided(void)
{
    uint8_t buffer[32];
    uint8_t out[64];
    const uint32_t lengths[4] = {8, 6, 0, 4};
    uint32_t out_len = 99, out_frames = 99;

    build_strided_buffer(buffer);
    memset(out, 0, sizeof(out));
    assert(vd_uac_reassemble(buffer, sizeof(buffer), lengths, 4, 8, VD_UAC_LAYOUT_STRIDED_MAXPKT,
                             out, sizeof(out), &out_len, &out_frames) == 0);
    expect_pcm(out, out_len);
    /* Zero-length frame 2 is skipped, not counted. */
    assert(out_frames == 3);
    printf("  reassemble (strided maxpkt) OK\n");
}

static void test_reassemble_packed(void)
{
    uint8_t buffer[32];
    uint8_t out[64];
    const uint32_t lengths[4] = {8, 6, 0, 4};
    uint32_t out_len = 99, out_frames = 99;

    build_packed_buffer(buffer);
    memset(out, 0, sizeof(out));
    assert(vd_uac_reassemble(buffer, sizeof(buffer), lengths, 4, 8, VD_UAC_LAYOUT_PACKED_ACTUAL,
                             out, sizeof(out), &out_len, &out_frames) == 0);
    expect_pcm(out, out_len);
    assert(out_frames == 3);
    printf("  reassemble (packed actual) OK\n");
}

static void test_reassemble_errors(void)
{
    uint8_t buffer[32];
    uint8_t out[64];
    uint32_t out_len = 99, out_frames = 99;

    build_strided_buffer(buffer);

    /* A packet can never exceed the endpoint max packet size. */
    {
        const uint32_t lengths[4] = {8, 9, 0, 4};
        assert(vd_uac_reassemble(buffer, sizeof(buffer), lengths, 4, 8,
                                 VD_UAC_LAYOUT_STRIDED_MAXPKT, out, sizeof(out), &out_len,
                                 &out_frames) == -EINVAL);
        assert(out_len == 0);
    }
    /* maxpkt == 0 cannot describe any packet. */
    {
        const uint32_t lengths[1] = {4};
        assert(vd_uac_reassemble(buffer, sizeof(buffer), lengths, 1, 0,
                                 VD_UAC_LAYOUT_STRIDED_MAXPKT, out, sizeof(out), &out_len,
                                 &out_frames) == -EINVAL);
    }
    /* Source range must stay inside the transfer buffer (strided: packet 3
     * at 3 * 8 = 24 needs bytes 24..27; a 27-byte buffer is one short). */
    {
        const uint32_t lengths[4] = {8, 6, 0, 4};
        assert(vd_uac_reassemble(buffer, 27, lengths, 4, 8, VD_UAC_LAYOUT_STRIDED_MAXPKT, out,
                                 sizeof(out), &out_len, &out_frames) == -EINVAL);
    }
    /* Destination overflow is rejected up front and writes nothing. */
    {
        const uint32_t lengths[4] = {8, 6, 0, 4};
        memset(out, 0x55, sizeof(out));
        assert(vd_uac_reassemble(buffer, sizeof(buffer), lengths, 4, 8,
                                 VD_UAC_LAYOUT_STRIDED_MAXPKT, out, 17, &out_len,
                                 &out_frames) == -ENOSPC);
        assert(out_len == 0 && out_frames == 0);
        assert(out[0] == 0x55);
    }
    /* Null arguments. */
    {
        const uint32_t lengths[4] = {8, 6, 0, 4};
        assert(vd_uac_reassemble(NULL, 32, lengths, 4, 8, VD_UAC_LAYOUT_STRIDED_MAXPKT, out,
                                 sizeof(out), &out_len, &out_frames) == -EINVAL);
        assert(vd_uac_reassemble(buffer, sizeof(buffer), NULL, 4, 8, VD_UAC_LAYOUT_STRIDED_MAXPKT,
                                 out, sizeof(out), &out_len, &out_frames) == -EINVAL);
        assert(vd_uac_reassemble(buffer, sizeof(buffer), lengths, 4, 8,
                                 VD_UAC_LAYOUT_STRIDED_MAXPKT, NULL, sizeof(out), &out_len,
                                 &out_frames) == -EINVAL);
        assert(vd_uac_reassemble(buffer, sizeof(buffer), lengths, 4, 8,
                                 VD_UAC_LAYOUT_STRIDED_MAXPKT, out, sizeof(out), NULL,
                                 &out_frames) == -EINVAL);
    }
    /* Zero frames is a legal empty reassembly (the completion validator is
     * what rejects an empty transfer). */
    {
        out_len = 99;
        assert(vd_uac_reassemble(buffer, sizeof(buffer), NULL, 0, 8, VD_UAC_LAYOUT_STRIDED_MAXPKT,
                                 out, sizeof(out), &out_len, &out_frames) == 0);
        assert(out_len == 0 && out_frames == 0);
    }
    printf("  reassemble error paths OK\n");
}

static void test_completion(void)
{
    /* Record ownership first (USB_FS_COMPLETE is a session-wide FIFO). */
    assert(vd_uac_check_completion(1, 1, 0, 32, 32) == USB_OUT_COMPLETION_OK);
    assert(vd_uac_check_completion(0, 1, 0, 32, 32) == USB_OUT_COMPLETION_OTHER);
    assert(vd_uac_check_completion(2, 1, USB_STATUS_TIMEOUT, 0, 32) == USB_OUT_COMPLETION_OTHER);

    /* Status errors outrank frame checks. */
    assert(vd_uac_check_completion(1, 1, USB_STATUS_CANCELLED, 32, 32) ==
           USB_OUT_COMPLETION_STATUS_ERROR);
    assert(vd_uac_check_completion(1, 1, USB_STATUS_TIMEOUT, 0, 32) ==
           USB_OUT_COMPLETION_STATUS_ERROR);

    /* Isoc transfers may legally complete with FEWER frames than queued —
     * partial completion is OK, empty or over-full is not. */
    assert(vd_uac_check_completion(1, 1, 0, 17, 32) == USB_OUT_COMPLETION_OK);
    assert(vd_uac_check_completion(1, 1, 0, 0, 32) == USB_OUT_COMPLETION_FRAME_ERROR);
    assert(vd_uac_check_completion(1, 1, 0, 33, 32) == USB_OUT_COMPLETION_FRAME_ERROR);
    printf("  isoc completion validation OK\n");
}

int main(void)
{
    printf("VITA5 UAC isoc host tests:\n");
    test_pcm_math();
    test_framing_layout();
    test_reassemble_strided();
    test_reassemble_packed();
    test_reassemble_errors();
    test_completion();
    printf("ALL PASS\n");
    return 0;
}
