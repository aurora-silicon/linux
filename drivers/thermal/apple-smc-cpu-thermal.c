// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple J700 CPU thermal policy over selected SMC telemetry
 *
 * Copyright (C) 2026 Eryk Wieliczko
 */
#include <linux/cpu_cooling.h>
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
 * Two controls act on the hottest of the six selected sensors: a passive
 * trip bound to maximum cpufreq cooling (95 C, released below 85 C), and a
 * hard tier that pins both clusters to their minimum frequency through a
 * frequency-QoS guard (from 103 C, released below 98 C).  The guard also
 * holds the minimum while readings are missing, stale or failing, while the
 * zone is disabled, and after a cpufreq DVFS fault.
 *
 * The band sits above the idle temperature: the original 80 C trip with a
 * 70 C release latched the throttle permanently while the die idled at
 * 70-77 C with the spinning idle loop, because step_wise never saw it drop
 * below the release point.
 *
 * The thresholds are Linux operating policy, not Apple or silicon limits.
 * SMC readings can lag a successful read by about a second under load, so
 * faster host polling does not bound the sensor age; the 200 ms limit only
 * bounds the host transaction.
 */
#define SMC_TRIP_MC 95000
#define SMC_RELEASE_MC 85000
#define SMC_HOT_MC 103000
#define SMC_HOT_RELEASE_MC 98000
#define SMC_SAMPLE_MS 25
#define SMC_MAX_AGE_MS APPLE_SMC_THERMAL_MAX_AGE_MS
#define SMC_WATCH_MS 25

struct apple_smc_thermal_cpu {
	struct cpufreq_policy *policy;
	struct freq_qos_request guard;
	struct thermal_cooling_device *cdev;
	unsigned int min_freq;
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
			cpu->min_freq : FREQ_QOS_MAX_DEFAULT_VALUE);
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

static bool smc_zone_bind(struct thermal_zone_device *zone, const struct thermal_trip *trip,
			  struct thermal_cooling_device *cdev, struct cooling_spec *spec)
{
	unsigned int i;

	guard(mutex)(&smc_lock);
	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++)
		if (smc_cpus[i] && smc_cpus[i]->cdev == cdev) {
			spec->lower = cdev->max_state;
			spec->upper = cdev->max_state;
			return true;
		}
	return false;
}

static const struct thermal_zone_device_ops smc_zone_ops = {
	.get_temp = smc_zone_temp,
	.change_mode = smc_zone_mode,
	.should_bind = smc_zone_bind,
};

static const struct thermal_trip smc_trips[] = {
	{ .temperature = SMC_TRIP_MC, .hysteresis = SMC_TRIP_MC - SMC_RELEASE_MC,
	  .type = THERMAL_TRIP_PASSIVE },
};

static const struct thermal_zone_params smc_params = {
	.governor_name = "step_wise",
	.no_hwmon = true,
};

static int smc_check_binding(struct thermal_trip *trip, void *data)
{
	struct thermal_zone_device *zone = data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++)
		if (!thermal_trip_is_bound_to_cdev(zone, trip, smc_cpus[i]->cdev))
			return -ENODEV;
	return 0;
}

/* provider_lock held; never register/update a zone while holding smc_lock. */
static void smc_start_zone(void)
{
	struct thermal_zone_device *zone;
	int ret;

	if (smc_zone || !smc_read || !smc_cpus[0] || !smc_cpus[1] ||
	    !smc_cpus[0]->cdev || !smc_cpus[1]->cdev)
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
	ret = for_each_thermal_trip(zone, smc_check_binding, zone);
	if (ret) {
		thermal_zone_device_unregister(zone);
		pr_err("apple-smc-thermal: missing CPU cooling binding\n");
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
		if (smc_good < 3)
			smc_good++;
		if (value >= SMC_HOT_MC)
			smc_hot = true;
		else if (value < SMC_HOT_RELEASE_MC)
			smc_hot = false;
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
	cpu->cdev = cpufreq_cooling_register(cpu->policy);
	if (IS_ERR(cpu->cdev)) {
		pr_err("apple-smc-thermal: cooling registration failed: %ld\n", PTR_ERR(cpu->cdev));
		cpu->cdev = NULL;
		return;
	}
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
	cpufreq_cooling_unregister(cpu->cdev);
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
