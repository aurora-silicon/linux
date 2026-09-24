// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J700 Linux policy over selected SMC telemetry, not a silicon safety limit. */
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

/* Linux operating policy, not an Apple threshold. SMC publication can lag
 * successful reads by roughly a second in observed load traces. Faster host
 * polling does not bound sensor age; higher OPP admission must remain limited.
 */
/*
 * Two controls: a passive trip bound to maximum cpufreq cooling, and a
 * hard tier that independently pins both clusters to
 * their minimum frequency through the QoS guard.  The PMP policy selects a 105 C
 * SMC backup threshold; this is not a qualified silicon safety limit.
 *
 * While the PMP fast policy (CONFIG_APPLE_PMP_V2_THERMAL) runs it retunes
 * this zone into the independent backup at 105 C, leaving these tiers to the
 * 25 ms PMP zone.
 *
 * The original qualification numbers were 80 C passive / 70 C release.  That
 * band held the throttle under the then-current idle policy: the die sat at
 * 70-77 C at idle and light load (measured 2026-09-20, load average 0.4,
 * one browser), i.e. permanently
 * inside the step_wise hold band.  The first excursion past 80 C latched the
 * throttle and it could never walk back out, because walking back out needs
 * < 70 C and the only way to get there was the minimum-frequency guard itself.
 * Observed result: P cluster held at 1.26-2.20 GHz of 4.044 GHz indefinitely.
 * The band is therefore moved above the idle temperature, keeping the same
 * 10 C hysteresis and staying under the selected 105 C backup.
 * This observation does not describe the later state-preserving WFI idle
 * path, which has a different idle power and temperature profile.
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
#ifdef CONFIG_APPLE_PMP_V2_THERMAL
static struct thermal_zone_device *smc_aux_zone;
#endif
static int (*smc_read)(void *, int *);
static void *smc_ctx;
static unsigned long smc_sample_time;
static unsigned int smc_good;
static int smc_value, smc_error = -ENODATA;
static bool smc_enabled, smc_hot;
static int smc_hot_mc = SMC_HOT_MC, smc_hot_release_mc = SMC_HOT_RELEASE_MC;
static void smc_sample_work(struct work_struct *work);
static void smc_watch_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(smc_sample, smc_sample_work);
static DECLARE_DELAYED_WORK(smc_watch, smc_watch_work);

static bool smc_fresh(void)
{
	return !smc_error && time_before(jiffies,
					 smc_sample_time + msecs_to_jiffies(SMC_MAX_AGE_MS));
}

#ifdef CONFIG_APPLE_PMP_V2_THERMAL
/* A nonblocking admission snapshot. Zero revokes; the deadline also expires
 * if the SMC workers stop. This is host freshness, not sensor publication age.
 */
static u64 smc_until;
static u64 smc_health_until;
static u64 smc_sample_start;

bool apple_smc_thermal_live(void)
{
	return ktime_get_boottime_ns() < READ_ONCE(smc_until);
}

bool apple_smc_thermal_healthy(void)
{
	return ktime_get_boottime_ns() < READ_ONCE(smc_health_until);
}

bool apple_smc_thermal_ready(void)
{
	unsigned int i;

	guard(mutex)(&smc_lock);
	if (!smc_zone || !smc_enabled || !smc_fresh() || smc_good < 3 || smc_hot ||
	    smc_value >= smc_hot_release_mc)
		return false;
	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++)
		if (!smc_cpus[i] || READ_ONCE(smc_cpus[i]->failed))
			return false;
	return true;
}
#endif

/* smc_lock held. QoS is independent of user limits and the thermal governor. */
static void smc_update_guards(void)
{
	bool healthy = smc_zone && smc_enabled && smc_fresh() && smc_good >= 3;
	bool clamp = !healthy || smc_hot;
	bool error = false;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++)
		if (smc_cpus[i] && READ_ONCE(smc_cpus[i]->failed)) {
			clamp = true;
			healthy = false;
		}
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
#ifdef CONFIG_APPLE_PMP_V2_THERMAL
	WRITE_ONCE(smc_health_until, healthy && !error && smc_cpus[0] && smc_cpus[1] ?
		   smc_sample_start + SMC_MAX_AGE_MS * 1000000ULL : 0);
	WRITE_ONCE(smc_until, !clamp && !error && smc_cpus[0] && smc_cpus[1] &&
		   smc_value < smc_hot_release_mc ? smc_sample_start +
		   SMC_MAX_AGE_MS * 1000000ULL : 0);
#endif
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

#ifdef CONFIG_APPLE_PMP_V2_THERMAL

struct smc_aux_trip_update {
	struct thermal_zone_device *zone;
	int temperature, hysteresis;
};

