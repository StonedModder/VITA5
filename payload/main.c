/* SPDX-License-Identifier: GPL-3.0-or-later
 * Anonymous USB producer storage and close-independent loopback IPC owner.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include "vd_shared.h"
#include "vd_ipc.h"
#include "vd_shm_prod.h"
#include "dock_ipc.h"
#include "dock_status.h"
#include "video_producer.h"
#include "audio_producer.h"
#include "pad_forwarder.h"
#ifndef VD_BUILD_REV
#define VD_BUILD_REV "unknown"
#endif
static volatile sig_atomic_t g_stop;
static time_t g_started_at;
static uint32_t g_heartbeat;
static int g_status_error;
static void write_status(const VdShmHandoff *handoff, const char *phase)
{
    int saved = errno;
    int rc = vd_dock_status_write("/data/VITA5", handoff, g_started_at, ++g_heartbeat, phase);
    if (rc != 0 && rc != g_status_error)
        printf("[vd5-status] snapshot failed rc=%d\n", rc);
    g_status_error = rc;
    errno = saved;
}
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}
static void wait_ms(unsigned ms)
{
    struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    (void)nanosleep(&ts, NULL);
}
int main(void)
{
    VdShmHandoff handoff;
    VdDockIpc *ipc = NULL;
    int video = 0, audio = 0, pad = 0, result = 1, rc;
    uint16_t port = 0;
    uint64_t last_status;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    vd_shm_handoff_init(&handoff);
    g_started_at = time(NULL);
    printf("[vd5] build=%s RAM=%u wire=%u anonymous/no-sandbox ownership\n", VD_BUILD_REV,
           VD_SHM_VERSION, VD_IPC_VERSION);
    if (vd_shm_handoff_poll(&handoff) < 0)
    {
        printf("[vd5] anonymous ring setup failed errno=%d\n", errno);
        goto done;
    }
    ipc = vd_dock_ipc_open(handoff.base, handoff.bytes, VD_IPC_PORT, &port);
    if (!ipc)
    {
        printf("[vd5] IPC listener failed errno=%d\n", errno);
        goto done;
    }
    printf("[vd5] anonymous ring=%lu bytes fd=%d path=empty; bound 127.0.0.1:%u\n", handoff.bytes,
           handoff.fd, (unsigned)port);
    rc = vd_video_producer_start(handoff.base, handoff.bytes);
    if (rc)
    {
        printf("[vd5] video start failed rc=%d\n", rc);
        goto done;
    }
    video = 1;
    rc = vd_audio_producer_start(handoff.base, handoff.bytes);
    if (rc)
    {
        printf("[vd5] audio start failed rc=%d\n", rc);
        goto done;
    }
    audio = 1;
    rc = vd_pad_forwarder_start(handoff.base, handoff.bytes);
    if (rc)
    {
        printf("[vd5] pad start failed rc=%d\n", rc);
        goto done;
    }
    pad = 1;
    write_status(&handoff, "running");
    last_status = vd_ipc_now_ms();
    result = 0;
    while (!g_stop)
    {
        uint64_t now = vd_ipc_now_ms();
        rc = vd_dock_ipc_tick(ipc, now);
        if (rc < 0)
        {
            printf("[vd5] IPC owner error rc=%d\n", rc);
            result = 1;
            break;
        }
        if (now - last_status >= 1000u)
        {
            write_status(&handoff, "running");
            last_status = now;
        }
        wait_ms(2);
    }
done:
    /* No peer event enters this producer teardown path. */
    if (pad)
        vd_pad_forwarder_stop();
    if (audio)
        vd_audio_producer_stop();
    if (video)
        vd_video_producer_stop();
    vd_dock_ipc_close(ipc);
    vd_shm_prod_fini(handoff.base);
    write_status(&handoff, result ? "failed" : "stopped");
    vd_shm_handoff_close(&handoff);
    printf("[vd5] payload stopped rc=%d\n", result);
    return result;
}
