// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

/*
 * J514S external display controllers (DCPEXT, DCP firmware 14.7.0 in its
 * external role) on the common driver.
 *
 * A controller node is published by the boot loader once it has reserved the
 * controller's firmware segments (memory-region, with their DART addresses)
 * and named the running image (apple,firmware-compat and -uuid). The node
 * links one Type-C route per USB-C connector it can drive; the Type-C port
 * controller enters the DisplayPort alternate mode and reports it through
 * those routes.
 *
 * The lifecycle, all of it driven by the Type-C routes and KMS:
 *
 * - The first DP state of a port activates its route: the PHY's DP hold, then
 *   the crossbar connection at rate 0 (route_select()).
 * - HPD high starts the controller on first use, route first:
 *   RUN (none if the boot loader left the coprocessor running, which happens
 *   with a sink attached at power-on: then only its RTKit communication
 *   restarts), RTKit, IOMFB READY and the external-role check A410 = 0, the
 *   DPTX and AV services; then CONNECT, REQUEST_DISPLAY and HPD high, and after
 *   the first HPD reply the client open A401/A455.  The firmware trains the
 *   link through the DPTX callbacks and publishes the sink's catalog; the
 *   connector reports the sink once both are there.
 * - KMS sets the mode with A411 and powers the display with A472; presents
 *   reuse the panel's single-surface encoder and IOMFB queue.
 * - HPD low releases the sink (HPD low, RELEASE_DISPLAY); the firmware stops
 *   the link in callback 1.  Leaving DP mode deactivates the route: crossbar
 *   idle, then the PHY hold is dropped.
 *
 * The session is never stopped once RUN: unplugging keeps it, and a protocol
 * failure latches this controller only (iomfb_v14_7_fail()).
 *
 * The built-in HDMI port (DCPEXT0) has one fixed route instead: its own DP
 * PHY (ATC3, four lanes up to HBR3), the crossbar's selector 0 and the HDMI
 * converter, whose HPD line drives the same lifecycle (debounced HPD high:
 * route up, then the sink connection; low: the release, then the route
 * down). The firmware needs the converter service (DPTX group 8 command 9)
 * every 100 ms while it holds the sink.
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mux/consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/apple/dp-tunnel.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <drm/drm_edid.h>

#include "afk.h"
#include "av.h"
#include "dcp.h"
#include "dcp-internal.h"
#include "dcpext-t6030.h"
#include "dptx-service.h"
#include "parser.h"

/* The 14.7.0 image the J514S boot loader runs on every display coprocessor. */
#define J514S_DCPEXT_UUID	"DDF38191-93B3-324A-BC8F-643006F5AC82"

/*
 * Bounds of the firmware exchanges, none of them a delay: RTKit boot (a full
 * firmware startup), IOMFB READY + A410, the DPTX publication, lifecycle RPCs
 * (5 s each), the client open, and the link and catalog after the first HPD
 * reply.
 */
#define DCPEXT_RTKIT_BOOT_MS	16000
#define DCPEXT_READY_MS		10000
#define DCPEXT_PUBLISH_MS	5000
#define DCPEXT_RPC_MS		5000
#define DCPEXT_IRQ_RPC_MS	1000
#define DCPEXT_OPEN_MS		10000
#define DCPEXT_SINK_MS		5000
#define DCPEXT_SETTLE_MS	500

/*
 * Built-in HDMI: the converter's HPD is high for a sink after 400 ms, gone
 * after 2 s low (its short low pulses are interrupts for the converter
 * service, not removal), and the firmware needs the converter service every
 * 100 ms from REQUEST_DISPLAY to the release: without it, it never learns of
 * converter events (sink metadata, FRL readiness).
 */
#define DCPEXT_HDMI_HPD_HIGH_MS	400
#define DCPEXT_HDMI_HPD_LOW_MS	2000
#define DCPEXT_HDMI_SERVICE_MS	100

/* AV EDID object: 8-byte OSSerialize prefix + 1024 bytes. */
#define DCPEXT_EDID_CAPACITY	1032
#define DCPEXT_EDID_MAX		1024
#define DCPEXT_EDID_OBJECT	(64 + 48 + DCPEXT_EDID_CAPACITY)

/* The built-in HDMI output: its DP PHY, the converter's HPD line, HDMI-A. */
static bool dcp_t6030_ext_hdmi(struct device_node *np)
{
	return of_property_match_string(np, "apple,connector-type", "HDMI-A") >= 0 &&
	       of_property_match_string(np, "phy-names", "dp-phy") >= 0 &&
	       of_property_present(np, "hdmi-hpd-gpios");
}

/*
 * The handoff, from the device tree alone: the board, the qualified 14.7.0
 * image, one of the two controllers' coprocessor windows, the firmware
 * reservations and the outputs: Type-C routes or the built-in HDMI port.
 */
static int dcp_t6030_ext_handoff(struct device_node *np, u64 *coproc)
{
	struct device_node *routes __free(device_node) = NULL;
	struct resource res;
	const char *uuid;
	u32 compat[3];
	int index;

	if (!of_machine_is_compatible("apple,j514s") ||
	    !of_machine_is_compatible("apple,t6030") ||
	    !of_device_is_compatible(np, "apple,t6030-dcpext"))
		return -ENODEV;
	if (of_property_count_u32_elems(np, "apple,firmware-compat") != 3 ||
	    of_property_read_u32_array(np, "apple,firmware-compat", compat, 3) ||
	    compat[0] != 14 || compat[1] != 7 || compat[2] != 0 ||
	    of_property_read_string(np, "apple,firmware-uuid", &uuid) ||
	    strcmp(uuid, J514S_DCPEXT_UUID))
		return -EPROTONOSUPPORT;
	index = of_property_match_string(np, "reg-names", "coproc");
	if (index < 0 || of_address_to_resource(np, index, &res) ||
	    resource_size(&res) != SZ_16K ||
	    (res.start != J514S_DCPEXT0_COPROC && res.start != J514S_DCPEXT1_COPROC))
		return -EINVAL;
	/* The loader publishes a controller only with its firmware reserved. */
	if (of_count_phandle_with_args(np, "memory-region", NULL) <= 0)
		return -ENOENT;
	routes = of_get_child_by_name(np, "typec-routes");
	if (!routes && !dcp_t6030_ext_hdmi(np))
		return -ENOENT;
	*coproc = res.start;
	return 0;
}

/*
 * Whether a published controller takes part in the DRM device: exactly when
 * its handoff is qualified, which the probe then admits.  A controller the
 * loader did not publish is disabled and never waited for.
 */
bool dcp_t6030_ext_supported(struct device_node *np)
{
	u64 coproc;

	return !dcp_t6030_ext_handoff(np, &coproc);
}

int dcp_t6030_ext_admit(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	u64 coproc;
	int ret;

	ret = dcp_t6030_ext_handoff(dev->of_node, &coproc);
	if (ret) {
		dev_info(dev, "no qualified boot loader DCPEXT handoff (%d); not binding\n", ret);
		return -ENODEV;
	}
	dev_info(dev, "J514S DCPEXT%u handoff admitted\n", coproc == J514S_DCPEXT1_COPROC);
	return 0;
}

/* ---- DPTX link callbacks: decoding and replies ---- */

static u32 dcpext_get32(const void *data, size_t off)
{
	return get_unaligned_le32((const u8 *)data + off);
}

/* The callback data each index reads or writes, at least. */
static size_t dcpext_link_min_size(u32 index, u32 lanes)
{
	switch (index) {
	case DPTX_APCALL_GET_MAX_DRIVE_SETTINGS:
	case DPTX_APCALL_SET_LINK_RATE:
	case DPTX_APCALL_GET_MAX_LANE_COUNT:
	case DPTX_APCALL_GET_ACTIVE_LANE_COUNT:
	case DPTX_APCALL_SET_ACTIVE_LANE_COUNT:
	case DPTX_APCALL_SET_DOWN_SPREAD:
		return DCPEXT_CB_VALUE + 8;
	case DPTX_APCALL_GET_MAX_LINK_RATE:
	case DPTX_APCALL_GET_LINK_RATE:
	case DPTX_APCALL_GET_SUPPORTS_DOWN_SPREAD:
	case DPTX_APCALL_GET_DOWN_SPREAD:
	case DPTX_APCALL_GET_SUPPORTS_LANE_MAPPING:
	case DPTX_APCALL_GET_SUPPORTS_HPD:
	case DPTX_APCALL_SET_DRIVE_SETTINGS:
		return DCPEXT_CB_VALUE + 4;
	case DPTX_APCALL_GET_DRIVE_SETTINGS:
		return DCPEXT_CB_DRIVE + 8 * lanes;
	default:
		return 4;	/* the status word */
	}
}

/*
 * Decode DPTX callback @index of the external profile and plan the hardware
 * step its acknowledgement depends on.  An error means no reply at all: an
 * unknown callback or a request outside the qualified profile is never
 * acknowledged with an invented answer.
 */
