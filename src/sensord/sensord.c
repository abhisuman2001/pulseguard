/*
 * sensord - simulated ventilator hardware exposed as a QNX resource manager.
 *
 *   /dev/pulseguard/plant     devctl(DCMD_PG_EXCHANGE)   command in, sample out
 *                             devctl(DCMD_PG_GET_SAMPLE) read-only sample
 *                             devctl(DCMD_PG_SET_FAULTS) fault injection (demo)
 *                             read()                     human readable status
 *
 * The plant physics advance on their own 1 kHz timer thread, exactly like a
 * real sensor/actuator board would. Swapping this process for a real driver
 * (I2C pressure sensor, PWM blower) requires no change in controld: it only
 * knows the path name. That is the QNX resource-manager contract.
 *
 * Hardware fail-safe: if no actuator command arrives for PG_PLANT_FAILSAFE_MS
 * (controller crashed / hung) the "hardware" opens the exhale valve and
 * drops the blower to a safe ambient level, as a real ventilator's
 * independent safety valve would.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "pg_core.h"
#include "pg_devctl.h"
#include "../common/pg_rt.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* PI by default on QNX */
static pg_plant_t      g_plant;
static pg_plant_command_t g_cmd;
static uint64_t        g_last_cmd_ns;
static uint64_t        g_t0_ns;
static int             g_failsafe;

static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t      g_io_funcs;
static iofunc_attr_t          g_attr;

/* ---------------------------------------------------------------- */
static void fill_sample(pg_plant_sample_t *s)
{
    s->t_ns       = pg_now_ns() - g_t0_ns;
    s->paw_cmh2o  = g_plant.pao;
    s->flow_lpm   = g_plant.q_meas_lps * 60.0f;
    s->volume_ml  = g_plant.vol_meas_ml;
    s->failsafe   = (uint32_t)g_failsafe;
    s->fault_mask = g_plant.faults;
}

static void *plant_thread(void *arg)
{
    (void)arg;
    pg_periodic_t per;

    pg_set_fifo(PG_PRIO_PLANT);
    if (pg_periodic_start(&per, PG_PLANT_PERIOD_NS, PG_PRIO_PLANT, PG_PULSE_TICK) == -1) {
        PG_ERR("periodic timer: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    uint64_t last = pg_now_ns();
    for (;;) {
        pg_periodic_wait(&per, PG_PULSE_TICK);
        uint64_t now = pg_now_ns();
        float dt = (float)(now - last) * 1e-9f;
        last = now;
        if (dt > 0.01f) dt = 0.01f;

        pthread_mutex_lock(&g_lock);
        int stale = (now - g_last_cmd_ns) > (uint64_t)PG_PLANT_FAILSAFE_MS * 1000000ull;
        if (stale && !g_failsafe) {
            g_failsafe = 1;
            PG_WARN("no controller command for %d ms -> HARDWARE FAIL-SAFE (valve open)",
                    PG_PLANT_FAILSAFE_MS);
        }
        float blower = g_failsafe ? 0.0f : g_cmd.blower_cmh2o;
        int   valve  = g_failsafe ? 1 : (int)g_cmd.exhale_valve_open;
        pg_plant_step(&g_plant, blower, valve, dt);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb)
{
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) return status;

    void    *in = _DEVCTL_DATA(msg->i);
    size_t   in_len = msg->i.nbytes;
    unsigned nbytes = 0;
    pg_plant_xchg_t x;

    switch (msg->i.dcmd) {
    case DCMD_PG_EXCHANGE:
        if (in_len < sizeof(pg_plant_command_t)) return EINVAL;
        memcpy(&x.cmd, in, sizeof(x.cmd));
        pthread_mutex_lock(&g_lock);
        g_cmd = x.cmd;
        g_last_cmd_ns = pg_now_ns();
        if (g_failsafe) {
            g_failsafe = 0;
            PG_INFO("controller back (seq %u) -> leaving fail-safe", x.cmd.seq);
        }
        fill_sample(&x.sample);
        pthread_mutex_unlock(&g_lock);
        nbytes = sizeof(x.sample);
        break;

    case DCMD_PG_GET_SAMPLE:
        pthread_mutex_lock(&g_lock);
        fill_sample(&x.sample);
        pthread_mutex_unlock(&g_lock);
        nbytes = sizeof(x.sample);
        break;

    case DCMD_PG_SET_FAULTS: {
        uint32_t f;
        if (in_len < sizeof(f)) return EINVAL;
        memcpy(&f, in, sizeof(f));
        pthread_mutex_lock(&g_lock);
        pg_plant_set_faults(&g_plant, f);
        pthread_mutex_unlock(&g_lock);
        PG_INFO("fault mask set to 0x%x", f);
        return EOK;
    }

    default:
        return ENOSYS;
    }

    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.ret_val = 0;
    msg->o.nbytes = nbytes;
    memcpy(_DEVCTL_DATA(msg->o), &x, nbytes);
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + nbytes);
}

/* `cat /dev/pulseguard/plant` prints a status line: handy on the target */
static int io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

    char buf[256];
    pg_plant_sample_t s;
    pthread_mutex_lock(&g_lock);
    fill_sample(&s);
    pthread_mutex_unlock(&g_lock);
    int len = snprintf(buf, sizeof(buf),
        "paw=%.2f cmH2O flow=%.1f L/min vol=%.0f mL failsafe=%u faults=0x%x\n",
        s.paw_cmh2o, s.flow_lpm, s.volume_ml, s.failsafe, s.fault_mask);

    off_t off = ocb->offset;
    size_t n = 0;
    if (off < len) {
        n = (size_t)(len - off);
        if (n > msg->i.nbytes) n = msg->i.nbytes;
        ocb->offset += n;
    }
    /* reply now: buf lives on our stack */
    MsgReply(ctp->rcvid, (long)n, buf + (off < len ? off : 0), n);
    return _RESMGR_NOREPLY;
}

