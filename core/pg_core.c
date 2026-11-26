/*
 * PulseGuard portable core implementation.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pg_core.h"
#include "pg_model_weights.h"

#include <math.h>
#include <string.h>

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/* ================================================================== */
/* Plant                                                               */
/* ================================================================== */
void pg_plant_init(pg_plant_t *p, float C, float R)
{
    memset(p, 0, sizeof(*p));
    p->base_C = p->C_ml_cmh2o = C;
    p->base_R = p->R_cmh2o_lps = R;
    p->blower_tau_s = 0.015f;
    p->leak_k = 0.0f;
}

void pg_plant_set_faults(pg_plant_t *p, uint32_t faults)
{
    p->faults = faults;
    p->C_ml_cmh2o  = (faults & PG_FAULT_STIFF_LUNG) ? p->base_C * 0.4f : p->base_C;
    p->R_cmh2o_lps = (faults & PG_FAULT_OCCLUSION)  ? p->base_R * 4.0f : p->base_R;
    p->leak_k      = (faults & PG_FAULT_LEAK)       ? 0.12f : 0.0f;
}

void pg_plant_step(pg_plant_t *p, float blower_cmd, int exhale_open, float dt)
{
    (void)exhale_open; /* modelled through the blower set-point */

    /* blower dynamics: first order lag towards command */
    float a = dt / (p->blower_tau_s + dt);
    p->pao += a * (blower_cmd - p->pao);
    if (p->pao < 0.0f) p->pao = 0.0f;

    float pao_seen = p->pao;
    if (p->faults & PG_FAULT_DISCONNECT) {
        /* open circuit: pressure collapses at the wye, lung deflates */
        pao_seen = p->pao * 0.05f;
        float q_out = -(p->v_ml / p->C_ml_cmh2o) / p->R_cmh2o_lps;
        p->q_lps = q_out;
        p->v_ml += q_out * 1000.0f * dt;
        if (p->v_ml < 0.0f) p->v_ml = 0.0f;
        p->q_meas_lps = 0.9f * blower_cmd / 10.0f;   /* free flow out of the tube */
        p->vol_meas_ml += p->q_meas_lps * 1000.0f * dt;
        p->pao = pao_seen;
        return;
    }

    float palv = p->v_ml / p->C_ml_cmh2o;
    p->q_lps = (pao_seen - palv) / p->R_cmh2o_lps;
    p->v_ml += p->q_lps * 1000.0f * dt;
    if (p->v_ml < 0.0f) p->v_ml = 0.0f;

    float q_leak = p->leak_k * sqrtf(pao_seen > 0.0f ? pao_seen : 0.0f);
    p->q_meas_lps = p->q_lps + q_leak;
    p->vol_meas_ml += p->q_meas_lps * 1000.0f * dt;
}

/* ================================================================== */
/* Safety envelope                                                     */
/* ================================================================== */
void pg_settings_default(pg_settings_t *s)
{
    s->pip_cmh2o  = 20.0f;
    s->peep_cmh2o = 5.0f;
    s->rate_bpm   = 15.0f;
    s->ie_ratio   = 2.0f;
}

static int fix(float *v, float lo, float hi, float fb)
{
    if (!(*v == *v) || isinf(*v)) { *v = fb; return 1; }   /* NaN / inf */
    float c = clampf(*v, lo, hi);
    if (c != *v) { *v = c; return 1; }
    return 0;
}

