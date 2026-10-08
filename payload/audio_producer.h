/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — audio producer (PS5 root payload): continuous UAC isoc capture that
 * publishes PCM into the shared audio ring.
 *
 * The payload main mmaps the shared region (layout per core/include/vd_shared.h)
 * and drives this module with exactly two calls:
 *
 *   int  vd_audio_producer_start(void *shm, unsigned long shm_bytes);
 *   void vd_audio_producer_stop(void);
 *
 * start() validates the mapping, publishes the fixed capture format
 * (48 kHz stereo int16 interleaved) into the shared header, and spawns a
 * capture worker thread; it returns 0 once the worker is running (a missing
 * Vita is NOT an error — the worker retries with capped exponential back-off),
 * or -EINVAL/-EBUSY/-errno on setup failure. stop() signals the worker, joins
 * it (bounded by one in-flight isoc transfer, <= ~450 ms), and tears the USB
 * session down cleanly. start()/stop() are called from the payload main only,
 * not concurrently with each other; stop() is a no-op when not started.
 */
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /* Start the PCM capture worker against the shared ring at `shm`.
     *   shm        mmap'd shared region base (vd_shared.h layout)
     *   shm_bytes  size of that mapping; must cover the header + audio ring
     * Returns 0 once the worker thread is running, or -errno:
     *   -EINVAL  shm is NULL or shm_bytes does not cover the audio ring
     *   -EBUSY   the producer is already running
     *   -errno   thread creation failure */
    int vd_audio_producer_start(void *shm, unsigned long shm_bytes);

    /* Stop the worker and release the USB session. Safe to call at any point
     * after start(), and a no-op otherwise. */
    void vd_audio_producer_stop(void);

#ifdef __cplusplus
}
#endif
