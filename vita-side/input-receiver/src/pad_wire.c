/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pad_wire.c - VITA5 pad report wire format decoder (Vita side).
 *
 * Host-compilable pure C99. All multi-byte fields are decoded with explicit
 * little-endian shifts: no struct overlays, no unaligned loads, no
 * endianness assumptions about the host.
 */

#include "pad_wire.h"

#include <string.h>

static uint32_t pad_wire_rd_u32(const unsigned char *p)
{
	return ((uint32_t)p[0]) |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint64_t pad_wire_rd_u64(const unsigned char *p)
{
	return ((uint64_t)pad_wire_rd_u32(p)) |
	       ((uint64_t)pad_wire_rd_u32(p + 4) << 32);
}

static uint16_t pad_wire_rd_u16(const unsigned char *p)
{
	return (uint16_t)(((uint16_t)p[0]) | ((uint16_t)p[1] << 8));
}

int pad_wire_decode(const void *buf, size_t len, pad_wire_report *out)
{
	const unsigned char *p = (const unsigned char *)buf;

	if (buf == NULL || out == NULL)
		return -1;
	if (len != PAD_WIRE_SIZE)
		return -1;

	out->report_id    = pad_wire_rd_u32(p + PAD_WIRE_OFF_REPORT_ID);
	out->timestamp_us = pad_wire_rd_u64(p + PAD_WIRE_OFF_TIMESTAMP_US);
	out->buttons      = pad_wire_rd_u32(p + PAD_WIRE_OFF_BUTTONS);
	out->left_x       = (int16_t)pad_wire_rd_u16(p + PAD_WIRE_OFF_LEFT_X);
	out->left_y       = (int16_t)pad_wire_rd_u16(p + PAD_WIRE_OFF_LEFT_Y);
	out->right_x      = (int16_t)pad_wire_rd_u16(p + PAD_WIRE_OFF_RIGHT_X);
	out->right_y      = (int16_t)pad_wire_rd_u16(p + PAD_WIRE_OFF_RIGHT_Y);
	out->l2           = p[PAD_WIRE_OFF_L2];
	out->r2           = p[PAD_WIRE_OFF_R2];
	out->reserved     = pad_wire_rd_u16(p + PAD_WIRE_OFF_RESERVED);

	return 0;
}

uint32_t pad_wire_buttons_to_ctrl(uint32_t wire_buttons)
{
	/* Bits 0..16 are numerically identical to SceCtrlButtons; TOUCH
	 * (bit 17) has no SceCtrl equivalent and is dropped. */
	return wire_buttons & PAD_WIRE_CTRL_MASK;
}

uint32_t pad_wire_apply_triggers(uint32_t ctrl_buttons,
                                 uint8_t l2, uint8_t r2,
                                 uint8_t threshold)
{
	if (l2 > threshold)
		ctrl_buttons |= PAD_WIRE_CTRL_LTRIGGER;
	if (r2 > threshold)
		ctrl_buttons |= PAD_WIRE_CTRL_RTRIGGER;
	return ctrl_buttons;
}

uint8_t pad_wire_stick_to_u8(int16_t value)
{
	return (uint8_t)(((int)value + 32768) >> 8);
}

/* ------------------------------------------------------------------------- */
/* Touch wire format                                                         */
/* ------------------------------------------------------------------------- */

static void pad_wire_wr_u16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)(v & 0xFFu);
	p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

int pad_wire_touch_decode(const void *buf, size_t len, pad_wire_touch *out)
{
	const unsigned char *p = (const unsigned char *)buf;

	if (buf == NULL || out == NULL || len != PAD_WIRE_TOUCH_SIZE)
		return -1;

	out->port      = p[PAD_WIRE_TOUCH_OFF_PORT];
	out->count     = p[PAD_WIRE_TOUCH_OFF_COUNT];
	out->f0_x      = pad_wire_rd_u16(p + PAD_WIRE_TOUCH_OFF_F0_X);
	out->f0_y      = pad_wire_rd_u16(p + PAD_WIRE_TOUCH_OFF_F0_Y);
	out->f1_x      = pad_wire_rd_u16(p + PAD_WIRE_TOUCH_OFF_F1_X);
	out->f1_y      = pad_wire_rd_u16(p + PAD_WIRE_TOUCH_OFF_F1_Y);
	out->f0_active = p[PAD_WIRE_TOUCH_OFF_F0_ACTIVE];
	out->f1_active = p[PAD_WIRE_TOUCH_OFF_F1_ACTIVE];

	/* A malformed record must never reach the touch driver. */
	if (out->port > PAD_WIRE_TOUCH_PORT_REAR || out->count > 2u)
		return -1;
	if ((out->f0_active > 1u) || (out->f1_active > 1u))
		return -1;
	if (out->f0_x > PAD_WIRE_TOUCH_X_MAX || out->f0_y > PAD_WIRE_TOUCH_Y_MAX ||
	    out->f1_x > PAD_WIRE_TOUCH_X_MAX || out->f1_y > PAD_WIRE_TOUCH_Y_MAX)
		return -1;
	return 0;
}

size_t pad_wire_touch_encode(const pad_wire_touch *in, void *buf, size_t cap)
{
	unsigned char *p = (unsigned char *)buf;

	if (in == NULL || buf == NULL || cap < PAD_WIRE_TOUCH_SIZE)
		return 0;

	memset(p, 0, PAD_WIRE_TOUCH_SIZE);
	p[PAD_WIRE_TOUCH_OFF_PORT]      = in->port;
	p[PAD_WIRE_TOUCH_OFF_COUNT]     = in->count;
	pad_wire_wr_u16(p + PAD_WIRE_TOUCH_OFF_F0_X, in->f0_x);
	pad_wire_wr_u16(p + PAD_WIRE_TOUCH_OFF_F0_Y, in->f0_y);
	pad_wire_wr_u16(p + PAD_WIRE_TOUCH_OFF_F1_X, in->f1_x);
	pad_wire_wr_u16(p + PAD_WIRE_TOUCH_OFF_F1_Y, in->f1_y);
	p[PAD_WIRE_TOUCH_OFF_F0_ACTIVE] = in->f0_active;
	p[PAD_WIRE_TOUCH_OFF_F1_ACTIVE] = in->f1_active;
	return PAD_WIRE_TOUCH_SIZE;
}
