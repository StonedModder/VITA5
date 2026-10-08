/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — host regression: real uac_audio.c transfer framing LIFETIME under
 * an EBUSY USB_FS_START with an older pending transfer (artificial USB).
 *
 * The defect this catches (pre-fix uac_audio.c): when USB_FS_START failed
 * with EBUSY — meaning an OLDER transfer was still queued and the kernel
 * could still copy out through the session's persistent framing arrays —
 * the error path cleared the framing fields anyway. The next vd_uac_read()
 * then started a NEW transfer over the same arrays while the old one was
 * still addressable, and the session-FIFO completion of the old transfer
 * could be reaped as the new one's. The fix keeps the in-flight guard and
 * cancels/reaps first (quarantining the session when the reap fails), the
 * same discipline usb_transfer.c already used.
 *
 * Scenarios:
 *   1. baseline     — normal transfer completes and reassembles 32*192 PCM.
 *   2. EBUSY START  — older transfer pending AND un-reapable (STOP ok,
 *                     completion never arrives, CLOSE fails): the framing
 *                     must SURVIVE (guard kept), the session must be dead,
 *                     and no further USB_FS_START may be issued.
 *   3. EBUSY START  — older transfer pending but its cancel reaps: framing
 *                     may then be cleared and the next read must work.
 *
 * Build+run: tools/test-uac-framing.sh (used by tools/build-core-tests.sh).
 * Test-only: no production files are modified by this harness.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "uac_audio.h"
#include "usb_transfer.h"
#include "usb_vita.h"

/* ---- tiny check framework ---------------------------------------------- */

static int g_passes;
static int g_failures;

#define CHECK(cond, ...)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (cond)                                                                                  \
        {                                                                                          \
            g_passes++;                                                                            \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            fprintf(stderr, "RED: " __VA_ARGS__);                                                  \
            fprintf(stderr, "\n       at %s:%d\n", __FILE__, __LINE__);                            \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* ---- scripted artificial USB ------------------------------------------- */

enum MockComplete
{
    MOCK_BUSY = 0, /* USB_FS_COMPLETE: -1/EBUSY, nothing reaped */
    MOCK_REAP,     /* USB_FS_COMPLETE: 0, kernel copy-out performed */
    MOCK_FAIL      /* USB_FS_COMPLETE: -1/<errno>, nothing reaped */
};

#define SCRIPT_MAX 32

static struct
{
    /* USB_FS_START script: 0 = success, else errno. */
    int start_script[SCRIPT_MAX];
    int start_len;
    int start_pos;
    int start_count;

    /* USB_FS_COMPLETE script. */
    enum MockComplete complete_script[SCRIPT_MAX];
    int complete_errno[SCRIPT_MAX];       /* for MOCK_FAIL */
    uint32_t complete_frames[SCRIPT_MAX]; /* for MOCK_REAP */
    int complete_status[SCRIPT_MAX];      /* for MOCK_REAP */
    int complete_len;
    int complete_pos;

    /* USB_FS_CLOSE script: 0 = success, else errno. */
    int close_script[SCRIPT_MAX];
    int close_len;
    int close_pos;
} g_mock;

/* The session under test: the kernel copy-out writes into its endpoint
 * array, exactly like the real ugen usbfs does. */
static VdUacSession *g_session;

static void mock_reset(void)
{
    memset(&g_mock, 0, sizeof(g_mock));
}

static void mock_push_start(int err)
{
    g_mock.start_script[g_mock.start_len++] = err;
}

static void mock_push_complete_busy(void)
{
    g_mock.complete_script[g_mock.complete_len++] = MOCK_BUSY;
}

static void mock_push_complete_reap(uint32_t frames, int status)
{
    g_mock.complete_script[g_mock.complete_len] = MOCK_REAP;
    g_mock.complete_frames[g_mock.complete_len] = frames;
    g_mock.complete_status[g_mock.complete_len] = status;
    g_mock.complete_len++;
}

static void mock_push_close(int err)
{
    g_mock.close_script[g_mock.close_len++] = err;
}

int __wrap_open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    return 42;
}

int __wrap_close(int fd)
{
    (void)fd;
    return 0;
}

