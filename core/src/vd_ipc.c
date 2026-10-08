/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vd_ipc.h"
#include "pad_passthrough.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

uint32_t vd_ipc_get_u32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8u) | ((uint32_t)b[2] << 16u) |
           ((uint32_t)b[3] << 24u);
}

void vd_ipc_put_u32(void *p, uint32_t value)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)value;
    b[1] = (uint8_t)(value >> 8u);
    b[2] = (uint8_t)(value >> 16u);
    b[3] = (uint8_t)(value >> 24u);
}

size_t vd_ipc_video_bytes(uint32_t w, uint32_t h, uint32_t format)
{
    if (format != VD_PIX_NV12 || w == 0u || h == 0u || w > 1280u || h > 720u ||
        ((w | h) & 1u) != 0u)
        return 0;
    return (size_t)w * (size_t)h * 3u / 2u;
}

void vd_ipc_make_hello(uint8_t out[VD_IPC_HELLO_BYTES], const char *revision, uint32_t pid)
{
    size_t i = 0;
    memset(out, 0, VD_IPC_HELLO_BYTES);
    vd_ipc_put_u32(out, VD_SHM_VERSION);
    vd_ipc_put_u32(out + 4, pid);
    if (revision)
        while (i < 31u && revision[i] != '\0')
        {
            out[8u + i] = (uint8_t)revision[i];
            ++i;
        }
}

static int valid_length(uint32_t type, size_t length)
{
    switch (type)
    {
    case VD_IPC_HELLO:
        return length == VD_IPC_HELLO_BYTES;
    case VD_IPC_STATUS:
        return length == VD_IPC_STATUS_BYTES;
    case VD_IPC_VIDEO:
        return length > VD_IPC_VIDEO_META_BYTES && length <= VD_IPC_MAX_BODY;
    case VD_IPC_AUDIO:
        return length == VD_AUDIO_CHUNK_BYTES;
    case VD_IPC_PAD:
        return length == VD_PAD_WIRE_BYTES;
    case VD_IPC_TOUCH:
        return length == VD_TOUCH_WIRE_BYTES;
    case VD_IPC_PING:
        return length == 0;
    default:
        return 0;
    }
}

int vd_ipc_validate_body(uint32_t type, const void *body, size_t length)
{
    const uint8_t *b = (const uint8_t *)body;
    if (!valid_length(type, length) || (length != 0u && !body))
        return -EPROTO;
    if (type == VD_IPC_HELLO &&
        (vd_ipc_get_u32(b) != VD_SHM_VERSION || b[VD_IPC_HELLO_BYTES - 1u] != 0u))
        return -EPROTO;
    if (type == VD_IPC_VIDEO)
    {
        size_t bytes =
            vd_ipc_video_bytes(vd_ipc_get_u32(b), vd_ipc_get_u32(b + 4), vd_ipc_get_u32(b + 8));
        if (bytes == 0u || length != VD_IPC_VIDEO_META_BYTES + bytes)
            return -EPROTO;
    }
    if (type == VD_IPC_TOUCH)
    {
        unsigned i;
        if (b[0] > 1u || b[1] > 2u || b[12] > 1u || b[13] > 1u)
            return -EPROTO;
        for (i = 0; i < 2u; ++i)
        {
            uint16_t x = (uint16_t)((unsigned)b[4u + i * 4u] | ((unsigned)b[5u + i * 4u] << 8u));
            uint16_t y = (uint16_t)((unsigned)b[6u + i * 4u] | ((unsigned)b[7u + i * 4u] << 8u));
            if (x > 1919u || y > 1087u)
                return -EPROTO;
        }
    }
    return 0;
}

size_t vd_ipc_packet(void *out, size_t capacity, uint32_t type, const void *body, size_t length)
{
    uint8_t *b = (uint8_t *)out;
    if (!out || length > VD_IPC_MAX_BODY || capacity < VD_IPC_HEADER_BYTES + length ||
        vd_ipc_validate_body(type, body, length) != 0)
        return 0;
    vd_ipc_put_u32(b, VD_IPC_MAGIC);
    vd_ipc_put_u32(b + 4, VD_IPC_VERSION);
    vd_ipc_put_u32(b + 8, type);
    vd_ipc_put_u32(b + 12, (uint32_t)length);
    if (length != 0u)
        memmove(b + VD_IPC_HEADER_BYTES, body, length);
    return VD_IPC_HEADER_BYTES + length;
}

static int configure_socket(int fd)
{
    int enabled = 1;
    int send_bytes = 65536;
    int receive_bytes = 262144;
#if defined(__PROSPERO__)
    /* Console native sockets do not reliably accept libc fcntl; this
     * explicit nonblocking option is documented by the stock template. */
    if (setsockopt(fd, SOL_SOCKET, 0x1200, &enabled, sizeof(enabled)) != 0)
        return -errno;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        return -errno;
#endif
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_bytes, sizeof(send_bytes)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_bytes, sizeof(receive_bytes)) != 0)
        return -errno;
    return 0;
}

