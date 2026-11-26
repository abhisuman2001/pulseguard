/*
 * telemetryd - best-effort bridge between PulseGuard and the clinician
 * dashboard (TCP port 7777, newline-delimited JSON).
 *
 * Deliberately low priority: if the network or the dashboard misbehaves,
 * nothing safety-relevant notices. Telemetry is read from shared memory
 * mapped READ-ONLY; commands go to controld/aiguard as QNX messages, where
 * they are validated like any other input.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "pg_telemetry.h"
#include "../common/pg_rt.h"

extern char **environ;

#define FRAME_NS 40000000L       /* 25 frames/s */

static const pg_shm_t      *g_shm;
static const pg_wdog_shm_t *g_wd;
static int                  g_ctrl_coid = -1, g_ai_coid = -1;
static pid_t                g_stress_pid = -1;
static int                  g_rogue;
static uint32_t             g_rogue_ai_restarts;
static char                 g_dir[PATH_MAX];
static char                 g_frame[256 * 1024];

static const void *map_ro(const char *name, size_t len)
{
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd == -1) return NULL;
    void *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

/* MsgSend with automatic reconnect (controld may have been restarted) */
static int send_to(int *coid, const char *name, const void *m, size_t len,
                   void *r, size_t rlen)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (*coid == -1) *coid = name_open(name, 0);
        if (*coid == -1) return -1;
        if (MsgSend(*coid, m, len, r, rlen) != -1) return 0;
        ConnectDetach(*coid);
        *coid = -1;
    }
    return -1;
}

static void stress(int on)
{
    if (on && g_stress_pid <= 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/pg_stressor", g_dir);
        char *argv[] = { path, NULL };
        if (posix_spawn(&g_stress_pid, path, NULL, NULL, argv, environ) != 0) {
            PG_WARN("cannot spawn %s", path);
            g_stress_pid = -1;
        } else {
            PG_WARN("STRESS ON: pid %d saturating CPUs", g_stress_pid);
        }
    } else if (!on && g_stress_pid > 0) {
        kill(g_stress_pid, SIGTERM);
        waitpid(g_stress_pid, NULL, 0);
        g_stress_pid = -1;
        PG_INFO("STRESS OFF");
    }
}

