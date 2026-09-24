/* SPDX-License-Identifier: GPL-2.0-only */
/* Qualified observation provider for the 25F84/25G83 DATA profile; no
 * frequency permission or actuation.
 */
#include <asm/arch_timer.h>
#include "pmp-v2-sample-age.h"

enum pmp_sample_reason {
	PMP_SAMPLE_OWNER = 1, PMP_SAMPLE_TIMER, PMP_SAMPLE_IDENTITY,
	PMP_SAMPLE_CONFIG, PMP_SAMPLE_POINTER, PMP_SAMPLE_TUPLE,
	PMP_SAMPLE_METADATA, PMP_SAMPLE_AGE, PMP_SAMPLE_VALUE,
	PMP_SAMPLE_COUNT_PENDING, PMP_SAMPLE_CACHE_PENDING,
	PMP_SAMPLE_ACTIVITY_PENDING, PMP_SAMPLE_DELIVERY_PENDING,
	PMP_SAMPLE_DROPPED_PENDING,
};

struct pmp_sample_diagnostic {
	unsigned int reason, provider, pending;
	unsigned int pending_provider[2];
	unsigned int dropped;
	u64 dropped_timestamp[2];
	u64 requested, activity, exception, selection, start, count;
	unsigned int pointer_class, metadata_flags, valid_lanes;
};

static void pmp_sample_pending(struct pmp_sample_diagnostic *d, unsigned int provider,
			       unsigned int reason)
{
	d->pending |= BIT(reason);
	d->pending_provider[provider] |= BIT(reason);
}

