/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — pure UAC isochronous audio logic (host-testable).
 *
 * Shared by the PS5 payload implementation (core/src/uac_audio.c) and the
 * host unit tests (tests/test_uac_audio.c). This header deliberately has no
 * PS5/USB includes so the framing-layout and PCM reassembly rules run on the
 * dev host (WSL clang).
 *
 * Isochronous framing (dev/usb/usb_ioctl.h, struct usb_fs_endpoint):
 *   "isochronous USB transfer only use one buffer, but can have multiple
 *    frame lengths!"
 * That "one buffer" is the KERNEL-side DMA buffer: ugen_fs_copy_in()/
 * ugen_fs_copy_out() (sys/dev/usb/usb_generic.c) keep all frames in
 * frbuffers[0] at offsets equal to the cumulative REQUESTED frame lengths,
 * while the userland side is a PER-FRAME contract: ppBuffer[i] is the
 * destination of packet i, pLength[i] is its requested length (updated to the
 * actual length on completion), and aFrames is the number of completed
 * packets. On completion packet i is copied out to ppBuffer[i], so a session
 * that points ppBuffer[i] at buffer + i * maxpkt receives packets strided by
 * max packet size (VD_UAC_LAYOUT_STRIDED_MAXPKT — confirmed against the
 * ugen_fs_copy_out source). VD_UAC_LAYOUT_PACKED_ACTUAL (cumulative actual
 * lengths) is retained for hardware that packs differently; both layouts are
 * implemented and tested here.
 */
#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "usb_transfer_state.h"

/* Confirmed device format: 'Line (Vita USB Stream)' is 48 kHz stereo 16-bit
 * interleaved PCM on the PC side (VitaUSBStream mixes and resamples). */
#define VD_UAC_SAMPLE_RATE 48000u
#define VD_UAC_CHANNELS 2u
#define VD_UAC_BITS 16u

/* Where completed isoc packets sit inside the single transfer buffer. */
enum VdUacFrameLayout
{
    VD_UAC_LAYOUT_STRIDED_MAXPKT = 0, /* packet i at offset i * maxpkt */
    VD_UAC_LAYOUT_PACKED_ACTUAL = 1   /* packet i at sum of earlier actual lengths */
};

/* Size of one interleaved PCM sample frame (all channels, one sample tick). */
static inline uint32_t vd_uac_frame_bytes(uint32_t channels, uint32_t bits)
{
    return (channels * bits) / 8u;
}

/* Expected payload bytes per USB frame (one isoc packet) at `interval_us`.
 * 48 kHz stereo int16 delivers 192 bytes per 1 ms frame (24 per 125 us
 * micro-frame). Rounds down when the rate does not divide evenly. */
static inline uint32_t vd_uac_packet_bytes(uint32_t sample_rate, uint32_t channels, uint32_t bits,
                                           uint32_t interval_us)
{
    return (uint32_t)(((uint64_t)sample_rate * (bits / 8u) * channels * interval_us) / 1000000ull);
}

/* Initialize a transfer's per-frame length array. For isoc IN every frame is
 * REQUESTED at maxpkt bytes: the device may legally deliver less (a short
 * packet per frame is normal), and requesting maxpkt for every frame is what
 * makes the strided and packed-by-requested buffer layouts identical. */
static inline void vd_uac_init_frame_lengths(uint32_t *lengths, uint32_t frames, uint32_t maxpkt)
{
    uint32_t i;
    if (!lengths)
        return;
    for (i = 0; i < frames; ++i)
        lengths[i] = maxpkt;
}

/* Read one interleaved 16-bit little-endian PCM sample (host-testable). */
static inline int16_t vd_uac_pcm_sample(const uint8_t *pcm, uint32_t frame_index, uint32_t channel,
                                        uint32_t channels)
{
    uint32_t offset = (frame_index * channels + channel) * 2u;
    return (int16_t)(uint16_t)((uint16_t)pcm[offset] | (uint16_t)((uint16_t)pcm[offset + 1] << 8));
}

