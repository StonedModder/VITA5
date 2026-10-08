/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pad_wire.h - VITA5 pad report wire format decoder (Vita side).
 *
 * Pure C99, no Vita/SDK dependencies: this decoder is host-compilable and
 * host-tested (see tests/pad_wire_test.c). It is the exact mirror of the
 * PS5-side encoder.
 *
 * WIRE FORMAT - 28 bytes, little-endian:
 *
 *   offset  0  u32 report_id
 *   offset  4  u64 timestamp_us
 *   offset 12  u32 buttons
 *   offset 16  i16 left_x
 *   offset 18  i16 left_y
 *   offset 20  i16 right_x
 *   offset 22  i16 right_y
 *   offset 24  u8  l2
 *   offset 25  u8  r2
 *   offset 26  u16 reserved (0)
 *
 * Sticks are full-range signed: -32768..32767, 0 = centered.
 * L2/R2 analog are 0..255.
 */

#ifndef VITA5_PAD_WIRE_H
#define VITA5_PAD_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Wire constants                                                            */
/* ------------------------------------------------------------------------- */

/** Size of one pad report on the wire, in bytes. */
#define PAD_WIRE_SIZE 28u

/* Field offsets (documentation / static asserts below). */
#define PAD_WIRE_OFF_REPORT_ID    0u
#define PAD_WIRE_OFF_TIMESTAMP_US 4u
#define PAD_WIRE_OFF_BUTTONS      12u
#define PAD_WIRE_OFF_LEFT_X       16u
#define PAD_WIRE_OFF_LEFT_Y       18u
#define PAD_WIRE_OFF_RIGHT_X      20u
#define PAD_WIRE_OFF_RIGHT_Y      22u
#define PAD_WIRE_OFF_L2           24u
#define PAD_WIRE_OFF_R2           25u
#define PAD_WIRE_OFF_RESERVED     26u

/* ------------------------------------------------------------------------- */
/* Wire button bits (u32 `buttons` field)                                    */
/* ------------------------------------------------------------------------- */

#define PAD_WIRE_BTN_SELECT    (1u << 0)
#define PAD_WIRE_BTN_L3        (1u << 1)
#define PAD_WIRE_BTN_R3        (1u << 2)
#define PAD_WIRE_BTN_START     (1u << 3)
#define PAD_WIRE_BTN_UP        (1u << 4)
#define PAD_WIRE_BTN_RIGHT     (1u << 5)
#define PAD_WIRE_BTN_DOWN      (1u << 6)
#define PAD_WIRE_BTN_LEFT      (1u << 7)
#define PAD_WIRE_BTN_L2        (1u << 8)
#define PAD_WIRE_BTN_R2        (1u << 9)
#define PAD_WIRE_BTN_L1        (1u << 10)
#define PAD_WIRE_BTN_R1        (1u << 11)
#define PAD_WIRE_BTN_TRIANGLE  (1u << 12)
#define PAD_WIRE_BTN_CIRCLE    (1u << 13)
#define PAD_WIRE_BTN_CROSS     (1u << 14)
#define PAD_WIRE_BTN_SQUARE    (1u << 15)
#define PAD_WIRE_BTN_HOME      (1u << 16)
#define PAD_WIRE_BTN_TOUCH     (1u << 17)

/* ------------------------------------------------------------------------- */
/* SceCtrl-equivalent button values                                          */
/*                                                                           */
/* The wire bit layout intentionally matches SceCtrlButtons for bits 0..16:  */
/*   SELECT=0x1 L3=0x2 R3=0x4 START=0x8 UP=0x10 RIGHT=0x20 DOWN=0x40         */
/*   LEFT=0x80 L2/LTRIGGER=0x100 R2/RTRIGGER=0x200 L1=0x400 R1=0x800        */
/*   TRIANGLE=0x1000 CIRCLE=0x2000 CROSS=0x4000 SQUARE=0x8000               */
/*   HOME=0x10000 (== SCE_CTRL_PSBUTTON / SCE_CTRL_INTERCEPTED)             */
/* PAD_WIRE_BTN_TOUCH (bit 17) has no SceCtrlButtons equivalent and is       */
/* stripped by pad_wire_buttons_to_ctrl().                                   */
/*                                                                           */
/* These numeric values are asserted equal to the real SceCtrlButtons enum   */
/* in the kernel plugin (src/input_receiver.c) and by the host test.         */
/* ------------------------------------------------------------------------- */

