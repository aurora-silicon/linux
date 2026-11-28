/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Apple DockChannel HID transport: MTP interface power commands
 */
#ifndef _DOCKCHANNEL_HID_PROTOCOL_H
#define _DOCKCHANNEL_HID_PROTOCOL_H

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

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

/* IOKit kIOReturnBadArgument: returned when firmware rejects power method 1. */
#define DCHID_ERR_BAD_ARGUMENT		0xe00002c2

static inline bool dchid_pm1_needs_pm2_fallback(int ret, u32 retcode,
						bool pm_explicit,
						bool use_ring,
						bool has_afe_reset)
{
	return ret == -EIO && retcode == DCHID_ERR_BAD_ARGUMENT &&
	       !pm_explicit && !use_ring && has_afe_reset;
}

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

/*
 * Newer trackpad firmware ("SensingAlgsTrackpad26C", hid-magicmouse's TP26
 * frames) sends no report ID: a frame is 0x5a 0x01, a frame type (2 for data),
 * a header to offset 31, then records of 0x02, a 16-byte stream id, 0x02, two
 * type bytes, a little-endian 16-bit length and the payload. The button
 * stream's payload is a 32-bit word with bit 0 set while the pad is clicked.
 */
#define DCHID_TP26_HEADER_SIZE		31
#define DCHID_TP26_RECORD_HDR		22

static const u8 dchid_tp26_button_id[16] = {
	0x91, 0x62, 0x0e, 0x00, 0x8a, 0x5d, 0x81, 0xd5,
	0xc2, 0x87, 0x93, 0x63, 0x28, 0x29, 0xc8, 0xbf,
};

/* Whether a TP26 data frame carries a button stream record that says clicked. */
static inline bool dchid_tp26_clicked(const u8 *data, size_t size)
{
	size_t pos;

	if (size < DCHID_TP26_HEADER_SIZE || data[0] != 0x5a || data[1] != 0x01 ||
	    data[2] != 2)
		return false;
	for (pos = DCHID_TP26_HEADER_SIZE; pos + DCHID_TP26_RECORD_HDR <= size;) {
		u16 len = get_unaligned_le16(data + pos + 20);

		if (data[pos] != 0x02 || data[pos + 17] != 0x02 ||
		    len > size - pos - DCHID_TP26_RECORD_HDR)
			return false;
		if (len >= 4 && !memcmp(data + pos + 1, dchid_tp26_button_id,
					 sizeof(dchid_tp26_button_id)))
			return get_unaligned_le32(data + pos + DCHID_TP26_RECORD_HDR) & 1;
		pos += DCHID_TP26_RECORD_HDR + len;
	}
	return false;
}

/*
 * Whether a failed receive-ring command is to be absorbed. Only a node that
 * has an MMIO FIFO receive path of its own (fifo_fallback) may fall back to
 * it; the T6041 ring is its only receive path and its failure must fail the
 * interface start.
 */
static inline bool dchid_ring_refused(int ret, bool fifo_fallback)
{
	return ret < 0 && fifo_fallback;
}

/*
 * Whether the ring stays unused for the session. Only a refused CMD_REGISTER_RING does:
 * when the preparation command failed, CMD_REGISTER_RING was never sent, so nothing
 * was refused and a later interface start may register the ring.
 */
static inline bool dchid_ring_stays_on_fifo(bool prepared, int ret, bool fifo_fallback)
{
	return prepared && dchid_ring_refused(ret, fifo_fallback);
}

#endif /* _DOCKCHANNEL_HID_PROTOCOL_H */