int __wrap_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    (void)fds;
    (void)nfds;
    (void)timeout;
    return 1; /* ready-ish: loops stay fast, deadlines still bound them */
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;

    (void)fd;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    switch (request)
    {
    case USB_IFACE_DRIVER_DETACH:
    case USB_SET_ALTINTERFACE:
    case USB_FS_INIT:
    case USB_FS_UNINIT:
    case USB_FS_STOP:
        return 0;

    case USB_FS_OPEN:
    {
        struct usb_fs_open *open = (struct usb_fs_open *)arg;

        open->max_packet_length = 192;
        open->max_frames = 32;
        return 0;
    }

    case USB_FS_START:
    {
        int err = 0;

        g_mock.start_count++;
        if (g_mock.start_pos < g_mock.start_len)
            err = g_mock.start_script[g_mock.start_pos++];
        if (err != 0)
        {
            errno = err;
            return -1;
        }
        return 0;
    }

    case USB_FS_COMPLETE:
    {
        struct usb_fs_complete *complete = (struct usb_fs_complete *)arg;
        enum MockComplete behavior = MOCK_BUSY;
        int pos = g_mock.complete_pos++;

        if (pos < g_mock.complete_len)
            behavior = g_mock.complete_script[pos];

        if (behavior == MOCK_BUSY)
        {
            errno = EBUSY;
            return -1;
        }
        if (behavior == MOCK_FAIL)
        {
            errno = g_mock.complete_errno[pos];
            return -1;
        }
        /* MOCK_REAP: kernel copy-out into the session's endpoint slot. */
        if (g_session != NULL)
        {
            struct usb_fs_endpoint *ep = &g_session->eps[0];
            uint32_t i;
            uint32_t frames = g_mock.complete_frames[pos];

            complete->ep_index = 0;
            ep->aFrames = frames;
            ep->status = g_mock.complete_status[pos];
            for (i = 0; i < frames && ep->pLength != NULL; i++)
                ep->pLength[i] = 192;
        }
        return 0;
    }

    case USB_FS_CLOSE:
    {
        int err = 0;

        if (g_mock.close_pos < g_mock.close_len)
            err = g_mock.close_script[g_mock.close_pos++];
        if (err != 0)
        {
            errno = err;
            return -1;
        }
        return 0;
    }

    default:
        errno = ENOTTY;
        return -1;
    }
}

/* uac_audio.c's raw SET_INTERFACE fallback; irrelevant under the mock. */
int usb_ctrl_xfer(int fd, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                  void *data, uint16_t len, uint16_t flags)
{
    (void)fd;
    (void)bmRequestType;
    (void)bRequest;
    (void)wValue;
    (void)wIndex;
    (void)data;
    (void)len;
    (void)flags;
    return 0;
}

/* ---- scenarios ---------------------------------------------------------- */

static void open_session(VdUacSession *s, VdUsbVitaDevice *dev)
{
    memset(s, 0, sizeof(*s));
    memset(dev, 0, sizeof(*dev));
    dev->has_audio = 1;
    dev->audio_isoc_ep = 0x83;
    dev->audio_isoc_maxpkt = 192;
    dev->audio_stream_interface = 3;
    dev->audio_isoc_alt = 1;
    g_session = s;

    CHECK(vd_uac_session_open(s, dev) == 0, "session_open failed");
    CHECK(vd_uac_start(s, 0) == 0, "start failed");
}

static void scenario_baseline(void)
{
    VdUacSession s;
    VdUsbVitaDevice dev;
    uint8_t pcm[8192];
    uint32_t pcm_len = 123, packets = 456;
    int rc;

    mock_reset();
    open_session(&s, &dev);

    mock_push_start(0);
    mock_push_complete_busy(); /* not complete yet */
    mock_push_complete_reap(32, 0);

    rc = vd_uac_read(&s, pcm, sizeof(pcm), &pcm_len, NULL, 0, &packets);
    CHECK(rc == 0, "baseline read rc=%d, expected 0", rc);
    CHECK(pcm_len == 32u * 192u, "baseline pcm_len=%u, expected 6144", pcm_len);
    CHECK(packets == 32u, "baseline packets=%u, expected 32", packets);
    CHECK(s.eps[0].nFrames == 0, "baseline: framing not cleared after success");

    vd_uac_stop(&s);
    vd_uac_session_close(&s);
    g_session = NULL;
    printf("  scenario baseline OK\n");
}

