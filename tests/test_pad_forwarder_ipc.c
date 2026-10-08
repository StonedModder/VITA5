/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real adapter/socket/worker; discovery and synchronous USB are artificial. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "dock_ipc.h"
#include "pad_forwarder.h"
#include "vd_ipc.h"
#include "usb_transfer.h"
#include "usb_vita.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int blocked, entered, absent, fail_touch;
static VdPadReport pads[256];
static VdTouchReport touches[256];
static unsigned npad, ntouch;
static void pause_ms(unsigned ms)
{
    struct timespec t = {ms / 1000u, (long)(ms % 1000u) * 1000000L};
    nanosleep(&t, NULL);
}
int vd_shm_prod_contract_ok(const void *base)
{
    const VdSharedHeader *h = base;
    return h->magic == VD_SHM_MAGIC && h->version == VD_SHM_VERSION;
}
size_t vd_usb_scan_vita(VdUsbVitaDevice *d, size_t cap)
{
    (void)cap;
    pthread_mutex_lock(&lock);
    int missing = absent;
    pthread_mutex_unlock(&lock);
    if (missing)
        return 0;
    memset(d, 0, sizeof(*d));
    strcpy(d->path, "/dev/null");
    return 1;
}
int usb_send_pad_report(int fd, struct usb_fs_endpoint *ep, const VdPadReport *r)
{
    (void)fd;
    (void)ep;
    pthread_mutex_lock(&lock);
    assert(npad < 256);
    pads[npad++] = *r;
    if (blocked)
    {
        entered = 1;
        pthread_cond_broadcast(&wake);
        while (blocked)
            pthread_cond_wait(&wake, &lock);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
int usb_send_touch_report(int fd, const VdTouchReport *t)
{
    (void)fd;
    pthread_mutex_lock(&lock);
    if (fail_touch && t->count == 0)
    {
        fail_touch = 0;
        pthread_mutex_unlock(&lock);
        return -EIO;
    }
    assert(ntouch < 256);
    touches[ntouch++] = *t;
    if (blocked)
    {
        entered = 1;
        pthread_cond_broadcast(&wake);
        while (blocked)
            pthread_cond_wait(&wake, &lock);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
static void packet(int fd, uint32_t kind, const void *b, size_t n)
{
    uint8_t p[128];
    size_t len = vd_ipc_packet(p, sizeof(p), kind, b, n), off = 0;
    assert(len);
    for (unsigned i = 0; i < 1000; ++i)
    {
        int rc = vd_ipc_send(fd, p, len, &off);
        assert(rc >= 0);
        if (rc)
            return;
        pause_ms(1);
    }
    assert(!"send timeout");
}
static void send_pad(int fd, unsigned id)
{
    VdPadReport r;
    uint8_t w[VD_PAD_WIRE_BYTES];
    vd_pad_make_report(&r, id, id, VD_PAD_CROSS, 0, 0, 0, 0, 0, 0);
    assert(vd_pad_serialize(&r, w, sizeof(w)) == sizeof(w));
    packet(fd, VD_IPC_PAD, w, sizeof(w));
}
static void send_touch(int fd, unsigned port, unsigned count)
{
    VdTouchReport t = {0};
    uint8_t w[VD_TOUCH_WIRE_BYTES];
    t.port = (uint8_t)port;
    t.count = (uint8_t)count;
    t.f0_active = (uint8_t)count;
    t.f0_x = 123;
    t.f0_y = 456;
    assert(vd_touch_serialize(&t, w) == sizeof(w));
    packet(fd, VD_IPC_TOUCH, w, sizeof(w));
}
static void ticks(VdDockIpc *s, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
    {
        assert(vd_dock_ipc_tick(s, vd_ipc_now_ms()) == 0);
        pause_ms(1);
    }
}
static void release_worker(void)
{
    pthread_mutex_lock(&lock);
    blocked = 0;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *mode = argv[1];
    VdSharedHeader *h = calloc(1, VD_SHM_TOTAL_BYTES);
    assert(h);
    h->magic = VD_SHM_MAGIC;
    h->version = VD_SHM_VERSION;
    _Static_assert(sizeof(VdSharedHeader) <= VD_SHM_HEADER_SIZE, "header overflow");
    uint16_t port;
    VdDockIpc *s = vd_dock_ipc_open(h, VD_SHM_TOTAL_BYTES, 0, &port);
    assert(s);
    absent = strcmp(mode, "absent") == 0 || strcmp(mode, "saturation") == 0 ||
             strcmp(mode, "reconnect") == 0;
    fail_touch = strcmp(mode, "failure") == 0;
    assert(vd_pad_forwarder_start(h, VD_SHM_TOTAL_BYTES) == 0);
    pause_ms(30); /* Worker primes before first input. */
    int fd = vd_ipc_connect(port);
    assert(fd >= 0);
    ticks(s, 3);
    uint8_t hello[VD_IPC_HELLO_BYTES];
    vd_ipc_make_hello(hello, "artificial-pad-test", 1);
    packet(fd, VD_IPC_HELLO, hello, sizeof(hello));
    ticks(s, 3);
    if (strcmp(mode, "reset") == 0 || strcmp(mode, "overrun") == 0)
    {
        pthread_mutex_lock(&lock);
        blocked = 1;
        pthread_mutex_unlock(&lock);
        send_pad(fd, 1);
        for (unsigned i = 0; i < 1000; ++i)
        {
            ticks(s, 1);
            pthread_mutex_lock(&lock);
            int ready = entered;
            pthread_mutex_unlock(&lock);
            if (ready)
                break;
        }
        assert(entered);
        unsigned count = strcmp(mode, "reset") == 0 ? 8u : 12u;
        for (unsigned i = 2; i < count + 2; ++i)
            send_pad(fd, i);
        ticks(s, 8);
        if (strcmp(mode, "reset") == 0)
        {
            close(fd);
            fd = -1;
            ticks(s, 8);
        }
        release_worker();
        ticks(s, 50);
        pthread_mutex_lock(&lock);
        if (strcmp(mode, "reset") == 0)
        {
            int neutral = 0;
            for (unsigned i = 1; i < npad; ++i)
            {
                if (!pads[i].buttons)
                    neutral = 1;
                if (neutral)
                    assert(!pads[i].buttons);
            }
            assert(neutral);
        }
        else
        {
            assert(npad == 9);
            for (unsigned i = 1; i < npad; ++i)
                assert(pads[i].report_id == i + 5u);
        }
        pthread_mutex_unlock(&lock);
    }
    else
    {
        if (strcmp(mode, "saturation") == 0)
        {
            for (unsigned i = 0; i < 80; ++i)
                send_touch(fd, i % 2u, i == 0 ? 1u : i % 2u);
            ticks(s, 40);
            /* Timeout must invalidate queued held states, retain both releases. */
            assert(vd_dock_ipc_tick(s, vd_ipc_now_ms() + VD_IPC_PEER_IDLE_MS) == 0);
        }
        else
        {
            if (strcmp(mode, "slow") == 0)
            {
                pthread_mutex_lock(&lock);
                blocked = 1;
                pthread_mutex_unlock(&lock);
            }
            send_touch(fd, 0, 1);
            if (strcmp(mode, "slow") == 0)
            {
                for (unsigned i = 0; i < 1000; ++i)
                {
                    ticks(s, 1);
                    pthread_mutex_lock(&lock);
                    int ready = entered;
                    pthread_mutex_unlock(&lock);
                    if (ready)
                        break;
                }
                assert(entered);
            }
            send_touch(fd, 0, 0);
            send_touch(fd, 1, 1);
            send_touch(fd, 1, 0);
            ticks(s, 10);
            if (strcmp(mode, "slow") == 0)
                release_worker();
            if (strcmp(mode, "absent") == 0 || strcmp(mode, "reconnect") == 0)
            {
                close(fd);
                fd = -1;
                ticks(s, 5);
                if (strcmp(mode, "reconnect") == 0)
                {
                    fd = vd_ipc_connect(port);
                    assert(fd >= 0);
                    ticks(s, 3);
                    packet(fd, VD_IPC_HELLO, hello, sizeof(hello));
                    ticks(s, 3);
                    send_touch(fd, 1, 1);
                    ticks(s, 3);
                }
            }
        }
        if (strcmp(mode, "absent") == 0 || strcmp(mode, "saturation") == 0 ||
            strcmp(mode, "reconnect") == 0)
        {
            assert(__atomic_load_n(&h->touch_ack_seq, __ATOMIC_ACQUIRE) == 0);
            assert(__atomic_load_n(&h->touch_seq, __ATOMIC_ACQUIRE) == 2);
        }
        pthread_mutex_lock(&lock);
        absent = 0;
        pthread_mutex_unlock(&lock);
        ticks(s, 350);
        pthread_mutex_lock(&lock);
        if (strcmp(mode, "saturation") == 0)
        {
            assert(ntouch >= 2);
            assert(touches[ntouch - 2].port == 0 && touches[ntouch - 2].count == 0);
            assert(touches[ntouch - 1].port == 1 && touches[ntouch - 1].count == 0);
            assert(ntouch == 3); /* outstanding held + two reserved releases */
            assert(touches[0].port == 0 && touches[0].count == 1);
            for (unsigned i = 1; i < ntouch; ++i)
                assert(touches[i].count == 0);
        }
        else if (strcmp(mode, "reconnect") == 0)
        {
            assert(ntouch == 4);
            assert(touches[0].port == 0 && touches[0].count == 1);
            assert(touches[1].port == 0 && touches[1].count == 0);
            assert(touches[2].port == 1 && touches[2].count == 0);
            assert(touches[3].port == 1 && touches[3].count == 1);
        }
        else if (strcmp(mode, "absent") == 0)
        {
            assert(ntouch >= 2);
            assert(touches[ntouch - 2].port == 0 && touches[ntouch - 2].count == 0);
            assert(touches[ntouch - 1].port == 1 && touches[ntouch - 1].count == 0);
        }
        else
        {
            assert(ntouch == 4);
            for (unsigned i = 0; i < 4; ++i)
            {
                assert(touches[i].port == i / 2u);
                assert(touches[i].count == (i % 2u == 0));
            }
        }
        pthread_mutex_unlock(&lock);
    }
    vd_pad_forwarder_stop();
    if (fd >= 0)
        close(fd);
    vd_dock_ipc_close(s);
    free(h);
    printf("pad-forwarder IPC %s: PASS (artificial USB only)\n", mode);
    return 0;
}
