/* SPDX-License-Identifier: GPL-3.0-or-later
 * Process-owned anonymous producer storage; never an app-sandbox resource.
 */
#include "vd_shm_prod.h"
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef char vd_shm_header_fits[(sizeof(VdSharedHeader) <= VD_SHM_HEADER_SIZE) ? 1 : -1];

/* ---- low-level sequence-counter protocol (vd_shared.h) ---------------- */

static inline uint32_t vd_seq_load(const volatile uint32_t *seq)
{
    return __atomic_load_n(seq, __ATOMIC_ACQUIRE);
}

static inline void vd_seq_store(volatile uint32_t *seq, uint32_t value)
{
    __atomic_store_n(seq, value, __ATOMIC_RELEASE);
}

/* ---- ring construction ------------------------------------------------ */

int vd_shm_prod_init(void *base, unsigned long bytes)
{
    VdSharedHeader *hdr;

    if (base == NULL || bytes < VD_SHM_TOTAL_BYTES)
    {
        return -EINVAL;
    }

    memset(base, 0, VD_SHM_TOTAL_BYTES);
    hdr = (VdSharedHeader *)base;
    hdr->magic = VD_SHM_MAGIC;
    hdr->version = VD_SHM_VERSION;
    hdr->video_pixel_format = VD_PIX_NONE;
    hdr->audio_sample_rate = 48000;
    hdr->audio_channels = 2;
    hdr->audio_bits = 16;
    hdr->last_error = VD_ERR_NONE;
    return 0;
}

void vd_shm_prod_fini(void *base)
{
    VdSharedHeader *hdr = (VdSharedHeader *)base;

    if (hdr == NULL)
    {
        return;
    }
    hdr->vita_detected = 0;
    hdr->stream_active = 0;
}

int vd_shm_prod_contract_ok(const void *base)
{
    const VdSharedHeader *hdr = (const VdSharedHeader *)base;

    return hdr != NULL && hdr->magic == VD_SHM_MAGIC && hdr->version == VD_SHM_VERSION;
}

/* ---- slot publish/consume helpers ------------------------------------- */

