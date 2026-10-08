/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 streamdiag — "is the Vita actually sending video + audio?" probe.
 *
 * Runs entirely from the PS5 as an elfldr payload; prints a verdict over its
 * own console (payload_send.py captures it). No app needed. The goal is to
 * answer, before any app-side debugging:
 *
 *   VIDEO: does the UVC bulk IN endpoint deliver UVC payloads? (counts
 *          payloads, frames completed via the EOF bit, and errors)
 *   AUDIO: does the UAC isoc IN endpoint deliver PCM? (bytes + packet count)
 *
 * Both use the core transport exactly like the dock payload does, so this
 * isolates transport/gadget health from the app.
 *
 * Build (WSL):  make -C payload VITA5-streamdiag.elf
 * Run:          python tools/payload_send.py payload/VITA5-streamdiag.elf
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#include "uac_audio.h"
#include "usb_transfer.h"
#include "usb_vita.h"
#include "uvc_stream.h"

#define DIAG_SECONDS        5
#ifndef DIAG_VIDEO_ONLY
#define DIAG_VIDEO_ONLY 0
#endif
#ifndef DIAG_AUDIO_ONLY
#define DIAG_AUDIO_ONLY 0
#endif
#ifndef DIAG_WORKER
#define DIAG_WORKER 0
#endif
#ifndef VD_BUILD_REV
#define VD_BUILD_REV "unknown"
#endif
#if DIAG_VIDEO_ONLY && DIAG_AUDIO_ONLY
#error "Choose only one isolated stream"
#endif
#define DIAG_MAX_PAYLOAD    (792 * 1024) /* frame 783360 + header + slack */
#define DIAG_PCM_CAP        (128 * 1024)

static uint8_t g_payload[DIAG_MAX_PAYLOAD];
static uint8_t g_pcm[DIAG_PCM_CAP];

static uint64_t diag_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    return (uint64_t)time(NULL) * 1000u;
}

static int open_vita(VdUsbVitaDevice *dev)
{
    int i;

    memset(dev, 0, sizeof(*dev));
    for (i = 0; i < 10; i++) {
        if (vd_usb_scan_vita(dev, 1u) != 0) {
            printf("[diag] Vita found at %s (vid=%04x pid=%04x) video=%d audio=%d pad=%d\n",
                   dev->path, dev->vid, dev->pid, dev->has_video,
                   dev->has_audio, dev->has_pad);
            return 0;
        }
        printf("[diag] no Vita UVC device yet (attempt %d/10), retrying in 2s\n", i + 1);
        usleep(2000000);
    }
    return -1;
}

/* ---- video ------------------------------------------------------------- */

static void print_ugen_nodes(const char *tag)
{
    DIR *d = opendir("/dev");
    struct dirent *e;
    printf("[diag] %s ugen nodes:", tag);
    if (!d) {
        printf(" <opendir failed errno=%d>\n", errno);
        return;
    }
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "ugen", 4) == 0)
            printf(" %s", e->d_name);
    }
    closedir(d);
    printf("\n");
}

/* Read-only liveness probe on the LIVE session fd after a transport hard
 * error: a successful EP0 GET_CUR proves the device is still enumerated and
 * answering control requests (session-level fault); failure plus a missing
 * ugen node proves device loss (link/gadget fault). */
static void probe_alive(VdUvcSession *s)
{
    VdUvcStreamingControl c;
    int rc;
    memset(&c, 0, sizeof(c));
    rc = usb_uvc_ctrl_xfer(s->fd, s->vs_interface, UVC_GET_CUR,
                           UVC_VS_PROBE_CONTROL, NULL, 0, &c, sizeof(c));
    printf("[diag] POST-ERROR liveness: EP0 GET_CUR rc=%d errno=%d fmt=%u frame=%u\n",
           rc, rc == 0 ? 0 : errno, c.bFormatIndex, c.bFrameIndex);
}

/* Detach the PS5 system's uaudio driver from the UAC interfaces: at attach
 * the console adopts the Vita as a USB microphone (uaudio0/pcm0) and its
 * isoc IN polling then runs alongside our bulk capture. Detaching before the
 * stream starts tests — and for a video-only run removes — that co-activity. */
static void detach_audio_drivers(int fd, const VdUsbVitaDevice *dev)
{
    int iface, rc;
    if (dev->audio_stream_interface == 0)
        return;
    iface = (int)dev->audio_stream_interface;
    rc = ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface);
    printf("[diag] detach iface %d (AudioStreaming) rc=%d errno=%d\n",
           iface, rc, rc == 0 ? 0 : errno);
    iface = (int)dev->audio_stream_interface - 1; /* AudioControl */
    rc = ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface);
    printf("[diag] detach iface %d (AudioControl) rc=%d errno=%d\n",
           iface, rc, rc == 0 ? 0 : errno);
    usleep(300000); /* let uaudio release its isoc transfers */
}

