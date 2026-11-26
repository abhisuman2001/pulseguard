/*
 * Host tests for the telemetry encoder / command parser.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pg_core.h"
#include "pg_telemetry.h"

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static pg_shm_t S;
static pg_wdog_shm_t W;
static char buf[256 * 1024];

int main(void)
{
    pg_cmd_t c;
    CHECK(pg_cmd_parse("{\"cmd\":\"settings\",\"pip\":22,\"rr\":18}", &c) == PG_CMD_SETTINGS,
          "settings parsed");
    CHECK(c.settings.pip_cmh2o == 22.0f && c.settings.rate_bpm == 18.0f, "values");
    CHECK(isnan(c.settings.peep_cmh2o), "missing field -> NaN (keeps current)");
    CHECK(pg_cmd_parse("{\"cmd\": \"fault\", \"mask\": 5}", &c) == PG_CMD_FAULT && c.mask == 5,
          "fault mask");
    CHECK(pg_cmd_parse("{\"cmd\":\"stress\",\"on\":0}", &c) == PG_CMD_STRESS && c.on == 0, "stress off");
    CHECK(pg_cmd_parse("{\"cmd\":\"rm -rf\"}", &c) == PG_CMD_NONE, "unknown rejected");
    CHECK(pg_cmd_parse("garbage", &c) == PG_CMD_NONE, "garbage rejected");

    /* frame */
    pg_settings_default(&S.settings);
    for (int i = 0; i < 100; i++) {
        S.ring[i].paw = 5.0f + i * 0.1f;
        S.ring[i].t_ns = (uint64_t)i * 1000000ull;
    }
    S.write_idx = 100;
    S.ai_proposed.rate_bpm = NAN;            /* NaN must never reach JSON */
    snprintf(W.last_event_text, sizeof(W.last_event_text), "quote\" and \\ backslash");
    uint64_t last = 0;
    pg_tel_flags_t fl = { 1, 0, 1, 4 };
    size_t n = pg_tel_frame(buf, sizeof(buf), &S, &W, &last, &fl, 123);
    CHECK(n > 0 && buf[n - 1] == '\n', "frame ends with newline");
    CHECK(last == 100, "consumed ring");
    CHECK(strstr(buf, "nan") == NULL && strstr(buf, "inf") == NULL, "no NaN/inf in JSON");
    CHECK(strstr(buf, "quote\\\" and \\\\ backslash") != NULL, "string escaping");
    CHECK(strstr(buf, "\"emu\":1") != NULL, "emulator flag");

    /* tiny buffer -> overflow reported, not truncated JSON */
    char small[64];
    last = 0;
    CHECK(pg_tel_frame(small, sizeof(small), &S, &W, &last, &fl, 0) == 0, "overflow detected");

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
