/*
 * Real-time helpers (QNX 8.0).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pg_rt.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/syspage.h>

slog2_buffer_t pg_log_buf;
const char    *pg_log_tag = "pulseguard";

int pg_log_init(const char *name)
{
    slog2_buffer_set_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    pg_log_tag = name;
    cfg.buffer_set_name = name;
    cfg.num_buffers = 1;
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "main";
    cfg.buffer_config[0].num_pages = 4;
    if (slog2_register(&cfg, &pg_log_buf, 0) == -1) {
        pg_log_buf = 0;
        fprintf(stderr, "[%s] slog2_register failed (%s), logging to stderr only\n",
                name, strerror(errno));
        return -1;
    }
    return 0;
}

int pg_set_fifo(int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != EOK) {
        PG_WARN("pthread_setschedparam(FIFO,%d): %s", prio, strerror(rc));
        return -1;
    }
    return 0;
}

int pg_num_cpus(void)
{
    return _syspage_ptr->num_cpu;
}

int pg_pin_cpu(int cpu)
{
    int n = pg_num_cpus();
    if (cpu < 0 || cpu >= n || cpu >= 32) return -1;
    uintptr_t mask = (uintptr_t)1u << cpu;
    /* _NTO_TCTL_RUNMASK: restrict this thread to a single CPU so the
     * control loop never migrates and never shares its cache. */
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)mask) == -1) {
        PG_WARN("ThreadCtl(RUNMASK cpu %d): %s", cpu, strerror(errno));
        return -1;
    }
    return cpu;
}

uint64_t pg_cycles(void)
{
    return ClockCycles();
}

uint64_t pg_cycles_to_ns(uint64_t cycles)
{
    static uint64_t cps;
    if (!cps) cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    /* split to avoid overflow for long intervals */
    uint64_t sec = cycles / cps;
    uint64_t rem = cycles % cps;
    return sec * 1000000000ull + (rem * 1000000000ull) / cps;
}

uint64_t pg_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int pg_lock_memory(void)
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        PG_WARN("mlockall: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int pg_drop_abilities(void)
{
    /* After initialisation none of our processes needs to create new
     * processes. Deny and lock these abilities so a compromised or buggy
     * component cannot spawn/fork anything. */
    int rc = procmgr_ability(0,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SPAWN,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_FORK,
        PROCMGR_AID_EOL);
    if (rc != EOK) {
        PG_WARN("procmgr_ability: %s", strerror(rc));
        return -1;
    }
    return 0;
}

int pg_periodic_start(pg_periodic_t *p, long period_ns, int prio, int code)
{
    struct sigevent ev;
    struct itimerspec its;

    p->chid = ChannelCreate(0);
    if (p->chid == -1) return -1;
    p->coid = ConnectAttach(0, 0, p->chid, _NTO_SIDE_CHANNEL, 0);
    if (p->coid == -1) return -1;

    SIGEV_PULSE_INIT(&ev, p->coid, prio, code, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &p->timer) == -1) return -1;

    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec     = 0;
    its.it_value.tv_nsec    = period_ns;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = period_ns;
    if (timer_settime(p->timer, 0, &its, NULL) == -1) return -1;
    return 0;
}

int pg_periodic_wait(pg_periodic_t *p, int code)
{
    struct _pulse pulse;
    for (;;) {
        int rcvid = MsgReceivePulse(p->chid, &pulse, sizeof(pulse), NULL);
        if (rcvid == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pulse.code == code) return 0;
    }
}
