/* SPDX-License-Identifier: GPL-3.0-or-later
 * HOST INTEGRATION REGRESSION -- ARTIFICIAL USB FIXTURE, NOT HARDWARE OUTPUT.
 * Links the actual core/src/uvc_stream.c and payload/video_producer.c.
 * Only device discovery and USB hardware transport are substituted. In
 * particular vd_uvc_read_payload, session negotiation, frame assembly and
 * ring publication are real. Drive the worker through public start/stop.
 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "usb_transfer.h"
#include "usb_vita.h"
#include "uvc_protocol.h"
#include "uvc_stream.h"
#include "vd_shared.h"
#include "video_producer.h"
#include "vd_shm_prod.h"
#include "vd_ipc.h"
#include "dock_ipc.h"

#define WIDTH 960u
#define HEIGHT 544u
#define INTERVAL 166666u
#define PIXEL_BYTES VD_NV12_FRAME_SIZE(WIDTH, HEIGHT)
#define WIRE_BYTES (UVC_PAYLOAD_HEADER_SIZE + PIXEL_BYTES)
#define FIXTURE_FRAMES VD_VIDEO_SLOTS

_Static_assert(PIXEL_BYTES == 783360u, "fixture must be full 960x544 NV12");

static uint8_t wire[FIXTURE_FRAMES][WIRE_BYTES];
static pthread_mutex_t fixture_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fixture_cond = PTHREAD_COND_INITIALIZER;
static int parked;
static int reopen_mode, partial_mode, stage, advance;
static unsigned partial_step;
#define PARTIAL_PIXELS 4096u
#define B_WIDTH 640u
#define B_HEIGHT 480u
#define B_PIXELS VD_NV12_FRAME_SIZE(B_WIDTH, B_HEIGHT)
/* These are worker-only until stop() joins the worker. */
static unsigned delivered_frames, offset, bulk_calls;
static unsigned opens, closes, releases, probe_sets, commit_sets;
static unsigned probe_gets, commit_gets, detaches, alt_sets;
/* Inject transport failures only in the direct reader checks after stop/join. */
static int injected_read_error;

static unsigned fixture_pixels(void)
{
    return reopen_mode && opens >= 2 ? B_PIXELS : PIXEL_BYTES;
}

/* Worker parks at a hardware boundary, establishing a happens-before edge
 * for ring/server inspection without concurrently reading producer writes. */
static void fixture_park(int target)
{
    pthread_mutex_lock(&fixture_lock);
    stage = target;
    pthread_cond_broadcast(&fixture_cond);
    while (advance < target)
        pthread_cond_wait(&fixture_cond, &fixture_lock);
    pthread_mutex_unlock(&fixture_lock);
}

static uint8_t pixel_byte(unsigned frame, unsigned index)
{
    return (uint8_t)(0x31u + frame * 53u + index * 17u + index / 251u);
}

size_t vd_usb_scan_vita(VdUsbVitaDevice *devices, size_t capacity)
{
    assert(devices != NULL && capacity > 0);
    VdUsbVitaDevice *d = &devices[0];
    memset(d, 0, sizeof(*d));
    strcpy(d->path, "/dev/null"); /* real session_open owns/closes this fd */
    d->is_vita = d->has_video = 1;
    d->video_in_count = 1;
    d->video_in_eps[0] = 0x81;
    d->uvc.interface_number = 1;
    d->uvc.endpoint_address = 0x81;
    d->uvc.format_count = 1;
    VdUvcFormat *fmt = &d->uvc.formats[0];
    fmt->format_index = 1;
    fmt->is_nv12 = 1;
    fmt->frame_count = 1;
    fmt->frames[0] = (VdUvcFrame){.frame_index = 1,
                                  .width = WIDTH,
                                  .height = HEIGHT,
                                  .max_frame_size = PIXEL_BYTES,
                                  .default_interval = INTERVAL};
    if (reopen_mode && opens >= 1)
    {
        fmt->frames[0].width = B_WIDTH;
        fmt->frames[0].height = B_HEIGHT;
        fmt->frames[0].max_frame_size = B_PIXELS;
    }
    return 1;
}

