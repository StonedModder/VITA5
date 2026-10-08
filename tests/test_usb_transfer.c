/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — host-side unit tests for the pure USB transfer logic.
 *
 * These run on the dev machine (no PS5, no USB):
 *   1. Session-FIFO completion validation — the ported Ghostcontrol
 *      usb_out_check_completion semantics (record ownership, status, frame
 *      and length rules) plus the bulk IN variant (short reads legal).
 *   2. Pad wire round-trip against the EXACT 28-byte little-endian layout
 *      the Vita side mirrors, byte for byte.
 * The ugen/ioctl transfer paths in core/src/usb_transfer.c are NOT covered
 * here — those need PS5 hardware. Build: see tests/ or the command in
 * docs/TESTING.md.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "pad_passthrough.h"
#include "usb_transfer_state.h"

static void test_out_completion(void)
{
    /* Happy path: our record, success, 1 frame, exact length. */
    assert(usb_out_check_completion(1, 1, 0, 1, 28, 28) == USB_OUT_COMPLETION_OK);
    /* A zero-length OUT that completes at zero is valid. */
    assert(usb_out_check_completion(1, 1, 0, 1, 0, 0) == USB_OUT_COMPLETION_OK);

    /* Record ownership comes FIRST: USB_FS_COMPLETE is a session-wide FIFO,
     * so a record for another endpoint must be drained (OTHER), never treated
     * as our completion — even when its own fields look broken. */
    assert(usb_out_check_completion(0, 1, 0, 1, 28, 28) == USB_OUT_COMPLETION_OTHER);
    assert(usb_out_check_completion(2, 1, USB_STATUS_TIMEOUT, 0, 999, 28) ==
           USB_OUT_COMPLETION_OTHER);

    /* Status errors outrank frame/length checks. */
    assert(usb_out_check_completion(1, 1, USB_STATUS_TIMEOUT, 0, 0, 28) ==
           USB_OUT_COMPLETION_STATUS_ERROR);
    assert(usb_out_check_completion(1, 1, USB_STATUS_CANCELLED, 5, 3, 28) ==
           USB_OUT_COMPLETION_STATUS_ERROR);
    assert(usb_out_check_completion(1, 1, USB_STATUS_INTERRUPTED, 1, 28, 28) ==
           USB_OUT_COMPLETION_STATUS_ERROR);
    assert(usb_out_check_completion(1, 1, 7, 1, 28, 28) == USB_OUT_COMPLETION_STATUS_ERROR);

    /* Exactly one frame was queued, so any other completed frame count is a
     * validation failure. */
    assert(usb_out_check_completion(1, 1, 0, 0, 28, 28) == USB_OUT_COMPLETION_FRAME_ERROR);
    assert(usb_out_check_completion(1, 1, 0, 2, 28, 28) == USB_OUT_COMPLETION_FRAME_ERROR);

    /* OUT must complete with EXACTLY the requested length; short or overrun
     * writes are corrupt reports. */
    assert(usb_out_check_completion(1, 1, 0, 1, 27, 28) == USB_OUT_COMPLETION_LENGTH_ERROR);
    assert(usb_out_check_completion(1, 1, 0, 1, 29, 28) == USB_OUT_COMPLETION_LENGTH_ERROR);

    printf("  out completion validation OK\n");
}

static void test_in_completion(void)
{
    /* Full, short and zero-length reads are all legal bulk IN results (UVC
     * payloads end in short packets). */
    assert(usb_in_check_completion(0, 0, 0, 1, 32, 32) == USB_OUT_COMPLETION_OK);
    assert(usb_in_check_completion(0, 0, 0, 1, 12, 32) == USB_OUT_COMPLETION_OK);
    assert(usb_in_check_completion(0, 0, 0, 1, 0, 32) == USB_OUT_COMPLETION_OK);

    /* An overrun past the caller's buffer is a validation failure. */
    assert(usb_in_check_completion(0, 0, 0, 1, 33, 32) == USB_OUT_COMPLETION_LENGTH_ERROR);

    /* Same ownership/status/frame rules as OUT. */
    assert(usb_in_check_completion(1, 0, 0, 1, 32, 32) == USB_OUT_COMPLETION_OTHER);
    assert(usb_in_check_completion(0, 0, USB_STATUS_CANCELLED, 1, 32, 32) ==
           USB_OUT_COMPLETION_STATUS_ERROR);
    assert(usb_in_check_completion(0, 0, 0, 2, 32, 32) == USB_OUT_COMPLETION_FRAME_ERROR);

    printf("  in completion validation OK\n");
}