int dcpext_link_plan(const struct dcpext_link *link, u32 index,
		     const void *data, size_t size, struct dcpext_link_op *op)
{
	u32 value, lanes, i;

	*op = (struct dcpext_link_op) { .hw = DCPEXT_HW_NONE };
	if (size < dcpext_link_min_size(index, link->lanes_max))
		return -EMSGSIZE;

	switch (index) {
	case DPTX_APCALL_ACTIVATE:
		if (!link->lanes_max)
			return -ENODEV;
		op->hw = DCPEXT_HW_AUX_ON;
		return 0;
	case DPTX_APCALL_DEACTIVATE:
		op->hw = DCPEXT_HW_DEACTIVATE;
		return 0;
	case DPTX_APCALL_SET_DRIVE_SETTINGS:
		/* Body 32 + 8 * lanes: the whole negotiated width. */
		lanes = dcpext_get32(data, DCPEXT_CB_VALUE);
		if (!lanes || lanes != link->lanes_max || lanes > DCPEXT_MAX_LANES ||
		    size < DCPEXT_CB_DRIVE + 8 * lanes)
			return -EINVAL;
		op->lanes = lanes;
		for (i = 0; i < lanes; i++) {
			op->voltage[i] = dcpext_get32(data, DCPEXT_CB_DRIVE + 8 * i);
			op->pre[i] = dcpext_get32(data, DCPEXT_CB_DRIVE + 8 * i + 4);
			if (op->voltage[i] > DCPEXT_DRIVE_MAX || op->pre[i] > DCPEXT_DRIVE_MAX ||
			    op->voltage[i] + op->pre[i] > DCPEXT_DRIVE_MAX)
				return -EINVAL;
		}
		/* Drive levels are programmed into a running link only. */
		if (link->rate)
			op->hw = DCPEXT_HW_DRIVE;
		return 0;
	case DPTX_APCALL_WILL_CHANGE_LINKG_CONFIG:
		if (link->xbar_up)
			op->hw = DCPEXT_HW_XBAR_DOWN;
		return 0;
	case DPTX_APCALL_DID_CHANGE_LINK_CONFIG:
		/*
		 * Up again after the change, also at rate 0 while the sink is
		 * linked: the connection carries AUX.
		 */
		if (!link->xbar_up && (link->rate || link->linked))
			op->hw = DCPEXT_HW_XBAR_UP;
		return 0;
	case DPTX_APCALL_SET_LINK_RATE:
		value = dcpext_get32(data, DCPEXT_CB_VALUE);
		/* No down-spread; RBR, HBR and HBR2, HBR3 where the PHY has it. */
		if (dcpext_get32(data, DCPEXT_CB_VALUE2) ||
		    (value && value != DCPEXT_RATE_RBR && value != DCPEXT_RATE_HBR &&
		     value != DCPEXT_RATE_HBR2 &&
		     (value != DCPEXT_RATE_HBR3 || link->rate_max != DCPEXT_RATE_HBR3)))
			return -EINVAL;
		op->hw = DCPEXT_HW_RATE;
		op->value = value;
		return 0;
	case DPTX_APCALL_SET_ACTIVE_LANE_COUNT:
		value = get_unaligned_le64((const u8 *)data + DCPEXT_CB_VALUE);
		/* The whole width or nothing: the lane order is per pair. */
		if (value && value != link->lanes_max)
			return -EINVAL;
		op->hw = DCPEXT_HW_LANES;
		op->value = value;
		return 0;
	case DPTX_APCALL_SET_DOWN_SPREAD:
		return get_unaligned_le64((const u8 *)data + DCPEXT_CB_VALUE) ? -EINVAL : 0;
	case DPTX_APCALL_GET_MAX_DRIVE_SETTINGS:
	case DPTX_APCALL_GET_DRIVE_SETTINGS:
	case DPTX_APCALL_GET_MAX_LINK_RATE:
	case DPTX_APCALL_GET_LINK_RATE:
	case DPTX_APCALL_GET_MAX_LANE_COUNT:
	case DPTX_APCALL_GET_ACTIVE_LANE_COUNT:
	case DPTX_APCALL_GET_SUPPORTS_DOWN_SPREAD:
	case DPTX_APCALL_GET_DOWN_SPREAD:
	case DPTX_APCALL_GET_SUPPORTS_LANE_MAPPING:
	case DPTX_APCALL_GET_SUPPORTS_HPD:
	case DPTX_APCALL_INACTIVE_SINK_DETECTED:
	case DPTX_APCALL_SET_TILED_DISPLAY_HINTS:
	case DPTX_APCALL_DEVICE_NOT_RESPONDING:
	case DPTX_APCALL_DEVICE_BUSY_TIMEOUT:
	case DPTX_APCALL_DEVICE_NOT_STARTED:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * The planned step succeeded (with @op holding what was programmed): update
 * the link and build the reply, the callback data with status 0 at +0 and
 * the callback's results.
 */
int dcpext_link_reply(struct dcpext_link *link, u32 index,
		      const struct dcpext_link_op *op, const void *data,
		      size_t size, void *reply, size_t room)
{
	u8 *r = reply;
	u32 i;

	if (room < size)
		return -EMSGSIZE;
	memcpy(r, data, size);
	put_unaligned_le32(0, r);

	switch (index) {
	case DPTX_APCALL_ACTIVATE:
		link->aux = true;
		break;
	case DPTX_APCALL_DEACTIVATE:
		link->xbar_up = false;
		link->aux = false;
		link->rate = 0;
		link->lanes = 0;
		break;
	case DPTX_APCALL_GET_MAX_DRIVE_SETTINGS:
		put_unaligned_le32(DCPEXT_DRIVE_MAX, r + DCPEXT_CB_VALUE);
		put_unaligned_le32(DCPEXT_DRIVE_MAX, r + DCPEXT_CB_VALUE2);
		break;
	case DPTX_APCALL_SET_DRIVE_SETTINGS:
		/* What the PHY programmed, or the request while the link is down. */
		for (i = 0; i < op->lanes; i++) {
			link->drive[i][0] = op->voltage[i];
			link->drive[i][1] = op->pre[i];
		}
		break;
	case DPTX_APCALL_GET_DRIVE_SETTINGS:
		/* Lane count at +0, control +16 cleared, the programmed levels. */
		put_unaligned_le32(link->lanes_max, r);
		put_unaligned_le32(0, r + DCPEXT_CB_VALUE);
		for (i = 0; i < link->lanes_max; i++) {
			put_unaligned_le32(link->drive[i][0], r + DCPEXT_CB_DRIVE + 8 * i);
			put_unaligned_le32(link->drive[i][1], r + DCPEXT_CB_DRIVE + 8 * i + 4);
		}
		break;
	case DPTX_APCALL_WILL_CHANGE_LINKG_CONFIG:
		link->xbar_up = false;
		break;
	case DPTX_APCALL_DID_CHANGE_LINK_CONFIG:
		if (op->hw == DCPEXT_HW_XBAR_UP)
			link->xbar_up = true;
		/* A link-up ends an inactive sink. */
		if (link->rate)
			link->inactive = false;
		break;
	case DPTX_APCALL_GET_MAX_LINK_RATE:
		put_unaligned_le32(link->rate_max ?: DCPEXT_RATE_MAX, r + DCPEXT_CB_VALUE);
		break;
	case DPTX_APCALL_GET_LINK_RATE:
		put_unaligned_le32(link->rate, r + DCPEXT_CB_VALUE);
		break;
	case DPTX_APCALL_SET_LINK_RATE:
		link->xbar_up = false;
		link->rate = op->value;
		break;
	case DPTX_APCALL_GET_MAX_LANE_COUNT:
		put_unaligned_le64(link->lanes_max, r + DCPEXT_CB_VALUE);
		break;
	case DPTX_APCALL_GET_ACTIVE_LANE_COUNT:
		put_unaligned_le64(link->lanes, r + DCPEXT_CB_VALUE);
		break;
	case DPTX_APCALL_SET_ACTIVE_LANE_COUNT:
		link->lanes = op->value;
		break;
	case DPTX_APCALL_GET_SUPPORTS_DOWN_SPREAD:
	case DPTX_APCALL_GET_DOWN_SPREAD:
	case DPTX_APCALL_GET_SUPPORTS_LANE_MAPPING:
		put_unaligned_le32(0, r + DCPEXT_CB_VALUE);
		break;
	case DPTX_APCALL_GET_SUPPORTS_HPD:
		put_unaligned_le32(1, r + DCPEXT_CB_VALUE);
		break;
	case DPTX_APCALL_INACTIVE_SINK_DETECTED:
		link->inactive = true;
		break;
	default:
		break;
	}
	return 0;
}

/* ---- HPD generations and IRQ_HPD ---- */

/* An HPD transition: a new generation, queued IRQs dropped.  True if changed. */
bool dcpext_hpd_set(struct dcpext_hpd *hpd, bool high)
{
	if (hpd->high == high)
		return false;
	hpd->high = high;
	hpd->generation++;
	hpd->irq_dropped += hpd->irq_pending;
	hpd->irq_pending = 0;
	return true;
}

/* A fresh IRQ_HPD: queued for the current sink, if the firmware has its HPD. */
bool dcpext_hpd_irq(struct dcpext_hpd *hpd, bool delivered)
{
	if (!hpd->high || !delivered) {
		hpd->irq_dropped++;
		return false;
	}
	if (hpd->irq_pending && hpd->irq_generation != hpd->generation) {
		hpd->irq_dropped += hpd->irq_pending;
		hpd->irq_pending = 0;
	}
	hpd->irq_pending++;
	hpd->irq_generation = hpd->generation;
	hpd->irq_queued++;
	return true;
}

/* The next IRQ_HPD to forward, never one of an earlier sink. */
bool dcpext_hpd_irq_take(struct dcpext_hpd *hpd, u64 *generation)
{
	if (!hpd->irq_pending)
		return false;
	if (hpd->irq_generation != hpd->generation || !hpd->high) {
		hpd->irq_dropped += hpd->irq_pending;
		hpd->irq_pending = 0;
		return false;
	}
	hpd->irq_pending--;
	*generation = hpd->generation;
	return true;
}

/* ---- The sink's catalog ---- */

/* 16.16 Hz refresh of a DRM mode, from its kHz clock and totals. */
static u64 dcpext_rate16(const struct drm_display_mode *mode)
{
	u64 total = (u64)mode->htotal * mode->vtotal;

	return total ? div64_u64((u64)mode->clock * 1000 * 65536 + total / 2, total) : 0;
}

/* Payload of one lane at a link rate code: 0.27 Gb/s per unit, 8b/10b. */
static u64 dcpext_lane_kbps(u32 rate)
{
	return (u64)rate * 270000 * 8 / 10;
}

/*
 * Keep the timings the routed link can carry uncompressed (24 bpp within the
 * lanes' payload at the link's highest rate @rate, 0 for HBR2)
 * or compressed by the firmware (a DSC-required colour the sink takes
 * compressed, at least DCPEXT_DSC_MIN_BPP within the payload less FEC), and
 * the external pipe is qualified with (at most 3840x2160, 24 to 120 Hz). If
 * the firmware's best timing goes, the largest, then fastest, remaining one
 * becomes preferred.
 */
unsigned int dcpext_catalog_filter(struct dcp_display_mode *modes, unsigned int count,
				   u32 lanes, u32 rate, unsigned int *rejected)
{
	u64 budget = lanes * dcpext_lane_kbps(rate ?: DCPEXT_RATE_MAX);
	u64 dsc_budget = budget - budget * DCPEXT_FEC_PERMILLE / 1000;
	unsigned int i, kept = 0, best = 0;
	bool preferred = false;

	*rejected = 0;
	for (i = 0; i < count; i++) {
		const struct drm_display_mode *m = &modes[i].mode;
		u64 rate = dcpext_rate16(m);
		bool fits = modes[i].dsc ?
			(u64)m->clock * DCPEXT_DSC_MIN_BPP <= dsc_budget :
			(u64)m->clock * DCPEXT_WIRE_BPP <= budget;

		if (!fits || m->hdisplay > DCPEXT_MAX_WIDTH || m->vdisplay > DCPEXT_MAX_HEIGHT ||
		    rate < DCPEXT_MIN_RATE_16 || rate > DCPEXT_MAX_RATE_16) {
			(*rejected)++;
			continue;
		}
		preferred |= m->type & DRM_MODE_TYPE_PREFERRED;
		modes[kept++] = modes[i];
	}
	if (preferred || !kept)
		return kept;
	for (i = 1; i < kept; i++) {
		const struct drm_display_mode *a = &modes[i].mode, *b = &modes[best].mode;
		u64 area_a = (u64)a->hdisplay * a->vdisplay, area_b = (u64)b->hdisplay * b->vdisplay;

		if (area_a > area_b || (area_a == area_b && dcpext_rate16(a) > dcpext_rate16(b)))
			best = i;
	}
	modes[best].mode.type |= DRM_MODE_TYPE_PREFERRED;
	return kept;
}

/*
 * The AV service's EDID object: capacity 1032 echoed, the EDID as an
 * OSSerialize data value of n bytes, n a multiple of 128 up to 1024 covering
 * the base block's extension count, every block summing to zero, and the
 * returned body covering all of it.  Returns n.
 */
int dcpext_edid_object(const u8 *o, size_t len, const u8 **edid)
{
	static const u8 header[] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	u64 used;
	u32 n, body, i, j;
	u8 sum;

	if (len < 120 || get_unaligned_le16(o + 2) != 1 || get_unaligned_le32(o + 4) != 7 ||
	    get_unaligned_le32(o + 12) != EPIC_SERVICE_CALL_MAGIC ||
	    get_unaligned_le64(o + 64) != DCPEXT_EDID_CAPACITY)
		return -EPROTO;
	used = get_unaligned_le64(o + 96);
	body = get_unaligned_le32(o + 8);
	if (used < 136 || used > DCPEXT_EDID_CAPACITY || get_unaligned_le32(o + 112) != 0xd3)
		return -EPROTO;
	n = used - 8;
	if (n % 128 || n > DCPEXT_EDID_MAX || 120 + n > len || 56 + n > body ||
	    64 + (size_t)body > len || get_unaligned_le32(o + 116) != (0x8a000000 | n) ||
	    memcmp(o + 120, header, sizeof(header)) || n != 128 * (o[120 + 126] + 1U))
		return -EPROTO;
	for (i = 0; i < n; i += 128) {
		for (sum = 0, j = 0; j < 128; j++)
			sum += o[120 + i + j];
		if (sum)
			return -EPROTO;
	}
	*edid = o + 120;
	return n;
}

/* ---- Hardware steps of the routed port ---- */

static struct dcp_ext_v14_7 *dcp_ext(struct apple_dcp *dcp)
{
	return dcp->v14_7->ext;
}

/*
 * Crossbar connection up or down while the route keeps it selected (park
 * and unpark around link changes), looked up at runtime so appledrm does not
 * require the crossbar driver.
 */
static int dcp_ext_xbar_link(struct apple_dcp_typec_route *route, bool up)
{
	typeof(&apple_dpxbar_link_up) fn;
	int ret;

	fn = up ? symbol_get(apple_dpxbar_link_up) : symbol_get(apple_dpxbar_link_down);
	if (!fn)
		return -ENOENT;
	ret = fn(route->xbar);
	if (up)
		symbol_put(apple_dpxbar_link_up);
	else
		symbol_put(apple_dpxbar_link_down);
	return ret;
}

/* Run a DPTX callback's planned step on the held route (link_lock held). */
static int dcp_ext_link_hw(struct dcp_ext_v14_7 *ext, struct dcpext_link_op *op)
{
	struct apple_dcp_typec_route *route = ext->route;
	union phy_configure_opts opts = {};
	unsigned int i;
	int ret;

	lockdep_assert_held(&ext->link_lock);
	if (op->hw == DCPEXT_HW_NONE)
		return 0;
	/* Without a route the link is already stopped and parked. */
	if (!route)
		return op->hw == DCPEXT_HW_DEACTIVATE || op->hw == DCPEXT_HW_XBAR_DOWN ?
		       0 : -ENODEV;

	switch (op->hw) {
	case DCPEXT_HW_AUX_ON:
		return phy_set_mode_ext(route->phy, PHY_MODE_DP, 0);
	case DCPEXT_HW_DEACTIVATE:
		/* Park, then stop the main link and AUX. */
		ret = ext->link.xbar_up ? dcp_ext_xbar_link(route, false) : 0;
		if (ret)
			return ret;
		ext->link.xbar_up = false;
		return phy_set_mode_ext(route->phy, PHY_MODE_INVALID, 0);
	case DCPEXT_HW_XBAR_DOWN:
		return dcp_ext_xbar_link(route, false);
	case DCPEXT_HW_XBAR_UP:
		return dcp_ext_xbar_link(route, true);
	case DCPEXT_HW_RATE:
		/* The crossbar is parked while the rate changes. */
		ret = ext->link.xbar_up ? dcp_ext_xbar_link(route, false) : 0;
		if (ret)
			return ret;
		ext->link.xbar_up = false;
		/* Rate 0 once callback 1 stopped the link and AUX: nothing to stop. */
		if (!op->value && !ext->link.aux && !ext->link.rate)
			return 0;
		opts.dp.link_rate = op->value * 270;
		opts.dp.set_rate = 1;
		return phy_configure(route->phy, &opts);
	case DCPEXT_HW_LANES:
		opts.dp.lanes = op->value;
		opts.dp.set_lanes = 1;
		return phy_configure(route->phy, &opts);
	case DCPEXT_HW_DRIVE:
		opts.dp.lanes = op->lanes;
		opts.dp.set_voltages = 1;
		for (i = 0; i < op->lanes; i++) {
			opts.dp.voltage[i] = op->voltage[i];
			opts.dp.pre[i] = op->pre[i];
		}
		ret = phy_configure(route->phy, &opts);
		if (ret)
			return ret;
		/* Report what was programmed, never an unapplied request. */
		for (i = 0; i < op->lanes; i++) {
			op->voltage[i] = opts.dp.voltage[i];
			op->pre[i] = opts.dp.pre[i];
		}
		return 0;
	default:
		return -EINVAL;
	}
}

static void dcp_ext_update_connector(struct dcp_ext_v14_7 *ext);

/*
 * A DPTX callback of the external profile (EP 0x2a, AFK receive worker).  The
 * callback index decides; the group is only logged.  A refused callback gets
 * no reply; a hardware step that fails leaves the link in an unknown state
 * and latches this controller.
 */
int dcp_ext_v14_7_dptx_call(struct apple_epic_service *service, u16 group, u32 command,
			    const void *data, size_t size, void *reply, size_t room)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	struct dcpext_link_op op;
	bool linked = false, was_inactive, inactive;
	int ret;

	if (command < ARRAY_SIZE(ext->callbacks))
		ext->callbacks[command]++;
	dev_dbg(dcp->dev, "DPTX callback %u:%u (%zu bytes)\n", group, command, size);

	scoped_guard(mutex, &ext->link_lock) {
		was_inactive = ext->link.inactive;
		ret = dcpext_link_plan(&ext->link, command, data, size, &op);
		if (ret) {
			ext->callbacks_refused++;
			dev_err(dcp->dev, "DPTX callback %u:%u (%zu bytes) refused: %d\n",
				group, command, size, ret);
			return ret;
		}
		ret = dcp_ext_link_hw(ext, &op);
		if (ret) {
			dev_err(dcp->dev, "DPTX callback %u link step %d failed: %d\n",
				command, op.hw, ret);
			iomfb_v14_7_fail(dcp, "DPTX link step failed");
			return ret;
		}
		ret = dcpext_link_reply(&ext->link, command, &op, data, size, reply, room);
		linked = ext->link.lanes && ext->link.rate;
		inactive = ext->link.inactive;
	}
	if (ret)
		return ret;

	/*
	 * An inactive sink (a TV in standby or on another input) is reported
	 * disconnected until its link is up again.
	 */
	if (inactive != was_inactive) {
		dev_info(dcp->dev, inactive ? "DPTX sink inactive: disconnected until it is active again\n" :
					      "DPTX sink active again\n");
		dcp_ext_update_connector(ext);
	}

	switch (command) {
	case DPTX_APCALL_SET_LINK_RATE:
		dev_info(dcp->dev, "DPTX link rate %#x\n", op.value);
		break;
	case DPTX_APCALL_SET_ACTIVE_LANE_COUNT:
		if (op.value)
			complete(&dcp->dptxport[0].linkcfg_completion);
		break;
	case DPTX_APCALL_DID_CHANGE_LINK_CONFIG:
		if (linked)
			dev_info(dcp->dev, "DPTX link up: %u lanes at %#x\n",
				 ext->link.lanes, ext->link.rate);
		break;
	case DPTX_APCALL_SET_TILED_DISPLAY_HINTS:
		if (size >= 68)
			dev_info(dcp->dev, "DPTX tiled display hints: %ux%u at %u\n",
				 dcpext_get32(data, 56), dcpext_get32(data, 60),
				 dcpext_get32(data, 64));
		break;
	case DPTX_APCALL_DEVICE_NOT_RESPONDING:
	case DPTX_APCALL_DEVICE_BUSY_TIMEOUT:
	case DPTX_APCALL_DEVICE_NOT_STARTED:
		ext->sink_errors++;
		dev_warn(dcp->dev, "DPTX sink error %u: %*ph\n", command,
			 (int)min_t(size_t, size, 16), data);
		break;
	default:
		break;
	}
	wake_up_all(&ext->wq);
	return 0;
}

/* ---- DPTX port commands ---- */

struct dcpext_connect_cmd {
	__le32 attributes;
	__le32 target;
} __packed;

struct dcpext_hpd_cmd {
	u8 pad[16];
	__le32 level;
} __packed;

static int dcp_ext_dptx_command(struct apple_dcp *dcp, enum dcpext_dptx_cmd cmd, u32 target)
{
	struct dptx_port *port = &dcp->dptxport[0];
	struct apple_epic_service *service;
	struct dcpext_connect_cmd connect = {}, connected = {};
	struct dcpext_hpd_cmd hpd = {}, hpd_reply = {};
	u8 status[16] = {};
	int ret;

	if (!dptx_service_available(port))
		return -ENODEV;
	service = port->service;

	switch (cmd) {
	case DCPEXT_DPTX_CONNECT:
		/* A direct PHY: attributes word 0, then the ATC target. */
		connect.target = cpu_to_le32(target);
		ret = afk_service_call_timeout(service, 0, 11, &connect, sizeof(connect), 24,
					       &connected, sizeof(connected), 24, DCPEXT_RPC_MS);
		if (!ret && le32_to_cpu(connected.target) != target)
			ret = -EPROTO;
		return ret;
	case DCPEXT_DPTX_REQUEST:
		return afk_service_call_timeout(service, 0, 6, NULL, 0, 16, NULL, 0, 16,
						DCPEXT_RPC_MS);
	case DCPEXT_DPTX_RELEASE:
		return afk_service_call_timeout(service, 0, 7, NULL, 0, 16, NULL, 0, 16,
						DCPEXT_RPC_MS);
	case DCPEXT_DPTX_HPD_LOW:
	case DCPEXT_DPTX_HPD_HIGH:
		hpd.level = cpu_to_le32(cmd == DCPEXT_DPTX_HPD_HIGH);
		ret = afk_service_call_timeout(service, 8, 8, &hpd, sizeof(hpd), 12,
					       &hpd_reply, sizeof(hpd_reply), 12, DCPEXT_RPC_MS);
		if (!ret && hpd_reply.level != hpd.level)
			ret = -EPROTO;
		return ret;
	case DCPEXT_DPTX_IRQ:
		/* InterruptRequestOccurred: group 8, command 9, zero body. */
		ret = afk_service_call_timeout(service, 8, 9, NULL, 0, 16, status,
					       sizeof(status), 0, DCPEXT_IRQ_RPC_MS);
		if (!ret && get_unaligned_le32(status))
			ret = -EIO;
		return ret;
	}
	return -EINVAL;
}

static const char *const dcpext_dptx_names[] = {
	[DCPEXT_DPTX_CONNECT] = "CONNECT",
	[DCPEXT_DPTX_REQUEST] = "REQUEST_DISPLAY",
	[DCPEXT_DPTX_HPD_LOW] = "HPD low",
	[DCPEXT_DPTX_HPD_HIGH] = "HPD high",
	[DCPEXT_DPTX_RELEASE] = "RELEASE_DISPLAY",
	[DCPEXT_DPTX_IRQ] = "IRQ_HPD",
};

static int dcp_ext_command(struct dcp_ext_v14_7 *ext, enum dcpext_dptx_cmd cmd, u32 target)
{
	struct apple_dcp *dcp = ext->dcp;
	int ret = dcp_ext_dptx_command(dcp, cmd, target);

	if (ret)
		dev_err(dcp->dev, "DPTX %s failed: %d\n", dcpext_dptx_names[cmd], ret);
	else
		dev_dbg(dcp->dev, "DPTX %s done\n", dcpext_dptx_names[cmd]);
	return ret;
}

/* ---- The connector and the sink's description ---- */

static u64 dcp_ext_generation(struct dcp_ext_v14_7 *ext, bool *high)
{
	unsigned long flags;
	u64 generation;

	spin_lock_irqsave(&ext->lock, flags);
	generation = ext->hpd.generation;
	if (high)
		*high = ext->hpd.high;
	spin_unlock_irqrestore(&ext->lock, flags);
	return generation;
}

/*
 * The connector reports the sink once the firmware has its HPD and the
 * sink's catalog of this HPD generation is admitted, unless the sink of a
 * powered display is inactive (DPTX callback 20 until its link is up or it is
 * described again; a display powered off has no link to keep).
 */
static void dcp_ext_update_connector(struct dcp_ext_v14_7 *ext)
{
	struct apple_dcp *dcp = ext->dcp;
	struct apple_connector *connector = READ_ONCE(dcp->connector);
	bool high, connected;
	u64 generation = dcp_ext_generation(ext, &high);
	unsigned int action;

	connected = high && READ_ONCE(ext->hpd_delivered) && READ_ONCE(ext->catalog_valid) &&
		    READ_ONCE(ext->catalog_generation) == generation &&
		    READ_ONCE(dcp->nr_modes) &&
		    !(READ_ONCE(ext->link.inactive) && READ_ONCE(ext->display_on)) &&
		    !READ_ONCE(dcp->crashed);
	if (!connected && connector)
		WRITE_ONCE(connector->edid_invalidation_pending, true);
	action = dcp_mode_hotplug(&dcp->mode_state, connected,
				  connector ? &connector->connected : NULL);
	dcp_handle_hotplug_actions(dcp, action);
}

static void dcp_ext_publish_modes(struct apple_dcp *dcp, struct dcp_display_mode *modes,
				  unsigned int count)
{
	unsigned long flags;

	/* Mode readers may still hold the previous array; it is retained. */
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	dcp->modes = modes;
	dcp->nr_modes = count;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
}

/*
 * D129 TimingElements (or D107 removing it, @blob NULL): the sink's catalog
 * for the current HPD generation, through the common parser's external SDR
 * path in its fw14 RGB8 profile, then the routed link's budget.  A catalog
 * that does not parse admits nothing; IDs come from the catalog only.
 */
void dcp_ext_v14_7_catalog(struct apple_dcp *dcp, const void *blob, u32 size)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	struct dcp_display_mode *modes = NULL;
	unsigned int count = 0, kept = 0, rejected = 0, dsc = 0, i;
	struct dcp_parse_ctx ctx;
	u32 lanes, rate;
	int ret;

	scoped_guard(mutex, &ext->link_lock) {
		lanes = ext->link.lanes_max;
		rate = ext->link.rate_max;
	}
	if (blob) {
		ret = parse(blob, size, &ctx);
		if (!ret) {
			ctx.dcp = dcp;
			ctx.v14_7_rgb8 = true;
			/*
			 * DSC-required colours the sink takes compressed, on
			 * either port, within the link's budget: the firmware
			 * compresses them itself.
			 */
			ctx.v14_7_dsc = true;
			modes = enumerate_modes(&ctx, &count, dcp->width_mm, dcp->height_mm,
						0, false, true);
			if (IS_ERR(modes)) {
				ret = PTR_ERR(modes);
				modes = NULL;
			}
		}
		if (ret)
			dev_warn(dcp->dev, "TimingElements (%u bytes) not admitted: %d\n",
				 size, ret);
		if (modes)
			kept = dcpext_catalog_filter(modes, count, lanes, rate, &rejected);
		for (i = 0; i < kept; i++)
			dsc += modes[i].dsc;
		dev_info(dcp->dev, "TimingElements: %u timings admitted (%u with DSC), %u over the %u-lane budget or limits\n",
			 kept, dsc, rejected, lanes);
	}
	if (!kept) {
		kfree(modes);
		modes = NULL;
	}
	/*
	 * The firmware withdrew the sink's modes (D107), as it does when it
	 * re-plugs the sink by itself: the mode it had set is gone with them,
	 * and its display is off. The next modeset sets the mode again.
	 */
	if (!blob)
		WRITE_ONCE(ext->mode_lost, true);
	/*
	 * A sink the firmware describes again is active: it publishes a
	 * catalog only for a sink it reads. Its link trains with the next
	 * modeset (a display powered off meanwhile has no link to train).
	 */
	if (kept) {
		scoped_guard(mutex, &ext->link_lock) {
			if (ext->link.inactive) {
				ext->link.inactive = false;
				dev_info(dcp->dev, "DPTX sink described again: active\n");
			}
		}
	}
	dcp_ext_publish_modes(dcp, modes, kept);
	WRITE_ONCE(ext->catalog_admitted, kept);
	WRITE_ONCE(ext->catalog_rejected, rejected);
	WRITE_ONCE(ext->catalog_lanes, lanes);
	WRITE_ONCE(ext->catalog_generation, dcp_ext_generation(ext, NULL));
	WRITE_ONCE(ext->catalog_valid, kept > 0);
	dcp_ext_update_connector(ext);
	wake_up_all(&ext->wq);
}

