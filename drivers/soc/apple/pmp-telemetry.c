// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Read-only sensors supplied by the existing PMP firmware owner. */
#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/rculist.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/soc/apple/pmp-telemetry.h>
#include <linux/soc/apple/pmp-temps.h>
#include <linux/soc/apple/pmp-power-policy.h>
#include <linux/soc/apple/t8132.h>

bool apple_pmp_telemetry_retained_board(void)
{
	return apple_t8132_firmware_path();
}

static LIST_HEAD(apple_pmp_telemetry_list);
static DEFINE_MUTEX(apple_pmp_telemetry_lock);
static atomic64_t apple_pmp_telemetry_generation = ATOMIC64_INIT(0);

struct apple_pmp_telemetry {
	struct device *dev;
	struct list_head list;
	void __iomem *sram;
	void __iomem *asc;
	bool online;
	spinlock_t gpu_lock;
	struct apple_pmp_gpu_lease gpu;
	u64 generation;
	size_t size;
	/* Where the running firmware keeps each temperature record. */
	struct apple_pmp_temp_map temps;
	struct delayed_work locate;
	unsigned int locate_tries;
};

/*
 * The firmware's RTKit header names the start of its data segment, where
 * its heap and so the temperature records are: 0x3c000 in the 26.6.2
 * (25G83) PMP firmware, 0x3d000 in 27.2 (26B123).
 */
#define PMP_TEMP_DATA_START_HDR	0x234
#define PMP_TEMP_LOCATE_MS	1000
#define PMP_TEMP_LOCATE_TRIES	60

static bool apple_pmp_temps_at(struct apple_pmp_telemetry *sensors,
			       const struct apple_pmp_temp_map *map)
{
	u8 record[PMP_TEMP_RECORD_SIZE];
	unsigned int i;

	for (i = 0; i < PMP_TEMP_CHANNELS; i++) {
		memcpy_fromio(record, sensors->sram + map->offset[i], sizeof(record));
		if (!apple_pmp_temp_record_plausible(record, apple_pmp_temp_records[i].name))
			return false;
	}
	return true;
}

/* The firmware fills its records some time after it starts: look until they are there. */
static void apple_pmp_temps_locate(struct work_struct *work)
{
	struct apple_pmp_telemetry *sensors =
		container_of(to_delayed_work(work), struct apple_pmp_telemetry, locate);
	struct apple_pmp_temp_map map;
	size_t start = 0;
	u32 data;
	unsigned int i;
	int ret;

	if (!READ_ONCE(sensors->online))
		return;
	apple_pmp_temp_map_default(&map);
	if (apple_pmp_temps_at(sensors, &map))
		return;
	data = readl_relaxed(sensors->sram + PMP_TEMP_DATA_START_HDR);
	if (!(data & 3) && data < sensors->size)
		start = data;
	ret = apple_pmp_temp_locate(sensors->sram, start, sensors->size, &map);
	if (!ret) {
		for (i = 0; i < PMP_TEMP_CHANNELS; i++)
			WRITE_ONCE(sensors->temps.offset[i], map.offset[i]);
		dev_info(sensors->dev,
			 "temperature records at %#x %#x %#x %#x %#x %#x (data from %#zx)\n",
			 map.offset[0], map.offset[1], map.offset[2], map.offset[3],
			 map.offset[4], map.offset[5], start);
		return;
	}
	if (++sensors->locate_tries < PMP_TEMP_LOCATE_TRIES) {
		schedule_delayed_work(&sensors->locate, msecs_to_jiffies(PMP_TEMP_LOCATE_MS));
		return;
	}
	dev_warn(sensors->dev, "temperature records not found (%d, data from %#zx)\n",
		 ret, start);
}

static bool apple_pmp_telemetry_online(const struct apple_pmp_telemetry *sensors)
{
	return READ_ONCE(sensors->online) &&
	       (readl(sensors->asc + 0x44) & BIT(4));
}

int apple_pmp_telemetry_get_hotspot(const struct device_node *np, long *value)
{
	struct apple_pmp_telemetry *sensors;
	int ret = -EPROBE_DEFER;

	if (!np || !value)
		return -EINVAL;
	rcu_read_lock();
	list_for_each_entry_rcu(sensors, &apple_pmp_telemetry_list, list) {
		if (sensors->dev->of_node != np)
			continue;
		if (!apple_pmp_telemetry_online(sensors)) {
			ret = -ENODEV;
			break;
		}
		ret = apple_pmp_temp_hotspot_map(sensors->sram, &sensors->temps, value);
		/* A crash concurrent with the snapshot invalidates the result. */
		if (!apple_pmp_telemetry_online(sensors))
			ret = -ENODEV;
		break;
	}
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmp_telemetry_get_hotspot);

struct apple_pmp_gpu {
	struct device_node *node;
	u64 generation;
	u64 epoch;
};

