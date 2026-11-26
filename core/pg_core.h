/*
 * PulseGuard portable core: physics, control, safety, estimation.
 * Pure C99, no OS dependencies -> unit-testable on any host and
 * reused unchanged by the QNX processes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PG_CORE_H
#define PG_CORE_H

#include <stdint.h>
#include "pg_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Patient / circuit plant (single-compartment lung)                    */
/*   Q = (Pao - Palv) / R,  Palv = V / C                               */
/* ------------------------------------------------------------------ */
typedef struct {
    /* parameters */
    float C_ml_cmh2o;       /* compliance                      */
    float R_cmh2o_lps;      /* airway resistance               */
    float blower_tau_s;     /* blower first-order lag          */
    float leak_k;           /* leak coefficient L/s per sqrt(cmH2O) */
    /* state */
    float pao;              /* airway opening pressure (cmH2O) */
    float v_ml;             /* lung volume above FRC           */
    float q_lps;            /* patient flow                    */
    float q_meas_lps;       /* flow seen by ventilator sensor  */
    float vol_meas_ml;      /* integrated measured volume      */
    uint32_t faults;        /* PG_FAULT_*                      */
    float base_C, base_R;   /* healthy values                  */
} pg_plant_t;

void  pg_plant_init(pg_plant_t *p, float C, float R);
void  pg_plant_set_faults(pg_plant_t *p, uint32_t faults);
/* advance by dt seconds with given blower command; failsafe => vent to atm/PEEP */
void  pg_plant_step(pg_plant_t *p, float blower_cmd, int exhale_open, float dt);

/* ------------------------------------------------------------------ */
/* Safety envelope                                                     */
/* ------------------------------------------------------------------ */
void  pg_settings_default(pg_settings_t *s);
/* clamps in place; returns 1 if anything was changed, 0 otherwise.
 * also enforces PEEP + 5 <= PIP. Rejects NaN by restoring 'fallback'. */
int   pg_settings_clamp(pg_settings_t *s, const pg_settings_t *fallback);
/* AI proposals are further rate limited: max step per proposal */
int   pg_settings_limit_step(pg_settings_t *proposed, const pg_settings_t *cur,
                             float max_pressure_step, float max_rate_step);

/* ------------------------------------------------------------------ */
/* Breath sequencer + pressure controller (runs in the 1 kHz loop)     */
/* ------------------------------------------------------------------ */
typedef struct {
    pg_settings_t set;
    float  t_in_breath;     /* s since breath start               */
    int    phase;           /* 0 insp, 1 exp                      */
    float  integ;           /* PI integrator                      */
    float  kp, ki;
    float  rise_time_s;     /* pressure ramp                       */
    /* per-breath accumulators */
    float  vti_ml, vte_ml, pip_seen;
    /* last completed breath */
    float  last_vti, last_vte, last_pip;
    uint32_t breaths;
    uint32_t alarms;        /* PG_ALM_* (current)                  */
    float  disconnect_s;    /* time with paw < 2 cmH2O in insp     */
} pg_vent_t;

typedef struct {
    float blower_cmd;
    int   exhale_open;
    int   breath_done;      /* 1 on the tick that completes a breath */
    float target;
} pg_vent_out_t;

void  pg_vent_init(pg_vent_t *v, const pg_settings_t *s);
void  pg_vent_apply_settings(pg_vent_t *v, const pg_settings_t *s);
pg_vent_out_t pg_vent_step(pg_vent_t *v, float paw, float flow_lps, float dt);

/* ------------------------------------------------------------------ */
/* Per-breath least-squares estimator   Paw = R*Q + V/C + P0           */
/* ------------------------------------------------------------------ */
typedef struct {
    double s[3][3];          /* normal matrix  X^T X */
    double b[3];             /* X^T y                */
    uint32_t n;
    /* feature accumulators */
    double vin_ml, vout_ml, qpeak, paw_sum;
} pg_rc_est_t;

typedef struct {
    float R;                 /* cmH2O/(L/s)                 */
    float C;                 /* mL/cmH2O                    */
    float P0;
    float leak_frac;         /* (Vin - Vout) / Vin          */
    float qpeak_lps;
    float paw_mean;
    float vt_ml;
    int   valid;
} pg_breath_features_t;

void  pg_rc_reset(pg_rc_est_t *e);
void  pg_rc_add(pg_rc_est_t *e, float paw, float q_lps, float v_ml, float dt);
pg_breath_features_t pg_rc_solve(const pg_rc_est_t *e);

/* ------------------------------------------------------------------ */
/* Tiny classifier (softmax regression, weights trained offline)       */
/* ------------------------------------------------------------------ */
#define PG_NFEAT 6
void  pg_features_vector(const pg_breath_features_t *f, float x[PG_NFEAT]);
int   pg_classify(const pg_breath_features_t *f, float probs[PG_DX_COUNT]);

/* Advisor policy: small, explainable setting changes for a diagnosis.
 * Returns 1 if a proposal was written to *p. Proposals are advisory:
 * controld clamps, rate-limits and requires clinician acceptance. */
int   pg_ai_propose(int dx, const pg_breath_features_t *f,
                    const pg_settings_t *cur, pg_settings_t *p);

/* ------------------------------------------------------------------ */
/* Jitter statistics                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t n;
    int32_t  min_ns, max_ns;
    double   sum_ns, sumsq_ns;
    uint64_t hist[PG_JITTER_BINS];   /* 1 us bins, last = overflow */
    uint64_t misses;                 /* lateness > threshold        */
    int32_t  miss_threshold_ns;
} pg_jitter_t;

void  pg_jitter_init(pg_jitter_t *j, int32_t miss_threshold_ns);
void  pg_jitter_add(pg_jitter_t *j, int32_t late_ns);
double pg_jitter_mean(const pg_jitter_t *j);
double pg_jitter_stddev(const pg_jitter_t *j);

#ifdef __cplusplus
}
#endif
#endif