/* D129 DisplayAttributes: the sink's size for the modes and the connector. */
void dcp_ext_v14_7_attributes(struct apple_dcp *dcp, const void *blob, u32 size)
{
	struct dcp_parse_ctx ctx;
	int ret;

	ret = parse(blob, size, &ctx);
	if (!ret) {
		ctx.dcp = dcp;
		ret = parse_display_attributes(&ctx, &dcp->width_mm, &dcp->height_mm);
	}
	if (ret) {
		dev_warn(dcp->dev, "DisplayAttributes not parsed: %d\n", ret);
		return;
	}
	dcp_set_dimensions(dcp);
}

/* The sink is gone: its modes, size and EDID with it. */
static void dcp_ext_withdraw(struct dcp_ext_v14_7 *ext)
{
	struct apple_dcp *dcp = ext->dcp;

	WRITE_ONCE(ext->catalog_valid, false);
	dcp_ext_publish_modes(dcp, NULL, 0);
	dcp->width_mm = 0;
	dcp->height_mm = 0;
	dcp_ext_update_connector(ext);
}

u32 dcp_ext_v14_7_stride(struct apple_dcp *dcp)
{
	return READ_ONCE(dcp_ext(dcp)->stride);
}

/* No new presents while the sink is inactive. */
bool dcp_ext_v14_7_presentable(struct apple_dcp *dcp)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);

	if (!READ_ONCE(ext->link.inactive) && READ_ONCE(ext->display_on))
		return true;
	ext->presents_refused++;
	return false;
}

