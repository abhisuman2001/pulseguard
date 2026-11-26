/*
 * PulseGuard - mixed-criticality ventilator controller for QNX 8.0
 * Shared protocol definitions (messages, pulses, shared-memory layout).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PG_PROTO_H
#define PG_PROTO_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Names / paths                                                       */
/* ------------------------------------------------------------------ */
#define PG_DEV_PLANT        "/dev/pulseguard/plant"   /* sensord resource manager  */
#define PG_CTRL_NAME        "pulseguard/control"      /* controld name_attach()     */
#define PG_WDOG_NAME        "pulseguard/watchdog"     /* watchdogd name_attach()    */
#define PG_SHM_NAME         "/pulseguard_telemetry"   /* telemetry shared memory    */
#define PG_TELEMETRY_PORT   7777

/* ------------------------------------------------------------------ */
/* Scheduling plan (priorities 1..255 on QNX; 255 reserved-ish)        */
/* ------------------------------------------------------------------ */
#define PG_PRIO_PLANT       62   /* simulated hardware: highest            */
#define PG_PRIO_CONTROL     60   /* safety-critical 1 kHz loop              */
#define PG_PRIO_WATCHDOG    58   /* supervisor                              */
#define PG_PRIO_STRESS      55   /* hostile load (demo), every core but 0   */
#define PG_PRIO_AI_HIGH     30   /* AI advisor, sporadic: normal priority   */
#define PG_PRIO_AI_LOW       5   /* AI advisor, sporadic: budget exhausted  */
#define PG_PRIO_TELEMETRY   20   /* best-effort networking                  */

#define PG_CONTROL_PERIOD_NS   1000000L   /* 1 kHz */
#define PG_PLANT_PERIOD_NS     1000000L   /* 1 kHz */
#define PG_HEARTBEAT_TIMEOUT_MS      50   /* watchdog trip                     */
#define PG_PLANT_FAILSAFE_MS         20   /* plant opens valve if no command   */

/* ------------------------------------------------------------------ */
/* devctl() commands for /dev/pulseguard/plant                          */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t t_ns;          /* plant time                                  */
    float    paw_cmh2o;     /* airway pressure measured                    */
    float    flow_lpm;      /* flow into patient (L/min, + = inspiration)  */
    float    volume_ml;     /* integrated tidal volume                     */
    uint32_t failsafe;      /* 1 if plant is in hardware fail-safe          */
    uint32_t fault_mask;    /* injected faults currently active            */
} pg_plant_sample_t;

typedef struct {
    float    blower_cmh2o;  /* commanded blower pressure                   */
    uint32_t exhale_valve_open;
    uint32_t seq;
} pg_plant_command_t;

/* Fault injection bits (demo) */
#define PG_FAULT_LEAK        0x01u   /* circuit leak                       */
#define PG_FAULT_OCCLUSION   0x02u   /* kinked tube / mucus plug           */
#define PG_FAULT_STIFF_LUNG  0x04u   /* compliance drops (ARDS-like)       */
#define PG_FAULT_DISCONNECT  0x08u   /* patient disconnected               */

/* devctl numbers: _DEVCTL_DATA / __DIOF/__DIOT are defined in <devctl.h>
 * on QNX. We keep the numbers here and build the codes in plant_devctl.h */
#define PG_DCMD_BASE        0x5047  /* 'PG' */

/* ------------------------------------------------------------------ */
/* Messages to controld (MsgSend / MsgReceive)                          */
/* ------------------------------------------------------------------ */
enum {
    PG_MSG_BASE = 0x1000,       /* above _IO_MAX so we never clash with IO  */
    PG_MSG_GET_STATUS,
    PG_MSG_SET_SETTINGS,        /* from clinician (telemetryd)              */
    PG_MSG_AI_ADVICE,           /* from aiguard                             */
    PG_MSG_INJECT_FAULT,        /* demo: forwarded to plant                 */
    PG_MSG_CRASH,               /* demo: controld aborts itself             */
    PG_MSG_ACCEPT_AI,           /* clinician accepts pending AI proposal    */
    PG_MSG_AI_ROGUE,            /* demo -> aiguard: simulate runaway model  */
};

#define PG_AI_NAME          "pulseguard/ai"           /* aiguard name_attach()      */

/* Ventilation settings (pressure-controlled ventilation) */
typedef struct {
    float pip_cmh2o;        /* peak inspiratory pressure target  */
    float peep_cmh2o;       /* positive end-expiratory pressure  */
    float rate_bpm;         /* breaths per minute                */
    float ie_ratio;         /* inspiratory : expiratory (1:x)    */
} pg_settings_t;

/* Hard safety envelope: nothing (clinician or AI) can leave this box. */
#define PG_ENV_PIP_MIN    8.0f
#define PG_ENV_PIP_MAX   35.0f
#define PG_ENV_PEEP_MIN   0.0f
#define PG_ENV_PEEP_MAX  15.0f
#define PG_ENV_RATE_MIN   6.0f
#define PG_ENV_RATE_MAX  35.0f
#define PG_ENV_IE_MIN     1.0f
#define PG_ENV_IE_MAX     4.0f
#define PG_ALARM_PAW_HIGH 40.0f /* immediate exhale valve open */

