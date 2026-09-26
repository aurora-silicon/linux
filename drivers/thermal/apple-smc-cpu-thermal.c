// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple J700 CPU thermal policy over selected SMC telemetry
 *
 * Copyright (C) 2026 Eryk Wieliczko
 */
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/soc/apple/smc-thermal.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

/*
 * The hottest of the six selected sensors drives a frequency cap on each
 * cluster through a frequency-QoS guard.  The cap walks the cluster's own
 * OPP table: every SMC_TIGHTEN_MS that the reading is at or above
 * SMC_HIGH_MC it moves one step tighter, plus one per 2 C above the band
 * (five at most), and every SMC_RELAX_MS that a low-pass filtered reading
 * (time constant about two seconds) is below SMC_LOW_MC it moves one step
 * looser.  The die sensor of this fanless machine moves 30 C within a
 * second of a clock change and the SMC updates it about once a second, so
 * a single trip bound to maximum cooling swung the P cluster between 4044
 * and 744 MHz every two seconds, and a walk driven only by the filtered
 * value never tightened while the readings alternated above and below the
 * band.  Tightening from the raw reading lands the walk near the step the
 * enclosure can sustain within a few readings; relaxing from the average
 * keeps it within one step of it.  A hard tier on the raw reading still pins both clusters to their
 * minimum from SMC_HOT_MC until SMC_HOT_RELEASE_MC, and the guard holds the
 * minimum while readings are missing, stale or failing, while the zone is
 * disabled, and after a cpufreq DVFS fault.
 *
 * The band sits above the idle temperature: the original 80 C trip with a
 * 70 C release latched the throttle permanently while the die idled at
 * 70-77 C with the spinning idle loop.
 *
 * The thresholds are Linux operating policy, not Apple or silicon limits.
 */
#define SMC_HIGH_MC 95000
#define SMC_LOW_MC 88000
#define SMC_HOT_MC 103000
#define SMC_HOT_RELEASE_MC 98000
#define SMC_TIGHTEN_MS 500
#define SMC_RELAX_MS 4000
/* Steps of the walk; a cluster with fewer OPPs maps them proportionally. */
#define SMC_STEPS 17
/* Filter weight per 25 ms sample, in 1/1024: about a two second time constant. */
#define SMC_FILTER_WEIGHT 13
#define SMC_SAMPLE_MS 25
#define SMC_MAX_AGE_MS APPLE_SMC_THERMAL_MAX_AGE_MS
#define SMC_WATCH_MS 25

struct apple_smc_thermal_cpu {
	struct cpufreq_policy *policy;
	struct freq_qos_request guard;
	unsigned int min_freq;
	unsigned int cap[SMC_STEPS];
	bool failed;
};

static DEFINE_MUTEX(smc_lock);
/* Only sampling/removal take this lock: a stuck SMC read cannot block guards. */
static DEFINE_MUTEX(smc_provider_lock);
static struct apple_smc_thermal_cpu *smc_cpus[2];
static struct thermal_zone_device *smc_zone;
static int (*smc_read)(void *, int *);
static void *smc_ctx;
static unsigned long smc_sample_time;
static unsigned int smc_good;
static int smc_value, smc_error = -ENODATA;
static bool smc_enabled, smc_hot;
static unsigned int smc_step;
static unsigned long smc_step_time;
static int smc_filtered;
static void smc_sample_work(struct work_struct *work);
static void smc_watch_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(smc_sample, smc_sample_work);
static DECLARE_DELAYED_WORK(smc_watch, smc_watch_work);

static bool smc_fresh(void)
{
	return !smc_error && time_before(jiffies,
					 smc_sample_time + msecs_to_jiffies(SMC_MAX_AGE_MS));
}

/* smc_lock held. QoS is independent of user limits and the thermal governor. */
static void smc_update_guards(void)
{
	bool healthy = smc_zone && smc_enabled && smc_fresh() && smc_good >= 3;
	bool clamp = !healthy || smc_hot;
	bool error = false;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++)
		if (smc_cpus[i] && READ_ONCE(smc_cpus[i]->failed))
			clamp = true;
	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++) {
		struct apple_smc_thermal_cpu *cpu = smc_cpus[i];
		int ret;

		if (!cpu)
			continue;
		ret = freq_qos_update_request(&cpu->guard, clamp || READ_ONCE(cpu->failed) ?
			cpu->min_freq : cpu->cap[smc_step]);
		if (ret < 0) {
			error = true;
			WRITE_ONCE(cpu->failed, true);
			freq_qos_update_request(&cpu->guard, cpu->min_freq);
			pr_err_ratelimited("apple-smc-thermal: CPU%u guard update failed: %d\n",
					   cpu->policy->cpu, ret);
		}
	}
	if (error)
		for (i = 0; i < ARRAY_SIZE(smc_cpus); i++)
			if (smc_cpus[i])
				freq_qos_update_request(&smc_cpus[i]->guard, smc_cpus[i]->min_freq);
}