/* The session latched: no sink is reported any more. */
void dcp_ext_v14_7_failed(struct apple_dcp *dcp)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);

	if (!ext)
		return;
	wake_up_all(&ext->wq);
	dcp_ext_update_connector(ext);
}

/* ---- Cold start ---- */

static int dcp_ext_start_locked(struct dcp_ext_v14_7 *ext)
{
	struct apple_dcp *dcp = ext->dcp;
	unsigned long deadline;
	int ret;

	lockdep_assert_held(&dcp->external_lock);
	if (ext->started)
		return READ_ONCE(dcp->crashed) ? -EIO : 0;
	if (!dcp->rtk || !dcp->iommu_dom || !iommu_get_domain_for_dev(dcp->dev))
		return -ENODEV;

	/*
	 * DMA pinned, then RUN only on a cold ASC, or nothing on one the boot
	 * loader left running: nothing is torn down from here.
	 */
	ret = dcp_ext_v14_7_run(dcp);
	if (ret == -EBUSY)
		return ret;
	ext->started = true;
	if (ret)
		goto failed;

	/* RTKit protocol 12 with the library's system endpoints. */
	deadline = jiffies + msecs_to_jiffies(DCPEXT_RTKIT_BOOT_MS);
	ret = apple_rtkit_wake(dcp->rtk);
	while (ret == -ETIME && time_before(jiffies, deadline))
		ret = apple_rtkit_boot(dcp->rtk);
	if (ret) {
		dev_err(dcp->dev, "DCPEXT RTKit boot failed: %d\n", ret);
		goto failed;
	}

	/* IOMFB: INIT, READY 0x400101, then A410 must name the external role. */
	reinit_completion(&dcp->start_done);
	ret = iomfb_start_rtkit(dcp);
	if (ret)
		goto failed;
	if (!wait_for_completion_timeout(&dcp->start_done, msecs_to_jiffies(DCPEXT_READY_MS)) ||
	    READ_ONCE(dcp->crashed) || !dcp->active) {
		ret = -ETIMEDOUT;
		goto failed;
	}

	/* DPTX (EP 0x2a, both units published) and AV (EP 0x28, opened on publication). */
	ret = dptxep_init(dcp);
	if (ret == -ETIMEDOUT && dcp->dptxep &&
	    wait_for_completion_timeout(&dcp->dptxport[0].enable_completion,
					msecs_to_jiffies(DCPEXT_PUBLISH_MS)))
		ret = 0;
	if (ret) {
		dev_err(dcp->dev, "DPTX port service not published: %d\n", ret);
		goto failed;
	}
	ret = dpavservep_init(dcp);
	if (ret)
		dev_warn(dcp->dev, "AV service endpoint not started (no EDID): %d\n", ret);
	dev_info(dcp->dev, "DCPEXT session up: READY %#llx, %u callbacks, %u buffers\n",
		 dcp->v14_7->ready_word, dcp->v14_7->callbacks, dcp->v14_7->objects);
	return 0;

failed:
	iomfb_v14_7_fail(dcp, "DCPEXT cold start failed");
	return ret ?: -EIO;
}