/* Fresh session from scratch (open + endpoint + probe/commit). fmt 1,
 * frame 1 (960x544), default interval. */
static int video_open(const VdUsbVitaDevice *dev, VdUvcSession *s)
{
    int rc;
    memset(s, 0, sizeof(*s));
    rc = vd_uvc_session_open(s, dev);
    if (rc != 0) {
        printf("[diag] VIDEO: session open FAILED rc=%d\n", rc);
        return rc;
    }
    detach_audio_drivers(s->fd, dev);
    rc = vd_uvc_start_stream(s, 1, 1, 166666, 783360u);
    if (rc != 0) {
        printf("[diag] VIDEO: start_stream FAILED rc=%d\n", rc);
        vd_uvc_session_close(s);
        return rc;
    }
    return 0;
}

static int test_video(const VdUsbVitaDevice *dev)
{
    VdUvcSession s;
    VdUsbVitaDevice dev_cur;
    uint32_t payloads = 0, frames_done = 0, timeouts = 0, errors = 0;
    uint32_t first_sizes = 0, eof_fids = 0, recoveries = 0;
    uint64_t t_start, t_read, deadline;
    int i, session_open;

    printf("[diag] --- VIDEO: bounded %ds on bulk IN ---\n", DIAG_SECONDS);
    dev_cur = *dev;
    if (video_open(&dev_cur, &s) != 0)
        return 1;
    session_open = 1;
    t_start = diag_ms();
    printf("[diag] VIDEO identity: fd=%d session=%p eps=%p errno_ptr=%p\n",
           s.fd, (void *)&s, (void *)s.eps, (void *)&errno);
    /* Discriminant: hold the session open with NO bulk reads for 3s after the
     * handshake. If the gadget drops here, the kill is at stream start
     * (alt-select / commit / frame production), not at our transfers. */
    {
        uint64_t idle_end = diag_ms() + 3000u;
        printf("[diag] VIDEO idle-after-start: 3s with no bulk reads\n");
        while (diag_ms() < idle_end)
            usleep(200000);
        probe_alive(&s);
        print_ugen_nodes("POST-IDLE");
    }
    deadline = diag_ms() + DIAG_SECONDS * 1000u;
    for (i = 0; i < 200 && diag_ms() < deadline; i++) {
        int frame_done = 0, fid = 0;
        int got;
        t_read = diag_ms();
        got = vd_uvc_read_payload(&s, g_payload, DIAG_MAX_PAYLOAD,
                                  &frame_done, &fid);
        if (got < 0) {
            if (got == -ETIMEDOUT) {
                timeouts++;
                vd_uvc_recommit(&s);
                continue;
            }
            errors++;
            printf("[diag] VIDEO hard error t=+%llums rc=%d last_errno=%d read_wait=%llums\n",
                   (unsigned long long)(diag_ms() - t_start), got, s.last_errno,
                   (unsigned long long)(diag_ms() - t_read));
            probe_alive(&s);
            print_ugen_nodes("POST-ERROR");
            vd_uvc_stop_stream(&s);
            vd_uvc_session_close(&s);
            session_open = 0;
            if (recoveries >= 1) {
                printf("[diag] VIDEO recovery budget exhausted\n");
                break;
            }
            recoveries++;
            memset(&dev_cur, 0, sizeof(dev_cur));
            if (vd_usb_scan_vita(&dev_cur, 1u) == 0) {
                printf("[diag] VIDEO recovery: no Vita device on rescan\n");
                break;
            }
            printf("[diag] VIDEO recovery: Vita re-found at %s, reopening session\n",
                   dev_cur.path);
            if (video_open(&dev_cur, &s) != 0) {
                printf("[diag] VIDEO recovery: reopen failed\n");
                break;
            }
            session_open = 1;
            continue;
        }
        payloads++;
        if (payloads <= 5)
            first_sizes += (uint32_t)got; /* sample of early payload sizes */
        if (frame_done) {
            frames_done++;
            eof_fids += (uint32_t)fid;
        }
    }

    if (session_open) {
        vd_uvc_stop_stream(&s);
        vd_uvc_session_close(&s);
    }
    print_ugen_nodes("POST-RUN");

    printf("[diag] VIDEO RESULT: payloads=%u frames_complete=%u timeouts=%u errors=%u\n",
           payloads, frames_done, timeouts, errors);
    if (payloads > 0)
        printf("[diag] VIDEO avg first-payload size=%u bytes, fids seen=%u\n",
               first_sizes / (payloads < 5 ? payloads : 5), eof_fids);
    printf("[diag] VIDEO VERDICT: %s\n",
           frames_done > 0 ? "STREAMING OK" :
           (payloads > 0 ? "payloads arriving but no full frames" :
            (timeouts > 0 ? "endpoint idle (gadget not streaming - check VUS Control / screen)"
                          : "HARD ERROR on the endpoint")));
    return frames_done > 0 && errors == 0 ? 0 : 1;
}

