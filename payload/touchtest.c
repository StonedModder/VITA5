/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 touchtest — scripted SceTouch-style strokes over the chunked
 * control channel. Proves the touch wire format + the input_receiver
 * plugin's SceTouch* injection on real hardware without the native app.
 *
 * Wire format (docs/VITA_INPUT_API.md): 16-byte touch record, little-
 * endian, sent as 4 no-data control OUTs (bmRequestType 0x40, bRequest
 * 0x58..0x5B, 4 bytes per request in wValue/wIndex).
 *
 * Script (each phase ~100 ms steps):
 *   1. front touchscreen single-finger tap at (960, 544)
 *   2. front swipe x 200 -> 1700 at y 400
 *   3. front two-finger tap at (600, 544) + (1300, 544)
 *   4. rear touchpad swipe x 200 -> 1700 at y 544
 *   5. release
 *
 * Watch the Vita screen: the front taps/swipes should visibly drive the
 * LiveArea / whatever is under the finger.
 *
 * Build (WSL):
 *   export PS5_PAYLOAD_SDK=/mnt/c/ps5-payload-sdk/ps5-payload-sdk
 *   make -C payload VITA5-touchtest.elf
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "usb_transfer.h"
#include "usb_vita.h"

#define TOUCHTEST_REPORT_HZ 10
#define TOUCHTEST_STEP_US   (1000000 / TOUCHTEST_REPORT_HZ)

#define TOUCH_REQ_BASE      0x58
#define TOUCH_CHUNKS        4
#define TOUCH_RECORD_SIZE   16

#define PORT_FRONT          0
#define PORT_REAR           1

#define VD_LOG(...)                      \
    do                                   \
    {                                    \
        fprintf(stderr, "[touchtest] " __VA_ARGS__); \
    } while (0)

/* Discover the Vita and open its ugen node (same as padtest). */
static int open_vita(int *fd_out)
{
    VdUsbVitaDevice dev;

    memset(&dev, 0, sizeof(dev));
    if (vd_usb_scan_vita(&dev, 1u) == 0)
    {
        VD_LOG("no PS Vita UVC device found on /dev/ugen*\n");
        return -ENODEV;
    }
    VD_LOG("vita at %s (vid=0x%04x pid=0x%04x)\n", dev.path, dev.vid, dev.pid);

    *fd_out = open(dev.path, O_RDWR | O_NONBLOCK);
    if (*fd_out < 0)
    {
        VD_LOG("open %s failed: %d\n", dev.path, errno);
        return -errno;
    }
    return 0;
}

static int send_failures = 0;

static int send_touch(int fd, uint8_t port, uint8_t count,
                      uint16_t x0, uint16_t y0, uint8_t a0,
                      uint16_t x1, uint16_t y1, uint8_t a1)
{
    uint8_t wire[TOUCH_RECORD_SIZE];
    int i;

    /* Independent longhand encoder per the documented layout. */
    memset(wire, 0, sizeof(wire));
    wire[0]  = port;
    wire[1]  = count;
    wire[4]  = (uint8_t)(x0 & 0xFF); wire[5]  = (uint8_t)(x0 >> 8);
    wire[6]  = (uint8_t)(y0 & 0xFF); wire[7]  = (uint8_t)(y0 >> 8);
    wire[8]  = (uint8_t)(x1 & 0xFF); wire[9]  = (uint8_t)(x1 >> 8);
    wire[10] = (uint8_t)(y1 & 0xFF); wire[11] = (uint8_t)(y1 >> 8);
    wire[12] = a0;
    wire[13] = a1;

    for (i = 0; i < TOUCH_CHUNKS; i++) {
        uint16_t wValue = (uint16_t)(wire[i * 4] | ((uint16_t)wire[i * 4 + 1] << 8));
        uint16_t wIndex = (uint16_t)(wire[i * 4 + 2] | ((uint16_t)wire[i * 4 + 3] << 8));
        int rc = usb_ctrl_xfer(fd, 0x40, (uint8_t)(TOUCH_REQ_BASE + i),
                               wValue, wIndex, NULL, 0, 0);
        if (rc != 0) {
            send_failures++;
            return rc;
        }
    }
    return 0;
}

static void hold_touch(int fd, uint8_t port, int steps,
                       uint16_t x0, uint16_t y0, uint8_t a0,
                       uint16_t x1, uint16_t y1, uint8_t a1)
{
    int i;

    for (i = 0; i < steps; i++) {
        send_touch(fd, port, (uint8_t)(a0 + a1),
                   x0, y0, a0, x1, y1, a1);
        usleep(TOUCHTEST_STEP_US);
    }
    send_touch(fd, port, 0, 0, 0, 0, 0, 0, 0);
    usleep(TOUCHTEST_STEP_US);
}

static void swipe(int fd, uint8_t port, uint16_t y,
                  uint16_t x_from, uint16_t x_to, int steps)
{
    int i;

    for (i = 0; i < steps; i++) {
        uint16_t x = (uint16_t)(x_from + ((int)(x_to - x_from) * i) / (steps - 1));

        send_touch(fd, port, 1, x, y, 1, 0, 0, 0);
        usleep(TOUCHTEST_STEP_US);
    }
    send_touch(fd, port, 0, 0, 0, 0, 0, 0, 0);
    usleep(TOUCHTEST_STEP_US);
}

int main(void)
{
    int fd = -1;
    int i, rc;

    printf("VITA5 touchtest: looking for the Vita USB gadget\n");
    for (i = 0; i < 8; i++) {
        rc = open_vita(&fd);
        if (rc == 0)
            break;
        printf("attempt %d: rc=%d (retrying in 2s)\n", i + 1, rc);
        usleep(2000000);
    }
    if (fd < 0) {
        printf("FAILED: no Vita device\n");
        return 1;
    }
    printf("vita device open (fd=%d)\n", fd);

    printf("phase 1: front single-finger tap (960, 544)\n");
    hold_touch(fd, PORT_FRONT, 6, 960, 544, 1, 0, 0, 0);

    printf("phase 2: front swipe x 200->1700 at y 400\n");
    swipe(fd, PORT_FRONT, 400, 200, 1700, 30);

    printf("phase 3: front two-finger tap (600, 544) + (1300, 544)\n");
    hold_touch(fd, PORT_FRONT, 6, 600, 544, 1, 1300, 544, 1);

    printf("phase 4: rear swipe x 200->1700 at y 544\n");
    swipe(fd, PORT_REAR, 544, 200, 1700, 30);

    send_touch(fd, PORT_FRONT, 0, 0, 0, 0, 0, 0, 0);
    close(fd);

    if (send_failures == 0) {
        printf("touchtest: SUCCESS - all touch records sent\n");
        return 0;
    }
    printf("touchtest: FAILURES=%d\n", send_failures);
    return 1;
}