/* ---- Built-in HDMI converter service ---- */

/*
 * InterruptRequestOccurred (DPTX group 8, command 9) every 100 ms while the
 * firmware holds the sink, one outstanding with its 1 s reply bound. It runs
 * beside every other DPTX and IOMFB call, which do not wait for it.
 */
static void dcp_ext_service_work(struct work_struct *work)
{
	struct dcp_ext_v14_7 *ext = container_of(to_delayed_work(work), struct dcp_ext_v14_7,
						 service_work);
	struct apple_dcp *dcp = ext->dcp;
	int ret;

	if (!READ_ONCE(ext->service_armed) || READ_ONCE(dcp->crashed))
		return;
	ret = dcp_ext_dptx_command(dcp, DCPEXT_DPTX_IRQ, 0);
	if (ret) {
		ext->service_failed++;
		dev_warn_ratelimited(dcp->dev, "HDMI converter service: %d\n", ret);
	} else {
		ext->service_ok++;
	}
	if (READ_ONCE(ext->service_armed))
		queue_delayed_work(system_freezable_wq, &ext->service_work,
				   msecs_to_jiffies(DCPEXT_HDMI_SERVICE_MS));
}

/* From REQUEST_DISPLAY on, on the built-in HDMI route. */
static void dcp_ext_service_arm(struct dcp_ext_v14_7 *ext)
{
	if (!ext->fixed || ext->route != ext->fixed || READ_ONCE(ext->service_armed))
		return;
	WRITE_ONCE(ext->service_armed, true);
	mod_delayed_work(system_freezable_wq, &ext->service_work, 0);
}

/* No service request while the sink is released (or the session suspended). */
static void dcp_ext_service_disarm(struct dcp_ext_v14_7 *ext)
{
	WRITE_ONCE(ext->service_armed, false);
	cancel_delayed_work_sync(&ext->service_work);
}

/* ---- Sink connection ---- */

static void dcp_ext_av_close(struct dcp_ext_v14_7 *ext);

/* HPD low and RELEASE_DISPLAY for what the firmware holds; firmware cb 1 stops the link. */
static int dcp_ext_release_locked(struct dcp_ext_v14_7 *ext)
{
	struct apple_dcp *dcp = ext->dcp;
	int ret;

	lockdep_assert_held(&dcp->external_lock);
	/* The sink's audio goes first, as on a DPTX disconnect of the other DCPs. */
	if (dcp->avep)
		av_service_disconnect(dcp);
	/*
	 * Then the AV client: the firmware completes the HPD low only once the
	 * sink's AV device has no client left.
	 */
	dcp_ext_av_close(ext);
	dcp_ext_service_disarm(ext);
	if (ext->dptx == DCPEXT_DPTX_HPD) {
		ret = dcp_ext_command(ext, DCPEXT_DPTX_HPD_LOW, 0);
		if (ret)
			return ret;
		ext->dptx = DCPEXT_DPTX_REQUESTED;
		WRITE_ONCE(ext->hpd_delivered, false);
		WRITE_ONCE(dcp->dptxport[0].connected, false);
	}
	if (ext->dptx != DCPEXT_DPTX_NONE) {
		ret = dcp_ext_command(ext, DCPEXT_DPTX_RELEASE, 0);
		if (ret)
			return ret;
		ext->dptx = DCPEXT_DPTX_NONE;
		ext->releases++;
	}
	scoped_guard(mutex, &ext->link_lock) {
		ext->link.linked = false;
		ext->link.inactive = false;
	}
	/* The next modeset sets the mode and powers the display again. */
	ext->display_on = false;
	ext->timing_id = 0;
	ext->color_id = 0;
	return 0;
}

static bool dcp_ext_waited(struct dcp_ext_v14_7 *ext, u64 generation, bool done)
{
	return done || dcp_ext_generation(ext, NULL) != generation ||
	       READ_ONCE(ext->dcp->crashed);
}

/*
 * CONNECT, REQUEST_DISPLAY and HPD high, each reply-checked, with the crossbar
 * connection up before CONNECT.  On the first sink the client opens after the
 * HPD reply: link training follows A401/A455, so waiting for the link first
 * would time out.
 */
static int dcp_ext_link_locked(struct dcp_ext_v14_7 *ext, u64 generation)
{
	struct apple_dcp *dcp = ext->dcp;
	u32 target;
	int ret = 0;

	lockdep_assert_held(&dcp->external_lock);
	if (ext->link_error && ext->link_error_generation == generation)
		return ext->link_error;
	if (!dptx_service_available(&dcp->dptxport[0]))
		return -ENODEV;

	/*
	 * The firmware still holds an earlier sink: its release failed at the
	 * HPD low (the next HPD edge retries it). Release it before this one.
	 */
	if (ext->dptx != DCPEXT_DPTX_NONE && ext->dptx_generation != generation) {
		ret = dcp_ext_release_locked(ext);
		if (ret)
			goto failed;
	}

	if (ext->dptx != DCPEXT_DPTX_HPD) {
		scoped_guard(mutex, &ext->link_lock) {
			if (!ext->route)
				return -ENODEV;
			if (!ext->link.xbar_up) {
				ret = dcp_ext_xbar_link(ext->route, true);
				if (!ret)
					ext->link.xbar_up = true;
			}
		}
		if (ret) {
			dev_err(dcp->dev, "crossbar connection not up before CONNECT: %d\n", ret);
			goto failed;
		}
		/* CONNECT target 0x8000 | ATC << 4 (the DPTX core 0 of that ATC). */
		target = FIELD_PREP(DCPDPTX_REMOTE_PORT_ATC, dcp->dptx_phy) |
			 FIELD_PREP(DCPDPTX_REMOTE_PORT_DIE, dcp->dptx_die) |
			 DCPDPTX_REMOTE_PORT_CONNECTED;
		reinit_completion(&dcp->dptxport[0].linkcfg_completion);
		if (ext->dptx == DCPEXT_DPTX_NONE) {
			ret = dcp_ext_command(ext, DCPEXT_DPTX_CONNECT, target);
			if (ret)
				goto failed;
			ext->dptx = DCPEXT_DPTX_CONNECTED;
			ext->dptx_generation = generation;
		}
		if (ext->dptx == DCPEXT_DPTX_CONNECTED) {
			ret = dcp_ext_command(ext, DCPEXT_DPTX_REQUEST, 0);
			if (ret)
				goto failed;
			ext->dptx = DCPEXT_DPTX_REQUESTED;
		}
		/* The converter service runs from REQUEST on, before HPD. */
		dcp_ext_service_arm(ext);
		/* An HPD low meanwhile: its disconnect releases what is held. */
		if (dcp_ext_generation(ext, NULL) != generation)
			return -ECANCELED;
		ret = dcp_ext_command(ext, DCPEXT_DPTX_HPD_HIGH, 0);
		if (ret)
			goto failed;
		ext->dptx = DCPEXT_DPTX_HPD;
		ext->connects++;
		WRITE_ONCE(dcp->dptxport[0].connected, true);
		scoped_guard(mutex, &ext->link_lock)
			ext->link.linked = true;
		WRITE_ONCE(ext->hpd_delivered, true);
		dev_info(dcp->dev, "sink HPD delivered: target %#x, %u lanes\n", target,
			 ext->link.lanes_max);
		/* The AV client again, if its instance outlived the last release. */
		queue_work(system_freezable_wq, &ext->av_work);
	}

	if (!dcp->v14_7->opened) {
		ret = iomfb_v14_7_ext_open(dcp, msecs_to_jiffies(DCPEXT_OPEN_MS));
		if (ret)
			return ret;
	}

	/* The link configuration and the sink's catalog, bounded. */
	if (!wait_event_timeout(ext->wq, dcp_ext_waited(ext, generation, READ_ONCE(ext->link.lanes)),
				msecs_to_jiffies(DCPEXT_SINK_MS)))
		dev_warn(dcp->dev, "no link configuration %u ms after HPD\n", DCPEXT_SINK_MS);
	if (!wait_event_timeout(ext->wq,
				dcp_ext_waited(ext, generation,
					       READ_ONCE(ext->catalog_valid) &&
					       READ_ONCE(ext->catalog_generation) == generation),
				msecs_to_jiffies(DCPEXT_SINK_MS)))
		dev_warn(dcp->dev, "no sink catalog %u ms after HPD\n", DCPEXT_SINK_MS);
	dcp_ext_update_connector(ext);
	return 0;

failed:
	ext->link_error = ret;
	ext->link_error_generation = generation;
	return ret;
}

static void dcp_ext_connect_work(struct work_struct *work)
{
	struct dcp_ext_v14_7 *ext = container_of(work, struct dcp_ext_v14_7, connect_work);
	struct apple_dcp *dcp = ext->dcp;
	u64 generation;
	bool high;
	int ret;

	guard(mutex)(&dcp->external_lock);
	generation = dcp_ext_generation(ext, &high);
	if (!ext->drm_ready || !high || READ_ONCE(dcp->crashed) || !ext->route)
		return;
	ret = dcp_ext_start_locked(ext);
	if (!ret)
		ret = dcp_ext_link_locked(ext, generation);
	if (ret && ret != -ECANCELED)
		dev_err(dcp->dev, "sink connection failed: %d\n", ret);
}

/* HPD high from the Type-C route (or a replay): start or connect in process context. */
int dcp_ext_v14_7_connect(struct apple_dcp *dcp)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	unsigned long flags;

	spin_lock_irqsave(&ext->lock, flags);
	dcpext_hpd_set(&ext->hpd, true);
	spin_unlock_irqrestore(&ext->lock, flags);
	WRITE_ONCE(dcp->typec_cable_connected, true);
	queue_work(system_freezable_wq, &ext->connect_work);
	return 0;
}

/*
 * HPD low, SAFE or a route change: withdraw the sink, then HPD low and
 * RELEASE_DISPLAY.  A failure keeps the state, and the route its hold, for
 * the next attempt.
 */
