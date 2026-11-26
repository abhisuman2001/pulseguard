/*
 * simgen: generate labelled per-breath feature vectors for training the
 * PulseGuard advisor. Uses exactly the same plant / controller /
 * estimator code that runs on QNX, so train and inference features match.
 *
 *   ./simgen <n_patients_per_class> <seed>  > dataset.csv
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "pg_core.h"

static double urand(void) { return rand() / (RAND_MAX + 1.0); }
static double uni(double a, double b) { return a + (b - a) * urand(); }
static double gauss(void)
{
    double u = urand() + 1e-12, v = urand();
    return sqrt(-2.0 * log(u)) * cos(2 * M_PI * v);
}

static const uint32_t class_fault[PG_DX_COUNT] = {
    0, PG_FAULT_LEAK, PG_FAULT_OCCLUSION, PG_FAULT_STIFF_LUNG, PG_FAULT_DISCONNECT
};

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 400;
    srand(argc > 2 ? (unsigned)atoi(argv[2]) : 1u);
    const float dt = 0.001f;

    printf("label,R,C,leak_frac,qpeak,paw_mean,vt\n");
    for (int cls = 0; cls < PG_DX_COUNT; cls++) {
        for (int i = 0; i < n; i++) {
            pg_plant_t p;
            pg_plant_init(&p, (float)uni(40, 90), (float)uni(4, 18));
            /* severity variation */
            pg_plant_set_faults(&p, class_fault[cls]);
            if (cls == PG_DX_LEAK)       p.leak_k = (float)uni(0.05, 0.25);
            if (cls == PG_DX_OCCLUSION)  p.R_cmh2o_lps = p.base_R * (float)uni(2.5, 6.0);
            if (cls == PG_DX_STIFF_LUNG) p.C_ml_cmh2o  = p.base_C * (float)uni(0.25, 0.55);

            pg_settings_t s;
            s.pip_cmh2o = (float)uni(14, 30);
            s.peep_cmh2o = (float)uni(3, 10);
            s.rate_bpm = (float)uni(10, 25);
            s.ie_ratio = (float)uni(1.5, 3.0);
            pg_settings_clamp(&s, NULL);

            pg_vent_t v;
            pg_vent_init(&v, &s);
            pg_rc_est_t est;
            pg_rc_reset(&est);
            float paw = 0, flow = 0, vol = 0;
            pg_vent_out_t o = {0};
            int breaths = 0;
            pg_breath_features_t f = {0};

            for (long t = 0; t < 60000 && breaths < 4; t++) {
                o = pg_vent_step(&v, paw, flow, dt);
                if (o.breath_done) {
                    breaths++;
                    if (breaths == 4) { f = pg_rc_solve(&est); break; }
                    pg_rc_reset(&est);
                    vol = 0;
                }
                pg_plant_step(&p, o.blower_cmd, o.exhale_open, dt);
                paw  = p.pao + 0.15f * (float)gauss();
                flow = p.q_meas_lps + 0.01f * (float)gauss();
                vol += flow * 1000.0f * dt;
                pg_rc_add(&est, paw, flow, vol, dt);
            }
            printf("%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f\n", cls,
                   f.R, f.C, f.leak_frac, f.qpeak_lps, f.paw_mean, f.vt_ml);
        }
    }
    return 0;
}
