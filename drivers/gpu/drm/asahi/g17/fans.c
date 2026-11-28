// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The M5 thermal governor's sensor and fan calls (thermal.rs). The thermal
 * core and the SMC hwmon cell have stubs where they are not built; Rust
 * reaches them through these. All macsmc-hwmon device tree binding knowledge
 * stays in drivers/hwmon/macsmc-hwmon.c.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mfd/macsmc.h>
#include <linux/thermal.h>
#include "fans.h"

int asahi_zone_temp(const char *name, int *temp)
{
	struct thermal_zone_device *zone = thermal_zone_get_zone_by_name(name);

	if (IS_ERR_OR_NULL(zone))
		return zone ? PTR_ERR(zone) : -ENODEV;
	return thermal_zone_get_temp(zone, temp);
}

static int asahi_zone_passive_trip(struct thermal_trip *trip, void *data)
{
	int *temp = data;

	if (trip->type == THERMAL_TRIP_PASSIVE &&
	    (*temp == THERMAL_TEMP_INVALID || trip->temperature < *temp))
		*temp = trip->temperature;
	return 0;
}

/*
 * The lowest passive trip of the thermal zone @name: for the SoC die zone,
 * the die temperature from which the machine sheds performance (from its ADT).
 * -ENODATA when the zone has none.
 */
int asahi_zone_passive(const char *name, int *temp)
{
	struct thermal_zone_device *zone;
	int passive = THERMAL_TEMP_INVALID;

	/* The trip walk has no stub without the thermal core. */
	if (!IS_ENABLED(CONFIG_THERMAL))
		return -ENODEV;
	zone = thermal_zone_get_zone_by_name(name);
	if (IS_ERR_OR_NULL(zone))
		return zone ? PTR_ERR(zone) : -ENODEV;
	thermal_zone_for_each_trip(zone, asahi_zone_passive_trip, &passive);
	if (passive == THERMAL_TEMP_INVALID)
		return -ENODATA;
	*temp = passive;
	return 0;
}

int asahi_fans_request(struct device *dev, unsigned int permille, unsigned int min_step_rpm)
{
	return macsmc_hwmon_fans_request(dev, permille, min_step_rpm);
}

bool asahi_fans_hotspot_described(void)
{
	return macsmc_hwmon_hotspot_assist_available();
}

int asahi_fans_hotspot_policy(struct device *dev, int *trigger_mc, int *release_mc,
			      unsigned int *hold_ms)
{
	struct macsmc_hwmon_fan_assist policy;
	int ret;

	ret = macsmc_hwmon_hotspot_assist_policy(dev, &policy);
	if (ret)
		return ret;
	*trigger_mc = policy.trigger_mc;
	*release_mc = policy.release_mc;
	*hold_ms = policy.hold_ms;
	return 0;
}

/*
 * A kernel fan request for hotspot fan assistance on the bound provider @dev:
 * refused (-EPERM) unless the provider exposes a valid DT-configured hotspot
 * assist policy, and only drives fans marked with apple,fan-manual-control.
 * Handing the fans back (@permille 0) is always allowed.
 */
int asahi_fans_request_described(struct device *dev, unsigned int permille,
				 unsigned int min_step_rpm)
{
	if (permille && macsmc_hwmon_hotspot_assist_policy(dev, NULL))
		return -EPERM;
	return macsmc_hwmon_fans_request_manual(dev, permille, min_step_rpm);
}
