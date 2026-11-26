/*
 * aiguard - the AI clinical advisor, fenced in by the QNX scheduler.
 *
 * Every breath it fits a respiratory-mechanics model (R, C) to the live
 * waveforms, runs a small trained classifier (normal / leak / occlusion /
 * stiff lung / disconnect) and sends an advisory message - optionally with
 * a proposed settings change - to controld.
 *
 * Mixed criticality, enforced by the OS rather than by good intentions:
 *  - The advisor thread runs under SCHED_SPORADIC: normal priority
 *    PG_PRIO_AI_HIGH with a 2 ms budget per 10 ms replenishment period.
 *    If the model ever runs away (bug, adversarial input, huge model)
 *    the kernel drops it to PG_PRIO_AI_LOW automatically.
 *  - Its CPU consumption is measured with its own per-thread CPU-time
 *    clock (pthread_getcpuclockid) and reported to controld.
 *  - It can only *read* the telemetry (PROT_READ mapping) and can only
 *    *propose* settings through controld, which clamps, rate-limits, and
 *    requires clinician acceptance.
 *  - It sends heartbeat pulses to watchdogd; if they stop, watchdogd kills
 *    and restarts it. The control loop never depends on it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>

#include "pg_core.h"
#include "../common/pg_rt.h"

#define AI_BUDGET_NS      2000000L   /* 2 ms ...           */
#define AI_REPL_NS       10000000L   /* ... every 10 ms    */
#define AI_POLL_NS        5000000L   /* read telemetry every 5 ms */

static const pg_shm_t *g_shm;
static int             g_ctrl_coid = -1;
static int             g_wdog_coid = -1;
static atomic_int      g_rogue;
static clockid_t       g_cpu_clock;

static uint64_t thread_cpu_ns(void)
{
    struct timespec ts;
    if (clock_gettime(g_cpu_clock, &ts) == -1) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ns(long ns)
{
    struct timespec ts = { 0, ns };
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) { }
}

static void send_advice(pg_msg_ai_advice_t *m)
{
    pg_msg_reply_t r;
    if (g_ctrl_coid == -1) g_ctrl_coid = name_open(PG_CTRL_NAME, 0);
    if (g_ctrl_coid == -1) return;
    if (MsgSend(g_ctrl_coid, m, sizeof(*m), &r, sizeof(r)) == -1) {
        ConnectDetach(g_ctrl_coid);           /* controld restarted? reconnect */
        g_ctrl_coid = -1;
    }
}

/* Simulated runaway model: burns CPU forever, occasionally emitting
 * garbage proposals. The scheduler, controld and watchdogd contain it. */
static void rogue_mode(void)
{
    PG_WARN("ROGUE MODE: model is spinning out of control");
    uint64_t w0 = pg_now_ns();
    unsigned samples = 0, demoted = 0;
    for (;;) {
        volatile double sink = 0;
        for (int i = 0; i < 200000; i++) sink += sqrt((double)i);

        /* QNX reports the *effective* priority in sched_curpriority:
         * once the sporadic budget is spent it reads PG_PRIO_AI_LOW. */
        struct sched_param cur;
        int pol;
        if (pthread_getschedparam(pthread_self(), &pol, &cur) == EOK) {
            samples++;
            if (cur.sched_curpriority <= PG_PRIO_AI_LOW) demoted++;
        }

        uint64_t now = pg_now_ns();
        if (now - w0 > 200000000ull) {
            pg_msg_ai_advice_t m;
            memset(&m, 0, sizeof(m));
            m.type = PG_MSG_AI_ADVICE;
            m.dx = PG_DX_STIFF_LUNG;
            m.confidence = 0.99f;
            m.has_proposal = 1;
            m.proposed.pip_cmh2o = 80.0f;      /* lethal */
            m.proposed.peep_cmh2o = 30.0f;
            m.proposed.rate_bpm = NAN;
            m.proposed.ie_ratio = 0.1f;
            m.cpu_ns = thread_cpu_ns();
            m.throttled = (samples && demoted * 2 >= samples) ? 1 : 0;
            PG_WARN("rogue: demoted to prio %d in %u/%u samples", PG_PRIO_AI_LOW, demoted, samples);
            send_advice(&m);
            w0 = now; samples = demoted = 0;
        }
    }
}