static void handle_cmd(const char *line)
{
    pg_cmd_t c;
    pg_msg_reply_t r;
    switch (pg_cmd_parse(line, &c)) {
    case PG_CMD_SETTINGS: {
        pg_msg_settings_t m = { PG_MSG_SET_SETTINGS, 0, c.settings };
        send_to(&g_ctrl_coid, PG_CTRL_NAME, &m, sizeof(m), &r, sizeof(r));
        break;
    }
    case PG_CMD_FAULT: {
        pg_msg_fault_t m = { PG_MSG_INJECT_FAULT, 0, c.mask };
        send_to(&g_ctrl_coid, PG_CTRL_NAME, &m, sizeof(m), NULL, 0);
        break;
    }
    case PG_CMD_CRASH: {
        pg_msg_hdr_t m = { PG_MSG_CRASH, 0 };
        send_to(&g_ctrl_coid, PG_CTRL_NAME, &m, sizeof(m), NULL, 0);
        break;
    }
    case PG_CMD_ACCEPT_AI: {
        pg_msg_hdr_t m = { PG_MSG_ACCEPT_AI, 0 };
        send_to(&g_ctrl_coid, PG_CTRL_NAME, &m, sizeof(m), &r, sizeof(r));
        break;
    }
    case PG_CMD_ROGUE: {
        pg_msg_rogue_t m = { PG_MSG_AI_ROGUE, 0, (uint32_t)c.on };
        if (send_to(&g_ai_coid, PG_AI_NAME, &m, sizeof(m), NULL, 0) == 0) {
            g_rogue = c.on;
            g_rogue_ai_restarts = g_wd ? g_wd->restarts[PG_CHILD_AIGUARD] : 0;
        }
        break;
    }
    case PG_CMD_STRESS:
        stress(c.on);
        break;
    default:
        PG_WARN("unknown command: %.80s", line);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    pg_log_init("pg_telemetryd");
    pg_set_fifo(PG_PRIO_TELEMETRY);
    signal(SIGPIPE, SIG_IGN);

    char self[PATH_MAX];
    strncpy(self, argv[0], sizeof(self) - 1);
    self[sizeof(self) - 1] = 0;
    strncpy(g_dir, dirname(self), sizeof(g_dir) - 1);

    for (int i = 0; i < 50 && !g_shm; i++) {
        g_shm = map_ro(PG_SHM_NAME, sizeof(pg_shm_t));
        if (!g_shm) usleep(100000);
    }
    if (!g_shm) { PG_ERR("telemetry shm not found"); return EXIT_FAILURE; }
    g_wd = map_ro(PG_WDOG_SHM_NAME, sizeof(pg_wdog_shm_t));

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PG_TELEMETRY_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) == -1 || listen(ls, 2) == -1) {
        PG_ERR("bind/listen :%d: %s", PG_TELEMETRY_PORT, strerror(errno));
        return EXIT_FAILURE;
    }
    PG_INFO("dashboard bridge listening on tcp/%d", PG_TELEMETRY_PORT);

    int cl = -1;
    char rx[1024];
    size_t rxn = 0;
    uint64_t last_idx = g_shm->write_idx;
    uint64_t next_frame = pg_now_ns();
    pg_tel_flags_t fl = { 0, 0, 0, pg_num_cpus() };

    for (;;) {
        uint64_t now = pg_now_ns();
        long wait_ns = next_frame > now ? (long)(next_frame - now) : 0;
        struct timeval tv = { 0, wait_ns / 1000 };
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        int maxfd = ls;
        if (cl != -1) { FD_SET(cl, &rf); if (cl > maxfd) maxfd = cl; }

        int n = select(maxfd + 1, &rf, NULL, NULL, &tv);
        if (n > 0 && FD_ISSET(ls, &rf)) {
            int s = accept(ls, NULL, NULL);
            if (s != -1) {
                if (cl != -1) close(cl);           /* newest dashboard wins */
                cl = s;
                setsockopt(cl, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                rxn = 0;
                last_idx = g_shm->write_idx;
                PG_INFO("dashboard connected");
            }
        }
        if (n > 0 && cl != -1 && FD_ISSET(cl, &rf)) {
            ssize_t r = recv(cl, rx + rxn, sizeof(rx) - 1 - rxn, 0);
            if (r <= 0) { close(cl); cl = -1; PG_INFO("dashboard disconnected"); }
            else {
                rxn += (size_t)r;
                rx[rxn] = 0;
                char *nl;
                while ((nl = strchr(rx, '\n')) != NULL) {
                    *nl = 0;
                    handle_cmd(rx);
                    rxn -= (size_t)(nl + 1 - rx);
                    memmove(rx, nl + 1, rxn + 1);
                }
                if (rxn >= sizeof(rx) - 1) rxn = 0;   /* garbage: drop */
            }
        }

        now = pg_now_ns();
        if (now >= next_frame) {
            next_frame = now + FRAME_NS;
            if (!g_wd) g_wd = map_ro(PG_WDOG_SHM_NAME, sizeof(pg_wdog_shm_t));
            fl.stress_on = g_stress_pid > 0;
            if (g_rogue && g_wd && g_wd->restarts[PG_CHILD_AIGUARD] != g_rogue_ai_restarts)
                g_rogue = 0;                       /* watchdog restarted a clean model */
            fl.rogue_on  = g_rogue;
            if (cl != -1) {
                size_t len = pg_tel_frame(g_frame, sizeof(g_frame), g_shm, g_wd,
                                          &last_idx, &fl, now);
                if (len && send(cl, g_frame, len, MSG_DONTWAIT) < 0 &&
                    errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(cl);
                    cl = -1;
                }
            }
        }
    }
}