int usb_transfer_open_endpoint(int fd, struct usb_fs_endpoint *eps, unsigned count, uint8_t index,
                               uint8_t ep, int iface, uint32_t capacity, uint32_t *maxpkt)
{
    assert(fd >= 0 && eps != NULL && count == 2);
    assert(index == VD_USB_EP_INDEX_VIDEO_IN && ep == 0x81 && iface == 1);
    assert(capacity >=
               UVC_PAYLOAD_HEADER_SIZE + (reopen_mode && opens >= 1 ? B_PIXELS : PIXEL_BYTES) &&
           maxpkt != NULL);
    ++opens;
    *maxpkt = 512;
    return 0;
}

static void check_control(const VdUvcStreamingControl *p)
{
    assert(p->bFormatIndex == 1 && p->bFrameIndex == 1);
    assert(p->dwFrameInterval == INTERVAL);
    assert(p->dwMaxVideoFrameSize == fixture_pixels());
    assert(p->dwMaxPayloadTransferSize == UVC_PAYLOAD_HEADER_SIZE + fixture_pixels());
}

int usb_uvc_ctrl_xfer(int fd, uint8_t iface, uint8_t request, uint8_t selector, const void *out,
                      uint16_t out_len, void *in, uint16_t in_len)
{
    assert(fd >= 0 && iface == 1 && opens >= 1);
    assert(selector == UVC_VS_PROBE_CONTROL || selector == UVC_VS_COMMIT_CONTROL);
    if (request == UVC_SET_CUR)
    {
        assert(out != NULL && out_len == sizeof(VdUvcStreamingControl));
        assert(in == NULL && in_len == 0);
        check_control(out);
        if (selector == UVC_VS_PROBE_CONTROL)
            ++probe_sets;
        else
            ++commit_sets;
    }
    else
    {
        assert(request == UVC_GET_CUR && out == NULL && out_len == 0);
        assert(in != NULL && in_len == sizeof(VdUvcStreamingControl));
        /* Artificial device negotiation: format1/frame1, 960x544 NV12. */
        VdUvcStreamingControl p = {.bFormatIndex = 1,
                                   .bFrameIndex = 1,
                                   .dwFrameInterval = INTERVAL,
                                   .dwMaxVideoFrameSize = PIXEL_BYTES,
                                   .dwMaxPayloadTransferSize = WIRE_BYTES,
                                   .bPreferedVersion = 1};
        p.dwMaxVideoFrameSize = fixture_pixels();
        p.dwMaxPayloadTransferSize = UVC_PAYLOAD_HEADER_SIZE + fixture_pixels();
        memcpy(in, &p, sizeof(p));
        if (selector == UVC_VS_PROBE_CONTROL)
            ++probe_gets;
        else
            ++commit_gets;
    }
    return 0;
}

