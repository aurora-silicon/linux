/* SPDX-License-Identifier: GPL-2.0-only */
/* Standard polled thermal policy, included by the pinned native PMP owner.
 * A boot-time frequency setting alone is not a replacement for runtime
 * thermal control; the independent SMC policy stays active as the backup.
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/thermal.h>
#include <linux/soc/apple/pmp-cpufreq.h>
#include <linux/soc/apple/smc-thermal.h>

/* Not Apple safety limits or a peak guarantee.  The band must sit above the
 * die's idle temperature or step_wise can never leave it: J700 idles at
 * 70-77 C, so the original 80 C trip / 70 C release latched the throttle
 * permanently (see the rationale in drivers/thermal/apple-smc-cpu-thermal.c).
 * Release is PT_TRIP_MC - PT_HYST_MC = 85 C, clear of idle; the minimum tier
 * and the SMC backup stay ordered 95 < 100 < 105 C.
 */
#define PT_TRIP_MC 95000
#define PT_HYST_MC 10000
#define PT_MINIMUM_TRIP_MC 100000
#define PT_FIRST_CAP_KHZ 2472000
#define PT_POLL_MS 25

static DEFINE_MUTEX(pt_lock);
static struct pmp_v2 *pt_owner;
static struct cpufreq_policy *pt_policy[2];
static struct freq_qos_request pt_qos[2];
static unsigned int pt_applied[2], pt_good, pt_waits;
static u64 pt_timestamp[2];
static bool pt_attached, pt_configured, pt_stopping, pt_zone_enabled, pt_failed;
static bool pt_admitted;
static int pt_error = -ENODATA, pt_temperature, pt_peak = -100000;
static unsigned int pt_failures, pt_recoveries;
/* Expose the full OPP range; thermal and user QoS limits still apply. */
static unsigned int max_pstate = 17, max_estate = 6;
module_param(max_pstate, uint, 0444);
MODULE_PARM_DESC(max_pstate, "P state ceiling (1..17); default 17, thermal limits still apply");
module_param(max_estate, uint, 0444);
MODULE_PARM_DESC(max_estate, "E state ceiling (1..6); thermal limits still apply");

static void pt_start(struct work_struct *work);
static DECLARE_DELAYED_WORK(pt_start_work, pt_start);
/* J700 operating choice: if this policy never starts (firmware identity,
 * DMA/APF or RTKit startup failure) the tree's previously qualified regime,
 * the independent two-tier SMC policy over the full OPP range, takes over
 * instead of pinning both clusters to their minimum state until reboot.
 * Faults after admission keep the fail-closed minimum caps and recovery.
 */
static bool startup_fallback = true;
module_param(startup_fallback, bool, 0444);
MODULE_PARM_DESC(startup_fallback, "Hand over to the SMC policy if the PMP policy never starts (default Y)");

/* This policy samples only while the cores are awake, so it requires idle=nop
 * -- and idle=nop is expensive: arm64's ARM64_IDLE_NOP takes neither the wfi()
 * nor the yield branch of cpu_do_idle(), so every core spins at full clock
 * whenever the system is idle.  Measured on J700 2026-09-20: 43.4 C idle with
 * idle=wfi against 85-92 C (peaks of 106.7 C) with idle=nop, at the same load
 * average.  That heat is what forced the cpufreq cooling states down and made
 * the machine sluggish.  Setting pmp_v2.thermal_policy=0 boots idle=wfi and
 * leaves the independent two-tier SMC policy as the sole throttle -- the same
 * regime startup_fallback selects.
 */
static bool thermal_policy = true;
module_param(thermal_policy, bool, 0444);
MODULE_PARM_DESC(thermal_policy, "Run the PMP fast thermal policy (default Y; requires idle=nop). N leaves the SMC policy as the throttle and allows idle=wfi.");

static unsigned int pt_policy_frequency(struct cpufreq_policy *policy, unsigned int state)
{
	struct cpufreq_frequency_table *entry;

	cpufreq_for_each_valid_entry(entry, policy->freq_table)
		if (entry->driver_data == state)
			return entry->frequency;
	return 0;
}

/* pt_lock held. No private cpufreq callbacks acquire this mutex: constraints
 * are the sole policy actuator. Try BOTH clusters even after an error.
 */
static int pt_guard(bool release)
{
	unsigned int i, state[2] = { max_estate, max_pstate };
	int ret, error = 0;

	if (!pt_attached)
		return -ENODEV;
	release &= pt_configured && pt_zone_enabled && !pt_stopping && !pt_failed;
	for (i = 0; i < 2; i++) {
		unsigned int ceiling = release ? state[i] : 1;

		ret = freq_qos_update_request(&pt_qos[i],
			pt_policy_frequency(pt_policy[i], ceiling));
		pt_applied[i] = ret < 0 ? 0 : ceiling;
		if (ret < 0)
			error = ret;
	}
	if (error) {
		pt_failed = true;
		/* A partial release must not leave the other cluster unrestricted. */
		for (i = 0; i < 2; i++) {
			ret = freq_qos_update_request(&pt_qos[i],
				pt_policy_frequency(pt_policy[i], 1));
			pt_applied[i] = ret < 0 ? 0 : 1;
		}
	}
	return error;
}

