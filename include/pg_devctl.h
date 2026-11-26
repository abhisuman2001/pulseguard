/*
 * devctl() command codes for the /dev/pulseguard/plant resource manager.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PG_DEVCTL_H
#define PG_DEVCTL_H

#include <devctl.h>
#include "pg_proto.h"

/* One round trip per control tick: the client writes the actuator command
 * and receives the newest sensor sample in the same message pass. */
typedef union {
    pg_plant_command_t cmd;      /* in  */
    pg_plant_sample_t  sample;   /* out */
} pg_plant_xchg_t;

#define DCMD_PG_EXCHANGE    __DIOTF(_DCMD_MISC, 0x50, pg_plant_xchg_t)
#define DCMD_PG_GET_SAMPLE  __DIOF (_DCMD_MISC, 0x51, pg_plant_sample_t)
#define DCMD_PG_SET_FAULTS  __DIOT (_DCMD_MISC, 0x52, uint32_t)

#endif
