// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * DCP_FIRMWARE_IBOOT: the T8152 DCPs, driven through their firmware's
 * iBoot interface alone.
 *
 * The bootloader leaves a successful display's RTKit session running, or
 * quiesces it after a failed setup, and leaves an unused DCPEXT's firmware
 * loaded and waiting for an RTKit wake (apple,rtkit-wake). This adopts or
 * wakes that session, stops the AFK endpoints the bootloader left running
 * and starts the iBoot (disp0-service or dispext-service), DPAV and DPTX
 * port endpoints, reconnects the DPTX port and asserts HPD, then registers
 * a DRM device whose flips are iBoot SET_SURFACE calls (iboot_drm.c), in
 * the order the bootloader brings the display up.
 */

#include <linux/aperture.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/iommu.h>
#include <linux/libnvdimm.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/vmalloc.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_probe_helper.h>

#include "afk.h"
#include "dcp.h"
#include "dcp-internal.h"
#include "ibootep.h"
#include "iomfb_iboot.h"
#include "t8152_display.h"

#define DCP_IBOOT_HPD_TIMEOUT_MS	10000
/* RTKit's oslog system endpoint */
#define DCP_IBOOT_RTKIT_EP_OSLOG	8
/* The coprocessor's CPU control (bit 4: run) and status registers */
#define DCP_IBOOT_CPU_CONTROL		0x44
#define DCP_IBOOT_CPU_CONTROL_RUN	BIT(4)
#define DCP_IBOOT_CPU_STATUS		0x48

static void dcp_iboot_rtk_crashed(void *cookie, const void *crashlog, size_t size)
{
	struct apple_dcp *dcp = cookie;

	WRITE_ONCE(dcp->crashed, true);
	dev_err(dcp->dev, "DCP firmware crashed\n");
}

static void dcp_iboot_rtk_recv(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;
	struct apple_dcp_afkep *ep = NULL;

	if (READ_ONCE(dcp->quiescing))
		return;
	switch (endpoint) {
	case DISP0_ENDPOINT:
		ep = READ_ONCE(dcp->ibootep);
		break;
	case DPTX_CONTROLLER_ENDPOINT:
		ep = READ_ONCE(dcp->dptx_controller_ep);
		break;
	case DPTX_ENDPOINT:
		ep = READ_ONCE(dcp->dptxep);
		break;
	}
	if (IS_ERR_OR_NULL(ep)) {
		dev_dbg(dcp->dev, "message %#llx for idle endpoint %#x\n",
			message, endpoint);
		return;
	}
	afk_receive_message(ep, message);
}

/*
 * Buffers the firmware placed in its own DART mappings, or new ones, as for
 * every DCP; and a woken DCPEXT's oslog ring.
 */
static int dcp_iboot_rtk_shmem_setup(void *cookie, u8 endpoint,
				     struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	/*
	 * A woken DCPEXT offers the oslog ring in its own firmware segment, at
	 * an address only the firmware's retained DART tables map. Linux never
	 * reads that ring and only acknowledges its write counters: take it as
	 * the firmware's, without a CPU mapping. The request needs no reply;
	 * the bootloader sends none either.
	 */
	if (bfr->iova && endpoint == DCP_IBOOT_RTKIT_EP_OSLOG) {
		struct iommu_domain *domain = iommu_get_domain_for_dev(dcp->dev);

		if (domain && !iommu_iova_to_phys(domain, bfr->iova)) {
			bfr->is_mapped = true;
			return 0;
		}
	}
	return dcp_rtk_shmem_setup(cookie, bfr);
}

static void dcp_iboot_rtk_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	/* The firmware's oslog ring, taken without a mapping */
	if (bfr->is_mapped && !bfr->buffer)
		return;
	dcp_rtk_shmem_destroy(cookie, bfr);
}

static bool dcp_iboot_rtk_syslog_debug(void *cookie, const char *context,
				       const char *message)
{
	/* Surface logging through fbcon would itself submit another surface. */
	return strstr(message, "setSurfaceRequest") ||
	       strstr(message, "swap_submit_dcp") ||
	       strstr(message, "validateRequest: inSize");
}