static int pt_temp(struct thermal_zone_device *zone, int *temperature)
{
	struct pmp_sample_diagnostic diagnostic;
	u64 timestamp[2], ages[2];
	int cluster[2], value, ret;
	bool release;

	guard(mutex)(&pt_lock);
	if (!pt_configured || pt_stopping || pt_failed)
		return -ENODATA;

	/* Validation still checks firmware identity, coherent provider records,
	 * and <=5 ms delivery age AT THE READ. This is not a 5 ms permission
	 * lease between polls; no high-frequency reader or expiry timer exists.
	 */
	ret = pmp_v2_sample(pt_owner, timestamp, ages, &value, cluster, &diagnostic);
	if (!ret && (timestamp[0] <= pt_timestamp[0] ||
		     timestamp[1] <= pt_timestamp[1]))
		ret = -EAGAIN;
	if (!ret && !apple_smc_thermal_healthy())
		ret = -ENODATA;
	if (ret) {
		pt_good = 0;
		pt_admitted = false;
		pt_error = ret;
		pt_failures++;
		pt_guard(false);
		return ret;
	}

	pt_timestamp[0] = timestamp[0];
	pt_timestamp[1] = timestamp[1];
	pt_temperature = *temperature = value;
	pt_peak = max(pt_peak, value);
	pt_error = 0;
	if (pt_good < 3)
		pt_good++;
	/* As on J713, do not release an error/startup guard while hot until
	 * standard cooling has actually engaged for BOTH clusters.
	 */
	release = pt_good == 3 && (value < PT_TRIP_MC ||
				  apple_smc_thermal_aux_cooling_ready());
	ret = pt_guard(release);
	if (ret) {
		pt_error = ret;
		pt_admitted = false;
		return ret;
	}
	release &= pt_zone_enabled;
	if (release && !pt_admitted)
		pt_recoveries++;
	pt_admitted = release;
	return 0;
}

static int pt_mode(struct thermal_zone_device *zone, enum thermal_device_mode mode)
{
	guard(mutex)(&pt_lock);
	pt_zone_enabled = mode == THERMAL_DEVICE_ENABLED;
	pt_good = 0;
	pt_admitted = false;
	return pt_attached ? pt_guard(false) : 0;
}

static bool pt_bind(struct thermal_zone_device *zone, const struct thermal_trip *trip,
		    struct thermal_cooling_device *cdev, struct cooling_spec *spec)
{
	unsigned long lower;
	int id = THERMAL_TRIP_PRIV_TO_INT(trip->priv);

	if (!apple_smc_thermal_aux_matches(cdev) || !cdev->max_state)
		return false;
	if (id == 1) {
		/* Skip all intermediate states at the upper passive trip. */
		lower = cdev->max_state;
	} else if (id == 0) {
		if (apple_smc_thermal_aux_cooling_floor(cdev, PT_FIRST_CAP_KHZ, &lower))
			return false;
	} else {
		return false;
	}
	spec->lower = lower;
	spec->upper = cdev->max_state;
	return true;
}

static const struct thermal_zone_device_ops pt_ops = {
	.get_temp = pt_temp, .change_mode = pt_mode, .should_bind = pt_bind,
};
static const struct thermal_trip pt_trips[] = {
	{ .type = THERMAL_TRIP_PASSIVE, .temperature = PT_TRIP_MC,
	  .hysteresis = PT_HYST_MC },
	{ .type = THERMAL_TRIP_PASSIVE, .temperature = PT_MINIMUM_TRIP_MC,
	  .hysteresis = PT_HYST_MC, .priv = THERMAL_INT_TO_TRIP_PRIV(1) },
};

static int pt_offline(unsigned int cpu)
{
	return READ_ONCE(pt_attached) ? -EBUSY : 0;
}