int vd_shm_prod_publish_video(void *base, uint32_t width, uint32_t height, uint32_t pixel_format,
                              const void *frame, size_t bytes)
{
    VdSharedHeader *hdr = (VdSharedHeader *)base;
    uint8_t *dst;
    uint32_t slot;
    uint32_t seq;

    if (base == NULL || frame == NULL || bytes == 0 || bytes > VD_SHM_VIDEO_SLOT_SIZE)
    {
        return -EINVAL;
    }

    /* Global geometry describes ALL ready slots. Retire every old frame
     * before mutation; advancing to odd makes any in-flight reader fail its
     * final sequence check and prevents fresh readers accepting old bytes. */
    if (hdr->video_width != width || hdr->video_height != height ||
        hdr->video_pixel_format != pixel_format)
    {
        for (unsigned i = 0; i < VD_VIDEO_SLOTS; ++i)
        {
            uint32_t before = vd_seq_load(&hdr->video_slot_seq[i]);
            vd_seq_store(&hdr->video_slot_seq[i], (before + 1u) | 1u);
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        hdr->video_width = width;
        hdr->video_height = height;
        hdr->video_pixel_format = pixel_format;
    }

    slot = hdr->video_producer % VD_VIDEO_SLOTS;
    seq = vd_seq_load(&hdr->video_slot_seq[slot]);
    /* Retired odd generations must stay odd during the entire new copy. */
    seq += seq & 1u;
    vd_seq_store(&hdr->video_slot_seq[slot], seq + 1u); /* odd: filling */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    dst = VD_SHM_VIDEO_PTR(base, slot);
    memcpy(dst, frame, bytes);
    if (bytes < VD_SHM_VIDEO_SLOT_SIZE)
    {
        memset(dst + bytes, 0, VD_SHM_VIDEO_SLOT_SIZE - bytes);
    }
    vd_seq_store(&hdr->video_slot_seq[slot], seq + 2u); /* even: released */
    hdr->video_producer = (slot + 1u) % VD_VIDEO_SLOTS;
    __atomic_add_fetch(&hdr->frames_captured, 1u, __ATOMIC_RELAXED);
    return 0;
}

int vd_shm_prod_publish_audio(void *base, const void *chunk, size_t bytes)
{
    VdSharedHeader *hdr = (VdSharedHeader *)base;
    uint8_t *dst;
    uint32_t slot;
    uint32_t seq;

    if (base == NULL || chunk == NULL || bytes == 0 || bytes > VD_AUDIO_CHUNK_BYTES)
    {
        return -EINVAL;
    }

    slot = hdr->audio_producer % VD_AUDIO_SLOTS;
    seq = vd_seq_load(&hdr->audio_slot_seq[slot]);
    vd_seq_store(&hdr->audio_slot_seq[slot], seq + 1u); /* odd: filling */

    dst = VD_SHM_AUDIO_PTR(base, slot);
    memcpy(dst, chunk, bytes);
    if (bytes < VD_AUDIO_CHUNK_BYTES)
    {
        memset(dst + bytes, 0, VD_AUDIO_CHUNK_BYTES - bytes);
    }

    vd_seq_store(&hdr->audio_slot_seq[slot], seq + 2u); /* even: released */
    hdr->audio_producer = (slot + 1u) % VD_AUDIO_SLOTS;
    __atomic_add_fetch(&hdr->audio_chunks, 1u, __ATOMIC_RELAXED);
    return 0;
}

int vd_shm_prod_consume_pad(void *base, VdShmPadCursor *cur, void *out)
{
    VdSharedHeader *hdr = (VdSharedHeader *)base;
    uint32_t before;
    uint32_t after;
    uint32_t slot;
    unsigned int i;

    if (base == NULL || cur == NULL || out == NULL)
    {
        return -EINVAL;
    }

    if (!cur->primed)
    {
        /* Everything published before we attached is stale input. */
        for (i = 0; i < VD_PAD_SLOTS; ++i)
        {
            cur->seen[i] = vd_seq_load(&hdr->pad_slot_seq[i]);
        }
        cur->cursor = hdr->pad_producer % VD_PAD_SLOTS;
        cur->primed = 1;
    }

    slot = cur->cursor;
    before = vd_seq_load(&hdr->pad_slot_seq[slot]);
    if ((before & 1u) != 0u || before == cur->seen[slot])
    {
        return 0; /* producer mid-write, or nothing new */
    }
    memcpy(out, VD_SHM_PAD_PTR(base, slot), VD_PAD_REPORT_BYTES);
    after = vd_seq_load(&hdr->pad_slot_seq[slot]);
    if (after != before)
    {
        return -1; /* torn copy: drop and retry */
    }
    cur->seen[slot] = before;
    cur->cursor = (slot + 1u) % VD_PAD_SLOTS;
    return 1;
}

void vd_shm_handoff_init(VdShmHandoff *h)
{
    if (h)
    {
        memset(h, 0, sizeof(*h));
        h->fd = -1;
    }
}

int vd_shm_handoff_poll(VdShmHandoff *h)
{
    if (!h)
    {
        errno = EINVAL;
        return -1;
    }
    if (h->base)
        return 1;
    void *base =
        mmap(NULL, VD_SHM_TOTAL_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return -1;
    if (vd_shm_prod_init(base, VD_SHM_TOTAL_BYTES) != 0)
    {
        munmap(base, VD_SHM_TOTAL_BYTES);
        errno = EINVAL;
        return -1;
    }
    h->base = base;
    h->bytes = VD_SHM_TOTAL_BYTES;
    return 1;
}

void vd_shm_handoff_close(VdShmHandoff *h)
{
    if (!h)
        return;
    if (h->base)
        munmap(h->base, h->bytes);
    /* Only descriptors explicitly owned by this handle may be closed. */
    if (h->have_file && h->fd >= 0)
        close(h->fd);
    vd_shm_handoff_init(h);
}
