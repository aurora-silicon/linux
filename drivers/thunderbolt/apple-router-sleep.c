// SPDX-License-Identifier: GPL-2.0-only
/* Apple host-router state transfer. The caller owns power and ring ordering. */
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/string.h>

#include "tb.h"
#include "apple-router-sleep.h"

#define APPLE_ROUTER_SAVE_SLEEP	0x8000
#define APPLE_ROUTER_RESTORE_SLEEP	0x8001
#define APPLE_ROUTER_NOT_READABLE	1

/*
 * Both operations transfer sixteen data words and zero metadata. Do not use
 * DMA addresses or interpret the returned words. A failed restore leaves the
 * saved copy intact so the transaction owner can choose its recovery path.
 */
static int apple_router_sleep_op(struct tb_switch *sw, u16 opcode, u32 *data)
{
	u32 words[17] = {}, command, status;
	ktime_t deadline;
	int ret;

	lockdep_assert_held(&sw->tb->lock);
	if (tb_route(sw))
		return -EINVAL;
	memcpy(words, data, sizeof(words) - sizeof(words[16]));
	ret = tb_sw_write(sw, words, TB_CFG_SWITCH, ROUTER_CS_9, ARRAY_SIZE(words));
	if (ret)
		return ret;
	command = opcode | ROUTER_CS_26_OV;
	ret = tb_sw_write(sw, &command, TB_CFG_SWITCH, ROUTER_CS_26, 1);
	if (ret)
		return ret;

	deadline = ktime_add_ms(ktime_get(), 1000);
	do {
		ret = tb_sw_read(sw, &command, TB_CFG_SWITCH, ROUTER_CS_26, 1);
		if (ret)
			return ret;
		if (!(command & ROUTER_CS_26_OV))
			break;
		if (ktime_compare(ktime_get(), deadline) >= 0)
			return -ETIMEDOUT;
		usleep_range(500, 1000);
	} while (true);

	if (command & ROUTER_CS_26_ONS)
		return -EOPNOTSUPP;
	status = (command & ROUTER_CS_26_STATUS_MASK) >> ROUTER_CS_26_STATUS_SHIFT;
	if (status == APPLE_ROUTER_NOT_READABLE)
		return -EIO;
	ret = tb_sw_read(sw, words, TB_CFG_SWITCH, ROUTER_CS_9, ARRAY_SIZE(words));
	if (ret)
		return ret;
	memcpy(data, words, sizeof(words) - sizeof(words[16]));
	return 0;
}

int apple_router_save_sleep(struct tb_switch *sw, struct apple_router_sleep *state)
{
	u32 data[ARRAY_SIZE(state->data)] = {};
	int ret;

	if (state->valid)
		return -EBUSY;
	ret = apple_router_sleep_op(sw, APPLE_ROUTER_SAVE_SLEEP, data);
	if (ret)
		return ret;
	memcpy(state->data, data, sizeof(data));
	state->valid = true;
	return 0;
}

int apple_router_restore_sleep(struct tb_switch *sw, struct apple_router_sleep *state)
{
	u32 data[ARRAY_SIZE(state->data)];
	int ret;

	if (!state->valid)
		return -ENODATA;
	memcpy(data, state->data, sizeof(data));
	ret = apple_router_sleep_op(sw, APPLE_ROUTER_RESTORE_SLEEP, data);
	if (ret)
		return ret;
	state->valid = false;
	return 0;
}
