/*
 * Telemetry wire protocol (newline-delimited JSON over TCP) shared by the
 * QNX telemetryd and the host emulator. Portable C99.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PG_TELEMETRY_H
#define PG_TELEMETRY_H

#include <stddef.h>
#include <stdint.h>
#include "pg_proto.h"
#include "pg_wdog.h"

typedef struct {
    int stress_on;
    int rogue_on;
    int emulator;       /* 1 = host emulator, NOT real-time: dashboard shows a banner */
    int ncpu;
} pg_tel_flags_t;

/* Build one frame (one JSON object + '\n'). Consumes ring samples after
 * *last_idx. w may be NULL. Returns length written (0 on overflow). */
size_t pg_tel_frame(char *buf, size_t cap, const pg_shm_t *s, const pg_wdog_shm_t *w,
                    uint64_t *last_idx, const pg_tel_flags_t *fl, uint64_t now_ns);

enum {
    PG_CMD_NONE = 0,
    PG_CMD_SETTINGS,
    PG_CMD_FAULT,
    PG_CMD_CRASH,
    PG_CMD_ACCEPT_AI,
    PG_CMD_ROGUE,
    PG_CMD_STRESS,
};

typedef struct {
    int           kind;
    pg_settings_t settings;
    uint32_t      mask;
    int           on;
} pg_cmd_t;

/* Parse {"cmd":"...",...}. Returns kind (PG_CMD_NONE if unknown). */
int pg_cmd_parse(const char *line, pg_cmd_t *c);

#endif
