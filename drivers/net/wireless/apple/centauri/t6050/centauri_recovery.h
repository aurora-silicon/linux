/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AURORA_CENTAURI_RECOVERY_H
#define AURORA_CENTAURI_RECOVERY_H

#include <linux/ktime.h>
#include <linux/types.h>
#include "centauri_t6050.h"

int centauri_t6050_boot_phase_for_control_stage(u32 control_stage,
						enum centauri_t6050_boot_phase *phase);

void centauri_t6050_transport_recover_init(struct centauri_t6050 *c);
void centauri_t6050_transport_recover_cancel(struct centauri_t6050 *c);
void centauri_t6050_request_transport_recover(struct centauri_t6050 *c);
void centauri_t6050_schedule_transport_recover(struct centauri_t6050 *c);

bool centauri_control_idle_ready(bool event_valid, u32 stage_first,
				 u32 stage_second, bool ring_drained);

#endif
