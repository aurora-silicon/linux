/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Link between the Apple ISP and the platform neural engine (ANE).
 *
 * The ISP runs its ML video noise reduction (MLVNR) on the ANE. The ANE
 * driver loads the network package (ispane.afpp) into the ANE and both
 * firmwares talk over a pair of rings the ISP driver allocates. This header
 * is the in-kernel interface between the two drivers.
 *
 * Based on Ryan Murray's draft for H16 (camera/isp-ane-mlvnr). The functions
 * are always declared so that the ISP driver can take them with
 * symbol_get() and keep working when the ANE driver is not loaded.
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
 * @rings: the ISP's read ring then its write ring, @ring_len bytes each.
 *	   Allocated and mapped by the ISP driver; the ANE driver maps the
 *	   same pages into the ANE DART while the endpoint is enabled.
 * @ring_len: bytes per ring (0x4000 on H16 and H17)
 * @isp_doorbell: in, how the ANE firmware signals the ISP firmware (a PA
 *	   on H17: the ISP CPU software interrupt register)
 * @isp_doorbell_sel: in, the bitmask for @isp_doorbell
 * @ane_doorbell: out, how the ISP firmware signals the ANE firmware (a PA)
 * @ane_doorbell_sel: out, selector for @ane_doorbell
 * @surface_get: in, optional: the ISP firmware's shared surface @id, the ID
 *	   the ISP driver gave in its SHAREDMALLOC reply (macOS: the IOSurface
 *	   ID). The ANE firmware asks for such surfaces by ID and the ANE driver
 *	   maps them into the ANE DART. Takes a reference on the surface's pages
 *	   until @surface_put. Both are leaf calls: they take no lock of the ANE
 *	   driver and may be called with its locks held.
 * @surface_put: in, drops what @surface_get took (@s->cookie)
 * @surface_ctx: in, for both
 * @surface_map: out, true when the ANE driver serves such requests: only
 *	   then may the ISP driver hand nonzero IDs to its firmware
 */
struct ane_isp_surface {
	struct sg_table *sgt;
	u64 size;
	u64 iova;
	u32 tag;
	void *cookie;
};

struct ane_isp_endpoint {
	struct sg_table *rings;
	u32 ring_len;
	u64 isp_doorbell;
	u32 isp_doorbell_sel;
	u64 ane_doorbell;
	u32 ane_doorbell_sel;
	int (*surface_get)(void *ctx, u32 id, struct ane_isp_surface *s);
	void (*surface_put)(void *ctx, void *cookie);
	void *surface_ctx;
	bool surface_map;
};

/*
 * ane_isp_link_get: the one ANE (@ane_np may be NULL), or an ERR_PTR:
 * -EPROBE_DEFER until its node exists, -EBUSY while another link is held.
 * ane_isp_endpoint_enable: load the network and set the endpoint up; the
 * ANE stays powered until ane_isp_endpoint_disable(). Never blocks on a
 * system sleep: -EAGAIN then. Both may sleep (process context only).
 */
struct ane_isp_link *ane_isp_link_get(struct device *isp_dev, struct device_node *ane_np);
void ane_isp_link_put(struct ane_isp_link *link);
int ane_isp_endpoint_enable(struct ane_isp_link *link, const char *afpp,
			    struct ane_isp_endpoint *ep);
void ane_isp_endpoint_disable(struct ane_isp_link *link);

#endif