static int smc_aux_set_trip(struct thermal_trip *trip, void *data)
{
	struct smc_aux_trip_update *update = data;

	/* Only retune the primary trip; preserve additional PMP cooling tiers. */
	if (THERMAL_TRIP_PRIV_TO_INT(trip->priv) != 0)
		return 0;
	WRITE_ONCE(trip->hysteresis, update->hysteresis);
	thermal_zone_set_trip_temp(update->zone, trip, update->temperature);
	return 0;
}

bool apple_smc_thermal_aux_matches(struct thermal_cooling_device *cdev)
{
	unsigned int i;
	bool found = false;

	guard(mutex)(&smc_lock);
	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++)
		if (smc_cpus[i] && smc_cpus[i]->cdev == cdev)
			found = true;
	return found;
}

int apple_smc_thermal_aux_cooling_floor(struct thermal_cooling_device *cdev,
				      unsigned int max_khz, unsigned long *state)
{
	unsigned int i;

	guard(mutex)(&smc_lock);
	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++) {
		struct apple_smc_thermal_cpu *cpu = smc_cpus[i];
		struct cpufreq_frequency_table *entry;
		unsigned long count = 0, above = 0;

		if (!cpu || cpu->cdev != cdev)
			continue;
		/* Neo's cooling devices use the pinned, sorted cpufreq table.
		 * Cooling state zero is its highest frequency, not P-state zero.
		 */
		if (!cpu->policy || !cpu->policy->freq_table ||
		    cpu->policy->freq_table_sorted == CPUFREQ_TABLE_UNSORTED)
			return -EINVAL;
		cpufreq_for_each_valid_entry(entry, cpu->policy->freq_table) {
			count++;
			if (entry->frequency > max_khz)
				above++;
		}
		if (count < 2 || count - 1 != cdev->max_state)
			return -EINVAL;
		/* Also reduce the E cluster by at least one state at the trip. */
		*state = clamp(above, 1UL, cdev->max_state);
		return 0;
	}
	return -ENODEV;
}

int apple_smc_thermal_aux_register(const struct thermal_zone_device_ops *ops,
				 const struct thermal_trip *trips, int count)
{
	struct thermal_zone_device *zone;
	int ret;

	guard(mutex)(&smc_provider_lock);
	if (smc_aux_zone || !smc_zone || !smc_cpus[0] || !smc_cpus[1] ||
	    !smc_cpus[0]->cdev || !smc_cpus[1]->cdev)
		return -ENODEV;
	/* Production sampling belongs to the normal thermal-core polling. */
	zone = thermal_zone_device_register_with_trips("apple-pmp-selected", trips,
		count, NULL, ops, &smc_params, 25, 25);
	if (IS_ERR(zone))
		return PTR_ERR(zone);
	ret = for_each_thermal_trip(zone, smc_check_binding, zone);
	if (ret) {
		thermal_zone_device_unregister(zone);
		return ret;
	}
	smc_aux_zone = zone;
	ret = thermal_zone_device_enable(zone);
	if (ret) {
		smc_aux_zone = NULL;
		thermal_zone_device_unregister(zone);
	}
	return ret;
}

#ifdef CONFIG_APPLE_PMP_V2_THERMAL
bool apple_smc_thermal_aux_cooling_ready(void)
{
	unsigned int i;
	unsigned long state;

	/* Called inside a zone update: never retake smc_provider_lock here. */
	guard(mutex)(&smc_lock);
	for (i = 0; i < ARRAY_SIZE(smc_cpus); i++) {
		struct thermal_cooling_device *cdev = smc_cpus[i] ? smc_cpus[i]->cdev : NULL;

		if (!cdev || cdev->ops->get_cur_state(cdev, &state) || !state)
			return false;
	}
	return true;
}
#endif

void apple_smc_thermal_aux_update(void)
{
	guard(mutex)(&smc_provider_lock);
	if (smc_aux_zone)
		thermal_zone_device_update(smc_aux_zone, THERMAL_EVENT_UNSPECIFIED);
}

/* Retune the auxiliary zone's primary trip and move this SMC zone, both its
 * passive trip and its hard QoS tier, to the backup thresholds.
 */
