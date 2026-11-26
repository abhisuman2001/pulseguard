/*
 * rtbench - the same 1 kHz periodic loop on any POSIX OS, so the timing
 * claim "QNX vs a general-purpose OS" can be reproduced, not just asserted.
 *
 *   rtbench [-s seconds] [-p prio] [-l load_threads]
 *
 * Builds with qcc for QNX 8.0 and with gcc for Linux. Uses only POSIX:
 * clock_nanosleep(TIMER_ABSTIME) + SCHED_FIFO (needs root / CAP_SYS_NICE).
 * Prints a latency histogram and percentiles of the wake-up lateness.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __QNX__
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PERIOD_NS 1000000L
#define MAXUS     10000

static volatile int g_stop;
static uint64_t hist[MAXUS + 1];

static void *load(void *a)
{
    (void)a;
    volatile double x = 1;
    while (!g_stop) for (int i = 0; i < 100000; i++) x = sqrt(x + i);
    return NULL;
}

static int64_t ts_ns(const struct timespec *t) { return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec; }

int main(int argc, char **argv)
{
    int secs = 30, prio = 60, nload = 0, opt;
    while ((opt = getopt(argc, argv, "s:p:l:")) != -1) {
        if (opt == 's') secs = atoi(optarg);
        else if (opt == 'p') prio = atoi(optarg);
        else if (opt == 'l') nload = atoi(optarg);
    }
    mlockall(MCL_CURRENT | MCL_FUTURE);

    pthread_t lt[64];
    for (int i = 0; i < nload && i < 64; i++) pthread_create(&lt[i], NULL, load, NULL);

    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    int rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;

    struct timespec next, now;
    clock_gettime(CLOCK_MONOTONIC, &next);
    long n = (long)secs * 1000, over = 0;
    int64_t worst = 0;
    double sum = 0;
    for (long i = 0; i < n; i++) {
        next.tv_nsec += PERIOD_NS;
        if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t late = ts_ns(&now) - ts_ns(&next);
        if (late < 0) late = 0;
        if (late > worst) worst = late;
        sum += (double)late;
        int64_t us = late / 1000;
        if (us > MAXUS) { us = MAXUS; over++; }
        hist[us]++;
    }
    g_stop = 1;
    for (int i = 0; i < nload && i < 64; i++) pthread_join(lt[i], NULL);

    double p[] = { 50, 99, 99.9, 99.99 };
    printf("os=%s rt_sched=%s prio=%d load_threads=%d samples=%ld\n",
#ifdef __QNX__
           "QNX",
#else
           "Linux",
#endif
           rt ? "SCHED_FIFO" : "NO(not permitted)", prio, nload, n);
    for (size_t k = 0; k < sizeof(p) / sizeof(p[0]); k++) {
        uint64_t target = (uint64_t)ceil(p[k] / 100.0 * (double)n), acc = 0;
        for (int us = 0; us <= MAXUS; us++) {
            acc += hist[us];
            if (acc >= target) { printf("p%-6g %6d us\n", p[k], us); break; }
        }
    }
    printf("mean    %8.1f us\nmax     %8.1f us\n>10ms   %ld\n", sum / n / 1000.0, worst / 1000.0, over);
    return 0;
}
