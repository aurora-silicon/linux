/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Apple DockChannel HID transport: MTP interface power commands
 */
#ifndef _DOCKCHANNEL_HID_PROTOCOL_H
#define _DOCKCHANNEL_HID_PROTOCOL_H

#include <linux/string.h>
#include <linux/types.h>

/*
 * Interface power commands go to the comm interface as SET_REPORT feature
 * reports. Byte 0 is the command, byte 1 the power method, byte 2 the
 * interface number and byte 3 the target power state.
 *
 * Power method 1 has nothing more. The coprocessor sequences the device
 * itself and asks the host to pulse a reset line when it needs one.
 *
 * Power method 2 adds a has-changed flag in byte 4 and four bytes of
 * padding. Each transition is announced with the flag clear, the host then
 * drives the AFE reset line, and the transition is confirmed with the flag
 * set. The J700 MTP firmware accepts only this nine-byte form.
 */
#define DCHID_CMD_RESET_INTERFACE	0x40

#define DCHID_POWER_METHOD_1		1
#define DCHID_POWER_METHOD_2		2

#define DCHID_POWER_STATE_OFF		0
#define DCHID_POWER_STATE_ON		2

#define DCHID_PM1_CMD_SIZE		4
#define DCHID_PM2_CMD_SIZE		9

static inline void dchid_pm1_command(u8 iface, u8 state,
				     u8 cmd[DCHID_PM1_CMD_SIZE])
{
	cmd[0] = DCHID_CMD_RESET_INTERFACE;
	cmd[1] = DCHID_POWER_METHOD_1;
	cmd[2] = iface;
	cmd[3] = state;
}

static inline void dchid_pm2_command(u8 iface, u8 state, bool has_changed,
				     u8 cmd[DCHID_PM2_CMD_SIZE])
{
	memset(cmd, 0, DCHID_PM2_CMD_SIZE);
	cmd[0] = DCHID_CMD_RESET_INTERFACE;
	cmd[1] = DCHID_POWER_METHOD_2;
	cmd[2] = iface;
	cmd[3] = state;
	cmd[4] = has_changed;
}

#endif /* _DOCKCHANNEL_HID_PROTOCOL_H */
