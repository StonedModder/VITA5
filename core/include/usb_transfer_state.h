/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — pure USB transfer state logic.
 *
 * Shared by the PS5 payload implementation (core/src/usb_transfer.c) and the
 * host unit tests (tests/test_usb_transfer.c). This header deliberately has no
 * PS5/USB includes so the completion-validation rules run on the dev host.
 *
 * Ported from Ghostcontrol's proven usb_helpers_state.h / usb_helpers.c.
 * Key semantics kept exactly:
 *
 *   FreeBSD's USB_FS_COMPLETE dequeues the OLDEST completion record for the
 *   WHOLE ugen session; its input ep_index is not a filter. A waiter must
 *   validate that the dequeued record belongs to the endpoint it started
 *   (completed_index == expected_index) and must drain records for other
 *   endpoints rather than mistaking them for its own completion.
 */
#pragma once

#include <errno.h>
#include <stdint.h>

/* usb_error_t values from dev/usb/usbdi.h that usb_ioctl.h does not export.
 * These are part of FreeBSD's stable USB ABI. */
#define USB_STATUS_CANCELLED    5
#define USB_STATUS_TIMEOUT     20
#define USB_STATUS_SHORT_XFER  21 /* short packet terminated the transfer */
#define USB_STATUS_INTERRUPTED 23

/* Completion outcomes. The enum/constant names are kept verbatim from the
 * ported Ghostcontrol helper (usb_helpers_state.h); the outcomes themselves
 * are direction-neutral and are reused by the bulk IN validator below. */
enum UsbOutCompletionCheck {
    USB_OUT_COMPLETION_OTHER = 0,
    USB_OUT_COMPLETION_OK = 1,
    USB_OUT_COMPLETION_STATUS_ERROR = -1,
    USB_OUT_COMPLETION_FRAME_ERROR = -2,
    USB_OUT_COMPLETION_LENGTH_ERROR = -3
};

/* Map a FreeBSD USB_ERR_* status to a negative errno. */
static inline int
usb_status_to_errno(int status) {
    if (status == USB_STATUS_TIMEOUT)
        return -ETIMEDOUT;
    if (status == USB_STATUS_CANCELLED)
        return -ECANCELED;
    if (status == USB_STATUS_INTERRUPTED)
        return -EINTR;
    return -EIO;
}

/* Bulk OUT completion validation (exact Ghostcontrol semantics): the record
 * must belong to the endpoint we started, the transfer must have succeeded,
 * exactly one frame must have completed, and an OUT write must complete with
 * EXACTLY the requested length — a short/overrun write is corrupt. */
static inline enum UsbOutCompletionCheck
usb_out_check_completion(uint8_t completed_index, uint8_t expected_index,
                         int status, uint32_t actual_frames,
                         uint32_t actual_length, uint32_t expected_length) {
    if (completed_index != expected_index)
        return USB_OUT_COMPLETION_OTHER;
    if (status != 0)
        return USB_OUT_COMPLETION_STATUS_ERROR;
    if (actual_frames != 1)
        return USB_OUT_COMPLETION_FRAME_ERROR;
    if (actual_length != expected_length)
        return USB_OUT_COMPLETION_LENGTH_ERROR;
    return USB_OUT_COMPLETION_OK;
}

/* Bulk IN completion validation: same record ownership/status/frame rules,
 * but a SHORT read is legal (UVC payloads legitimately end in short packets),
 * so the length check is an overrun guard: actual_length must fit the buffer
 * the caller supplied as `buffer_capacity`.
 *
 * This kernel reports a short-packet-terminated transfer as
 * USB_ERR_SHORT_XFER (status 21) instead of status 0 + short length. The
 * received bytes are valid and `actual_length` holds their size, so 21 is a
 * NORMAL completion for an IN payload read - treating it as an error dropped
 * every short payload ("video frames sometimes").
 */
static inline enum UsbOutCompletionCheck
usb_in_check_completion(uint8_t completed_index, uint8_t expected_index,
                        int status, uint32_t actual_frames,
                        uint32_t actual_length, uint32_t buffer_capacity) {
    if (completed_index != expected_index)
        return USB_OUT_COMPLETION_OTHER;
    if (status != 0 && status != USB_STATUS_SHORT_XFER)
        return USB_OUT_COMPLETION_STATUS_ERROR;
    if (actual_frames != 1)
        return USB_OUT_COMPLETION_FRAME_ERROR;
    if (actual_length > buffer_capacity)
        return USB_OUT_COMPLETION_LENGTH_ERROR;
    return USB_OUT_COMPLETION_OK;
}
