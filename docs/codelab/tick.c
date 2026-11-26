/*
 * Codelab step 2: a 1 kHz periodic loop driven by timer PULSES, with
 * per-tick jitter measured by ClockCycles(). QNX 8.0.
 *
 *   qcc -V gcc_ntox86_64 -O2 -o tick tick.c
 *   ./tick [seconds] [cpu]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#define PERIOD_NS 1000000L
#define TICK_CODE (_PULSE_CODE_MINAVAIL)

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    int cpu  = argc > 2 ? atoi(argv[2]) : 1;

    /* 1. never page-fault in the loop */
    mlockall(MCL_CURRENT | MCL_FUTURE);

    /* 2. real-time priority */
    struct sched_param sp = { .sched_priority = 60 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* 3. pin to one CPU with a runmask */
    if (cpu < _syspage_ptr->num_cpu)
        ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu));

    /* 4. a channel + a connection to ourselves */
    int chid = ChannelCreate(0);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);

    /* 5. a timer that delivers a pulse every 1 ms */
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, 60, TICK_CODE, 0);
    timer_t tid;
    timer_create(CLOCK_MONOTONIC, &ev, &tid);
    struct itimerspec its = { { 0, PERIOD_NS }, { 0, PERIOD_NS } };
    timer_settime(tid, 0, &its, NULL);

    /* 6. the loop */
    uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    struct _pulse p;
    MsgReceivePulse(chid, &p, sizeof(p), NULL);          /* align to first tick */
    uint64_t t0 = ClockCycles();
    int64_t worst = 0, n = (int64_t)secs * 1000, misses = 0;
    for (int64_t k = 1; k <= n; k++) {
        MsgReceivePulse(chid, &p, sizeof(p), NULL);
        uint64_t now = ClockCycles();
        int64_t ideal_ns = k * PERIOD_NS;
        int64_t real_ns  = (int64_t)((now - t0) * 1000000000.0 / (double)cps);
        int64_t late = real_ns - ideal_ns;
        if (late > worst) worst = late;
        if (late > 200000) misses++;
        /* ... your control law goes here ... */
    }
    printf("%lld ticks, worst lateness %.1f us, misses(>200us) %lld\n",
           (long long)n, worst / 1000.0, (long long)misses);
    return 0;
}