int pg_settings_clamp(pg_settings_t *s, const pg_settings_t *fb)
{
    pg_settings_t d;
    if (!fb) { pg_settings_default(&d); fb = &d; }
    int ch = 0;
    ch |= fix(&s->pip_cmh2o,  PG_ENV_PIP_MIN,  PG_ENV_PIP_MAX,  fb->pip_cmh2o);
    ch |= fix(&s->peep_cmh2o, PG_ENV_PEEP_MIN, PG_ENV_PEEP_MAX, fb->peep_cmh2o);
    ch |= fix(&s->rate_bpm,   PG_ENV_RATE_MIN, PG_ENV_RATE_MAX, fb->rate_bpm);
    ch |= fix(&s->ie_ratio,   PG_ENV_IE_MIN,   PG_ENV_IE_MAX,   fb->ie_ratio);
    if (s->pip_cmh2o < s->peep_cmh2o + 5.0f) {
        s->pip_cmh2o = s->peep_cmh2o + 5.0f;
        if (s->pip_cmh2o > PG_ENV_PIP_MAX) {
            s->pip_cmh2o  = PG_ENV_PIP_MAX;
            s->peep_cmh2o = PG_ENV_PIP_MAX - 5.0f;
        }
        ch = 1;
    }
    return ch;
}

static int step_limit(float *p, float cur, float max)
{
    float d = *p - cur;
    if (d >  max) { *p = cur + max; return 1; }
    if (d < -max) { *p = cur - max; return 1; }
    return 0;
}

int pg_settings_limit_step(pg_settings_t *p, const pg_settings_t *c,
                           float max_p, float max_r)
{
    int ch = 0;
    ch |= step_limit(&p->pip_cmh2o,  c->pip_cmh2o,  max_p);
    ch |= step_limit(&p->peep_cmh2o, c->peep_cmh2o, max_p);
    ch |= step_limit(&p->rate_bpm,   c->rate_bpm,   max_r);
    ch |= step_limit(&p->ie_ratio,   c->ie_ratio,   0.5f);
    return ch;
}

/* ================================================================== */
/* Ventilator sequencer / controller                                   */
/* ================================================================== */
void pg_vent_init(pg_vent_t *v, const pg_settings_t *s)
{
    memset(v, 0, sizeof(*v));
    v->kp = 0.6f;
    v->ki = 25.0f;
    v->rise_time_s = 0.08f;
    pg_vent_apply_settings(v, s);
}

void pg_vent_apply_settings(pg_vent_t *v, const pg_settings_t *s)
{
    v->set = *s;
    (void)pg_settings_clamp(&v->set, NULL);
}

pg_vent_out_t pg_vent_step(pg_vent_t *v, float paw, float flow_lps, float dt)
{
    pg_vent_out_t o = {0};
    const pg_settings_t *s = &v->set;
    float period = 60.0f / s->rate_bpm;
    float ti = period / (1.0f + s->ie_ratio);

    v->t_in_breath += dt;
    if (v->t_in_breath >= period) {
        /* breath complete */
        v->last_vti = v->vti_ml;
        v->last_vte = v->vte_ml;
        v->last_pip = v->pip_seen;
        v->breaths++;
        o.breath_done = 1;

        v->alarms &= ~PG_ALM_LOW_VT;
        if (v->last_vte < 150.0f) v->alarms |= PG_ALM_LOW_VT;

        v->t_in_breath -= period;
        v->vti_ml = v->vte_ml = 0.0f;
        v->pip_seen = 0.0f;
    }

    int phase = (v->t_in_breath < ti) ? 0 : 1;
    if (phase != v->phase) v->integ = 0.0f;
    v->phase = phase;

    float target;
    if (phase == 0) {
        float r = v->t_in_breath / v->rise_time_s;
        if (r > 1.0f) r = 1.0f;
        target = s->peep_cmh2o + r * (s->pip_cmh2o - s->peep_cmh2o);
    } else {
        target = s->peep_cmh2o;
    }

    /* PI pressure loop with feed-forward and anti-windup */
    float err = target - paw;
    v->integ = clampf(v->integ + v->ki * err * dt, -10.0f, 10.0f);
    float cmd = target + v->kp * err + v->integ;
    cmd = clampf(cmd, 0.0f, PG_ENV_PIP_MAX + 5.0f);

    /* volumes */
    if (flow_lps > 0) v->vti_ml += flow_lps * 1000.0f * dt;
    else              v->vte_ml += -flow_lps * 1000.0f * dt;
    if (paw > v->pip_seen) v->pip_seen = paw;

    /* hard alarms - evaluated every tick */
    if (paw > PG_ALARM_PAW_HIGH) {
        v->alarms |= PG_ALM_HIGH_PRESSURE;
        cmd = 0.0f;                 /* dump pressure */
        o.exhale_open = 1;
    } else if (paw < PG_ALARM_PAW_HIGH - 5.0f) {
        v->alarms &= ~PG_ALM_HIGH_PRESSURE;
    }

    if (phase == 0 && v->t_in_breath > v->rise_time_s * 2 && paw < 2.0f) {
        v->disconnect_s += dt;
    } else if (phase == 0 && paw > 3.0f) {
        v->disconnect_s = 0.0f;
    }
    if (v->disconnect_s > 0.15f) v->alarms |= PG_ALM_DISCONNECT;
    else                         v->alarms &= ~PG_ALM_DISCONNECT;

    o.blower_cmd = cmd;
    o.exhale_open |= (phase == 1);
    o.target = target;
    return o;
}

