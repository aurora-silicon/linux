/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __APPLE_SPEAKER_ENERGY_H
#define __APPLE_SPEAKER_ENERGY_H

#include <linux/spinlock.h>
#include <linux/types.h>

/* Internal prototype, not a userspace ABI or an advertised capability. */
#define APPLE_ENERGY_SPEAKERS 6
#define APPLE_ENERGY_QUEUE 8
#define APPLE_ENERGY_MAX_FRAMES 4096

struct apple_energy_snapshot {
	u64 epoch;
	u64 sequence;
	u64 submitted[APPLE_ENERGY_SPEAKERS];
	u64 completed[APPLE_ENERGY_SPEAKERS];
	u32 rate;
	u8 speakers;
	u8 pending;
};

struct apple_energy_ticket {
	u64 id;
	u64 energy[APPLE_ENERGY_SPEAKERS];
};

struct apple_energy_gate {
	/* Serializes credit transactions, descriptor reservations and completions. */
	spinlock_t lock;
	struct apple_energy_snapshot state;
	u64 ceiling[APPLE_ENERGY_SPEAKERS];
	u64 deadline_ns;
	u64 next_ticket;
	struct apple_energy_ticket queue[APPLE_ENERGY_QUEUE];
	u8 slot[APPLE_ENERGY_SPEAKERS];
	u8 channels;
	u8 head;
	bool enabled;
	bool failed;
};

/* Init once for the entire ledger lifetime, never on a stream restart. */
void apple_energy_init(struct apple_energy_gate *gate);
int apple_energy_configure(struct apple_energy_gate *gate, unsigned int rate,
			   unsigned int channels, unsigned int speakers,
			   const u8 *slots);
void apple_energy_snapshot(struct apple_energy_gate *gate,
			   struct apple_energy_snapshot *snapshot);
int apple_energy_grant(struct apple_energy_gate *gate,
		       const struct apple_energy_snapshot *snapshot,
		       const u64 *ceiling, u64 now_ns, u64 deadline_ns);
void apple_energy_revoke(struct apple_energy_gate *gate);
/*
 * The caller owns private, normalized Q31 samples, not a userspace DMA ring.
 * Success reserves one FIFO completion ticket. Failure zeros the samples and
 * must not publish a descriptor. This prototype's exhaustion tail is zero.
 */
int apple_energy_filter(struct apple_energy_gate *gate, s32 *samples,
			unsigned int frames, unsigned int channels,
			u64 now_ns, u64 *ticket);
/* Complete only after the exact immutable descriptor is no longer readable. */
int apple_energy_complete(struct apple_energy_gate *gate, u64 ticket);
u64 apple_energy_cost(s32 sample);

#endif