#define PAD_WIRE_CTRL_SELECT    0x00000001u
#define PAD_WIRE_CTRL_L3        0x00000002u
#define PAD_WIRE_CTRL_R3        0x00000004u
#define PAD_WIRE_CTRL_START     0x00000008u
#define PAD_WIRE_CTRL_UP        0x00000010u
#define PAD_WIRE_CTRL_RIGHT     0x00000020u
#define PAD_WIRE_CTRL_DOWN      0x00000040u
#define PAD_WIRE_CTRL_LEFT      0x00000080u
#define PAD_WIRE_CTRL_LTRIGGER  0x00000100u
#define PAD_WIRE_CTRL_RTRIGGER  0x00000200u
#define PAD_WIRE_CTRL_L1        0x00000400u
#define PAD_WIRE_CTRL_R1        0x00000800u
#define PAD_WIRE_CTRL_TRIANGLE  0x00001000u
#define PAD_WIRE_CTRL_CIRCLE    0x00002000u
#define PAD_WIRE_CTRL_CROSS     0x00004000u
#define PAD_WIRE_CTRL_SQUARE    0x00008000u
#define PAD_WIRE_CTRL_HOME      0x00010000u /* SCE_CTRL_PSBUTTON */

/** All wire bits that map onto SceCtrlButtons (TOUCH excluded). */
#define PAD_WIRE_CTRL_MASK      0x0001FFFFu

/** Analog trigger values above this force the L2/R2 digital bits (see
 *  pad_wire_apply_triggers). Mirrors the PS5-side threshold. */
#define PAD_WIRE_TRIGGER_THRESHOLD 32u

/* ------------------------------------------------------------------------- */
/* Decoded report                                                            */
/* ------------------------------------------------------------------------- */

/**
 * Decoded view of one 28-byte wire report.
 *
 * NOTE: this struct is NOT an overlay of the wire bytes (it is naturally
 * aligned and therefore padded). The wire buffer must always be decoded
 * through pad_wire_decode(), never cast.
 */
typedef struct pad_wire_report {
	uint32_t report_id;
	uint64_t timestamp_us;
	uint32_t buttons;          /* PAD_WIRE_BTN_* bits */
	int16_t  left_x;
	int16_t  left_y;
	int16_t  right_x;
	int16_t  right_y;
	uint8_t  l2;
	uint8_t  r2;
	uint16_t reserved;
} pad_wire_report;

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

/**
 * Decode one wire report.
 *
 * @param buf  raw bytes received on the USB OUT endpoint
 * @param len  number of bytes received (must be exactly PAD_WIRE_SIZE)
 * @param out  decoded report (written only on success)
 * @return 0 on success, -1 on invalid argument or wrong length
 */
int pad_wire_decode(const void *buf, size_t len, pad_wire_report *out);

/**
 * Map wire button bits to SceCtrlButtons-equivalent bits.
 * Identity for bits 0..16; PAD_WIRE_BTN_TOUCH (bit 17) is stripped.
 */
uint32_t pad_wire_buttons_to_ctrl(uint32_t wire_buttons);

/**
 * Force the L2/R2 digital trigger bits from the analog trigger values.
 *
 * @param ctrl_buttons  output of pad_wire_buttons_to_ctrl()
 * @param l2, r2        analog trigger values 0..255
 * @param threshold     a trigger value > threshold sets its digital bit
 *                      (pass PAD_WIRE_TRIGGER_THRESHOLD)
 * @return ctrl_buttons with PAD_WIRE_CTRL_LTRIGGER/RTRIGGER OR'd in as needed
 */
uint32_t pad_wire_apply_triggers(uint32_t ctrl_buttons,
                                 uint8_t l2, uint8_t r2,
                                 uint8_t threshold);

/**
 * Convert a full-range signed stick axis (-32768..32767) to the SceCtrl
 * unsigned range (0..255, 0x80 = centered).
 *
 *   -32768 -> 0x00,  -1 -> 0x7F,  0 -> 0x80,  256 -> 0x81,  32767 -> 0xFF
 */
uint8_t pad_wire_stick_to_u8(int16_t value);

/* ------------------------------------------------------------------------- */
/* Compile-time wire layout checks                                           */
/* ------------------------------------------------------------------------- */

#define PAD_WIRE_STATIC_ASSERT(cond, name) \
	typedef char pad_wire_static_assert_##name[(cond) ? 1 : -1]