static int smc_zone_temp(struct thermal_zone_device *zone, int *temp)
{
	guard(mutex)(&smc_lock);
	if (!smc_fresh())
		return smc_error ?: -ETIMEDOUT;
	*temp = smc_value;
	return 0;
}

static int smc_zone_mode(struct thermal_zone_device *zone, enum thermal_device_mode mode)
{
	guard(mutex)(&smc_lock);
	smc_enabled = mode == THERMAL_DEVICE_ENABLED;
	smc_good = 0;
	smc_update_guards();
	return 0;
}

static const struct thermal_zone_device_ops smc_zone_ops = {
	.get_temp = smc_zone_temp,
	.change_mode = smc_zone_mode,
};

/* The band and the hard tier as passive trips, for sysfs; nothing is bound to them. */
static const struct thermal_trip smc_trips[] = {
	{ .temperature = SMC_HIGH_MC, .hysteresis = SMC_HIGH_MC - SMC_LOW_MC,
	  .type = THERMAL_TRIP_PASSIVE },
	{ .temperature = SMC_HOT_MC, .hysteresis = SMC_HOT_MC - SMC_HOT_RELEASE_MC,
	  .type = THERMAL_TRIP_PASSIVE },
};

static const struct thermal_zone_params smc_params = {
	.governor_name = "step_wise",
	.no_hwmon = true,
};

/* provider_lock held; never register/update a zone while holding smc_lock. */
static void smc_start_zone(void)
{
	struct thermal_zone_device *zone;
	int ret;

	if (smc_zone || !smc_read || !smc_cpus[0] || !smc_cpus[1])
		return;
	zone = thermal_zone_device_register_with_trips("apple-smc-selected",
						       smc_trips,
						       ARRAY_SIZE(smc_trips),
						       NULL, &smc_zone_ops,
						       &smc_params,
						       SMC_SAMPLE_MS,
						       SMC_SAMPLE_MS);
	if (IS_ERR(zone)) {
		pr_err("apple-smc-thermal: zone registration failed: %ld\n", PTR_ERR(zone));
		return;
	}
	mutex_lock(&smc_lock);
	smc_zone = zone;
	mutex_unlock(&smc_lock);
	ret = thermal_zone_device_enable(zone);
	if (ret)
		pr_err("apple-smc-thermal: zone enable failed: %d; retaining minimum caps\n", ret);
}

static void smc_sample_work(struct work_struct *work)
{
	unsigned long start = jiffies;
	int value = 0, ret;

	guard(mutex)(&smc_provider_lock);
	if (!smc_read)
		return;
	ret = smc_read(smc_ctx, &value);
	if (!ret && time_after_eq(jiffies, start + msecs_to_jiffies(SMC_MAX_AGE_MS)))
		ret = -ETIMEDOUT;
	mutex_lock(&smc_lock);
	smc_error = ret;
	if (ret) {
		smc_good = 0;
	} else {
		smc_value = value;
		smc_sample_time = start; /* Bound the age of the oldest read in the round. */
		if (smc_good < 3) {
			smc_good++;
			smc_filtered = value;
		} else {
			smc_filtered += ((value - smc_filtered) * SMC_FILTER_WEIGHT) / 1024;
		}
		if (value >= SMC_HOT_MC)
			smc_hot = true;
		else if (value < SMC_HOT_RELEASE_MC)
			smc_hot = false;
		if (value >= SMC_HIGH_MC &&
		    time_after_eq(start, smc_step_time + msecs_to_jiffies(SMC_TIGHTEN_MS))) {
			/* One step, plus one per 2 C above the band, up to five. */
			unsigned int steps = 1 + min((value - SMC_HIGH_MC) / 2000, 4);

			smc_step = min(smc_step + steps, SMC_STEPS - 1);
			smc_step_time = start;
		} else if (smc_step > 0 && smc_filtered < SMC_LOW_MC &&
			   time_after_eq(start, smc_step_time + msecs_to_jiffies(SMC_RELAX_MS))) {
			smc_step--;
			smc_step_time = start;
		}
	}
	smc_update_guards();
	mutex_unlock(&smc_lock);
	if (smc_zone)
		thermal_zone_device_update(smc_zone, THERMAL_EVENT_UNSPECIFIED);
	queue_delayed_work(system_unbound_wq, &smc_sample, msecs_to_jiffies(SMC_SAMPLE_MS));
}

static void smc_watch_work(struct work_struct *work)
{
	guard(mutex)(&smc_lock);
	if (!smc_fresh())
		smc_good = 0;
	smc_update_guards();
	if (smc_cpus[0] || smc_cpus[1])
		queue_delayed_work(system_unbound_wq, &smc_watch, msecs_to_jiffies(SMC_WATCH_MS));
}

/*
 * The walk's caps for one cluster: step 0 is uncapped, the last step is the
 * lowest OPP, and the steps between map proportionally onto the cluster's
 * table so that both clusters descend together.
 */