struct apple_pmp_gpu *apple_pmp_gpu_get(struct device *dev)
{
	struct apple_pmp_telemetry *sensors;
	struct apple_pmp_gpu *gpu;
	int ret = -EPROBE_DEFER;

	gpu = kzalloc_obj(*gpu);
	if (!gpu)
		return ERR_PTR(-ENOMEM);
	gpu->node = of_parse_phandle(dev->of_node, "apple,pmp", 0);
	if (!gpu->node) {
		ret = -EINVAL;
		goto fail;
	}
	rcu_read_lock();
	list_for_each_entry_rcu(sensors, &apple_pmp_telemetry_list, list) {
		if (sensors->dev->of_node != gpu->node || !READ_ONCE(sensors->online))
			continue;
		spin_lock(&sensors->gpu_lock);
		gpu->epoch = apple_pmp_gpu_lease_attach(&sensors->gpu);
		spin_unlock(&sensors->gpu_lock);
		if (!gpu->epoch) {
			ret = -EBUSY;
			break;
		}
		gpu->generation = sensors->generation;
		ret = 0;
		break;
	}
	rcu_read_unlock();
	if (!ret)
		return gpu;
fail:
	of_node_put(gpu->node);
	kfree(gpu);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(apple_pmp_gpu_get);

void apple_pmp_gpu_put(struct apple_pmp_gpu *gpu)
{
	struct apple_pmp_telemetry *sensors;

	rcu_read_lock();
	list_for_each_entry_rcu(sensors, &apple_pmp_telemetry_list, list) {
		if (sensors->dev->of_node != gpu->node ||
		    sensors->generation != gpu->generation)
			continue;
		spin_lock(&sensors->gpu_lock);
		apple_pmp_gpu_lease_detach(&sensors->gpu, gpu->epoch);
		spin_unlock(&sensors->gpu_lock);
		break;
	}
	rcu_read_unlock();
	of_node_put(gpu->node);
	kfree(gpu);
}
EXPORT_SYMBOL_GPL(apple_pmp_gpu_put);

unsigned int apple_pmp_gpu_power_state(struct apple_pmp_gpu *gpu)
{
	struct apple_pmp_telemetry *sensors;
	unsigned int level = 8;

	rcu_read_lock();
	list_for_each_entry_rcu(sensors, &apple_pmp_telemetry_list, list) {
		if (sensors->dev->of_node != gpu->node ||
		    sensors->generation != gpu->generation || !READ_ONCE(sensors->online))
			continue;
		if (readl(sensors->asc + 0x44) & BIT(4)) {
			spin_lock(&sensors->gpu_lock);
			level = apple_pmp_gpu_lease_level(&sensors->gpu, gpu->epoch, ktime_get_ns());
			spin_unlock(&sensors->gpu_lock);
		}
		break;
	}
	rcu_read_unlock();
	return level;
}
EXPORT_SYMBOL_GPL(apple_pmp_gpu_power_state);

u64 apple_pmp_gpu_epoch(const struct apple_pmp_sensor_data *data)
{
	struct apple_pmp_telemetry *sensors = data->cookie;
	u64 epoch;

	spin_lock(&sensors->gpu_lock);
	epoch = sensors->gpu.attached ? sensors->gpu.epoch : 0;
	spin_unlock(&sensors->gpu_lock);
	return epoch;
}
EXPORT_SYMBOL_GPL(apple_pmp_gpu_epoch);

void apple_pmp_gpu_publish(const struct apple_pmp_sensor_data *data,
			   u64 epoch, unsigned int level, u64 sample_ns)
{
	struct apple_pmp_telemetry *sensors = data->cookie;

	spin_lock(&sensors->gpu_lock);
	apple_pmp_gpu_lease_publish(&sensors->gpu, epoch, level, sample_ns);
	spin_unlock(&sensors->gpu_lock);
}
EXPORT_SYMBOL_GPL(apple_pmp_gpu_publish);

static int apple_pmp_sensor_read(void *cookie, unsigned int channel,
				 long *value)
{
	struct apple_pmp_telemetry *sensors = cookie;
	int ret;

	if (channel >= PMP_TEMP_CHANNELS)
		return -EINVAL;
	if (!apple_pmp_telemetry_online(sensors))
		return -ENODEV;
	ret = apple_pmp_temp_sample_map(sensors->sram, &sensors->temps, channel, value);

	return apple_pmp_telemetry_online(sensors) ? ret : -ENODEV;
}

static umode_t apple_pmp_hwmon_visible(const void *data,
				       enum hwmon_sensor_types type, u32 attr,
				       int channel)
{
	return type == hwmon_temp && channel >= 0 &&
			       channel < PMP_TEMP_CHANNELS &&
			       (attr == hwmon_temp_input ||
				attr == hwmon_temp_label) ?
		       0444 :
		       0;
}

static int apple_pmp_hwmon_read(struct device *dev,
				enum hwmon_sensor_types type, u32 attr,
				int channel, long *value)
{
	struct apple_pmp_telemetry *sensors = dev_get_drvdata(dev);

	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	if (channel < 0 || channel >= PMP_TEMP_CHANNELS)
		return -EINVAL;
	/* Preserve the original ABI: aggregate first, then five named records.
	 * The sixth firmware record still contributes to the aggregate.
	 */
	if (channel)
		return apple_pmp_sensor_read(sensors, channel - 1, value);
	return apple_pmp_telemetry_get_hotspot(sensors->dev->of_node, value);
}

static int apple_pmp_hwmon_label(struct device *dev,
				 enum hwmon_sensor_types type, u32 attr,
				 int channel, const char **label)
{
	if (type != hwmon_temp || attr != hwmon_temp_label || channel < 0 ||
	    channel >= PMP_TEMP_CHANNELS)
		return -EOPNOTSUPP;
	*label = channel ? apple_pmp_temp_records[channel - 1].label :
			   "PMP aggregate die hotspot";
	return 0;
}

static const struct hwmon_ops apple_pmp_hwmon_ops = {
	.is_visible = apple_pmp_hwmon_visible,
	.read = apple_pmp_hwmon_read,
	.read_string = apple_pmp_hwmon_label,
};

static const struct hwmon_channel_info *const apple_pmp_hwmon_info[] = {
	HWMON_CHANNEL_INFO(
		temp, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL),
	NULL,
};

static const struct hwmon_chip_info apple_pmp_hwmon_chip = {
	.ops = &apple_pmp_hwmon_ops,
	.info = apple_pmp_hwmon_info,
};

static void apple_pmp_thermal_unregister(void *data)
{
	platform_device_unregister(data);
}

static void apple_pmp_telemetry_release(void *data)
{
	struct apple_pmp_telemetry *sensors = data;

	cancel_delayed_work_sync(&sensors->locate);
	mutex_lock(&apple_pmp_telemetry_lock);
	if (!list_empty(&sensors->list))
		list_del_rcu(&sensors->list);
	mutex_unlock(&apple_pmp_telemetry_lock);
	synchronize_rcu();
}

int apple_pmp_telemetry_register(struct device *dev, void __iomem *sram,
				 size_t size, void __iomem *asc)
{
	struct apple_pmp_sensor_data data;
	struct apple_pmp_telemetry *sensors;
	struct platform_device *child;
	struct device *hwmon;
	int ret;

	if (!of_device_is_compatible(dev->of_node, "apple,t8132-pmp-v2"))
		return 0;
	if (size < apple_pmp_temp_records[PMP_TEMP_CHANNELS - 1].offset +
			   PMP_TEMP_RECORD_SIZE)
		return -EINVAL;
	sensors = devm_kzalloc(dev, sizeof(*sensors), GFP_KERNEL);
	if (!sensors)
		return -ENOMEM;
	sensors->dev = dev;
	sensors->size = size;
	apple_pmp_temp_map_default(&sensors->temps);
	INIT_DELAYED_WORK(&sensors->locate, apple_pmp_temps_locate);
	INIT_LIST_HEAD(&sensors->list);
	ret = devm_add_action_or_reset(dev, apple_pmp_telemetry_release,
				       sensors);
	if (ret)
		return ret;
	spin_lock_init(&sensors->gpu_lock);
	sensors->sram = sram;
	sensors->asc = asc;
	sensors->online = true;
	sensors->generation = atomic64_inc_return(&apple_pmp_telemetry_generation);
	mutex_lock(&apple_pmp_telemetry_lock);
	list_add_tail_rcu(&sensors->list, &apple_pmp_telemetry_list);
	mutex_unlock(&apple_pmp_telemetry_lock);
	schedule_delayed_work(&sensors->locate, 0);
	if (IS_REACHABLE(CONFIG_HWMON)) {
		hwmon = devm_hwmon_device_register_with_info(
			dev, "apple_pmp", sensors, &apple_pmp_hwmon_chip, NULL);
		if (IS_ERR(hwmon))
			return PTR_ERR(hwmon);
	}
	data.read = apple_pmp_sensor_read;
	data.cookie = sensors;
	data.dev = dev;
	child = platform_device_register_data(dev, "apple-pmp-thermal",
					      PLATFORM_DEVID_AUTO, &data,
					      sizeof(data));
	if (IS_ERR(child))
		return PTR_ERR(child);
	ret = devm_add_action_or_reset(dev, apple_pmp_thermal_unregister,
				       child);
	if (ret)
		return ret;
	return 0;
}

void apple_pmp_telemetry_stop(struct device *dev)
{
	struct apple_pmp_telemetry *sensors;

	rcu_read_lock();
	list_for_each_entry_rcu(sensors, &apple_pmp_telemetry_list, list)
		if (sensors->dev == dev) {
			spin_lock(&sensors->gpu_lock);
			WRITE_ONCE(sensors->online, false);
			apple_pmp_gpu_lease_publish(&sensors->gpu, 0, 8, 0);
			spin_unlock(&sensors->gpu_lock);
		}
	rcu_read_unlock();
}