static void scenario_ebusy_unreapable(void)
{
    VdUacSession s;
    VdUsbVitaDevice dev;
    uint8_t pcm[8192];
    uint32_t pcm_len = 0, packets = 0;
    int rc, starts_after_first;

    mock_reset();
    open_session(&s, &dev);

    /* EBUSY START = an OLDER transfer is still queued; then the cancel
     * cannot be reaped (no completion ever arrives) and CLOSE fails. */
    mock_push_start(EBUSY);
    mock_push_close(EIO); /* cancel's fallback close fails: not benign */

    rc = vd_uac_read(&s, pcm, sizeof(pcm), &pcm_len, NULL, 0, &packets);
    CHECK(rc == -EBUSY, "EBUSY START read rc=%d, expected -EBUSY", rc);

    /* The defect: clearing the framing while the OLDER transfer can still
     * copy out through it. It must survive as the in-flight guard. */
    CHECK(s.eps[0].nFrames != 0, "framing CLEARED after EBUSY START with an un-reapable older "
                                 "transfer pending (nFrames=0): the kernel may still copy out "
                                 "through this framing and the next read would START over it");
    CHECK(s.dead != 0, "session not quarantined (dead=0) after an un-reapable cancel");

    starts_after_first = g_mock.start_count;
    rc = vd_uac_read(&s, pcm, sizeof(pcm), &pcm_len, NULL, 0, &packets);
    CHECK(rc == -EIO, "dead-session read rc=%d, expected -EIO", rc);
    CHECK(g_mock.start_count == starts_after_first,
          "a new USB_FS_START (%d -> %d) was issued while an older transfer "
          "is still pending and un-reaped",
          starts_after_first, g_mock.start_count);

    vd_uac_stop(&s);
    vd_uac_session_close(&s);
    g_session = NULL;
    printf("  scenario ebusy-unreapable OK\n");
}

static void scenario_ebusy_reaped(void)
{
    VdUacSession s;
    VdUsbVitaDevice dev;
    uint8_t pcm[8192];
    uint32_t pcm_len = 0, packets = 0;
    int rc;

    mock_reset();
    open_session(&s, &dev);

    /* EBUSY START again, but this time the older transfer's cancel is
     * reaped: only then may the framing be cleared and a new read run. */
    mock_push_start(EBUSY);
    mock_push_complete_reap(0, USB_STATUS_CANCELLED); /* cancel completion */

    rc = vd_uac_read(&s, pcm, sizeof(pcm), &pcm_len, NULL, 0, &packets);
    CHECK(rc == -EBUSY, "EBUSY START read rc=%d, expected -EBUSY", rc);
    CHECK(s.eps[0].nFrames == 0, "framing kept after a REAPED cancel (nFrames!=0): the guard must "
                                 "clear once the older transfer is accounted for");
    CHECK(s.dead == 0, "session wrongly quarantined after a reaped cancel");

    /* And the session is fully usable again. */
    mock_push_start(0);
    mock_push_complete_reap(32, 0);
    rc = vd_uac_read(&s, pcm, sizeof(pcm), &pcm_len, NULL, 0, &packets);
    CHECK(rc == 0, "post-cancel read rc=%d, expected 0", rc);
    CHECK(pcm_len == 32u * 192u, "post-cancel pcm_len=%u, expected 6144", pcm_len);

    vd_uac_stop(&s);
    vd_uac_session_close(&s);
    g_session = NULL;
    printf("  scenario ebusy-reaped OK\n");
}

int main(void)
{
    printf("[test-uac-framing] real uac_audio.c, artificial USB\n");
    scenario_baseline();
    scenario_ebusy_unreapable();
    scenario_ebusy_reaped();
    printf("[test-uac-framing] %d checks pass, %d FAIL\n", g_passes, g_failures);
    return g_failures != 0;
}
