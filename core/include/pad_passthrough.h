/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — controller passthrough (input-lag-critical path).
 *
 * The PS5 DualSense state is read in the native app (scePadRead), written into
 * the shared-memory pad ring, and forwarded to the Vita by the kernel payload.
 *
 * Input-lag contract:
 *   - One scePadRead per update (batch drain to the newest record).
 *   - The pad ring is single-producer/single-consumer with a seq counter, so a
 *     write is one store + one release; the kernel consumer copies and forwards
 *     immediately, no syscall on the hot path beyond the USB OUT transfer.
 *   - We keep the LAST-GOOD report and a monotonically increasing report id so
 *     the Vita side can detect held/repeated state without an edge rebuild.
 *
 * The final hop (PS5 -> Vita) is a USB OUT transfer to a vendor/HID endpoint.
 * The Vita-side plugin must accept it; see docs/TRACKING.md (Vita-side HID
 * input reception is the one open dependency for true pad passthrough). Until
 * the Vita endpoint is confirmed, this module stages reports and exposes a
 * forward hook the payload calls per report.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Normalized pad report the Vita side consumes. We forward the raw DualSense
 * state fields we care about rather than scePad's full layout, so the Vita
 * mapping is independent of the PS5 Pad ABI. */
typedef struct {
    uint32_t report_id;    /* monotonically increasing */
    uint64_t timestamp_us;
    /* Buttons: bit mask, Vita-facing order defined in vd_pad_buttons. */
    uint32_t buttons;
    /* Axes normalized to signed 16-bit full range. */
    int16_t left_x, left_y;
    int16_t right_x, right_y;
    uint8_t l2, r2;        /* triggers 0..255 */
    /* Touchpad/motion not forwarded in v1. */
} VdPadReport;

/* Button bits (Vita-facing). These map to the Vita's own control layout. */
enum {
    VD_PAD_SELECT   = 1u << 0,
    VD_PAD_L3       = 1u << 1,
    VD_PAD_R3       = 1u << 2,
    VD_PAD_START    = 1u << 3,
    VD_PAD_UP       = 1u << 4,
    VD_PAD_RIGHT    = 1u << 5,
    VD_PAD_DOWN     = 1u << 6,
    VD_PAD_LEFT     = 1u << 7,
    VD_PAD_L2       = 1u << 8,
    VD_PAD_R2       = 1u << 9,
    VD_PAD_L1       = 1u << 10,
    VD_PAD_R1       = 1u << 11,
    VD_PAD_TRIANGLE = 1u << 12,
    VD_PAD_CIRCLE   = 1u << 13,
    VD_PAD_CROSS    = 1u << 14,
    VD_PAD_SQUARE   = 1u << 15,
    VD_PAD_HOME     = 1u << 16,
    VD_PAD_TOUCH    = 1u << 17,
};

/* Convert a raw DualSense/ScePad button mask into the Vita-facing mask.
 * The concrete scePad bit positions are filled in from the reviewed public
 * Pad ABI header (blackbearreloaded/ps5-native-gamepad-input-research); the
 * translation table lives in the native app where scePadRead runs. */
uint32_t vd_pad_translate_buttons(uint32_t scepad_buttons);

/* Build a report from normalized inputs (native app side). */
void vd_pad_make_report(VdPadReport *out, uint32_t report_id, uint64_t ts_us,
                        uint32_t buttons,
                        int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                        uint8_t l2, uint8_t r2);

/* Wire format (little-endian), VD_PAD_WIRE_BYTES bytes — EXACT, the Vita-side
 * parser mirrors this layout:
 *   0  u32 report_id
 *   4  u64 timestamp_us
 *   12 u32 buttons
 *   16 i16 left_x
 *   18 i16 left_y
 *   20 i16 right_x
 *   22 i16 right_y
 *   24 u8  l2
 *   25 u8  r2
 *   26 u16 reserved (0) */
#define VD_PAD_WIRE_BYTES 28u

/* Serialize a report into a compact wire format for the USB OUT endpoint.
 * Returns the number of bytes written (<= cap). Wire layout is stable and
 * little-endian; the Vita-side parser mirrors this exactly. */
size_t vd_pad_serialize(const VdPadReport *r, uint8_t *buf, size_t cap);

/* Deserialize on the consumer (payload -> USB) side. Returns 0 on success. */
int vd_pad_deserialize(const uint8_t *buf, size_t len, VdPadReport *out);

/* Touch report (front touchscreen / rear touchpad), max two fingers.
 * Coordinates are raw Vita touch space: 0..1919 (x), 0..1087 (y). */
typedef struct VdTouchReport {
    uint8_t port;               /* 0 = front touchscreen, 1 = rear touchpad */
    uint8_t count;              /* active fingers (0..2) */
    uint16_t f0_x, f0_y, f1_x, f1_y;
    uint8_t f0_active, f1_active;
} VdTouchReport;

/* Touch wire format (little-endian), VD_TOUCH_WIRE_BYTES bytes — EXACT, the
 * Vita-side parser (pad_wire.h pad_wire_touch) mirrors this layout:
 *   0  u8  port
 *   1  u8  count
 *   2  u16 reserved (0)
 *   4  u16 f0_x
 *   6  u16 f0_y
 *   8  u16 f1_x
 *   10 u16 f1_y
 *   12 u8  f0_active
 *   13 u8  f1_active
 *   14 u16 reserved (0) */
#define VD_TOUCH_WIRE_BYTES 16u

/* Serialize a touch record into the 16-byte little-endian wire format
 * (docs/VITA_INPUT_API.md section 10). Returns the number of bytes written
 * (VD_TOUCH_WIRE_BYTES), or 0 if `t`/`out` is NULL. */
size_t vd_touch_serialize(const VdTouchReport *t, uint8_t out[16]);

/* Inverse of vd_touch_serialize. Returns 1 on success, 0 on NULL arguments
 * or a malformed record (count > 2). */
int vd_touch_deserialize(const uint8_t in[16], VdTouchReport *t);
