/*
 * Host unit tests for the PulseGuard portable core.
 * Build & run:  make test        (any POSIX host with a C99 compiler)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pg_core.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do {                                         \
    if (cond) { g_pass++; }                                           \
    else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
           printf(__VA_ARGS__); printf("\n"); }                       \
} while (0)

/* ---------- closed-loop helper ---------- */
typedef struct {
    float max_paw, min_paw_exp, last_vte, last_vti;
    uint32_t alarms_seen;
    pg_breath_features_t feat;
    int dx;
} run_result_t;

static run_result_t run_closed_loop(float C, float R, uint32_t faults,
                                    const pg_settings_t *s, int breaths)
{
    run_result_t r;
    memset(&r, 0, sizeof(r));
    r.min_paw_exp = 1e9f;
    pg_plant_t p; pg_plant_init(&p, C, R); pg_plant_set_faults(&p, faults);
    pg_vent_t v;  pg_vent_init(&v, s);
    pg_rc_est_t e; pg_rc_reset(&e);
    float paw = 0, flow = 0, vol = 0;
    int b = 0;
    for (long t = 0; t < 600000 && b < breaths; t++) {
        pg_vent_out_t o = pg_vent_step(&v, paw, flow, 0.001f);
        r.alarms_seen |= v.alarms;
        if (o.breath_done) {
            b++;
            if (b == breaths) { r.feat = pg_rc_solve(&e); break; }
            pg_rc_reset(&e); vol = 0;
        }
        pg_plant_step(&p, o.blower_cmd, o.exhale_open, 0.001f);
        paw = p.pao; flow = p.q_meas_lps; vol += flow;   /* 1000*dt = 1 */
        pg_rc_add(&e, paw, flow, vol, 0.001f);
        if (b >= 2) {
            if (paw > r.max_paw) r.max_paw = paw;
            if (v.phase == 1 && v.t_in_breath > 60.0f / s->rate_bpm - 0.2f &&
                paw < r.min_paw_exp) r.min_paw_exp = paw;
        }
    }
    r.last_vte = v.last_vte;
    r.last_vti = v.last_vti;
    float probs[PG_DX_COUNT];
    r.dx = pg_classify(&r.feat, probs);
    return r;
}

/* ---------- tests ---------- */
static void test_envelope(void)
{
    pg_settings_t s, d; pg_settings_default(&d);

    s = d;
    CHECK(pg_settings_clamp(&s, &d) == 0, "default settings must be inside envelope");

    s = d; s.pip_cmh2o = 80.0f;
    CHECK(pg_settings_clamp(&s, &d) == 1 && s.pip_cmh2o == PG_ENV_PIP_MAX,
          "PIP 80 must clamp to %.1f (got %.1f)", PG_ENV_PIP_MAX, s.pip_cmh2o);

    s = d; s.rate_bpm = NAN;
    CHECK(pg_settings_clamp(&s, &d) == 1 && s.rate_bpm == d.rate_bpm,
          "NaN rate must fall back to previous value");

    s = d; s.peep_cmh2o = 14.0f; s.pip_cmh2o = 10.0f;
    pg_settings_clamp(&s, &d);
    CHECK(s.pip_cmh2o >= s.peep_cmh2o + 5.0f, "PIP must stay >= PEEP + 5");

    s = d; s.peep_cmh2o = INFINITY;
    pg_settings_clamp(&s, &d);
    CHECK(s.peep_cmh2o == d.peep_cmh2o, "inf PEEP rejected");
}

static void test_step_limit(void)
{
    pg_settings_t cur, p; pg_settings_default(&cur);
    p = cur; p.pip_cmh2o += 12.0f; p.rate_bpm -= 20.0f;
    int ch = pg_settings_limit_step(&p, &cur, 2.0f, 3.0f);
    CHECK(ch == 1, "large AI step must be limited");
    CHECK(fabsf(p.pip_cmh2o - (cur.pip_cmh2o + 2.0f)) < 1e-4f, "PIP step <= 2");
    CHECK(fabsf(p.rate_bpm - (cur.rate_bpm - 3.0f)) < 1e-4f, "rate step <= 3");
}

