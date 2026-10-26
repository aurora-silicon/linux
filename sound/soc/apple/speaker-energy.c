// SPDX-License-Identifier: GPL-2.0-only
/* Finite energy credit and completion ledger; no hardware adapter yet. */
#include <linux/errno.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "speaker-energy.h"

u64 apple_energy_cost(s32 sample)
{
	u64 magnitude = sample < 0 ? -(s64)sample : sample;
	u64 square = magnitude * magnitude;

	/* Q31 squared -> Q30, rounding up, without overflowing square + mask. */
	return (square >> 32) + !!(square & U32_MAX);
}

static void apple_energy_disable(struct apple_energy_gate *gate)
{
	gate->enabled = false;
	gate->deadline_ns = 0;
	memcpy(gate->ceiling, gate->state.submitted, sizeof(gate->ceiling));
}

static bool apple_energy_next(struct apple_energy_gate *gate)
{
	if (gate->failed || gate->state.sequence == U64_MAX) {
		gate->failed = true;
		apple_energy_disable(gate);
		return false;
	}
	gate->state.sequence++;
	return true;
}

void apple_energy_init(struct apple_energy_gate *gate)
{
	memset(gate, 0, sizeof(*gate));
	spin_lock_init(&gate->lock);
}

int apple_energy_configure(struct apple_energy_gate *gate, unsigned int rate,
			   unsigned int channels, unsigned int speakers,
			   const u8 *slots)
{
	unsigned long flags;
	unsigned int k;
	int ret = 0;

	spin_lock_irqsave(&gate->lock, flags);
	/* Even a refused reconfiguration cannot leave stale credit enabled. */
	apple_energy_disable(gate);
	if (!apple_energy_next(gate)) {
		ret = -EOVERFLOW;
		goto out;
	}
	if (!rate || rate > 192000 || !channels ||
	    channels > APPLE_ENERGY_SPEAKERS || !speakers ||
	    speakers > APPLE_ENERGY_SPEAKERS || !slots) {
		ret = -EINVAL;
		goto out;
	}
	for (k = 0; k < speakers; k++) {
		if (slots[k] >= channels) {
			ret = -EINVAL;
			goto out;
		}
	}
	/* A new physical mapping cannot reinterpret queued energy. */
	if (gate->state.pending) {
		ret = -EBUSY;
		goto out;
	}
	if (gate->state.epoch == U64_MAX) {
		gate->failed = true;
		ret = -EOVERFLOW;
		goto out;
	}
	gate->state.epoch++;
	gate->state.rate = rate;
	gate->state.speakers = speakers;
	gate->channels = channels;
	memcpy(gate->slot, slots, speakers);
out:
	spin_unlock_irqrestore(&gate->lock, flags);
	return ret;
}

void apple_energy_snapshot(struct apple_energy_gate *gate,
			   struct apple_energy_snapshot *snapshot)
{
	unsigned long flags;

	spin_lock_irqsave(&gate->lock, flags);
	*snapshot = gate->state;
	spin_unlock_irqrestore(&gate->lock, flags);
}

int apple_energy_grant(struct apple_energy_gate *gate,
		       const struct apple_energy_snapshot *snapshot,
		       const u64 *ceiling, u64 now_ns, u64 deadline_ns)
{
	unsigned long flags;
	unsigned int k;
	int ret = 0;

	spin_lock_irqsave(&gate->lock, flags);
	if (gate->failed) {
		ret = -EOVERFLOW;
		goto out;
	}
	if (!snapshot || !ceiling || !gate->state.rate ||
	    deadline_ns <= now_ns) {
		ret = -EINVAL;
		goto out;
	}
	if (snapshot->epoch != gate->state.epoch ||
	    snapshot->sequence != gate->state.sequence) {
		ret = -ESTALE;
		goto out;
	}
	for (k = 0; k < gate->state.speakers; k++) {
		if (ceiling[k] < gate->state.submitted[k]) {
			ret = -ERANGE;
			goto out;
		}
	}
	if (!apple_energy_next(gate)) {
		ret = -EOVERFLOW;
		goto out;
	}
	memcpy(gate->ceiling, ceiling,
	       gate->state.speakers * sizeof(*ceiling));
	gate->deadline_ns = deadline_ns;
	gate->enabled = true;
out:
	/* An invalid or stale transaction must not preserve uncertain credit. */
	if (ret) {
		apple_energy_disable(gate);
		apple_energy_next(gate);
	}
	spin_unlock_irqrestore(&gate->lock, flags);
	return ret;
}