PAD_WIRE_STATIC_ASSERT(PAD_WIRE_SIZE == 28, size_is_28);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_REPORT_ID == 0, off_report_id);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_TIMESTAMP_US == 4, off_timestamp);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_BUTTONS == 12, off_buttons);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_LEFT_X == 16, off_left_x);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_LEFT_Y == 18, off_left_y);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_RIGHT_X == 20, off_right_x);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_RIGHT_Y == 22, off_right_y);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_L2 == 24, off_l2);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_R2 == 25, off_r2);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_RESERVED == 26, off_reserved);

/* ------------------------------------------------------------------------- */
/* Touch wire format (16 bytes, little-endian)                               */
/* ------------------------------------------------------------------------- */
/*
 * Carried as its own chunked control sequence (bRequest 0x58..0x5B), one
 * record per touch state change:
 *
 *   offset  0  u8  port       0 = front touchscreen, 1 = rear touchpad
 *   offset  1  u8  count      active fingers (0..2)
 *   offset  2  u16 reserved   (0)
 *   offset  4  u16 f0_x       finger 0, raw Vita space 0..1919
 *   offset  6  u16 f0_y       finger 0, raw Vita space 0..1087
 *   offset  8  u16 f1_x       finger 1
 *   offset 10  u16 f1_y       finger 1
 *   offset 12  u8  f0_active  0/1
 *   offset 13  u8  f1_active  0/1
 *   offset 14  u16 reserved   (0)
 *
 * The Vita-side plugin maps finger 0/1 onto injection slots 0/1
 * (synthetic touch ids 0x70/0x71). Inactive fingers are ignored even if
 * their coordinates are stale.
 */

#define PAD_WIRE_TOUCH_SIZE          16u
#define PAD_WIRE_TOUCH_OFF_PORT      0u
#define PAD_WIRE_TOUCH_OFF_COUNT     1u
#define PAD_WIRE_TOUCH_OFF_RESERVED0 2u
#define PAD_WIRE_TOUCH_OFF_F0_X      4u
#define PAD_WIRE_TOUCH_OFF_F0_Y      6u
#define PAD_WIRE_TOUCH_OFF_F1_X      8u
#define PAD_WIRE_TOUCH_OFF_F1_Y      10u
#define PAD_WIRE_TOUCH_OFF_F0_ACTIVE 12u
#define PAD_WIRE_TOUCH_OFF_F1_ACTIVE 13u
#define PAD_WIRE_TOUCH_OFF_RESERVED1 14u

#define PAD_WIRE_TOUCH_PORT_FRONT    0u
#define PAD_WIRE_TOUCH_PORT_REAR     1u

/** Raw Vita touch coordinate space (both ports). */
#define PAD_WIRE_TOUCH_X_MAX         1919
#define PAD_WIRE_TOUCH_Y_MAX         1087

/** Decoded view of one 16-byte touch record (not an overlay - decode only). */
typedef struct pad_wire_touch {
	uint8_t  port;
	uint8_t  count;
	uint16_t f0_x;
	uint16_t f0_y;
	uint16_t f1_x;
	uint16_t f1_y;
	uint8_t  f0_active;
	uint8_t  f1_active;
} pad_wire_touch;

/**
 * Decode one touch record.
 *
 * @return 0 on success, -1 on invalid argument or wrong length
 */
int pad_wire_touch_decode(const void *buf, size_t len, pad_wire_touch *out);

/**
 * Encode one touch record (mirror of the decoder; used by senders/tests).
 *
 * @return number of bytes written (PAD_WIRE_TOUCH_SIZE), or 0 on error
 */
size_t pad_wire_touch_encode(const pad_wire_touch *in, void *buf, size_t cap);

PAD_WIRE_STATIC_ASSERT(PAD_WIRE_TOUCH_SIZE == 16, touch_size_is_16);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_TOUCH_OFF_F0_X == 4, touch_off_f0_x);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_TOUCH_OFF_F1_Y == 10, touch_off_f1_y);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_TOUCH_OFF_F1_ACTIVE == 13, touch_off_f1_active);
PAD_WIRE_STATIC_ASSERT(PAD_WIRE_OFF_RESERVED + 2 == PAD_WIRE_SIZE, size_matches_fields);

#ifdef __cplusplus
}
#endif

#endif /* VITA5_PAD_WIRE_H */
