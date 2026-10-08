/* SPDX-License-Identifier: GPL-3.0-or-later
 * Process-owned anonymous producer ring. Socket IPC copies records to the app;
 * the producer allocation never moves or changes ownership on peer lifecycle.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "vd_shared.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ---- ring construction on an already-shared mapping ---------------- */

    /* Initialise a VD_SHM_TOTAL_BYTES region as a fresh VD ring: zero it,
     * stamp magic/version, set the fixed capture defaults (48 kHz stereo
     * int16) and clear every counter/sequence. Returns 0, or -EINVAL when
     * base is NULL or bytes does not cover the contract. */
    int vd_shm_prod_init(void *base, unsigned long bytes);

    /* Clean shutdown marker: clears vita_detected/stream_active but keeps
     * magic/version so an attached app reads "disconnected", not "corrupt".
     * NULL base is a no-op. */
    void vd_shm_prod_fini(void *base);

    /* Non-zero when the region carries a valid VDOK header. */
    int vd_shm_prod_contract_ok(const void *base);

    /* ---- slot publish/consume helpers (for producers) ------------------ */

    /* Publish one NV12 frame into the next video slot (zero-fills a partial
     * slot; the app copies whole slots). bytes must be in
     * (0, VD_SHM_VIDEO_SLOT_SIZE]. Returns 0 or -EINVAL. */
    int vd_shm_prod_publish_video(void *base, uint32_t width, uint32_t height,
                                  uint32_t pixel_format, const void *frame, size_t bytes);

    /* Publish one PCM chunk into the next audio slot (zero-fills a partial
     * chunk). bytes must be in (0, VD_AUDIO_CHUNK_BYTES]. Returns 0 or
     * -EINVAL. */
    int vd_shm_prod_publish_audio(void *base, const void *chunk, size_t bytes);

    /* Consumer cursor for the pad ring (payload side reads what the app
     * publishes). Prime once, then drain in publish order. */
    typedef struct
    {
        uint32_t seen[VD_PAD_SLOTS];
        unsigned int cursor;
        int primed;
    } VdShmPadCursor;

    /* Copy the next fresh pad slot (VD_PAD_REPORT_BYTES) into `out`.
     * Returns 1 when a report was copied, 0 when nothing new, -EINVAL on bad
     * arguments, -1 when the copy was torn (slot dropped; retry later). */
    int vd_shm_prod_consume_pad(void *base, VdShmPadCursor *cur, void *out);

    /* fd/have_file/path remain inert for dock_status compatibility. */
    typedef struct
    {
        int fd;
        void *base;
        unsigned long bytes;
        char path[512];
        dev_t file_dev;
        ino_t file_ino;
        int have_file;
    } VdShmHandoff;
    void vd_shm_handoff_init(VdShmHandoff *h);
    /* Allocate and initialize once; subsequent polls never reset the ring.
     * Returns 1 on success, -1 with errno on failure. */
    int vd_shm_handoff_poll(VdShmHandoff *h);
    /* Call only after every producer has stopped. */
    void vd_shm_handoff_close(VdShmHandoff *h);
#ifdef __cplusplus
}
#endif
