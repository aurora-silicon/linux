/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_PMP_POWER_POLICY_H
#define _LINUX_SOC_APPLE_PMP_POWER_POLICY_H

/* Provisional J713 measured-rail feedback, not Apple's power model/TDP.
 * Domains are PP0b (P CPU), PPbb (E CPU), PP1b (GPU). Their sum excludes
 * other SoC/system rails; independent die protection remains mandatory.
 * Slow temperature feedback adapts sustainable watts. A bounded energy bucket
 * permits bursts; neither a cool die spike nor missing telemetry refills it.
 * Units: mW, mJ, mC, monotonic ns. No floating point or per-OPP watt guesses.
 */
#define APPLE_PMP_BURST_MJ 600000LL
#define APPLE_PMP_BURST_MW 20000
#define APPLE_PMP_POWER_STALE_NS 3000000000ULL

struct apple_pmp_power_policy {
	unsigned long long last_ns, change_ns, tune_ns;
	long long credit_mj;
	int sustainable_mw, allowed_mw, total_mw, filtered_mc;
	unsigned int level[3], next_release, good;
	unsigned int active;
	bool initialized, fault;
};

/* SMC little-endian IEEE binary32 Watts -> mW. Reject NaN/Inf, negative,
 * or >50 W per rail rather than overflowing or interpreting errors as idle.
 */
static inline bool apple_pmp_power_decode(unsigned int bits, unsigned int *mw)
{
	unsigned int exponent = (bits >> 23) & 255;
	unsigned long long value;

	if ((bits >> 31) || exponent > 132)
		return false;
	if (exponent < 100) {
		*mw = 0;
		return true;
	}
	value = ((bits & 0x7fffff) | 0x800000ULL) * 1000;
	value >>= 150 - exponent;
	if (value > 50000)
		return false;
	*mw = value;
	return true;
}

static inline void apple_pmp_power_init(struct apple_pmp_power_policy *s)
{
	*s = (struct apple_pmp_power_policy) {
		.sustainable_mw = 8000, .allowed_mw = 8000,
		.level = {8, 8, 8}, .fault = true,
		.active = 7,
	};
}

/* A newly attached consumer starts restricted. An absent consumer cannot
 * absorb releases or be selected as a way to reduce measured consumption.
 */
static inline void apple_pmp_power_set_active(struct apple_pmp_power_policy *s,
					    unsigned int active)
{
	for (unsigned int i = 0; i < 3; i++) {
		if (!(active & (1U << i)))
			s->level[i] = 0;
		else if (!(s->active & (1U << i)))
			s->level[i] = 8;
	}
	s->active = active & 7;
}

static inline void apple_pmp_power_fail(struct apple_pmp_power_policy *s)
{
	for (unsigned int i = 0; i < 3; i++)
		s->level[i] = s->active & (1U << i) ? 8 : 0;
	s->fault = true;
	s->good = 0;
}

