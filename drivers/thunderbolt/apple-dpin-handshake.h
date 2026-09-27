/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_DPIN_HANDSHAKE_H
#define _APPLE_DPIN_HANDSHAKE_H

#include <linux/errno.h>

#define APPLE_DPIN_HPD 0x00
#define APPLE_DPIN_CONTROL 0x0c
#define APPLE_DPIN_ACK 0x10
#define APPLE_DPIN_HPD_LEVEL (1U << 2)
#define APPLE_DPIN_INACTIVE 1U

/* Activation sets CONNECTED without clearing unrelated HPD or CONTROL bits. */
#define APPLE_DPIN_CONNECTED (1U << 1)

/*
 * The caller supplies the experimental DPIN mode value. Bound it to the
 * four-bit MODE_B field and the range of one-hot MODE_A bits that teardown
 * clears, so changing the parameter cannot modify unrelated register bits.
 */
#define APPLE_DPIN_MODE_A 0x14
#define APPLE_DPIN_MODE_B 0x1c
#define APPLE_DPIN_MODE_VALUE_MAX 15U

/* Caller owns powered register access and provides a bounded wait. */
struct apple_dpin_io {
	unsigned int (*read)(void *ctx, unsigned int offset);
	void (*write)(void *ctx, unsigned int offset, unsigned int value);
	int (*wait)(void *ctx);
	void *ctx;
};

static inline int apple_dpin_handshake(const struct apple_dpin_io *io,
				      int active, unsigned int mode_value)
{
	unsigned int hpd, saved, value, ack;
	int ret;

	if (active && mode_value > APPLE_DPIN_MODE_VALUE_MAX)
		return -EINVAL;

	if (active) {
		hpd = io->read(io->ctx, APPLE_DPIN_HPD);
		if (hpd == ~0U)
			return -EIO;
		if (!(hpd & APPLE_DPIN_HPD_LEVEL))
			return -ENOLINK;
	}
	saved = io->read(io->ctx, APPLE_DPIN_CONTROL);
	if (saved == ~0U)
		return -EIO;
	if (active) {
		unsigned int mode_a, mode_b;

		io->write(io->ctx, APPLE_DPIN_HPD, hpd | APPLE_DPIN_CONNECTED);

		mode_b = io->read(io->ctx, APPLE_DPIN_MODE_B);
		if (mode_b != ~0U)
			io->write(io->ctx, APPLE_DPIN_MODE_B,
				  mode_b | (mode_value << 7));

		mode_a = io->read(io->ctx, APPLE_DPIN_MODE_A);
		if (mode_a != ~0U)
			io->write(io->ctx, APPLE_DPIN_MODE_A,
				  (mode_a & ~0xffU) |
				  (1U << mode_value));
	} else {
		unsigned int mode_a, mode_b;

		/* Clear only the fields that an in-range mode value can modify. */
		mode_b = io->read(io->ctx, APPLE_DPIN_MODE_B);
		if (mode_b != ~0U)
			io->write(io->ctx, APPLE_DPIN_MODE_B,
				  mode_b & ~(APPLE_DPIN_MODE_VALUE_MAX << 7));

		mode_a = io->read(io->ctx, APPLE_DPIN_MODE_A);
		if (mode_a != ~0U)
			io->write(io->ctx, APPLE_DPIN_MODE_A,
				  mode_a & ~((1U << (APPLE_DPIN_MODE_VALUE_MAX + 1)) - 1));
	}
	value = active ? (saved & ~APPLE_DPIN_INACTIVE) | APPLE_DPIN_CONNECTED :
			 saved | APPLE_DPIN_INACTIVE;
	io->write(io->ctx, APPLE_DPIN_CONTROL, value);
	for (;;) {
		ack = io->read(io->ctx, APPLE_DPIN_ACK);
		if (ack == ~0U) {
			ret = -EIO;
			break;
		}
		if ((ack & APPLE_DPIN_INACTIVE) ==
		    (value & APPLE_DPIN_INACTIVE))
			return 0;
		if (active) {
			hpd = io->read(io->ctx, APPLE_DPIN_HPD);
			if (hpd == ~0U || !(hpd & APPLE_DPIN_HPD_LEVEL)) {
				ret = hpd == ~0U ? -EIO : -ENOLINK;
				break;
			}
		}
		ret = io->wait(io->ctx);
		if (ret)
			break;
	}
	/* Restore the owned CONTROL bits after a failed handshake. */
	ack = io->read(io->ctx, APPLE_DPIN_CONTROL);
	if (ack != ~0U)
		io->write(io->ctx, APPLE_DPIN_CONTROL,
			  (ack & ~(APPLE_DPIN_INACTIVE | APPLE_DPIN_CONNECTED)) |
			  (saved & (APPLE_DPIN_INACTIVE | APPLE_DPIN_CONNECTED)));
	return ret;
}
#endif