/* AI diagnosis classes */
enum {
    PG_DX_NORMAL = 0,
    PG_DX_LEAK,
    PG_DX_OCCLUSION,
    PG_DX_STIFF_LUNG,
    PG_DX_DISCONNECT,
    PG_DX_COUNT
};

typedef struct {
    uint16_t       type;            /* PG_MSG_AI_ADVICE */
    uint16_t       dx;              /* PG_DX_*          */
    float          confidence;      /* 0..1             */
    float          est_compliance;  /* mL/cmH2O         */
    float          est_resistance;  /* cmH2O/(L/s)      */
    pg_settings_t  proposed;        /* advisory only    */
    uint32_t       has_proposal;
    uint64_t       cpu_ns;          /* advisor thread CPU time (per-thread clock) */
    uint32_t       throttled;       /* sporadic budget exhausted since last report */
    uint32_t       zero;
} pg_msg_ai_advice_t;

typedef struct {
    uint16_t       type;            /* PG_MSG_AI_ROGUE */
    uint16_t       zero;
    uint32_t       enable;
} pg_msg_rogue_t;

typedef struct {
    uint16_t       type;            /* PG_MSG_SET_SETTINGS */
    uint16_t       zero;
    pg_settings_t  settings;
} pg_msg_settings_t;

typedef struct {
    uint16_t       type;            /* PG_MSG_INJECT_FAULT */
    uint16_t       zero;
    uint32_t       fault_mask;
} pg_msg_fault_t;

typedef struct {
    uint16_t       type;
    uint16_t       zero;
} pg_msg_hdr_t;

typedef struct {
    int32_t        status;          /* 0 = accepted, else EINVAL/ERANGE */
    uint32_t       clamped;         /* 1 if request was clamped to envelope */
    pg_settings_t  active;
} pg_msg_reply_t;

/* Pulse codes (MsgSendPulse / timer pulses) */
#define PG_PULSE_TICK       (_PULSE_CODE_MINAVAIL + 0)
#define PG_PULSE_HEARTBEAT  (_PULSE_CODE_MINAVAIL + 1)
#define PG_PULSE_AI_BEAT    (_PULSE_CODE_MINAVAIL + 2)
#define PG_PULSE_CHECK      (_PULSE_CODE_MINAVAIL + 3)

/* ------------------------------------------------------------------ */
/* Telemetry shared memory (single writer: controld)                   */
/* ------------------------------------------------------------------ */
#define PG_RING_LEN      4096           /* ~4 s of 1 kHz samples        */
#define PG_JITTER_BINS   64             /* 1 us bins, last bin = overflow */

typedef struct {
    uint64_t t_ns;
    float    paw;
    float    flow;
    float    volume;
    float    blower_cmd;
    int32_t  jitter_ns;                 /* wake-up lateness vs ideal    */
    uint16_t phase;                     /* 0 = insp, 1 = exp            */
    uint16_t alarms;                    /* PG_ALM_* bits                */
} pg_sample_t;

#define PG_ALM_HIGH_PRESSURE  0x0001u
#define PG_ALM_LOW_VT         0x0002u
#define PG_ALM_DISCONNECT     0x0004u
#define PG_ALM_DEADLINE       0x0008u
#define PG_ALM_PLANT_FAILSAFE 0x0010u
#define PG_ALM_AI_SILENT      0x0020u

typedef struct {
    /* seqlock: writer increments before & after update; odd = busy   */
    volatile uint32_t seq;
    uint32_t          version;
    uint64_t          write_idx;        /* monotonically increasing     */
    pg_sample_t       ring[PG_RING_LEN];

    /* control loop health */
    uint64_t          cycles;
    uint64_t          deadline_misses;
    int32_t           jitter_max_ns;
    int32_t           jitter_min_ns;
    uint64_t          jitter_hist[PG_JITTER_BINS];
    uint32_t          control_cpu;      /* core the loop is pinned to   */
    uint32_t          restarts;         /* set by watchdog              */

    /* clinical */
    pg_settings_t     settings;
    float             last_vt_ml;
    float             last_pip;
    uint32_t          breath_count;
    uint32_t          alarms_latched;

    /* AI */
    uint16_t          ai_dx;
    uint16_t          ai_state;         /* 0 ok, 1 throttled, 2 silent  */
    float             ai_confidence;
    float             ai_compliance;
    float             ai_resistance;
    uint32_t          ai_advice_received;
    uint32_t          ai_advice_clamped;
    uint32_t          ai_advice_rejected;
    uint32_t          ai_has_proposal;
    pg_settings_t     ai_proposed;      /* already clamped + rate limited */
    uint64_t          ai_cpu_ns;        /* from per-thread CPU clock    */
    uint64_t          ai_last_ns;       /* time of last advice          */

    uint32_t          fault_mask;
    uint32_t          pid;
    uint64_t          start_ns;
} pg_shm_t;

#define PG_SHM_VERSION 2u

#endif /* PG_PROTO_H */