static const struct apple_rtkit_ops dcp_iboot_rtkit_ops = {
	.crashed = dcp_iboot_rtk_crashed,
	.recv_message = dcp_iboot_rtk_recv,
	.shmem_setup_ep = dcp_iboot_rtk_shmem_setup,
	.shmem_destroy = dcp_iboot_rtk_shmem_destroy,
	.syslog_is_debug = dcp_iboot_rtk_syslog_debug,
};

/*
 * Nothing scans out the bootloader's framebuffers while the main DCP stays
 * off: drop the simpledrm devices on them, outputs that would show nothing.
 * This detaches a simpledrm that is bound already, as the built-in one is
 * long before this probe; one loaded later would bind again.
 */
static void dcp_iboot_drop_boot_fbs(void)
{
	struct device_node *chosen __free(device_node) = of_find_node_by_path("/chosen");

	for_each_child_of_node_scoped(chosen, np) {
		struct resource res;

		if (!of_device_is_compatible(np, "simple-framebuffer") ||
		    of_address_to_resource(np, 0, &res))
			continue;
		aperture_remove_conflicting_devices(res.start, resource_size(&res),
						    dcp_iboot_driver_name());
	}
}

static bool dcp_iboot_handoff_quiesced(struct dcp_iboot *ib)
{
	struct device_node *chosen __free(device_node) = of_find_node_by_path("/chosen");
	const char *version;
	u32 running;

	if (ib->disp.hdmi || !chosen)
		return false;
	if (!of_property_read_u32(chosen, "apple,dcp-rtkit-running", &running))
		return running == 0;
	/*
	 * This older J873 loader predates the state property. Its failed
	 * display setup quiesces DCP and keeps iBoot's placeholder framebuffer;
	 * a successful setup hands over ON. Restrict this fallback to the
	 * verified loader build, rather than guessing for other handoffs.
	 */
	if (of_property_read_string(chosen, "asahi,m1n1-stage2-version", &version) ||
	    strcmp(version, "v0.1.0-259-ga30a589a"))
		return false;
	for_each_child_of_node_scoped(chosen, np) {
		u32 width, height;

		if (of_device_is_compatible(np, "simple-framebuffer") &&
		    !of_property_read_u32(np, "width", &width) &&
		    !of_property_read_u32(np, "height", &height))
			return width == 640 && height == 1136;
	}
	return false;
}

/* The unused DCPEXT firmware is loaded and powered by iBoot. */
static int dcp_iboot_check_wake(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	struct resource *res;
	void __iomem *psd, *coproc;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "coproc-psd");
	if (!res)
		return -EINVAL;
	psd = devm_ioremap(dcp->dev, res->start, resource_size(res));
	if (!psd)
		return -ENOMEM;
	/* A read from the coprocessor while gated hangs the machine. */
	if (readl(psd + 0x80) & 7)
		return -ENODEV;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "coproc");
	if (!res)
		return -EINVAL;
	coproc = devm_ioremap(dcp->dev, res->start, resource_size(res));
	if (!coproc)
		return -ENOMEM;
	dev_info(dcp->dev, "firmware handoff: CPU control %#x status %#x\n",
		 readl(coproc + DCP_IBOOT_CPU_CONTROL),
		 readl(coproc + DCP_IBOOT_CPU_STATUS));
	return (readl(coproc + DCP_IBOOT_CPU_CONTROL) &
		DCP_IBOOT_CPU_CONTROL_RUN) ? 0 : -ENODEV;
}

/*
 * Wake only a quiesced session: IOP INIT on a running DCP can panic it. A
 * running one is taken as it runs, without the HELLO/EPMAP handshake;
 * its AFK endpoints are then registered as they start (hw.afk_adopt_endpoints).
 */
static int dcp_iboot_boot(struct dcp_iboot *ib)
{
	struct apple_dcp *dcp = ib->dcp;
	bool wake = of_property_read_bool(dcp->dev->of_node, "apple,rtkit-wake");
	struct apple_rtkit *rtk;
	int ret;

	if ((!ib->disp.hdmi && wake) || dcp_iboot_handoff_quiesced(ib)) {
		if (wake) {
			ret = dcp_iboot_check_wake(dcp);
			if (ret)
				return ret;
		}
		rtk = apple_rtkit_init(dcp->dev, dcp, NULL, 0, &dcp_iboot_rtkit_ops);
		if (IS_ERR(rtk))
			return PTR_ERR(rtk);
		dcp->rtk = rtk;
		ret = apple_rtkit_wake(rtk);
		dev_info(dcp->dev, "quiesced DCP wake: %d\n", ret);
		return ret;
	}
	rtk = apple_rtkit_init_adopted(dcp->dev, dcp, NULL, 0, &dcp_iboot_rtkit_ops);
	if (IS_ERR(rtk))
		return PTR_ERR(rtk);
	dcp->rtk = rtk;
	return apple_rtkit_adopt_running(rtk);
}

