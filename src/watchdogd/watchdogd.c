/*
 * watchdogd - supervisor and launcher for the PulseGuard system.
 *
 *  - Spawns sensord -> controld -> aiguard -> telemetryd in dependency order.
 *  - controld sends a heartbeat *pulse* every 10 ms; if none arrives for
 *    PG_HEARTBEAT_TIMEOUT_MS, or the process exits, controld is killed and
 *    re-spawned. It restores the clinician's settings from shared memory.
 *    Meanwhile the plant's independent hardware fail-safe protects the
 *    patient (see sensord).
 *  - aiguard sends a heartbeat every 100 ms; silence for 1.5 s (e.g. a
 *    runaway model demoted by the sporadic scheduler) -> kill + restart.
 *  - Detection latency and recovery time are measured and published.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/wait.h>

#include "pg_proto.h"
#include "pg_wdog.h"
#include "../common/pg_rt.h"

extern char **environ;

#define CHECK_PERIOD_NS   5000000L      /* 5 ms supervision tick */
#define AI_TIMEOUT_MS     1500
#define GRACE_MS          1500          /* after (re)spawn       */

typedef struct {
    const char *name;
    pid_t       pid;
    uint64_t    spawned_ns;
    uint64_t    last_beat_ns;
    int         timeout_ms;             /* 0 = no heartbeat supervision */
    int         awaiting_first_beat;
    uint64_t    detect_ns;
} child_t;

static char           g_dir[PATH_MAX];
static child_t        g_child[PG_CHILD_N] = {
    [PG_CHILD_SENSORD]    = { "pg_sensord",    -1, 0, 0, 0,                       0, 0 },
    [PG_CHILD_CONTROLD]   = { "pg_controld",   -1, 0, 0, PG_HEARTBEAT_TIMEOUT_MS, 0, 0 },
    [PG_CHILD_AIGUARD]    = { "pg_aiguard",    -1, 0, 0, AI_TIMEOUT_MS,           0, 0 },
    [PG_CHILD_TELEMETRYD] = { "pg_telemetryd", -1, 0, 0, 0,                       0, 0 },
};
static pg_wdog_shm_t *g_st;
static char           g_extra_args[PG_CHILD_N][128];

static void st_event(int idx, int reason, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void st_event(int idx, int reason, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_st->last_event_text, sizeof(g_st->last_event_text), fmt, ap);
    va_end(ap);
    g_st->last_event_ns = pg_now_ns();
    g_st->last_event_child = (uint32_t)idx;
    g_st->last_event_reason = (uint32_t)reason;
    PG_WARN("%s", g_st->last_event_text);
}

static int spawn_child(int idx)
{
    child_t *c = &g_child[idx];
    char path[PATH_MAX], rbuf[16];
    snprintf(path, sizeof(path), "%s/%s", g_dir, c->name);
    snprintf(rbuf, sizeof(rbuf), "%u", g_st->restarts[idx]);

    char *argv[8];
    int n = 0;
    argv[n++] = path;
    if (idx == PG_CHILD_CONTROLD) { argv[n++] = "-r"; argv[n++] = rbuf; }
    /* optional extra args (e.g. patient parameters for sensord) */
    char extra[128];
    strncpy(extra, g_extra_args[idx], sizeof(extra) - 1);
    extra[sizeof(extra) - 1] = 0;
    for (char *tok = strtok(extra, " "); tok && n < 7; tok = strtok(NULL, " "))
        argv[n++] = tok;
    argv[n] = NULL;

    pid_t pid;
    int rc = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (rc != 0) {
        PG_ERR("posix_spawn(%s): %s", path, strerror(rc));
        return -1;
    }
    c->pid = pid;
    c->spawned_ns = pg_now_ns();
    c->last_beat_ns = c->spawned_ns;
    g_st->pid[idx] = pid;
    PG_INFO("started %s (pid %d)", c->name, pid);
    return 0;
}

static void restart_child(int idx, int reason)
{
    child_t *c = &g_child[idx];
    uint64_t now = pg_now_ns();
    uint64_t since_beat_us = (now - c->last_beat_ns) / 1000;

    if (c->pid > 0) {
        kill(c->pid, SIGKILL);
        waitpid(c->pid, NULL, 0);
    }
    g_st->restarts[idx]++;
    g_st->last_detect_latency_us = reason == 1 ? since_beat_us : 0;
    st_event(idx, reason, "%s %s -> restart #%u",
             c->name, reason == 1 ? "heartbeat lost" : "exited",
             g_st->restarts[idx]);
    c->detect_ns = now;
    c->awaiting_first_beat = 1;
    c->pid = -1;
    spawn_child(idx);
}

