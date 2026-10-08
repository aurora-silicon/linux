// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J613 (M3 Air 13 inch) experimental internal display: macOS 26.6.2 (25G83) IOMFB over DCPLink
 *
 * Copyright The Asahi Linux Contributors
 * Based on the M3 Pro DCP lab client by Eryk Wieliczko.
 *
 * iBoot leaves the DCP running, scanning out the boot framebuffer. This
 * variant never boots, stops or powers the coprocessor down. In order:
 *
 *  probe  the firmware must be the board's qualified image, the boot loader
 *         must have handed the display over (apple,j613-25g83-mapping-handoff, or the
 *         board's own marker), and the PMP must be running and have
 *         acknowledged the display request;
 *  bind   the DCP CPU must still be running; its RTKit session is woken;
 *  start  DCPLink is brought up, then the start signal (A401) and the first
 *         client open (A457) run with every callback answered in the
 *         kernel; the panel mode is taken from PreferredTimingElements;
 *  KMS    only after all of that does drm/apple remove the boot framebuffer
 *         driver and register the DRM device.
 *
 * Any failure before KMS leaves the boot framebuffer in place. Everything the
 * firmware has seen (the RTKit session, the RPC memory, the buffers it
 * allocated, the scanout buffers of failed swaps) is kept until reboot, and
 * the module cannot be unloaded once RTKit is up.
 *
 * Scanout is one plane, XRGB8888 or ARGB8888, linear, exactly the mode size.
 * Each flush is one swap, completed when the firmware reports that swap id,
 * and then the page-flip event is sent. The notch rows (apple,notch-height,
 * unless appledrm.t6030_show_notch=1) are outside the mode and stay black.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/soc/apple/pmp.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "dcp.h"
#include "dcp-internal.h"
#include "dcp-lifecycle.h"
#include "iomfb_internal.h"
#include "iomfb_v26_6.h"
#include "j613-25g83.h"
#include "iomfb_v26_6_link.h"
#include "iomfb_v26_6_swap.h"
#include "parser.h"

#define A(n) DCP_V26_TAG('A', n)
#define D(n) DCP_V26_TAG('D', n)

/* Exact image from the same-boot proxy observation. No 14.x ABI alias. */
#define DCP_V26_J613_FIRMWARE_UUID "C042E95C-B9D8-3F0E-94B3-582A08AA6FDD"

#define DCP_V26_CPU_CONTROL	0x44
#define DCP_V26_CPU_STATUS	0x48
#define DCP_V26_MAX_PROPERTIES	256
#define DCP_V26_MAX_RAW		32
#define DCP_V26_MAX_RAW_BYTES	SZ_8M
#define DCP_V26_MAX_BUFFERS	64
#define DCP_V26_MAX_BUFFER	SZ_16M
#define DCP_V26_MAX_BUFFER_BYTES SZ_64M
#define DCP_V26_MAX_PUBLISHED	128
/* IOMFB layer of the primary plane. */
#define DCP_V26_LAYER		0
#define DCP_V26_BLACK		0xff000000
/* Swaps kept for the present history, and the swap-complete payload size. */
#define DCP_V26_HISTORY		256
#define DCP_V26_D590_SIZE	0x730

/* SDR user maximum of the J613/25G83 laptop panels; the DT gives the panel's own. */
/* Reported until the first request; the firmware keeps its own until then. */
#define DCP_V26_DEFAULT_NITS	140
/* How long a brightness request waits for a flip to carry it. */
#define DCP_V26_BL_FLIP_WAIT	msecs_to_jiffies(25)

/* Fail closed outside the single qualified board and firmware identity. */
struct dcp_v26_board {
	/* In log messages. */
	const char *name;
	const char *dcp_compatible;
	/* The machine compatible; NULL for any machine with this DCP. */
	const char *machine;
	/* The firmware image whose layouts were checked for this board. */
	const char *firmware_uuid;
	/* Set to 1 by the boot loader on the DCP, display and PIODMA nodes. */
	const char *handoff;
	/* Backlight maximum in nits; the DT maximum is capped to it. */
	u32 max_nits;
	/* Native panel size, notch rows included; 0: any, from the boot framebuffer. */
	u32 panel_width, panel_height;
	/* The panel may have a 120 Hz variable-refresh (ProMotion) timing. */
	bool promotion;
	/* If set and true, start without apple,pmp-report (not measured). */
	const bool *without_pmp;
};

static const struct dcp_v26_board dcp_v26_board_j613 = {
	.name = "J613/25G83",
	.dcp_compatible = "apple,t8122-dcp",
	.machine = "apple,j613",
	.firmware_uuid = DCP_V26_J613_FIRMWARE_UUID,
	.handoff = "apple,j613-25g83-mapping-handoff",
	.max_nits = 525,
	.panel_width = 2560,
	.panel_height = 1664,
};
static const struct dcp_v26_board *const dcp_v26_boards[] = {
	&dcp_v26_board_j613,
};

struct dcp_v26_property {
	u32 service;
	char key[64];
	u64 value;
};

struct dcp_v26_raw {
	u32 service;
	char key[64];
	void *data;
	u32 size;
};

/* One swap's timeline, CLOCK_MONOTONIC; 0 where it did not get that far. */
struct dcp_v26_present_record {
	ktime_t start;		/* swap start sent */
	ktime_t submitted;	/* swap start answered, swap sent */
	ktime_t replied;	/* swap reply received */
	ktime_t completed;	/* swap-complete message received */
	u32 id;
	u8 surface;
	u8 timed;
};

/* The last publication of a runtime property: its id, then 12 bytes. */
struct dcp_v26_published {
	u32 id;
	u8 data[12];
	u64 count;
};

struct dcp_v26_buffer {
	void *cpu;
	dma_addr_t iova;
	phys_addr_t phys;
	size_t size;
	bool retired;
	bool piodma_mapped;
};

struct apple_dcp_v26 {
	struct device *dev;
	const struct dcp_v26_board *board;
	/* Cleared when KMS unbinds; the firmware session outlives it. */
	struct apple_dcp *dcp;
	struct apple_rtkit *rtk;
	struct dcp_v26_link link;

	/* Owns the RPC stream: start, swaps and idle callbacks. */
	struct mutex lock;
	bool ctm_valid;
	u64 ctm_calls;
	u32 ctm_status, ctm_get_status;
	u64 ctm_readback[9];
	struct work_struct idle_work;
	bool failed;
	bool started;

	/* Boot framebuffer and native panel timing (notch rows included). */
	u32 stride;
	u32 fb_width, fb_height;
	u32 panel_width, panel_height;
	/* The panel's notch rows (apple,notch-height), hidden or not. */
	u32 notch_rows;
	u64 clock_rate;
	/* The mode is a variable-refresh (ProMotion) timing. */
	bool vrr;
	/*
	 * Set by the modeset: the CRTC shows the 120 Hz ProMotion mode, whose
	 * swaps carry timing words, and its refresh (for debugfs).
	 */
	bool timed;
	u32 refresh;

	struct dcp_v26_property properties[DCP_V26_MAX_PROPERTIES];
	u32 property_count;
	struct dcp_v26_raw raw[DCP_V26_MAX_RAW];
	u32 raw_count, raw_bytes;
	void *chunk;
	u32 chunk_size, chunk_offset;
	u64 analytics;
	u8 analytics_last[4164];
	struct dcp_v26_published published[DCP_V26_MAX_PUBLISHED];
	u32 published_count;
	u64 publications;

	struct dcp_v26_buffer buffers[DCP_V26_MAX_BUFFERS];
	u32 buffer_count;
	u64 buffer_bytes;
	struct platform_device *piodma;
	struct iommu_domain *piodma_domain;

	/* Scanned out until the next swap completes. */
	struct drm_framebuffer *active_fb;
	u64 swaps;
	u64 swap_ns_max;

	/* The last swaps and the last two swap-complete payloads, for debugfs. */
	struct dcp_v26_present_record history[DCP_V26_HISTORY];
	u64 history_count;
	u8 d590[2][DCP_V26_D590_SIZE];
	ktime_t d590_time[2];
	u64 d590_count;

	/* Panel brightness, carried by swaps. Under the lock. */
	u32 bl_nits;		/* requested */
	u32 bl_sent;		/* carried by the last brightness swap */
	u64 bl_seq;		/* bumped by every change of the target */
	u64 bl_swaps;
	bool bl_owned;		/* requested at least once: the driver sets it */
	bool bl_off;		/* the CRTC is off: the target is 0 */
	bool bl_pending;	/* the target has not been carried yet */
	bool bl_queued;		/* the backlight device is being registered */
	wait_queue_head_t bl_wait;

	/* The backlight device, registered after the first frame. */
	struct mutex bl_lock;
	struct work_struct bl_register_work;
	struct backlight_device *bl_dev;
	bool bl_detached;
	u32 bl_max;
};

/* One DCP session per boot: RTKit is never started twice. */
static bool dcp_v26_session;

static bool j613_25g83_swap_timestamps = true;
module_param(j613_25g83_swap_timestamps, bool, 0644);
MODULE_PARM_DESC(j613_25g83_swap_timestamps,
		 "J613/25G83: time-stamp swaps on a 120 Hz variable-refresh panel so they are shown at 120 Hz, not 60 Hz (default: on)");

static unsigned int j613_25g83_refresh = 120;
module_param(j613_25g83_refresh, uint, 0444);
MODULE_PARM_DESC(j613_25g83_refresh,
		 "J613/25G83: preferred refresh of the built-in display: 120 (variable refresh, ProMotion) or 60 (locked); both modes stay available (default: 120)");

static bool j613_25g83_flip_timestamp = true;
module_param(j613_25g83_flip_timestamp, bool, 0644);
MODULE_PARM_DESC(j613_25g83_flip_timestamp,
		 "J613/25G83: stamp page-flip events with the arrival of the firmware's swap-complete message; off: when the flush ends, less 1 ms (default: on)");

static int dcp_v26_callback(void *cookie, u32 tag, const void *input, u32 in_size,
			    void *output, u32 out_size);
static void dcp_v26_backlight_register(struct work_struct *work);
static void dcp_v26_backlight_detach(struct apple_dcp_v26 *v26);

/* Called with the lock held, or from a callback. */
static int dcp_v26_call(struct apple_dcp_v26 *v26, u32 tag, const void *in, u32 in_size,
			void *out, u32 out_size, u32 completion)
{
	int ret;

	if (v26->failed)
		return -EIO;
	dev_dbg(v26->dev, "call %#x in %u out %u\n", tag, in_size, out_size);
	ret = dcp_v26_link_call(&v26->link, tag, in, in_size, out, out_size, completion,
				dcp_v26_callback, v26);
	if (ret) {
		v26->failed = true;
		dev_err(v26->dev, "DCP call %#x failed: %d; recovery requires a reboot\n",
			tag, ret);
	}
	return ret;
}

/* A call with an optional u32 input of 1 and an optional u32 result to check. */
static int dcp_v26_simple_call(struct apple_dcp_v26 *v26, u32 tag, bool input, bool reply,
			       bool expected)
{
	__le32 one = cpu_to_le32(1), result = 0;
	int ret;

	ret = dcp_v26_call(v26, tag, input ? &one : NULL, input ? 4 : 0,
			   reply ? &result : NULL, reply ? 4 : 0, 0);
	if (!ret && reply && le32_to_cpu(result) != expected)
		return -EPROTO;
	return ret;
}

static struct dcp_v26_property *dcp_v26_property(struct apple_dcp_v26 *v26, u32 service,
						 const u8 *key, bool create)
{
	struct dcp_v26_property *p;
	u32 i;

	if (!memchr(key, 0, 64))
		return ERR_PTR(-EINVAL);
	for (i = 0; i < v26->property_count; i++) {
		p = &v26->properties[i];
		if (p->service == service && !strcmp(p->key, key))
			return p;
	}
	if (!create)
		return NULL;
	if (v26->property_count == DCP_V26_MAX_PROPERTIES)
		return ERR_PTR(-ENOSPC);
	p = &v26->properties[v26->property_count++];
	p->service = service;
	strscpy(p->key, key, sizeof(p->key));
	return p;
}

static int dcp_v26_raw_property(struct apple_dcp_v26 *v26, u32 service, const u8 *key,
				const void *data, u32 size)
{
	u32 i, old_size = 0;
	void *copy;

	if (!memchr(key, 0, 64) || size > SZ_1M)
		return -EINVAL;
	for (i = 0; i < v26->raw_count; i++)
		if (v26->raw[i].service == service && !strcmp(v26->raw[i].key, key))
			break;
	if (i == DCP_V26_MAX_RAW)
		return -ENOSPC;
	if (i < v26->raw_count)
		old_size = v26->raw[i].size;
	if (v26->raw_bytes - old_size + size > DCP_V26_MAX_RAW_BYTES)
		return -ENOSPC;
	copy = kvmemdup(data, size ?: 1, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	if (i == v26->raw_count)
		v26->raw_count++;
	kvfree(v26->raw[i].data);
	v26->raw[i].service = service;
	strscpy(v26->raw[i].key, key, sizeof(v26->raw[i].key));
	v26->raw[i].data = copy;
	v26->raw[i].size = size;
	v26->raw_bytes += size - old_size;
	return 0;
}

/* The firmware's allocate-buffer callback wants a physically contiguous buffer. */
static int dcp_v26_alloc(struct apple_dcp_v26 *v26, u64 request, u32 *id)
{
	struct dcp_v26_buffer *buf;
	struct sg_table table;
	dma_addr_t iova;
	size_t size;
	void *cpu;
	int ret;

	if (!request || request > DCP_V26_MAX_BUFFER)
		return -EINVAL;
	size = ALIGN(request, SZ_16K);
	if (v26->buffer_count == DCP_V26_MAX_BUFFERS ||
	    v26->buffer_bytes + size > DCP_V26_MAX_BUFFER_BYTES)
		return -ENOSPC;
	cpu = dma_alloc_attrs(v26->dev, size, &iova, GFP_KERNEL, DMA_ATTR_FORCE_CONTIGUOUS);
	if (!cpu)
		return -ENOMEM;
	ret = dma_get_sgtable_attrs(v26->dev, &table, cpu, iova, size,
				    DMA_ATTR_FORCE_CONTIGUOUS);
	if (ret)
		goto free;
	if (table.orig_nents != 1 || table.sgl->length < size) {
		sg_free_table(&table);
		ret = -ERANGE;
		goto free;
	}
	buf = &v26->buffers[v26->buffer_count];
	buf->phys = sg_phys(table.sgl);
	sg_free_table(&table);
	memset(cpu, 0, size);
	dma_wmb();
	buf->cpu = cpu;
	buf->iova = iova;
	buf->size = size;
	v26->buffer_bytes += size;
	/* The firmware takes id 0 as a failure. */
	*id = ++v26->buffer_count;
	dev_info(v26->dev, "DCP buffer %u: %zu bytes at %pad\n", *id, size, &iova);
	return 0;
free:
	dma_free_attrs(v26->dev, size, cpu, iova, DMA_ATTR_FORCE_CONTIGUOUS);
	return ret;
}

/* Maps a firmware buffer at the same address for the PIODMA stream. */
static int dcp_v26_map_piodma(struct apple_dcp_v26 *v26, u32 id)
{
	struct dcp_v26_buffer *buf = &v26->buffers[id - 1];
	struct device_node *node;
	size_t offset;
	u32 marker;
	int ret;

	if (buf->retired)
		return -EINVAL;
	if (buf->piodma_mapped)
		return 0;
	if (!v26->piodma) {
		node = of_get_child_by_name(v26->dev->of_node, "piodma");
		if (!node || !of_device_is_available(node) ||
		    of_property_read_u32(node, v26->board->handoff, &marker) || marker != 1) {
			of_node_put(node);
			return -ENODEV;
		}
		v26->piodma = of_platform_device_create(node, NULL, v26->dev);
		if (!v26->piodma) {
			of_node_put(node);
			return -ENOMEM;
		}
		ret = dma_set_mask_and_coherent(&v26->piodma->dev, DMA_BIT_MASK(42));
		if (!ret)
			ret = of_dma_configure(&v26->piodma->dev, node, true);
		of_node_put(node);
		/* Never destroy a device attached to a locked DART stream. */
		if (ret)
			return ret;
		v26->piodma_domain = iommu_get_domain_for_dev(&v26->piodma->dev);
	}
	if (IS_ERR_OR_NULL(v26->piodma_domain))
		return -EIO;
	ret = iommu_map(v26->piodma_domain, buf->iova, buf->phys, buf->size,
			IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret)
		return ret;
	for (offset = 0; offset < buf->size; offset += SZ_16K)
		if (iommu_iova_to_phys(v26->piodma_domain, buf->iova + offset) !=
		    buf->phys + offset)
			return -EIO;
	buf->piodma_mapped = true;
	dev_info(v26->dev, "PIODMA mapped DCP buffer %u: %zu bytes at %pad\n",
		 id, buf->size, &buf->iova);
	return 0;
}

/*
 * Keeps the last publication of each runtime property, keyed by its first
 * word (the property id). The M1/M2 firmware reports the brightness it
 * applies this way; kept here, it can be compared with brightness requests
 * on a running system before anything relies on its layout.
 */
static void dcp_v26_publish(struct apple_dcp_v26 *v26, const u8 *in)
{
	u32 id = get_unaligned_le32(in), i;

	v26->publications++;
	for (i = 0; i < v26->published_count; i++)
		if (v26->published[i].id == id)
			break;
	if (i == DCP_V26_MAX_PUBLISHED)
		return;
	if (i == v26->published_count)
		v26->published_count++;
	v26->published[i].id = id;
	memcpy(v26->published[i].data, in + 4, sizeof(v26->published[i].data));
	v26->published[i].count++;
	dev_dbg(v26->dev, "published %u: %*ph\n", id, 12, in + 4);
}

/* Runs on the thread that owns the RPC stream; the lock is held. */
static int dcp_v26_callback(void *cookie, u32 tag,
		    const void *input, u32 in_size, void *output, u32 out_size)
{
	struct apple_dcp_v26 *v26 = cookie;
	const u8 *in = input;
	u8 *out = output;
	struct dcp_v26_property *p;
	u32 id;
	u32 count, offset, service, key_offset;
	int ret;

#define SHAPE(i, o) (in_size == (i) && out_size == (o))
	/* The transport zeroes output, so absent optional metadata stays absent. */
	if (tag == D(114) && SHAPE(4164, 4100)) {
		/* J613 CoreAnalyticsSendEvent: name[64], serialized dictionary[4096],
		 * nullable flag. Retain the latest event locally. The dictionary is
		 * in/out: firmware parses it even on failure, so a zero-filled reply
		 * is invalid. Return its own serialized dictionary without changes.
		 * The final u32 is the method status (zero = accepted).
		 */
		if (!memchr(in, 0, 64) || in[4160] > 1 ||
		    (!in[4160] && in[64] != 'd'))
			return -EINVAL;
		memcpy(v26->analytics_last, in, sizeof(v26->analytics_last));
		v26->analytics++;
		if (!in[4160])
			memcpy(out, in + 64, 4096);
		put_unaligned_le32(0, out + 4096);
		dev_info(v26->dev, "J613 analytics event %llu: %.64s (retained locally)\n",
			 v26->analytics, in);
		return 0;
	}
	if (tag == D(590) && SHAPE(0x730, 0)) {
		id = v26->d590_count++ % ARRAY_SIZE(v26->d590);
		memcpy(v26->d590[id], in, DCP_V26_D590_SIZE);
		v26->d590_time[id] = v26->link.callback_time;
		return 0;
	}
	if (tag == D(588) && SHAPE(0, 0))
		return 0;
	if (tag == D(599) && SHAPE(0, 0))
		return dcp_v26_simple_call(v26, A(412), false, true, true);
	if ((tag == D(108) || tag == D(109) || tag == D(110) || tag == D(111) ||
	     tag == D(112) || tag == D(113) || tag == D(0) || tag == D(1)) && SHAPE(0, 4)) {
		out[0] = 1;
		return 0;
	}
	if (tag == D(575) && SHAPE(88, 76)) {
		if (!(in[84] & 1))
			memcpy(out, in + 8, 76);
		return 0;
	}
	if (tag == D(576) && SHAPE(4, 0))
		return 0;
	if (tag == D(300) && SHAPE(16, 0)) {
		dcp_v26_publish(v26, in);
		return 0;
	}
	if (tag == D(127) && SHAPE(4, 4)) {
		count = get_unaligned_le32(in);
		if (v26->chunk || !count || count > 0x100001)
			return -EINVAL;
		v26->chunk_size = count - 1;
		v26->chunk_offset = 0;
		v26->chunk = kvzalloc(max_t(u32, 1, v26->chunk_size), GFP_KERNEL);
		if (!v26->chunk)
			return -ENOMEM;
		out[0] = 1;
		return 0;
	}
	if (tag == D(128) && SHAPE(0x1008, 4)) {
		offset = get_unaligned_le32(in + 0x1000);
		count = get_unaligned_le32(in + 0x1004);
		if (!v26->chunk || offset != v26->chunk_offset || count > 4096 ||
		    offset > v26->chunk_size || count > v26->chunk_size - offset)
			return -EINVAL;
		memcpy(v26->chunk + offset, in, count);
		v26->chunk_offset += count;
		out[0] = 1;
		return 0;
	}
	if (tag == D(129) && SHAPE(64, 4)) {
		if (!v26->chunk || v26->chunk_offset != v26->chunk_size || !memchr(in, 0, 64) ||
		    v26->raw_count == DCP_V26_MAX_RAW || v26->raw_bytes + v26->chunk_size > SZ_8M)
			return -EINVAL;
		ret = dcp_v26_raw_property(v26, 0, in, v26->chunk, v26->chunk_size);
		if (ret)
			return ret;
		kvfree(v26->chunk);
		v26->chunk = NULL;
		dev_info(v26->dev, "J613 property %.64s: %u bytes\n", in, v26->chunk_size);
		out[0] = 1;
		return 0;
	}
	if (tag == D(408) && SHAPE(8, 8)) {
		count = get_unaligned_le32(in + 4);
		if (memcmp(in, "VORP", 4) || count > 1)
			return -EINVAL;
		/* Same-boot ADT clock observation, supplied by the guarded runner. */
		put_unaligned_le64(count ? 0 : v26->clock_rate, out);
		return 0;
	}
	if (tag == D(125) && SHAPE(100, 36)) {
		count = get_unaligned_le32(in + 64);
		if (count > 8)
			return -EINVAL;
		memcpy(out, in + 68, count * 4);
		return 0;
	}
	if (tag == D(6) && SHAPE(84, 80)) {
		if (!(in[80] & 1))
			memcpy(out, in, 80);
		return 0;
	}
	if ((tag == D(122) || tag == D(123)) && SHAPE(0, 4))
		return 0;
	if (tag == D(121) && SHAPE(0, 4)) {
		static const struct { u32 tag; bool in, reply, expected; } sequence[] = {
			{A(389)}, {A(446), false, true, true}, {A(29)},
			{A(468), true}, {A(0), true, true, true}, {A(465), false, true, true},
		};
		for (count = 0; count < ARRAY_SIZE(sequence); count++) {
			ret = dcp_v26_simple_call(v26, sequence[count].tag, sequence[count].in,
					  sequence[count].reply, sequence[count].expected);
			if (ret)
				return ret;
		}
		out[0] = 1;
		return 0;
	}
	if (tag == D(101) && SHAPE(0, 4)) {
		put_unaligned_le32(v26->stride, out);
		return 0;
	}
	if (tag == D(3) && SHAPE(4, 60)) {
		put_unaligned_le32(1, out + 56); /* Explicitly decline bandwidth service. */
		return 0;
	}
	if (tag == D(451) && SHAPE(20, 28)) {
		u32 alignment = get_unaligned_le32(in + 12);
		u64 size = get_unaligned_le64(in + 4);
		struct dcp_v26_buffer *buf;

		dev_dbg(v26->dev, "allocation flags %#x size %llu alignment %u\n",
			get_unaligned_le32(in), size, alignment);
		if (get_unaligned_le32(in) != 0x703 || !is_power_of_2(alignment) ||
		    alignment > SZ_16K || in[16] || in[17] || in[18])
			return -EINVAL;
		ret = dcp_v26_alloc(v26, size, &id);
		if (ret)
			return ret;
		buf = &v26->buffers[id - 1];
		if ((buf->phys | buf->iova) & (alignment - 1))
			return -EINVAL;
		put_unaligned_le64(buf->phys, out);
		put_unaligned_le64(buf->iova, out + 8);
		put_unaligned_le64(buf->size, out + 16);
		put_unaligned_le32(id, out + 24);
		return 0;
	}
	/* Map a buffer for PIODMA; the AP virtual address is not used. */
	if (tag == D(201) && SHAPE(12, 16)) {
		u64 buffer = get_unaligned_le64(in);

		if (!buffer || buffer > v26->buffer_count || get_unaligned_le32(in + 8) != 1)
			return -EINVAL;
		ret = dcp_v26_map_piodma(v26, buffer);
		if (ret)
			return ret;
		put_unaligned_le32(buffer, out);
		put_unaligned_le64(v26->buffers[buffer - 1].iova, out + 4);
		return 0;
	}
	/* Release: the buffer stays allocated and mapped until reboot. */
	if (tag == D(454) && SHAPE(4, 4)) {
		id = get_unaligned_le32(in);
		if (!id || id > v26->buffer_count || v26->buffers[id - 1].retired)
			return -EINVAL;
		v26->buffers[id - 1].retired = true;
		out[0] = 1;
		return 0;
	}
	if (tag == D(582) && SHAPE(8, 4))
		return 0; /* Optional default surface is not allocated by this host. */
	if (tag == D(400) && SHAPE(76, 0xc04)) {
		count = get_unaligned_le32(in + 68);
		service = get_unaligned_le32(in);
		if (count > 0xc00 || (in[72] & 1) || !memchr(in + 4, 0, 64))
			return -EINVAL;
		for (offset = 0; offset < v26->raw_count; offset++) {
			if (v26->raw[offset].service != service || strcmp(v26->raw[offset].key, in + 4))
				continue;
			if (v26->raw[offset].size > count)
				return -ENOSPC;
			memcpy(out, v26->raw[offset].data, v26->raw[offset].size);
			put_unaligned_le32(v26->raw[offset].size, out + 0xc00);
			break;
		}
		return 0;
	}
	if (tag == D(401) && SHAPE(80, 12)) {
		p = dcp_v26_property(v26, get_unaligned_le32(in), in + 4, false);
		if (IS_ERR(p))
			return PTR_ERR(p);
		if (p) {
			put_unaligned_le64(p->value, out);
			out[8] = 1;
		}
		return 0;
	}
	if ((tag == D(413) && SHAPE(4168, 4)) ||
	    ((tag == D(552) || tag == D(561)) && SHAPE(4164, 4)) ||
	    (tag == D(567) && SHAPE(128, 4))) {
		key_offset = tag == D(413) ? 4 : 0;
		service = key_offset ? get_unaligned_le32(in) : 0;
		if (tag != D(567) && (in[key_offset + 4160] & 1)) {
			out[0] = 1;
			return 0;
		}
		count = tag == D(567) ? strnlen(in + 64, 64) : 4096;
		ret = dcp_v26_raw_property(v26, service, in + key_offset, in + key_offset + 64, count);
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if ((tag == D(563) && SHAPE(76, 4)) || (tag == D(565) && SHAPE(72, 4)) ||
	    (tag == D(414) && SHAPE(80, 4)) || (tag == D(415) && SHAPE(76, 4)) ||
	    ((tag == D(102) || tag == D(104)) && SHAPE(68, 0))) {
		key_offset = (tag == D(414) || tag == D(415)) ? 4 : 0;
		service = key_offset ? get_unaligned_le32(in) : 0;
		count = (tag == D(563) || tag == D(414)) ? 8 : 4;
		if (out_size && (in[key_offset + 64 + count] & 1)) {
			out[0] = 1;
			return 0;
		}
		p = dcp_v26_property(v26, service, in + key_offset, true);
		if (IS_ERR(p))
			return PTR_ERR(p);
		p->value = count == 8 ? get_unaligned_le64(in + key_offset + 64) :
				       get_unaligned_le32(in + key_offset + 64);
		if (tag == D(104))
			p->value = in[64];
		if (out_size)
			out[0] = 1;
		return 0;
	}
	if (tag == D(406) && SHAPE(72, 0))
		return 0;
	if (tag == D(107) && SHAPE(64, 0)) {
		p = dcp_v26_property(v26, 0, in, false);
		if (IS_ERR(p))
			return PTR_ERR(p);
		if (p) {
			*p = v26->properties[--v26->property_count];
			memset(&v26->properties[v26->property_count], 0, sizeof(*p));
		}
		return 0;
	}
	if ((tag == D(206) || tag == D(207)) && SHAPE(0, 4)) {
		ret = dcp_v26_simple_call(v26, tag == D(206) ? A(131) : A(132), false, true, false);
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if (tag == D(100) && SHAPE(0, 0))
		return dcp_v26_simple_call(v26, A(390), false, true, false);
	dev_err(v26->dev, "J613 unhandled kernel callback %#x %u/%u\n", tag, in_size, out_size);
	return -EOPNOTSUPP;
#undef SHAPE
}

static void dcp_v26_idle(struct work_struct *work)
{
	struct apple_dcp_v26 *v26 = container_of(work, struct apple_dcp_v26, idle_work);
	int ret = 0;

	mutex_lock(&v26->lock);
	while (!v26->failed && !ret)
		ret = dcp_v26_link_pump(&v26->link, 0, dcp_v26_callback, v26);
	if (ret && ret != -ETIMEDOUT && ret != -EAGAIN) {
		v26->failed = true;
		dev_err(v26->dev, "DCP notification failed: %d; recovery requires a reboot\n", ret);
	}
	mutex_unlock(&v26->lock);
}

static void dcp_v26_crashed(void *cookie, const void *crashlog, size_t crashlog_size)
{
	struct apple_dcp_v26 *v26 = cookie;
	/* failed is session-owned; no dereference of a detachable KMS device. */
	WRITE_ONCE(v26->failed, true);
	dev_err(v26->dev, "DCP firmware crashed; its buffers are kept until reboot\n");
	dcp_v26_link_fail(&v26->link);
}

static void dcp_v26_recv(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp_v26 *v26 = cookie;

	if (endpoint != APPLE_DCP_LINK_ENDPOINT) {
		dev_dbg(v26->dev, "ignored endpoint %#x message %#llx\n", endpoint, message);
		return;
	}
	dcp_v26_link_receive(&v26->link, message);
}

/*
 * Buffers the firmware already owns must lie in a reserved region the boot
 * loader described, and be mapped linearly there. The OS log is a physical
 * address, not a DART address.
 */
static int dcp_v26_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp_v26 *v26 = cookie;
	struct device *dev = v26->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	phys_addr_t phys;
	int i;

	if (!READ_ONCE(v26->dcp) || !bfr->size || bfr->size > SZ_16M || !domain)
		return -EINVAL;
	if (!bfr->iova) {
		bfr->buffer = dma_alloc_coherent(dev, bfr->size, &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;
		memset(bfr->buffer, 0, bfr->size);
		dma_wmb();
		return 0;
	}
	phys = iommu_iova_to_phys(domain, bfr->iova);
	for (i = 0; ; i++) {
		struct device_node *node = of_parse_phandle(dev->of_node, "memory-region", i);
		struct resource res;
		size_t off;
		bool oslog, valid;

		if (!node)
			return -ERANGE;
		oslog = of_property_read_bool(node, "apple,dcp-os-log");
		valid = !of_address_to_resource(node, 0, &res);
		of_node_put(node);
		if (!valid)
			continue;
		if (oslog && bfr->iova == res.start)
			phys = res.start;
		if (phys < res.start || phys > res.end || bfr->size - 1 > res.end - phys)
			continue;
		if (!oslog) {
			for (off = 0; off < bfr->size; off += SZ_16K)
				if (iommu_iova_to_phys(domain, bfr->iova + off) != phys + off)
					return -ERANGE;
			if (iommu_iova_to_phys(domain, bfr->iova + bfr->size - 1) !=
			    phys + bfr->size - 1)
				return -ERANGE;
		}
		bfr->buffer = memremap(phys, bfr->size, MEMREMAP_WB);
		if (!bfr->buffer)
			return -ENOMEM;
		bfr->is_mapped = true;
		dev_dbg(dev, "RTKit buffer at %pad: %pa, %zu bytes\n", &bfr->iova, &phys,
			bfr->size);
		return 0;
	}
}

static void dcp_v26_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp_v26 *v26 = cookie;

	if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(v26->dev, bfr->size, bfr->buffer, bfr->iova);
}

static const struct apple_rtkit_ops dcp_v26_rtkit_ops = {
	.crashed = dcp_v26_crashed,
	.recv_message = dcp_v26_recv,
	.shmem_setup = dcp_v26_shmem_setup,
	.shmem_destroy = dcp_v26_shmem_destroy,
};

/* Freed only if RTKit never started: the firmware may call into it until reboot. */
static void dcp_v26_release(void *data)
{
	struct apple_dcp_v26 *v26 = data;

	if (!v26->rtk) {
		put_device(v26->dev);
		kfree(v26);
	}
}

/* Read only: bind maps and claims these registers. */
static int dcp_v26_cpu_running(struct device *dev, const char *name)
{
	struct resource *res;
	void __iomem *coproc;
	u32 control;

	res = platform_get_resource_byname(to_platform_device(dev), IORESOURCE_MEM, "coproc");
	if (!res)
		return -ENODEV;
	coproc = ioremap_np(res->start, resource_size(res));
	if (!coproc)
		return -ENOMEM;
	control = readl(coproc + DCP_V26_CPU_CONTROL);
	iounmap(coproc);
	if (!(control & APPLE_DCP_COPROC_CPU_CONTROL_RUN))
		return dev_err_probe(dev, -EBUSY,
				     "%s display not started: the DCP CPU is stopped (%#x); the PMP is not started for it\n",
				     name, control);
	return 0;
}

/*
 * The board of a 14.x IOMFB DCP node: NULL if the node is not one, an error
 * if it is but the machine is not a board this file knows.
 */
const struct dcp_v26_board *iomfb_v26_6_board(struct device *dev)
{
	const struct dcp_v26_board *board;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dcp_v26_boards); i++) {
		board = dcp_v26_boards[i];
		if (!of_device_is_compatible(dev->of_node, board->dcp_compatible))
			continue;
		if (board->machine && !of_machine_is_compatible(board->machine)) {
			dev_err(dev, "%s display not started: this machine is not a %s\n",
				board->name, board->machine);
			return ERR_PTR(-ENODEV);
		}
		return board;
	}
	return NULL;
}

const char *iomfb_v26_6_board_name(const struct dcp_v26_board *board)
{
	return board->name;
}

static int keep_display_power(struct device *dev)
{
	struct device_node *node;
	const u8 *table;
	int length, ret = -EINVAL;

	/* The power API is exported before the firmware supplier has probed.
	 * Link to the DT supplier first so coldplug defers and removal is ordered. */
	ret = apple_pmp_link_device(dev);
	if (ret)
		return ret;
	node = of_parse_phandle(dev->of_node, "apple,pmp", 0);
	if (!node)
		return -ENODEV;
	table = of_get_property(node, "apple,tunable-soc-device", &length);
	/* J613/25G83: logical ID 8, record/bitmap index 11, name DISP. */
	if (table && length == 20 * 0x7c &&
	    get_unaligned_le32(table + 11 * 0x7c) == 8 &&
	    !memcmp(table + 11 * 0x7c + 0x74, "DISP\0\0\0\0", 8))
		ret = apple_pmp_set_device_power(0x0f, 8, 1);
	of_node_put(node);
	if (ret)
		return ret;
	/* G52's synchronous PMP API checks the command acknowledgement. That
	 * does not qualify domain stability; retain the vote until reboot. */
	dev_info(dev, "J613 native DISP power vote acknowledged: logical-id=8 record=11\n");
	return 0;
}

int iomfb_v26_6_probe(struct apple_dcp *dcp)
{
	struct device *dev = dcp->dev;
	struct device_node *np = dev->of_node;
	const struct dcp_v26_board *board = iomfb_v26_6_board(dev);
	struct apple_dcp_v26 *v26;
	const char *uuid = NULL;
	u32 profile, markers[3];
	struct device_node *display, *piodma;
	int ret;
	u32 version[3];

	if (of_property_count_u32_elems(np, "apple,j613-25g83-profile") != 1 ||
	    of_property_read_u32(np, "apple,j613-25g83-profile", &profile) ||
	    of_property_count_u32_elems(np, "apple,firmware-compat") != 3 ||
	    of_property_read_u32_array(np, "apple,firmware-compat", version, 3) ||
	    of_property_read_string(np, "apple,firmware-uuid", &uuid) ||
	    !j613_25g83_identity(of_machine_is_compatible("apple,j613"), profile, version, 3, uuid))
		return -ENODEV;
	if (IS_ERR_OR_NULL(board))
		return -ENODEV;
	if (of_property_read_string(np, "apple,firmware-uuid", &uuid) ||
	    strcmp(uuid, board->firmware_uuid))
		return dev_err_probe(dev, -ENODEV,
				     "%s display not started: DCP firmware %s is not supported\n",
				     board->name, uuid ?: "(unknown)");
	display = of_find_node_by_path("/soc/display-subsystem");
	piodma = of_get_child_by_name(np, "piodma");
	ret = of_property_count_u32_elems(np, board->handoff) == 1 &&
		!of_property_read_u32(np, board->handoff, &markers[0]) &&
		display && of_property_count_u32_elems(display, board->handoff) == 1 &&
		!of_property_read_u32(display, board->handoff, &markers[1]) &&
		piodma && of_property_count_u32_elems(piodma, board->handoff) == 1 &&
		!of_property_read_u32(piodma, board->handoff, &markers[2]) &&
		j613_25g83_mappings(markers, 3) && apple_j613_25g83_clock_hz() ? 0 : -ENODEV;
	of_node_put(display);
	of_node_put(piodma);
	if (ret)
		return dev_err_probe(dev, ret, "J613/25G83 display mapping or clock handoff missing\n");
	if (!iommu_get_domain_for_dev(dev))
		return dev_err_probe(dev, -ENODEV,
				     "%s display not started: the DCP has no DART domain\n",
				     board->name);
	ret = dcp_v26_cpu_running(dev, board->name);
	if (ret)
		return ret;

	ret = keep_display_power(dev);
	if (ret)
		return dev_err_probe(dev, ret, "J613/25G83 DISP power vote failed\n");
	ret = dcp_v26_cpu_running(dev, board->name);
	if (ret)
		return ret;

	v26 = kzalloc_obj(*v26);
	if (!v26)
		return -ENOMEM;
	v26->dev = get_device(dev);
	v26->board = board;
	v26->dcp = dcp;
	mutex_init(&v26->lock);
	INIT_WORK(&v26->idle_work, dcp_v26_idle);
	init_waitqueue_head(&v26->bl_wait);
	mutex_init(&v26->bl_lock);
	INIT_WORK(&v26->bl_register_work, dcp_v26_backlight_register);
	ret = devm_add_action_or_reset(dev, dcp_v26_release, v26);
	if (ret)
		return ret;
	dcp->v26 = v26;
	dev_info(dev, "PMP running with the display request acknowledged\n");
	return 0;
}

/*
 * The boot framebuffer gives the stride and the panel size. The boot loader either hides
 * the notch rows from it (the default) or, with chosen.asahi,show-notch=1 in m1n1, keeps
 * them; apple,notch-height is recorded either way. A board with a known panel tells the
 * two apart by the height; any other board is taken to have the notch hidden.
 */
static int dcp_v26_geometry(struct apple_dcp_v26 *v26)
{
	struct device_node *fb = of_find_compatible_node(NULL, NULL, "simple-framebuffer");
	u32 notch = 0;
	int ret;

	if (!fb)
		return -ENODEV;
	ret = of_property_read_u32(fb, "width", &v26->fb_width);
	if (!ret)
		ret = of_property_read_u32(fb, "height", &v26->fb_height);
	if (!ret)
		ret = of_property_read_u32(fb, "stride", &v26->stride);
	of_node_put(fb);
	if (ret)
		return ret;
	of_property_read_u32(v26->dev->of_node, "apple,notch-height", &notch);
	/* The firmware takes this stride as its default: 4 bytes per pixel. */
	if (!v26->fb_width || !v26->fb_height || v26->stride != v26->fb_width * 4 ||
	    notch > MAX_NOTCH_HEIGHT)
		return -EINVAL;
	v26->notch_rows = notch;
	v26->panel_width = v26->fb_width;
	v26->panel_height = v26->fb_height + notch;
	/* The boot loader kept the notch rows: the framebuffer is the whole panel. */
	if (v26->board->panel_height && notch && v26->fb_height == v26->board->panel_height)
		v26->panel_height = v26->fb_height;
	/* A board with a known panel takes no other. */
	if (v26->board->panel_width &&
	    (v26->panel_width != v26->board->panel_width ||
	     v26->panel_height != v26->board->panel_height)) {
		dev_err(v26->dev, "boot framebuffer %ux%u with %u notch rows is not the %ux%u %s panel\n",
			v26->fb_width, v26->fb_height, notch, v26->board->panel_width,
			v26->board->panel_height, v26->board->name);
		return -EINVAL;
	}
	return 0;
}

int iomfb_v26_6_bind(struct apple_dcp *dcp)
{
	struct apple_dcp_v26 *v26 = dcp->v26;
	struct device *dev = dcp->dev;
	struct apple_rtkit *rtk;
	u32 control, status;
	int ret;

	if (!v26)
		return -ENODEV;
	if (v26->rtk || dcp_v26_session)
		return dev_err_probe(dev, -EBUSY,
				     "%s display not started: an earlier DCP session is kept; reboot to restart the display\n",
				     v26->board->name);

	control = readl(dcp->coproc_reg + DCP_V26_CPU_CONTROL);
	status = readl(dcp->coproc_reg + DCP_V26_CPU_STATUS);
	dev_info(dev, "DCP CPU control %#x status %#x\n", control, status);
	if (!(control & APPLE_DCP_COPROC_CPU_CONTROL_RUN))
		return dev_err_probe(dev, -EBUSY,
				     "%s display not started: the DCP CPU is stopped, and interrupted firmware is never resumed\n",
				     v26->board->name);

	ret = dcp_v26_geometry(v26);
	if (ret)
		return dev_err_probe(dev, ret,
				     "%s display not started: no usable boot framebuffer\n",
				     v26->board->name);
	v26->clock_rate = apple_j613_25g83_clock_hz();
	if (!v26->clock_rate)
		return dev_err_probe(dev, -EINVAL, "J613/25G83 same-boot clock observation required\n");

	dcp_v26_link_init(&v26->link, dev, NULL);
	rtk = apple_rtkit_init(dev, v26, "mbox", 0, &dcp_v26_rtkit_ops);
	if (IS_ERR(rtk))
		return dev_err_probe(dev, PTR_ERR(rtk),
				     "%s display not started: RTKit init failed\n",
				     v26->board->name);
	/* From here nothing is freed and the module stays loaded. */
	v26->rtk = rtk;
	v26->link.rtk = rtk;
	dcp_v26_session = true;
	__module_get(THIS_MODULE);

	ret = apple_rtkit_wake(rtk);
	if (ret) {
		v26->failed = true;
		return dev_err_probe(dev, ret,
				     "%s display not started: the DCP RTKit session did not wake; the boot framebuffer stays\n",
				     v26->board->name);
	}
	dev_info(dev, "DCP RTKit session running\n");
	return 0;
}

void iomfb_v26_6_unbind(struct apple_dcp *dcp)
{
	struct apple_dcp_v26 *v26 = dcp->v26;

	dcp->active = false;
	dcp_mode_invalidate(&dcp->mode_state);
	if (!v26)
		return;
	dcp_v26_backlight_detach(v26);
	mutex_lock(&v26->lock);
	WRITE_ONCE(v26->dcp, NULL);
	mutex_unlock(&v26->lock);
	if (v26->rtk)
		dev_info(dcp->dev, "display unbound; the DCP session and its buffers are kept until reboot\n");
}

static int dcp_v26_status_show(struct seq_file *m, void *unused)
{
	struct apple_dcp_v26 *v26 = m->private;
	struct drm_framebuffer *fb;
	u32 i;
	int ret;

	ret = mutex_lock_interruptible(&v26->lock);
	if (ret)
		return ret;
	fb = v26->active_fb;
	seq_printf(m, "started %d\nfailed %d\nswaps %llu\nswap_max_us %llu\n",
		   v26->started, v26->failed, v26->swaps, v26->swap_ns_max / NSEC_PER_USEC);
	seq_printf(m, "active_fb %u\nimported %d\n", fb ? fb->base.id : 0,
		   fb && fb->obj[0]->import_attach);
	seq_printf(m, "panel %ux%u\nboot_fb %ux%u stride %u\nclock %llu\n",
		   v26->panel_width, v26->panel_height, v26->fb_width, v26->fb_height,
		   v26->stride, v26->clock_rate);
	seq_printf(m, "buffers %u bytes %llu\nproperties %u raw %u\nanalytics %llu\n",
		   v26->buffer_count, v26->buffer_bytes, v26->property_count, v26->raw_count,
		   v26->analytics);
	seq_printf(m, "publications %llu\n", v26->publications);
	for (i = 0; i < v26->published_count; i++)
		seq_printf(m, "published %u %*phN x%llu\n", v26->published[i].id,
			   (int)sizeof(v26->published[i].data), v26->published[i].data,
			   v26->published[i].count);
	seq_printf(m, "backlight %d max %u\nbrightness requested %u sent %u owned %d off %d pending %d swaps %llu\n",
		   !!READ_ONCE(v26->bl_dev), v26->bl_max, v26->bl_nits, v26->bl_sent,
		   v26->bl_owned, v26->bl_off, v26->bl_pending, v26->bl_swaps);
	seq_printf(m, "ctm valid %d calls %llu setter %#x getter %#x\n", v26->ctm_valid, v26->ctm_calls, v26->ctm_status, v26->ctm_get_status);
        for (i = 0; i < 9; i++)
                seq_printf(m, "ctm_readback[%u] %#llx\n", i, v26->ctm_readback[i]);
	mutex_unlock(&v26->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dcp_v26_status);

struct dcp_v26_present_snapshot {
	struct dcp_v26_present_record history[DCP_V26_HISTORY];
	u8 d590[2][DCP_V26_D590_SIZE];
	ktime_t d590_time[2];
	u64 history_count, d590_count;
	bool vrr;
};

static s64 dcp_v26_since(ktime_t t, ktime_t start)
{
	return t ? ktime_to_ns(ktime_sub(t, start)) : -1;
}

/*
 * The swap timeline, oldest first: times are ns after the swap start, -1
 * where the swap did not get that far. Then the last swap-complete payloads.
 */
static int dcp_v26_present_show(struct seq_file *m, void *unused)
{
	struct apple_dcp_v26 *v26 = m->private;
	struct dcp_v26_present_snapshot *snap;
	u64 first, i;
	int ret;

	snap = kvmalloc_obj(*snap);
	if (!snap)
		return -ENOMEM;
	ret = mutex_lock_interruptible(&v26->lock);
	if (ret) {
		kvfree(snap);
		return ret;
	}
	memcpy(snap->history, v26->history, sizeof(snap->history));
	memcpy(snap->d590, v26->d590, sizeof(snap->d590));
	memcpy(snap->d590_time, v26->d590_time, sizeof(snap->d590_time));
	snap->history_count = v26->history_count;
	snap->d590_count = v26->d590_count;
	snap->vrr = v26->vrr;
	mutex_unlock(&v26->lock);

	seq_printf(m, "vrr %d\nrefresh %u\ntimed %d\ntimestamps %d\nflip_timestamp %d\nswaps %llu\n",
		   snap->vrr, READ_ONCE(v26->refresh), READ_ONCE(v26->timed),
		   READ_ONCE(j613_25g83_swap_timestamps), READ_ONCE(j613_25g83_flip_timestamp),
		   snap->history_count);
	seq_puts(m, "# id surface timed start_ns submitted_ns replied_ns completed_ns\n");
	first = snap->history_count > DCP_V26_HISTORY ? snap->history_count - DCP_V26_HISTORY : 0;
	for (i = first; i < snap->history_count; i++) {
		struct dcp_v26_present_record *r = &snap->history[i % DCP_V26_HISTORY];

		seq_printf(m, "%u %u %u %lld %lld %lld %lld\n", r->id, r->surface, r->timed,
			   ktime_to_ns(r->start), dcp_v26_since(r->submitted, r->start),
			   dcp_v26_since(r->replied, r->start),
			   dcp_v26_since(r->completed, r->start));
	}
	first = snap->d590_count > 2 ? snap->d590_count - 2 : 0;
	for (i = first; i < snap->d590_count; i++) {
		seq_printf(m, "# swap-complete %llu received_ns %lld\n", i + 1,
			   ktime_to_ns(snap->d590_time[i % 2]));
		seq_hex_dump(m, "", DUMP_PREFIX_OFFSET, 16, 1, snap->d590[i % 2],
			     DCP_V26_D590_SIZE, false);
	}
	kvfree(snap);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dcp_v26_present);

/*
 * The modes: the native timing of the boot panel, less the hidden notch rows,
 * and on the 120 Hz ProMotion panel also a locked 60 Hz mode.
 *
 * The locked mode is the same firmware timing with half the pixel clock, so
 * it reports 60 Hz. The firmware is never asked for a timing (see
 * iomfb_v26_6_modeset()): what differs is only that its swaps carry no
 * timing words, and the firmware shows untimed swaps at 60 Hz. Switching
 * between the two is a modeset in DRM terms and nothing more for the panel.
 * appledrm.j613_25g83_refresh picks which one is preferred.
 */
static int dcp_v26_mode(struct apple_dcp *dcp, struct apple_dcp_v26 *v26)
{
	struct dcp_display_mode *modes, *best = NULL, *preferred, *selected;
	struct dcp_parse_ctx ctx;
	unsigned int count, i;
	void *blob = NULL;
	u32 size = 0;
	int ret;

	for (i = 0; i < v26->raw_count; i++) {
		if (!v26->raw[i].service && !strcmp(v26->raw[i].key, "PreferredTimingElements")) {
			size = v26->raw[i].size;
			blob = v26->raw[i].data;
			break;
		}
	}
	if (!blob)
		return -ENODATA;
	ret = parse(blob, size, &ctx);
	if (ret)
		return ret;
	ctx.dcp = dcp;
	modes = enumerate_modes(&ctx, &count, dcp->panel.width_mm, dcp->panel.height_mm,
				dcp->notch_height, true);
	if (IS_ERR(modes))
		return PTR_ERR(modes);
	for (i = 0; i < count; i++) {
		struct drm_display_mode *m = &modes[i].mode;
		int hz = drm_mode_vrefresh(m);

		dev_dbg(dcp->dev, "timing %u/%u: " DRM_MODE_FMT "%s\n", i + 1, count,
			DRM_MODE_ARG(m), m->type & DRM_MODE_TYPE_PREFERRED ? " best" : "");
		if (m->hdisplay != v26->panel_width ||
		    m->vdisplay != v26->panel_height - dcp->notch_height ||
		    (hz != 60 && (hz != 120 || !v26->board->promotion)))
			continue;
		if (!best || (m->type & DRM_MODE_TYPE_PREFERRED))
			best = &modes[i];
	}
	if (!best) {
		dev_err(dcp->dev, "no %ux%u timing at 60%s Hz among %u\n", v26->panel_width,
			v26->panel_height - dcp->notch_height,
			v26->board->promotion ? " or 120" : "", count);
		kfree(modes);
		return -EINVAL;
	}
	/*
	 * The parser marks a ProMotion mode by its size and a sync rate of
	 * exactly 120 Hz; any 120 Hz mode of this built-in panel is one.
	 */
	v26->vrr = best->vrr || drm_mode_vrefresh(&best->mode) == 120;
	best->vrr = v26->vrr;
	count = v26->vrr ? 2 : 1;
	selected = kcalloc(count, sizeof(*best), GFP_KERNEL);
	if (!selected) {
		kfree(modes);
		return -ENOMEM;
	}
	selected[0] = *best;
	kfree(modes);
	preferred = &selected[0];
	if (v26->vrr) {
		struct dcp_display_mode *locked = &selected[1];

		*locked = selected[0];
		locked->vrr = false;
		locked->mode.clock = DIV_ROUND_CLOSEST(locked->mode.clock, 2);
		drm_mode_set_name(&locked->mode);
		if (j613_25g83_refresh == 60)
			preferred = locked;
		else if (j613_25g83_refresh != 120)
			dev_warn(dcp->dev, "appledrm.j613_25g83_refresh=%u is neither 60 nor 120; preferring 120 Hz\n",
				 j613_25g83_refresh);
	} else if (j613_25g83_refresh == 120 && v26->board->promotion) {
		dev_warn(dcp->dev, "the panel has no 120 Hz timing; only %d Hz is available\n",
			 drm_mode_vrefresh(&preferred->mode));
	}
	for (i = 0; i < count; i++)
		selected[i].mode.type &= ~DRM_MODE_TYPE_PREFERRED;
	preferred->mode.type |= DRM_MODE_TYPE_PREFERRED;
	mutex_lock(&dcp->modes_lock);
	kfree(dcp->modes);
	dcp->modes = selected;
	dcp->nr_modes = count;
	dcp->modes_admitted = true;
	mutex_unlock(&dcp->modes_lock);
	dev_info(dcp->dev, "modes: %s; preferred %d Hz (appledrm.j613_25g83_refresh)\n",
		 v26->vrr ? "120 Hz variable refresh and 60 Hz locked" : "one fixed timing",
		 drm_mode_vrefresh(&preferred->mode));
	return 0;
}

int iomfb_v26_6_start(struct apple_dcp *dcp)
{
	struct apple_dcp_v26 *v26 = dcp->v26;
	const struct drm_display_mode *mode;
	const char *step;
	int ret;

	if (!v26 || !v26->rtk || v26->failed || v26->started)
		return -ENODEV;

	step = "DCPLink";
	ret = dcp_v26_link_start(&v26->link);
	if (ret)
		goto fail;

	mutex_lock(&v26->lock);
	step = "start signal";
	ret = dcp_v26_simple_call(v26, A(401), false, true, true);
	if (!ret) {
		step = "first client open";
		ret = dcp_v26_simple_call(v26, A(457), false, false, false);
	}
	mutex_unlock(&v26->lock);
	if (ret)
		goto fail;
	dcp_v26_link_set_idle_work(&v26->link, &v26->idle_work);

	step = "panel mode";
	mutex_lock(&v26->lock);
	ret = dcp_v26_mode(dcp, v26);
	mutex_unlock(&v26->lock);
	if (ret)
		goto fail;

	/* Nits; the backlight device appears with the first frame. */
	v26->bl_max = min_t(u32, dcp->brightness.maximum, v26->board->max_nits);
	v26->bl_nits = min_t(u32, DCP_V26_DEFAULT_NITS, v26->bl_max);
	v26->started = true;
	/* Never removed, like the session it describes. */
	debugfs_create_file("dcp-j613_25g83", 0400, NULL, v26, &dcp_v26_status_fops);
	debugfs_create_file("dcp-j613_25g83-present", 0400, NULL, v26, &dcp_v26_present_fops);
	dcp->connector->connected = true;
	dcp_set_dimensions(dcp, dcp_modes_transfer_begin(dcp));
	dcp_mode_set_valid(&dcp->mode_state, true);
	dcp->active = true;
	complete(&dcp->start_done);

	mode = &dcp->modes[0].mode;
	dev_info(dcp->dev, "%s display started: %ux%u@%d, %u notch rows %s, %ux%u mm\n",
		 v26->board->name, mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode),
		 v26->notch_rows,
		 dcp->notch_height ? "hidden" : "shown", mode->width_mm, mode->height_mm);
	return 0;
fail:
	v26->failed = true;
	dev_err(dcp->dev,
		"%s display not started: %s failed: %d; the boot framebuffer stays (reboot to retry)\n",
		v26->board->name, step, ret);
	return ret;
}

/*
 * Panel brightness travels in the swap record, in nits.
 *
 * Until the first request the firmware keeps the brightness iBoot set, and
 * no swap touches it. From then on the driver sets it: a request, the CRTC
 * going off (0 nits) and the CRTC coming back (the requested nits) each make
 * the target pending, and the next swap carries it. A request waits a frame
 * for a flip to carry it, then makes a swap of its own (see
 * dcp_v26_brightness_commit()).
 */

/* Called with the lock held. */
static void dcp_v26_brightness_changed(struct apple_dcp_v26 *v26)
{
	lockdep_assert_held(&v26->lock);
	if (!v26->bl_owned)
		return;
	WRITE_ONCE(v26->bl_pending, true);
	v26->bl_seq++;
}

/* Adds a pending target to @swap; returns the token dcp_v26_brightness_done() takes. */
static u64 dcp_v26_brightness_encode(struct apple_dcp_v26 *v26, u8 *swap)
{
	lockdep_assert_held(&v26->lock);
	if (!v26->bl_pending)
		return 0;
	dcp_v26_encode_brightness(swap, v26->bl_off ? 0 : v26->bl_nits);
	return v26->bl_seq;
}

/* A swap completed; @token is what it carried. Called with the lock held. */
static void dcp_v26_brightness_done(struct apple_dcp_v26 *v26, u64 token)
{
	lockdep_assert_held(&v26->lock);
	/* The first frame is on the panel: KMS is up, so is the backlight now. */
	if (!v26->bl_queued && v26->bl_max) {
		v26->bl_queued = true;
		schedule_work(&v26->bl_register_work);
	}
	if (!token)
		return;
	v26->bl_swaps++;
	/* A change made since then is still pending. */
	if (token != v26->bl_seq)
		return;
	v26->bl_sent = v26->bl_off ? 0 : v26->bl_nits;
	WRITE_ONCE(v26->bl_pending, false);
	wake_up_all(&v26->bl_wait);
}

/*
 * Swap start, then the swap; returns once the firmware completed that swap,
 * with @completed set to when its completion came in.
 */
/* Matched UUID ABI: location 9, packed 80-byte request, signed Q32. */
static int dcp_v26_ctm_locked(struct apple_dcp_v26 *v26,
                          const struct drm_crtc_state *state)
{
        u8 request[80] = {}, readback[76] = {};
        __le32 status = 0;
        const struct drm_color_ctm *ctm;
        int i, ret;

        if (!state || (v26->ctm_valid && !state->color_mgmt_changed &&
                       !state->mode_changed && !state->active_changed))
                return 0;
        ctm = state->ctm ? state->ctm->data : NULL;
        put_unaligned_le32(9, request);
        for (i = 0; i < 9; i++) {
                u64 coefficient = ctm ? ctm->matrix[i] :
                                  ((i == 0 || i == 4 || i == 8) ? 1ULL << 32 : 0);
                u64 magnitude = coefficient & ~(1ULL << 63);
                u64 signed_q32 = coefficient & (1ULL << 63) ? -magnitude : magnitude;

                put_unaligned_le64(signed_q32, request + 4 + 8 * i);
        }
        ret = dcp_v26_call(v26, A(423), request, sizeof(request),
                       &status, sizeof(status), 0);
        v26->ctm_calls++;
        v26->ctm_status = le32_to_cpu(status);
        v26->ctm_valid = false;
        if (ret || v26->ctm_status) {
                dev_err(v26->dev, "CTM setter transport=%d status=%#x\n",
                        ret, v26->ctm_status);
                return ret ? ret : -EIO;
        }
        /* Independent firmware getter, before the swap and on the same lock. */
        memset(request + 4, 0, 76);
        ret = dcp_v26_call(v26, A(422), request, sizeof(request),
                       readback, sizeof(readback), 0);
        v26->ctm_get_status = get_unaligned_le32(readback + 72);
        for (i = 0; i < 9; i++)
                v26->ctm_readback[i] = get_unaligned_le64(readback + i * 8);
        dev_info(v26->dev, "CTM call %llu setter=%#x getter_transport=%d getter=%#x diagonal=%#llx,%#llx,%#llx\n",
                 v26->ctm_calls, v26->ctm_status, ret, v26->ctm_get_status,
                 v26->ctm_readback[0], v26->ctm_readback[4], v26->ctm_readback[8]);
        if (ret || v26->ctm_get_status)
                return ret ? ret : -EIO;
        v26->ctm_valid = true;
        return 0;
}

static int dcp_v26_swap(struct apple_dcp_v26 *v26, const u8 *surface, u64 iova,
			u32 width, u32 height, u32 dst_y, ktime_t *completed,
                        const struct drm_crtc_state *ctm_state)
{
	struct dcp_v26_present_record rec = {};
	__le32 start[4] = {}, started[2] = {}, result[3];
	u64 brightness = 0;
	u8 *swap;
	u32 id;
	int ret;

	if (surface && (!iova || !width || width > v26->panel_width || !height ||
			dst_y > v26->panel_height || height > v26->panel_height - dst_y))
		return -EINVAL;
	swap = kmalloc(DCP_V26_SWAP_SIZE, GFP_KERNEL);
	if (!swap)
		return -ENOMEM;

	mutex_lock(&v26->lock);
	rec.surface = !!surface;
	rec.start = ktime_get();
	ret = dcp_v26_ctm_locked(v26, ctm_state);
	if (!ret)
	ret = dcp_v26_call(v26, A(406), start, sizeof(start), started, sizeof(started), 0);
	if (!ret && le32_to_cpu(started[1]))
		ret = -EIO;
	id = le32_to_cpu(started[0]);
	if (!ret && !id)
		ret = -EPROTO;
	if (!ret) {
		dcp_v26_encode_swap(swap, id, DCP_V26_BLACK, surface, iova, width, height,
				    dst_y, DCP_V26_LAYER);
		/* A target long past: show it at the next refresh, as on M1/M2. */
		if (surface && READ_ONCE(v26->timed) && READ_ONCE(j613_25g83_swap_timestamps)) {
			dcp_v26_swap_set_timing(swap, 120);
			rec.timed = 1;
		}
		rec.id = id;
		rec.submitted = ktime_get();
		brightness = dcp_v26_brightness_encode(v26, swap);
		ret = dcp_v26_call(v26, A(407), swap, DCP_V26_SWAP_SIZE, result, sizeof(result),
				   id);
		/* The swap status sits at byte 5 of the reply. */
		if (!ret && get_unaligned_le32((u8 *)result + 5))
			ret = -EIO;
	}
	if (ret) {
		v26->failed = true;
	} else {
		dcp_v26_brightness_done(v26, brightness);
		rec.replied = v26->link.reply_time;
		rec.completed = v26->link.completion_time;
		*completed = rec.completed;
	}
	v26->history[v26->history_count++ % DCP_V26_HISTORY] = rec;
	mutex_unlock(&v26->lock);

	kfree(swap);
	return ret;
}

/*
 * Shows @fb (or only the black background) and keeps it until the next swap.
 * @presented, if set, gets the time the firmware reported the swap complete.
 */
static bool dcp_v26_present(struct apple_dcp_v26 *v26, struct drm_framebuffer *fb, u32 dst_y,
			    ktime_t *presented, const struct drm_crtc_state *ctm_state)
{
	u8 surface[DCP_V26_SURFACE_SIZE];
	struct drm_framebuffer *old;
	u64 iova = 0, start, elapsed, swaps;
	ktime_t completed;
	int ret;

	if (READ_ONCE(v26->failed))
		return false;
	if (fb) {
		drm_framebuffer_get(fb);
		dcp_v26_encode_surface(surface, fb->pitches[0], fb->width, fb->height,
				       fb->format->format == DRM_FORMAT_XRGB8888);
		iova = drm_fb_dma_get_gem_obj(fb, 0)->dma_addr;
	}
	start = ktime_get_ns();
	ret = dcp_v26_swap(v26, fb ? surface : NULL, iova, fb ? fb->width : 0,
			   fb ? fb->height : 0, dst_y, &completed, ctm_state);
	elapsed = ktime_get_ns() - start;
	if (ret) {
		/* Either framebuffer may still be scanned out: keep both. */
		dev_err(v26->dev, "native DCP flip failed %d; buffers pinned until reboot\n", ret);
		return false;
	}
	mutex_lock(&v26->lock);
	old = v26->active_fb;
	v26->active_fb = fb;
	swaps = ++v26->swaps;
	v26->swap_ns_max = max(v26->swap_ns_max, elapsed);
	mutex_unlock(&v26->lock);
	if (old)
		drm_framebuffer_put(old);
	if (swaps <= 3)
		dev_info(v26->dev, "swap %llu complete: surface %d, imported %d, %llu us\n",
			 swaps, !!fb, fb && fb->obj[0]->import_attach, elapsed / NSEC_PER_USEC);
	if (presented)
		*presented = completed;
	return true;
}

/* Releases helper waiters without reporting a completed flip. */
static void dcp_v26_cancel_event(struct apple_dcp *dcp)
{
	struct apple_crtc *crtc = dcp->crtc;
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	event = crtc->event;
	crtc->event = NULL;
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
	if (!event)
		return;
	if (event->base.fence) {
		dma_fence_set_error(event->base.fence, -EIO);
		dma_fence_signal(event->base.fence);
	}
	if (event->base.completion) {
		complete_all(event->base.completion);
		if (event->base.completion_release)
			event->base.completion_release(event->base.completion);
		event->base.completion = NULL;
	}
	drm_event_cancel_free(crtc->base.dev, &event->base);
}

/* Only the layout the firmware was qualified with: one full-mode linear plane. */
int iomfb_v26_6_atomic_check(struct apple_dcp *dcp, struct drm_crtc *crtc,
			     struct drm_atomic_state *state)
{
	struct apple_dcp_v26 *v26 = dcp->v26;
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_plane_state *p = drm_atomic_get_new_plane_state(state, crtc->primary);
	struct drm_gem_dma_object *obj;
	struct drm_framebuffer *fb;
	u32 width, height;

	if (dcp->crashed || !v26 || READ_ONCE(v26->failed))
		return -EIO;
	if (!crtc_state || !crtc_state->active || !p || p->crtc != crtc || !p->visible)
		return 0;

	fb = p->fb;
	width = crtc_state->mode.hdisplay;
	height = crtc_state->mode.vdisplay;
	if (fb->format->format != DRM_FORMAT_XRGB8888 &&
	    fb->format->format != DRM_FORMAT_ARGB8888)
		return -EINVAL;
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR || fb->offsets[0] ||
	    fb->pitches[0] < width * 4 || (fb->pitches[0] & 63))
		return -EINVAL;
	if (p->src_x || p->src_y || p->crtc_x || p->crtc_y ||
	    p->src_w != width << 16 || p->src_h != height << 16 ||
	    p->crtc_w != width || p->crtc_h != height ||
	    fb->width != width || fb->height != height)
		return -EINVAL;
	obj = drm_fb_dma_get_gem_obj(fb, 0);
	if (!obj || !obj->dma_addr || (u64)fb->pitches[0] * height > obj->base.size)
		return -EINVAL;
	return 0;
}

