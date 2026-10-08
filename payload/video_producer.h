/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — video producer: continuous UVC capture into the shared video ring.
 *
 * Runs inside the root payload. Called by the payload main:
 *   vd_video_producer_start(shm, shm_bytes)  -> spawn the capture worker
 *   vd_video_producer_stop()                 -> stop it and tear the session down
 *
 * The producer scans for the Vita (backing off and retrying while absent),
 * runs the UVC probe/commit handshake through core/src/uvc_stream.c and
 * copies each complete NV12 frame into the next video ring slot using the
 * per-slot release/acquire sequence protocol of core/include/vd_shared.h.
 */
#pragma once

/* Start publishing UVC video frames into the shared ring.
 *
 * `shm` is the mmap'd shared-region base (layout per vd_shared.h) and
 * `shm_bytes` its size in bytes (>= VD_SHM_TOTAL_BYTES). Spawns a worker
 * thread that runs until vd_video_producer_stop(). Returns 0 on success or
 * -errno on failure; no worker is left running on failure. */
int vd_video_producer_start(void *shm, unsigned long shm_bytes);

/* Stop the capture worker and tear down the UVC session. Joins the worker
 * before returning. Safe to call when the producer was never started or is
 * already stopped. */
void vd_video_producer_stop(void);