int dcp_ext_v14_7_disconnect(struct apple_dcp *dcp, bool physical)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	unsigned long flags;

	spin_lock_irqsave(&ext->lock, flags);
	dcpext_hpd_set(&ext->hpd, false);
	spin_unlock_irqrestore(&ext->lock, flags);
	if (physical)
		WRITE_ONCE(dcp->typec_cable_connected, false);
	wake_up_all(&ext->wq);

	guard(mutex)(&dcp->external_lock);
	dcp_ext_withdraw(ext);
	if (!ext->started || READ_ONCE(dcp->crashed))
		return 0;
	/*
	 * No new present starts without the sink; one in flight completes
	 * (D589) before the release, which may stop the pipe it waits for.
	 */
	if (iomfb_queue_settle(dcp, msecs_to_jiffies(DCPEXT_SETTLE_MS)) == -ETIMEDOUT)
		dev_warn(dcp->dev, "a present is still in flight at the sink release\n");
	return dcp_ext_release_locked(ext);
}

static void dcp_ext_irq_work(struct work_struct *work)
{
	struct dcp_ext_v14_7 *ext = container_of(work, struct dcp_ext_v14_7, irq_work);
	struct apple_dcp *dcp = ext->dcp;
	unsigned long flags;
	u64 generation;
	bool take;
	int ret;

	for (;;) {
		spin_lock_irqsave(&ext->lock, flags);
		take = dcpext_hpd_irq_take(&ext->hpd, &generation);
		spin_unlock_irqrestore(&ext->lock, flags);
		if (!take || READ_ONCE(dcp->crashed))
			return;
		/* Independent of a pending HPD or modeset call. */
		ext->irq_sent++;
		ret = dcp_ext_dptx_command(dcp, DCPEXT_DPTX_IRQ, 0);
		if (ret) {
			ext->irq_failed++;
			dev_warn(dcp->dev, "DPTX IRQ_HPD not acknowledged: %d\n", ret);
		} else {
			ext->irq_ok++;
		}
	}
}

/* A fresh IRQ_HPD from the routed sink: one InterruptRequestOccurred each. */
void dcp_ext_v14_7_irq_hpd(struct apple_dcp *dcp)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	unsigned long flags;
	bool queued;

	spin_lock_irqsave(&ext->lock, flags);
	queued = dcpext_hpd_irq(&ext->hpd, READ_ONCE(ext->hpd_delivered));
	spin_unlock_irqrestore(&ext->lock, flags);
	if (queued)
		queue_work(system_freezable_wq, &ext->irq_work);
}

/* ---- Route activation ---- */

/* A link rate code from a PHY's rate in Mb/s per lane; 0 if it has none. */
static u32 dcpext_rate_code(unsigned int link_rate)
{
	return link_rate / 270;
}

/*
 * The fixed HDMI route starts with AUX on and HBR on all four lanes before
 * its crossbar comes up; the firmware then trains the rate it wants.
 */
static int dcp_ext_fixed_prepare(struct dcp_ext_v14_7 *ext, struct apple_dcp_typec_route *route)
{
	union phy_configure_opts opts = {};
	int ret;

	lockdep_assert_held(&ext->link_lock);
	ret = phy_set_mode_ext(route->phy, PHY_MODE_DP, 0);
	/* An incomplete AUX power-up is undone like a complete one. */
	ext->link.aux = true;
	if (ret)
		return ret;
	opts.dp.link_rate = 2700;
	opts.dp.set_rate = 1;
	ret = phy_configure(route->phy, &opts);
	if (!ret)
		ext->link.rate = DCPEXT_RATE_HBR;
	return ret;
}

/*
 * Activation: the PHY's DP hold freezes the negotiated mode and orientation
 * (on the fixed HDMI route: the PHY's power-up), then the crossbar
 * connection comes up at rate 0 (HDMI: at HBR).  A PHY not yet in its DP
 * mode is retried by the Type-C driver; after the hold, a crossbar error
 * keeps it (hardware was written).
 */
int dcp_ext_v14_7_route_select(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	union phy_configure_opts opts = {};
	int ret;

	guard(mutex)(&ext->link_lock);
	if (ext->route && ext->route != route)
		return -EBUSY;
	if (!ext->route) {
		ret = phy_power_on(route->phy);
		if (ret == -ENOLINK || ret == -EBUSY)
			return -EAGAIN;
		if (ret)
			return ret;
		ret = phy_validate(route->phy, PHY_MODE_DP, 0, &opts);
		if (ret || !opts.dp.lanes || opts.dp.lanes > DCPEXT_MAX_LANES) {
			phy_power_off(route->phy);
			return -EAGAIN;
		}
		ext->route = route;
		ext->link = (struct dcpext_link) {
			.lanes_max = opts.dp.lanes,
			.rate_max = dcpext_rate_code(opts.dp.link_rate),
		};
		dev_info(dcp->dev, "%s route of ATC%u: %u DP lanes held, crossbar input %u\n",
			 route == ext->fixed ? "HDMI" : "Type-C", route->dptx_phy, opts.dp.lanes,
			 route->mux_index);
		if (route == ext->fixed) {
			ret = dcp_ext_fixed_prepare(ext, route);
			if (ret) {
				dev_err(dcp->dev, "HDMI link not prepared: %d\n", ret);
				return ret;
			}
		}
	}
	if (ext->xbar_selected)
		return 0;
	ret = mux_control_select(route->xbar, route->mux_index);
	if (ret == -EBUSY && !ext->link.aux) {
		/* No idle image: nothing was written, so nothing stays held. */
		if (!phy_power_off(route->phy)) {
			ext->route = NULL;
			ext->link = (struct dcpext_link) {};
		}
		return -EAGAIN;
	}
	if (ret)
		return ret;
	ext->xbar_selected = true;
	ext->link.xbar_up = true;
	return 0;
}

/*
 * Deactivation, first step (external_lock held, before the crossbar is
 * deselected): the sink released if the firmware still holds it, and the
 * link parked and stopped if the firmware's callback 1 has not done so.
 * Nothing here consumes the route, so a failure is retried.
 */
int dcp_ext_v14_7_route_release(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	int ret;

	lockdep_assert_held(&dcp->external_lock);
	if (ext->started && !READ_ONCE(dcp->crashed) && ext->dptx != DCPEXT_DPTX_NONE) {
		ret = dcp_ext_release_locked(ext);
		if (ret)
			return ret;
	}

	guard(mutex)(&ext->link_lock);
	if (ext->route != route)
		return 0;
	if (ext->link.xbar_up) {
		ret = dcp_ext_xbar_link(route, false);
		if (ret)
			return ret;
		ext->link.xbar_up = false;
	}
	if (ext->link.aux || ext->link.rate) {
		ret = phy_set_mode_ext(route->phy, PHY_MODE_INVALID, 0);
		if (ret)
			return ret;
		ext->link.aux = false;
		ext->link.rate = 0;
		ext->link.lanes = 0;
	}
	return 0;
}

/*
 * Deactivation, second step: the crossbar idle (its image verified), then
 * the PHY hold dropped, which lets the PHY apply the deferred Type-C state.
 * A failure keeps the hold.
 */
int dcp_ext_v14_7_route_deselect(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	int ret;

	guard(mutex)(&ext->link_lock);
	/* Nothing of a route that never took the hold was selected. */
	if (ext->route != route)
		return 0;
	if (ext->xbar_selected) {
		/* The selection is dropped even when the idle image is not proven. */
		ret = mux_control_deselect(route->xbar);
		ext->xbar_selected = false;
		if (ret)
			return ret;
		ext->link.xbar_up = false;
	}
	/* A link the firmware started after the release step is stopped too. */
	if (ext->link.aux || ext->link.rate) {
		ret = phy_set_mode_ext(route->phy, PHY_MODE_INVALID, 0);
		if (ret)
			return ret;
		ext->link.aux = false;
		ext->link.rate = 0;
		ext->link.lanes = 0;
	}
	ret = phy_power_off(route->phy);
	if (ret) {
		dev_err(dcp->dev, "Type-C route of ATC%u: PHY hold not released: %d\n",
			route->dptx_phy, ret);
		return ret;
	}
	ext->route = NULL;
	ext->link = (struct dcpext_link) {};
	dev_info(dcp->dev, "%s route of ATC%u released\n",
		 route == ext->fixed ? "HDMI" : "Type-C", route->dptx_phy);
	return 0;
}

/* ---- The built-in HDMI port: the converter's HPD ---- */

/*
 * Debounced HPD: high brings the route up (PHY, link at HBR, crossbar) and
 * connects the sink; low releases the sink, parks and stops the link and
 * takes the route down. A failure is retried at the next HPD edge.
 */
static void dcp_ext_hdmi_hpd_work(struct work_struct *work)
{
	struct dcp_ext_v14_7 *ext = container_of(to_delayed_work(work), struct dcp_ext_v14_7,
						 hpd_work);
	struct apple_dcp *dcp = ext->dcp;
	bool high = gpiod_get_value_cansleep(dcp->hdmi_hpd) > 0;
	int ret;

	if (high == ext->hdmi_hpd || READ_ONCE(dcp->crashed))
		return;
	if (high) {
		ret = dcp_ext_v14_7_route_select(ext->fixed);
		if (ret) {
			dev_err(dcp->dev, "HDMI sink present, route not up: %d\n", ret);
			return;
		}
		ext->hdmi_hpd = true;
		dev_info(dcp->dev, "HDMI sink present\n");
		dcp_ext_v14_7_connect(dcp);
		return;
	}

	ext->hdmi_hpd = false;
	dev_info(dcp->dev, "HDMI sink gone\n");
	ret = dcp_ext_v14_7_disconnect(dcp, true);
	if (!ret) {
		guard(mutex)(&dcp->external_lock);
		ret = dcp_ext_v14_7_route_release(ext->fixed);
	}
	if (!ret)
		ret = dcp_ext_v14_7_route_deselect(ext->fixed);
	if (ret)
		dev_err(dcp->dev, "HDMI route not released: %d\n", ret);
}

/*
 * Both edges of the converter's HPD line restart the debounce. An edge that
 * leaves the line high while the firmware holds the sink is a converter
 * pulse: it runs the converter service at once.
 */
irqreturn_t dcp_ext_v14_7_hpd_irq(int irq, void *data)
{
	struct apple_dcp *dcp = data;
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	bool high = gpiod_get_value_cansleep(dcp->hdmi_hpd) > 0;

	ext->hpd_edges++;
	if (high && READ_ONCE(ext->service_armed)) {
		ext->service_kicks++;
		mod_delayed_work(system_freezable_wq, &ext->service_work, 0);
	}
	mod_delayed_work(system_freezable_wq, &ext->hpd_work,
			 msecs_to_jiffies(high ? DCPEXT_HDMI_HPD_HIGH_MS : DCPEXT_HDMI_HPD_LOW_MS));
	return IRQ_HANDLED;
}