int iomfb_v26_6_modeset(struct apple_dcp *dcp, struct drm_crtc_state *crtc_state)
{
	struct dcp_display_mode mode;

	if (!lookup_mode(dcp, &crtc_state->mode, &mode))
		return -EINVAL;
	/*
	 * The firmware keeps the timing it booted with, which every mode
	 * shares: nothing is sent to it. The mode only decides whether the
	 * swaps that follow carry timing words (120 Hz ProMotion) or not
	 * (locked 60 Hz).
	 */
	if (dcp->v26) {
		WRITE_ONCE(dcp->v26->timed, mode.vrr);
		WRITE_ONCE(dcp->v26->refresh, drm_mode_vrefresh(&mode.mode));
	}
	dcp_mode_set_valid(&dcp->mode_state, true);
	return 0;
}

void iomfb_v26_6_flush(struct apple_dcp *dcp, struct drm_crtc *crtc,
		       struct drm_atomic_state *state)
{
	struct drm_plane_state *p = drm_atomic_get_new_plane_state(state, crtc->primary);
	struct drm_framebuffer *fb;
	ktime_t presented;
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);

	if (!cs)
		cs = crtc->state;
	if (!p)
		p = crtc->primary->state;
	fb = p && p->visible ? p->fb : NULL;
	dcp->swap_start = ktime_get();
	if (!dcp->v26 || !dcp_v26_present(dcp->v26, fb, dcp->notch_height, &presented, cs)) {
		dcp_v26_cancel_event(dcp);
		return;
	}
	if (READ_ONCE(j613_25g83_flip_timestamp)) {
		/*
		 * The flip completed when the firmware's swap-complete message
		 * came in, not when this worker got to run: stamp the event with
		 * that time as it is (no swap_start correction).
		 */
		dcp->swap_start = KTIME_MIN;
		dcp_drm_crtc_page_flip(dcp, presented);
	} else {
		dcp_drm_crtc_page_flip(dcp, ktime_get());
	}
}