int usb_bulk_read(int fd, struct usb_fs_endpoint *ep, uint8_t *buf, uint32_t capacity,
                  uint32_t *got)
{
    assert(fd >= 0 && ep != NULL && buf != NULL && got != NULL);
    if (injected_read_error != 0)
    {
        *got = 0;
        return injected_read_error;
    }
    assert(commit_sets >= 1 && capacity > 0 && capacity <= 65536u);
    if (partial_mode && delivered_frames == FIXTURE_FRAMES)
    {
        if (partial_step == 0)
        {
            buf[0] = UVC_PAYLOAD_HEADER_SIZE;
            buf[1] = UVC_STREAM_EOH | (partial_mode == 1 ? UVC_STREAM_EOF : 0);
            memset(buf + 2, 0, UVC_PAYLOAD_HEADER_SIZE - 2u);
            for (unsigned i = 0; i < PARTIAL_PIXELS; ++i)
                buf[UVC_PAYLOAD_HEADER_SIZE + i] = pixel_byte(9, i);
            *got = UVC_PAYLOAD_HEADER_SIZE + PARTIAL_PIXELS;
            ++partial_step;
            return 0;
        }
        if (partial_step == 1)
        {
            *got = 0; /* actual reader completes the short payload */
            ++partial_step;
            return 0;
        }
        if (partial_step == 2 && partial_mode == 2)
        {
            *got = 0; /* capture has already copied non-EOF pixels */
            ++partial_step;
            return -EIO;
        }
        if (partial_step <= 3)
        {
            fixture_park(1); /* EOF drop or read-error abandon has happened */
            partial_step = 4;
        }
    }
    if (partial_mode && delivered_frames == FIXTURE_FRAMES + 1u && advance < 2)
        fixture_park(2);
    if (reopen_mode && delivered_frames == FIXTURE_FRAMES && opens == 1)
    {
        *got = 0;
        return -EIO; /* eight real capture errors force close/rescan/open */
    }
    if (reopen_mode && opens == 2 && delivered_frames == FIXTURE_FRAMES && offset == 0)
        fixture_park(1);
    if (reopen_mode && opens == 3 && advance < 3)
        fixture_park(3);
    if (reopen_mode && delivered_frames == FIXTURE_FRAMES + 1u && advance < 2)
        fixture_park(2);
    if (delivered_frames == FIXTURE_FRAMES + (reopen_mode || partial_mode ? 1u : 0u))
    {
        /* Entering the NEXT read proves the preceding frame passed through
         * real capture_loop. Signal via a mutex, not racy ring polling. A
         * bounded hardware-idle simulation lets public stop() join normally.
         * Repeated idle reads are also bounded if the main thread is delayed.
         */
        pthread_mutex_lock(&fixture_lock);
        parked = 1;
        pthread_cond_signal(&fixture_cond);
        pthread_mutex_unlock(&fixture_lock);
        struct timespec idle = {.tv_sec = 0, .tv_nsec = 100000000L};
        (void)nanosleep(&idle, NULL);
        *got = 0;
        return -EIO;
    }
    unsigned frame_wire_bytes = UVC_PAYLOAD_HEADER_SIZE + fixture_pixels();
    unsigned remaining = frame_wire_bytes - offset;
    unsigned n = capacity < remaining ? capacity : remaining;
    memcpy(buf, wire[delivered_frames % FIXTURE_FRAMES] + offset, n);
    *got = n;
    offset += n;
    ++bulk_calls;
    if (offset == frame_wire_bytes)
    {
        offset = 0;
        ++delivered_frames;
    }
    return 0;
}

/* Linker wrapping is confined to the USB teardown ioctls; real open/close
 * and all producer/session functions remain untouched. */
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd >= 0);
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == USB_IFACE_DRIVER_DETACH)
    {
        assert(*(int *)arg == 1);
        ++detaches;
    }
    else
    {
        assert(request == USB_SET_ALTINTERFACE);
        const struct usb_alt_interface *alt = arg;
        assert(alt->uai_interface_index == 1 && alt->uai_alt_index == 0);
        ++alt_sets;
    }
    return 0;
}

void usb_transfer_close_endpoint(int fd, struct usb_fs_endpoint *eps, unsigned count, uint8_t index)
{
    assert(fd >= 0 && eps != NULL && count == 2);
    assert(index == VD_USB_EP_INDEX_VIDEO_IN);
    ++closes;
}

void usb_transfer_release_after_uninit(struct usb_fs_endpoint *ep)
{
    assert(ep != NULL && closes >= 1);
    ++releases;
}

