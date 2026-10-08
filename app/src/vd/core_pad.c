/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 app — bridge TU: compiles the committed core implementations into
 * the app without copying their bytes.
 *
 * core/src/pad_passthrough.c is the single source of truth for VdPadReport's
 * wire format (the kernel payload and the tests use the same file).
 *
 * On the console target (Clang's x86_64-sie-ps5, which predefines
 * __PROSPERO__) the working pad transport is bridged the same way:
 * core/src/usb_transfer.c (usb_send_pad_report — the 28-byte wire report as
 * seven SETUP-only control OUTs, hardware-proven per docs/VITA_INPUT_API.md)
 * and core/src/usb_vita.c (the /dev/ugen* scan helpers). The vd_pad_usb_*
 * session wrapper at the bottom is what app/src/vd/pad_bridge.cpp drives:
 * discover the Vita's ugen node on demand, send, and after a failed transfer
 * drop the session and rebuild it through discovery (the
 * payload/pad_forwarder.c pattern) with a quiet exponential backoff, so a
 * missing Vita never spams the log.
 *
 * Host builds compile only the wire layer (the host has no dev/usb headers);
 * the payload-side USB entry points become weak stubs there so host tests
 * can resolve the link contract (any real definition wins over these).
 */
#include "../../../core/src/pad_passthrough.c"

#if defined(__PROSPERO__)

#include "../../../core/src/usb_transfer.c"
#undef VD_LOG /* usb_vita.c tags its own log lines */
#include "../../../core/src/usb_vita.c"
#undef VD_LOG
#include "../../../core/src/uvc_protocol.c"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VD_PAD_USB_LOG(...)                                                                        \
    do                                                                                             \
    {                                                                                              \
        fprintf(stderr, "[pad-usb] " __VA_ARGS__);                                                 \
    } while (0)

#define VD_PAD_USB_BACKOFF_MIN_MS 250u
#define VD_PAD_USB_BACKOFF_MAX_MS 1000u

static int vd_pad_usb_fd = -1;
static uint64_t vd_pad_usb_next_open_ms;
static uint32_t vd_pad_usb_backoff_ms = VD_PAD_USB_BACKOFF_MIN_MS;
static int vd_pad_usb_up;
static int vd_pad_usb_open_warned;

static uint64_t vd_pad_usb_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    return 0u;
}

/* Quiet exponential backoff between (re)open attempts. */
static void vd_pad_usb_postpone(void)
{
    vd_pad_usb_next_open_ms = vd_pad_usb_now_ms() + vd_pad_usb_backoff_ms;
    if (vd_pad_usb_backoff_ms < VD_PAD_USB_BACKOFF_MAX_MS)
    {
        vd_pad_usb_backoff_ms *= 2u;
        if (vd_pad_usb_backoff_ms > VD_PAD_USB_BACKOFF_MAX_MS)
            vd_pad_usb_backoff_ms = VD_PAD_USB_BACKOFF_MAX_MS;
    }
}

/* Hand one report to the transport: discover/open the Vita's ugen node on
 * demand, then usb_send_pad_report(). Returns 0 when the report was sent,
 * -ENODEV while no Vita is attached (quiet, retried on a backoff), or
 * another negative errno after a failed open/send (logged once per incident,
 * never per attempt). */
int vd_pad_usb_send(const VdPadReport *report)
{
    uint64_t now;
    int rc;

    if (!report)
        return -EINVAL;
    now = vd_pad_usb_now_ms();

    if (vd_pad_usb_fd < 0)
    {
        VdUsbVitaDevice dev;

        if (now < vd_pad_usb_next_open_ms)
            return -ENODEV; /* quiet retry window */
        memset(&dev, 0, sizeof(dev));
        if (vd_usb_scan_vita(&dev, 1u) == 0)
        {
            if (vd_pad_usb_up)
            {
                VD_PAD_USB_LOG("vita left the bus; waiting for it to return\n");
                vd_pad_usb_up = 0;
            }
            vd_pad_usb_postpone();
            return -ENODEV;
        }
        vd_pad_usb_fd = open(dev.path, O_RDWR | O_NONBLOCK);
        if (vd_pad_usb_fd < 0)
        {
            rc = -errno;
            if (!vd_pad_usb_open_warned)
            {
                VD_PAD_USB_LOG("open %s failed (%d)\n", dev.path, rc);
                vd_pad_usb_open_warned = 1;
            }
            vd_pad_usb_postpone();
            return rc;
        }
        VD_PAD_USB_LOG("transport up: %s (vid=0x%04x pid=0x%04x)\n", dev.path, dev.vid, dev.pid);
        vd_pad_usb_up = 1;
        vd_pad_usb_open_warned = 0;
        vd_pad_usb_backoff_ms = VD_PAD_USB_BACKOFF_MIN_MS;
        vd_pad_usb_next_open_ms = 0;
    }

    /* One report = seven SETUP-only control OUTs (usb_send_pad_report). */
    rc = usb_send_pad_report(vd_pad_usb_fd, NULL, report);
    if (rc == 0)
    {
        vd_pad_usb_backoff_ms = VD_PAD_USB_BACKOFF_MIN_MS;
        return 0;
    }

    /* A failed transfer leaves the ugen session in an unknown state: drop it
     * and rebuild it through discovery rather than reusing it. */
    close(vd_pad_usb_fd);
    vd_pad_usb_fd = -1;
    if (vd_pad_usb_up)
    {
        VD_PAD_USB_LOG("send failed (%d); rebuilding the session\n", rc);
        vd_pad_usb_up = 0;
    }
    vd_pad_usb_postpone();
    return rc;
}

/* Nonzero while a ugen session to the Vita is open. */
int vd_pad_usb_connected(void)
{
    return vd_pad_usb_fd >= 0;
}

/* Tear the session down (app exit / passthrough disable). */
void vd_pad_usb_close(void)
{
    if (vd_pad_usb_fd >= 0)
        close(vd_pad_usb_fd);
    vd_pad_usb_fd = -1;
    vd_pad_usb_up = 0;
    vd_pad_usb_open_warned = 0;
    vd_pad_usb_backoff_ms = VD_PAD_USB_BACKOFF_MIN_MS;
    vd_pad_usb_next_open_ms = 0;
}

#else /* !__PROSPERO__: host build */

#include <errno.h>

/* Weak host stubs for the payload-side USB transport entry points (the real
 * ones are core/src/usb_transfer.c, bridged in above on the console target).
 * They only exist so host tests can resolve the link contract declared in
 * the bridges (app/src/vd/pad_bridge.cpp, app/src/vd/touch_bridge.cpp);
 * any real definition overrides these. */
#if defined(__GNUC__) || defined(__clang__)
#define VD_PAD_HOST_STUB __attribute__((weak))
#else
#define VD_PAD_HOST_STUB
#endif

/* File-scope tag so the parameter list sees the same type the console
 * prototype uses (usb_transfer.h's opaque endpoint handle). */
struct usb_fs_endpoint;

VD_PAD_HOST_STUB int usb_send_pad_report(int fd, struct usb_fs_endpoint *ep,
                                         const VdPadReport *report)
{
    (void)fd;
    (void)ep;
    (void)report;
    return -ENOSYS;
}

VD_PAD_HOST_STUB int usb_send_touch_report(int fd, const VdTouchReport *t)
{
    (void)fd;
    (void)t;
    return -ENOSYS;
}

#endif /* __PROSPERO__ */
