/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 touchswipe — vertical LiveArea swipes over the touch chunk channel.
 *
 * Sequence requested for the manual check:
 *   swipe UP 15 times, pause, swipe DOWN 15 times, pause, swipe UP 15 times.
 * Net result if injection works: the LiveArea list ends 15 swipes up from
 * where it started ("a different spot").
 *
 * A swipe is a fast flick: finger travels x=960 from y 880->180 (up) or
 * 180->880 (down) in 8 steps of 30 ms, then releases (400 ms between
 * swipes).
 *
 * Touch record: 16-byte little-endian layout from docs/VITA_INPUT_API.md
 * section 10, sent as 4 no-data control OUTs (0x40 / 0x58..0x5B).
 *
 * Build (WSL):
 *   export PS5_PAYLOAD_SDK=/mnt/c/ps5-payload-sdk/ps5-payload-sdk
 *   make -C payload VITA5-touchswipe.elf
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "usb_transfer.h"
#include "usb_vita.h"

#define TOUCH_REQ_BASE      0x58
#define TOUCH_CHUNKS        4
#define TOUCH_RECORD_SIZE   16

#define SWIPE_X             960
#define SWIPE_Y_BOTTOM      880
#define SWIPE_Y_TOP         180
#define SWIPE_STEPS         8
#define SWIPE_STEP_US       30000     /* fast flick */
#define SWIPE_GAP_US        400000

#define VD_LOG(...)                      \
    do                                   \
    {                                    \
        fprintf(stderr, "[touchswipe] " __VA_ARGS__); \
    } while (0)

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

static int send_touch(int fd, uint8_t port, uint8_t count,
                      uint16_t x0, uint16_t y0, uint8_t a0,
                      uint16_t x1, uint16_t y1, uint8_t a1)
{
    uint8_t wire[TOUCH_RECORD_SIZE];
    int i;

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
        if (rc != 0)
            return rc;
    }
    return 0;
}

/* dir = 1: finger travels bottom->top ("swipe up"). dir = -1: reversed. */
static int swipe(int fd, int dir)
{
    uint16_t y_from = (dir > 0) ? SWIPE_Y_BOTTOM : SWIPE_Y_TOP;
    uint16_t y_to   = (dir > 0) ? SWIPE_Y_TOP : SWIPE_Y_BOTTOM;
    int i, rc = 0;

    for (i = 0; i < SWIPE_STEPS; i++) {
        uint16_t y = (uint16_t)(y_from + ((int)(y_to - y_from) * i) / (SWIPE_STEPS - 1));

        rc = send_touch(fd, 0, 1, SWIPE_X, y, 1, 0, 0, 0);
        if (rc != 0)
            return rc;
        usleep(SWIPE_STEP_US);
    }
    rc = send_touch(fd, 0, 0, 0, 0, 0, 0, 0, 0);
    usleep(SWIPE_GAP_US);
    return rc;
}

static int run_batch(int fd, const char *what, int dir, int times)
{
    int i;

    for (i = 0; i < times; i++) {
        int rc = swipe(fd, dir);

        if (rc != 0) {
            printf("%s: swipe %d FAILED rc=%d\n", what, i + 1, rc);
            return rc;
        }
        printf("%s: swipe %d/%d sent\n", what, i + 1, times);
    }
    return 0;
}

int main(void)
{
    int fd = -1;
    int i, rc;

    printf("VITA5 touchswipe: looking for the Vita USB gadget\n");
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

    printf("batch 1: UP x15\n");
    if (run_batch(fd, "up", 1, 15) != 0) goto fail;
    usleep(1000000);
    printf("batch 2: DOWN x15\n");
    if (run_batch(fd, "down", -1, 15) != 0) goto fail;
    usleep(1000000);
    printf("batch 3: UP x15\n");
    if (run_batch(fd, "up", 1, 15) != 0) goto fail;

    send_touch(fd, 0, 0, 0, 0, 0, 0, 0, 0);
    close(fd);
    printf("touchswipe: DONE - 45 swipes sent (net: 15 up from start)\n");
    return 0;

fail:
    close(fd);
    printf("touchswipe: FAILED\n");
    return 1;
}
