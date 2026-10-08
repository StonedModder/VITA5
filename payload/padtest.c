/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — scripted pad-report sender (end-to-end input test).
 *
 * Standalone PS5 payload for testing the pad passthrough path WITHOUT the
 * native app: it discovers the PS Vita (vd_usb_scan_vita), opens its ugen
 * node, and sends a scripted sequence of 28-byte pad wire reports as the
 * data stage of a vendor control OUT on EP0 (bmRequestType 0x40,
 * bRequest 0x50 'P') — the channel the vita-side/input-receiver plugin
 * listens on (it wraps the UVC gadget's processRequest and queues the data
 * stage on the gadget's own EP0; no extra USB interface is published, so
 * the stream descriptors stay untouched):
 *
 *   neutral -> D-PAD (up/down/left/right) -> face buttons (cross, circle,
 *   square, triangle) -> shoulders/triggers (L1, R1, L2, R2) -> START/SELECT
 *   -> stick sweeps (LX, LY, RX, RY) -> neutral.
 *
 * Each step is held ~600 ms (several reports at 10 Hz, above the Vita-side
 * 500 ms stuck-input reset and its ~0.5 s emulation window), so whatever the
 * Vita is showing (LiveArea, a game, ...) visibly reacts. HOME is not in the
 * sequence on purpose (firmware-dependent emulation, would kick the user out).
 *
 * Log output goes to the elfldr console (stderr). Ctrl-C / SIGTERM stops it.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pad_passthrough.h"
#include "usb_transfer.h"
#include "usb_vita.h"

#define VD_LOG(...)                      \
    do                                   \
    {                                    \
        fprintf(stderr, "[padtest] " __VA_ARGS__); \
    } while (0)

#define PADTEST_REPORT_HZ 10
#define PADTEST_HOLD_MS 600

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void wait_ms(unsigned ms)
{
    struct timespec req;

    req.tv_sec = ms / 1000u;
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&req, &req) != 0 && errno == EINTR && !g_stop)
        ;
}

typedef struct
{
    const char *name;
    uint32_t buttons;
    int16_t lx, ly, rx, ry;
    uint8_t l2, r2;
} PadStep;

static const PadStep k_steps[] = {
    {"CROSS", VD_PAD_CROSS, 0, 0, 0, 0, 0, 0},
    {"CIRCLE", VD_PAD_CIRCLE, 0, 0, 0, 0, 0, 0},
    {"neutral", 0, 0, 0, 0, 0, 0, 0},
    {"D-PAD UP", VD_PAD_UP, 0, 0, 0, 0, 0, 0},
    {"D-PAD DOWN", VD_PAD_DOWN, 0, 0, 0, 0, 0, 0},
    {"D-PAD LEFT", VD_PAD_LEFT, 0, 0, 0, 0, 0, 0},
    {"D-PAD RIGHT", VD_PAD_RIGHT, 0, 0, 0, 0, 0, 0},
    {"CROSS", VD_PAD_CROSS, 0, 0, 0, 0, 0, 0},
    {"CIRCLE", VD_PAD_CIRCLE, 0, 0, 0, 0, 0, 0},
    {"SQUARE", VD_PAD_SQUARE, 0, 0, 0, 0, 0, 0},
    {"TRIANGLE", VD_PAD_TRIANGLE, 0, 0, 0, 0, 0, 0},
    {"L1", VD_PAD_L1, 0, 0, 0, 0, 0, 0},
    {"R1", VD_PAD_R1, 0, 0, 0, 0, 0, 0},
    {"L2", VD_PAD_L2, 0, 0, 0, 0, 0, 0},
    {"R2", VD_PAD_R2, 0, 0, 0, 0, 0, 0},
    {"START", VD_PAD_START, 0, 0, 0, 0, 0, 0},
    {"SELECT", VD_PAD_SELECT, 0, 0, 0, 0, 0, 0},
    {"L-STICK X sweep", 0, -32768, 0, 0, 0, 0, 0},
    {"L-STICK X sweep +", 0, 32767, 0, 0, 0, 0, 0},
    {"L-STICK Y sweep", 0, 0, -32768, 0, 0, 0, 0},
    {"L-STICK Y sweep +", 0, 0, 32767, 0, 0, 0, 0},
    {"R-STICK X sweep", 0, 0, 0, -32768, 0, 0, 0},
    {"R-STICK X sweep +", 0, 0, 0, 32767, 0, 0, 0},
    {"R-STICK Y sweep", 0, 0, 0, 0, -32768, 0, 0},
    {"R-STICK Y sweep +", 0, 0, 0, 0, 32767, 0, 0},
    {"triggers half", 0, 0, 0, 0, 0, 128, 128},
    {"neutral (reset)", 0, 0, 0, 0, 0, 0, 0},
};

