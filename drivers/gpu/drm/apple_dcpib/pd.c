// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * USB-C plug state from the port's ACE3 PD controller (J873g hpm3) over
 * SPMI, for the USB-C display's hotplug: the DP alt mode connection and the
 * plug orientation. Logical registers are read through a window (aura
 * src/ace3.c): select with a register-zero write, wait for the busy bit to
 * clear, read the size at 0x1f and the data from 0x20.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/spmi.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "dcpib.h"

#define ACE3_SELECT		0x00
#define ACE3_SELECT_BUSY	BIT(7)
#define ACE3_SIZE		0x1f
#define ACE3_DATA		0x20
#define ACE3_BURST		16

#define ACE3_REG_CMD1		0x08
#define ACE3_REG_DATA1		0x09
#define ACE3_REG_STATUS		0x1a
#define ACE3_REG_DATA_STATUS	0x5f

#define STATUS_PLUG_PRESENT	BIT(0)
#define STATUS_UPSIDE_DOWN	BIT(4)
#define DATA_DP_CONNECTION	BIT(8)
#define DATA_HPD_LEVEL		BIT(15)	/* CD321x, as tipd */

static int ace3_read(struct spmi_device *sdev, u8 reg, u8 *buf, size_t len)
{
	u8 sel, size;
	int ret, err = 0;

	ret = spmi_register_zero_write(sdev, reg);
	if (ret)
		return ret;
	ret = read_poll_timeout(spmi_ext_register_read, err,
				err || !(sel & ACE3_SELECT_BUSY), 50, 20000,
				false, sdev, ACE3_SELECT, &sel, 1);
	if (ret || err)
		return ret ?: err;
	ret = spmi_ext_register_read(sdev, ACE3_SIZE, &size, 1);
	if (ret)
		return ret;
	if (size < len)
		return -EIO;
	for (size_t done = 0; done < len; done += ACE3_BURST) {
		ret = spmi_ext_register_read(sdev, ACE3_DATA + done, buf + done,
					     min_t(size_t, len - done, ACE3_BURST));
		if (ret)
			return ret;
	}
	return 0;
}

static int ace3_select(struct spmi_device *sdev, u8 reg)
{
	u8 sel;
	int ret, err = 0;

	ret = spmi_register_zero_write(sdev, reg);
	if (ret)
		return ret;
	ret = read_poll_timeout(spmi_ext_register_read, err,
				err || !(sel & ACE3_SELECT_BUSY), 50, 20000,
				false, sdev, ACE3_SELECT, &sel, 1);
	return ret ?: err;
}

static int ace3_write(struct spmi_device *sdev, u8 reg, const u8 *buf,
		      size_t len)
{
	int ret = ace3_select(sdev, reg);

	return ret ?: spmi_ext_register_write(sdev, ACE3_DATA, (u8 *)buf, len);
}

/* A 4CC command with an optional data byte; 0 once the controller took it. */
static int ace3_cmd(struct spmi_device *sdev, const char *cmd, int data)
{
	u8 val[4];
	int ret;

	ret = spmi_command_wakeup(sdev);
	if (ret)
		return ret;
	if (data >= 0) {
		u8 d = data;

		ret = ace3_write(sdev, ACE3_REG_DATA1, &d, 1);
		if (ret)
			return ret;
	}
	ret = ace3_write(sdev, ACE3_REG_CMD1, cmd, 4);
	if (ret)
		return ret;
	for (int i = 0; i < 1000; i++) {
		ret = ace3_read(sdev, ACE3_REG_CMD1, val, sizeof(val));
		if (ret)
			return ret;
		if (!memcmp(val, "!CMD", 4))
			return -EIO;
		if (!memcmp(val, "\0\0\0\0", 4))
			return 0;
		usleep_range(1000, 2000);
	}
	return -ETIMEDOUT;
}

/*
 * Leave and enter DP alt mode again, as unplugging and plugging the cable
 * does: reset the controller (Gaid), then power the port up again once it
 * takes commands (SSPS 0, as aura). A sink that stopped answering AUX after
 * a monitor's deep sleep comes back this way.
 */
int dcpib_pd_reconnect(struct dcpib *dcp)
{
	int ret;

	/* Gaid resets the controller before it can report completion. */
	ret = spmi_command_wakeup(dcp->pd);
	if (!ret)
		ret = ace3_write(dcp->pd, ACE3_REG_CMD1, "Gaid", 4);
	if (ret)
		return ret;
	for (int i = 0; i < 60; i++) {
		msleep(500);
		ret = ace3_cmd(dcp->pd, "SSPS", 0);
		if (!ret)
			return 0;
	}
	return ret;
}

/* 1 with a DisplayPort sink in DP alt mode and HPD high, 0 without. */
int dcpib_pd_sink(struct spmi_device *pd, bool *flipped)
{
	u8 status[4], data[4];
	int ret;

	/* Without a plug the controller sleeps and ignores selects. */
	ret = spmi_command_wakeup(pd);
	if (ret)
		return ret;
	ret = ace3_read(pd, ACE3_REG_STATUS, status, sizeof(status));
	if (!ret)
		ret = ace3_read(pd, ACE3_REG_DATA_STATUS, data, sizeof(data));
	if (ret)
		return ret;
	*flipped = status[0] & STATUS_UPSIDE_DOWN;
	/* The sink answers AUX once it raised HPD, not at alt mode entry. */
	return (status[0] & STATUS_PLUG_PRESENT) &&
	       (get_unaligned_le32(data) & DATA_DP_CONNECTION) &&
	       (get_unaligned_le32(data) & DATA_HPD_LEVEL);
}

/* The PD controller @np's apple,usb-pd points at, NULL without one. */
struct spmi_device *dcpib_pd_get(struct dcpib *dcp, struct device_node *np)
{
	struct device_node *pd_np = of_parse_phandle(np, "apple,usb-pd", 0);
	struct spmi_device *pd;

	if (!pd_np)
		return NULL;
	/* Holds a reference for the lifetime of the (unbindable) device. */
	pd = spmi_find_device_by_of_node(pd_np);
	of_node_put(pd_np);
	return pd ?: ERR_PTR(-EPROBE_DEFER);
}
