// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright (C) 2026 The Aurora Silicon Contributors
/* Finite energy credit and completion ledger for immutable DMA payloads. */
#include <linux/errno.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <sound/pcm.h>

#include "speaker-energy.h"

u64 apple_energy_cost(s32 sample)
{
	u64 magnitude = sample < 0 ? -(s64)sample : sample;
	u64 square = magnitude * magnitude;
	const unsigned int shift = 62 - APPLE_ENERGY_UNIT_SHIFT;

	/* Round normalized energy upward without overflowing square + mask. */
	return (square >> shift) + !!(square & ((1ULL << shift) - 1));
}
EXPORT_SYMBOL_GPL(apple_energy_cost);

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
EXPORT_SYMBOL_GPL(apple_energy_init);

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
	gate->configured = false;
	if (!apple_energy_next(gate)) {
		ret = -EOVERFLOW;
		goto out;
	}
	if (rate < 8000 || rate > 192000 || !channels ||
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
	gate->configured = true;
out:
	spin_unlock_irqrestore(&gate->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_energy_configure);

void apple_energy_snapshot(struct apple_energy_gate *gate,
			   struct apple_energy_snapshot *snapshot)
{
	unsigned long flags;

	spin_lock_irqsave(&gate->lock, flags);
	*snapshot = gate->state;
	memcpy(snapshot->ceiling, gate->ceiling, sizeof(snapshot->ceiling));
	memcpy(snapshot->gain_q31, gate->gain_q31, sizeof(snapshot->gain_q31));
	snapshot->deadline_ns = gate->deadline_ns;
	snapshot->fallback_q31 = gate->fallback_q31;
	snapshot->configured = gate->configured;
	snapshot->enabled = gate->enabled;
	snapshot->failed = gate->failed;
	snapshot->clock_ns = ktime_get_boottime_ns();
	spin_unlock_irqrestore(&gate->lock, flags);
}
EXPORT_SYMBOL_GPL(apple_energy_snapshot);

int apple_energy_grant_gains(struct apple_energy_gate *gate,
			     const struct apple_energy_snapshot *snapshot,
			     const u64 *ceiling, const u32 *gains, u64 now_ns,
			     u64 deadline_ns)
{
	unsigned long flags;
	unsigned int k;
	int ret = 0;

	spin_lock_irqsave(&gate->lock, flags);
	if (gate->failed) {
		ret = -EOVERFLOW;
		goto out;
	}
	if (!snapshot || !ceiling || !gate->configured ||
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
		if (gains && gains[k] > APPLE_ENERGY_UNITY) {
			ret = -EINVAL;
			goto out;
		}
		if (ceiling[k] < gate->state.submitted[k]) {
			ret = -ERANGE;
			goto out;
		}
	}
	if (!apple_energy_next(gate)) {
		ret = -EOVERFLOW;
		goto out;
	}
	memcpy(gate->ceiling, ceiling, gate->state.speakers * sizeof(*ceiling));
	gate->deadline_ns = deadline_ns;
	for (k = 0; k < gate->state.speakers; k++)
		gate->gain_q31[k] = gains ? gains[k] : APPLE_ENERGY_UNITY;
	gate->enabled = true;
	/* Nothing newly submitted may enter a higher-gain epoch uncharged. */
	gate->cache_raw = false;
	for (k = 0; k < gate->state.speakers; k++) {
		if (gate->state.unbudgeted[k] >
		    ceiling[k] - gate->state.submitted[k]) {
			ret = -EAGAIN;
			goto out;
		}
	}
	for (k = 0; k < gate->state.pending; k++) {
		struct apple_energy_ticket *entry =
			&gate->queue[(gate->head + k) % APPLE_ENERGY_QUEUE];
		unsigned int speaker;

		if (!entry->raw)
			continue;
		for (speaker = 0; speaker < gate->state.speakers; speaker++) {
			entry->energy[speaker] = entry->raw_cost[speaker];
			gate->state.submitted[speaker] +=
				entry->raw_cost[speaker];
		}
		entry->raw = false;
	}
	memset(gate->state.unbudgeted, 0, sizeof(gate->state.unbudgeted));
out:
	/* An invalid or stale transaction must not preserve uncertain credit. */
	if (ret && ret != -EAGAIN) {
		apple_energy_disable(gate);
		apple_energy_next(gate);
	}
	spin_unlock_irqrestore(&gate->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_energy_grant_gains);

int apple_energy_grant(struct apple_energy_gate *gate,
		       const struct apple_energy_snapshot *snapshot,
		       const u64 *ceiling, u64 now_ns, u64 deadline_ns)
{
	return apple_energy_grant_gains(gate, snapshot, ceiling, NULL, now_ns,
					deadline_ns);
}
EXPORT_SYMBOL_GPL(apple_energy_grant);

void apple_energy_revoke(struct apple_energy_gate *gate)
{
	unsigned long flags;

	spin_lock_irqsave(&gate->lock, flags);
	apple_energy_disable(gate);
	gate->cache_raw = gate->hardware_safe && !gate->failed;
	apple_energy_next(gate);
	spin_unlock_irqrestore(&gate->lock, flags);
}
EXPORT_SYMBOL_GPL(apple_energy_revoke);

void apple_energy_fallback(struct apple_energy_gate *gate, u32 gain_q31)
{
	unsigned long flags;

	spin_lock_irqsave(&gate->lock, flags);
	apple_energy_disable(gate);
	gate->fallback_q31 = min(gain_q31, APPLE_ENERGY_UNITY);
	apple_energy_next(gate);
	spin_unlock_irqrestore(&gate->lock, flags);
}
EXPORT_SYMBOL_GPL(apple_energy_fallback);

int apple_energy_hardware_safe(struct apple_energy_gate *gate, bool safe)
{
	unsigned long flags;
	unsigned int k;
	int ret = 0;

	spin_lock_irqsave(&gate->lock, flags);
	if (!safe) {
		for (k = 0; k < gate->state.speakers; k++) {
			if (gate->state.unbudgeted[k]) {
				ret = -EAGAIN;
				goto out;
			}
		}
	}
	if (safe != gate->hardware_safe) {
		if (gate->safe_epoch == U64_MAX) {
			gate->failed = true;
			apple_energy_disable(gate);
			ret = -EOVERFLOW;
			goto out;
		}
		gate->safe_epoch++;
	}
	gate->hardware_safe = safe;
	gate->cache_raw = safe && !gate->enabled && !gate->failed;
out:
	spin_unlock_irqrestore(&gate->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_energy_hardware_safe);

static u64 apple_energy_rate_cost(s32 sample, u32 rate)
{
	return DIV_ROUND_UP_ULL(
		apple_energy_cost(sample) * APPLE_ENERGY_UNIT_RATE, rate);
}

static u64 apple_energy_meter(s32 sample, u32 rate, u32 metric)
{
	u64 raw = metric == APPLE_ENERGY_ABSOLUTE ?
			  DIV_ROUND_UP_ULL(
				  abs((s64)sample),
				  1ULL << (31 - APPLE_ENERGY_UNIT_SHIFT)) :
			  apple_energy_cost(sample);

	return DIV_ROUND_UP_ULL(raw * APPLE_ENERGY_UNIT_RATE, rate);
}

int apple_energy_metric(struct apple_energy_gate *gate, u32 metric)
{
	unsigned long flags;
	unsigned int k;
	int ret = 0;

	spin_lock_irqsave(&gate->lock, flags);
	apple_energy_disable(gate);
	if (gate->state.pending || metric > APPLE_ENERGY_ABSOLUTE)
		ret = -EBUSY;
	else {
		for (k = 0; k < APPLE_ENERGY_SPEAKERS; k++)
			if (metric != gate->metric &&
			    (gate->state.submitted[k] ||
			     gate->state.source_submitted[k]))
				ret = -EINVAL;
		if (!ret)
			gate->metric = metric;
	}
	apple_energy_next(gate);
	spin_unlock_irqrestore(&gate->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_energy_metric);

static s32 apple_energy_clamp(s32 sample, u64 remaining, u32 rate, u32 metric)
{
	u64 bound;

	if (apple_energy_meter(sample, rate, metric) <= remaining)
		return sample;
	/* Here remaining * rate < UNIT_SCALE * 48000: neither product can wrap. */
	bound = div_u64(remaining * rate, APPLE_ENERGY_UNIT_RATE);
	bound = metric == APPLE_ENERGY_ABSOLUTE ?
			bound << (31 - APPLE_ENERGY_UNIT_SHIFT) :
			int_sqrt64(bound << (62 - APPLE_ENERGY_UNIT_SHIFT));
	return sample < 0 ? (s32)-(s64)bound : (s32)bound;
}

static s32 apple_energy_scale(s32 sample, u32 gain)
{
	return (s64)sample * gain / (1LL << 31);
}

static int apple_energy_width(int format)
{
	switch (format) {
	case SNDRV_PCM_FORMAT_S16_LE:
		return 2;
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S32_LE:
	case APPLE_ENERGY_FORMAT_NEO_F32:
		return 4;
	default:
		return -EINVAL;
	}
}

/* Decode exactly with integers. Only Q31-representable words are accepted;
 * fractional Q31 input must not survive as unbilled raw-prefix energy.
 * The producer's integer-to-float conversion satisfies this restriction.
 */
static int apple_energy_float_get(u32 bits, s32 *value)
{
	u32 exponent = (bits >> 23) & 255;
	u32 magnitude = (bits & 0x7fffff) | (1U << 23);

	if (!(bits & 0x7fffffff)) {
		*value = 0;
		return 0;
	}
	if (exponent < 95 || exponent > 126)
		return -EINVAL;
	if (exponent < 118) {
		u32 shift = 118 - exponent;

		if (magnitude & ((1U << shift) - 1))
			return -EINVAL;
		magnitude >>= shift;
	} else {
		magnitude <<= exponent - 118;
	}
	if (magnitude > (1U << 31) ||
	    (!(bits & (1U << 31)) && magnitude == (1U << 31)))
		return -EINVAL;
	*value = bits & (1U << 31) ? -(s64)magnitude : magnitude;
	return 0;
}

static int apple_energy_get(const void *sample, int format, s32 *value)
{
	if (format == SNDRV_PCM_FORMAT_S16_LE)
		*value = (s32)(s16)get_unaligned_le16(sample) * 65536;
	else if (format == SNDRV_PCM_FORMAT_S24_LE)
		*value = sign_extend32(get_unaligned_le32(sample), 23) * 256;
	else if (format == APPLE_ENERGY_FORMAT_NEO_F32)
		return apple_energy_float_get(get_unaligned_le32(sample), value);
	else
		*value = (s32)get_unaligned_le32(sample);
	return 0;
}

static s32 apple_energy_put(void *sample, s32 value, int format)
{
	if (format == APPLE_ENERGY_FORMAT_NEO_F32) {
		u32 magnitude = value < 0 ? -(s64)value : value;
		u32 sign = value < 0 ? (1U << 31) : 0;
		u32 bits, p, mantissa;

		if (!magnitude) {
			put_unaligned_le32(0, sample);
			return 0;
		}
		p = 31 - __builtin_clz(magnitude);
		mantissa = p > 23 ? magnitude >> (p - 23) :
				    magnitude << (23 - p);
		bits = sign | ((p + 95) << 23) | (mantissa & 0x7fffff);
		put_unaligned_le32(bits, sample);
		/* Truncation is toward zero for both signs; bill the emitted word. */
		apple_energy_float_get(bits, &value);
		return value;
	}
	if (format == SNDRV_PCM_FORMAT_S16_LE) {
		value /=
			65536; /* toward zero, never more energy than was authorized */
		put_unaligned_le16(value, sample);
		return value * 65536;
	}
	if (format == SNDRV_PCM_FORMAT_S24_LE) {
		value /= 256;
		put_unaligned_le32(value, sample);
		return value * 256;
	}
	put_unaligned_le32(value, sample);
	return value;
}

int apple_energy_filter_pcm(struct apple_energy_gate *gate, void *samples,
			    unsigned int frames, unsigned int channels,
			    int format, u64 now_ns, u64 *ticket)
{
	struct apple_energy_ticket *entry;
	unsigned int f, ch, k;
	unsigned long flags;
	size_t bytes;
	int ret = 0;
	int width = apple_energy_width(format);

	if (!samples || !ticket || !channels || width < 0 ||
	    channels > APPLE_ENERGY_SPEAKERS ||
	    frames > APPLE_ENERGY_MAX_FRAMES || !frames)
		return -EINVAL;
	bytes = (size_t)frames * channels * width;
	*ticket = 0;
	spin_lock_irqsave(&gate->lock, flags);
	if (channels != gate->channels || !gate->configured) {
		ret = -EINVAL;
		goto fail;
	}
	if (gate->failed || gate->state.sequence == U64_MAX ||
	    gate->next_ticket == U64_MAX) {
		gate->failed = true;
		ret = -EOVERFLOW;
		goto fail;
	}
	if (gate->state.pending == APPLE_ENERGY_QUEUE) {
		ret = -ENOSPC;
		goto fail;
	}
	if (gate->enabled && now_ns >= gate->deadline_ns) {
		apple_energy_disable(gate);
		apple_energy_next(gate);
	}
	entry = &gate->queue[(gate->head + gate->state.pending) %
			     APPLE_ENERGY_QUEUE];
	memset(entry, 0, sizeof(*entry));
	entry->id = ++gate->next_ticket;
	/* A Neo volume limit only affects future producer copies. Even a
	 * mistaken caller's hardware-safe assertion cannot forgive old words.
	 */
	entry->born_safe = gate->hardware_safe &&
		format != APPLE_ENERGY_FORMAT_NEO_F32;
	entry->safe_epoch = gate->safe_epoch;
	entry->raw = entry->born_safe && gate->cache_raw;
	for (f = 0; f < frames; f++) {
		for (ch = 0; ch < channels; ch++) {
			void *word = samples + (f * channels + ch) * width;
			s32 input, sample, fallback;
			s32 floor;
			u32 gain = APPLE_ENERGY_UNITY;
			bool mapped = false;
			u64 cost;

			ret = apple_energy_get(word, format, &input);
			if (ret) {
				gate->failed = true;
				goto fail;
			}
			sample = input;
			fallback = apple_energy_scale(input, gate->fallback_q31);
			for (k = 0; k < gate->state.speakers; k++) {
				if (gate->slot[k] != ch)
					continue;
				mapped = true;
				gain = min(gain, gate->gain_q31[k]);
			}
			sample = apple_energy_scale(sample, gain);
			floor = abs((s64)sample) < abs((s64)fallback) ?
					sample :
					fallback;
			if (entry->raw) {
				sample = input;
			} else if (!mapped || !gate->enabled) {
				sample = fallback;
			} else {
				for (k = 0; k < gate->state.speakers; k++) {
					u64 remaining;

					if (gate->slot[k] != ch)
						continue;
					remaining = gate->ceiling[k] -
						    gate->state.submitted[k];
					sample = apple_energy_clamp(
						sample, remaining,
						gate->state.rate, gate->metric);
				}
				if (abs((s64)sample) < abs((s64)floor))
					sample = floor;
			}
			sample = apple_energy_put(word, sample, format);
			/*
			 * One safe baseline covers ALL unbilled output: a certificate
			 * must prove P0 + F_cert * fallback_gain^p <= Ps. Background
			 * cannot spend that allowance again; residual B is separate.
			 */
			cost = entry->raw || abs((s64)sample) <=
						       abs((s64)fallback) ?
				       0 :
				       apple_energy_meter(sample,
							  gate->state.rate,
							  gate->metric);
			for (k = 0; k < gate->state.speakers; k++) {
				u64 source;
				u64 raw = apple_energy_rate_cost(
					input, gate->state.rate);

				if (gate->slot[k] != ch)
					continue;
				if (check_add_overflow(
					    gate->state.source_submitted[k],
					    raw, &source)) {
					gate->failed = true;
					ret = -EOVERFLOW;
					goto fail;
				}
				gate->state.source_submitted[k] = source;
				entry->source[k] += raw;
				entry->raw_cost[k] += apple_energy_meter(
					input, gate->state.rate, gate->metric);
				/* cost <= remaining: cumulative counters cannot wrap. */
				gate->state.submitted[k] += cost;
				entry->energy[k] += cost;
			}
		}
	}
	if (entry->raw)
		for (k = 0; k < gate->state.speakers; k++)
			gate->state.unbudgeted[k] += entry->raw_cost[k];
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
EXPORT_SYMBOL_GPL(apple_energy_filter_pcm);

int apple_energy_filter(struct apple_energy_gate *gate, s32 *samples,
			unsigned int frames, unsigned int channels, u64 now_ns,
			u64 *ticket)
{
	return apple_energy_filter_pcm(gate, samples, frames, channels,
				       SNDRV_PCM_FORMAT_S32_LE, now_ns, ticket);
}
EXPORT_SYMBOL_GPL(apple_energy_filter);

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
	for (k = 0; k < APPLE_ENERGY_SPEAKERS; k++) {
		gate->state.completed[k] += entry->energy[k];
		gate->state.source_completed[k] += entry->source[k];
		if (entry->raw)
			gate->state.unbudgeted[k] -= entry->raw_cost[k];
		else if (entry->born_safe && gate->hardware_safe &&
			 entry->safe_epoch == gate->safe_epoch)
			gate->state.refunded[k] += entry->energy[k];
	}
	gate->head = (gate->head + 1) % APPLE_ENERGY_QUEUE;
	gate->state.pending--;
out:
	spin_unlock_irqrestore(&gate->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_energy_complete);

MODULE_LICENSE("GPL");
