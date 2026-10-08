/* SPDX-License-Identifier: GPL-3.0-or-later
 * Transport adapter for the existing anonymous producer ring.
 * Open/tick/close are called by one owner thread, never by USB workers.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct VdDockIpc VdDockIpc;
    /* Ring stays owned by caller and must outlive this adapter. port=0 for tests. */
    VdDockIpc *vd_dock_ipc_open(void *ring, size_t bytes, uint16_t port, uint16_t *actual_port);
    /* Nonblocking bounded work. Peer errors reset only the peer, not USB workers.
     * now_ms is monotonic (injectable in host tests). Negative only on owner error. */
    int vd_dock_ipc_tick(VdDockIpc *ipc, uint64_t now_ms);
    int vd_dock_ipc_connected(const VdDockIpc *ipc);
    void vd_dock_ipc_close(VdDockIpc *ipc);
#ifdef __cplusplus
}
#endif