static int pt_attach(void)
{
	struct cpufreq_policy *policies[2] = {};
	unsigned int cpu, i, added = 0;
	int ret = -EPROBE_DEFER;

	guard(cpus_read_lock)();
	if (num_online_cpus() != 6)
		return -EPROBE_DEFER;
	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		unsigned int weight;

		if (!policy)
			goto fail;
		weight = cpumask_weight(policy->related_cpus);
		if ((weight != 2 && weight != 4) || !cpumask_equal(policy->related_cpus, policy->cpus)) {
			cpufreq_cpu_put(policy);
			ret = -EINVAL;
			goto fail;
		}
		i = weight == 2;
		if (!policies[i])
			policies[i] = policy;
		else {
			bool same = policies[i] == policy;

			cpufreq_cpu_put(policy);
			if (!same) {
				ret = -EINVAL;
				goto fail;
			}
		}
	}
	if (!policies[0] || !policies[1] || !apple_smc_thermal_healthy())
		goto fail;
	if (!max_pstate || max_pstate > 17 || !max_estate || max_estate > 6 ||
	    !pt_policy_frequency(policies[0], max_estate) ||
	    !pt_policy_frequency(policies[1], max_pstate)) {
		ret = -EINVAL;
		goto fail;
	}
	for (i = 0; i < 2; i++) {
		if (!pt_policy_frequency(policies[i], 1) || apple_pmp_cpufreq_readback(policies[i]->cpu) != 1)
			goto fail;
		ret = freq_qos_add_request(&policies[i]->constraints, &pt_qos[i],
					  FREQ_QOS_MAX, pt_policy_frequency(policies[i], 1));
		if (ret < 0)
			goto fail;
		added++;
	}
	ret = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_AP_ONLINE_DYN,
			"apple/pmp-thermal:online", NULL, pt_offline);
	if (ret < 0)
		goto fail;
	mutex_lock(&pt_lock);
	for (i = 0; i < 2; i++) {
		pt_policy[i] = policies[i];
		pt_applied[i] = 1;
	}
	/* Only the hotplug veto reads this outside pt_lock. */
	WRITE_ONCE(pt_attached, true);
	mutex_unlock(&pt_lock);
	return 0;
fail:
	while (added)
		freq_qos_remove_request(&pt_qos[--added]);
	for (i = 0; i < 2; i++)
		if (policies[i])
			cpufreq_cpu_put(policies[i]);
	return ret < 0 ? ret : -EINVAL;
}


/* Release the cpufreq boot caps of every cluster without this policy's
 * attachment. -EPROBE_DEFER until both cluster policies exist.
 */
static int pt_release_bootcaps(void)
{
	unsigned int cpu, released = 0;
	int ret, error = 0;

	guard(cpus_read_lock)();
	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;
		if (cpumask_first(policy->related_cpus) == cpu) {
			ret = apple_pmp_thermal_release_bootcap(policy);
			if (ret && ret != -EINVAL) {
				pr_err("apple-pmp-v2: boot cap release failed for CPU%u: %d\n",
				       cpu, ret);
				error = ret;
			} else {
				released++;
			}
		}
		cpufreq_cpu_put(policy);
	}
	if (error)
		return error;
	return released < 2 ? -EPROBE_DEFER : 0;
}

/* The owner's probe failed for good: no policy will ever start. */
static void pt_owner_failed(int error)
{
	if (!startup_fallback) {
		pr_err("apple-pmp-v2: owner probe failed (%d); cpufreq boot caps retained\n", error);
		return;
	}
	pr_err("apple-pmp-v2: owner probe failed (%d); SMC policy is the throttle (startup_fallback)\n",
	       error);
	schedule_delayed_work(&pt_start_work, 0);
}