/* ---- audio ------------------------------------------------------------- */

static int test_audio(const VdUsbVitaDevice *dev)
{
    VdUacSession s;
    uint32_t reads_ok = 0, pcm_bytes = 0, packets = 0, timeouts = 0, errors = 0;
    int i, rc;

    printf("[diag] --- AUDIO: bounded %ds on isoc IN ---\n", DIAG_SECONDS);
    if (!dev->has_audio || dev->audio_isoc_ep == 0) {
        printf("[diag] AUDIO: no isoc endpoint in descriptors - FAIL\n");
        return 1;
    }
    memset(&s, 0, sizeof(s));
    rc = vd_uac_session_open(&s, dev);
    if (rc != 0) {
        printf("[diag] AUDIO: session open FAILED rc=%d\n", rc);
        return 1;
    }
    rc = vd_uac_start(&s, 0);
    if (rc != 0) {
        printf("[diag] AUDIO: start FAILED rc=%d\n", rc);
        vd_uac_session_close(&s);
        return 1;
    }

    printf("[diag] AUDIO identity: fd=%d session=%p eps=%p errno_ptr=%p\n",
           s.fd, (void *)&s, (void *)s.eps, (void *)&errno);
    uint64_t deadline = diag_ms() + DIAG_SECONDS * 1000u;
    for (i = 0; i < 100 && diag_ms() < deadline; i++) {
        uint32_t pcm_len = 0, packet_count = 0;
        rc = vd_uac_read(&s, g_pcm, DIAG_PCM_CAP, &pcm_len, NULL, 0, &packet_count);
        if (rc != 0) {
            if (rc == -ETIMEDOUT) {
                timeouts++;
                continue;
            }
            errors++;
            printf("[diag] AUDIO stopping on hard error: rc=%d\n", rc);
            break;
        }
        reads_ok++;
        pcm_bytes += pcm_len;
        packets += packet_count;
    }

    vd_uac_stop(&s);
    vd_uac_session_close(&s);

    printf("[diag] AUDIO RESULT: reads=%u pcm_bytes=%u packets=%u timeouts=%u errors=%u\n",
           reads_ok, pcm_bytes, packets, timeouts, errors);
    printf("[diag] AUDIO VERDICT: %s\n",
           pcm_bytes > 0 ? "PCM FLOWING" :
           (timeouts > 0 ? "endpoint idle (audio not streaming - check VUS Control audio toggle)"
                         : "HARD ERROR on the endpoint"));
    return pcm_bytes > 0 && errors == 0 ? 0 : 1;
}

static int run_tests(void)
{
    VdUsbVitaDevice dev;
    printf("[diag] THREAD identity: self=%lu errno_ptr=%p\n",
           (unsigned long)(uintptr_t)pthread_self(), (void *)&errno);
    if (open_vita(&dev) != 0) {
        printf("[diag] FATAL: no Vita on USB after retries\n");
        return 1;
    }
    if (!DIAG_AUDIO_ONLY && test_video(&dev) != 0)
        return 1;
    if (!DIAG_VIDEO_ONLY) {
        if (!DIAG_AUDIO_ONLY) {
            memset(&dev, 0, sizeof(dev));
            if (vd_usb_scan_vita(&dev, 1u) == 0) {
                printf("[diag] FATAL: Vita vanished between streams\n");
                return 1;
            }
        }
        if (test_audio(&dev) != 0)
            return 1;
    }
    return 0;
}

static void *worker_main(void *unused)
{
    (void)unused;
    return (void *)(uintptr_t)run_tests();
}

int main(void)
{
    int rc;
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("[diag] VITA5 streamdiag starting rev=%s pid=%d video_only=%d audio_only=%d worker=%d\n",
           VD_BUILD_REV, (int)getpid(), DIAG_VIDEO_ONLY, DIAG_AUDIO_ONLY, DIAG_WORKER);
    printf("[diag] MAIN identity: self=%lu errno_ptr=%p\n",
           (unsigned long)(uintptr_t)pthread_self(), (void *)&errno);
    if (DIAG_WORKER) {
        pthread_t thread;
        void *result = NULL;
        rc = pthread_create(&thread, NULL, worker_main, NULL);
        if (rc != 0) {
            printf("[diag] FATAL: pthread_create rc=%d\n", rc);
            return 1;
        }
        rc = pthread_join(thread, &result);
        if (rc != 0) {
            printf("[diag] FATAL: pthread_join rc=%d\n", rc);
            return 1;
        }
        rc = (int)(uintptr_t)result;
    } else {
        rc = run_tests();
    }
    printf("[diag] DONE %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}