static void smc_cpu_caps(struct apple_smc_thermal_cpu *cpu)
{
	struct cpufreq_frequency_table *pos, *table = cpu->policy->freq_table;
	unsigned int freqs[SMC_STEPS * 4], count = 0, i, j;

	cpufreq_for_each_valid_entry(pos, table) {
		if (count == ARRAY_SIZE(freqs))
			break;
		for (i = 0; i < count && freqs[i] > pos->frequency; i++)
			;
		for (j = count; j > i; j--)
			freqs[j] = freqs[j - 1];
		freqs[i] = pos->frequency;
		count++;
	}
	for (i = 0; i < SMC_STEPS; i++) {
		if (!i || !count) {
			cpu->cap[i] = FREQ_QOS_MAX_DEFAULT_VALUE;
			continue;
		}
		j = (i * (count - 1) + (SMC_STEPS - 1) / 2) / (SMC_STEPS - 1);
		cpu->cap[i] = freqs[min(j, count - 1)];
	}
}

struct apple_smc_thermal_cpu *apple_smc_thermal_cpu_add(struct cpufreq_policy *policy)
{
	struct apple_smc_thermal_cpu *cpu;
	unsigned int i;
	int ret;

	if (!of_machine_is_compatible("apple,j700"))
		return ERR_PTR(-ENODEV);
	cpu = kzalloc_obj(*cpu);
	if (!cpu)
		return ERR_PTR(-ENOMEM);
	cpu->policy = policy;
	cpu->min_freq = policy->freq_table[0].frequency;
	smc_cpu_caps(cpu);
	ret = freq_qos_add_request(&policy->constraints, &cpu->guard, FREQ_QOS_MAX,
				   policy->freq_table[0].frequency);
	if (ret < 0) {
		kfree(cpu);
		return ERR_PTR(ret);
	}
	mutex_lock(&smc_provider_lock);
	mutex_lock(&smc_lock);
	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++)
		if (!smc_cpus[i]) {
			smc_cpus[i] = cpu;
			break;
		}
	mutex_unlock(&smc_lock);
	mutex_unlock(&smc_provider_lock);
	if (i == ARRAY_SIZE(smc_cpus)) {
		freq_qos_remove_request(&cpu->guard);
		kfree(cpu);
		return ERR_PTR(-EBUSY);
	}
	queue_delayed_work(system_unbound_wq, &smc_watch, 0);
	return cpu;
}
EXPORT_SYMBOL_GPL(apple_smc_thermal_cpu_add);

void apple_smc_thermal_cpu_ready(struct apple_smc_thermal_cpu *cpu)
{
	if (!cpu)
		return;
	guard(mutex)(&smc_provider_lock);
	smc_start_zone();
}
EXPORT_SYMBOL_GPL(apple_smc_thermal_cpu_ready);

void apple_smc_thermal_cpu_fault(struct apple_smc_thermal_cpu *cpu)
{
	if (!cpu)
		return;
	/* Called inside target_index: do not recursively take the policy lock. */
	WRITE_ONCE(cpu->failed, true);
	mod_delayed_work(system_unbound_wq, &smc_watch, 0);
}
EXPORT_SYMBOL_GPL(apple_smc_thermal_cpu_fault);

void apple_smc_thermal_cpu_remove(struct apple_smc_thermal_cpu *cpu)
{
	struct thermal_zone_device *zone;
	unsigned int i;

	if (!cpu)
		return;
	guard(mutex)(&smc_provider_lock);
	mutex_lock(&smc_lock);
	zone = smc_zone;
	smc_zone = NULL;
	smc_enabled = false;
	smc_good = 0;
	smc_update_guards();
	mutex_unlock(&smc_lock);
	if (zone)
		thermal_zone_device_unregister(zone);
	mutex_lock(&smc_lock);
	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++)
		if (smc_cpus[i] == cpu)
			smc_cpus[i] = NULL;
	freq_qos_remove_request(&cpu->guard);
	mutex_unlock(&smc_lock);
	kfree(cpu);
}
EXPORT_SYMBOL_GPL(apple_smc_thermal_cpu_remove);

static void smc_provider_remove(void *unused)
{
	mutex_lock(&smc_provider_lock);
	smc_read = NULL;
	smc_ctx = NULL;
	mutex_lock(&smc_lock);
	smc_error = -ENODEV;
	smc_good = 0;
	smc_update_guards();
	mutex_unlock(&smc_lock);
	mutex_unlock(&smc_provider_lock);
	cancel_delayed_work_sync(&smc_sample);
}

int devm_apple_smc_thermal_register(struct device *dev, int (*read)(void *, int *), void *ctx)
{
	int ret;

	if (!of_machine_is_compatible("apple,j700"))
		return -ENODEV;
	mutex_lock(&smc_provider_lock);
	if (smc_read) {
		mutex_unlock(&smc_provider_lock);
		return -EBUSY;
	}
	smc_ctx = ctx;
	smc_read = read;
	smc_start_zone();
	mutex_unlock(&smc_provider_lock);
	ret = devm_add_action_or_reset(dev, smc_provider_remove, NULL);
	if (!ret)
		queue_delayed_work(system_unbound_wq, &smc_sample, 0);
	return ret;
}
EXPORT_SYMBOL_GPL(devm_apple_smc_thermal_register);