void apple_energy_revoke(struct apple_energy_gate *gate)
{
	unsigned long flags;

	spin_lock_irqsave(&gate->lock, flags);
	apple_energy_disable(gate);
	apple_energy_next(gate);
	spin_unlock_irqrestore(&gate->lock, flags);
}

static s32 apple_energy_clamp(s32 sample, u64 remaining)
{
	u64 bound;

	if (apple_energy_cost(sample) <= remaining)
		return sample;
	/* remaining is now < 2^30, so the shift and square root cannot wrap. */
	bound = int_sqrt64(remaining << 32);
	return sample < 0 ? -(s64)bound : bound;
}

int apple_energy_filter(struct apple_energy_gate *gate, s32 *samples,
			unsigned int frames, unsigned int channels,
			u64 now_ns, u64 *ticket)
{
	struct apple_energy_ticket *entry;
	unsigned int f, ch, k;
	unsigned long flags;
	size_t bytes;
	int ret = 0;

	if (!samples || !ticket || !channels ||
	    channels > APPLE_ENERGY_SPEAKERS ||
	    frames > APPLE_ENERGY_MAX_FRAMES || !frames)
		return -EINVAL;
	bytes = (size_t)frames * channels * sizeof(*samples);
	*ticket = 0;
	spin_lock_irqsave(&gate->lock, flags);
	if (channels != gate->channels || !gate->state.rate) {
		ret = -EINVAL;
		goto fail;
	}
	if (!apple_energy_next(gate) || gate->next_ticket == U64_MAX) {
		gate->failed = true;
		ret = -EOVERFLOW;
		goto fail;
	}
	if (gate->state.pending == APPLE_ENERGY_QUEUE) {
		ret = -ENOSPC;
		goto fail;
	}
	if (now_ns >= gate->deadline_ns)
		apple_energy_disable(gate);
	entry = &gate->queue[(gate->head + gate->state.pending) %
			     APPLE_ENERGY_QUEUE];
	memset(entry, 0, sizeof(*entry));
	entry->id = ++gate->next_ticket;
	for (f = 0; f < frames; f++) {
		for (ch = 0; ch < channels; ch++) {
			s32 *sample = &samples[f * channels + ch];
			bool mapped = false;
			u64 cost;

			if (!gate->enabled)
				*sample = 0;
			for (k = 0; k < gate->state.speakers; k++) {
				u64 remaining;

				if (gate->slot[k] != ch)
					continue;
				mapped = true;
				remaining = gate->ceiling[k] - gate->state.submitted[k];
				*sample = apple_energy_clamp(*sample, remaining);
			}
			/* Unmapped words cannot bypass the physical speaker ledger. */
			if (!mapped)
				*sample = 0;
			cost = apple_energy_cost(*sample);
			for (k = 0; k < gate->state.speakers; k++) {
				if (gate->slot[k] != ch)
					continue;
				/* cost <= remaining: cumulative counters cannot wrap. */
				gate->state.submitted[k] += cost;
				entry->energy[k] += cost;
			}
		}
	}
	gate->state.pending++;
	*ticket = entry->id;
	goto out;
fail:
	apple_energy_disable(gate);
	memset(samples, 0, bytes);
out:
	spin_unlock_irqrestore(&gate->lock, flags);
	return ret;
}

int apple_energy_complete(struct apple_energy_gate *gate, u64 ticket)
{
	struct apple_energy_ticket *entry;
	unsigned int k;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&gate->lock, flags);
	if (!gate->state.pending || !ticket ||
	    gate->queue[gate->head].id != ticket) {
		apple_energy_disable(gate);
		apple_energy_next(gate);
		ret = -ESTALE;
		goto out;
	}
	entry = &gate->queue[gate->head];
	/* All live tickets retain their original mapping until completion. */
	for (k = 0; k < APPLE_ENERGY_SPEAKERS; k++)
		gate->state.completed[k] += entry->energy[k];
	gate->head = (gate->head + 1) % APPLE_ENERGY_QUEUE;
	gate->state.pending--;
	apple_energy_next(gate);
out:
	spin_unlock_irqrestore(&gate->lock, flags);
	return ret;
}

MODULE_LICENSE("GPL");