/* The fixed output of a controller with the HDMI port (bound DRM device). */
static int dcp_ext_hdmi_init(struct dcp_ext_v14_7 *ext)
{
	struct apple_dcp *dcp = ext->dcp;
	struct apple_dcp_typec_route *route;

	if (ext->fixed || !dcp->fixed_phy || !dcp->xbar || !dcp->hdmi_hpd || !dcp->hdmi_hpd_irq)
		return 0;
	route = devm_kzalloc(dcp->dev, sizeof(*route), GFP_KERNEL);
	if (!route)
		return -ENOMEM;
	route->dcp = dcp;
	route->phy = dcp->fixed_phy;
	route->xbar = dcp->xbar;
	route->mux_index = dcp->fixed_mux_index;
	route->dptx_phy = dcp->fixed_dptx_phy;
	ext->fixed = route;
	return 0;
}

/* ---- AV service and EDID ---- */

/*
 * The AV service's client: the host opens the published instance (DCPAV Open)
 * to read the sink's EDID through it, and closes it (DCPAV Close) when it
 * releases the sink or the firmware terminates the instance. An open client
 * is one the firmware waits for, up to 10 s, before it removes the instance:
 * the DPTX controller holds an HPD low's reply until the instance's device
 * has closed ("timeout while waiting for DCPDPDevice close"), and with it the
 * sink's release and the next sink.
 */
static void dcp_ext_av_work(struct work_struct *work)
{
	struct dcp_ext_v14_7 *ext = container_of(work, struct dcp_ext_v14_7, av_work);
	struct apple_dcp *dcp = ext->dcp;
	struct apple_epic_service *service;
	struct apple_connector *connector;
	u8 reply[32] = {};
	u64 epoch;
	int ret;

	guard(mutex)(&ext->av_lock);
	scoped_guard(mutex, &dcp->dcpavserv.lock) {
		epoch = dcp->dcpavserv.epoch;
		if (ext->av_opened && ext->av_epoch == epoch)
			return;
		service = dcp->dcpavserv.enabled ? afk_service_get(dcp->dcpavserv.service) : NULL;
	}
	if (!service)
		return;
	/*
	 * DCPAV Open: command 4, a zero 32-byte body; the reply carries a
	 * status at +0 and the opened Boolean at +16. It never gates DPTX.
	 * (Command 6 is SetVirtualEDIDMode: with the service's interfaces
	 * present it removes and recreates them, a software unplug of the
	 * sink.)
	 */
	ret = afk_service_call_timeout(service, 0, 4, NULL, 0, 32, reply, sizeof(reply), 0,
				       DCPEXT_RPC_MS);
	if (!ret && (get_unaligned_le32(reply) || !get_unaligned_le64(reply + 16)))
		ret = -EIO;
	scoped_guard(mutex, &dcp->dcpavserv.lock) {
		ext->av_result = ret;
		/* An instance gone meanwhile took its open with it. */
		if (!ret && dcp->dcpavserv.service == service && dcp->dcpavserv.epoch == epoch) {
			ext->av_opened = true;
			ext->av_terminated = false;
			ext->av_epoch = epoch;
		}
	}
	afk_service_put(service);
	dev_info(dcp->dev, "AV service open: %d\n", ret);
	connector = READ_ONCE(dcp->connector);
	if (!ret && connector && READ_ONCE(connector->connected))
		schedule_work(&connector->hotplug_wq);
}

/*
 * DCPAV Close (command 5, a zero 16-byte body, status at +0) on the instance
 * the client opened, and only on it: a removed instance took its open with
 * it, and its successor is never closed for it. The endpoint
 * keeps receiving meanwhile: neither lock here is taken on its receive path.
 */
static void dcp_ext_av_close(struct dcp_ext_v14_7 *ext)
{
	struct apple_dcp *dcp = ext->dcp;
	struct apple_epic_service *service = NULL;
	u8 reply[16] = {};
	int ret;

	guard(mutex)(&ext->av_lock);
	scoped_guard(mutex, &dcp->dcpavserv.lock) {
		if (ext->av_opened && ext->av_epoch == dcp->dcpavserv.epoch)
			service = afk_service_get(dcp->dcpavserv.service);
		ext->av_opened = false;
		ext->av_terminated = false;
	}
	if (!service)
		return;
	ret = afk_service_call_timeout(service, 0, 5, NULL, 0, 16, reply, sizeof(reply), 0,
				       DCPEXT_RPC_MS);
	afk_service_put(service);
	/* Removed while the Close was on its way: nothing is left open. */
	if (ret == -ENODEV)
		return;
	if (!ret && get_unaligned_le32(reply))
		ret = -EIO;
	ext->av_closes++;
	if (ret)
		dev_err(dcp->dev, "AV service close failed: %d\n", ret);
	else
		dev_info(dcp->dev, "AV service closed\n");
}

static void dcp_ext_av_close_work(struct work_struct *work)
{
	dcp_ext_av_close(container_of(work, struct dcp_ext_v14_7, av_close_work));
}

/*
 * A report of the AV service (EP 0x28 receive path). Terminate (report
 * group 2, command 1) on the opened instance: no new use of it, and the
 * client's Close from a work item, so the endpoint keeps receiving. An
 * instance never opened has nothing to close, and a REPORT gets no
 * acknowledgement.
 */
void dcp_ext_v14_7_av_report(struct apple_epic_service *service, u32 type,
			     const void *data, size_t size)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	const struct epic_service_call *call = data;
	bool terminate = false;

	if (type != EPIC_SUBTYPE_STD_SERVICE || size < sizeof(*call) ||
	    le32_to_cpu(call->magic) != EPIC_SERVICE_CALL_MAGIC ||
	    le16_to_cpu(call->group) != 2 || le32_to_cpu(call->command) != 1)
		return;
	scoped_guard(mutex, &dcp->dcpavserv.lock) {
		if (dcp->dcpavserv.service == service && ext->av_opened &&
		    ext->av_epoch == dcp->dcpavserv.epoch && !ext->av_terminated) {
			ext->av_terminated = true;
			ext->av_terminates++;
			terminate = true;
		}
	}
	if (!terminate)
		return;
	dev_info(dcp->dev, "AV service terminated: closing the client\n");
	queue_work(system_freezable_wq, &ext->av_close_work);
}

/*
 * DP/HDMI audio (EP 0x29) on the opt-in, as the other DCPs start it, once per
 * session and after the sink's first EDID (see dcp_ext_v14_7_read_edid());
 * only a controller whose port@0 links an audio device gets it.
 */
static void dcp_ext_audio_work(struct work_struct *work)
{
	struct dcp_ext_v14_7 *ext = container_of(work, struct dcp_ext_v14_7, audio_work);
	struct apple_dcp *dcp = ext->dcp;
	int ret;

	if (!IS_ENABLED(CONFIG_DRM_APPLE_AUDIO) || !hdmi_audio || ext->audio_started ||
	    READ_ONCE(dcp->crashed))
		return;
	ext->audio_started = true;
	ret = avep_init(dcp);
	if (ret)
		dev_warn(dcp->dev, "AV audio endpoint not started: %d\n", ret);
}

/* The AV service was published (or re-published after a replug): open it. */
void dcp_ext_v14_7_av_published(struct apple_dcp *dcp)
{
	queue_work(system_freezable_wq, &dcp_ext(dcp)->av_work);
}

static const struct drm_edid *dcp_ext_copy_edid(struct apple_epic_service *service);

/*
 * The sink's EDID through the opened AV service, read once per HPD generation
 * and service instance: a re-plug inside the firmware keeps it. A CopyEDID
 * has the firmware report the sink's physical address again (it sets it,
 * repeats included), and with the audio endpoint's interfaces published, a
 * re-read on every reconnection was followed by their rebuild and another
 * software re-plug of the sink, each time. The audio endpoint therefore
 * starts after the first EDID, and later reconnections take the cached one.
 */
const struct drm_edid *dcp_ext_v14_7_read_edid(struct apple_epic_service *service)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	const struct drm_edid *edid;
	u64 generation, epoch;

	scoped_guard(mutex, &dcp->dcpavserv.lock) {
		if (!ext->av_opened || ext->av_terminated || ext->av_epoch != dcp->dcpavserv.epoch)
			return ERR_PTR(-ENODEV);
		epoch = ext->av_epoch;
	}
	generation = dcp_ext_generation(ext, NULL);

	guard(mutex)(&ext->edid_lock);
	if (ext->edid && ext->edid_generation == generation && ext->edid_epoch == epoch)
		return drm_edid_dup(ext->edid);

	edid = dcp_ext_copy_edid(service);
	if (IS_ERR(edid))
		return edid;
	drm_edid_free(ext->edid);
	ext->edid = drm_edid_dup(edid);
	ext->edid_generation = generation;
	ext->edid_epoch = epoch;
	queue_work(system_freezable_wq, &ext->audio_work);
	return edid;
}

/* CopyEDID: group 1, command 7. */
static const struct drm_edid *dcp_ext_copy_edid(struct apple_epic_service *service)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	struct epic_service_call *call;
	u8 *object __free(kfree) = NULL;
	const u8 *edid;
	u32 retcode;
	int ret;

	object = kzalloc(DCPEXT_EDID_OBJECT, GFP_KERNEL);
	if (!object)
		return ERR_PTR(-ENOMEM);
	call = (struct epic_service_call *)object;
	call->group = cpu_to_le16(1);
	call->command = cpu_to_le32(7);
	call->data_len = cpu_to_le32(DCPEXT_EDID_OBJECT - sizeof(*call));
	call->magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC);
	put_unaligned_le64(DCPEXT_EDID_CAPACITY, object + sizeof(*call));

	ext->edid_reads++;
	ret = afk_send_command_timeout(service, EPIC_SUBTYPE_STD_SERVICE, object,
				       DCPEXT_EDID_OBJECT, object, DCPEXT_EDID_OBJECT,
				       &retcode, DCPEXT_RPC_MS);
	if (!ret && retcode)
		ret = -EIO;
	if (!ret)
		ret = dcpext_edid_object(object, DCPEXT_EDID_OBJECT, &edid);
	if (ret < 0) {
		ext->edid_errors++;
		return ERR_PTR(ret);
	}
	return drm_edid_alloc(edid, ret);
}

/* ---- Probe, DRM and power ---- */

int dcp_ext_v14_7_init(struct apple_dcp *dcp)
{
	struct dcp_ext_v14_7 *ext;

	ext = devm_kzalloc(dcp->dev, sizeof(*ext), GFP_KERNEL);
	if (!ext)
		return -ENOMEM;
	ext->dcp = dcp;
	ext->stride = 7680;
	spin_lock_init(&ext->lock);
	mutex_init(&ext->link_lock);
	mutex_init(&ext->edid_lock);
	mutex_init(&ext->av_lock);
	init_waitqueue_head(&ext->wq);
	INIT_WORK(&ext->connect_work, dcp_ext_connect_work);
	INIT_WORK(&ext->irq_work, dcp_ext_irq_work);
	INIT_WORK(&ext->av_work, dcp_ext_av_work);
	INIT_WORK(&ext->av_close_work, dcp_ext_av_close_work);
	INIT_WORK(&ext->audio_work, dcp_ext_audio_work);
	INIT_DELAYED_WORK(&ext->hpd_work, dcp_ext_hdmi_hpd_work);
	INIT_DELAYED_WORK(&ext->service_work, dcp_ext_service_work);
	dcp->v14_7->ext = ext;
	return 0;
}

