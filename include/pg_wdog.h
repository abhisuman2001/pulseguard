/*
 * Watchdog status shared memory (single writer: watchdogd).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PG_WDOG_H
#define PG_WDOG_H
#include <stdint.h>

#define PG_WDOG_SHM_NAME "/pulseguard_watchdog"

enum { PG_CHILD_SENSORD, PG_CHILD_CONTROLD, PG_CHILD_AIGUARD, PG_CHILD_TELEMETRYD, PG_CHILD_N };

typedef struct {
    uint32_t version;
    uint32_t restarts[PG_CHILD_N];
    int32_t  pid[PG_CHILD_N];
    uint64_t last_event_ns;           /* CLOCK_MONOTONIC                    */
    uint32_t last_event_child;
    uint32_t last_event_reason;       /* 1 = heartbeat timeout, 2 = exited  */
    uint64_t last_detect_latency_us;  /* last beat -> detection             */
    uint64_t last_recovery_us;        /* detection -> first new heartbeat   */
    char     last_event_text[96];
} pg_wdog_shm_t;

#define PG_WDOG_VERSION 1u
#endif
