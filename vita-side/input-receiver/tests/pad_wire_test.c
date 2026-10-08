/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pad_wire_test.c - host-side round-trip unit test for the pad wire decoder.
 *
 * The encoder below is written independently from the SPEC (longhand
 * little-endian stores at the exact documented offsets), not from the
 * decoder, so encode->decode round-tripping is a meaningful cross-check.
 *
 * Build (see README): clang-18 -std=c99 -Wall -Wextra -Werror ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pad_wire.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do {                                                  \
	g_checks++;                                                       \
	if (!(cond)) {                                                    \
		g_failures++;                                             \
		fprintf(stderr, "FAIL %s:%d: %s\n",                       \
		        __FILE__, __LINE__, #cond);                       \
	}                                                                 \
} while (0)

#define CHECK_EQ_U32(actual, expected) do {                               \
	unsigned long _a = (unsigned long)(actual);                       \
	unsigned long _e = (unsigned long)(expected);                     \
	g_checks++;                                                       \
	if (_a != _e) {                                                   \
		g_failures++;                                             \
		fprintf(stderr, "FAIL %s:%d: %s == 0x%08lx, expected 0x%08lx\n", \
		        __FILE__, __LINE__, #actual, _a, _e);             \
	}                                                                 \
} while (0)

/* ------------------------------------------------------------------------- */
/* Independent reference encoder (from the wire spec, not from pad_wire.c)    */
/* ------------------------------------------------------------------------- */

static void spec_encode(unsigned char *out, const pad_wire_report *r)
{
	/* offset 0: u32 report_id */
	out[0] = (unsigned char)(r->report_id & 0xFF);
	out[1] = (unsigned char)((r->report_id >> 8) & 0xFF);
	out[2] = (unsigned char)((r->report_id >> 16) & 0xFF);
	out[3] = (unsigned char)((r->report_id >> 24) & 0xFF);

	/* offset 4: u64 timestamp_us */
	out[4] = (unsigned char)(r->timestamp_us & 0xFF);
	out[5] = (unsigned char)((r->timestamp_us >> 8) & 0xFF);
	out[6] = (unsigned char)((r->timestamp_us >> 16) & 0xFF);
	out[7] = (unsigned char)((r->timestamp_us >> 24) & 0xFF);
	out[8] = (unsigned char)((r->timestamp_us >> 32) & 0xFF);
	out[9] = (unsigned char)((r->timestamp_us >> 40) & 0xFF);
	out[10] = (unsigned char)((r->timestamp_us >> 48) & 0xFF);
	out[11] = (unsigned char)((r->timestamp_us >> 56) & 0xFF);

	/* offset 12: u32 buttons */
	out[12] = (unsigned char)(r->buttons & 0xFF);
	out[13] = (unsigned char)((r->buttons >> 8) & 0xFF);
	out[14] = (unsigned char)((r->buttons >> 16) & 0xFF);
	out[15] = (unsigned char)((r->buttons >> 24) & 0xFF);

	/* offset 16..22: four i16 stick axes */
	out[16] = (unsigned char)((uint16_t)r->left_x & 0xFF);
	out[17] = (unsigned char)(((uint16_t)r->left_x >> 8) & 0xFF);
	out[18] = (unsigned char)((uint16_t)r->left_y & 0xFF);
	out[19] = (unsigned char)(((uint16_t)r->left_y >> 8) & 0xFF);
	out[20] = (unsigned char)((uint16_t)r->right_x & 0xFF);
	out[21] = (unsigned char)(((uint16_t)r->right_x >> 8) & 0xFF);
	out[22] = (unsigned char)((uint16_t)r->right_y & 0xFF);
	out[23] = (unsigned char)(((uint16_t)r->right_y >> 8) & 0xFF);

	/* offset 24: u8 l2, offset 25: u8 r2 */
	out[24] = r->l2;
	out[25] = r->r2;

	/* offset 26: u16 reserved (0) */
	out[26] = (unsigned char)(r->reserved & 0xFF);
	out[27] = (unsigned char)((r->reserved >> 8) & 0xFF);
}

static int reports_equal(const pad_wire_report *a, const pad_wire_report *b)
{
	return a->report_id == b->report_id &&
	       a->timestamp_us == b->timestamp_us &&
	       a->buttons == b->buttons &&
	       a->left_x == b->left_x &&
	       a->left_y == b->left_y &&
	       a->right_x == b->right_x &&
	       a->right_y == b->right_y &&
	       a->l2 == b->l2 &&
	       a->r2 == b->r2 &&
	       a->reserved == b->reserved;
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

/* Golden vector: literal bytes as they appear on the wire.
 *
 *   report_id    = 0xAABBCCDD
 *   timestamp_us = 0x1122334455667788
 *   buttons      = 0x00028051  (bits 0,4,6,15,17: SELECT,UP,DOWN,SQUARE,TOUCH)
 *   left_x  = -2,  left_y = 300,  right_x = -300,  right_y = 32767
 *   l2 = 0x33, r2 = 0x44, reserved = 0
 */
static void test_golden_vector(void)
{
	static const unsigned char golden[PAD_WIRE_SIZE] = {
		0xDD, 0xCC, 0xBB, 0xAA,                   /* report_id */
		0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, /* timestamp_us */
		0x51, 0x80, 0x02, 0x00,                   /* buttons */
		0xFE, 0xFF,                               /* left_x  = -2 */
		0x2C, 0x01,                               /* left_y  = 300 */
		0xD4, 0xFE,                               /* right_x = -300 */
		0xFF, 0x7F,                               /* right_y = 32767 */
		0x33,                                     /* l2 */
		0x44,                                     /* r2 */
		0x00, 0x00                                /* reserved */
	};
	pad_wire_report r;
	unsigned char reencoded[PAD_WIRE_SIZE];
	uint32_t ctrl;

	memset(&r, 0, sizeof(r));
	CHECK(pad_wire_decode(golden, sizeof(golden), &r) == 0);

	CHECK_EQ_U32(r.report_id, 0xAABBCCDD);
	CHECK(r.timestamp_us == 0x1122334455667788ull);
	CHECK_EQ_U32(r.buttons, 0x00028051);
	CHECK(r.left_x == -2);
	CHECK(r.left_y == 300);
	CHECK(r.right_x == -300);
	CHECK(r.right_y == 32767);
	CHECK_EQ_U32(r.l2, 0x33);
	CHECK_EQ_U32(r.r2, 0x44);
	CHECK_EQ_U32(r.reserved, 0x0000);

	/* Button mapping: bits 0,4,6,15 survive, TOUCH (bit 17) is stripped. */
	ctrl = pad_wire_buttons_to_ctrl(r.buttons);
	CHECK_EQ_U32(ctrl, 0x00008051);

	/* Analog triggers above threshold force their digital bits:
	 * 0x8051 | L2(0x100) | R2(0x200) = 0x8351 */
	ctrl = pad_wire_apply_triggers(ctrl, r.l2, r.r2, PAD_WIRE_TRIGGER_THRESHOLD);
	CHECK_EQ_U32(ctrl, 0x00008351);

	/* Stick conversion (full-range signed -> 0..255, 0x80 centered). */
	CHECK_EQ_U32(pad_wire_stick_to_u8(r.left_x), 0x7F);  /* -2   -> 127 */
	CHECK_EQ_U32(pad_wire_stick_to_u8(r.left_y), 0x81);  /* 300  -> 129 */
	CHECK_EQ_U32(pad_wire_stick_to_u8(r.right_x), 0x7E); /* -300 -> 126 */
	CHECK_EQ_U32(pad_wire_stick_to_u8(r.right_y), 0xFF); /* 32767-> 255 */

	/* The decoder must round-trip the golden bytes through the reference
	 * encoder byte-for-byte. */
	spec_encode(reencoded, &r);
	CHECK(memcmp(reencoded, golden, PAD_WIRE_SIZE) == 0);
}

static void test_all_buttons(void)
{
	unsigned char buf[PAD_WIRE_SIZE];
	pad_wire_report r;
	static const struct {
		uint32_t bit;
		uint32_t expected;
	} cases[] = {
		{ PAD_WIRE_BTN_SELECT,   PAD_WIRE_CTRL_SELECT },
		{ PAD_WIRE_BTN_L3,       PAD_WIRE_CTRL_L3 },
		{ PAD_WIRE_BTN_R3,       PAD_WIRE_CTRL_R3 },
		{ PAD_WIRE_BTN_START,    PAD_WIRE_CTRL_START },
		{ PAD_WIRE_BTN_UP,       PAD_WIRE_CTRL_UP },
		{ PAD_WIRE_BTN_RIGHT,    PAD_WIRE_CTRL_RIGHT },
		{ PAD_WIRE_BTN_DOWN,     PAD_WIRE_CTRL_DOWN },
		{ PAD_WIRE_BTN_LEFT,     PAD_WIRE_CTRL_LEFT },
		{ PAD_WIRE_BTN_L2,       PAD_WIRE_CTRL_LTRIGGER },
		{ PAD_WIRE_BTN_R2,       PAD_WIRE_CTRL_RTRIGGER },
		{ PAD_WIRE_BTN_L1,       PAD_WIRE_CTRL_L1 },
		{ PAD_WIRE_BTN_R1,       PAD_WIRE_CTRL_R1 },
		{ PAD_WIRE_BTN_TRIANGLE, PAD_WIRE_CTRL_TRIANGLE },
		{ PAD_WIRE_BTN_CIRCLE,   PAD_WIRE_CTRL_CIRCLE },
		{ PAD_WIRE_BTN_CROSS,    PAD_WIRE_CTRL_CROSS },
		{ PAD_WIRE_BTN_SQUARE,   PAD_WIRE_CTRL_SQUARE },
		{ PAD_WIRE_BTN_HOME,     PAD_WIRE_CTRL_HOME },
		{ PAD_WIRE_BTN_TOUCH,    0 }, /* stripped: no SceCtrl equivalent */
	};
	size_t i;

	memset(buf, 0, sizeof(buf));
	buf[0] = 1; /* report_id = 1 so the report is not all-zero */

	/* Every wire button bit individually. */
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		pad_wire_report out;
		uint32_t ctrl;

		buf[12] = (unsigned char)(cases[i].bit & 0xFF);
		buf[13] = (unsigned char)((cases[i].bit >> 8) & 0xFF);
		buf[14] = (unsigned char)((cases[i].bit >> 16) & 0xFF);
		buf[15] = 0;

		CHECK(pad_wire_decode(buf, sizeof(buf), &out) == 0);
		CHECK_EQ_U32(out.buttons, cases[i].bit);
		ctrl = pad_wire_buttons_to_ctrl(out.buttons);
		CHECK_EQ_U32(ctrl, cases[i].expected);
	}

	/* All bits at once: buttons 0x0003FFFF -> ctrl 0x0001FFFF. */
	memset(&r, 0, sizeof(r));
	r.report_id = 2;
	r.buttons = 0x0003FFFFu;
	spec_encode(buf, &r);
	CHECK(pad_wire_decode(buf, sizeof(buf), &r) == 0);
	CHECK_EQ_U32(pad_wire_buttons_to_ctrl(r.buttons), 0x0001FFFFu);
}

static void test_stick_conversion(void)
{
	CHECK_EQ_U32(pad_wire_stick_to_u8(-32768), 0x00);
	CHECK_EQ_U32(pad_wire_stick_to_u8(-32767), 0x00);
	CHECK_EQ_U32(pad_wire_stick_to_u8(-1), 0x7F);
	CHECK_EQ_U32(pad_wire_stick_to_u8(0), 0x80);
	CHECK_EQ_U32(pad_wire_stick_to_u8(1), 0x80);
	CHECK_EQ_U32(pad_wire_stick_to_u8(255), 0x80);
	CHECK_EQ_U32(pad_wire_stick_to_u8(256), 0x81);
	CHECK_EQ_U32(pad_wire_stick_to_u8(32767), 0xFF);
}

static void test_trigger_threshold(void)
{
	/* Threshold semantics: strictly greater than. */
	CHECK_EQ_U32(pad_wire_apply_triggers(0, 0, 0, 32), 0);
	CHECK_EQ_U32(pad_wire_apply_triggers(0, 32, 32, 32), 0);
	CHECK_EQ_U32(pad_wire_apply_triggers(0, 33, 0, 32), PAD_WIRE_CTRL_LTRIGGER);
	CHECK_EQ_U32(pad_wire_apply_triggers(0, 0, 33, 32), PAD_WIRE_CTRL_RTRIGGER);
	CHECK_EQ_U32(pad_wire_apply_triggers(0, 255, 255, 32),
	             PAD_WIRE_CTRL_LTRIGGER | PAD_WIRE_CTRL_RTRIGGER);
	/* Existing bits are preserved. */
	CHECK_EQ_U32(pad_wire_apply_triggers(PAD_WIRE_CTRL_CROSS, 33, 33, 32),
	             PAD_WIRE_CTRL_CROSS | PAD_WIRE_CTRL_LTRIGGER |
	             PAD_WIRE_CTRL_RTRIGGER);
}

static void test_length_and_null_rejects(void)
{
	unsigned char buf[PAD_WIRE_SIZE];
	pad_wire_report r;

	memset(buf, 0, sizeof(buf));
	CHECK(pad_wire_decode(buf, PAD_WIRE_SIZE - 1, &r) == -1);
	CHECK(pad_wire_decode(buf, PAD_WIRE_SIZE + 1, &r) == -1);
	CHECK(pad_wire_decode(buf, 0, &r) == -1);
	CHECK(pad_wire_decode(NULL, PAD_WIRE_SIZE, &r) == -1);
	CHECK(pad_wire_decode(buf, PAD_WIRE_SIZE, NULL) == -1);
	/* Right size must still succeed. */
	CHECK(pad_wire_decode(buf, PAD_WIRE_SIZE, &r) == 0);
	/* All-zero report: sticks decode to centered 0x80. */
	CHECK_EQ_U32(pad_wire_stick_to_u8(r.left_x), 0x80);
	CHECK_EQ_U32(pad_wire_stick_to_u8(r.left_y), 0x80);
	CHECK_EQ_U32(pad_wire_stick_to_u8(r.right_x), 0x80);
	CHECK_EQ_U32(pad_wire_stick_to_u8(r.right_y), 0x80);
}

static void test_roundtrip_fuzz(void)
{
	unsigned char buf[PAD_WIRE_SIZE];
	pad_wire_report in, out;
	uint32_t seed = 0xC0FFEE01u;
	int i;

	for (i = 0; i < 100000; i++) {
		/* Deterministic LCG so failures are reproducible. */
		seed = seed * 1664525u + 1013904223u;
		in.report_id = seed;
		seed = seed * 1664525u + 1013904223u;
		in.timestamp_us = ((uint64_t)seed << 32);
		seed = seed * 1664525u + 1013904223u;
		in.timestamp_us |= seed;
		seed = seed * 1664525u + 1013904223u;
		in.buttons = seed & 0x0003FFFFu;
		seed = seed * 1664525u + 1013904223u;
		in.left_x = (int16_t)(seed & 0xFFFF);
		seed = seed * 1664525u + 1013904223u;
		in.left_y = (int16_t)(seed & 0xFFFF);
		seed = seed * 1664525u + 1013904223u;
		in.right_x = (int16_t)(seed & 0xFFFF);
		seed = seed * 1664525u + 1013904223u;
		in.right_y = (int16_t)(seed & 0xFFFF);
		seed = seed * 1664525u + 1013904223u;
		in.l2 = (uint8_t)(seed & 0xFF);
		in.r2 = (uint8_t)((seed >> 8) & 0xFF);
		in.reserved = 0; /* spec-mandated */

		spec_encode(buf, &in);
		memset(&out, 0, sizeof(out));
		if (pad_wire_decode(buf, PAD_WIRE_SIZE, &out) != 0 ||
		    !reports_equal(&in, &out)) {
			g_failures++;
			fprintf(stderr, "FAIL roundtrip at iteration %d\n", i);
			return;
		}
		g_checks++;
	}
}

/* Touch record: independent longhand encoder per the SPEC + round-trip. */
static void test_touch_wire(void)
{
	pad_wire_touch out;
	unsigned char buf[PAD_WIRE_TOUCH_SIZE];
	uint32_t seed = 0x7A110005u;
	unsigned int i;

	/* Golden vector: rear port, one finger at (123, 456). */
	memset(buf, 0, sizeof(buf));
	buf[0] = 1;      /* port: rear */
	buf[1] = 1;      /* count */
	buf[4] = 123; buf[5] = 0;    /* f0_x = 123 LE */
	buf[6] = 0x58; buf[7] = 0x01; /* f0_y = 0x0158 = 344 LE */
	buf[12] = 1;     /* f0_active */
	CHECK(pad_wire_touch_decode(buf, PAD_WIRE_TOUCH_SIZE, &out) == 0);
	CHECK_EQ_U32(out.port, PAD_WIRE_TOUCH_PORT_REAR);
	CHECK_EQ_U32(out.count, 1);
	CHECK_EQ_U32(out.f0_x, 123);
	CHECK_EQ_U32(out.f0_y, 0x0158);
	CHECK_EQ_U32(out.f0_active, 1);
	CHECK_EQ_U32(out.f1_active, 0);

	/* Rejects: wrong length, NULLs, bad port, bad active flag, out-of-range
	 * coordinates. */
	CHECK(pad_wire_touch_decode(buf, PAD_WIRE_TOUCH_SIZE - 1, &out) == -1);
	CHECK(pad_wire_touch_decode(buf, PAD_WIRE_TOUCH_SIZE + 1, &out) == -1);
	CHECK(pad_wire_touch_decode(buf, 0, &out) == -1);
	CHECK(pad_wire_touch_decode(NULL, PAD_WIRE_TOUCH_SIZE, &out) == -1);
	CHECK(pad_wire_touch_decode(buf, PAD_WIRE_TOUCH_SIZE, NULL) == -1);
	buf[0] = 2;
	CHECK(pad_wire_touch_decode(buf, PAD_WIRE_TOUCH_SIZE, &out) == -1);
	buf[0] = 1;
	buf[12] = 2;
	CHECK(pad_wire_touch_decode(buf, PAD_WIRE_TOUCH_SIZE, &out) == -1);
	buf[12] = 1;
	buf[4] = 0xFF; buf[5] = 0xFF; /* f0_x = 65535 > 1919 */
	CHECK(pad_wire_touch_decode(buf, PAD_WIRE_TOUCH_SIZE, &out) == -1);

	/* Encode/decode round-trip against an independent longhand writer. */
	for (i = 0; i < 20000; ++i) {
		pad_wire_touch in;
		unsigned char ref[PAD_WIRE_TOUCH_SIZE];

		seed = seed * 1664525u + 1013904223u;
		in.port = (uint8_t)(seed & 1u);
		seed = seed * 1664525u + 1013904223u;
		in.count = (uint8_t)(seed % 3u);
		seed = seed * 1664525u + 1013904223u;
		in.f0_x = (uint16_t)(seed % (PAD_WIRE_TOUCH_X_MAX + 1u));
		seed = seed * 1664525u + 1013904223u;
		in.f0_y = (uint16_t)(seed % (PAD_WIRE_TOUCH_Y_MAX + 1u));
		seed = seed * 1664525u + 1013904223u;
		in.f1_x = (uint16_t)(seed % (PAD_WIRE_TOUCH_X_MAX + 1u));
		seed = seed * 1664525u + 1013904223u;
		in.f1_y = (uint16_t)(seed % (PAD_WIRE_TOUCH_Y_MAX + 1u));
		seed = seed * 1664525u + 1013904223u;
		in.f0_active = (uint8_t)(seed & 1u);
		seed = seed * 1664525u + 1013904223u;
		in.f1_active = (uint8_t)(seed & 1u);

		/* Independent longhand encoder (SPEC offsets). */
		memset(ref, 0, sizeof(ref));
		ref[0] = in.port;
		ref[1] = in.count;
		ref[4] = (unsigned char)(in.f0_x & 0xFFu);
		ref[5] = (unsigned char)(in.f0_x >> 8);
		ref[6] = (unsigned char)(in.f0_y & 0xFFu);
		ref[7] = (unsigned char)(in.f0_y >> 8);
		ref[8] = (unsigned char)(in.f1_x & 0xFFu);
		ref[9] = (unsigned char)(in.f1_x >> 8);
		ref[10] = (unsigned char)(in.f1_y & 0xFFu);
		ref[11] = (unsigned char)(in.f1_y >> 8);
		ref[12] = in.f0_active;
		ref[13] = in.f1_active;

		CHECK(pad_wire_touch_encode(&in, buf, sizeof(buf)) ==
		      PAD_WIRE_TOUCH_SIZE);
		CHECK(memcmp(buf, ref, sizeof(ref)) == 0);
		CHECK(pad_wire_touch_decode(buf, sizeof(buf), &out) == 0);
		CHECK_EQ_U32(out.port, in.port);
		CHECK_EQ_U32(out.count, in.count);
		CHECK_EQ_U32(out.f0_x, in.f0_x);
		CHECK_EQ_U32(out.f0_y, in.f0_y);
		CHECK_EQ_U32(out.f1_x, in.f1_x);
		CHECK_EQ_U32(out.f1_y, in.f1_y);
		CHECK_EQ_U32(out.f0_active, in.f0_active);
		CHECK_EQ_U32(out.f1_active, in.f1_active);
	}
}

int main(void)
{
	test_golden_vector();
	test_all_buttons();
	test_stick_conversion();
	test_trigger_threshold();
	test_length_and_null_rejects();
	test_roundtrip_fuzz();
	test_touch_wire();

	if (g_failures == 0) {
		printf("pad_wire_test: ALL TESTS PASSED (%d checks)\n", g_checks);
		return EXIT_SUCCESS;
	}
	printf("pad_wire_test: %d/%d CHECKS FAILED\n", g_failures, g_checks);
	return EXIT_FAILURE;
}
