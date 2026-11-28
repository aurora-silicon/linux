// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * G18: the CLPC's GPU input. While the GPU has work, the CLPC holds the
 * memory controller (DCS) and fabric floor at the top of its map, as the G17
 * backend reports its work. The T8152 CLPC otherwise raises that floor only
 * for busy CPU clusters. Each job the driver waits for holds the request from
 * its start until it ends, however long it runs; the request is dropped 16 ms
 * after the last job ended.
 */

#include <linux/bug.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/soc/apple/clpc.h>
#include <linux/workqueue.h>

#define G18_DEMAND_IDLE_MS 16

struct g18_demand {
	struct apple_clpc_gpu *gpu;
	struct delayed_work idle;
	/* Orders the request against the idle work, which drops it only with no job. */
	struct mutex lock;
	unsigned int jobs;
};

int g18_demand_get(struct device *dev, struct g18_demand **out);
void g18_demand_begin(struct g18_demand *demand);
void g18_demand_end(struct g18_demand *demand);
void g18_demand_put(struct g18_demand *demand);

static void g18_demand_idle(struct work_struct *work)
{
	struct g18_demand *demand =
		container_of(to_delayed_work(work), struct g18_demand, idle);

	mutex_lock(&demand->lock);
	if (!demand->jobs)
		apple_clpc_gpu_set_active(demand->gpu, false);
	mutex_unlock(&demand->lock);
}

/*
 * *out is NULL without an apple,clpc provider, or when the provider refuses the
 * GPU input: the GPU then runs without the floor. -EPROBE_DEFER while the
 * provider has not probed yet, and -ENOMEM, are returned for the probe to act
 * on, before it touches the GPU.
 */
int g18_demand_get(struct device *dev, struct g18_demand **out)
{
	struct apple_clpc_gpu *gpu = NULL;
	struct g18_demand *demand;
	int ret = apple_clpc_gpu_get(dev, &gpu);

	*out = NULL;
	if (ret == -EPROBE_DEFER || ret == -ENOMEM)
		return ret;
	if (ret || !gpu) {
		if (ret)
			dev_warn(dev, "G18: no CLPC GPU input: %d\n", ret);
		return 0;
	}
	demand = kzalloc(sizeof(*demand), GFP_KERNEL);
	if (!demand) {
		apple_clpc_gpu_put(gpu);
		return -ENOMEM;
	}
	demand->gpu = gpu;
	INIT_DELAYED_WORK(&demand->idle, g18_demand_idle);
	mutex_init(&demand->lock);
	*out = demand;
	return 0;
}

/* A job started: hold the floor until it ends. */
void g18_demand_begin(struct g18_demand *demand)
{
	if (!demand)
		return;
	mutex_lock(&demand->lock);
	demand->jobs++;
	apple_clpc_gpu_set_active(demand->gpu, true);
	mutex_unlock(&demand->lock);
}

/* A job ended: drop the floor 16 ms after the last one, unless another starts. */
void g18_demand_end(struct g18_demand *demand)
{
	if (!demand)
		return;
	mutex_lock(&demand->lock);
	if (!WARN_ON_ONCE(!demand->jobs))
		demand->jobs--;
	mutex_unlock(&demand->lock);
	mod_delayed_work(system_wq, &demand->idle,
			 msecs_to_jiffies(G18_DEMAND_IDLE_MS));
}

/* With no job left: stop the idle work, drop the request and the CLPC reference. */
void g18_demand_put(struct g18_demand *demand)
{
	if (!demand)
		return;
	cancel_delayed_work_sync(&demand->idle);
	apple_clpc_gpu_put(demand->gpu);
	mutex_destroy(&demand->lock);
	kfree(demand);
}