/*
 * Makes a swap for a pending brightness: a commit of the CRTC alone, which
 * shows the plane as it is. Nothing is done while the CRTC is off (enabling
 * it sends the brightness) or before its first modeset (the first frame
 * sends it), nor when a flip carried it in the meantime.
 */
static int dcp_v26_brightness_commit(struct apple_dcp_v26 *v26)
{
	struct apple_dcp *dcp = READ_ONCE(v26->dcp);
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_state *state;
	struct drm_crtc *crtc;
	int ret;

	if (!dcp || !dcp->crtc)
		return 0;
	crtc = &dcp->crtc->base;
	state = drm_atomic_state_alloc(crtc->dev);
	if (!state)
		return -ENOMEM;
	drm_modeset_acquire_init(&ctx, DRM_MODESET_ACQUIRE_INTERRUPTIBLE);
	state->acquire_ctx = &ctx;
retry:
	ret = drm_modeset_lock(&crtc->mutex, &ctx);
	if (ret)
		goto out;
	if (!crtc->state || !crtc->state->active || !READ_ONCE(v26->bl_pending))
		goto out;
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}
	ret = drm_atomic_commit(state);
out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry;
	}
	drm_atomic_state_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return ret;
}

static int dcp_v26_backlight_update(struct backlight_device *bd)
{
	struct apple_dcp_v26 *v26 = bl_get_data(bd);
	int ret = 0;

	mutex_lock(&v26->lock);
	if (v26->failed) {
		ret = -EIO;
	} else {
		/* Sent even when unchanged: it also repairs the firmware's value. */
		v26->bl_nits = backlight_get_brightness(bd);
		v26->bl_owned = true;
		dcp_v26_brightness_changed(v26);
	}
	mutex_unlock(&v26->lock);
	if (ret)
		return ret;

	if (wait_event_timeout(v26->bl_wait, !READ_ONCE(v26->bl_pending), DCP_V26_BL_FLIP_WAIT))
		return 0;
	return dcp_v26_brightness_commit(v26);
}

