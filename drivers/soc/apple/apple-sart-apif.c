// SPDX-License-Identifier: GPL-2.0-only
/* SART policy stays in Linux; native table-5 calls use the common APIF client. */
#include <linux/dma-direct.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/aurora/apif-mmio.h>

#include "sart-backend.h"

#define SART_APIF_MAX_REGIONS 16

struct sart_apif {
	struct device *dev;
	struct aurora_apif *apif;
	struct mutex lock;
	bool active;
	struct {
		phys_addr_t address;
		size_t size;
	} regions[SART_APIF_MAX_REGIONS];
};

static int sart_apif_region(struct apple_sart *sart, phys_addr_t address,
			    size_t size, bool map)
{
	struct sart_apif *state = apple_sart_backend_data(sart);
	struct aurora_apif_op op;
	u64 pa;
	int i, ret;

	if (!size || ((address | size) & (SZ_16K - 1)) ||
	    address > PHYS_ADDR_MAX - (size - 1))
		return -EINVAL;
	mutex_lock(&state->lock);
	for (i = 0; i < SART_APIF_MAX_REGIONS; i++)
		if (map ? !state->regions[i].size :
			  state->regions[i].address == address &&
				    state->regions[i].size == size)
			break;
	if (i == SART_APIF_MAX_REGIONS) {
		ret = map ? -EBUSY : -ENOENT;
		goto out;
	}
	op = (struct aurora_apif_op){
		.selector = AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE,
						 AURORA_APIF_TRANSLATE),
		.arg = { dma_to_phys(state->dev, address), size },
	};
	ret = aurora_apif_submit(state->apif, &op, 1, NULL);
	if (ret)
		goto out;
	pa = op.ret;
	if (!state->active) {
		op = (struct aurora_apif_op){
			.selector = AURORA_APIF_SELECTOR(
				5, 0), /* SET_STATE, void result */
			.arg = { 1 },
		};
		ret = aurora_apif_submit(state->apif, &op, 1, NULL);
		if (ret)
			goto out;
		state->active = true;
	}
	op = (struct aurora_apif_op){
		.selector = AURORA_APIF_SELECTOR(5, map ? 1 : 2),
		.arg = { pa, size, map, map },
	};
	ret = aurora_apif_submit(state->apif, &op, 1, NULL);
	/* MAP is void; UNMAP returns status. A refused unmap keeps the region
	 * live so the consumer cannot treat its DMA backing as released.
	 */
	if (!ret && !map && op.ret)
		ret = -EIO;
	if (!ret) {
		state->regions[i].address = map ? address : 0;
		state->regions[i].size = map ? size : 0;
	}
out:
	mutex_unlock(&state->lock);
	return ret;
}

static int sart_apif_add(struct apple_sart *sart, phys_addr_t address,
			 size_t size)
{
	return sart_apif_region(sart, address, size, true);
}

static int sart_apif_remove(struct apple_sart *sart, phys_addr_t address,
			    size_t size)
{
	return sart_apif_region(sart, address, size, false);
}

static const struct apple_sart_backend sart_apif_backend = {
	.add = sart_apif_add,
	.remove = sart_apif_remove,
};

static void sart_apif_put(void *data)
{
	aurora_apif_put(data);
}

static int sart_apif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sart_apif *state;
	struct apple_sart *sart;
	int ret;

	state = devm_kzalloc(dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	state->dev = dev;
	mutex_init(&state->lock);
	state->apif = aurora_apif_get(dev);
	if (IS_ERR(state->apif))
		return dev_err_probe(dev, PTR_ERR(state->apif),
				     "APIF transport unavailable\n");
	ret = devm_add_action_or_reset(dev, sart_apif_put, state->apif);
	if (ret)
		return ret;
	sart = apple_sart_create_backend(dev, &sart_apif_backend, state);
	if (IS_ERR(sart))
		return PTR_ERR(sart);
	platform_set_drvdata(pdev, sart);
	return 0;
}

static void sart_apif_shutdown(struct platform_device *pdev)
{
	struct apple_sart *sart = platform_get_drvdata(pdev);
	struct sart_apif *state = apple_sart_backend_data(sart);

	/* Supplier ordering stops consumers before their Linux-owned grants. */
	for (unsigned int i = 0; i < SART_APIF_MAX_REGIONS; i++)
		if (state->regions[i].size &&
		    sart_apif_remove(sart, state->regions[i].address,
				     state->regions[i].size))
			dev_err(state->dev,
				"failed to release DMA region at shutdown\n");
}

static const struct of_device_id sart_apif_of_match[] = {
	{ .compatible = "apple,sart-apif" },
	{}
};
MODULE_DEVICE_TABLE(of, sart_apif_of_match);

static struct platform_driver sart_apif_driver = {
	.driver = {
		.name = "apple-sart-apif",
		.of_match_table = sart_apif_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = sart_apif_probe,
	.shutdown = sart_apif_shutdown,
};
module_platform_driver(sart_apif_driver);

MODULE_DESCRIPTION("Apple SART over the Aurora platform interface");
MODULE_LICENSE("GPL");