/* Reassemble the per-packet payloads of one completed isoc IN transfer into a
 * tightly-packed interleaved PCM buffer.
 *
 *   buffer/buffer_cap  the single isoc transfer buffer as filled by the kernel
 *   lengths/frames     pLength[0..frames-1] as updated on completion (actual
 *                      byte counts; 0 = no data in that USB frame)
 *   maxpkt             endpoint wMaxPacketSize: a packet can never exceed it
 *   layout             in-buffer packet placement (see header comment)
 *   out/out_cap        destination PCM buffer
 *   *out_len           total PCM bytes written (0 on error)
 *   *out_frames        packets with non-zero length copied (may be NULL)
 *
 * Validation is two-pass: on ANY error the destination is untouched and
 * *out_len is 0. Returns 0 or -errno:
 *   -EINVAL   bad argument, a packet longer than maxpkt, or a packet whose
 *             source range leaves the transfer buffer
 *   -ENOSPC   out_cap cannot hold the reassembled PCM
 */
static inline int vd_uac_reassemble(const uint8_t *buffer, uint32_t buffer_cap,
                                    const uint32_t *lengths, uint32_t frames, uint32_t maxpkt,
                                    enum VdUacFrameLayout layout, uint8_t *out, uint32_t out_cap,
                                    uint32_t *out_len, uint32_t *out_frames)
{
    uint32_t offset = 0;
    uint32_t total = 0;
    uint32_t used = 0;
    uint32_t i;

    if (out_len)
        *out_len = 0;
    if (out_frames)
        *out_frames = 0;
    if (!out_len || !out || (!buffer && frames != 0) || (!lengths && frames != 0))
        return -EINVAL;

    /* Pass 1: validate every packet and compute the total size. */
    for (i = 0; i < frames; ++i)
    {
        uint32_t length = lengths[i];
        uint32_t source;
        if (length == 0)
            continue; /* missing/zero-length packet: contributes nothing */
        if (maxpkt == 0 || length > maxpkt)
            return -EINVAL;
        source = (layout == VD_UAC_LAYOUT_PACKED_ACTUAL) ? offset : i * maxpkt;
        if (source > buffer_cap || length > buffer_cap - source)
            return -EINVAL;
        total += length;
        ++used;
        if (layout == VD_UAC_LAYOUT_PACKED_ACTUAL)
            offset += length;
    }
    if (total > out_cap)
        return -ENOSPC;

    /* Pass 2: copy (identical traversal, so the two passes agree). */
    offset = 0;
    total = 0;
    for (i = 0; i < frames; ++i)
    {
        uint32_t length = lengths[i];
        uint32_t source;
        if (length == 0)
            continue;
        source = (layout == VD_UAC_LAYOUT_PACKED_ACTUAL) ? offset : i * maxpkt;
        memcpy(out + total, buffer + source, length);
        total += length;
        if (layout == VD_UAC_LAYOUT_PACKED_ACTUAL)
            offset += length;
    }

    *out_len = total;
    if (out_frames)
        *out_frames = used;
    return 0;
}

/* Multi-frame isoc completion validation. Mirrors the session-FIFO rules of
 * usb_in_check_completion (record ownership first, then status), but frame
 * counting differs: an isoc transfer may legally complete with FEWER frames
 * than queued (aFrames <= nFrames, non-zero), so partial completion is OK and
 * only 0 or > nFrames frames is a frame error. Per-packet lengths are
 * validated later by vd_uac_reassemble. */
static inline enum UsbOutCompletionCheck vd_uac_check_completion(uint8_t completed_index,
                                                                 uint8_t expected_index, int status,
                                                                 uint32_t actual_frames,
                                                                 uint32_t queued_frames)
{
    if (completed_index != expected_index)
        return USB_OUT_COMPLETION_OTHER;
    if (status != 0)
        return USB_OUT_COMPLETION_STATUS_ERROR;
    if (actual_frames == 0 || actual_frames > queued_frames)
        return USB_OUT_COMPLETION_FRAME_ERROR;
    return USB_OUT_COMPLETION_OK;
}
