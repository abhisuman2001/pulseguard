/*
 * pg_emulator - HOST-ONLY stand-in for the QNX target, for dashboard
 * development and CI. Runs the same portable core (plant, controller,
 * estimator, classifier, envelope) in a single Linux process and speaks
 * the same telemetry protocol on tcp/7777.
 *
 * It is NOT the product and NOT real-time: frames carry "emu":1 and the
 * dashboard shows a banner. Crash / rogue-AI / watchdog behaviour is
 * *simulated* here; on QNX it is real (separate processes, sporadic
 * scheduler, heartbeat pulses).
 *
 *   ./pg_emulator [port]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "pg_core.h"
#include "pg_telemetry.h"

static pg_shm_t      S;
static pg_wdog_shm_t W;
static volatile int  g_stress;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *burn(void *a)
{
    (void)a;
    volatile double x = 1;
    while (g_stress) for (int i = 0; i < 100000; i++) x = sqrt(x + i);
    return NULL;
}

static void wd_event(int child, int reason, const char *txt, uint64_t t)
{
    W.restarts[child]++;
    W.last_event_ns = t;
    W.last_event_child = (uint32_t)child;
    W.last_event_reason = (uint32_t)reason;
    snprintf(W.last_event_text, sizeof(W.last_event_text), "%s -> restart #%u (emulated)",
             txt, W.restarts[child]);
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : PG_TELEMETRY_PORT;
    signal(SIGPIPE, SIG_IGN);

    S.version = PG_SHM_VERSION;
    pg_settings_default(&S.settings);
    W.version = PG_WDOG_VERSION;

    pg_plant_t plant;  pg_plant_init(&plant, 55.0f, 9.0f);
    pg_vent_t  vent;   pg_vent_init(&vent, &S.settings);
    pg_jitter_t jit;   pg_jitter_init(&jit, 200000);
    pg_rc_est_t est;   pg_rc_reset(&est);
    pg_vent_out_t out = {0};
    float breath_vol = 0, paw = 0, flow = 0;
    int prev_phase = 1, breaths_seen = 0;
    uint64_t outage_until = 0, rogue_until = 0;
    pthread_t stress_th[8];
    int nstress = 0;

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1, cl = -1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 2)) { perror("bind"); return 1; }
    fcntl(ls, F_SETFL, O_NONBLOCK);
    fprintf(stderr, "pg_emulator (HOST, not real-time) on tcp/%d\n", port);

    static char frame[256 * 1024];
    char rx[1024]; size_t rxn = 0;
    uint64_t last_idx = 0, t0 = now_ns(), k = 0, next_frame = t0;
    uint64_t start = t0;
    S.start_ns = t0;
    S.pid = (uint32_t)getpid();
    S.control_cpu = 0;

    for (;;) {
        k++;
        uint64_t due = t0 + k * 1000000ull;
        struct timespec ts = { (time_t)(due / 1000000000ull), (long)(due % 1000000000ull) };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        uint64_t now = now_ns();
        int64_t late = (int64_t)(now - due);
        if (late > 50000000) { t0 = now; k = 0; late = 0; }

        int down = now < outage_until;          /* emulated controller crash */
        if (!down) pg_jitter_add(&jit, (int32_t)late);

        pg_plant_step(&plant, down ? 0.0f : out.blower_cmd, down ? 1 : out.exhale_open, 0.001f);
        paw = plant.pao;
        flow = plant.q_meas_lps;

        if (!down) {
            out = pg_vent_step(&vent, paw, flow, 0.001f);
            if (out.breath_done) breath_vol = 0;
            breath_vol += flow;
        }

        uint64_t idx = S.write_idx;
        pg_sample_t *s = &S.ring[idx % PG_RING_LEN];
        s->t_ns = now - start; s->paw = paw; s->flow = flow; s->volume = breath_vol;
        s->blower_cmd = out.blower_cmd; s->jitter_ns = (int32_t)late;
        s->phase = (uint16_t)vent.phase;
        s->alarms = (uint16_t)(vent.alarms | (down ? PG_ALM_PLANT_FAILSAFE : 0));
        S.write_idx = idx + 1;
        S.cycles = jit.n; S.deadline_misses = jit.misses;
        S.jitter_max_ns = jit.max_ns; S.jitter_min_ns = jit.min_ns;
        if ((k & 63) == 0) memcpy(S.jitter_hist, jit.hist, sizeof(jit.hist));
        S.settings = vent.set;
        S.alarms_latched |= s->alarms;
        S.fault_mask = plant.faults;
        if (out.breath_done) {
            S.last_vt_ml = vent.last_vte; S.last_pip = vent.last_pip; S.breath_count = vent.breaths;
        }

        /* advisor (inline here; a separate sporadic-scheduled process on QNX) */
        if (now >= rogue_until && rogue_until) {
            rogue_until = 0; S.ai_state = 0;
            wd_event(PG_CHILD_AIGUARD, 1, "pg_aiguard heartbeat lost", now);
        }
        if (prev_phase == 1 && s->phase == 0) {
            if (breaths_seen++ > 0 && !rogue_until) {
                pg_breath_features_t f = pg_rc_solve(&est);
                float pr[PG_DX_COUNT];
                int dx = pg_classify(&f, pr);
                pg_settings_t p, cur = vent.set;
                int has = pg_ai_propose(dx, &f, &cur, &p);
                S.ai_advice_received++;
                S.ai_dx = (uint16_t)dx; S.ai_confidence = pr[dx];
                S.ai_compliance = f.C; S.ai_resistance = f.R;
                S.ai_last_ns = now;
                if (has && pr[dx] > 0.7f) {
                    int cl2 = pg_settings_clamp(&p, &cur);
                    cl2 |= pg_settings_limit_step(&p, &cur, 2.0f, 3.0f);
                    if (cl2) S.ai_advice_clamped++;
                    S.ai_proposed = p; S.ai_has_proposal = 1;
                } else if (dx == PG_DX_NORMAL) S.ai_has_proposal = 0;
            }
            pg_rc_reset(&est);
        }
        prev_phase = s->phase;
        pg_rc_add(&est, s->paw, s->flow, s->volume, 0.001f);

        if (now < next_frame) continue;
        next_frame = now + 40000000ull;

        /* network: accept + commands + frame */
        int c = accept(ls, NULL, NULL);
        if (c >= 0) {
            if (cl >= 0) close(cl);
            cl = c; fcntl(cl, F_SETFL, O_NONBLOCK);
            setsockopt(cl, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            last_idx = S.write_idx; rxn = 0;
        }
        if (cl >= 0) {
            ssize_t r = recv(cl, rx + rxn, sizeof(rx) - 1 - rxn, 0);
            if (r == 0 || (r < 0 && errno != EAGAIN)) { close(cl); cl = -1; continue; }
            if (r > 0) {
                rxn += (size_t)r; rx[rxn] = 0;
                char *nl;
                while ((nl = strchr(rx, '\n'))) {
                    *nl = 0;
                    pg_cmd_t cmd;
                    switch (pg_cmd_parse(rx, &cmd)) {
                    case PG_CMD_SETTINGS: {
                        pg_settings_t ns = cmd.settings, cur = vent.set;
                        pg_settings_clamp(&ns, &cur);
                        pg_vent_apply_settings(&vent, &ns);
                        break; }
                    case PG_CMD_FAULT:
                        pg_plant_set_faults(&plant, cmd.mask); S.alarms_latched = 0; break;
                    case PG_CMD_CRASH:
                        outage_until = now + 23000000ull;
                        wd_event(PG_CHILD_CONTROLD, 1, "pg_controld heartbeat lost", now);
                        W.last_detect_latency_us = 50000; W.last_recovery_us = 23000;
                        S.restarts++;
                        break;
                    case PG_CMD_ACCEPT_AI:
                        if (S.ai_has_proposal) {
                            pg_settings_t ns = S.ai_proposed, cur = vent.set;
                            pg_settings_clamp(&ns, &cur);
                            pg_vent_apply_settings(&vent, &ns);
                            S.ai_has_proposal = 0;
                        }
                        break;
                    case PG_CMD_ROGUE:
                        if (cmd.on) {
                            rogue_until = now + 1500000000ull; S.ai_state = 1;
                            S.ai_advice_clamped++;
                        }
                        break;
                    case PG_CMD_STRESS:
                        if (cmd.on && !nstress) {
                            g_stress = 1;
                            long n = sysconf(_SC_NPROCESSORS_ONLN);
                            for (int i = 0; i < n - 1 && i < 8; i++)
                                pthread_create(&stress_th[nstress++], NULL, burn, NULL);
                        } else if (!cmd.on && nstress) {
                            g_stress = 0;
                            for (int i = 0; i < nstress; i++) pthread_join(stress_th[i], NULL);
                            nstress = 0;
                        }
                        break;
                    }
                    rxn -= (size_t)(nl + 1 - rx);
                    memmove(rx, nl + 1, rxn + 1);
                }
                if (rxn >= sizeof(rx) - 1) rxn = 0;
            }
            pg_tel_flags_t fl = { nstress > 0, rogue_until != 0, 1, (int)sysconf(_SC_NPROCESSORS_ONLN) };
            size_t len = pg_tel_frame(frame, sizeof(frame), &S, &W, &last_idx, &fl, now);
            if (len && send(cl, frame, len, MSG_DONTWAIT) < 0 && errno != EAGAIN) { close(cl); cl = -1; }
        }
    }
}