static void test_tracking(void)
{
    pg_settings_t s; pg_settings_default(&s);
    run_result_t r = run_closed_loop(50.0f, 10.0f, 0, &s, 6);
    CHECK(fabsf(r.max_paw - s.pip_cmh2o) < 1.5f,
          "PIP tracking: target %.1f got %.2f", s.pip_cmh2o, r.max_paw);
    CHECK(fabsf(r.min_paw_exp - s.peep_cmh2o) < 1.0f,
          "PEEP tracking: target %.1f got %.2f", s.peep_cmh2o, r.min_paw_exp);
    /* healthy: Vt ~= C * (PIP-PEEP) = 750 mL (time-constant limited) */
    CHECK(r.last_vte > 550.0f && r.last_vte < 800.0f, "tidal volume %.0f mL", r.last_vte);
    CHECK((r.alarms_seen & (PG_ALM_HIGH_PRESSURE | PG_ALM_DISCONNECT | PG_ALM_LOW_VT)) == 0,
          "no alarms on healthy patient (0x%x)", r.alarms_seen);
}

static void test_estimator(void)
{
    pg_settings_t s; pg_settings_default(&s);
    run_result_t r = run_closed_loop(50.0f, 10.0f, 0, &s, 5);
    CHECK(r.feat.valid, "estimator produced a solution");
    CHECK(fabsf(r.feat.C - 50.0f) / 50.0f < 0.10f, "C est %.1f vs 50", r.feat.C);
    CHECK(fabsf(r.feat.R - 10.0f) / 10.0f < 0.15f, "R est %.1f vs 10", r.feat.R);
}

static void test_alarms(void)
{
    pg_settings_t s; pg_settings_default(&s);
    run_result_t r = run_closed_loop(50.0f, 10.0f, PG_FAULT_DISCONNECT, &s, 4);
    CHECK(r.alarms_seen & PG_ALM_DISCONNECT, "disconnect alarm raised");

    /* high-pressure: drive the controller directly with an over-pressure reading */
    pg_vent_t v; pg_vent_init(&v, &s);
    pg_vent_out_t o = pg_vent_step(&v, 45.0f, 0.0f, 0.001f);
    CHECK((v.alarms & PG_ALM_HIGH_PRESSURE) && o.blower_cmd == 0.0f && o.exhale_open,
          "over-pressure dumps within ONE control tick");
}

static void test_classifier(void)
{
    static const struct { uint32_t fault; int dx; const char *name; } cases[] = {
        { 0,                   PG_DX_NORMAL,     "normal"     },
        { PG_FAULT_LEAK,       PG_DX_LEAK,       "leak"       },
        { PG_FAULT_OCCLUSION,  PG_DX_OCCLUSION,  "occlusion"  },
        { PG_FAULT_STIFF_LUNG, PG_DX_STIFF_LUNG, "stiff lung" },
        { PG_FAULT_DISCONNECT, PG_DX_DISCONNECT, "disconnect" },
    };
    pg_settings_t s; pg_settings_default(&s);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_result_t r = run_closed_loop(60.0f, 8.0f, cases[i].fault, &s, 4);
        CHECK(r.dx == cases[i].dx, "classifier on %s -> %d", cases[i].name, r.dx);
    }
}

static void test_jitter(void)
{
    pg_jitter_t j; pg_jitter_init(&j, 50000);
    pg_jitter_add(&j, 1500);
    pg_jitter_add(&j, -500);
    pg_jitter_add(&j, 70000);
    CHECK(j.n == 3 && j.max_ns == 70000 && j.min_ns == -500, "min/max");
    CHECK(j.misses == 1, "one deadline miss");
    CHECK(j.hist[1] == 1 && j.hist[0] == 1 && j.hist[PG_JITTER_BINS - 1] == 1, "histogram bins");
}

int main(void)
{
    test_envelope();
    test_step_limit();
    test_tracking();
    test_estimator();
    test_alarms();
    test_classifier();
    test_jitter();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
