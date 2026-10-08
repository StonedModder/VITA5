// SPDX-License-Identifier: GPL-3.0-or-later
// Real app, producer storage and local-socket lifecycle regression.
// HOST ONLY: does not emulate PS5 ShellCore, USB or GPU behavior.
#ifdef VD5_HOST_SHUTDOWN_REPRO
#include "vd/vd_shm.hpp"
#include "dock_ipc.h"
#include "vd_ipc.h"
#include "vd_shm_prod.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static unsigned failures, checks;
static VdSharedHeader *fixture_ring;
static VdTouchReport fixture_touch[2]{};
// Artificial successful synchronous USB boundary for this lifecycle harness.
// The separate pad-forwarder suite exercises the actual USB worker/retries.
static void acknowledge_fixture_touch()
{
    const auto seq = __atomic_load_n(&fixture_ring->touch_seq, __ATOMIC_ACQUIRE);
    if ((seq & 1u) || seq == __atomic_load_n(&fixture_ring->touch_ack_seq, __ATOMIC_ACQUIRE))
        return;
    VdTouchReport delivered{};
    if (vd_touch_deserialize(fixture_ring->touch_wire, &delivered) != 1)
        std::abort();
    if (seq != __atomic_load_n(&fixture_ring->touch_seq, __ATOMIC_ACQUIRE))
        return;
    fixture_touch[delivered.port] = delivered;
    __atomic_store_n(&fixture_ring->touch_ack_seq, seq, __ATOMIC_RELEASE);
}
#define CHECK(expr)                                                                                \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(expr))                                                                               \
        {                                                                                          \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL line %u: %s errno=%d\n", (unsigned)__LINE__, #expr, errno); \
        }                                                                                          \
    } while (0)

static void ownership(const VdShmHandoff &h, const void *original)
{
    CHECK(h.fd == -1 && h.have_file == 0 && h.path[0] == '\0');
    CHECK(h.base == original && vd_shm_prod_contract_ok(h.base));
}

static void pump(VdDockIpc *ipc, vd5::Shm *app, unsigned ms)
{
    const uint64_t deadline = vd_ipc_now_ms() + ms;
    do
    {
        CHECK(vd_dock_ipc_tick(ipc, vd_ipc_now_ms()) >= 0);
        acknowledge_fixture_touch();
        if (app)
            app->poll(0.001f);
        usleep(1000);
    } while (vd_ipc_now_ms() < deadline);
}

static bool attach(VdDockIpc *ipc, vd5::Shm &app, uint16_t port)
{
    app.attach(port);
    const uint64_t deadline = vd_ipc_now_ms() + 2000u;
    while (!app.attached() && vd_ipc_now_ms() < deadline)
        pump(ipc, &app, 2);
    return app.attached();
}

static void records(VdShmHandoff &h, VdDockIpc *ipc, vd5::Shm &app)
{
    auto *hdr = static_cast<VdSharedHeader *>(h.base);
    uint8_t frame[24], a[VD_AUDIO_CHUNK_BYTES], b[VD_AUDIO_CHUNK_BYTES];
    std::memset(frame, 0x6d, sizeof(frame));
    std::memset(a, 0x2a, sizeof(a));
    std::memset(b, 0x7b, sizeof(b));
    hdr->vita_detected = 1;
    hdr->stream_active = 1;
    CHECK(vd_shm_prod_publish_video(h.base, 4u, 4u, VD_PIX_NV12, frame, sizeof(frame)) == 0);
    CHECK(vd_shm_prod_publish_audio(h.base, a, sizeof(a)) == 0);
    CHECK(vd_shm_prod_publish_audio(h.base, b, sizeof(b)) == 0);
    pump(ipc, &app, 300);
    std::vector<uint8_t> picture(VD_VIDEO_MAX_FRAME);
    uint32_t w = 0, height = 0;
    CHECK(app.read_video(picture.data(), picture.size(), &w, &height));
    CHECK(w == 4u && height == 4u && std::memcmp(picture.data(), frame, sizeof(frame)) == 0);
    CHECK(app.vita_detected() && app.stream_active());
    uint8_t pcm[VD_AUDIO_CHUNK_BYTES];
    CHECK(app.read_audio(pcm, sizeof(pcm)) && std::memcmp(pcm, a, sizeof(pcm)) == 0);
    CHECK(app.read_audio(pcm, sizeof(pcm)) && std::memcmp(pcm, b, sizeof(pcm)) == 0);
    CHECK(!app.read_audio(pcm, sizeof(pcm)));
    VdShmPadCursor cursor{};
    uint8_t pad[VD_PAD_REPORT_BYTES];
    CHECK(vd_shm_prod_consume_pad(h.base, &cursor, pad) == 0);
    VdPadReport report{};
    vd_pad_make_report(&report, 1234u, 987654u, VD_PAD_CROSS, 12, -34, 56, -78, 3, 4);
    CHECK(app.publish_pad(report));
    VdTouchReport touch{};
    touch.port = 1;
    touch.count = 1;
    touch.f0_active = 1;
    touch.f0_x = 600;
    touch.f0_y = 300;
    CHECK(app.publish_touch(touch));
    pump(ipc, &app, 30);
    CHECK(vd_shm_prod_consume_pad(h.base, &cursor, pad) == 1);
    VdPadReport decoded{};
    CHECK(vd_pad_deserialize(pad, VD_PAD_WIRE_BYTES, &decoded) == 0);
    CHECK(decoded.report_id == report.report_id && decoded.timestamp_us == report.timestamp_us &&
          decoded.buttons == report.buttons && decoded.left_y == report.left_y);
    VdTouchReport decoded_touch{};
    CHECK(vd_touch_deserialize(hdr->touch_wire, &decoded_touch) == 1);
    CHECK(decoded_touch.port == 1u && decoded_touch.count == 1u && decoded_touch.f0_x == 600u);
    std::printf("PASS exact video, ordered PCM, controller and touch over real sockets\n");
}

// A live dock must not look dead because PCM playback is temporarily paused.
// Only the audio consumer is stalled; socket polling and PING continue normally.
static void audio_backpressure(VdShmHandoff &h, VdDockIpc *ipc, vd5::Shm &app)
{
    auto *hdr = static_cast<VdSharedHeader *>(h.base);
    const auto initial_failures = failures;
    const auto initial_overruns = app.stats().audio_overruns;
    uint8_t pcm[VD_AUDIO_CHUNK_BYTES];
    for (unsigned id = 1; id <= VD_AUDIO_SLOTS + 8u; ++id)
    {
        std::memset(pcm, static_cast<int>(id), sizeof(pcm));
        CHECK(vd_shm_prod_publish_audio(h.base, pcm, sizeof(pcm)) == 0);
        pump(ipc, &app, 15);
    }
    uint8_t pixels[24];
    std::memset(pixels, 0x91, sizeof(pixels));
    CHECK(vd_shm_prod_publish_video(h.base, 4u, 4u, VD_PIX_NV12, pixels, sizeof(pixels)) == 0);
    hdr->last_error = VD_ERR_NO_VITA;
    pump(ipc, &app, 2200);
    CHECK(app.attached() && vd_dock_ipc_connected(ipc));
    CHECK(app.service_error() == VD_ERR_NO_VITA);
    std::vector<uint8_t> picture(VD_VIDEO_MAX_FRAME);
    uint32_t width = 0, height = 0;
    CHECK(app.read_video(picture.data(), picture.size(), &width, &height));
    CHECK(width == 4u && height == 4u && std::memcmp(picture.data(), pixels, sizeof(pixels)) == 0);
    CHECK(app.stats().audio_overruns == initial_overruns + 8u);
    // Drop oldest PCM on overrun, retain the newest bounded queue in order.
    for (unsigned id = 9u; id <= VD_AUDIO_SLOTS + 8u; ++id)
    {
        const bool received = app.read_audio(pcm, sizeof(pcm));
        CHECK(received);
        if (!received)
            continue;
        bool exact = true;
        for (const auto byte : pcm)
            exact = exact && byte == id;
        CHECK(exact);
    }
    CHECK(!app.read_audio(pcm, sizeof(pcm)));
    hdr->last_error = VD_ERR_NONE;
    std::printf("%s paused PCM consumer: live video/status, bounded oldest-drop, retained order\n",
                failures == initial_failures ? "PASS" : "FAIL");
}

static void process_lifecycle(VdShmHandoff &h, VdDockIpc *ipc, uint16_t port)
{
    auto *hdr = static_cast<VdSharedHeader *>(h.base);
    void *const original = h.base;
    int ready[2];
    CHECK(pipe(ready) == 0);
    CHECK(fcntl(ready[0], F_SETFL, O_NONBLOCK) == 0);
    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0)
    {
        close(ready[0]);
        vd_dock_ipc_close(ipc); // close inherited duplicate server descriptors
        vd_shm_handoff_close(&h);
        vd5::Shm app;
        app.attach(port);
        bool notified = false;
        const uint64_t deadline = vd_ipc_now_ms() + 10000u;
        while (vd_ipc_now_ms() < deadline)
        {
            app.poll(0.001f);
            if (!app.attached())
                app.attach(port);
            if (app.attached() && !notified)
            {
                VdPadReport report{};
                vd_pad_make_report(&report, 2000u, vd_ipc_now_ms() * 1000u, VD_PAD_CROSS, 0, 0, 0,
                                   0, 0, 0);
                if (app.publish_pad(report))
                {
                    const char token = 'R';
                    if (write(ready[1], &token, 1) != 1)
                        _exit(2);
                    notified = true;
                }
            }
            usleep(1000);
        }
        _exit(2);
    }
    close(ready[1]);
    const uint64_t deadline = vd_ipc_now_ms() + 2000u;
    char token = 0;
    while (token != 'R' && vd_ipc_now_ms() < deadline)
    {
        pump(ipc, nullptr, 2);
        (void)read(ready[0], &token, 1);
    }
    close(ready[0]);
    CHECK(token == 'R');
    pump(ipc, nullptr, 20);
    CHECK(vd_dock_ipc_connected(ipc));
    ownership(h, original);
    CHECK(kill(child, SIGSTOP) == 0);
    int status = 0;
    CHECK(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
    std::vector<uint8_t> frame(VD_VIDEO_MAX_FRAME, 0x44);
    CHECK(vd_shm_prod_publish_video(h.base, 1280u, 720u, VD_PIX_NV12, frame.data(), frame.size()) ==
          0);
    pump(ipc, nullptr, VD_IPC_PEER_IDLE_MS + 200u);
    CHECK(!vd_dock_ipc_connected(ipc));
    ownership(h, original);
    const uint32_t slot = (hdr->pad_producer + VD_PAD_SLOTS - 1u) % VD_PAD_SLOTS;
    VdPadReport neutral{};
    CHECK(vd_pad_deserialize(VD_SHM_PAD_PTR(h.base, slot), VD_PAD_WIRE_BYTES, &neutral) == 0);
    CHECK(neutral.buttons == 0u && neutral.left_x == 0 && neutral.right_x == 0);
    CHECK(fixture_touch[0].count == 0u && fixture_touch[1].count == 0u);
    CHECK(hdr->touch_ack_seq == hdr->touch_seq);
    CHECK(hdr->vita_detected == 1u && hdr->stream_active == 1u);
    std::printf("PASS real SIGSTOP/nonreading peer: bounded reset, neutral input, stable ring\n");
    CHECK(kill(child, SIGCONT) == 0);
    pump(ipc, nullptr, 300);
    CHECK(vd_dock_ipc_connected(ipc));
    ownership(h, original);
    CHECK(kill(child, SIGKILL) == 0);
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
          WTERMSIG(status) == SIGKILL);
    ownership(h, original); // IMMEDIATE, without destructor or payload cleanup tick
    pump(ipc, nullptr, 30);
    CHECK(!vd_dock_ipc_connected(ipc));
    std::printf("PASS real SIGKILL: no sandbox references before/after any cleanup tick\n");
}