static void wait_for_path(const char *p, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        if (access(p, F_OK) == 0) return;
        struct timespec ts = { 0, 10000000L };
        nanosleep(&ts, NULL);
    }
    PG_WARN("timeout waiting for %s", p);
}

static void on_beat(int idx)
{
    child_t *c = &g_child[idx];
    uint64_t now = pg_now_ns();
    c->last_beat_ns = now;
    if (c->awaiting_first_beat) {
        c->awaiting_first_beat = 0;
        g_st->last_recovery_us = (now - c->detect_ns) / 1000;
        PG_INFO("%s recovered: first heartbeat %llu us after detection",
                c->name, (unsigned long long)g_st->last_recovery_us);
    }
}

static void supervise(void)
{
    uint64_t now = pg_now_ns();

    /* reap exited children */
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < PG_CHILD_N; i++) {
            if (g_child[i].pid == pid) {
                g_child[i].pid = -1;
                restart_child(i, 2);
            }
        }
    }

    for (int i = 0; i < PG_CHILD_N; i++) {
        child_t *c = &g_child[i];
        if (!c->timeout_ms || c->pid <= 0) continue;
        if (now - c->spawned_ns < (uint64_t)GRACE_MS * 1000000ull && c->awaiting_first_beat) continue;
        if (now - c->last_beat_ns > (uint64_t)c->timeout_ms * 1000000ull)
            restart_child(i, 1);
    }
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "s:")) != -1)
        if (opt == 's')
            strncpy(g_extra_args[PG_CHILD_SENSORD], optarg, sizeof(g_extra_args[0]) - 1);

    pg_log_init("pg_watchdogd");
    pg_lock_memory();
    pg_set_fifo(PG_PRIO_WATCHDOG);

    char self[PATH_MAX];
    strncpy(self, argv[0], sizeof(self) - 1);
    self[sizeof(self) - 1] = 0;
    strncpy(g_dir, dirname(self), sizeof(g_dir) - 1);

    int fd = shm_open(PG_WDOG_SHM_NAME, O_RDWR | O_CREAT, 0644);
    if (fd == -1 || ftruncate(fd, sizeof(pg_wdog_shm_t)) == -1) {
        PG_ERR("watchdog shm: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    g_st = mmap(NULL, sizeof(*g_st), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_st == MAP_FAILED) return EXIT_FAILURE;
    memset(g_st, 0, sizeof(*g_st));
    g_st->version = PG_WDOG_VERSION;

    /* fresh system start: discard any stale control shm */
    shm_unlink(PG_SHM_NAME);

    name_attach_t *att = name_attach(NULL, PG_WDOG_NAME, 0);
    if (!att) { PG_ERR("name_attach: %s", strerror(errno)); return EXIT_FAILURE; }

    /* supervision timer delivers pulses to the same channel as heartbeats */
    int coid = ConnectAttach(0, 0, att->chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, PG_PRIO_WATCHDOG, PG_PULSE_CHECK, 0);
    timer_t tid;
    timer_create(CLOCK_MONOTONIC, &ev, &tid);
    struct itimerspec its = { { 0, CHECK_PERIOD_NS }, { 0, CHECK_PERIOD_NS } };
    timer_settime(tid, 0, &its, NULL);

    spawn_child(PG_CHILD_SENSORD);
    wait_for_path(PG_DEV_PLANT, 3000);
    spawn_child(PG_CHILD_CONTROLD);
    wait_for_path("/dev/name/local/" PG_CTRL_NAME, 3000);
    spawn_child(PG_CHILD_AIGUARD);
    spawn_child(PG_CHILD_TELEMETRYD);
    for (int i = 0; i < PG_CHILD_N; i++) g_child[i].awaiting_first_beat = 0;

    PG_INFO("PulseGuard up. Supervising (controld timeout %d ms, aiguard %d ms)",
            PG_HEARTBEAT_TIMEOUT_MS, AI_TIMEOUT_MS);

    union { struct _pulse pulse; uint16_t type; char raw[64]; } msg;
    for (;;) {
        int rcvid = MsgReceive(att->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) continue;
        if (rcvid == 0) {
            switch (msg.pulse.code) {
            case PG_PULSE_HEARTBEAT:   on_beat(PG_CHILD_CONTROLD); break;
            case PG_PULSE_AI_BEAT:     on_beat(PG_CHILD_AIGUARD);  break;
            case PG_PULSE_CHECK:       supervise();                break;
            case _PULSE_CODE_DISCONNECT: ConnectDetach(msg.pulse.scoid); break;
            default: break;
            }
            continue;
        }
        if (msg.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }
        MsgError(rcvid, ENOSYS);
    }
}
