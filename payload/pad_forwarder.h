/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — pad forwarder (PS5 root payload): controller input -> USB -> Vita.
 *
 * Consumes the shared pad ring (core/include/vd_shared.h) and forwards every
 * published pad report to the Vita over USB bulk OUT 0x02 on the vendor
 * interface (see vita-side/input-receiver/README.md). The report is serialized
 * with vd_pad_serialize (core/src/pad_passthrough.c) — the single source of
 * truth for the 28-byte little-endian wire layout the Vita-side parser
 * mirrors — and sent as one synchronous bulk OUT transfer.
 *
 * Lifecycle (called by the payload main):
 *   vd_pad_forwarder_start() validates the region and spawns a worker thread.
 *   The worker discovers the Vita itself, drains the pad ring in publish
 *   order, and forwards each report with minimal latency (no sleeps on the
 *   active path beyond the USB OUT transfer itself). vd_pad_forwarder_stop()
 *   signals the worker, joins it, and tears the USB session down.
 *
 * Tolerates the Vita being absent: reports published while no device (or no
 * pad endpoint) is available are drained and dropped so the sender stays at
 * the live edge of the ring, and discovery is retried with bounded backoff.
 */
#pragma once

/* Start forwarding 28-byte pad reports from the shared pad ring to the Vita
 * over USB bulk OUT. `shm` is the mmap'd shared region base (VdSharedHeader at
 * offset 0; its magic/version must already be initialized to VD_SHM_MAGIC /
 * VD_SHM_VERSION) and `shm_bytes` the mapped size (>= VD_SHM_TOTAL_BYTES).
 * Spawns a worker thread. Returns 0 on success or a negative errno:
 * -EINVAL (bad pointer / size / header), -EBUSY (already started), or the
 * pthread_create error number negated. */
int vd_pad_forwarder_start(void *shm, unsigned long shm_bytes);

/* Stop the worker and tear down the USB session. Blocks until the worker has
 * exited (bounded by one in-flight OUT transfer timeout) and frees any
 * quarantined transfer buffer. Safe to call when not started. */
void vd_pad_forwarder_stop(void);