/* The DRM device has bound with the ports' connectors: a sink may start the session. */
int dcp_ext_v14_7_start(struct apple_dcp *dcp)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	bool high;
	int ret;

	ret = dcp_ext_hdmi_init(ext);
	if (ret)
		return ret;
	scoped_guard(mutex, &dcp->external_lock)
		ext->drm_ready = true;
	dcp_ext_generation(ext, &high);
	if (high)
		queue_work(system_freezable_wq, &ext->connect_work);
	/* The HDMI port: its HPD from now on, and a sink already there. */
	if (ext->fixed) {
		enable_irq(dcp->hdmi_hpd_irq);
		mod_delayed_work(system_freezable_wq, &ext->hpd_work,
				 msecs_to_jiffies(DCPEXT_HDMI_HPD_HIGH_MS));
	}
	return 0;
}

void dcp_ext_v14_7_unbind(struct apple_dcp *dcp)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);

	if (ext->fixed) {
		disable_irq(dcp->hdmi_hpd_irq);
		cancel_delayed_work_sync(&ext->hpd_work);
	}
	scoped_guard(mutex, &dcp->external_lock)
		ext->drm_ready = false;
	wake_up_all(&ext->wq);
	disable_work_sync(&ext->connect_work);
	disable_work_sync(&ext->irq_work);
	disable_work_sync(&ext->av_work);
	disable_work_sync(&ext->av_close_work);
	disable_work_sync(&ext->audio_work);
	dcp_ext_service_disarm(ext);
	scoped_guard(mutex, &ext->edid_lock) {
		drm_edid_free(ext->edid);
		ext->edid = NULL;
	}
}

/* CRTC off (DPMS, disable): A472 off; route, client and buffers stay. */
void dcp_ext_v14_7_poweroff(struct apple_dcp *dcp)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	int ret;

	cancel_delayed_work(&dcp->swap_watchdog_wq);
	guard(mutex)(&dcp->external_lock);
	dcp_mode_invalidate(&dcp->mode_state);
	if (!ext->display_on || READ_ONCE(dcp->crashed))
		return;
	/*
	 * An inactive sink's connector goes away (TV standby, another input),
	 * but the firmware keeps its display powered: it retrains only a
	 * powered display's link, and that link-up is how the sink comes back.
	 * Its release (HPD low) powers it off.
	 */
	scoped_guard(mutex, &ext->link_lock) {
		if (ext->link.inactive)
			return;
	}
	/* Audio stops with the display, as the other DCPs' poweroff has it. */
	if (dcp->avep)
		av_service_disconnect(dcp);
	ret = iomfb_v14_7_ext_power(dcp, false);
	if (ret)
		dev_err(dcp->dev, "A472 off failed: %d\n", ret);
	ext->display_on = false;
}

/* A472 on (modeset path, external_lock held): the sink's audio again. */
void dcp_ext_v14_7_display_on(struct apple_dcp *dcp)
{
	lockdep_assert_held(&dcp->external_lock);
	if (dcp->avep)
		av_service_connect(dcp);
}

/* The HDMI port after sleep: its service if the sink is held, its HPD again. */
static void dcp_ext_hdmi_resume(struct dcp_ext_v14_7 *ext)
{
	struct apple_dcp *dcp = ext->dcp;

	if (!ext->fixed)
		return;
	scoped_guard(mutex, &dcp->external_lock) {
		if (ext->dptx != DCPEXT_DPTX_NONE)
			dcp_ext_service_arm(ext);
	}
	enable_irq(dcp->hdmi_hpd_irq);
	mod_delayed_work(system_freezable_wq, &ext->hpd_work,
			 msecs_to_jiffies(DCPEXT_HDMI_HPD_HIGH_MS));
}

/*
 * System sleep: DRM has already turned the CRTC off (A472 off).  A running
 * session keeps its power; its messages wait in the mailbox until resume,
 * as the panel's do.
 */
int dcp_ext_v14_7_suspend(struct apple_dcp *dcp)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	int ret;

	if (ext->fixed) {
		disable_irq(dcp->hdmi_hpd_irq);
		cancel_delayed_work_sync(&ext->hpd_work);
	}
	disable_work_sync(&ext->connect_work);
	disable_work_sync(&ext->irq_work);
	disable_work_sync(&ext->av_work);
	disable_work_sync(&ext->audio_work);
	/*
	 * A controller never started has no session to keep: its power chain
	 * may go off for system sleep and come back cold. A started one keeps
	 * its power, as the DCPEXT has no restartable sleep yet.
	 */
	if (!ext->started)
		return 0;
	device_set_wakeup_path(dcp->dev);
	if (READ_ONCE(dcp->crashed))
		return 0;
	dcp_ext_service_disarm(ext);
	iomfb_pause_presentation(dcp, true);
	ret = iomfb_queue_settle(dcp, msecs_to_jiffies(500));
	if (ret) {
		iomfb_pause_presentation(dcp, false);
		enable_work(&ext->connect_work);
		enable_work(&ext->irq_work);
		enable_work(&ext->av_work);
		enable_work(&ext->audio_work);
		dcp_ext_hdmi_resume(ext);
		return ret;
	}
	apple_rtkit_hold_rx(dcp->rtk);
	dcp->rx_held = true;
	return 0;
}

int dcp_ext_v14_7_resume(struct apple_dcp *dcp)
{
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	bool high;

	if (dcp->rx_held) {
		dcp->rx_held = false;
		apple_rtkit_resume_rx(dcp->rtk);
		iomfb_pause_presentation(dcp, false);
	}
	enable_work(&ext->connect_work);
	enable_work(&ext->irq_work);
	enable_work(&ext->av_work);
	enable_work(&ext->audio_work);
	/* A sink that came or went meanwhile is reported by the Type-C driver. */
	dcp_ext_generation(ext, &high);
	if (high)
		queue_work(system_freezable_wq, &ext->connect_work);
	/* The HDMI port reports it through its HPD line. */
	dcp_ext_hdmi_resume(ext);
	return 0;
}

/* ---- debugfs: crtc-N/dcpext (read only) ---- */

static const char *const dcpext_dptx_states[] = {
	[DCPEXT_DPTX_NONE] = "none",
	[DCPEXT_DPTX_CONNECTED] = "connected",
	[DCPEXT_DPTX_REQUESTED] = "requested",
	[DCPEXT_DPTX_HPD] = "hpd",
};

static int dcp_ext_status_show(struct seq_file *m, void *unused)
{
	struct apple_dcp *dcp = m->private;
	struct dcp_ext_v14_7 *ext = dcp_ext(dcp);
	struct apple_dcp_typec_route *route;
	struct dcpext_hpd hpd;
	struct dcpext_link link;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&ext->lock, flags);
	hpd = ext->hpd;
	spin_unlock_irqrestore(&ext->lock, flags);
	scoped_guard(mutex, &ext->link_lock) {
		route = ext->route;
		link = ext->link;
	}

	seq_printf(m, "role=external drm_ready=%d started=%d active=%d crashed=%d client_open=%d ready=%#llx callbacks=%u buffers=%u\n",
		   ext->drm_ready, ext->started, dcp->active, READ_ONCE(dcp->crashed),
		   dcp->v14_7->opened, dcp->v14_7->ready_word, dcp->v14_7->callbacks,
		   dcp->v14_7->objects);
	seq_printf(m, "hpd=%d generation=%llu delivered=%d dptx=%s connects=%u releases=%u link_error=%d\n",
		   hpd.high, hpd.generation, READ_ONCE(ext->hpd_delivered),
		   dcpext_dptx_states[ext->dptx], ext->connects, ext->releases,
		   ext->link_error);
	if (route)
		seq_printf(m, "route %s atc=%u input=%u held=1 lanes_max=%u lanes=%u rate=%#x rate_max=%#x aux=%d xbar_up=%d linked=%d inactive=%d\n",
			   route == ext->fixed ? "hdmi" : "typec", route->dptx_phy,
			   route->mux_index, link.lanes_max, link.lanes, link.rate,
			   link.rate_max ?: DCPEXT_RATE_MAX, link.aux, link.xbar_up, link.linked,
			   link.inactive);
	else
		seq_puts(m, "route=none\n");
	if (ext->fixed)
		seq_printf(m, "hdmi hpd_line=%d hpd=%d edges=%u service armed=%d ok=%u failed=%u kicks=%u\n",
			   gpiod_get_value_cansleep(dcp->hdmi_hpd), ext->hdmi_hpd, ext->hpd_edges,
			   READ_ONCE(ext->service_armed), ext->service_ok, ext->service_failed,
			   ext->service_kicks);
	seq_puts(m, "drive=");
	for (i = 0; i < link.lanes_max && i < DCPEXT_MAX_LANES; i++)
		seq_printf(m, "%s%u/%u", i ? "," : "", link.drive[i][0], link.drive[i][1]);
	seq_puts(m, "\n");
	seq_printf(m, "sink catalog=%d generation=%llu admitted=%u rejected=%u lanes=%u modes=%u width_mm=%d height_mm=%d av_opened=%d av_result=%d av_closes=%u av_terminates=%u edid_reads=%u edid_errors=%u\n",
		   ext->catalog_valid, ext->catalog_generation, ext->catalog_admitted,
		   ext->catalog_rejected, ext->catalog_lanes, dcp->nr_modes, dcp->width_mm,
		   dcp->height_mm, ext->av_opened, ext->av_result, ext->av_closes,
		   ext->av_terminates, ext->edid_reads, ext->edid_errors);
	seq_printf(m, "display on=%d timing=%u color=%u stride=%u presents_refused=%u\n",
		   ext->display_on, ext->timing_id, ext->color_id, ext->stride,
		   ext->presents_refused);
	seq_printf(m, "irq_hpd queued=%u dropped=%u pending=%u sent=%u ok=%u failed=%u\n",
		   hpd.irq_queued, hpd.irq_dropped, hpd.irq_pending, ext->irq_sent,
		   ext->irq_ok, ext->irq_failed);
	seq_printf(m, "dptx_callbacks refused=%u sink_errors=%u", ext->callbacks_refused,
		   ext->sink_errors);
	for (i = 0; i < ARRAY_SIZE(ext->callbacks); i++)
		if (ext->callbacks[i])
			seq_printf(m, " %u:%u", i, ext->callbacks[i]);
	seq_puts(m, "\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dcp_ext_status);

void dcp_ext_v14_7_debugfs_init(struct apple_dcp *dcp, struct dentry *root)
{
	debugfs_create_file("dcpext", 0444, root, dcp, &dcp_ext_status_fops);
}