static unsigned failures;
#define CHECK(condition, message)                                                                  \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "FAIL: %s\n", message);                                                \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static int test_baseline(void)
{
    /* Even a broken stop/join is bounded independently of the shell timeout. */
    alarm(5);
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned f = 0; f < FIXTURE_FRAMES; ++f)
    {
        memset(wire[f], 0xa5, UVC_PAYLOAD_HEADER_SIZE);
        wire[f][0] = UVC_PAYLOAD_HEADER_SIZE;
        wire[f][1] = UVC_STREAM_EOH | UVC_STREAM_EOF | (f & UVC_STREAM_FID);
        for (unsigned i = 0; i < PIXEL_BYTES; ++i)
            wire[f][UVC_PAYLOAD_HEADER_SIZE + i] = pixel_byte(f, i);
    }
    VdSharedHeader *hdr = calloc(1, VD_SHM_TOTAL_BYTES);
    assert(hdr != NULL);
    /* Non-pixel sentinel makes a missing tail unmistakable. */
    memset(VD_SHM_VIDEO_PTR(hdr, 0), 0xcc, VD_SHM_VIDEO_BYTES);
    assert(vd_video_producer_start(hdr, VD_SHM_TOTAL_BYTES) == 0);

    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&fixture_lock);
    int rc = 0;
    while (!parked && rc == 0)
        rc = pthread_cond_timedwait(&fixture_cond, &fixture_lock, &deadline);
    int saw_parked = parked;
    pthread_mutex_unlock(&fixture_lock);
    vd_video_producer_stop(); /* all ring reads below are after the join */

    printf("ARTIFICIAL USB: %ux%u NV12, pixel_bytes=%u header_bytes=%u "
           "delivered=%u bulk_calls=%u\n",
           WIDTH, HEIGHT, PIXEL_BYTES, UVC_PAYLOAD_HEADER_SIZE, delivered_frames, bulk_calls);
    printf("RING: captured=%u dropped=%u producer=%u seq0=%u seq1=%u\n", hdr->frames_captured,
           hdr->frames_dropped, hdr->video_producer, hdr->video_slot_seq[0],
           hdr->video_slot_seq[1]);
    CHECK(saw_parked, "worker must consume fixture within 3 seconds");
    CHECK(delivered_frames == FIXTURE_FRAMES, "all fixture frames delivered");
    CHECK(bulk_calls == FIXTURE_FRAMES * ((WIRE_BYTES + 65535u) / 65536u),
          "real reader must reassemble whole frames in bounded USB chunks");
    CHECK(hdr->frames_captured > 0, "real producer must capture full NV12 frames");
    CHECK(hdr->frames_captured == FIXTURE_FRAMES, "both complete frames published");
    CHECK(hdr->frames_dropped == 0, "complete frames must not be dropped");
    CHECK(hdr->video_width == WIDTH && hdr->video_height == HEIGHT &&
              hdr->video_pixel_format == VD_PIX_NV12,
          "negotiated ring metadata");
    CHECK(hdr->video_producer == FIXTURE_FRAMES % VD_VIDEO_SLOTS,
          "producer advances across both ring slots");
    CHECK(hdr->stream_active == 0, "stop must clear stream_active");
    CHECK(opens == 1 && closes == 1 && releases == 1 && detaches == 1 && alt_sets == 1,
          "session teardown completes exactly once");
    CHECK(probe_sets == 1 && commit_sets == 1 && probe_gets == 1 && commit_gets == 1,
          "real probe/commit handshake");
    for (unsigned slot = 0; slot < VD_VIDEO_SLOTS; ++slot)
    {
        uint8_t *pixels = VD_SHM_VIDEO_PTR(hdr, slot);
        printf("SLOT %u: seq=%u first=0x%02x expected=0x%02x "
               "last=0x%02x expected=0x%02x\n",
               slot, hdr->video_slot_seq[slot], pixels[0], pixel_byte(slot, 0),
               pixels[PIXEL_BYTES - 1u], pixel_byte(slot, PIXEL_BYTES - 1u));
        CHECK((hdr->video_slot_seq[slot] & 1u) == 0, "no odd slot sequence after stop");
        CHECK(hdr->video_slot_seq[slot] != 0, "complete frame has nonzero ready sequence");
        CHECK(pixels[0] == pixel_byte(slot, 0), "first pixel excludes UVC header");
        CHECK(pixels[PIXEL_BYTES - 1u] == pixel_byte(slot, PIXEL_BYTES - 1u),
              "last pixel copied (no double subtraction of header length)");
        CHECK(memcmp(pixels, wire[slot] + UVC_PAYLOAD_HEADER_SIZE, PIXEL_BYTES) == 0,
              "entire published frame matches artificial USB pixel bytes");
    }
    /* Actual reader, artificial transport: do not collapse an idle timeout or
     * a vanished endpoint into the same opaque -2 error. No hardware crash is
     * simulated or claimed by these checks. */
    const int read_errors[] = {-ETIMEDOUT, -ENXIO, -EINTR, -EBUSY, -EIO};
    VdUvcSession failed_session = {.fd = 1, .frame_bytes = WIRE_BYTES};
    uint8_t failed_payload[UVC_PAYLOAD_HEADER_SIZE];
    for (unsigned i = 0; i < sizeof(read_errors) / sizeof(read_errors[0]); ++i)
    {
        int done = 7;
        int fid = 7;
        injected_read_error = read_errors[i];
        int reader_rc = vd_uvc_read_payload(&failed_session, failed_payload, sizeof(failed_payload),
                                            &done, &fid);
        printf("ARTIFICIAL ERROR: transport=%d reader=%d\n", injected_read_error, reader_rc);
        CHECK(reader_rc == injected_read_error, "reader preserves transport errno");
        CHECK(done == 0 && fid == 0, "failed read never publishes EOF or FID");
    }
    injected_read_error = 0;
    free(hdr);
    alarm(0);
    printf("video producer integration: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void wait_stage(int target)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&fixture_lock);
    int rc = 0;
    while (stage < target && rc == 0)
        rc = pthread_cond_timedwait(&fixture_cond, &fixture_lock, &deadline);
    assert(stage >= target);
    pthread_mutex_unlock(&fixture_lock);
}

