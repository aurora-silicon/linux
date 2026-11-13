// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Measurement only: runtime switches for submission-path changes, so that
 * every variant can be compared on one boot. Each bit enables one change;
 * zero leaves the driver's behaviour unchanged. Bit 0 is a control that no
 * change reads.
 */

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/types.h>
#include <asm/page.h>

static unsigned int gpubench_opt;
module_param(gpubench_opt, uint, 0644);
MODULE_PARM_DESC(gpubench_opt, "Measurement: enabled submission-path changes (bit mask)");

unsigned int asahi_g17_gpubench_opt(void);
unsigned int asahi_g17_gpubench_opt(void)
{
	return READ_ONCE(gpubench_opt);
}

/*
 * Completion-path latency (bit 8): latency.rs reports one delta per kind for each
 * retired job, from the event worker only, so every statistic has one writer.
 * gpubench_lat prints them per engine in microseconds; any write clears them.
 */
#define LAT_ENGINES	2
#define LAT_KINDS	10
#define LAT_FINE	64	/* 250 ns buckets below 16 us */
#define LAT_COARSE	240	/* 1 us buckets from 16 us to 256 us */
#define LAT_BUCKETS	(LAT_FINE + LAT_COARSE + 1)

struct lat_stat {
	u64 count;
	u64 missing;
	u64 negative;
	u64 sum;
	u64 min;
	u64 max;
	u32 hist[LAT_BUCKETS];
};

static struct lat_stat lat_stats[LAT_ENGINES][LAT_KINDS];

static const char *const lat_engines[LAT_ENGINES] = { "compute", "render" };

/* In latency.rs's order. */
static const char *const lat_kinds[LAT_KINDS] = {
	"doorbell_irq", "irq_pass", "pass_locked", "locked_signal", "signal_call",
	"doorbell_signal", "doorbell_gpu_start", "gpu_busy", "gpu_end_irq",
	"signal_next_doorbell",
};

static unsigned int lat_bucket(u64 ns)
{
	if (ns < LAT_FINE * 250ULL)
		return ns / 250;
	if (ns < LAT_FINE * 250ULL + LAT_COARSE * 1000ULL)
		return LAT_FINE + (ns - LAT_FINE * 250ULL) / 1000;
	return LAT_BUCKETS - 1;
}

/* The upper edge of bucket @i in ns (the last bucket's is its maximum). */
static u64 lat_bucket_end(unsigned int i)
{
	if (i < LAT_FINE)
		return (i + 1) * 250ULL;
	return LAT_FINE * 250ULL + (i - LAT_FINE + 1) * 1000ULL;
}

void asahi_g17_lat_record(u32 engine, u32 kind, s64 ns);
void asahi_g17_lat_record(u32 engine, u32 kind, s64 ns)
{
	struct lat_stat *s;
	unsigned int bucket;
	u64 v;

	if (engine >= LAT_ENGINES || kind >= LAT_KINDS)
		return;
	s = &lat_stats[engine][kind];
	if (ns == S64_MIN) {
		WRITE_ONCE(s->missing, s->missing + 1);
		return;
	}
	if (ns < 0) {
		WRITE_ONCE(s->negative, s->negative + 1);
		return;
	}
	v = ns;
	bucket = lat_bucket(v);
	if (!s->count || v < s->min)
		WRITE_ONCE(s->min, v);
	if (v > s->max)
		WRITE_ONCE(s->max, v);
	WRITE_ONCE(s->sum, s->sum + v);
	WRITE_ONCE(s->hist[bucket], s->hist[bucket] + 1);
	WRITE_ONCE(s->count, s->count + 1);
}

/* The upper edge of the bucket holding the @pct percentile, capped at the maximum. */
static u64 lat_percentile(const struct lat_stat *s, u64 count, unsigned int pct)
{
	u64 rank = max_t(u64, (count * pct + 99) / 100, 1);
	u64 top = READ_ONCE(s->max);
	u64 seen = 0;
	unsigned int i;

	for (i = 0; i < LAT_BUCKETS - 1; i++) {
		seen += READ_ONCE(s->hist[i]);
		if (seen >= rank)
			return min(lat_bucket_end(i), top);
	}
	return top;
}

static int lat_us(char *buffer, int len, u64 ns)
{
	return scnprintf(buffer + len, PAGE_SIZE - len, " %llu.%02llu",
			 ns / 1000, ns % 1000 / 10);
}

static int lat_get(char *buffer, const struct kernel_param *kp)
{
	unsigned int engine, kind;
	int len;

	len = scnprintf(buffer, PAGE_SIZE,
			"engine kind count missing negative min p50 p90 p99 max mean\n");
	for (engine = 0; engine < LAT_ENGINES; engine++) {
		for (kind = 0; kind < LAT_KINDS; kind++) {
			const struct lat_stat *s = &lat_stats[engine][kind];
			u64 count = READ_ONCE(s->count);

			len += scnprintf(buffer + len, PAGE_SIZE - len, "%s %s %llu %llu %llu",
					 lat_engines[engine], lat_kinds[kind], count,
					 READ_ONCE(s->missing), READ_ONCE(s->negative));
			len += lat_us(buffer, len, count ? READ_ONCE(s->min) : 0);
			len += lat_us(buffer, len, lat_percentile(s, count, 50));
			len += lat_us(buffer, len, lat_percentile(s, count, 90));
			len += lat_us(buffer, len, lat_percentile(s, count, 99));
			len += lat_us(buffer, len, READ_ONCE(s->max));
			len += lat_us(buffer, len, count ? READ_ONCE(s->sum) / count : 0);
			len += scnprintf(buffer + len, PAGE_SIZE - len, "\n");
		}
	}
	return len;
}

static int lat_set(const char *val, const struct kernel_param *kp)
{
	memset(lat_stats, 0, sizeof(lat_stats));
	return 0;
}

static const struct kernel_param_ops lat_ops = {
	.set = lat_set,
	.get = lat_get,
};
module_param_cb(gpubench_lat, &lat_ops, NULL, 0644);
MODULE_PARM_DESC(gpubench_lat, "Measurement: completion-path latency per engine (write to clear)");
