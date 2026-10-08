/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real host socket tests of the committed framing adapter, not PS5 emulation.
 */
#ifdef VD5_HOST_IPC_TEST
#include "vd_ipc.h"
#include "pad_passthrough.h"
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned failures;
#define CHECK(expr)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL %s:%u: %s\n", __FILE__, (unsigned)__LINE__, #expr);              \
        }                                                                                          \
    } while (0)

static void test_validation(void)
{
    uint8_t hello[VD_IPC_HELLO_BYTES], packet[128], touch[VD_TOUCH_WIRE_BYTES];
    VdTouchReport report = {0};
    vd_ipc_make_hello(hello, "host-protocol-test", 123u);
    CHECK(vd_ipc_get_u32(hello) == VD_SHM_VERSION);
    CHECK(vd_ipc_get_u32(hello + 4) == 123u);
    CHECK(vd_ipc_packet(packet, sizeof(packet), VD_IPC_HELLO, hello, sizeof(hello)) ==
          VD_IPC_HEADER_BYTES + sizeof(hello));
    hello[0] ^= 1u;
    CHECK(vd_ipc_validate_body(VD_IPC_HELLO, hello, sizeof(hello)) == -EPROTO);
    CHECK(vd_ipc_video_bytes(1280u, 720u, VD_PIX_NV12) == VD_VIDEO_MAX_FRAME);
    CHECK(vd_ipc_video_bytes(0u, 544u, VD_PIX_NV12) == 0u);
    CHECK(vd_ipc_video_bytes(961u, 544u, VD_PIX_NV12) == 0u);
    CHECK(vd_ipc_video_bytes(1282u, 720u, VD_PIX_NV12) == 0u);
    CHECK(vd_ipc_video_bytes(960u, 544u, VD_PIX_NONE) == 0u);
    CHECK(vd_ipc_packet(packet, sizeof(packet), VD_IPC_PAD, packet, 1u) == 0u);
    CHECK(vd_ipc_packet(packet, sizeof(packet), 99u, NULL, 0u) == 0u);
    CHECK(vd_touch_serialize(&report, touch) == VD_TOUCH_WIRE_BYTES);
    CHECK(vd_ipc_validate_body(VD_IPC_TOUCH, touch, sizeof(touch)) == 0);
    touch[0] = 2u;
    CHECK(vd_ipc_validate_body(VD_IPC_TOUCH, touch, sizeof(touch)) == -EPROTO);
}

