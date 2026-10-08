/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 local IPC. The payload never opens an app-sandbox resource.
 * Wire integers are little-endian; framing is independent of RAM structs.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vd_shared.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define VD_IPC_PORT 47965u
#define VD_IPC_MAGIC 0x31494456u /* VDI1 */
#define VD_IPC_VERSION 1u
#define VD_IPC_HEADER_BYTES 16u
#define VD_IPC_HELLO_BYTES 40u /* RAM version, pid, 32-byte terminated revision */
#define VD_IPC_STATUS_WORDS 13u
#define VD_IPC_STATUS_BYTES (VD_IPC_STATUS_WORDS * 4u)
#define VD_IPC_VIDEO_META_BYTES 12u /* width, height, NV12 format */
#define VD_IPC_MAX_BODY (VD_VIDEO_MAX_FRAME + VD_IPC_VIDEO_META_BYTES)
#define VD_IPC_MAX_PACKET (VD_IPC_HEADER_BYTES + VD_IPC_MAX_BODY)
#define VD_IPC_PEER_IDLE_MS 500u
#define VD_IPC_PING_MS 100u

    enum VdIpcType
    {
        VD_IPC_HELLO = 1,
        VD_IPC_STATUS = 2,
        VD_IPC_VIDEO = 3,
        VD_IPC_AUDIO = 4,
        VD_IPC_PAD = 5,
        VD_IPC_TOUCH = 6,
        VD_IPC_PING = 7
    };
    /* STATUS word order. */
    enum VdIpcStatusField
    {
        VD_IPC_VITA,
        VD_IPC_STREAM,
        VD_IPC_WIDTH,
        VD_IPC_HEIGHT,
        VD_IPC_FORMAT,
        VD_IPC_AUDIO_RATE,
        VD_IPC_AUDIO_CHANNELS,
        VD_IPC_AUDIO_BITS,
        VD_IPC_CAPTURED,
        VD_IPC_DROPPED,
        VD_IPC_AUDIO_CHUNKS,
        VD_IPC_PAD_REPORTS,
        VD_IPC_ERROR
    };

    typedef struct VdIpcRx
    {
        uint8_t header[VD_IPC_HEADER_BYTES];
        size_t header_used;
        size_t body_used;
        uint32_t type;
        uint32_t length;
    } VdIpcRx;

    uint32_t vd_ipc_get_u32(const void *p);
    void vd_ipc_put_u32(void *p, uint32_t value);
    /* Return expected NV12 bytes, or zero for invalid geometry/format. */
    size_t vd_ipc_video_bytes(uint32_t width, uint32_t height, uint32_t format);
    void vd_ipc_make_hello(uint8_t out[VD_IPC_HELLO_BYTES], const char *revision, uint32_t pid);
    /* Checks exact lengths and bounded video geometry. 0 valid, -EPROTO invalid. */
    int vd_ipc_validate_body(uint32_t type, const void *body, size_t length);
    /* Serializes one complete bounded packet. Zero on invalid input/capacity. */
    size_t vd_ipc_packet(void *out, size_t capacity, uint32_t type, const void *body,
                         size_t length);

    /* All sockets are numeric IPv4 LOOPBACK ONLY, TCP_NODELAY, nonblocking.
     * All failure returns are negative errno; no global signal-handler changes.
     * port=0 permits isolated host tests to use an ephemeral port. */
    int vd_ipc_listen(uint16_t port, uint16_t *actual_port);
    int vd_ipc_accept(int listener);
    int vd_ipc_connect(uint16_t port);
    /* 1 connected, 0 still connecting, negative errno on error. */
    int vd_ipc_connected(int fd);
    void vd_ipc_rx_reset(VdIpcRx *rx);
    /* 1 whole validated packet, 0 pending, negative errno/EPROTO on failure.
     * Caller handles the packet then resets rx. Never read past body capacity. */
    int vd_ipc_receive(int fd, VdIpcRx *rx, void *body, size_t capacity);
    /* 1 whole packet sent, 0 pending, negative errno on failure; offset persists. */
    int vd_ipc_send(int fd, const void *packet, size_t length, size_t *offset);
    uint64_t vd_ipc_now_ms(void);

#ifdef __cplusplus
}
#endif
