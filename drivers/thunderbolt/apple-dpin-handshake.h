/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_DPIN_HANDSHAKE_H
#define _APPLE_DPIN_HANDSHAKE_H

#include <linux/errno.h>

#define APPLE_DPIN_HPD 0x00
#define APPLE_DPIN_CONTROL 0x0c
#define APPLE_DPIN_ACK 0x10
#define APPLE_DPIN_HPD_LEVEL (1U << 2)
#define APPLE_DPIN_INACTIVE 1U

/*
 * Set unconditionally by native AppleCIODPTX::bringConnectionUp on both the
 * HPD and CONTROL registers of this exact DPIN0 block when bringing a
 * connection up (single-lane-group case, which is what this port is). The
 * HPD-register write is additionally gated there on a single/multi-stream
 * check; every topology this driver drives through DPIN0 is a single
 * external monitor (SST), so it is unconditional here too. Neither write
 * clears any other bit (native masks both with 0, a pure OR). Not written
 * on deactivate: native's teardown path was not traced, so this preserves
 * the existing "do not invent resets" behavior there.
 */
#define APPLE_DPIN_CONNECTED (1U << 1)

/*
 * Two further offsets native AppleCIODPTX::bringConnectionUp writes on
 * this exact DPIN0 block, in between the HPD and CONTROL writes above
 * (native order: HPD, then MODE_B, then MODE_A, then CONTROL). Unlike
 * CONNECTED, the exact values here are NOT confirmed from the native
 * binary: native computes them from a per-connection attributes value
 * (rate class and an unidentified secondary field) that is copied
 * verbatim from this connection's negotiation and is not itself built
 * anywhere in the traced kernel/kext code -- its origin could not be
 * pinned down statically (most likely the separate DCP coprocessor
 * firmware). The value used is therefore an informed, explicitly-labeled
 * estimate, not a confirmed constant, and is passed in by the caller
 * (mode_value below) rather than fixed at compile time, so the bounded
 * formula's one free parameter can be swept across a live connection
 * without reinstalling or rebooting between values:
 *   - bits 4-7 of the field select a rate class using the same RBR=0/
 *     HBR=1/HBR2=2/HBR3=3 ordinal already used elsewhere in this driver
 *     (drivers/thunderbolt/tb_regs.h DP_COMMON_CAP_RATE_*); this link
 *     negotiates HBR2, so 2 -- high confidence, unchanged across the
 *     whole sweep.
 *   - a secondary bit, native-gated on lane_count>=2 (true here: 4) and
 *     on the same "which DPIN0 sub-instance" selector already confirmed
 *     unconditional-0 for this single, non-split port, is set from a
 *     nearby field that other native code also treats as a small,
 *     3-valid-value enumeration (0, 1, or 2) -- this is the weak half of
 *     the formula and the only free parameter, swept across
 *     mode_value = rate_class(2) * lane_count(4) + secondary_bit = 8, 9
 *     (both already tested via separate reboots, each a clean boot with
 *     no picture), or 10.
 *
 * mode_value is bounded to APPLE_DPIN_MODE_VALUE_MAX: this caps both the
 * single bit MODE_A can set (1 << mode_value) and the field width MODE_B
 * ORs in, which is also exactly the range the deactivate path below
 * knows how to clear back to a clean baseline. Raising this bound is a
 * new, reviewable change, not a runtime knob.
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

		/*
		 * Not native teardown behavior (never traced; see the
		 * CONNECTED comment above) -- this exists solely so a later
		 * activate on this same boot starts from clean state and is
		 * a valid isolated test of a different mode_value. Bounded
		 * to exactly the bits any in-range mode_value write above
		 * could have set: MODE_B's OR'd field (bits 7..7+MAX's
		 * width) and MODE_A's cleared-low-byte-plus-one-set-bit
		 * (bits 0..MAX). Never touches bits outside that range.
		 */
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
	/*
	 * Restore only the bits we own (INACTIVE, and CONNECTED now that we
	 * set it above); do not invent resets on a failed handshake. HPD is
	 * left as-is here: native's teardown path for it was not traced, so
	 * no rollback is invented for a register this driver otherwise never
	 * wrote before this change.
	 */
	ack = io->read(io->ctx, APPLE_DPIN_CONTROL);
	if (ack != ~0U)
		io->write(io->ctx, APPLE_DPIN_CONTROL,
			  (ack & ~(APPLE_DPIN_INACTIVE | APPLE_DPIN_CONNECTED)) |
			  (saved & (APPLE_DPIN_INACTIVE | APPLE_DPIN_CONNECTED)));
	return ret;
}
#endif