/*
 * The display pipe scans out through dart-disp0, the DCP fetches through
 * dart-dcp. Both are locked with their own live root, so each needs its own
 * device and domain: one domain spanning both would publish dart-dcp's root
 * into dart-disp0 and drop the scanout mappings the bootloader left there
 * (the pipe then faults, IOMFB axi_rd_err). Scanout buffers are allocated on
 * the DCP device and mapped into the scanout domain at the same address.
 */
static int dcp_iboot_scanout_init(struct dcp_iboot *ib)
{
	struct device *dev = ib->dcp->dev;
	struct device_node *np __free(device_node) =
		of_get_child_by_name(dev->of_node, "scanout");
	int ret;

	if (!np)
		return -ENODEV;
	ib->scanout = of_platform_device_create(np, NULL, dev);
	if (!ib->scanout)
		ib->scanout = of_find_device_by_node(np);
	if (!ib->scanout)
		return -ENODEV;
	ret = dma_set_mask_and_coherent(&ib->scanout->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	ret = of_dma_configure(&ib->scanout->dev, np, true);
	if (ret)
		return ret;
	ib->scanout_dom = iommu_get_domain_for_dev(&ib->scanout->dev);
	return ib->scanout_dom ? 0 : -ENODEV;
}

/* Firmware-visible memory: never freed, the firmware may still use it. */
static int dcp_iboot_dma_alloc(struct dcp_iboot *ib, struct dcp_iboot_dma *dma,
			       size_t size)
{
	dma->size = PAGE_ALIGN(size);
	dma->cpu = dma_alloc_coherent(ib->dcp->dev, dma->size, &dma->iova,
				      GFP_KERNEL);
	return dma->cpu ? 0 : -ENOMEM;
}

static void *dcp_iboot_vmap_wc(struct sg_table *sgt)
{
	unsigned int n = 0, count = 0;
	struct sg_page_iter iter;
	struct page **pages;
	void *vaddr;

	for_each_sgtable_page(sgt, &iter, 0)
		count++;
	pages = kvmalloc_array(count, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;
	for_each_sgtable_page(sgt, &iter, 0)
		pages[n++] = sg_page_iter_page(&iter);
	vaddr = vmap(pages, count, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	kvfree(pages);
	return vaddr;
}

/*
 * The display pipe's DART is locked and comes with the firmware's own
 * mappings. The address the coprocessor's allocator handed out for a scanout
 * buffer can be one of them, and mapping over it corrupts what the pipe
 * fetches every frame (the picture flickers). Start at @dva and go down until
 * a range with no mapping is found.
 */
static dma_addr_t dcp_iboot_scan_free_dva(struct dcp_iboot *ib, dma_addr_t dva,
					  size_t size)
{
	const size_t step = SZ_16K;

	size = ALIGN(size, step);
	for (int tries = 0; tries < 64 && dva >= size; tries++) {
		size_t off;

		for (off = 0; off < size; off += step)
			if (iommu_iova_to_phys(ib->scanout_dom, dva + off))
				break;
		if (off == size)
			return dva;
		dev_info(ib->dcp->dev, "scanout DART: %pad+%#zx is in use, moving down\n",
			 &dva, off);
		dva -= size;
	}
	return 0;
}

/* One scanout buffer: allocated, cleared, mapped for the CPU and the pipe. */
static int dcp_iboot_scan_alloc(struct dcp_iboot *ib, int i, size_t size)
{
	struct dcp_iboot_dma *fb = &ib->scan[i];
	struct sg_table sgt;
	ssize_t mapped;
	dma_addr_t dva;
	int ret;

	ret = dcp_iboot_dma_alloc(ib, fb, size);
	if (ret)
		return ret;
	ret = dma_get_sgtable(ib->dcp->dev, &sgt, fb->cpu, fb->iova, fb->size);
	if (ret)
		return ret;
	/*
	 * The DCP is coherent, the display pipe is not: it reads memory past
	 * the CPU caches, and frames written through the cacheable mapping
	 * show only once evicted. Write through a write-combined mapping of
	 * the same pages, with the zeroing of the allocation cleaned out first.
	 */
	arch_wb_cache_pmem(fb->cpu, fb->size);
	ib->scan_wc[i] = dcp_iboot_vmap_wc(&sgt);
	if (!ib->scan_wc[i]) {
		sg_free_table(&sgt);
		return -ENOMEM;
	}
	memset(ib->scan_wc[i], 0, fb->size);
	/*
	 * Cacheable, as the bootloader maps scanout buffers: the pipe faults on
	 * entries with the DART's no-cache bit (IOMFB axi_rd_err).
	 */
	dva = dcp_iboot_scan_free_dva(ib, fb->iova, fb->size);
	if (!dva) {
		sg_free_table(&sgt);
		return -ENOSPC;
	}
	mapped = iommu_map_sgtable(ib->scanout_dom, dva, &sgt,
				   IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE);
	sg_free_table(&sgt);
	if (mapped < (ssize_t)fb->size)
		return mapped < 0 ? mapped : -ENOMEM;
	ib->scan_dva[i] = dva;
	dev_info(ib->dcp->dev, "scanout buffer %d: %zu bytes at %pad\n", i,
		 fb->size, &dva);
	return 0;
}

/*
 * A fresh iBoot session has no surface. Whenever the pipe runs without one
 * (the link retrains when the DPTX services restart, SET_MODE turns it on),
 * it fetches from a null base and the firmware takes an NMI (IOMFB
 * axi_rd_err). Hand it a black buffer of the boot mode at once, sized from
 * the bootloader's simple-framebuffer.
 */
static int dcp_iboot_boot_surface(struct dcp_iboot *ib)
{
	/*
	 * The boot display's framebuffer is /chosen's; the bootloader adds the
	 * second display's (HDMI) at the root.
	 */
	struct device_node *chosen __free(device_node) =
		of_find_node_by_path("/chosen");
	struct device_node *parent __free(device_node) =
		ib->disp.dp_source ?
		of_find_node_by_path("/") : of_node_get(chosen);
	u32 width = 1280, height = 720, stride = 1280 * 4;
	size_t size;
	int ret;

	for_each_child_of_node_scoped(parent, np) {
		struct resource res;

		if (!of_device_is_compatible(np, "simple-framebuffer"))
			continue;
		of_property_read_u32(np, "width", &width);
		of_property_read_u32(np, "height", &height);
		of_property_read_u32(np, "stride", &stride);
		/*
		 * Take the display over from simpledrm on the bootloader's
		 * framebuffer, so it does not shadow this driver's output
		 * (fbcon on a framebuffer nothing scans out).
		 */
		if (!of_address_to_resource(np, 0, &res)) {
			ret = aperture_remove_conflicting_devices(res.start,
								  resource_size(&res),
								  dcp_iboot_driver_name());
			if (ret)
				dev_warn(ib->dcp->dev, "simpledrm stays: %d\n", ret);
		}
		break;
	}
	/*
	 * Large enough for the largest mode: while a SET_MODE switches, the
	 * pipe briefly scans the current surface with the new mode's size.
	 * A monitor may first appear after probe, with a larger mode list.
	 */
	size = max_t(size_t, (size_t)stride * height, 3840 * 2160 * 4);
	for (unsigned int i = 0; i < ib->nr_tmodes; i++)
		size = max_t(size_t, size, (size_t)le32_to_cpu(ib->tmodes[i].width) *
				   le32_to_cpu(ib->tmodes[i].height) * 4);
	ib->boot_w = width;
	ib->boot_h = height;
	ib->boot_stride = stride;
	for (int i = 0; i < ARRAY_SIZE(ib->scan); i++) {
		ret = dcp_iboot_scan_alloc(ib, i, size);
		if (ret)
			return ret;
	}
	ib->front = 0;
	ret = ibootep_set_surface(ib->dcp, ib->scan_dva[0], width, height, stride);
	dev_info(ib->dcp->dev, "boot surface %ux%u at %pad: %d\n", width, height,
		 &ib->scan_dva[0], ret);
	return ret;
}

/* Both mode lists of the current sink; a list is kept when its query fails. */
int dcp_iboot_get_modes(struct dcp_iboot *ib)
{
	struct iboot_timing_mode *tmodes;
	struct iboot_color_mode *cmodes;
	unsigned int count;
	int ret;

	ret = ibootep_get_timing_modes(ib->dcp, &tmodes, &count);
	if (ret)
		return ret;
	kfree(ib->tmodes);
	ib->tmodes = tmodes;
	ib->nr_tmodes = count;
	ret = ibootep_get_color_modes(ib->dcp, &cmodes, &count);
	if (ret)
		return ret;
	kfree(ib->cmodes);
	ib->cmodes = cmodes;
	ib->nr_cmodes = count;
	return 0;
}

/*
 * Wait until the firmware reports a sink with modes, as the bootloader does.
 * Nothing on the iBoot interface announces the sink, so GET_HPD is asked
 * every 100 ms; the J873g ports took 268 to 328 ms after power on.
 */
int dcp_iboot_wait_hpd(struct dcp_iboot *ib)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(DCP_IBOOT_HPD_TIMEOUT_MS);
	u32 timings = 0, colors = 0;
	int hpd;

	do {
		hpd = ibootep_get_hpd(ib->dcp, &timings, &colors);
		if (hpd > 0 && timings && colors) {
			dev_info(ib->dcp->dev, "sink: %u timing, %u color modes\n",
				 timings, colors);
			return 0;
		}
		msleep(100);
	} while (time_before(jiffies, timeout));

	dev_err(ib->dcp->dev, "no sink (hpd %d, %u timing, %u color modes)\n", hpd,
		timings, colors);
	return hpd < 0 ? hpd : -ENODEV;
}

static void dcp_iboot_av_work(struct work_struct *work)
{
	struct dcp_iboot *ib = container_of(to_delayed_work(work), struct dcp_iboot,
					    av_work);

	if (READ_ONCE(ib->ready))
		dcp_iboot_reset_mode(ib);
}

/*
 * The DPAV controller endpoint's services take no host commands, but the
 * firmware expects all of them opened: its video interface powers the
 * display on and starts its idle power off timer, so SET_MODE follows once
 * that one is up.
 */
static void dcp_iboot_dpav_init(struct apple_epic_service *service,
				const char *name, const char *class, s64 unit)
{
	struct dcp_iboot *ib = READ_ONCE(service->ep->dcp->iboot);

	if (ib && name && strstr(name, "dcpav-video-interface"))
		dcp_iboot_av_started(ib);
}

static const struct apple_epic_service_ops dcp_iboot_dpav_ops = {
	.name = "dcpav",
	.reusable = true,
	.init = dcp_iboot_dpav_init,
};

static unsigned int dcp_iboot_ep_services(struct apple_dcp_afkep *ep)
{
	unsigned int count = 0;

	for (unsigned int i = 0; i < AFK_MAX_CHANNEL; i++)
		count += READ_ONCE(ep->services[i].enabled);
	return count;
}

/*
 * The endpoint's services as it announces them, up to @expected within the
 * AFK timeout; none is an error, fewer a warning.
 */
static int dcp_iboot_wait_services(struct apple_dcp *dcp,
				   struct apple_dcp_afkep *ep, unsigned int expected)
{
	unsigned int count;

	wait_event_timeout(ep->service_wq, dcp_iboot_ep_services(ep) >= expected,
			   msecs_to_jiffies(dcp->hw.afk_timeout_ms));
	count = dcp_iboot_ep_services(ep);
	if (!count) {
		dev_err(dcp->dev, "endpoint %#x announced no service\n", ep->endpoint);
		return -ETIMEDOUT;
	}
	if (count < expected)
		dev_warn(dcp->dev, "endpoint %#x: %u of %u services\n", ep->endpoint,
			 count, expected);
	return 0;
}

static int dcp_iboot_start_ep(struct apple_dcp *dcp, u32 endpoint,
			      struct apple_dcp_afkep **slot,
			      const struct apple_epic_service_ops *ops,
			      unsigned int expected)
{
	struct apple_dcp_afkep *ep = afk_init(dcp, endpoint, NULL);
	int ret;

	if (IS_ERR(ep))
		return PTR_ERR(ep);
	ep->unmatched_ops = ops;
	WRITE_ONCE(*slot, ep);
	ret = afk_start(ep);
	if (ret) {
		dev_err(dcp->dev, "endpoint %#x did not start: %d\n", endpoint, ret);
		return ret;
	}
	return dcp_iboot_wait_services(dcp, ep, expected);
}

/* The iBoot service is ready once its init callback has run. */
static int dcp_iboot_start_iboot(struct apple_dcp *dcp)
{
	int ret;

	ret = ibootep_init(dcp);
	if (ret)
		return ret;
	wait_event_timeout(dcp->ibootep->service_wq, ibootep_is_ready(dcp),
			   msecs_to_jiffies(dcp->hw.afk_timeout_ms));
	return ibootep_is_ready(dcp) ? 0 : -ENODEV;
}

/*
 * Once adopted, the DCP may still DMA into our buffers. Stop the host side:
 * no more endpoint work or messages, no KMS work, and RTKit detached with
 * its buffers retained. Firmware-visible storage stays allocated until
 * reboot.
 */
static void dcp_iboot_detach(struct dcp_iboot *ib)
{
	struct apple_dcp *dcp = ib->dcp;

	WRITE_ONCE(ib->ready, false);
	WRITE_ONCE(dcp->quiescing, true);
	cancel_delayed_work_sync(&ib->av_work);
	dcp_iboot_kms_stop(ib);
	afk_quiesce(dcp->ibootep);
	afk_quiesce(dcp->dptx_controller_ep);
	afk_quiesce(dcp->dptxep);
	if (!IS_ERR_OR_NULL(dcp->rtk))
		apple_rtkit_free_retaining_buffers(dcp->rtk);
	WRITE_ONCE(dcp->rtk, NULL);
}

static int dcp_iboot_start(struct dcp_iboot *ib)
{
	struct t8152_display *disp = &ib->disp;
	struct apple_dcp *dcp = ib->dcp;
	struct device *dev = dcp->dev;
	bool sink = true;
	int ret;

	ret = dcp_iboot_start_iboot(dcp);
	if (ret)
		return dev_err_probe(dev, ret, "no disp0-service\n");
	/*
	 * The new iBoot session has dropped the surface while the pipe still
	 * runs: it fetches from a null base (IOMFB axi_rd_err, firmware NMI)
	 * unless it is powered off at once. It comes back on below, after a
	 * surface is set.
	 */
	ret = ibootep_set_power(dcp, false);
	if (ret)
		return dev_err_probe(dev, ret, "display power off failed\n");
	ret = dcp_iboot_start_ep(dcp, DPTX_CONTROLLER_ENDPOINT,
				 &dcp->dptx_controller_ep, &dcp_iboot_dpav_ops, 4);
	if (ret)
		return dev_err_probe(dev, ret, "no DPAV controllers\n");
	ret = dcp_iboot_start_ep(dcp, DPTX_ENDPOINT, &dcp->dptxep,
				 &t8152_dptx_port_ops, 2);
	if (ret)
		return dev_err_probe(dev, ret, "no DPTX port service\n");
	if (disp->usbc) {
		ret = t8152_dptx_disconnect(disp, 0);
		if (ret)
			return dev_err_probe(dev, ret, "cannot release inherited DP link\n");
	}

	/*
	 * USB-C negotiation can outlive probe or fail while a monitor sleeps.
	 * Register the connector first; the hotplug path owns all USB-C links.
	 */
	if (disp->usbc)
		sink = false;
	if (sink) {
		ret = t8152_dptx_connect(disp, 0);
		if (ret)
			return dev_err_probe(dev, ret, "DPTX connect failed\n");
		ret = t8152_dptx_set_hpd(disp, 0, true);
		if (ret)
			return dev_err_probe(dev, ret, "DPTX hotplug failed\n");
		ret = dcp_iboot_wait_hpd(ib);
		if (ret)
			return ret;
		ret = dcp_iboot_get_modes(ib);
		if (ret)
			return dev_err_probe(dev, ret, "no modes\n");
	}
	ret = dcp_iboot_boot_surface(ib);
	if (ret)
		return dev_err_probe(dev, ret, "no boot surface\n");
	/* Keep an idle connector registered while the PD controller negotiates. */
	if (!sink) {
		dev_info(dev, "waiting for a USB-C display\n");
		goto register_kms;
	}
	disp->hpd = 1;
	/*
	 * Without the bootloader's link (no display on the port at boot) the
	 * PHY starts cold, the right way round for the plug.
	 */
	if (disp->phy->detect && disp->phy->set_orientation) {
		bool flipped;

		if (disp->phy->detect(disp, &flipped) > 0) {
			if (disp->phy->select)
				disp->phy->select(disp);
			disp->phy->set_orientation(disp, flipped);
		}
	}
	ret = disp->phy->activate(disp);
	if (ret)
		return dev_err_probe(dev, ret, "PHY activation failed\n");
	/* While the display is still off and the firmware does no AUX. */
	dcp_iboot_update_edid(ib);
	ret = ibootep_set_power(dcp, true);
	if (ret)
		return dev_err_probe(dev, ret, "set_power failed\n");
	ib->powered = true;
	dev_info(dev, "%u timing modes, %u color modes\n", ib->nr_tmodes,
		 ib->nr_cmodes);

register_kms:
	ret = dcp_iboot_kms_init(ib);
	if (ret)
		return ret;
	if (sink) {
		ret = dcp_iboot_set_boot_mode(ib);
		if (ret)
			dev_warn(dev, "boot mode not set again: %d\n", ret);
	}
	ret = drm_dev_register(&ib->drm, 0);
	if (ret)
		return ret;
	WRITE_ONCE(ib->ready, true);
	if (disp->hpd_gpio || disp->usbc)
		drmm_kms_helper_poll_init(&ib->drm);
	if (disp->usbc)
		queue_delayed_work(system_freezable_wq, &ib->hpd_work,
				   msecs_to_jiffies(250));
	drm_client_setup(&ib->drm, NULL);
	return 0;
}

/*
 * hdmi_np: DCPEXT0's node, which describes the HDMI PHY; its source select
 * names the DCP driving HDMI (0 the main DCP, 1 DCPEXT0). On the J873g,
 * iBoot lights only a monitor on the back-right port, through the main DCP,
 * and leaves HDMI to the kernel even when HDMI is the only display. A
 * source a bootloader did set is still adopted.
 */
int dcp_iboot_probe(struct apple_dcp *dcp)
{
	struct device *dev = dcp->dev;
	/* Read by whichever DCP probes first, before any DCP drives a port. */
	int boot_link = t8152_atc_boot_link(dev);
	struct device_node *hdmi_np __free(device_node) = t8152_hdmi_node();
	int source = hdmi_np ? t8152_hdmi_source(hdmi_np) : -ENODEV;
	struct t8152_display *disp;
	struct dcp_iboot *ib;
	u32 dp_source = 0;
	bool main_dcp;
	u32 live;
	int ret;

	if (dev->of_node == hdmi_np && source != 1 &&
	    !of_property_present(dev->of_node, "apple,usb-c-ports")) {
		dev_info(dev, "%s\n", source == 0 ? "HDMI is on the main DCP" :
			 "no bootloader HDMI link to adopt");
		return -ENODEV;
	}

	/*
	 * A main DCP that the bootloader left without a display cannot power
	 * one: its SET_POWER never completes, on HDMI or USB-C. It has one
	 * when it drives HDMI or the bootloader's link on a USB-C port; none
	 * when iBoot found no monitor on back-right, with HDMI alone too. Then
	 * leave its outputs to the DCPEXTs, which start cold, and the shared
	 * HDMI port with them. If the ports could not be read, bind as before.
	 */
	of_property_read_u32(dev->of_node, "apple,dp-source", &dp_source);
	main_dcp = dev->of_node != hdmi_np && !dp_source;
	if (main_dcp && source != 0 && boot_link == 0) {
		dcp_iboot_drop_boot_fbs();
		dev_info(dev, "no display from the bootloader: main DCP off, the DCPEXTs drive the outputs\n");
		return -ENODEV;
	}

	/*
	 * The bootloader's AFK endpoint hand-off: without it there is no
	 * telling whether an endpoint is live, and an INIT on a live one
	 * asserts in the firmware.
	 */
	ret = of_property_read_u32(dev->of_node, "apple,afk-active-endpoints", &live);
	if (ret || live & ~GENMASK(15, 0))
		return dev_err_probe(dev, -EINVAL,
				     "bootloader AFK endpoint handoff required\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	ib = dcp_iboot_alloc(dev);
	if (IS_ERR(ib))
		return PTR_ERR(ib);
	ib->dcp = dcp;
	disp = &ib->disp;
	disp->dcp = dcp;
	disp->dev = dev;
	disp->dp_source = dev->of_node == hdmi_np ? 1 : 0;
	of_property_read_u32(dev->of_node, "apple,dp-source", &disp->dp_source);
	if (disp->dp_source > 2)
		return -EINVAL;
	of_property_read_u32(dev->of_node, "apple,die", &disp->die);
	dcp->afk_live_endpoints = live;
	mutex_init(&ib->lock);
	INIT_DELAYED_WORK(&ib->av_work, dcp_iboot_av_work);

	ret = dcp_iboot_scanout_init(ib);
	if (ret)
		return dev_err_probe(dev, ret, "no scanout DART stream\n");

	disp->hdmi = source >= 0 && source == disp->dp_source;
	if (disp->hdmi && dev->of_node != hdmi_np)
		dev_info(dev, "HDMI from the main DCP (no USB-C display at boot)\n");
	/* Read only: the pin belongs to a controller the AOP runs. */
	disp->hpd_gpio = NULL;
	if (disp->hdmi)
		disp->hpd_gpio = devm_fwnode_gpiod_get(dev, of_fwnode_handle(hdmi_np),
						       "hpd", GPIOD_ASIS, "hpd");
	if (PTR_ERR(disp->hpd_gpio) == -ENOENT)
		disp->hpd_gpio = NULL;
	if (IS_ERR(disp->hpd_gpio))
		return dev_err_probe(dev, PTR_ERR(disp->hpd_gpio), "no HPD GPIO\n");
	disp->hpd = 0;
	ret = disp->hdmi ? t8152_hdmi_init(disp, hdmi_np) : t8152_atc_init(disp);
	if (ret)
		return dev_err_probe(dev, ret, "no DisplayPort PHY\n");
	/* Nobody has HDMI from the bootloader: it is one of the shared ports. */
	if (!disp->hdmi && source < 0 && hdmi_np) {
		ret = t8152_hdmi_port_init(disp, hdmi_np);
		if (ret)
			return dev_err_probe(dev, ret, "no shared HDMI port\n");
	}

	WRITE_ONCE(dcp->t8152, disp);
	WRITE_ONCE(dcp->iboot, ib);
	ret = dcp_iboot_boot(ib);
	if (IS_ERR_OR_NULL(dcp->rtk)) {
		dcp->rtk = NULL;
		return dev_err_probe(dev, ret, "cannot adopt the DCP\n");
	}
	/*
	 * From here on the DCP may DMA into what this driver gives it, and
	 * there is no safe way to stop it: retain the session until reboot.
	 */
	if (dcp_retain_live_session(dcp)) {
		dev_warn(dev, "live session not recorded; its DMA stays allocated\n");
		WRITE_ONCE(dcp->retain_dma, true);
	}
	/* The device stays bound from here on: follow the USB-C ports. */
	if (t8152_typec_init(disp))
		dev_err(dev, "USB-C ports not followed, no USB-C display\n");
	if (ret)
		dev_err(dev, "cannot adopt the DCP: %d\n", ret);
	else
		ret = dcp_iboot_start(ib);
	if (!ret)
		return 0;

	/*
	 * Gate RX before draining endpoint work; keep RTKit available until
	 * every sender has stopped. Do not reclaim firmware-visible storage
	 * or let the driver core retry a partially started session.
	 */
	dcp_iboot_detach(ib);
	dev_err(dev, "initialization failed (%d); live DMA resources retained until reboot\n",
		ret);
	return 0;
}

/* An unbind: the DRM device goes, the firmware session and its DMA stay. */
void dcp_iboot_remove(struct apple_dcp *dcp)
{
	struct dcp_iboot *ib = dcp->iboot;
	bool registered;

	if (!ib)
		return;
	registered = READ_ONCE(ib->ready);
	if (ib->drm.mode_config.poll_enabled)
		drm_kms_helper_poll_disable(&ib->drm);
	if (!READ_ONCE(dcp->quiescing))
		dcp_iboot_detach(ib);
	if (registered) {
		drm_dev_unplug(&ib->drm);
		/* Drops the committed state's connector reference. */
		drm_atomic_helper_shutdown(&ib->drm);
	}
}
