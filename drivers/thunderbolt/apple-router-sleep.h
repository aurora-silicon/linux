/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_ROUTER_SLEEP_H
#define _APPLE_ROUTER_SLEEP_H

#include <linux/types.h>

struct tb_switch;

/* Opaque router state, owned by one NHI for one suspend/resume transaction. */
struct apple_router_sleep {
	u32 data[16];
	bool valid;
};

int apple_router_save_sleep(struct tb_switch *sw, struct apple_router_sleep *state);
int apple_router_restore_sleep(struct tb_switch *sw, struct apple_router_sleep *state);

#endif
