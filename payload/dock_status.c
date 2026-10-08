/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent dock evidence: identity + advancing heartbeat + live ring
 * counters. A stale file is NOT liveness; compare at least two heartbeats.
 */
#include "dock_status.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef VD_BUILD_REV
#define VD_BUILD_REV "unknown"
#endif

static uint32_t load_u32(const volatile uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void json_string(FILE *f, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    fputc('"', f);
    while (*p)
    {
        if (*p == '"' || *p == '\\')
            fprintf(f, "\\%c", *p);
        else if (*p < 32)
            fprintf(f, "\\u%04x", (unsigned)*p);
        else
            fputc(*p, f);
        ++p;
    }
    fputc('"', f);
}

static const char *error_name(uint32_t code)
{
    switch (code)
    {
    case VD_ERR_NONE:
        return "none";
    case VD_ERR_NO_VITA:
        return "no_vita";
    case VD_ERR_PROBE_FAILED:
        return "probe_failed";
    case VD_ERR_COMMIT_FAILED:
        return "commit_failed";
    case VD_ERR_BULK_READ_FAILED:
        return "bulk_read_failed";
    case VD_ERR_BAD_FRAME_HEADER:
        return "bad_frame_header";
    case VD_ERR_VITA_DISCONNECTED:
        return "vita_disconnected";
    default:
        return "unknown";
    }
}

int vd_dock_status_write(const char *directory, const VdShmHandoff *handoff, time_t started,
                         uint32_t heartbeat, const char *phase)
{
    char target[640], temporary[640];
    int n, saved;
    FILE *f;
    const VdSharedHeader *hdr;
    if (!directory || !handoff)
        return -EINVAL;
    n = snprintf(target, sizeof(target), "%s/dock-status.json", directory);
    if (n < 0 || (size_t)n >= sizeof(target))
        return -ENAMETOOLONG;
    n = snprintf(temporary, sizeof(temporary), "%s/dock-status.%ld.tmp", directory, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(temporary))
        return -ENAMETOOLONG;
    if (mkdir(directory, 0755) != 0 && errno != EEXIST)
        return -errno;
    f = fopen(temporary, "w");
    if (!f)
        return -errno;
    hdr = (const VdSharedHeader *)handoff->base;
    fprintf(f,
            "{\n  \"schema\": 1, \"pid\": %ld, \"started\": %lld, "
            "\"updated\": %lld, \"heartbeat\": %u,\n  \"build_rev\": ",
            (long)getpid(), (long long)started, (long long)time(NULL), heartbeat);
    json_string(f, VD_BUILD_REV);
    fprintf(f, ", \"built\": ");
    json_string(f, __DATE__ " " __TIME__);
    fprintf(f, ", \"phase\": ");
    json_string(f, phase);
    fprintf(f, ",\n  \"ring_path\": ");
    json_string(f, handoff->path);
    fprintf(f, ", \"ring_bytes\": %lu, \"file_dev\": %llu, \"file_ino\": %llu", handoff->bytes,
            (unsigned long long)handoff->file_dev, (unsigned long long)handoff->file_ino);
    if (hdr)
    {
        uint32_t code = load_u32(&hdr->last_error);
        fprintf(f,
                ",\n  \"magic\": %u, \"version\": %u, "
                "\"vita_detected\": %u, \"stream_active\": %u,\n"
                "  \"video_width\": %u, \"video_height\": %u, "
                "\"frames_captured\": %u, \"frames_dropped\": %u,\n"
                "  \"video_producer\": %u, \"video_seq\": [%u, %u],\n"
                "  \"audio_chunks\": %u, \"audio_producer\": %u, "
                "\"pad_reports\": %u, \"last_error\": %u, \"error_name\": ",
                load_u32(&hdr->magic), load_u32(&hdr->version), load_u32(&hdr->vita_detected),
                load_u32(&hdr->stream_active), load_u32(&hdr->video_width),
                load_u32(&hdr->video_height), load_u32(&hdr->frames_captured),
                load_u32(&hdr->frames_dropped), load_u32(&hdr->video_producer),
                load_u32(&hdr->video_slot_seq[0]), load_u32(&hdr->video_slot_seq[1]),
                load_u32(&hdr->audio_chunks), load_u32(&hdr->audio_producer),
                load_u32(&hdr->pad_reports), code);
        json_string(f, error_name(code));
    }
    fprintf(f, "\n}\n");
    if (ferror(f))
    {
        saved = errno ? errno : EIO;
        fclose(f);
        unlink(temporary);
        return -saved;
    }
    if (fclose(f) != 0)
    {
        saved = errno;
        unlink(temporary);
        return -saved;
    }
    if (rename(temporary, target) != 0)
    {
        saved = errno;
        unlink(temporary);
        return -saved;
    }
    return 0;
}