static int close_error(int fd, int error)
{
    close(fd);
    return error;
}

static void loopback_address(struct sockaddr_in *addr, uint16_t port)
{
    memset(addr, 0, sizeof(*addr));
#if defined(__PROSPERO__) || defined(__FreeBSD__)
    addr->sin_len = sizeof(*addr);
#endif
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr->sin_port = htons(port);
}

int vd_ipc_listen(uint16_t port, uint16_t *actual_port)
{
    struct sockaddr_in addr;
    socklen_t size = sizeof(addr);
    int enabled = 1;
    int rc;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;
    rc = configure_socket(fd);
    if (rc != 0)
        return close_error(fd, rc);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0)
        return close_error(fd, -errno);
    loopback_address(&addr, port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0)
        return close_error(fd, -errno);
    if (getsockname(fd, (struct sockaddr *)&addr, &size) != 0)
        return close_error(fd, -errno);
    if (actual_port)
        *actual_port = ntohs(addr.sin_port);
    return fd;
}

int vd_ipc_accept(int listener)
{
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    int rc;
    int fd = accept(listener, (struct sockaddr *)&address, &size);
    if (fd < 0)
        return -errno;
    if (address.sin_family != AF_INET || address.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        return close_error(fd, -EPERM);
    rc = configure_socket(fd);
    return rc == 0 ? fd : close_error(fd, rc);
}

int vd_ipc_connect(uint16_t port)
{
    struct sockaddr_in addr;
    int rc;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;
    rc = configure_socket(fd);
    if (rc != 0)
        return close_error(fd, rc);
    loopback_address(&addr, port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 && errno != EINPROGRESS &&
        errno != EWOULDBLOCK)
        return close_error(fd, -errno);
    return fd;
}

int vd_ipc_connected(int fd)
{
    struct pollfd pfd;
    int error = 0;
    int rc;
    socklen_t size = sizeof(error);
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLOUT;
    rc = poll(&pfd, 1, 0);
    if (rc < 0)
        return errno == EINTR ? 0 : -errno;
    if (rc == 0)
        return 0;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0)
        return -errno;
    if (error != 0)
        return -error;
    if ((pfd.revents & (POLLHUP | POLLNVAL | POLLERR)) != 0)
        return -ECONNRESET;
    return (pfd.revents & POLLOUT) != 0 ? 1 : 0;
}

void vd_ipc_rx_reset(VdIpcRx *rx)
{
    if (rx)
        memset(rx, 0, sizeof(*rx));
}

int vd_ipc_receive(int fd, VdIpcRx *rx, void *body, size_t capacity)
{
    unsigned work;
    if (!rx)
        return -EINVAL;
    for (work = 0; work < 8u; ++work)
    {
        void *target;
        size_t remaining;
        ssize_t received;
        if (rx->header_used < VD_IPC_HEADER_BYTES)
        {
            target = rx->header + rx->header_used;
            remaining = VD_IPC_HEADER_BYTES - rx->header_used;
        }
        else
        {
            if (vd_ipc_get_u32(rx->header) != VD_IPC_MAGIC ||
                vd_ipc_get_u32(rx->header + 4) != VD_IPC_VERSION)
                return -EPROTO;
            rx->type = vd_ipc_get_u32(rx->header + 8);
            rx->length = vd_ipc_get_u32(rx->header + 12);
            if (!valid_length(rx->type, rx->length) || rx->length > capacity ||
                (rx->length != 0u && !body))
                return -EPROTO;
            if (rx->body_used == rx->length)
                return vd_ipc_validate_body(rx->type, body, rx->length) == 0 ? 1 : -EPROTO;
            target = (uint8_t *)body + rx->body_used;
            remaining = rx->length - rx->body_used;
        }
        received = recv(fd, target, remaining, MSG_DONTWAIT);
        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return 0;
            return -errno;
        }
        if (received == 0)
            return -ECONNRESET;
        if (rx->header_used < VD_IPC_HEADER_BYTES)
            rx->header_used += (size_t)received;
        else
            rx->body_used += (size_t)received;
    }
    return 0;
}

int vd_ipc_send(int fd, const void *packet, size_t length, size_t *offset)
{
    ssize_t sent;
    if (!packet || !offset || *offset > length || length > VD_IPC_MAX_PACKET)
        return -EINVAL;
    if (*offset == length)
        return 1;
    sent =
        send(fd, (const uint8_t *)packet + *offset, length - *offset, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        return -errno;
    }
    if (sent == 0)
        return -ECONNRESET;
    *offset += (size_t)sent;
    return *offset == length ? 1 : 0;
}

uint64_t vd_ipc_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