int apple_smc_thermal_aux_set_policy(int temperature, int hysteresis,
				     int backup_trip, int backup_release)
{
	struct smc_aux_trip_update update, backup;

	guard(mutex)(&smc_provider_lock);
	if (!smc_aux_zone || !smc_zone || temperature <= hysteresis || hysteresis < 0 ||
	    backup_release <= 0 || backup_trip <= backup_release ||
	    backup_trip <= temperature)
		return -EINVAL;
	mutex_lock(&smc_lock);
	smc_hot_mc = backup_trip;
	smc_hot_release_mc = backup_release;
	if (smc_value >= smc_hot_mc)
		smc_hot = true;
	else if (smc_value < smc_hot_release_mc)
		smc_hot = false;
	smc_update_guards();
	mutex_unlock(&smc_lock);
	update = (struct smc_aux_trip_update){
		.zone = smc_aux_zone,
		.temperature = temperature,
		.hysteresis = hysteresis,
	};
	backup = (struct smc_aux_trip_update){
		.zone = smc_zone,
		.temperature = backup_trip,
		.hysteresis = backup_trip - backup_release,
	};
	thermal_zone_for_each_trip(smc_aux_zone, smc_aux_set_trip, &update);
	thermal_zone_for_each_trip(smc_zone, smc_aux_set_trip, &backup);
	thermal_zone_device_update(smc_aux_zone, THERMAL_TRIP_CHANGED);
	thermal_zone_device_update(smc_zone, THERMAL_TRIP_CHANGED);
	pr_info("apple-smc-thermal: backup policy: SMC passive trip %d mC, minimum from %d mC (release %d mC)\n",
		backup_trip, backup_trip, backup_release);
	return 0;
}

/* The auxiliary policy gave up: drop its zone and restore this zone's own
 * two-tier policy (95 C passive, 103 C minimum) as the sole throttle.
 */
void apple_smc_thermal_aux_unregister(void)
{
	struct smc_aux_trip_update backup = {
		.temperature = SMC_TRIP_MC,
		.hysteresis = SMC_TRIP_MC - SMC_RELEASE_MC,
	};
	struct thermal_zone_device *zone;

	guard(mutex)(&smc_provider_lock);
	zone = smc_aux_zone;
	smc_aux_zone = NULL;
	if (zone) {
		thermal_zone_device_disable(zone);
		thermal_zone_device_unregister(zone);
	}
	mutex_lock(&smc_lock);
	smc_hot_mc = SMC_HOT_MC;
	smc_hot_release_mc = SMC_HOT_RELEASE_MC;
	if (smc_value >= smc_hot_mc)
		smc_hot = true;
	else if (smc_value < smc_hot_release_mc)
		smc_hot = false;
	smc_update_guards();
	mutex_unlock(&smc_lock);
	if (!smc_zone)
		return;
	backup.zone = smc_zone;
	thermal_zone_for_each_trip(smc_zone, smc_aux_set_trip, &backup);
	thermal_zone_device_update(smc_zone, THERMAL_TRIP_CHANGED);
	pr_info("apple-smc-thermal: two-tier policy restored: passive %d mC, minimum from %d mC\n",
		SMC_TRIP_MC, SMC_HOT_MC);
}
#endif

/* provider_lock held; never register/update a zone while holding smc_lock. */
static void smc_start_zone(void)
{
	struct thermal_zone_device *zone;
	int ret;

	if (smc_zone || !smc_read || !smc_cpus[0] || !smc_cpus[1] ||
	    !smc_cpus[0]->cdev || !smc_cpus[1]->cdev)
		return;
	zone = thermal_zone_device_register_with_trips("apple-smc-selected", smc_trips,
		ARRAY_SIZE(smc_trips), NULL, &smc_zone_ops, &smc_params,
		SMC_SAMPLE_MS, SMC_SAMPLE_MS);
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
#ifdef CONFIG_APPLE_PMP_V2_THERMAL
	u64 sample_start = ktime_get_boottime_ns();
#endif
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
#ifdef CONFIG_APPLE_PMP_V2_THERMAL
		smc_sample_start = sample_start;
#endif
		if (smc_good < 3)
			smc_good++;
		if (value >= smc_hot_mc)
			smc_hot = true;
		else if (value < smc_hot_release_mc)
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

void apple_smc_thermal_cpu_fault(struct apple_smc_thermal_cpu *cpu)
{
	if (!cpu)
		return;
	/* Called inside target_index: do not recursively take the policy lock. */
	WRITE_ONCE(cpu->failed, true);
#ifdef CONFIG_APPLE_PMP_V2_THERMAL
	WRITE_ONCE(smc_until, 0);
	WRITE_ONCE(smc_health_until, 0);
#endif
	mod_delayed_work(system_unbound_wq, &smc_watch, 0);
}

void apple_smc_thermal_cpu_remove(struct apple_smc_thermal_cpu *cpu)
{
	struct thermal_zone_device *zone;
	unsigned int i;

	if (!cpu)
		return;
	guard(mutex)(&smc_provider_lock);
#ifdef CONFIG_APPLE_PMP_V2_THERMAL
	/* Revoke the auxiliary consumer before its shared cooling devices vanish. */
	if (smc_aux_zone) {
		thermal_zone_device_disable(smc_aux_zone);
		thermal_zone_device_unregister(smc_aux_zone);
		smc_aux_zone = NULL;
	}
#endif
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