static const struct backlight_ops dcp_v26_backlight_ops = {
	.update_status = dcp_v26_backlight_update,
};

/* Nits, like the M1/M2 panels: 0 to the DT maximum, linear in luminance. */
static void dcp_v26_backlight_register(struct work_struct *work)
{
	struct apple_dcp_v26 *v26 = container_of(work, struct apple_dcp_v26, bl_register_work);
	struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.scale = BACKLIGHT_SCALE_LINEAR,
		.max_brightness = v26->bl_max,
		.brightness = min_t(u32, DCP_V26_DEFAULT_NITS, v26->bl_max),
	};
	struct backlight_device *bd;

	mutex_lock(&v26->bl_lock);
	if (!v26->bl_detached && !v26->bl_dev) {
		bd = backlight_device_register("apple-panel-bl", v26->dev, v26,
					       &dcp_v26_backlight_ops, &props);
		if (IS_ERR(bd))
			dev_err(v26->dev, "no panel backlight device: %pe\n", bd);
		else
			WRITE_ONCE(v26->bl_dev, bd);
	}
	mutex_unlock(&v26->bl_lock);
}

/* KMS goes away: so does the backlight device, before the CRTC it commits to. */
static void dcp_v26_backlight_detach(struct apple_dcp_v26 *v26)
{
	struct backlight_device *bd;

	mutex_lock(&v26->bl_lock);
	v26->bl_detached = true;
	bd = v26->bl_dev;
	WRITE_ONCE(v26->bl_dev, NULL);
	mutex_unlock(&v26->bl_lock);
	cancel_work_sync(&v26->bl_register_work);
	backlight_device_unregister(bd);
}

/* 0 nits while the CRTC is off, the requested nits once it is on again. */
static void dcp_v26_brightness_power(struct apple_dcp_v26 *v26, bool on)
{
	mutex_lock(&v26->lock);
	v26->bl_off = !on;
	dcp_v26_brightness_changed(v26);
	mutex_unlock(&v26->lock);
}

void iomfb_v26_6_poweron(struct apple_dcp *dcp)
{
	/* The panel stays on; the next swap shows the plane again. */
	if (dcp->v26 && dcp->v26->started)
		dcp_v26_brightness_power(dcp->v26, true);
}

void iomfb_v26_6_poweroff(struct apple_dcp *dcp)
{
	/* Blank to black; the panel and the DCP stay powered. */
	if (dcp->v26 && dcp->v26->started) {
		dcp_v26_brightness_power(dcp->v26, false);
		dcp_v26_present(dcp->v26, NULL, 0, NULL, NULL);
	}
}
