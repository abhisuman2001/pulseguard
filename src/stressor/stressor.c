/*
 * stressor - hostile CPU load for the demo.
 *
 * One SCHED_FIFO busy thread per CPU, at PG_PRIO_STRESS (higher than
 * almost everything in the system except the safety path), each pinned
 * with a QNX runmask. CPU 0 is left alone by default so the network stack
 * keeps the dashboard alive; the control loop's own CPU (1) IS saturated.
 *
 *   pg_stressor [-p prio] [-a]      -a: also load CPU 0
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>

#include "../common/pg_rt.h"

static int g_prio = PG_PRIO_STRESS;
static volatile sig_atomic_t g_stop;

static void on_term(int s) { (void)s; g_stop = 1; }

static void *burn(void *arg)
{
    int cpu = (int)(intptr_t)arg;
    pg_pin_cpu(cpu);
    pg_set_fifo(g_prio);
    volatile double x = 1.0;
    while (!g_stop) {
        for (int i = 0; i < 100000; i++) x = sqrt(x + i);
        /* also thrash memory/cache a little */
        static __thread char junk[256 * 1024];
        for (size_t i = 0; i < sizeof(junk); i += 64) junk[i]++;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int all = 0, opt;
    while ((opt = getopt(argc, argv, "p:a")) != -1) {
        if (opt == 'p') g_prio = atoi(optarg);
        else if (opt == 'a') all = 1;
    }
    pg_log_init("pg_stressor");
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    int n = pg_num_cpus();
    int started = 0;
    pthread_t th[64];
    for (int c = all ? 0 : 1; c < n && c < 64; c++) {
        pthread_create(&th[started++], NULL, burn, (void *)(intptr_t)c);
    }
    PG_WARN("burning %d CPUs at FIFO prio %d", started, g_prio);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    return 0;
}
