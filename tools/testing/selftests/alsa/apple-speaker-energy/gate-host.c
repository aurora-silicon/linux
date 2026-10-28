// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Host-only adapter for the production ledger and immutable payload helpers.
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <linux/unaligned.h>
#include <linux/math.h>
#include <linux/errno.h>
#include <sound/pcm.h>
#include "speaker-energy.h"
#include "apple-speaker.h"
#include "apple-admac-payload.h"

_Thread_local u64 apple_test_clock_ns;

struct host_gate {
	struct apple_energy_gate gate;
	struct admac_payload payload;
	u8 *source;
	u32 rate, channels, frames, format, transfer, amp, safe, tail_us;
	char card[32];
	bool owner, unlocked, notice, playing, failed;
};

static void host_fault(void *opaque)
{
	struct host_gate *g = opaque;
	g->failed = true;

	apple_energy_revoke(&g->gate);
}

static int host_process(void *opaque, void *samples, size_t bytes, u64 *ticket)
{
	struct host_gate *g = opaque;
	u32 width = g->format == 2 ? 2 : 4;

	if (bytes != (size_t)g->frames * g->channels * width)
		return -EINVAL;
	return apple_energy_filter_pcm(&g->gate, samples, g->frames,
				       g->channels, g->format,
				       apple_test_clock_ns, ticket);
}

static int host_retire(void *opaque, u64 ticket)
{
	return apple_energy_complete(&((struct host_gate *)opaque)->gate,
				     ticket);
}

static void host_safe(struct host_gate *g)
{
	if (g->unlocked)
		g->notice = true;
	g->unlocked = false;
	apple_energy_hardware_safe(&g->gate, true);
}

static void host_expire(struct host_gate *g, u64 now)
{
	apple_test_clock_ns = now;
	if (g->unlocked && g->gate.enabled && now >= g->gate.deadline_ns) {
		apple_energy_revoke(&g->gate);
		host_safe(g);
	}
}

void *ag_new(u32 rate, u32 channels, u32 frames, u32 format, u32 transfer,
	     u32 amp, u32 safe, u32 tail_us, const char *card)
{
	struct host_gate *g;
	u8 slots[APPLE_ENERGY_SPEAKERS];
	u32 k, width = format == 2 ? 2 : 4;

	if (channels < 1 || channels > 6 || frames < 1 || frames > 4096 ||
	    (format != 2 && format != 6 && format != 10) ||
	    (transfer != 1 && transfer != 2))
		return NULL;
	g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->rate = rate;
	g->channels = channels;
	g->frames = frames;
	g->format = format;
	g->transfer = transfer;
	g->amp = amp;
	g->safe = safe;
	g->tail_us = tail_us;
	snprintf(g->card, sizeof(g->card), "%s", card);
	apple_energy_init(&g->gate);
	for (k = 0; k < channels; k++)
		slots[k] = k;
	if (apple_energy_configure(&g->gate, rate, channels, channels, slots))
		goto fail;
	if (apple_energy_metric(&g->gate, transfer == 2 ?
						  APPLE_ENERGY_ABSOLUTE :
						  APPLE_ENERGY_SQUARED))
		goto fail;
	apple_energy_fallback(
		&g->gate,
		(u32)floor(2147483648.0 * pow(10.0, -(double)safe / 20000.0)));
	apple_energy_hardware_safe(&g->gate, true);
	g->payload.filter = (struct apple_admac_playback_filter){
		.buffer_bytes = (size_t)frames * channels * width,
		.period_bytes = (size_t)frames * channels * width,
		.context = g,
		.process = host_process,
		.retire = host_retire,
		.fault = host_fault
	};
	g->source = calloc(1, g->payload.filter.period_bytes);
	g->payload.area =
		calloc(ADMAC_PAYLOAD_SLOTS, g->payload.filter.period_bytes);
	if (!g->source || !g->payload.area)
		goto fail;
	g->payload.filter.source = g->source;
	g->payload.dma = 0x100000;
	return g;
fail:
	free(g->source);
	free(g->payload.area);
	free(g);
	return NULL;
}