static inline void apple_pmp_power_sample(struct apple_pmp_power_policy *s,
		unsigned long long now, const unsigned int mw[3], int mc, int error)
{
	unsigned long long dt = s->last_ns ? now - s->last_ns : 0;
	unsigned long long ms = dt / 1000000ULL;
	/* Prefer graphics throughput when all domains contend. These are policy
	 * weights, not watt estimates/reservations: idle GPU capacity remains
	 * available to CPUs. Equal P/E weights, 4x GPU weight, round-robin ties.
	 */
	static const unsigned int weight[3] = {1, 1, 4};
	unsigned int i, chosen = 3;
	bool gap = dt > 2000000000ULL;

	if (error || gap) {
		apple_pmp_power_fail(s);
		s->last_ns = now;
		return;
	}
	/* Repeated reads do not advance the control clock or confirmation. */
	if (s->last_ns && dt < 400000000ULL)
		return;
	s->last_ns = now;
	s->total_mw = 0;
	for (i = 0; i < 3; i++)
		if (s->active & (1U << i))
			s->total_mw += mw[i];
	if (!s->initialized) {
		s->initialized = true;
		s->credit_mj = mc < 75000 ? APPLE_PMP_BURST_MJ : 0;
		s->filtered_mc = mc;
		s->tune_ns = now;
	}
	if (++s->good > 3)
		s->good = 3;
	if (s->good < 3)
		return;
	s->fault = false;
	s->filtered_mc += (mc - s->filtered_mc) * (long long)ms / (long long)(5000 + ms);
	if (s->total_mw > s->sustainable_mw)
		s->credit_mj -= (s->total_mw - s->sustainable_mw) * (long long)ms / 1000;
	else if (mc < 75000)
		s->credit_mj += (s->sustainable_mw - s->total_mw) * (long long)ms / 1000;
	if (s->credit_mj < 0)
		s->credit_mj = 0;
	if (s->credit_mj > APPLE_PMP_BURST_MJ)
		s->credit_mj = APPLE_PMP_BURST_MJ;
	/* Do not infer sustainable watts from a permitted short hot burst.
	 * Once its energy is spent, regulate toward 85 C with a 3 C deadband.
	 */
	if (now - s->tune_ns >= 2000000000ULL) {
		s->tune_ns = now;
		if (!s->credit_mj) {
			if (s->filtered_mc > 88000)
				s->sustainable_mw -= (s->filtered_mc - 85000) / 20;
			else if (s->filtered_mc < 82000 &&
				 (s->level[0] || s->level[1] || s->level[2]))
				s->sustainable_mw += 125;
			if (s->sustainable_mw < 4000)
				s->sustainable_mw = 4000;
			if (s->sustainable_mw > 16000)
				s->sustainable_mw = 16000;
		}
	}
	/* Spend at most the remaining energy in the next one-second window.
	 * This tapers the final burst instead of overspending a tiny last credit.
	 */
	s->allowed_mw = s->sustainable_mw +
		(s->credit_mj < APPLE_PMP_BURST_MW - s->sustainable_mw ?
		 s->credit_mj : APPLE_PMP_BURST_MW - s->sustainable_mw);
	if (now - s->change_ns < 1000000000ULL)
		return;
	if (s->total_mw > s->allowed_mw + 500) {
		/* Restrict the largest weight-normalized measured consumer that can yield.
		 * An idle CPU does not reserve watts that the GPU could use (or vice versa).
		 */
		for (i = 0; i < 3; i++)
			if ((s->active & (1U << i)) && s->level[i] < 8 && (chosen == 3 ||
			    mw[i] * weight[chosen] > mw[chosen] * weight[i]))
				chosen = i;
		if (chosen < 3)
			s->level[chosen]++;
		s->change_ns = now;
	} else if (s->total_mw < s->allowed_mw - 1000) {
		/* Release the largest weighted restriction, round-robin on ties.
		 * This restores graphics headroom before spending it on CPU stress.
		 * Fault recovery still restores only one step per elapsed interval.
		 */
		for (i = 0; i < 3; i++) {
			unsigned int candidate = (s->next_release + i) % 3;

			if ((s->active & (1U << candidate)) && s->level[candidate] &&
			    (chosen == 3 ||
			    s->level[candidate] * weight[candidate] >
			    s->level[chosen] * weight[chosen]))
				chosen = candidate;
		}
		if (chosen < 3) {
			s->level[chosen]--;
			s->next_release = (chosen + 1) % 3;
		}
		s->change_ns = now;
	}
}

/* Atomic publication contains the sample time and restriction together.
 * Zero, stale, future or malformed publications all select full protection.
 */
static inline unsigned long long apple_pmp_gpu_budget_pack(unsigned int level,
							 unsigned long long ns)
{
	return ((ns / 1000000ULL) << 4) | (level < 8 ? level : 8);
}

static inline unsigned int apple_pmp_gpu_budget_level(unsigned long long value,
						      unsigned long long now)
{
	if (!value || now / 1000000ULL - (value >> 4) >
	    APPLE_PMP_POWER_STALE_NS / 1000000ULL || (value & 15) > 8)
		return 8;
	return value & 15;
}
/* Provider-serialized consumer epochs. A rail read started for one consumer
 * must never relax the budget of a replacement that attached while it slept.
 */
struct apple_pmp_gpu_lease {
	unsigned long long epoch, publication;
	bool attached;
};

static inline unsigned long long apple_pmp_gpu_lease_attach(struct apple_pmp_gpu_lease *s)
{
	if (s->attached || s->epoch == ~0ULL)
		return 0;
	s->attached = true;
	s->publication = 0;
	return ++s->epoch;
}

static inline void apple_pmp_gpu_lease_detach(struct apple_pmp_gpu_lease *s,
					     unsigned long long epoch)
{
	if (s->epoch == epoch) {
		s->attached = false;
		s->publication = 0;
	}
}

static inline void apple_pmp_gpu_lease_publish(struct apple_pmp_gpu_lease *s,
	unsigned long long epoch, unsigned int level, unsigned long long sample_ns)
{
	/* Invalidation is always safe, including removal/mode changes. */
	if (!sample_ns)
		s->publication = 0;
	else if (s->attached && epoch == s->epoch)
		s->publication = apple_pmp_gpu_budget_pack(level, sample_ns);
}

static inline unsigned int apple_pmp_gpu_lease_level(const struct apple_pmp_gpu_lease *s,
	unsigned long long epoch, unsigned long long now)
{
	return s->attached && s->epoch == epoch ?
		apple_pmp_gpu_budget_level(s->publication, now) : 8;
}
#endif
