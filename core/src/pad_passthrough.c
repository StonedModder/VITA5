/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — controller passthrough implementation (serialization + wire).
 */
#include "pad_passthrough.h"
#include <string.h>

uint32_t vd_pad_translate_buttons(uint32_t scepad_buttons)
{
    /* The scePad -> Vita button translation table is filled from the reviewed
     * public Pad ABI. For now pass the mask through unchanged so the wire path
     * is testable end to end; the native app owns the real mapping. */
    return scepad_buttons;
}

void vd_pad_make_report(VdPadReport *out, uint32_t report_id, uint64_t ts_us,
                        uint32_t buttons,
                        int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                        uint8_t l2, uint8_t r2)
{
    if (!out) return;
    out->report_id = report_id;
    out->timestamp_us = ts_us;
    out->buttons = buttons;
    out->left_x = lx;  out->left_y = ly;
    out->right_x = rx; out->right_y = ry;
    out->l2 = l2;      out->r2 = r2;
}

/* Wire layout is documented on VD_PAD_WIRE_BYTES in pad_passthrough.h. */
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v); put_u32(p + 4, (uint32_t)(v >> 32));
}
static void put_i16(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
}
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
}
static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const uint8_t *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}
static int16_t get_i16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

size_t vd_pad_serialize(const VdPadReport *r, uint8_t *buf, size_t cap)
{
    if (!r || !buf || cap < VD_PAD_WIRE_BYTES) return 0;
    memset(buf, 0, VD_PAD_WIRE_BYTES);
    put_u32(buf + 0, r->report_id);
    put_u64(buf + 4, r->timestamp_us);
    put_u32(buf + 12, r->buttons);
    put_i16(buf + 16, r->left_x);
    put_i16(buf + 18, r->left_y);
    put_i16(buf + 20, r->right_x);
    put_i16(buf + 22, r->right_y);
    buf[24] = r->l2;
    buf[25] = r->r2;
    return VD_PAD_WIRE_BYTES;
}

int vd_pad_deserialize(const uint8_t *buf, size_t len, VdPadReport *out)
{
    if (!buf || !out || len < VD_PAD_WIRE_BYTES) return -1;
    out->report_id = get_u32(buf + 0);
    out->timestamp_us = get_u64(buf + 4);
    out->buttons = get_u32(buf + 12);
    out->left_x = get_i16(buf + 16);
    out->left_y = get_i16(buf + 18);
    out->right_x = get_i16(buf + 20);
    out->right_y = get_i16(buf + 22);
    out->l2 = buf[24];
    out->r2 = buf[25];
    return 0;
}

/* Touch wire layout is documented on VD_TOUCH_WIRE_BYTES in pad_passthrough.h. */
size_t vd_touch_serialize(const VdTouchReport *t, uint8_t out[16])
{
    if (!t || !out) return 0;
    memset(out, 0, VD_TOUCH_WIRE_BYTES);
    out[0] = t->port;
    out[1] = t->count;
    put_u16(out + 4, t->f0_x);
    put_u16(out + 6, t->f0_y);
    put_u16(out + 8, t->f1_x);
    put_u16(out + 10, t->f1_y);
    out[12] = t->f0_active;
    out[13] = t->f1_active;
    return VD_TOUCH_WIRE_BYTES;
}

/* Inverse of vd_touch_serialize (used by the kernel payload's forwarder to
 * rebuild the record the app published into the shared header). Returns 1
 * on success, 0 on NULL arguments or a malformed record (count > 2). */
int vd_touch_deserialize(const uint8_t in[16], VdTouchReport *t)
{
    if (!in || !t) return 0;
    if (in[1] > 2u) return 0;
    memset(t, 0, sizeof(*t));
    t->port = in[0];
    t->count = in[1];
    t->f0_x = get_u16(in + 4);
    t->f0_y = get_u16(in + 6);
    t->f1_x = get_u16(in + 8);
    t->f1_y = get_u16(in + 10);
    t->f0_active = in[12];
    t->f1_active = in[13];
    return 1;
}