static int pmp_v2_sample(struct pmp_v2 *pmp, u64 timestamp[2], u64 ages[2], int *temperature,
			  int cluster_temperature[2], struct pmp_sample_diagnostic *diagnostic)
{
	static const u64 providers[] = { 0x1053ce0, 0x10541f0 };
	static const struct pmp_data_field cache[] = {
		{ "data", 0, 8 }, { "metadata", 8, 8 },
	};
	static const struct pmp_data_field identity_fields[] = {
		{ "selection", 0x38, 8 }, { "cache", 0x50, 8 }, { "config", 0x180, 8 },
		{ "requested", 0x2a4, 4 }, { "activity", 0x2a8, 4 }, { "exception", 0x2b2, 1 },
	};
	u64 timer[3], check[3], v[16], identity[16], pair[2], pointer, line, counter;
	struct pmp_sample_diagnostic ignored = {};
	struct pmp_sample_diagnostic *d = diagnostic ?: &ignored;
	u64 start, count, after_start, after_count, e_cache = 0;
	unsigned int i, j;
	int ret = -ENODATA;

	memset(d, 0, sizeof(*d));
	if (!mutex_trylock(&pmp->lock))
		return -EBUSY;
	d->reason = PMP_SAMPLE_OWNER;
	if (!pmp_data_live(pmp))
		goto out;
	d->reason = PMP_SAMPLE_TIMER;
	if (arch_timer_get_cntfrq() != 1000000000 ||
	    pmp_data_tuple(pmp, 0x104ffb8, pmp_data_timer, 3, timer) ||
	    timer[0] != 24000000 || timer[1] || timer[2] ||
	    pmp_data_read(pmp, 0x1050028, 4, &line) || line != 64)
		goto out;
	*temperature = -100000;
	cluster_temperature[0] = -100000;
	cluster_temperature[1] = -100000;
	for (i = 0; i < 2; i++) {
		d->provider = i;
		d->requested = d->activity = d->exception = d->selection = 0;
		d->start = d->count = 0;
		d->pointer_class = d->metadata_flags = d->valid_lanes = 0;
		d->reason = PMP_SAMPLE_IDENTITY;
		if (pmp_data_tuple(pmp, providers[i], identity_fields + 1, 5, v + 1) ||
		    pmp_data_read(pmp, providers[i] + 0x38, 8, &v[0]))
			goto out;
		d->requested = v[3];
		d->activity = v[4];
		d->exception = v[5];
		d->selection = !!(v[0] & BIT_ULL(3));
		if (v[2] != (i ? 0x104afb0 : 0x104abf8) || v[3] > 1 || v[4] > 1 || v[5])
			goto out;
		d->reason = PMP_SAMPLE_CONFIG;
		if (pmp_data_read(pmp, v[2] + 0x64, 4, &start) ||
		    pmp_data_read(pmp, v[2] + 0x68, 4, &count))
			goto out;
		d->start = start;
		d->count = count;
		if ((count && count != (i ? 16 : 8)) ||
		    (start != (i ? 360 : 352) && (count || start)))
			goto out;
		pointer = v[1];
		d->reason = PMP_SAMPLE_POINTER;
		d->pointer_class = pointer ? 2 : 0;
		if (pointer && pmp_v2_heap_range(pointer, i ? 256 : 128))
			goto out;
		d->pointer_class = pointer ? 1 : 0;
		if (i && pointer && e_cache && pointer < e_cache + 128 && e_cache < pointer + 256)
			goto out;
		if (!i)
			e_cache = pointer;
		if (!count)
			pmp_sample_pending(d, i, PMP_SAMPLE_COUNT_PENDING);
		if (!pointer)
			pmp_sample_pending(d, i, PMP_SAMPLE_CACHE_PENDING);
		if (!v[3] || !v[4] || !(v[0] & BIT_ULL(3)))
			pmp_sample_pending(d, i, PMP_SAMPLE_ACTIVITY_PENDING);
		d->reason = PMP_SAMPLE_TUPLE;
		/* Never follow an unpublished allocation/capacity. Still bracket
		 * safely accessible fixed identities and validate the other provider.
		 */
		if (pointer && count &&
		    (round_down(pointer + 0x30, line) != round_down(pointer + 0x3f, line) ||
		     pmp_data_tuple(pmp, pointer + 0x30, cache, 2, pair)))
			goto out;
		if (pmp_data_tuple(pmp, providers[i], identity_fields + 1, 5, identity + 1) ||
		    pmp_data_read(pmp, providers[i] + 0x38, 8, &identity[0]) ||
		    memcmp(v + 1, identity + 1, 5 * sizeof(u64)) ||
		    ((v[0] ^ identity[0]) & BIT_ULL(3)) ||
		    pmp_data_read(pmp, v[2] + 0x64, 4, &after_start) || after_start != start ||
		    pmp_data_read(pmp, v[2] + 0x68, 4, &after_count) || after_count != count)
			goto out;
		if (!pointer || !count)
			continue;
		d->reason = PMP_SAMPLE_METADATA;
		d->metadata_flags = (pair[1] >> 54) & 3;
		if ((pair[1] & BIT_ULL(55)) &&
		    (!(pair[1] & BIT_ULL(54)) || !v[3] || !v[4] || !(v[0] & BIT_ULL(3))))
			goto out;
		if (!(pair[1] & BIT_ULL(54)))
			pmp_sample_pending(d, i, PMP_SAMPLE_DELIVERY_PENDING);
		timestamp[i] = pair[1] & GENMASK_ULL(53, 0);
		counter = __arch_counter_get_cntpct();
		d->reason = PMP_SAMPLE_AGE;
		if ((pair[1] & BIT_ULL(55)) && !timestamp[i])
			goto out;
		if ((pair[1] & BIT_ULL(54)) && pmp_sample_age(counter, timestamp[i], &ages[i]))
			goto out;
		d->reason = PMP_SAMPLE_VALUE;
		d->valid_lanes = 0;
		for (j = 0; j < 2; j++) {
			u16 raw = pair[0] >> (16 * j);
			int value = sign_extend32(raw & 0x7ff0, 14) * 1000 / 64;

			if (!(raw & BIT(15))) {
				pmp_sample_pending(d, i, PMP_SAMPLE_DELIVERY_PENDING);
				continue;
			}
			d->valid_lanes |= BIT(j);
			if (value < -10000 || value > 120000)
				goto out;
			cluster_temperature[i] = max(cluster_temperature[i], value);
			*temperature = max(*temperature, value);
		}
		if (pair[1] & BIT_ULL(55)) {
			d->reason = PMP_SAMPLE_METADATA;
			if (d->valid_lanes != 3)
				goto out;
			/* Bit55 records overwrite of an earlier update, not lane
			 * validity. No policy accepts it here: startup may wait for
			 * a later clean delivery; active policy still fails closed.
			 */
			d->dropped |= BIT(i);
			d->dropped_timestamp[i] = timestamp[i];
			pmp_sample_pending(d, i, PMP_SAMPLE_DROPPED_PENDING);
		}
	}
	d->reason = PMP_SAMPLE_TIMER;
	if (pmp_data_tuple(pmp, 0x104ffb8, pmp_data_timer, 3, check) ||
	    memcmp(timer, check, sizeof(timer)))
		goto out;
	if (d->pending) {
		d->reason = __ffs(d->pending);
		ret = -EAGAIN;
		goto out;
	}
	d->reason = PMP_SAMPLE_AGE;
	counter = __arch_counter_get_cntpct();
	for (i = 0; i < 2; i++)
		if (pmp_sample_age(counter, timestamp[i], &ages[i]))
			goto out;
	ret = 0;
	d->reason = 0;
out:
	mutex_unlock(&pmp->lock);
	return ret;
}