static void advance_stage(int target)
{
    pthread_mutex_lock(&fixture_lock);
    advance = target;
    pthread_cond_broadcast(&fixture_cond);
    pthread_mutex_unlock(&fixture_lock);
}

typedef struct
{
    VdDockIpc *server;
    int fd;
    VdIpcRx rx;
    uint8_t *body;
    unsigned videos;
} TestPeer;

static TestPeer attach_peer(VdSharedHeader *hdr)
{
    TestPeer p = {0};
    uint16_t port = 0;
    p.server = vd_dock_ipc_open(hdr, VD_SHM_TOTAL_BYTES, 0, &port);
    assert(p.server != NULL && port != 0);
    p.fd = vd_ipc_connect(port);
    assert(p.fd >= 0);
    p.body = malloc(VD_IPC_MAX_BODY);
    assert(p.body != NULL);
    uint8_t hello[VD_IPC_HELLO_BYTES], packet[VD_IPC_HEADER_BYTES + VD_IPC_HELLO_BYTES];
    vd_ipc_make_hello(hello, "artificial-usb-video-test", 1u);
    size_t length = vd_ipc_packet(packet, sizeof(packet), VD_IPC_HELLO, hello, sizeof(hello));
    size_t sent = 0;
    uint64_t deadline = vd_ipc_now_ms() + 1000u;
    int rc = 0;
    while (rc == 0 && vd_ipc_now_ms() < deadline)
    {
        assert(vd_dock_ipc_tick(p.server, vd_ipc_now_ms()) >= 0);
        rc = vd_ipc_send(p.fd, packet, length, &sent);
        assert(rc >= 0);
        usleep(1000);
    }
    assert(rc == 1);
    return p;
}

/* Exercise real nonblocking loopback framing, including whole-frame bytes. */
static void pump_peer(TestPeer *p, unsigned width, unsigned height, const uint8_t *expected)
{
    uint64_t deadline = vd_ipc_now_ms() + 150u;
    do
    {
        assert(vd_dock_ipc_tick(p->server, vd_ipc_now_ms()) >= 0);
        int rc = vd_ipc_receive(p->fd, &p->rx, p->body, VD_IPC_MAX_BODY);
        assert(rc >= 0);
        if (rc == 1)
        {
            if (p->rx.type == VD_IPC_VIDEO)
            {
                ++p->videos;
                CHECK(expected != NULL, "fresh peer must receive no retired/partial video");
                if (expected)
                {
                    size_t bytes = VD_NV12_FRAME_SIZE(width, height);
                    CHECK(vd_ipc_get_u32(p->body) == width &&
                              vd_ipc_get_u32(p->body + 4) == height &&
                              vd_ipc_get_u32(p->body + 8) == VD_PIX_NV12,
                          "loopback frame geometry matches its bytes");
                    CHECK(p->rx.length == VD_IPC_VIDEO_META_BYTES + bytes,
                          "loopback whole frame size");
                    CHECK(memcmp(p->body + VD_IPC_VIDEO_META_BYTES, expected, bytes) == 0,
                          "loopback full-byte frame comparison");
                }
            }
            vd_ipc_rx_reset(&p->rx);
        }
        usleep(1000);
    } while (vd_ipc_now_ms() < deadline);
}