/* ---------------------------------------------------------------- */
int main(int argc, char **argv)
{
    float C = 50.0f, R = 10.0f;
    int opt;
    while ((opt = getopt(argc, argv, "C:R:")) != -1) {
        if (opt == 'C') C = strtof(optarg, NULL);
        else if (opt == 'R') R = strtof(optarg, NULL);
    }

    pg_log_init("pg_sensord");
    pg_lock_memory();
    pg_plant_init(&g_plant, C, R);
    g_t0_ns = pg_now_ns();
    g_last_cmd_ns = 0;
    g_failsafe = 1;                       /* until a controller talks to us */

    dispatch_t *dpp = dispatch_create();
    if (!dpp) { PG_ERR("dispatch_create: %s", strerror(errno)); return EXIT_FAILURE; }

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect_funcs,
                     _RESMGR_IO_NFUNCS, &g_io_funcs);
    g_io_funcs.devctl = io_devctl;
    g_io_funcs.read   = io_read;
    iofunc_attr_init(&g_attr, S_IFCHR | 0660, NULL, NULL);

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max   = 1;
    rattr.msg_max_size = 2048;

    if (resmgr_attach(dpp, &rattr, PG_DEV_PLANT, _FTYPE_ANY, 0,
                      &g_connect_funcs, &g_io_funcs, &g_attr) == -1) {
        PG_ERR("resmgr_attach(%s): %s", PG_DEV_PLANT, strerror(errno));
        return EXIT_FAILURE;
    }

    pthread_t th;
    pthread_create(&th, NULL, plant_thread, NULL);

    PG_INFO("plant online at %s (C=%.0f mL/cmH2O, R=%.0f cmH2O/L/s)", PG_DEV_PLANT, C, R);
    pg_drop_abilities();

    /* Single server thread. It has no fixed priority of its own that
     * matters: QNX message passing makes it run at the priority of the
     * client it is serving (priority inheritance), so the 1 kHz control
     * loop's requests are handled at the control loop's priority. */
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        ctp = dispatch_block(ctp);
        if (!ctp) { PG_ERR("dispatch_block: %s", strerror(errno)); continue; }
        dispatch_handler(ctp);
    }
    return 0;
}
