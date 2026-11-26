/*
 * Small real-time helpers shared by all PulseGuard QNX processes.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PG_RT_H
#define PG_RT_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <sys/slog2.h>
#include "pg_proto.h"

extern slog2_buffer_t pg_log_buf;

/* Log both to slog2 (view with `slog2info`) and stderr */
#define PG_LOG(sev, fmt, ...) do {                                    \
    if (pg_log_buf) slog2f(pg_log_buf, 0, (sev), fmt, ##__VA_ARGS__); \
    fprintf(stderr, "[%s] " fmt "\n", pg_log_tag, ##__VA_ARGS__);     \
} while (0)
#define PG_INFO(fmt, ...)  PG_LOG(SLOG2_INFO,     fmt, ##__VA_ARGS__)
#define PG_WARN(fmt, ...)  PG_LOG(SLOG2_WARNING,  fmt, ##__VA_ARGS__)
#define PG_ERR(fmt, ...)   PG_LOG(SLOG2_ERROR,    fmt, ##__VA_ARGS__)

extern const char *pg_log_tag;

int      pg_log_init(const char *name);

/* SCHED_FIFO at the given priority for the calling thread */
int      pg_set_fifo(int prio);
/* Pin the calling thread to one CPU (QNX runmask). Returns cpu or -1. */
int      pg_pin_cpu(int cpu);
int      pg_num_cpus(void);

/* High-resolution time from the free-running cycle counter */
uint64_t pg_cycles(void);
uint64_t pg_cycles_to_ns(uint64_t cycles);
uint64_t pg_now_ns(void);

/* Lock all current and future pages (no page faults in the RT path) */
int      pg_lock_memory(void);

/* Drop abilities we never need after start-up (least privilege). */
int      pg_drop_abilities(void);

/* Create a private channel + side-channel connection and arm a periodic
 * timer that delivers `code` pulses at `prio` every `period_ns`. */
typedef struct {
    int     chid;
    int     coid;
    timer_t timer;
} pg_periodic_t;

int      pg_periodic_start(pg_periodic_t *p, long period_ns, int prio, int code);
/* Block until the next pulse with the given code. Returns 0 or -1. */
int      pg_periodic_wait(pg_periodic_t *p, int code);

#endif
