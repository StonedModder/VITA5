/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — EP0 OUT data-stage round-trip test.
 *
 * Checks whether control-OUT data stages actually reach the Vita gadget at
 * all: sends a UVC VS_PROBE_CONTROL SET_CUR with distinctive values (the
 * same 0x21/interface shape the stream's own handshake uses), then reads
 * them back with GET_CUR. If the values round-trip, EP0 OUT data delivery
 * works and the pad channel's zeroed buffers are our bug; if they don't,
 * no OUT data stage reaches the gadget on this firmware.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "uvc_protocol.h"
#include "usb_transfer.h"
#include "usb_vita.h"

#define LOG(...) fprintf(stderr, "[eptest] " __VA_ARGS__)

int main(void)
{
    VdUsbVitaDevice dev;
    int fd;
    uint8_t setbuf[64], getbuf[64];
    int rc;

    memset(&dev, 0, sizeof(dev));
    if (vd_usb_scan_vita(&dev, 1u) == 0)
    {
        LOG("no vita\n");
        return 1;
    }
    LOG("vita at %s uvc_iface=%u\n", dev.path, dev.uvc.interface_number);
    fd = open(dev.path, O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        LOG("open failed %d\n", errno);
        return 1;
    }

    /* Distinctive probe payload: format 1, frame 5 (640x480), interval
     * 0x00027BC1 (not a default anywhere). */
    memset(setbuf, 0, sizeof(setbuf));
    setbuf[0] = 0x01;             /* bFormatIndex */
    setbuf[1] = 0x05;             /* bFrameIndex */
    setbuf[21] = 0xC1;            /* dwFrameInterval = 0x00027BC1 */
    setbuf[22] = 0x7B;
    setbuf[23] = 0x02;
    setbuf[24] = 0x00;

    rc = usb_uvc_ctrl_xfer(fd, dev.uvc.interface_number, 0x01,
                           0x02 /* VS_PROBE_CONTROL */, setbuf, 34, NULL, 0);
    LOG("SET_CUR probe -> %d\n", rc);

    memset(getbuf, 0, sizeof(getbuf));
    rc = usb_uvc_ctrl_xfer(fd, dev.uvc.interface_number, 0x81,
                           0x02, NULL, 0, getbuf, 34);
    LOG("GET_CUR probe -> %d\n", rc);
    LOG("readback: format=%u frame=%u interval=0x%02x%02x%02x%02x\n",
        getbuf[0], getbuf[1], getbuf[24], getbuf[23], getbuf[22], getbuf[21]);
    if (getbuf[0] == 0x01 && getbuf[1] == 0x05 && getbuf[21] == 0xC1)
        LOG("RESULT: EP0 OUT data stages DELIVER (round-trip OK)\n");
    else
        LOG("RESULT: EP0 OUT data stages DO NOT DELIVER (readback is the gadget's old/default state)\n");

    /* Leave the probe state harmless: commit what we read back. */
    usb_uvc_ctrl_xfer(fd, dev.uvc.interface_number, 0x01,
                      0x02, getbuf, 34, NULL, 0);
    close(fd);
    return 0;
}