static void *advisor_thread(void *arg)
{
    (void)arg;
    pthread_getcpuclockid(pthread_self(), &g_cpu_clock);

    pg_rc_est_t est;
    pg_rc_reset(&est);
    uint64_t last = g_shm->write_idx;
    int prev_phase = 1;
    uint64_t last_beat = 0;
    int breaths_seen = 0;

    PG_INFO("advisor running: SCHED_SPORADIC prio %d->%d, budget %ld us / %ld ms",
            PG_PRIO_AI_HIGH, PG_PRIO_AI_LOW, AI_BUDGET_NS / 1000, AI_REPL_NS / 1000000);

    for (;;) {
        if (atomic_load(&g_rogue)) rogue_mode();

        uint64_t widx = g_shm->write_idx;
        if (widx - last > PG_RING_LEN) last = widx - PG_RING_LEN / 2;  /* lapped */

        for (; last < widx; last++) {
            const pg_sample_t *s = &g_shm->ring[last % PG_RING_LEN];
            if (prev_phase == 1 && s->phase == 0) {
                /* breath boundary */
                if (breaths_seen++ > 0) {
                    pg_breath_features_t f = pg_rc_solve(&est);
                    float probs[PG_DX_COUNT];
                    int dx = pg_classify(&f, probs);

                    pg_msg_ai_advice_t m;
                    memset(&m, 0, sizeof(m));
                    m.type = PG_MSG_AI_ADVICE;
                    m.dx = (uint16_t)dx;
                    m.confidence = probs[dx];
                    m.est_compliance = f.C;
                    m.est_resistance = f.R;
                    pg_settings_t cur = g_shm->settings;
                    m.has_proposal = (uint32_t)pg_ai_propose(dx, &f, &cur, &m.proposed);
                    m.cpu_ns = thread_cpu_ns();
                    send_advice(&m);
                }
                pg_rc_reset(&est);
            }
            prev_phase = s->phase;
            pg_rc_add(&est, s->paw, s->flow, s->volume, 0.001f);
        }

        uint64_t now = pg_now_ns();
        if (now - last_beat > 100000000ull) {
            if (g_wdog_coid == -1) g_wdog_coid = name_open(PG_WDOG_NAME, 0);
            if (g_wdog_coid != -1)
                MsgSendPulse(g_wdog_coid, PG_PRIO_WATCHDOG, PG_PULSE_AI_BEAT, 0);
            last_beat = now;
        }
        sleep_ns(AI_POLL_NS);
    }
    return NULL;
}

int main(void)
{
    pg_log_init("pg_aiguard");

    int fd = -1;
    for (int i = 0; i < 50 && fd == -1; i++) {   /* wait for controld */
        fd = shm_open(PG_SHM_NAME, O_RDONLY, 0);
        if (fd == -1) sleep_ns(100000000L);
    }
    if (fd == -1) { PG_ERR("shm_open: %s", strerror(errno)); return EXIT_FAILURE; }
    g_shm = mmap(NULL, sizeof(pg_shm_t), PROT_READ, MAP_SHARED, fd, 0);   /* read-only! */
    close(fd);
    if (g_shm == MAP_FAILED) { PG_ERR("mmap: %s", strerror(errno)); return EXIT_FAILURE; }

    name_attach_t *att = name_attach(NULL, PG_AI_NAME, 0);
    if (!att) { PG_ERR("name_attach: %s", strerror(errno)); return EXIT_FAILURE; }

    /* --- the fence: SCHED_SPORADIC --- */
    pthread_attr_t a;
    struct sched_param sp;
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_SPORADIC);
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority                 = PG_PRIO_AI_HIGH;
    sp.sched_ss_low_priority          = PG_PRIO_AI_LOW;
    sp.sched_ss_init_budget.tv_sec    = 0;
    sp.sched_ss_init_budget.tv_nsec   = AI_BUDGET_NS;
    sp.sched_ss_repl_period.tv_sec    = 0;
    sp.sched_ss_repl_period.tv_nsec   = AI_REPL_NS;
    sp.sched_ss_max_repl              = 8;
    pthread_attr_setschedparam(&a, &sp);

    pthread_t th;
    int rc = pthread_create(&th, &a, advisor_thread, NULL);
    if (rc != EOK) { PG_ERR("pthread_create(SCHED_SPORADIC): %s", strerror(rc)); return EXIT_FAILURE; }

    pg_drop_abilities();

    union { struct _pulse pulse; uint16_t type; pg_msg_rogue_t rogue; char raw[64]; } msg;
    struct _msg_info info;
    for (;;) {
        int rcvid = MsgReceive(att->chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) continue;
        if (rcvid == 0) {
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(msg.pulse.scoid);
            continue;
        }
        if (msg.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }
        if (msg.type == PG_MSG_AI_ROGUE && info.msglen >= sizeof(msg.rogue)) {
            atomic_store(&g_rogue, msg.rogue.enable ? 1 : 0);
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        MsgError(rcvid, ENOSYS);
    }
}