void ag_free(void *opaque)
{
	struct host_gate *g = opaque;
	if (g) {
		pthread_mutex_destroy(&g->gate.lock);
		free(g->payload.area);
		free(g->source);
		free(g);
	}
}

int ag_owner(void *opaque, int owner, u64 now)
{
	struct host_gate *g = opaque;
	host_expire(g, now);

	apple_energy_revoke(&g->gate);
	host_safe(g);
	g->owner = owner;
	g->notice = false;
	return 0;
}

int ag_snapshot(void *opaque, u64 now, void *output)
{
	struct host_gate *g = opaque;
	struct apple_energy_snapshot s;
	struct macaudio_energy_ctl *v = output;
	u32 k;
	_Static_assert(sizeof(*v) == 512, "wire layout");

	host_expire(g, now);
	apple_energy_snapshot(&g->gate, &s);
	memset(v, 0, sizeof(*v));
	v->version = MACAUDIO_ENERGY_VERSION;
	v->flags = MACAUDIO_ENERGY_AVAILABLE | MACAUDIO_ENERGY_CONFIGURED |
		   MACAUDIO_ENERGY_TRANSFER_VALID | MACAUDIO_ENERGY_REQUIRED;
	if (g->owner)
		v->flags |= MACAUDIO_ENERGY_OWNED;
	if (g->failed || s.failed)
		v->flags |= MACAUDIO_ENERGY_FAILED;
	if (s.enabled && now < s.deadline_ns)
		v->flags |= MACAUDIO_ENERGY_LIVE;
	if (g->playing)
		v->flags |= MACAUDIO_ENERGY_PLAYING;
	v->epoch = s.epoch;
	v->sequence = s.sequence;
	v->clock_ns = now;
	v->deadline_ns = s.deadline_ns;
	v->rate = g->rate;
	v->channels = g->channels;
	v->speakers = g->channels;
	v->format = g->format;
	v->transfer = g->transfer;
	v->amp_gain_millidbv = g->amp;
	v->safe_reduction_millidb = g->safe;
	v->pending = s.pending;
	memcpy(v->card_id, g->card, sizeof(v->card_id));
	for (k = 0; k < 6; k++) {
		v->slot[k] = k;
		v->submitted[k] = s.submitted[k];
		v->completed[k] = s.completed[k];
		v->source_submitted[k] = s.source_submitted[k];
		v->source_completed[k] = s.source_completed[k];
		v->refunded[k] = s.refunded[k];
		v->unbudgeted[k] = s.unbudgeted[k];
		v->ceiling[k] = s.ceiling[k];
		v->gain_q31[k] = s.gain_q31[k];
		v->tail_us[k] = k < g->channels ? g->tail_us : 0;
	}
	return 0;
}

int ag_write(void *opaque, const void *input, u64 now)
{
	struct host_gate *g = opaque;
	const struct macaudio_energy_ctl *v = input;
	struct apple_energy_snapshot s = {};
	u64 ceiling[6];
	u32 gains[6], k;
	int ret = -EINVAL;
	host_expire(g, now);

	if (!g->owner) {
		ret = -EPERM;
		goto fail;
	}
	if (v->version != MACAUDIO_ENERGY_VERSION)
		goto fail;
	for (k = 0; k < 6; k++)
		if (v->tail_us[k] != (k < g->channels ? g->tail_us : 0))
			goto fail;
	if (v->flags == MACAUDIO_ENERGY_REVOKE) {
		apple_energy_revoke(&g->gate);
		host_safe(g);
		return 0;
	}
	if (v->flags || v->rate != g->rate || v->channels != g->channels ||
	    v->speakers != g->channels || v->format != g->format ||
	    v->transfer != g->transfer || v->amp_gain_millidbv != g->amp ||
	    v->safe_reduction_millidb != g->safe ||
	    strncmp((const char *)v->card_id, g->card, 32))
		goto fail;
	if (v->deadline_ns <= now) {
		ret = -ETIMEDOUT;
		goto fail;
	}
	if (v->deadline_ns - now > 250000000ULL)
		goto fail;
	for (k = 0; k < 6; k++) {
		if (v->slot[k] != k)
			goto fail;
		ceiling[k] = v->ceiling[k];
		gains[k] = v->gain_q31[k];
	}
	s.epoch = v->epoch;
	s.sequence = v->sequence;
	ret = apple_energy_grant_gains(&g->gate, &s, ceiling, gains, now,
				       v->deadline_ns);
	if (ret == -EAGAIN) {
		host_safe(g);
		return ret;
	}
	if (ret)
		goto fail;
	ret = apple_energy_hardware_safe(&g->gate, false);
	if (ret)
		goto fail;
	g->unlocked = true;
	return 0;
fail:
	apple_energy_revoke(&g->gate);
	host_safe(g);
	return ret;
}

