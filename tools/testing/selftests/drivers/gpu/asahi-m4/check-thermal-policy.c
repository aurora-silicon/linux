// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Exercise the production C policy, shared by the CPU and GPU consumers. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/soc/apple/pmp-cooling.h>

static const char *reason_name(int reason)
{
	static const char *const names[] = {
		"startup", "sensor", "measured-heat", "prediction", "governor", "recovery",
	};
	return names[reason];
}

/* Legacy trace format retained so the immutable reference remains useful.
 * Actual GPU read scheduling is tested separately against the Rust helper.
 */
static bool trace_due(const struct apple_pmp_cooling *p, unsigned long long now)
{
	return !p->last_ns || now - p->last_ns >= 5000000ULL;
}

static unsigned int selector(unsigned int configured, unsigned int cap,
			     unsigned int user)
{
	unsigned int result = cap < 9 ? cap : 9;

	if (configured < result)
		result = configured;
	if (user < result)
		result = user;
	return result ? result : 1;
}

int main(void)
{
	struct apple_pmp_cooling p;
	char op;
	unsigned long long now, query;
	long mc;
	int error;
	unsigned int governor, power, configured, cap, user;

	apple_pmp_cooling_init(&p);
	while (scanf(" %c %llu %ld %d %u %u %u %u %u %llu", &op, &now, &mc,
		     &error, &governor, &power, &configured, &cap, &user, &query) == 10) {
		bool due_before = trace_due(&p, now);
		unsigned int ceiling = 0;
		const char *reason = "none";

		switch (op) {
		case 'R':
			apple_pmp_cooling_init(&p);
			break;
		case 'S':
			apple_pmp_cooling_sample(&p, now, mc, error);
			break;
		case 'O':
			apple_pmp_cooling_observe(&p, now, governor);
			break;
		case 'C': {
			unsigned int level, power_limit;

			apple_pmp_cooling_observe(&p, p.last_ns, governor);
			level = apple_pmp_cooling_level(&p);
			if (governor > 8)
				governor = 8;
			if (power > 8)
				power = 8;
			ceiling = 9 - (governor > level ? governor : level);
			power_limit = 9 - power;
			reason = power && power_limit < ceiling ? "power-budget" : reason_name(p.reason);
			if (power_limit < ceiling)
				ceiling = power_limit;
			break;
		}
		case 'D': case 'L': case 'H':
			break;
		default:
			return 1;
		}
		printf("%llu %llu %llu %llu %llu %ld %ld %ld %u %u %u %u %u %s %u %u %u %s %u %u\n",
		       p.last_ns, p.good_ns, p.anchor_ns, p.release_ns, p.dt_ns,
		       p.anchor_mc, p.previous_mc, p.projected_mc, p.good_reads,
		       p.prediction, p.level, p.hard, p.anchor_valid, reason_name(p.reason),
		       due_before, trace_due(&p, query), ceiling, reason,
		       selector(configured, cap, user), apple_pmp_heat_level(mc));
	}
	return ferror(stdin) ? 1 : 0;
}