static void test_fragments(void)
{
    int sockets[2];
    uint8_t hello[VD_IPC_HELLO_BYTES], packet[128], body[128];
    VdIpcRx rx = {0};
    size_t length, i;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    vd_ipc_make_hello(hello, "fragmented", 7u);
    length = vd_ipc_packet(packet, sizeof(packet), VD_IPC_HELLO, hello, sizeof(hello));
    for (i = 0; i < length; ++i)
    {
        int rc;
        CHECK(send(sockets[0], packet + i, 1u, MSG_NOSIGNAL) == 1);
        rc = vd_ipc_receive(sockets[1], &rx, body, sizeof(body));
        CHECK(rc == (i + 1u == length ? 1 : 0));
    }
    CHECK(rx.type == VD_IPC_HELLO);
    CHECK(memcmp(body, hello, sizeof(hello)) == 0);
    vd_ipc_rx_reset(&rx);
    length = vd_ipc_packet(packet, sizeof(packet), VD_IPC_PING, NULL, 0);
    CHECK(send(sockets[0], packet, length, MSG_NOSIGNAL) == (ssize_t)length);
    CHECK(vd_ipc_receive(sockets[1], &rx, NULL, 0) == 1);
    CHECK(rx.type == VD_IPC_PING && rx.length == 0u);
    vd_ipc_rx_reset(&rx);
    /* Oversize/type/version rejection occurs before touching body storage. */
    vd_ipc_put_u32(packet, VD_IPC_MAGIC);
    vd_ipc_put_u32(packet + 4, VD_IPC_VERSION);
    vd_ipc_put_u32(packet + 8, VD_IPC_VIDEO);
    vd_ipc_put_u32(packet + 12, VD_IPC_MAX_BODY + 1u);
    memset(body, 0xa5, sizeof(body));
    CHECK(send(sockets[0], packet, VD_IPC_HEADER_BYTES, MSG_NOSIGNAL) == VD_IPC_HEADER_BYTES);
    CHECK(vd_ipc_receive(sockets[1], &rx, body, sizeof(body)) == -EPROTO);
    for (i = 0; i < sizeof(body); ++i)
        CHECK(body[i] == 0xa5u);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_loopback_and_backpressure(void)
{
    uint16_t port = 0;
    struct sockaddr_in address;
    socklen_t address_bytes = sizeof(address);
    uint8_t *packet = calloc(1u, VD_IPC_MAX_PACKET);
    uint8_t *body = calloc(1u, VD_IPC_MAX_BODY);
    VdIpcRx rx = {0};
    size_t length, offset = 0;
    uint64_t deadline;
    int listener = vd_ipc_listen(0, &port);
    int client, server = -1, rc = 0;
    CHECK(packet != NULL && body != NULL && listener >= 0 && port != 0u);
    if (!packet || !body || listener < 0)
        exit(2);
    CHECK(getsockname(listener, (struct sockaddr *)&address, &address_bytes) == 0);
    CHECK(address.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    CHECK(vd_ipc_listen(port, NULL) == -EADDRINUSE);
    client = vd_ipc_connect(port);
    CHECK(client >= 0);
    deadline = vd_ipc_now_ms() + 2000u;
    while (server < 0 && vd_ipc_now_ms() < deadline)
    {
        server = vd_ipc_accept(listener);
        if (server < 0)
            usleep(1000);
    }
    CHECK(server >= 0 && vd_ipc_connected(client) == 1);
    vd_ipc_put_u32(body, 1280u);
    vd_ipc_put_u32(body + 4, 720u);
    vd_ipc_put_u32(body + 8, VD_PIX_NV12);
    memset(body + VD_IPC_VIDEO_META_BYTES, 0x6c, VD_VIDEO_MAX_FRAME);
    length = vd_ipc_packet(packet, VD_IPC_MAX_PACKET, VD_IPC_VIDEO, body, VD_IPC_MAX_BODY);
    CHECK(length == VD_IPC_MAX_PACKET);
    rc = vd_ipc_send(server, packet, length, &offset);
    CHECK(rc == 0 && offset > 0u && offset < length); /* peer deliberately hasn't read */
    memset(body, 0, VD_IPC_MAX_BODY);
    deadline = vd_ipc_now_ms() + 2000u;
    rc = 0;
    while (rc == 0 && vd_ipc_now_ms() < deadline)
    {
        int sent = vd_ipc_send(server, packet, length, &offset);
        CHECK(sent >= 0);
        rc = vd_ipc_receive(client, &rx, body, VD_IPC_MAX_BODY);
        if (rc == 0)
            usleep(1000);
    }
    CHECK(rc == 1 && offset == length && rx.type == VD_IPC_VIDEO);
    CHECK(memcmp(packet + VD_IPC_HEADER_BYTES, body, VD_IPC_MAX_BODY) == 0);
    close(server);
    vd_ipc_rx_reset(&rx);
    CHECK(vd_ipc_receive(client, &rx, body, VD_IPC_MAX_BODY) == -ECONNRESET);
    close(client);
    close(listener);
    free(packet);
    free(body);
}

int main(void)
{
    test_validation();
    test_fragments();
    test_loopback_and_backpressure();
    printf("dock IPC protocol: %s (real host sockets; NOT PS5 validation)\n",
           failures == 0u ? "all checks passed" : "FAILED");
    return failures == 0u ? 0 : 1;
}
#endif
