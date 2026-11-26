/*
 * controld - the safety-critical 1 kHz ventilation control loop.
 *
 * Threads
 *   control  SCHED_FIFO prio PG_PRIO_CONTROL, pinned to one CPU (runmask),
 *            woken by a 1 kHz timer *pulse* on a private channel. Each tick:
 *              1. one devctl() round trip to /dev/pulseguard/plant
 *                 (command out, sensor sample in)
 *              2. breath sequencer + PI pressure controller + hard alarms
 *              3. publish sample + jitter stats to shared memory (seqlock)
 *              4. every 10 ticks: heartbeat pulse to watchdogd
 *            No heap allocation, no blocking I/O, memory locked.
 *
 *   server   name_attach("pulseguard/control"): synchronous MsgReceive
 *            for settings (clinician), advice (AI), fault injection (demo).
 *            QNX priority inheritance means a request from a low-priority
 *            client is served at that client's priority - it can never
 *            delay the control thread.
 *
 * Every external input - clinician or AI - passes the same hard safety
 * envelope (pg_settings_clamp). AI proposals are additionally rate
 * limited and are never applied without a clinician's explicit accept.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>

#include "pg_core.h"
#include "pg_devctl.h"
#include "../common/pg_rt.h"

static pg_shm_t       *g_shm;
static int             g_plant_fd = -1;
static int             g_wdog_coid = -1;
static int             g_cpu = -1;

/* settings hand-off from server thread to control thread (lock-free) */
static pthread_mutex_t g_set_lock = PTHREAD_MUTEX_INITIALIZER;
static pg_settings_t   g_new_settings;
static atomic_int      g_settings_dirty;

/* ---------------------------------------------------------------- */
/* shared memory                                                     */
/* ---------------------------------------------------------------- */
static pg_shm_t *shm_setup(int *restored)
{
    int fd = shm_open(PG_SHM_NAME, O_RDWR | O_CREAT, 0644);
    if (fd == -1) { PG_ERR("shm_open: %s", strerror(errno)); return NULL; }
    if (ftruncate(fd, sizeof(pg_shm_t)) == -1) {
        PG_ERR("ftruncate: %s", strerror(errno)); close(fd); return NULL;
    }
    pg_shm_t *s = mmap(NULL, sizeof(pg_shm_t), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) { PG_ERR("mmap: %s", strerror(errno)); return NULL; }

    *restored = 0;
    if (s->version == PG_SHM_VERSION) {
        /* We are a restart after a crash: keep the clinician's settings. */
        pg_settings_t keep = s->settings;
        pg_settings_t def;
        pg_settings_default(&def);
        pg_settings_clamp(&keep, &def);
        s->settings = keep;
        *restored = 1;
    } else {
        memset(s, 0, sizeof(*s));
        s->version = PG_SHM_VERSION;
        pg_settings_default(&s->settings);
    }
    s->cycles = 0;
    s->deadline_misses = 0;
    s->jitter_max_ns = 0;
    s->jitter_min_ns = 0;
    memset(s->jitter_hist, 0, sizeof(s->jitter_hist));
    s->pid = (uint32_t)getpid();
    s->start_ns = pg_now_ns();
    return s;
}

#define SHM_BEGIN() do { g_shm->seq++; atomic_thread_fence(memory_order_release); } while (0)
#define SHM_END()   do { atomic_thread_fence(memory_order_release); g_shm->seq++; } while (0)