/* ================================================================== */
/* Least-squares R/C estimator                                         */
/* ================================================================== */
void pg_rc_reset(pg_rc_est_t *e) { memset(e, 0, sizeof(*e)); }

void pg_rc_add(pg_rc_est_t *e, float paw, float q, float v, float dt)
{
    double x[3] = { q, v, 1.0 };
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) e->s[i][k] += x[i] * x[k];
        e->b[i] += x[i] * paw;
    }
    e->n++;
    if (q > 0) e->vin_ml  += q * 1000.0 * dt;
    else       e->vout_ml += -q * 1000.0 * dt;
    if (q > e->qpeak) e->qpeak = q;
    e->paw_sum += paw;
}

static int solve3(double a[3][3], double b[3], double x[3])
{
    /* Gaussian elimination with partial pivoting */
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++)
            if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return -1;
        if (p != c) {
            for (int k = 0; k < 3; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
            double t = b[c]; b[c] = b[p]; b[p] = t;
        }
        for (int r = c + 1; r < 3; r++) {
            double f = a[r][c] / a[c][c];
            for (int k = c; k < 3; k++) a[r][k] -= f * a[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = 2; r >= 0; r--) {
        double s = b[r];
        for (int k = r + 1; k < 3; k++) s -= a[r][k] * x[k];
        x[r] = s / a[r][r];
    }
    return 0;
}

pg_breath_features_t pg_rc_solve(const pg_rc_est_t *e)
{
    pg_breath_features_t f;
    memset(&f, 0, sizeof(f));
    f.leak_frac = e->vin_ml > 1.0 ? (float)((e->vin_ml - e->vout_ml) / e->vin_ml) : 0.0f;
    f.qpeak_lps = (float)e->qpeak;
    f.paw_mean  = e->n ? (float)(e->paw_sum / e->n) : 0.0f;
    f.vt_ml     = (float)e->vin_ml;
    if (e->n < 50) return f;

    double a[3][3], b[3], x[3];
    memcpy(a, e->s, sizeof(a));
    memcpy(b, e->b, sizeof(b));
    if (solve3(a, b, x) != 0 || x[1] <= 1e-6) {
        f.R = 0; f.C = 0; f.P0 = 0;
        return f;
    }
    f.R  = (float)x[0];
    f.C  = (float)(1.0 / x[1]);
    f.P0 = (float)x[2];
    f.valid = 1;
    return f;
}

/* ================================================================== */
/* Classifier                                                          */
/* ================================================================== */
void pg_features_vector(const pg_breath_features_t *f, float x[PG_NFEAT])
{
    /* log-scaled physiology + flow/pressure shape features */
    x[0] = logf(clampf(f->R,  0.5f, 200.0f));
    x[1] = logf(clampf(f->C,  1.0f, 200.0f));
    x[2] = clampf(f->leak_frac, -1.0f, 1.0f);
    x[3] = clampf(f->qpeak_lps, 0.0f, 5.0f);
    x[4] = clampf(f->paw_mean, 0.0f, 50.0f) / 10.0f;
    x[5] = clampf(f->vt_ml, 0.0f, 3000.0f) / 500.0f;
}

int pg_classify(const pg_breath_features_t *f, float probs[PG_DX_COUNT])
{
    float x[PG_NFEAT];
    pg_features_vector(f, x);
    for (int i = 0; i < PG_NFEAT; i++)
        x[i] = (x[i] - PG_MODEL_MEAN[i]) / PG_MODEL_STD[i];

    float z[PG_DX_COUNT], zmax = -1e30f;
    for (int c = 0; c < PG_DX_COUNT; c++) {
        float s = PG_MODEL_B[c];
        for (int i = 0; i < PG_NFEAT; i++) s += PG_MODEL_W[c][i] * x[i];
        z[c] = s;
        if (s > zmax) zmax = s;
    }
    float sum = 0;
    for (int c = 0; c < PG_DX_COUNT; c++) { z[c] = expf(z[c] - zmax); sum += z[c]; }
    int best = 0;
    for (int c = 0; c < PG_DX_COUNT; c++) {
        probs[c] = z[c] / sum;
        if (probs[c] > probs[best]) best = c;
    }
    return best;
}

int pg_ai_propose(int dx, const pg_breath_features_t *f,
                  const pg_settings_t *cur, pg_settings_t *p)
{
    *p = *cur;
    switch (dx) {
    case PG_DX_STIFF_LUNG:
        /* compliance fell -> tidal volume fell. Raise PIP a little, but keep
         * plateau <= 30 cmH2O (lung-protective ventilation). */
        if (f->vt_ml < 450.0f && cur->pip_cmh2o < 30.0f) {
            p->pip_cmh2o = fminf(cur->pip_cmh2o + 2.0f, 30.0f);
            p->rate_bpm  = fminf(cur->rate_bpm + 2.0f, 30.0f);
            return 1;
        }
        return 0;
    case PG_DX_OCCLUSION:
        /* high resistance -> longer expiration to avoid air trapping */
        if (cur->rate_bpm > 8.0f || cur->ie_ratio < 3.0f) {
            p->rate_bpm = fmaxf(cur->rate_bpm - 2.0f, 8.0f);
            p->ie_ratio = fminf(cur->ie_ratio + 0.5f, 3.0f);
            return 1;
        }
        return 0;
    case PG_DX_NORMAL:
        if (f->vt_ml > 750.0f) { p->pip_cmh2o = cur->pip_cmh2o - 1.0f; return 1; }
        return 0;
    default:
        return 0;   /* leak / disconnect: alarm only, a human must act */
    }
}

/* ================================================================== */
/* Jitter                                                              */
/* ================================================================== */
void pg_jitter_init(pg_jitter_t *j, int32_t thr)
{
    memset(j, 0, sizeof(*j));
    j->min_ns = INT32_MAX;
    j->max_ns = INT32_MIN;
    j->miss_threshold_ns = thr;
}

void pg_jitter_add(pg_jitter_t *j, int32_t late)
{
    j->n++;
    if (late < j->min_ns) j->min_ns = late;
    if (late > j->max_ns) j->max_ns = late;
    j->sum_ns += late;
    j->sumsq_ns += (double)late * late;
    int32_t a = late < 0 ? -late : late;
    uint32_t bin = (uint32_t)(a / 1000);
    if (bin >= PG_JITTER_BINS) bin = PG_JITTER_BINS - 1;
    j->hist[bin]++;
    if (late > j->miss_threshold_ns) j->misses++;
}

double pg_jitter_mean(const pg_jitter_t *j)
{
    return j->n ? j->sum_ns / (double)j->n : 0.0;
}

double pg_jitter_stddev(const pg_jitter_t *j)
{
    if (j->n < 2) return 0.0;
    double m = pg_jitter_mean(j);
    double v = j->sumsq_ns / (double)j->n - m * m;
    return v > 0 ? sqrt(v) : 0.0;
}
