/*
 * Telemetry JSON encoder / command decoder.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pg_telemetry.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *p; size_t left; int overflow; } wbuf_t;

static void put(wbuf_t *w, const char *fmt, ...)
{
    if (w->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->p, w->left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->left) { w->overflow = 1; return; }
    w->p += n;
    w->left -= (size_t)n;
}

static double fin(double v) { return (v == v && !isinf(v)) ? v : 0.0; }

static void put_settings(wbuf_t *w, const pg_settings_t *s)
{
    put(w, "[%.2f,%.2f,%.2f,%.2f]", fin(s->pip_cmh2o), fin(s->peep_cmh2o),
        fin(s->rate_bpm), fin(s->ie_ratio));
}

static void put_jstr(wbuf_t *w, const char *s, size_t max)
{
    put(w, "\"");
    for (size_t i = 0; i < max && s[i]; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') put(w, "\\%c", c);
        else if ((unsigned char)c < 0x20) put(w, " ");
        else put(w, "%c", c);
    }
    put(w, "\"");
}

size_t pg_tel_frame(char *buf, size_t cap, const pg_shm_t *s, const pg_wdog_shm_t *wd,
                    uint64_t *last_idx, const pg_tel_flags_t *fl, uint64_t now_ns)
{
    wbuf_t w = { buf, cap, 0 };
    uint64_t widx = s->write_idx;
    uint64_t i = *last_idx;
    if (widx - i > PG_RING_LEN / 2) i = widx - PG_RING_LEN / 2;   /* lapped / first frame */
    if (widx - i > 400) i = widx - 400;                            /* keep frames bounded  */

    put(&w, "{\"v\":1,\"emu\":%d,\"ncpu\":%d,\"s\":[", fl->emulator, fl->ncpu);
    int first = 1;
    for (; i < widx; i++) {
        if (i & 1) continue;                     /* 500 Hz to the display */
        const pg_sample_t *x = &s->ring[i % PG_RING_LEN];
        put(&w, "%s[%.1f,%.2f,%.3f,%.0f,%u,%u,%.2f]", first ? "" : ",",
            x->t_ns / 1e6, fin(x->paw), fin(x->flow), fin(x->volume),
            x->phase, x->alarms, x->jitter_ns / 1000.0);
        first = 0;
    }
    *last_idx = widx;
    put(&w, "],");

    put(&w, "\"ctl\":{\"cycles\":%llu,\"misses\":%llu,\"jmax\":%.2f,\"jmin\":%.2f,"
            "\"cpu\":%u,\"restarts\":%u,\"breaths\":%u,\"vt\":%.0f,\"pip\":%.1f,"
            "\"latched\":%u,\"faults\":%u,\"pid\":%u,\"set\":",
        (unsigned long long)s->cycles, (unsigned long long)s->deadline_misses,
        s->jitter_max_ns / 1000.0, s->jitter_min_ns / 1000.0, s->control_cpu,
        s->restarts, s->breath_count, fin(s->last_vt_ml), fin(s->last_pip),
        s->alarms_latched, s->fault_mask, s->pid);
    put_settings(&w, &s->settings);
    put(&w, ",\"hist\":[");
    for (int b = 0; b < PG_JITTER_BINS; b++)
        put(&w, "%s%llu", b ? "," : "", (unsigned long long)s->jitter_hist[b]);
    put(&w, "]},");

    double ai_age = s->ai_last_ns ? (now_ns - s->ai_last_ns) / 1e6 : -1.0;
    put(&w, "\"ai\":{\"dx\":%u,\"conf\":%.3f,\"C\":%.1f,\"R\":%.1f,\"state\":%u,"
            "\"recv\":%u,\"clamped\":%u,\"rejected\":%u,\"has_prop\":%u,"
            "\"cpu_ms\":%.2f,\"age_ms\":%.0f,\"rogue\":%d,\"prop\":",
        s->ai_dx, fin(s->ai_confidence), fin(s->ai_compliance), fin(s->ai_resistance),
        s->ai_state, s->ai_advice_received, s->ai_advice_clamped, s->ai_advice_rejected,
        s->ai_has_proposal, s->ai_cpu_ns / 1e6, ai_age, fl->rogue_on);
    put_settings(&w, &s->ai_proposed);
    put(&w, "},");

    if (wd) {
        double age = wd->last_event_ns ? (now_ns - wd->last_event_ns) / 1e6 : -1.0;
        put(&w, "\"wd\":{\"restarts\":[%u,%u,%u,%u],\"detect_us\":%llu,\"recov_us\":%llu,"
                "\"age_ms\":%.0f,\"text\":",
            wd->restarts[0], wd->restarts[1], wd->restarts[2], wd->restarts[3],
            (unsigned long long)wd->last_detect_latency_us,
            (unsigned long long)wd->last_recovery_us, age);
        put_jstr(&w, wd->last_event_text, sizeof(wd->last_event_text));
        put(&w, "},");
    }
    put(&w, "\"stress\":%d}\n", fl->stress_on);
    return w.overflow ? 0 : (size_t)(w.p - buf);
}

/* ---------------------------------------------------------------- */
static int get_num(const char *line, const char *key, double *out)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    char *end;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static int cmd_is(const char *line, const char *name)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"cmd\":\"%s\"", name);
    if (strstr(line, pat)) return 1;
    snprintf(pat, sizeof(pat), "\"cmd\": \"%s\"", name);
    return strstr(line, pat) != NULL;
}

int pg_cmd_parse(const char *line, pg_cmd_t *c)
{
    double v;
    memset(c, 0, sizeof(*c));
    if (cmd_is(line, "settings")) {
        c->kind = PG_CMD_SETTINGS;
        c->settings.pip_cmh2o  = get_num(line, "pip",  &v) ? (float)v : NAN;
        c->settings.peep_cmh2o = get_num(line, "peep", &v) ? (float)v : NAN;
        c->settings.rate_bpm   = get_num(line, "rr",   &v) ? (float)v : NAN;
        c->settings.ie_ratio   = get_num(line, "ie",   &v) ? (float)v : NAN;
    } else if (cmd_is(line, "fault")) {
        c->kind = PG_CMD_FAULT;
        c->mask = get_num(line, "mask", &v) ? (uint32_t)v : 0;
    } else if (cmd_is(line, "crash")) {
        c->kind = PG_CMD_CRASH;
    } else if (cmd_is(line, "accept_ai")) {
        c->kind = PG_CMD_ACCEPT_AI;
    } else if (cmd_is(line, "rogue")) {
        c->kind = PG_CMD_ROGUE;
        c->on = get_num(line, "on", &v) ? (v != 0) : 1;
    } else if (cmd_is(line, "stress")) {
        c->kind = PG_CMD_STRESS;
        c->on = get_num(line, "on", &v) ? (v != 0) : 1;
    }
    return c->kind;
}