/* ---------------------------------------------------------------- */
/* control thread                                                    */
/* ---------------------------------------------------------------- */
static void *control_thread(void *arg)
{
    (void)arg;
    pg_periodic_t per;
    pg_vent_t     vent;
    pg_jitter_t   jit;
    pg_plant_xchg_t x;
    pg_vent_out_t out = { 0 };
    float breath_vol = 0.0f;
    uint32_t seq = 0;

    pg_set_fifo(PG_PRIO_CONTROL);
    g_cpu = pg_pin_cpu(pg_num_cpus() > 1 ? 1 : 0);

    pg_vent_init(&vent, &g_shm->settings);
    pg_jitter_init(&jit, 200000 /* 200 us = 20% of the period */);

    if (pg_periodic_start(&per, PG_CONTROL_PERIOD_NS, PG_PRIO_CONTROL, PG_PULSE_TICK) == -1) {
        PG_ERR("periodic timer: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* first wake-up defines the time base */
    pg_periodic_wait(&per, PG_PULSE_TICK);
    uint64_t t0 = pg_cycles_to_ns(pg_cycles());
    uint64_t k = 0;
    uint64_t last_ns = t0;

    PG_INFO("control loop running: 1 kHz, prio %d, cpu %d", PG_PRIO_CONTROL, g_cpu);

    for (;;) {
        pg_periodic_wait(&per, PG_PULSE_TICK);
        uint64_t now = pg_cycles_to_ns(pg_cycles());
        k++;
        int64_t late = (int64_t)now - (int64_t)(t0 + k * (uint64_t)PG_CONTROL_PERIOD_NS);
        /* if we ever fell a whole period behind (e.g. debugger), re-anchor */
        if (late > 10 * PG_CONTROL_PERIOD_NS || late < -10 * PG_CONTROL_PERIOD_NS) {
            t0 = now; k = 0; late = 0;
        }
        pg_jitter_add(&jit, (int32_t)late);
        float dt = (float)(now - last_ns) * 1e-9f;
        last_ns = now;
        if (dt <= 0.0f || dt > 0.01f) dt = 0.001f;

        /* apply new settings at most once per tick, never block */
        if (atomic_load_explicit(&g_settings_dirty, memory_order_acquire)) {
            if (pthread_mutex_trylock(&g_set_lock) == EOK) {
                pg_vent_apply_settings(&vent, &g_new_settings);
                atomic_store(&g_settings_dirty, 0);
                pthread_mutex_unlock(&g_set_lock);
            }
        }

        /* 1. plant I/O: one message pass, served at OUR priority */
        x.cmd.blower_cmh2o      = out.blower_cmd;
        x.cmd.exhale_valve_open = (uint32_t)out.exhale_open;
        x.cmd.seq               = ++seq;
        int rc = devctl(g_plant_fd, DCMD_PG_EXCHANGE, &x, sizeof(x), NULL);
        if (rc != EOK) {
            /* sensor path broken: fail safe and keep trying */
            out.blower_cmd = 0.0f;
            out.exhale_open = 1;
            continue;
        }
        float paw  = x.sample.paw_cmh2o;
        float flow = x.sample.flow_lpm / 60.0f;

        /* 2. control law + alarms */
        out = pg_vent_step(&vent, paw, flow, dt);
        if (out.breath_done) breath_vol = 0.0f;
        breath_vol += flow * 1000.0f * dt;

        uint16_t alarms = (uint16_t)vent.alarms;
        if (x.sample.failsafe)       alarms |= PG_ALM_PLANT_FAILSAFE;
        if (late > jit.miss_threshold_ns) alarms |= PG_ALM_DEADLINE;

        /* 3. publish */
        uint64_t idx = g_shm->write_idx;
        pg_sample_t *s = &g_shm->ring[idx % PG_RING_LEN];
        s->t_ns       = x.sample.t_ns;
        s->paw        = paw;
        s->flow       = flow;
        s->volume     = breath_vol;
        s->blower_cmd = out.blower_cmd;
        s->jitter_ns  = (int32_t)late;
        s->phase      = (uint16_t)vent.phase;
        s->alarms     = alarms;

        SHM_BEGIN();
        g_shm->write_idx      = idx + 1;
        g_shm->cycles         = jit.n;
        g_shm->deadline_misses= jit.misses;
        g_shm->jitter_max_ns  = jit.max_ns;
        g_shm->jitter_min_ns  = jit.min_ns;
        if ((k & 63) == 0)
            memcpy(g_shm->jitter_hist, jit.hist, sizeof(jit.hist));
        g_shm->control_cpu    = (uint32_t)g_cpu;
        g_shm->settings       = vent.set;
        g_shm->alarms_latched|= alarms;
        g_shm->fault_mask     = x.sample.fault_mask;
        if (out.breath_done) {
            g_shm->last_vt_ml   = vent.last_vte;
            g_shm->last_pip     = vent.last_pip;
            g_shm->breath_count = vent.breaths;
        }
        SHM_END();

        /* 4. heartbeat (non-blocking pulse) */
        if ((k % 10) == 0 && g_wdog_coid != -1)
            MsgSendPulse(g_wdog_coid, PG_PRIO_WATCHDOG, PG_PULSE_HEARTBEAT, (int)(k & 0x7fffffff));
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* server thread (main)                                              */
/* ---------------------------------------------------------------- */
static void push_settings(const pg_settings_t *s)
{
    pthread_mutex_lock(&g_set_lock);
    g_new_settings = *s;
    atomic_store_explicit(&g_settings_dirty, 1, memory_order_release);
    pthread_mutex_unlock(&g_set_lock);
}

static void reply_settings(int rcvid, int status, int clamped, const pg_settings_t *s)
{
    pg_msg_reply_t r;
    memset(&r, 0, sizeof(r));
    r.status = status;
    r.clamped = (uint32_t)clamped;
    r.active = *s;
    MsgReply(rcvid, EOK, &r, sizeof(r));
}

static void handle_ai(int rcvid, const pg_msg_ai_advice_t *m)
{
    pg_settings_t cur = g_shm->settings;
    int clamped = 0, rejected = 0;
    pg_settings_t prop = m->proposed;

    if (m->dx >= PG_DX_COUNT || !(m->confidence >= 0.0f && m->confidence <= 1.0f))
        rejected = 1;                      /* malformed (or NaN) - ignore */

    if (!rejected && m->has_proposal) {
        clamped |= pg_settings_clamp(&prop, &cur);
        clamped |= pg_settings_limit_step(&prop, &cur, 2.0f, 3.0f);
        clamped |= pg_settings_clamp(&prop, &cur);
    }

    SHM_BEGIN();
    g_shm->ai_advice_received++;
    g_shm->ai_last_ns = pg_now_ns();
    g_shm->ai_cpu_ns  = m->cpu_ns;
    g_shm->ai_state   = m->throttled ? 1 : 0;
    if (rejected) {
        g_shm->ai_advice_rejected++;
    } else {
        g_shm->ai_dx         = m->dx;
        g_shm->ai_confidence = m->confidence;
        g_shm->ai_compliance = m->est_compliance;
        g_shm->ai_resistance = m->est_resistance;
        if (m->has_proposal && m->confidence > 0.7f) {
            g_shm->ai_proposed     = prop;
            g_shm->ai_has_proposal = 1;
            if (clamped) g_shm->ai_advice_clamped++;
        } else if (m->dx == PG_DX_NORMAL) {
            g_shm->ai_has_proposal = 0;
        }
    }
    SHM_END();
    reply_settings(rcvid, rejected ? EINVAL : EOK, clamped, &g_shm->settings);
}

int main(int argc, char **argv)
{
    int opt;
    uint32_t restarts = 0;
    while ((opt = getopt(argc, argv, "r:")) != -1)
        if (opt == 'r') restarts = (uint32_t)strtoul(optarg, NULL, 10);

    pg_log_init("pg_controld");
    pg_lock_memory();

    int restored = 0;
    g_shm = shm_setup(&restored);
    if (!g_shm) return EXIT_FAILURE;
    g_shm->restarts = restarts;
    if (restored)
        PG_WARN("restart #%u: restored settings PIP %.0f PEEP %.0f RR %.0f",
                restarts, g_shm->settings.pip_cmh2o, g_shm->settings.peep_cmh2o,
                g_shm->settings.rate_bpm);

    g_plant_fd = open(PG_DEV_PLANT, O_RDWR);
    if (g_plant_fd == -1) {
        PG_ERR("open(%s): %s - is sensord running?", PG_DEV_PLANT, strerror(errno));
        return EXIT_FAILURE;
    }
    g_wdog_coid = name_open(PG_WDOG_NAME, 0);
    if (g_wdog_coid == -1)
        PG_WARN("watchdog not found (%s) - running unsupervised", strerror(errno));

    name_attach_t *att = name_attach(NULL, PG_CTRL_NAME, 0);
    if (!att) { PG_ERR("name_attach: %s", strerror(errno)); return EXIT_FAILURE; }

    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_t th;
    if (pthread_create(&th, &pa, control_thread, NULL) != EOK) {
        PG_ERR("pthread_create failed");
        return EXIT_FAILURE;
    }

    pg_drop_abilities();

    union {
        struct _pulse       pulse;
        uint16_t            type;
        pg_msg_hdr_t        hdr;
        pg_msg_settings_t   set;
        pg_msg_ai_advice_t  ai;
        pg_msg_fault_t      fault;
        char                raw[256];
    } msg;
    struct _msg_info info;

    for (;;) {
        int rcvid = MsgReceive(att->chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) { if (errno != EINTR) PG_WARN("MsgReceive: %s", strerror(errno)); continue; }
        if (rcvid == 0) {
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(msg.pulse.scoid);
            continue;
        }
        if (msg.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }
        if (msg.type > _IO_BASE && msg.type <= _IO_MAX) { MsgError(rcvid, ENOSYS); continue; }

        switch (msg.type) {
        case PG_MSG_GET_STATUS:
            reply_settings(rcvid, EOK, 0, &g_shm->settings);
            break;

        case PG_MSG_SET_SETTINGS: {
            if (info.msglen < sizeof(msg.set)) { MsgError(rcvid, EINVAL); break; }
            pg_settings_t s = msg.set.settings;
            pg_settings_t cur = g_shm->settings;
            int clamped = pg_settings_clamp(&s, &cur);
            push_settings(&s);
            PG_INFO("clinician settings PIP %.1f PEEP %.1f RR %.1f I:E 1:%.1f%s",
                    s.pip_cmh2o, s.peep_cmh2o, s.rate_bpm, s.ie_ratio,
                    clamped ? " (clamped to envelope)" : "");
            reply_settings(rcvid, EOK, clamped, &s);
            break;
        }

        case PG_MSG_AI_ADVICE:
            if (info.msglen < sizeof(msg.ai)) { MsgError(rcvid, EINVAL); break; }
            handle_ai(rcvid, &msg.ai);
            break;

        case PG_MSG_ACCEPT_AI: {
            if (!g_shm->ai_has_proposal) { MsgError(rcvid, ENOENT); break; }
            pg_settings_t s = g_shm->ai_proposed;
            pg_settings_t cur = g_shm->settings;
            int clamped = pg_settings_clamp(&s, &cur);   /* re-check: defence in depth */
            push_settings(&s);
            SHM_BEGIN(); g_shm->ai_has_proposal = 0; SHM_END();
            PG_INFO("clinician ACCEPTED AI proposal PIP %.1f PEEP %.1f RR %.1f",
                    s.pip_cmh2o, s.peep_cmh2o, s.rate_bpm);
            reply_settings(rcvid, EOK, clamped, &s);
            break;
        }

        case PG_MSG_INJECT_FAULT: {
            if (info.msglen < sizeof(msg.fault)) { MsgError(rcvid, EINVAL); break; }
            uint32_t f = msg.fault.fault_mask;
            int rc = devctl(g_plant_fd, DCMD_PG_SET_FAULTS, &f, sizeof(f), NULL);
            if (rc == EOK) { SHM_BEGIN(); g_shm->alarms_latched = 0; SHM_END(); }
            MsgReply(rcvid, rc, NULL, 0);
            break;
        }

        case PG_MSG_CRASH:
            PG_ERR("CRASH requested (demo) - aborting control process");
            MsgReply(rcvid, EOK, NULL, 0);
            abort();

        default:
            MsgError(rcvid, ENOSYS);
        }
    }
    return 0;
}
