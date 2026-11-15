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

#define DCHID_IFACE_COMM		0

/*
 * Whether a received frame is the FIFO doorbell for the receive ring: a
 * zero-length NOTIFY frame on the FIFO. The firmware may address it to the
 * interface whose frame was queued, so the interface is not part of the test.
 */
static inline bool dchid_notify_wants_drain(bool fifo, u8 iface, u32 length)
{
	return fifo && !length;
}

/*
 * A doorbell that finds the ring empty is looked at once more at once. One
 * that stops on a frame still being written is looked at again up to
 * DCHID_RING_RECHECKS times, DCHID_RING_RECHECK_US apart, before the frame is
 * left for the next doorbell. Only the second case sleeps: an empty ring is
 * the common case after a doorbell that the previous drain already served, and
 * a timer wakeup for each of those would add to the IRQ thread's load.
 */
#define DCHID_RING_RECHECKS		2
#define DCHID_RING_RECHECK_US		100

static inline bool dchid_ring_retry_needed(int frames, bool partial, int tries)
{
	if (partial)
		return tries < DCHID_RING_RECHECKS;
	return frames == 0 && tries < 1;
}

static inline unsigned int dchid_ring_recheck_delay_us(bool partial)
{
	return partial ? DCHID_RING_RECHECK_US : 0;
}

#endif /* _DOCKCHANNEL_HID_PROTOCOL_H */
