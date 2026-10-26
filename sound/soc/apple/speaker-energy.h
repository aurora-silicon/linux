/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_SPEAKER_ENERGY_H
#define __APPLE_SPEAKER_ENERGY_H

#include <linux/spinlock.h>
#include <linux/types.h>

/* Internal ledger. Wire units are fixed at Q30 squared frames at 48 kHz. */
#define APPLE_ENERGY_SPEAKERS 6
#define APPLE_ENERGY_QUEUE 8
#define APPLE_ENERGY_MAX_FRAMES 4096
#define APPLE_ENERGY_UNIT_RATE 48000
#define APPLE_ENERGY_UNITY 0x80000000U
#define APPLE_ENERGY_SQUARED 0
#define APPLE_ENERGY_ABSOLUTE 1

struct apple_energy_snapshot {
	u64 epoch;
	u64 sequence;
	u64 clock_ns;
	u64 submitted[APPLE_ENERGY_SPEAKERS];
	u64 completed[APPLE_ENERGY_SPEAKERS];
	u64 source_submitted[APPLE_ENERGY_SPEAKERS];
	u64 source_completed[APPLE_ENERGY_SPEAKERS];
	u64 refunded[APPLE_ENERGY_SPEAKERS];
	u64 unbudgeted[APPLE_ENERGY_SPEAKERS];
	u64 ceiling[APPLE_ENERGY_SPEAKERS];
	u64 deadline_ns;
	u32 gain_q31[APPLE_ENERGY_SPEAKERS];
	u32 fallback_q31;
	u32 rate;
	u8 speakers;
	u8 pending;
	bool configured;
	bool enabled;
	bool failed;
};

struct apple_energy_ticket {
	u64 id;
	u64 energy[APPLE_ENERGY_SPEAKERS];
	u64 source[APPLE_ENERGY_SPEAKERS];
	u64 raw_cost[APPLE_ENERGY_SPEAKERS];
	u64 safe_epoch;
	bool born_safe;
	bool raw;
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
	bool configured;
	bool enabled;
	bool failed;
	u32 gain_q31[APPLE_ENERGY_SPEAKERS];
	u32 fallback_q31;
	u64 safe_epoch;
	bool hardware_safe;
	bool cache_raw;
	u32 metric;
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
void apple_energy_fallback(struct apple_energy_gate *gate, u32 gain_q31);
/* Called only after the existing hardware caps took (or before raising them). */
int apple_energy_hardware_safe(struct apple_energy_gate *gate, bool safe);
int apple_energy_metric(struct apple_energy_gate *gate, u32 metric);
int apple_energy_grant_gains(struct apple_energy_gate *gate,
		       const struct apple_energy_snapshot *snapshot,
		       const u64 *ceiling, const u32 *gains,
		       u64 now_ns, u64 deadline_ns);
/*
 * The caller owns private, normalized Q31 samples, not a userspace DMA ring.
 * Success reserves one FIFO completion ticket. Failure zeros the samples and
 * must not publish a descriptor. Exhaustion uses the independently reserved
 * safe-level gain. Raw caching is permitted only under proven hardware caps;
 * every pending raw ticket must be charged before those caps can be raised.
 */
int apple_energy_filter(struct apple_energy_gate *gate, s32 *samples,
			unsigned int frames, unsigned int channels,
			u64 now_ns, u64 *ticket);
int apple_energy_filter_pcm(struct apple_energy_gate *gate, void *samples,
			unsigned int frames, unsigned int channels, int format,
			u64 now_ns, u64 *ticket);
/* Complete only after the exact immutable descriptor is no longer readable. */
int apple_energy_complete(struct apple_energy_gate *gate, u64 ticket);
u64 apple_energy_cost(s32 sample);

#endif