static void pt_start(struct work_struct *work)
{
	unsigned int i;
	int ret;

	if (READ_ONCE(pt_stopping))
		return;
	if (!pt_owner) {
		/* Fallback only: wait for both cluster policies, then release. */
		ret = pt_release_bootcaps();
		if (ret == -EPROBE_DEFER && ++pt_waits < 120) {
			schedule_delayed_work(&pt_start_work, msecs_to_jiffies(500));
			return;
		}
		if (ret)
			pr_err("apple-pmp-v2: cpufreq boot caps not released: %d\n", ret);
		return;
	}
	ret = pt_attach();
	if (ret == -EPROBE_DEFER && ++pt_waits < 120) {
		schedule_delayed_work(&pt_start_work, msecs_to_jiffies(500));
		return;
	}
	if (ret)
		goto fail;
	ret = pmp_v2_start(pt_owner);
	if (ret || !pt_owner->private_profile) {
		ret = ret ?: -ENODEV;
		goto fail;
	}
	/* A bounded inventory wait, not a firmware READY handshake. Later
	 * provider readiness is handled by normal polling under minimum caps.
	 */
	for (i = 0; i < 100; i++) {
		bool ready;

		mutex_lock(&pt_owner->lock);
		ready = pt_owner->protocol.entry_count >= 358 && !pt_owner->protocol.failed;
		mutex_unlock(&pt_owner->lock);
		if (ready)
			break;
		if (READ_ONCE(pt_stopping)) {
			ret = -ESHUTDOWN;
			goto fail;
		}
		msleep(20);
	}
	if (i == 100) {
		ret = -ETIMEDOUT;
		goto fail;
	}
	ret = apple_smc_thermal_aux_register(&pt_ops, pt_trips, ARRAY_SIZE(pt_trips));
	if (ret)
		goto fail;
	/* Keep the SMC backup; unlike LAB, zone re-enable may recover normally. */
	ret = apple_smc_thermal_aux_set_policy(PT_TRIP_MC, PT_HYST_MC, 105000, 100000);
	if (ret)
		goto fail;
	for (i = 0; i < 2; i++) {
		ret = apple_pmp_thermal_release_bootcap(pt_policy[i]);
		if (ret)
			goto fail;
	}
	mutex_lock(&pt_lock);
	if (pt_stopping) {
		mutex_unlock(&pt_lock);
		return;
	}
	pt_configured = true;
	mutex_unlock(&pt_lock);
	dev_info(pt_owner->dev,
		 "PMP thermal: step_wise, %u ms polling, %d/%d mC, minimum at %d mC, E%u/P%u ceiling\n",
		 PT_POLL_MS, PT_TRIP_MC, PT_TRIP_MC - PT_HYST_MC,
		 PT_MINIMUM_TRIP_MC, max_estate, max_pstate);
	apple_smc_thermal_aux_update();
	return;
fail:
	mutex_lock(&pt_lock);
	pt_failed = true;
	pt_error = ret;
	pt_guard(false);
	if (startup_fallback && !pt_stopping) {
		int released;

		/* Never admitted: release this policy's own constraints and the
		 * cpufreq boot caps; the SMC two-tier policy remains the throttle.
		 */
		for (i = 0; pt_attached && i < 2; i++)
			if (freq_qos_update_request(&pt_qos[i], FREQ_QOS_MAX_DEFAULT_VALUE) >= 0)
				pt_applied[i] = 0;
		mutex_unlock(&pt_lock);
		apple_smc_thermal_aux_unregister();
		released = pt_release_bootcaps();
		dev_err(pt_owner->dev, "PMP thermal startup failed (%d); SMC policy is the throttle (startup_fallback, boot caps %s)\n",
			ret, released ? "NOT released" : "released");
		return;
	}
	mutex_unlock(&pt_lock);
	dev_err(pt_owner->dev, "PMP thermal startup failed (%d); retaining minimum caps\n", ret);
}

static int pt_shutdown(void)
{
	unsigned int i;
	u64 until;
	int ret;

	mutex_lock(&pt_lock);
	pt_stopping = true;
	pt_admitted = false;
	mutex_unlock(&pt_lock);
	cancel_delayed_work_sync(&pt_start_work);
	mutex_lock(&pt_lock);
	ret = pt_guard(false);
	mutex_unlock(&pt_lock);
	if (ret)
		return ret;
	/* A bounded observation of normal cpufreq completion, not a firmware
	 * drain. Native owner mappings remain pinned through platform reset.
	 */
	until = ktime_get_boottime_ns() + 100000000ULL;
	do {
		bool minima = true;

		for (i = 0; i < 2; i++)
			minima &= apple_pmp_cpufreq_readback(pt_policy[i]->cpu) == 1;
		if (minima)
			return 0;
		usleep_range(1000, 2000);
	} while (ktime_get_boottime_ns() < until);
	return -ETIMEDOUT;
}

static int pt_status_show(struct seq_file *s, void *unused)
{
	guard(mutex)(&pt_lock);
	seq_printf(s, "attached=%u configured=%u admitted=%u fault=%d stopping=%u good=%u\n",
		   pt_attached, pt_configured, pt_admitted, pt_failed,
		   pt_stopping, pt_good);
	seq_printf(s, "poll_ms=%u sample_error=%d failures=%u recoveries=%u temperature_mC=%d peak_mC=%d\n",
		   PT_POLL_MS, pt_error, pt_failures, pt_recoveries, pt_temperature, pt_peak);
	seq_printf(s, "e_applied=%u p_applied=%u\n", pt_applied[0], pt_applied[1]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pt_status);

static int pt_init(struct pmp_v2 *pmp)
{
	debugfs_create_file("thermal_status", 0400, pmp->debug, pmp, &pt_status_fops);
	if (!thermal_policy) {
		/* Leave pt_owner NULL: pt_start then takes the fallback branch,
		 * releases the cpufreq boot caps and returns, which leaves the
		 * SMC zone's own two-tier policy as the throttle.
		 */
		pr_info("apple-pmp-v2: PMP thermal policy off (pmp_v2.thermal_policy=0); SMC policy is the throttle\n");
		schedule_delayed_work(&pt_start_work, 0);
		return 0;
	}
	pt_owner = pmp;
	schedule_delayed_work(&pt_start_work, 0);
	return 0;
}