static void test_status_map(void)
{
    assert(usb_status_to_errno(USB_STATUS_TIMEOUT) == -ETIMEDOUT);
    assert(usb_status_to_errno(USB_STATUS_CANCELLED) == -ECANCELED);
    assert(usb_status_to_errno(USB_STATUS_INTERRUPTED) == -EINTR);
    assert(usb_status_to_errno(0) == -EIO);
    assert(usb_status_to_errno(USB_STATUS_TIMEOUT + 1) == -EIO);
    printf("  usb status -> errno map OK\n");
}

static void test_pad_wire_layout(void)
{
    VdPadReport in, out;
    uint8_t wire[VD_PAD_WIRE_BYTES];
    size_t n;

    /* The button bit map is part of the wire contract with the Vita side. */
    assert(VD_PAD_SELECT == 1u << 0);
    assert(VD_PAD_L3 == 1u << 1);
    assert(VD_PAD_R3 == 1u << 2);
    assert(VD_PAD_START == 1u << 3);
    assert(VD_PAD_UP == 1u << 4);
    assert(VD_PAD_RIGHT == 1u << 5);
    assert(VD_PAD_DOWN == 1u << 6);
    assert(VD_PAD_LEFT == 1u << 7);
    assert(VD_PAD_L2 == 1u << 8);
    assert(VD_PAD_R2 == 1u << 9);
    assert(VD_PAD_L1 == 1u << 10);
    assert(VD_PAD_R1 == 1u << 11);
    assert(VD_PAD_TRIANGLE == 1u << 12);
    assert(VD_PAD_CIRCLE == 1u << 13);
    assert(VD_PAD_CROSS == 1u << 14);
    assert(VD_PAD_SQUARE == 1u << 15);
    assert(VD_PAD_HOME == 1u << 16);
    assert(VD_PAD_TOUCH == 1u << 17);

    assert(VD_PAD_WIRE_BYTES == 28u);

    vd_pad_make_report(&in, 0xA1B2C3D4u, UINT64_C(0x0102030405060708),
                       0x0003FFFFu, /* every defined button bit */
                       (int16_t)0x1122, (int16_t)0x3344, (int16_t)-1, (int16_t)0x7FFF, 0x5A, 0xA5);

    /* Pre-dirty the buffer: the serializer must zero the reserved u16. */
    memset(wire, 0xEE, sizeof(wire));
    n = vd_pad_serialize(&in, wire, sizeof(wire));
    assert(n == 28);

    /* Byte-exact little-endian layout from the wire spec. */
    {
        static const uint8_t expect[VD_PAD_WIRE_BYTES] = {
            0xD4, 0xC3, 0xB2, 0xA1,                         /* u32 report_id */
            0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, /* u64 timestamp_us */
            0xFF, 0xFF, 0x03, 0x00,                         /* u32 buttons */
            0x22, 0x11,                                     /* i16 left_x */
            0x44, 0x33,                                     /* i16 left_y */
            0xFF, 0xFF,                                     /* i16 right_x */
            0xFF, 0x7F,                                     /* i16 right_y */
            0x5A, 0xA5,                                     /* u8 l2, u8 r2 */
            0x00, 0x00                                      /* u16 reserved */
        };
        assert(memcmp(wire, expect, VD_PAD_WIRE_BYTES) == 0);
    }

    /* Round-trip every field. */
    assert(vd_pad_deserialize(wire, n, &out) == 0);
    assert(out.report_id == 0xA1B2C3D4u);
    assert(out.timestamp_us == UINT64_C(0x0102030405060708));
    assert(out.buttons == 0x0003FFFFu);
    assert(out.left_x == (int16_t)0x1122);
    assert(out.left_y == (int16_t)0x3344);
    assert(out.right_x == (int16_t)-1);
    assert(out.right_y == (int16_t)0x7FFF);
    assert(out.l2 == 0x5A);
    assert(out.r2 == 0xA5);

    /* Guard rails: undersized buffers are refused, not truncated. */
    assert(vd_pad_serialize(&in, wire, VD_PAD_WIRE_BYTES - 1) == 0);
    assert(vd_pad_serialize(NULL, wire, sizeof(wire)) == 0);
    assert(vd_pad_deserialize(wire, VD_PAD_WIRE_BYTES - 1, &out) == -1);
    assert(vd_pad_deserialize(NULL, VD_PAD_WIRE_BYTES, &out) == -1);

    printf("  pad wire layout + round-trip OK (28-byte wire)\n");
}

int main(void)
{
    printf("VITA5 usb transfer host tests:\n");
    test_out_completion();
    test_in_completion();
    test_status_map();
    test_pad_wire_layout();
    printf("ALL PASS\n");
    return 0;
}
