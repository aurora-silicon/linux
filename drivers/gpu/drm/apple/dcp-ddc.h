/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_DCP_DDC_H__
#define __APPLE_DCP_DDC_H__

#include <linux/types.h>

struct apple_connector;
struct drm_device;
struct i2c_adapter;
struct i2c_msg;

/* One DCPAV I2C transfer (epic/dpavserv-ddc.h). */
struct apple_ddc_xfer {
	bool none;	/* nothing to send: an EDID offset was only set */
	bool read;
	u8 chip;
	u8 offset;
	u8 *buf;
	size_t len;
};

int apple_ddc_plan(const struct i2c_msg *msgs, int num, u8 edid_offset,
		   struct apple_ddc_xfer *xfer);
struct i2c_adapter *apple_ddc_create(struct drm_device *drm,
				     struct apple_connector *connector,
				     const char *port);
void apple_ddc_destroy(struct i2c_adapter *adapter);

#endif
