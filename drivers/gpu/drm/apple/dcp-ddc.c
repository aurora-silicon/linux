// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The DDC bus of an external display, as an I2C adapter of its connector.
 *
 * The DCP owns the DisplayPort AUX channel and the HDMI DDC lines; the host
 * reaches the sink's DDC through the DCPAV service, one transfer at a time
 * (epic/dpavserv-ddc.h).  That service addresses a transfer as chip, offset
 * and data, so the adapter takes the transfers the DDC users make:
 *
 *  - a write to a chip: its first byte is the offset (0x51, the DDC/CI host
 *    address, for a DDC/CI message), the rest the data;
 *  - a read from 0x37 (DDC/CI reply), with the DDC/CI offset 0x51;
 *  - a one-byte write then a read of the same chip (the EDID at 0x50): a read
 *    at that offset.
 *
 * The connector's pipeline is looked up per transfer: a Type-C port changes
 * pipelines, and an unrouted connector has no DDC.
 */
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <drm/drm_connector.h>
#include <drm/drm_device.h>

#include "connector.h"
#include "dcp-ddc.h"
#include "dcp-internal.h"
#include "epic/dpavservep.h"
#include "epic/dpavserv-ddc.h"

#define DDC_CI_ADDR		0x37
#define DDC_CI_HOST_OFFSET	0x51
#define DDC_EDID_ADDR		0x50

struct apple_ddc {
	struct i2c_adapter adapter;
	struct apple_connector *connector;
	/* One transfer at a time: the service answers them in order. */
	struct mutex lock;
};

/*
 * The transfer for @msgs, or -EOPNOTSUPP for a combination the service
 * cannot carry (an address-only probe, a repeated start to another chip,
 * more than two messages, the E-DDC segment pointer).
 */
int apple_ddc_plan(const struct i2c_msg *msgs, int num, struct apple_ddc_xfer *xfer)
{
	const struct i2c_msg *m = &msgs[0];

	if (num < 1 || num > 2 || (m->flags & ~I2C_M_RD))
		return -EOPNOTSUPP;
	if (m->addr != DDC_CI_ADDR && m->addr != DDC_EDID_ADDR)
		return -EOPNOTSUPP;
	if (num == 2) {
		const struct i2c_msg *r = &msgs[1];

		if ((m->flags & I2C_M_RD) || m->len != 1 || r->addr != m->addr ||
		    r->flags != I2C_M_RD || !r->len || r->len > DCPAV_I2C_MAX_DATA)
			return -EOPNOTSUPP;
		*xfer = (struct apple_ddc_xfer) {
			.read = true, .chip = m->addr, .offset = m->buf[0],
			.buf = r->buf, .len = r->len,
		};
		return 0;
	}
	if (m->flags & I2C_M_RD) {
		/* A bare read has no offset of its own: only a DDC/CI reply has one. */
		if (m->addr != DDC_CI_ADDR || !m->len || m->len > DCPAV_I2C_MAX_DATA)
			return -EOPNOTSUPP;
		*xfer = (struct apple_ddc_xfer) {
			.read = true, .chip = m->addr, .offset = DDC_CI_HOST_OFFSET,
			.buf = m->buf, .len = m->len,
		};
		return 0;
	}
	if (!m->len || m->len - 1 > DCPAV_I2C_MAX_DATA)
		return -EOPNOTSUPP;
	*xfer = (struct apple_ddc_xfer) {
		.read = false, .chip = m->addr, .offset = m->buf[0],
		.buf = m->buf + 1, .len = m->len - 1,
	};
	return 0;
}

static int apple_ddc_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num)
{
	struct apple_ddc *ddc = i2c_get_adapdata(adapter);
	struct platform_device *pdev;
	struct apple_ddc_xfer xfer;
	int ret;

	ret = apple_ddc_plan(msgs, num, &xfer);
	if (ret)
		return ret;
	guard(mutex)(&ddc->lock);
	pdev = READ_ONCE(ddc->connector->dcp);
	if (!pdev)
		return -ENODEV;
	ret = dcpavserv_i2c(platform_get_drvdata(pdev), xfer.read, xfer.chip,
			    xfer.offset, xfer.buf, xfer.len);
	return ret ?: num;
}

static u32 apple_ddc_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm apple_ddc_algorithm = {
	.xfer = apple_ddc_xfer,
	.functionality = apple_ddc_functionality,
};

/* Before drm_connector_init_with_ddc(): DRM links the adapter from sysfs. */
struct i2c_adapter *apple_ddc_create(struct drm_device *drm,
				     struct apple_connector *connector,
				     const char *port)
{
	struct apple_ddc *ddc = kzalloc_obj(*ddc);
	int ret;

	if (!ddc)
		return ERR_PTR(-ENOMEM);
	ddc->connector = connector;
	mutex_init(&ddc->lock);
	ddc->adapter.owner = THIS_MODULE;
	ddc->adapter.algo = &apple_ddc_algorithm;
	ddc->adapter.dev.parent = drm->dev;
	/* A DCPAV call that the sink does not answer takes about a second. */
	ddc->adapter.timeout = 2 * HZ;
	ddc->adapter.retries = 0;
	snprintf(ddc->adapter.name, sizeof(ddc->adapter.name), "apple-dcp DDC %s", port);
	i2c_set_adapdata(&ddc->adapter, ddc);
	ret = i2c_add_adapter(&ddc->adapter);
	if (ret) {
		mutex_destroy(&ddc->lock);
		kfree(ddc);
		return ERR_PTR(ret);
	}
	return &ddc->adapter;
}

/* After drm_connector_cleanup(): i2c_del_adapter() waits for its users. */
void apple_ddc_destroy(struct i2c_adapter *adapter)
{
	struct apple_ddc *ddc;

	if (!adapter)
		return;
	ddc = container_of(adapter, struct apple_ddc, adapter);
	i2c_del_adapter(adapter);
	mutex_destroy(&ddc->lock);
	kfree(ddc);
}