static void close_peer(TestPeer *p)
{
    close(p->fd);
    vd_dock_ipc_close(p->server);
    free(p->body);
}

static int test_reopen(void)
{
    alarm(8);
    reopen_mode = 1;
    for (unsigned f = 0; f < FIXTURE_FRAMES; ++f)
    {
        wire[f][0] = UVC_PAYLOAD_HEADER_SIZE;
        wire[f][1] = UVC_STREAM_EOH | UVC_STREAM_EOF;
        for (unsigned i = 0; i < PIXEL_BYTES; ++i)
            wire[f][UVC_PAYLOAD_HEADER_SIZE + i] = pixel_byte(f, i);
    }
    VdSharedHeader *hdr = calloc(1, VD_SHM_TOTAL_BYTES);
    assert(hdr != NULL && vd_shm_prod_init(hdr, VD_SHM_TOTAL_BYTES) == 0);
    assert(vd_video_producer_start(hdr, VD_SHM_TOTAL_BYTES) == 0);
    wait_stage(1); /* actual close/rescan/open/commit B, before B's first bytes */
    CHECK(opens == 2 && closes == 1 && releases == 1, "real UVC reopen and teardown");
    CHECK(hdr->frames_captured == FIXTURE_FRAMES && hdr->video_width == B_WIDTH &&
              hdr->video_height == B_HEIGHT,
          "B negotiated before first B frame");
    for (unsigned slot = 0; slot < VD_VIDEO_SLOTS; ++slot)
    {
        CHECK(memcmp(VD_SHM_VIDEO_PTR(hdr, slot), wire[slot] + UVC_PAYLOAD_HEADER_SIZE,
                     PIXEL_BYTES) == 0,
              "A complete bytes retained but unavailable");
        CHECK((hdr->video_slot_seq[slot] & 1u) != 0,
              "session change retires every A slot before geometry mutation");
        CHECK(hdr->video_slot_seq[slot] > 2u, "retirement advances without sequence-reset ABA");
    }
    TestPeer peer = attach_peer(hdr);
    pump_peer(&peer, 0, 0, NULL);
    CHECK(peer.videos == 0, "reopen must never label retained A bytes with B geometry");
    for (unsigned i = 0; i < B_PIXELS; ++i)
        wire[0][UVC_PAYLOAD_HEADER_SIZE + i] = pixel_byte(7, i);
    advance_stage(1);
    wait_stage(2);
    pump_peer(&peer, B_WIDTH, B_HEIGHT, wire[0] + UVC_PAYLOAD_HEADER_SIZE);
    CHECK(peer.videos == 1, "existing peer receives exactly one complete B frame");
    close_peer(&peer);
    peer = attach_peer(hdr);
    pump_peer(&peer, B_WIDTH, B_HEIGHT, wire[0] + UVC_PAYLOAD_HEADER_SIZE);
    CHECK(peer.videos == 1, "fresh peer receives B, not retained A");
    close_peer(&peer);
    advance_stage(2);
    vd_video_producer_stop();
    CHECK(opens == 2 && closes == 2 && releases == 2, "both actual sessions close once");
    uint32_t before_restart[VD_VIDEO_SLOTS];
    for (unsigned i = 0; i < VD_VIDEO_SLOTS; ++i)
        before_restart[i] = hdr->video_slot_seq[i];
    assert(vd_video_producer_start(hdr, VD_SHM_TOTAL_BYTES) == 0);
    wait_stage(3); /* same storage, successful restart, no new frame */
    for (unsigned i = 0; i < VD_VIDEO_SLOTS; ++i)
        CHECK((hdr->video_slot_seq[i] & 1u) && hdr->video_slot_seq[i] > before_restart[i],
              "public stop/start preserves generations and invalidates all retained slots");
    peer = attach_peer(hdr);
    pump_peer(&peer, 0, 0, NULL);
    CHECK(peer.videos == 0, "restart before first new frame exposes no retained bytes");
    close_peer(&peer);
    advance_stage(3);
    vd_video_producer_stop();
    CHECK(opens == 3 && closes == 3 && releases == 3, "restart session closes once");
    free(hdr);
    alarm(0);
    printf("ARTIFICIAL USB reopen + real loopback: %s (%u failures)\n", failures ? "FAIL" : "PASS",
           failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int test_partial(int mode)
{
    alarm(8);
    partial_mode = mode;
    for (unsigned f = 0; f < FIXTURE_FRAMES; ++f)
    {
        wire[f][0] = UVC_PAYLOAD_HEADER_SIZE;
        wire[f][1] = UVC_STREAM_EOH | UVC_STREAM_EOF;
        for (unsigned i = 0; i < PIXEL_BYTES; ++i)
            wire[f][UVC_PAYLOAD_HEADER_SIZE + i] = pixel_byte(f, i);
    }
    VdSharedHeader *hdr = calloc(1, VD_SHM_TOTAL_BYTES);
    assert(hdr != NULL && vd_shm_prod_init(hdr, VD_SHM_TOTAL_BYTES) == 0);
    assert(vd_video_producer_start(hdr, VD_SHM_TOTAL_BYTES) == 0);
    wait_stage(1);
    CHECK(hdr->frames_captured == FIXTURE_FRAMES && hdr->frames_dropped == 1,
          "partial frame dropped, never captured");
    uint32_t invalid = hdr->video_slot_seq[0];
    CHECK((invalid & 1u) != 0, "partially overwritten ready slot stays invalid odd");
    CHECK(invalid > hdr->video_slot_seq[1], "abandon cannot restore old ready generation");
    uint8_t *expected_partial = malloc(PIXEL_BYTES);
    assert(expected_partial != NULL);
    memcpy(expected_partial, wire[0] + UVC_PAYLOAD_HEADER_SIZE, PIXEL_BYTES);
    for (unsigned i = 0; i < PARTIAL_PIXELS; ++i)
        expected_partial[i] = pixel_byte(9, i);
    CHECK(memcmp(VD_SHM_VIDEO_PTR(hdr, 0), expected_partial, PIXEL_BYTES) == 0,
          "full-byte check proves partial overwrite really reached the ring");
    free(expected_partial);
    TestPeer peer = attach_peer(hdr);
    pump_peer(&peer, WIDTH, HEIGHT, wire[1] + UVC_PAYLOAD_HEADER_SIZE);
    CHECK(peer.videos == 1, "fresh peer gets only untouched complete slot, not partial");
    close_peer(&peer);
    for (unsigned i = 0; i < PIXEL_BYTES; ++i)
        wire[0][UVC_PAYLOAD_HEADER_SIZE + i] = pixel_byte(11, i);
    advance_stage(1);
    wait_stage(2);
    CHECK(hdr->video_slot_seq[0] > invalid && !(hdr->video_slot_seq[0] & 1u),
          "next full frame releases a newer even generation from abandoned odd");
    CHECK(memcmp(VD_SHM_VIDEO_PTR(hdr, 0), wire[0] + UVC_PAYLOAD_HEADER_SIZE, PIXEL_BYTES) == 0,
          "recovered full frame replaces every partial byte");
    peer = attach_peer(hdr);
    pump_peer(&peer, WIDTH, HEIGHT, wire[0] + UVC_PAYLOAD_HEADER_SIZE);
    CHECK(peer.videos == 1, "fresh peer gets complete recovery frame");
    close_peer(&peer);
    advance_stage(2);
    vd_video_producer_stop();
    free(hdr);
    alarm(0);
    printf("ARTIFICIAL USB partial %s + real loopback: %s (%u failures)\n",
           mode == 1 ? "EOF" : "read error", failures ? "FAIL" : "PASS", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int test_helper(void)
{
    VdSharedHeader *hdr = calloc(1, VD_SHM_TOTAL_BYTES);
    uint8_t *frame = malloc(PIXEL_BYTES);
    assert(hdr != NULL && frame != NULL);
    assert(vd_shm_prod_init(hdr, VD_SHM_TOTAL_BYTES) == 0);
    for (unsigned f = 0; f < VD_VIDEO_SLOTS; ++f)
    {
        for (unsigned i = 0; i < PIXEL_BYTES; ++i)
            frame[i] = pixel_byte(f, i);
        assert(vd_shm_prod_publish_video(hdr, WIDTH, HEIGHT, VD_PIX_NV12, frame, PIXEL_BYTES) == 0);
    }
    uint32_t old[VD_VIDEO_SLOTS];
    for (unsigned i = 0; i < VD_VIDEO_SLOTS; ++i)
        old[i] = hdr->video_slot_seq[i];
    for (unsigned i = 0; i < B_PIXELS; ++i)
        frame[i] = pixel_byte(7, i);
    assert(vd_shm_prod_publish_video(hdr, B_WIDTH, B_HEIGHT, VD_PIX_NV12, frame, B_PIXELS) == 0);
    CHECK((hdr->video_slot_seq[1] & 1u) && hdr->video_slot_seq[1] > old[1],
          "helper geometry change retires every other old-geometry frame");
    CHECK(!(hdr->video_slot_seq[0] & 1u) && hdr->video_slot_seq[0] > old[0],
          "helper releases full B with a newer even sequence");
    CHECK(memcmp(VD_SHM_VIDEO_PTR(hdr, 0), frame, B_PIXELS) == 0, "helper full-byte B comparison");
    TestPeer peer = attach_peer(hdr);
    pump_peer(&peer, B_WIDTH, B_HEIGHT, frame);
    CHECK(peer.videos == 1, "helper B arrives over actual loopback server");
    close_peer(&peer);
    uint32_t retired = hdr->video_slot_seq[1];
    for (unsigned i = 0; i < B_PIXELS; ++i)
        frame[i] = pixel_byte(11, i);
    assert(vd_shm_prod_publish_video(hdr, B_WIDTH, B_HEIGHT, VD_PIX_NV12, frame, B_PIXELS) == 0);
    CHECK(!(hdr->video_slot_seq[1] & 1u) && hdr->video_slot_seq[1] > retired,
          "helper can fully publish into previously retired odd slot");
    CHECK(memcmp(VD_SHM_VIDEO_PTR(hdr, 1), frame, B_PIXELS) == 0,
          "helper retired slot full-byte recovery comparison");
    const uint8_t *tail = VD_SHM_VIDEO_PTR(hdr, 1);
    int zero_tail = 1;
    for (unsigned i = B_PIXELS; i < VD_SHM_VIDEO_SLOT_SIZE; ++i)
        if (tail[i] != 0)
            zero_tail = 0;
    CHECK(zero_tail, "helper clears the entire unused slot tail");
    peer = attach_peer(hdr);
    pump_peer(&peer, B_WIDTH, B_HEIGHT, frame);
    CHECK(peer.videos == 1, "helper recovery arrives over actual loopback server");
    close_peer(&peer);
    free(frame);
    free(hdr);
    printf("helper geometry + real loopback: %s (%u failures)\n", failures ? "FAIL" : "PASS",
           failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "reopen") == 0)
        return test_reopen();
    if (argc == 2 && strcmp(argv[1], "partial-eof") == 0)
        return test_partial(1);
    if (argc == 2 && strcmp(argv[1], "partial-error") == 0)
        return test_partial(2);
    if (argc == 2 && strcmp(argv[1], "helper") == 0)
        return test_helper();
    return test_baseline();
}
