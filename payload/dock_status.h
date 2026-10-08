/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <time.h>
#include "vd_shm_prod.h"

/* Best-effort atomic diagnostic snapshot. No new shared-ring fields and no
 * producer/USB side effects. `directory` must be outside the app sandbox;
 * payload root cannot create files inside download0. Returns 0 or -errno. */
int vd_dock_status_write(const char *directory, const VdShmHandoff *handoff, time_t started,
                         uint32_t heartbeat, const char *phase);