int main()
{
    VdShmHandoff h{};
    vd_shm_handoff_init(&h);
    CHECK(vd_shm_handoff_poll(&h) == 1);
    void *const original = h.base;
    if (!original)
        return 2;
    fixture_ring = static_cast<VdSharedHeader *>(h.base);
    ownership(h, original);
    uint16_t port = 0;
    VdDockIpc *ipc = vd_dock_ipc_open(h.base, h.bytes, 0, &port);
    CHECK(ipc != nullptr && port != 0u);
    if (!ipc)
        return 2;
    {
        vd5::Shm app;
        CHECK(attach(ipc, app, port));
        ownership(h, original);
        records(h, ipc, app);
        audio_backpressure(h, ipc, app);
    }
    ownership(h, original); // graceful exit before the next producer tick
    pump(ipc, nullptr, 30);
    CHECK(!vd_dock_ipc_connected(ipc));
    std::printf("PASS graceful exit: no shutdown sleep or release deadline\n");
    {
        vd5::Shm app;
        CHECK(attach(ipc, app, port));
        ownership(h, original);
        app.detach();
    }
    pump(ipc, nullptr, 30);
    std::printf("PASS app reconnect without producer remap/reset\n");
    process_lifecycle(h, ipc, port);
    {
        vd5::Shm app;
        CHECK(attach(ipc, app, port));
        ownership(h, original);
    }
    pump(ipc, nullptr, 30);
    vd_dock_ipc_close(ipc);
    uint16_t unused_port = 0;
    const int temporary = vd_ipc_listen(0, &unused_port);
    CHECK(temporary >= 0);
    close(temporary);
    {
        vd5::Shm app;
        app.attach(unused_port);
        app.poll(0.001f);
        CHECK(!app.attached());
    }
    ownership(h, original);
    std::printf("PASS unattached exit: ownership independent of app attachment\n");
    vd_shm_handoff_close(&h);
    unsigned char resident = 0;
    errno = 0;
    CHECK(mincore(original, 4096u, &resident) == -1 && errno == ENOMEM);
    CHECK(h.base == nullptr && h.fd == -1 && h.have_file == 0);
    std::printf("PASS anonymous mapping cleanup\n");
    std::printf(
        "dock lifecycle: %u checks, %u failures (real host processes; NOT PS5 safety proof)\n",
        checks, failures);
    return failures == 0u ? 0 : 1;
}
#endif
