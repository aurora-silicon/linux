/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AURORA_CENTAURI_RECOVERY_H
#define AURORA_CENTAURI_RECOVERY_H

#include <linux/ktime.h>
#include "centauri_t6050.h"

/*
 * M5 journal (channels/disconnect event -> scan cmd 0x1000f): failing boots
 * 0.34, 2.25, 2.51 ms; good 39.51, 43.04, 46.65 ms; no good pair under 10.79 ms.
 */
#define CEN_FW_EVENT_SCAN_SETTLE_MS 15

int centauri_t6050_boot_phase_for_control_stage(u32 control_stage,
						enum centauri_t6050_boot_phase *phase);

void centauri_t6050_transport_recover_init(struct centauri_t6050 *c);
void centauri_t6050_transport_recover_cancel(struct centauri_t6050 *c);
void centauri_t6050_request_transport_recover(struct centauri_t6050 *c);
void centauri_t6050_schedule_transport_recover(struct centauri_t6050 *c);

s64 centauri_fw_event_scan_settle_remainder_ms(bool event_valid, ktime_t event_at,
					       ktime_t now, unsigned settle_ms);

#endif
