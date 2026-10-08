// SPDX-License-Identifier: GPL-3.0-or-later
// Offline diagnostic, intentionally RED for beta 3. Exercises the real app
// Shm and payload handoff code with Linux files/processes. This does NOT
// reproduce PS5 unmount/ShellCore behavior or establish console safety.
// Historical beta-3 diagnostic; retained for comparison at commit 593f889.
// Current regression is tests/test_dock_lifecycle.cpp, run by the same script.
// Do not compile this fixture against the replacement IPC/RAM contract.
#if 0
#ifdef VD5_HOST_SHUTDOWN_REPRO
#include "vd/vd_shm.hpp"
#include "vd_shm_prod.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int ring_fd = -1;
static unsigned long sleep_us = 0;
static int failures = 0;

extern "C" int __real_open(const char *, int, ...);
extern "C" int __wrap_open(const char *path, int flags, ...)
{
    if (std::strcmp(path, "/download0/vd5/vdshm.bin") == 0)
        return dup(ring_fd); // redirect the app pathname to a REAL shared file
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0)
    {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    return __real_open(path, flags, mode);
}

extern "C" int __wrap_usleep(useconds_t usec)
{
    // Deterministic clock: close immediately after a payload poll. The next
    // tick is at +1000000 us (main.c's step_ms=1000); do not dispatch early.
    sleep_us += usec;
    return 0;
}

static void require(bool ok, const char *message)
{
    if (!ok)
    {
        std::fprintf(stderr, "HARNESS ERROR: %s errno=%d\n", message, errno);
        std::exit(2);
    }
}

static void expect_released(const VdShmHandoff &h, const char *scenario)
{
    if (h.have_file != 0 || h.fd >= 0)
    {
        ++failures;
        std::printf("FAIL %s: payload still owns ring fd=%d have_file=%d\n", scenario, h.fd,
                    h.have_file);
    }
    else
        std::printf("PASS %s: payload released backing file\n", scenario);
}

int main()
{
    char path[] = "/tmp/vita5-shutdown-repro-XXXXXX";
    ring_fd = mkstemp(path);
    require(ring_fd >= 0, "mkstemp");
    require(ftruncate(ring_fd, VD_SHM_TOTAL_BYTES) == 0, "size ring");
    void *base = mmap(nullptr, VD_SHM_TOTAL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd, 0);
    require(base != MAP_FAILED, "map payload ring");
    require(vd_shm_prod_init(base, VD_SHM_TOTAL_BYTES) == 0, "initialize ring");
    struct stat st
    {
    };
    require(fstat(ring_fd, &st) == 0, "stat ring");
    VdShmHandoff h{};
    vd_shm_handoff_init(&h);
    h.fd = ring_fd;
    h.base = base;
    h.bytes = VD_SHM_TOTAL_BYTES;
    h.have_file = 1;
    h.file_dev = st.st_dev;
    h.file_ino = st.st_ino;
    std::snprintf(h.path, sizeof(h.path), "%s", path);
    auto *header = static_cast<VdSharedHeader *>(base);

    // The real app rejects a mismatched contract and forgets the mapping.
    // This is a CANDIDATE explanation for attached=0, not its established
    // cause in klog_2.txt (no runtime header/revision is captured there).
    header->version = VD_SHM_VERSION - 1u;
    {
        vd5::Shm app;
        require(!app.attach(), "app must reject mismatched contract");
        require(!app.attached(), "app must be unattached");
        std::printf("Witness unattached: %s\n", app.last_error());
    }
    require(header->shutdown == 0u, "unattached app sent no shutdown");
    require(vd_shm_handoff_poll(&h) == 1, "payload retains unchanged ring");
    expect_released(h, "unattached-app exit");
    header->version = VD_SHM_VERSION;

    // Graceful app exit, worst phase of the real one-second poll schedule.
    require(vd_shm_handoff_poll(&h) == 1, "poll before graceful exit");
    {
        vd5::Shm app;
        require(app.attach(), "attach for graceful exit");
    }
    std::printf("Witness graceful: app waited %lu us; next payload poll at 1000000 us\n", sleep_us);
    require(header->shutdown == 1u, "graceful destructor requested shutdown");
    expect_released(h, "graceful exit before next poll");
    require(sleep_us < 1000000ul, "beta 3 sleep shorter than poll period");
    // Show the production poll does see the request LATER. No producers
    // are active here; do not infer that live MAP_FIXED replacement is safe.
    require(vd_shm_handoff_poll(&h) == 3, "next poll notices graceful shutdown");
    header->shutdown = 0u;

    // A REAL externally killed child cannot run its C++ destructor. This is
    // Linux SIGKILL, not an emulation of every detail of PS5 suspension.
    int ready[2];
    require(pipe(ready) == 0, "pipe");
    std::fflush(stdout);
    const pid_t child = fork();
    require(child >= 0, "fork");
    if (child == 0)
    {
        close(ready[0]);
        vd5::Shm app;
        if (!app.attach())
            _exit(2);
        const char token = 'R';
        if (write(ready[1], &token, 1) != 1)
            _exit(2);
        for (;;)
            pause();
    }
    close(ready[1]);
    char token = 0;
    require(read(ready[0], &token, 1) == 1 && token == 'R', "child attached");
    close(ready[0]);
    require(kill(child, SIGKILL) == 0, "kill app child");
    int status = 0;
    require(waitpid(child, &status, 0) == child, "wait child");
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "child was killed");
    require(header->shutdown == 0u, "killed app cannot send shutdown");
    require(vd_shm_handoff_poll(&h) == 1, "payload still retains unchanged ring");
    expect_released(h, "externally-killed app exit");

    vd_shm_handoff_close(&h);
    unlink(path);
    std::printf("repro_dock_shutdown: %d unsafe-exit scenarios reproduced (host only)\n", failures);
    return failures == 0 ? 0 : 1;
}
#endif // VD5_HOST_SHUTDOWN_REPRO
#endif // historical fixture
