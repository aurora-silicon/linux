/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Link between the Apple ISP and the platform neural engine (ANE).
 *
 * The ISP runs its ML video noise reduction on the ANE. The ANE driver
 * loads the network package (ispane.afpp) into the ANE and both firmwares
 * talk over a pair of rings the ISP driver allocates. This header is the
 * in-kernel interface between the two drivers.
 */
#ifndef _LINUX_SOC_APPLE_ANE_ISP_H
#define _LINUX_SOC_APPLE_ANE_ISP_H

#include <linux/err.h>
#include <linux/types.h>

struct device;
struct device_node;
struct sg_table;
struct ane_isp_link;

/**
 * struct ane_isp_endpoint - one ISP<->ANE endpoint
 * @rings: the read and write ring, @ring_len bytes each, read ring first.
 *	   Allocated and mapped by the ISP driver; the ANE driver maps the
 *	   same pages into the ANE DART.
 * @ring_len: bytes per ring (0x4000 on H16)
 * @isp_doorbell: in, how the ANE firmware signals the ISP firmware
 * @isp_doorbell_sel: in, selector for @isp_doorbell
 * @ane_doorbell: out, how the ISP firmware signals the ANE firmware
 * @ane_doorbell_sel: out, selector for @ane_doorbell
 */
struct ane_isp_endpoint {
	struct sg_table *rings;
	u32 ring_len;
	u64 isp_doorbell;
	u32 isp_doorbell_sel;
	u64 ane_doorbell;
	u32 ane_doorbell_sel;
};

#if IS_REACHABLE(CONFIG_DRM_ACCEL_ANE)
struct ane_isp_link *ane_isp_link_get(struct device *isp_dev, struct device_node *ane_np);
void ane_isp_link_put(struct ane_isp_link *link);
int ane_isp_endpoint_enable(struct ane_isp_link *link, const char *afpp,
			    struct ane_isp_endpoint *ep);
void ane_isp_endpoint_disable(struct ane_isp_link *link);
#else
static inline struct ane_isp_link *ane_isp_link_get(struct device *isp_dev,
						    struct device_node *ane_np)
{
	return ERR_PTR(-ENODEV);
}
static inline void ane_isp_link_put(struct ane_isp_link *link) {}
static inline int ane_isp_endpoint_enable(struct ane_isp_link *link, const char *afpp,
					  struct ane_isp_endpoint *ep)
{
	return -ENODEV;
}
static inline void ane_isp_endpoint_disable(struct ane_isp_link *link) {}
#endif

#endif