int ag_ping(void *opaque, u64 now)
{
	struct host_gate *g = opaque;
	host_expire(g, now);

	if (!g->owner)
		return -EPERM;
	if (g->notice) {
		g->notice = false;
		return -ETIMEDOUT;
	}
	if (!g->gate.enabled || g->gate.deadline_ns <= now)
		return -EACCES;
	return 0;
}

int ag_unlocked(void *opaque, u64 now)
{
	struct host_gate *g = opaque;
	host_expire(g, now);

	return g->unlocked;
}

int ag_submit(void *opaque, const s32 *input, s32 *output, u64 now, u64 *ticket)
{
	struct host_gate *g = opaque;
	dma_addr_t address;
	u32 k, width = g->format == 2 ? 2 : 4;
	u8 *copy;
	int ret;
	host_expire(g, now);

	for (k = 0; k < g->frames * g->channels; k++) {
		if (g->format == 2)
			put_unaligned_le16(input[k] / 65536,
					   g->source + k * width);
		else if (g->format == 6)
			put_unaligned_le32(input[k] / 256,
					   g->source + k * width);
		else
			put_unaligned_le32(input[k], g->source + k * width);
	}
	ret = admac_payload_prepare(&g->payload, 0, &address);
	if (ret)
		return ret;
	copy = g->payload.area + (address - g->payload.dma);
	for (k = 0; k < g->frames * g->channels; k++) {
		if (g->format == 2)
			output[k] =
				(s32)(s16)get_unaligned_le16(copy + k * width) *
				65536;
		else if (g->format == 6)
			output[k] = sign_extend32(get_unaligned_le32(copy +
								     k * width),
						  23) *
				    256;
		else
			output[k] = (s32)get_unaligned_le32(copy + k * width);
	}
	*ticket = g->payload.ticket[(g->payload.head + g->payload.pending - 1) %
				    ADMAC_PAYLOAD_SLOTS];
	return 0;
}

int ag_retire(void *opaque, u64 ticket, u64 now)
{
	struct host_gate *g = opaque;
	host_expire(g, now);

	if (!g->payload.pending ||
	    g->payload.ticket[g->payload.head] != ticket) {
		host_fault(g);
		return -ESTALE;
	}
	return admac_payload_retire(&g->payload);
}

int ag_stop(void *opaque, u64 now)
{
	struct host_gate *g = opaque;
	host_expire(g, now);

	/* As on proven stop, any possibly loud copy is charged in full. */
	while (g->payload.pending) {
		int ret = admac_payload_retire(&g->payload);
		if (ret)

			return ret;
	}
	apple_energy_revoke(&g->gate);
	host_safe(g);
	g->playing = false;
	return 0;
}

int ag_start(void *opaque, u64 now)
{
	struct host_gate *g = opaque;
	u8 slots[6];
	u32 k;
	int ret;
	ret = ag_stop(g, now);

	if (ret)
		return ret;
	for (k = 0; k < g->channels; k++)
		slots[k] = k;
	ret = apple_energy_configure(&g->gate, g->rate, g->channels,
				     g->channels, slots);
	g->playing = true;
	return ret;
}