/* Discover the Vita and open its ugen node. Pad reports ride EP0 control
 * transfers, so no bulk endpoint session is needed. */
static int open_vita(int *fd_out)
{
    VdUsbVitaDevice dev;

    memset(&dev, 0, sizeof(dev));
    if (vd_usb_scan_vita(&dev, 1u) == 0)
    {
        VD_LOG("no PS Vita UVC device found on /dev/ugen*\n");
        return -ENODEV;
    }
    VD_LOG("vita at %s (vid=0x%04x pid=0x%04x) video=%d audio=%d\n",
           dev.path, dev.vid, dev.pid, dev.has_video, dev.has_audio);

    *fd_out = open(dev.path, O_RDWR | O_NONBLOCK);
    if (*fd_out < 0)
    {
        VD_LOG("open %s failed: %d\n", dev.path, errno);
        return -errno;
    }
    VD_LOG("pad reports go out as vendor control OUT 0x40/0x50 on EP0\n");
    return 0;
}

int main(int argc, char **argv)
{
    /* "quick" runs only the CROSS and CIRCLE steps (focused single-button
     * check on the Vita's screen). */
    int quick = (argc > 1 && strcmp(argv[1], "quick") == 0);
    int fd = -1;
    uint32_t report_id = 0;
    size_t i;
    int rc;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    rc = open_vita(&fd);
    if (rc != 0)
        return 1;

    VD_LOG("running %zu steps, %d reports/s, %d ms per step\n",
           sizeof(k_steps) / sizeof(k_steps[0]), PADTEST_REPORT_HZ, PADTEST_HOLD_MS);

    for (i = 0; i < sizeof(k_steps) / sizeof(k_steps[0]) && !g_stop; ++i)
    {
        const PadStep *step = &k_steps[i];
        int reps = (PADTEST_HOLD_MS * PADTEST_REPORT_HZ) / 1000;
        int r;

        if (quick && strcmp(step->name, "CROSS") != 0 && strcmp(step->name, "CIRCLE") != 0)
            continue;
        if (reps < 1)
            reps = 1;
        VD_LOG("step %zu/%zu: %s\n", i + 1, sizeof(k_steps) / sizeof(k_steps[0]), step->name);
        for (r = 0; r < reps && !g_stop; ++r)
        {
            VdPadReport report;

            vd_pad_make_report(&report, ++report_id, monotonic_us(), step->buttons,
                               step->lx, step->ly, step->rx, step->ry, step->l2, step->r2);
            if (report_id <= 8)
            {
                /* Byte-level evidence for the Vita-side log comparison. */
                uint8_t w[VD_PAD_WIRE_BYTES];
                size_t wn = vd_pad_serialize(&report, w, sizeof(w));
                VD_LOG("sent[%u]:", report_id);
                for (size_t b = 0; b < wn && b < 16; ++b)
                    VD_LOG(" %02x", w[b]);
                VD_LOG("\n");
            }
            rc = usb_send_pad_report(fd, NULL, &report);
            if (rc != 0)
            {
                VD_LOG("send failed (%d) - reopening session\n", rc);
                close(fd);
                fd = -1;
                wait_ms(250);
                if (open_vita(&fd) != 0)
                {
                    VD_LOG("vita gone, stopping\n");
                    g_stop = 1;
                    break;
                }
                continue;
            }
            wait_ms(1000 / PADTEST_REPORT_HZ);
        }
    }

    /* Always leave the Vita with a neutral report so nothing stays pressed. */
    if (fd >= 0)
    {
        VdPadReport neutral;

        vd_pad_make_report(&neutral, ++report_id, monotonic_us(), 0, 0, 0, 0, 0, 0, 0);
        usb_send_pad_report(fd, NULL, &neutral);
        close(fd);
    }
    VD_LOG("done (sent %u reports)\n", report_id);
    return 0;
}
