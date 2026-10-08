/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test-only FreeBSD USB ioctl shim. NOT a hardware ABI definition; it
 * carries the usbfs endpoint/transfer structs and ioctl identifiers the
 * real core sources reference so they compile for host tests. */
#pragma once
#include <stdint.h>
struct usb_fs_endpoint
{
    void **ppBuffer;   /* pointer to userland buffers */
    uint32_t *pLength; /* frame lengths, updated to actual */
    uint32_t nFrames;  /* number of frames */
    uint32_t aFrames;  /* actual number of frames */
    uint16_t flags;
#define USB_FS_FLAG_SINGLE_SHORT_OK 0x0001
#define USB_FS_FLAG_MULTI_SHORT_OK 0x0002
#define USB_FS_FLAG_FORCE_SHORT 0x0004
#define USB_FS_FLAG_CLEAR_STALL 0x0008
    uint16_t timeout;
    uint16_t isoc_time_complete;
#define USB_FS_TIMEOUT_NONE 0
    int status;
};
struct usb_alt_interface
{
    uint8_t uai_interface_index;
    uint8_t uai_alt_index;
};
struct usb_fs_start
{
    uint8_t ep_index;
};
struct usb_fs_stop
{
    uint8_t ep_index;
};
struct usb_fs_complete
{
    uint8_t ep_index;
};
struct usb_fs_init
{
    struct usb_fs_endpoint *pEndpoints;
    uint8_t ep_index_max;
};
struct usb_fs_uninit
{
    uint8_t dummy;
};
struct usb_fs_open
{
#define USB_FS_MAX_BUFSIZE (1 << 25)
    uint32_t max_bufsize;
#define USB_FS_MAX_FRAMES (1U << 12)
#define USB_FS_MAX_FRAMES_PRE_SCALE (1U << 31)
    uint32_t max_frames;
    uint16_t max_packet_length;
    uint8_t dev_index;
    uint8_t ep_index;
    uint8_t ep_no;
};
struct usb_fs_close
{
    uint8_t ep_index;
};
/* Distinct identifiers only; values are irrelevant for the artificial USB
 * harness (the ioctl wrapper switches on these symbols). */
#define USB_IFACE_DRIVER_DETACH 0x56440001UL
#define USB_SET_ALTINTERFACE 0x56440002UL
#define USB_DO_REQUEST 0x56440003UL
#define USB_FS_START 0x56440010UL
#define USB_FS_STOP 0x56440011UL
#define USB_FS_COMPLETE 0x56440012UL
#define USB_FS_INIT 0x56440013UL
#define USB_FS_UNINIT 0x56440014UL
#define USB_FS_OPEN 0x56440015UL
#define USB_FS_CLOSE 0x56440016UL
