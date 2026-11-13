// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T8152 DCP on the iBoot interface: probe and bring-up.
 *
 * The bootloader (aura) trains the display and hands the DCP over with its
 * RTKit session running. This driver adopts that session, starts the iBoot,
 * DPAV and DPTX port endpoints, reconnects the DPTX port and asserts HPD,
 * then registers a DRM device whose flips are iBoot SET_SURFACE calls, in
 * the order aura's display_init_t8152 uses.
 */

#include <linux/aperture.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/iommu.h>
#include <linux/libnvdimm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "dcpib.h"

#define DCPIB_HPD_TIMEOUT_MS	10000

static void dcpib_rtk_recv(void *cookie, u8 ep, u64 message)
{
	struct dcpib *dcp = cookie;
	struct dcpib_ep *e = NULL;

	if (ep >= DCPIB_FIRST_EP && ep < DCPIB_FIRST_EP + DCPIB_NR_EPS)
		e = dcp->eps[ep - DCPIB_FIRST_EP];
	if (e)
		dcpib_ep_recv(e, message);
	else
		dev_dbg(dcp->dev, "message %#llx for idle endpoint %#x\n",
			message, ep);
}

static void dcpib_rtk_crashed(void *cookie, const void *crashlog, size_t size)
{
	struct dcpib *dcp = cookie;

	dev_err(dcp->dev, "DCP firmware crashed\n");
}

/* Buffers the firmware pre-allocated in its own DART mappings, or new ones. */
static int dcpib_rtk_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct dcpib *dcp = cookie;

	if (bfr->iova) {
		struct iommu_domain *domain = iommu_get_domain_for_dev(dcp->dev);
		phys_addr_t phys;

		if (!domain)
			return -ENOMEM;
		phys = iommu_iova_to_phys(domain, bfr->iova);
		if (!phys) {
			dev_warn(dcp->dev, "no mapping for firmware buffer %pad\n",
				 &bfr->iova);
			return -ENOMEM;
		}
		/* The firmware writes these behind the CPU's back. */
		bfr->buffer = memremap(phys, bfr->size, MEMREMAP_WC);
		if (!bfr->buffer)
			return -ENOMEM;
		bfr->is_mapped = true;
		return 0;
	}

	bfr->buffer = dma_alloc_coherent(dcp->dev, bfr->size, &bfr->iova,
					 GFP_KERNEL);
	return bfr->buffer ? 0 : -ENOMEM;
}

static void dcpib_rtk_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct dcpib *dcp = cookie;

	if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(dcp->dev, bfr->size, bfr->buffer, bfr->iova);
}

static const struct apple_rtkit_ops dcpib_rtkit_ops = {
	.crashed = dcpib_rtk_crashed,
	.recv_message = dcpib_rtk_recv,
	.shmem_setup = dcpib_rtk_shmem_setup,
	.shmem_destroy = dcpib_rtk_shmem_destroy,
};

DEFINE_DRM_GEM_DMA_FOPS(dcpib_fops);

static const struct drm_driver dcpib_drm_driver = {
	/*
	 * Frames are copied by the CPU, imported ones too: a compositor
	 * driving both displays imports one device's buffers into the other.
	 */
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name = "apple-dcpib",
	.desc = "Apple DCP (iBoot interface)",
	.major = 1,
	.minor = 0,
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &dcpib_fops,
};

/*
 * Adopt the bootloader's RTKit session. The T8152 DCP does not acknowledge
 * AP QUIESCED, so a quiesced handoff cannot be negotiated, and an IOP INIT
 * while its AP power is still ON panics the firmware ("Invalid AP power:
 * 32"). Keep the running session instead and start the application
 * endpoints explicitly (dcpib_ep_start), as drm/apple does for live M5
 * sessions.
 */
static void dcpib_rtkit_free(void *data)
{
	apple_rtkit_free(data);
}

static int dcpib_boot(struct dcpib *dcp)
{
	int ret;

	dcp->rtk = apple_rtkit_init_adopted(dcp->dev, dcp, NULL, 0,
					    &dcpib_rtkit_ops);
	if (IS_ERR(dcp->rtk))
		return PTR_ERR(dcp->rtk);
	ret = devm_add_action_or_reset(dcp->dev, dcpib_rtkit_free, dcp->rtk);
	if (ret)
		return ret;
	return apple_rtkit_adopt_running(dcp->rtk);
}

/*
 * The display pipe scans out through dart-disp0, the DCP fetches through
 * dart-dcp. Both are locked with their own live root, so each needs its own
 * device and domain: one domain spanning both would publish dart-dcp's root
 * into dart-disp0 and drop the scanout mappings the bootloader left there
 * (the pipe then faults, IOMFB axi_rd_err). Scanout buffers are allocated on
 * the DCP device and mapped into the scanout domain at the same address.
 */
static int dcpib_scanout_init(struct dcpib *dcp)
{
	struct device_node *np __free(device_node) =
		of_get_child_by_name(dcp->dev->of_node, "scanout");
	int ret;

	if (!np)
		return -ENODEV;
	dcp->scanout = of_platform_device_create(np, NULL, dcp->dev);
	if (!dcp->scanout)
		dcp->scanout = of_find_device_by_node(np);
	if (!dcp->scanout)
		return -ENODEV;
	ret = dma_set_mask_and_coherent(&dcp->scanout->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	ret = of_dma_configure(&dcp->scanout->dev, np, true);
	if (ret)
		return ret;
	dcp->scanout_dom = iommu_get_domain_for_dev(&dcp->scanout->dev);
	return dcp->scanout_dom ? 0 : -ENODEV;
}

static void *dcpib_vmap_wc(struct sg_table *sgt)
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
static dma_addr_t dcpib_scan_free_dva(struct dcpib *dcp, dma_addr_t dva,
				      size_t size)
{
	const size_t step = SZ_16K;

	size = ALIGN(size, step);
	for (int tries = 0; tries < 64 && dva >= size; tries++) {
		size_t off;

		for (off = 0; off < size; off += step)
			if (iommu_iova_to_phys(dcp->scanout_dom, dva + off))
				break;
		if (off == size)
			return dva;
		dev_info(dcp->dev, "scanout DART: %pad+%#zx is in use, moving down\n",
			 &dva, off);
		dva -= size;
	}
	return 0;
}

/* One scanout buffer: allocated, cleared, mapped for the CPU and the pipe. */
static int dcpib_scan_alloc(struct dcpib *dcp, int i, size_t size)
{
	struct dcpib_dma *fb = &dcp->scan[i];
	struct sg_table sgt;
	ssize_t mapped;
	dma_addr_t dva;
	int ret;

	ret = dcpib_dma_alloc(dcp, fb, size);
	if (ret)
		return ret;
	ret = dma_get_sgtable(dcp->dev, &sgt, fb->cpu, fb->iova, fb->size);
	if (ret)
		return ret;
	/*
	 * The DCP is coherent, the display pipe is not: it reads memory past
	 * the CPU caches, and frames written through the cacheable mapping
	 * show only once evicted. Write through a write-combined mapping of
	 * the same pages, with the zeroing of the allocation cleaned out first.
	 */
	arch_wb_cache_pmem(fb->cpu, fb->size);
	dcp->scan_wc[i] = dcpib_vmap_wc(&sgt);
	if (!dcp->scan_wc[i]) {
		sg_free_table(&sgt);
		return -ENOMEM;
	}
	memset(dcp->scan_wc[i], 0, fb->size);
	/*
	 * Cacheable, as iBoot maps scanout buffers: the pipe faults on
	 * entries with the DART's no-cache bit (IOMFB axi_rd_err).
	 */
	dva = dcpib_scan_free_dva(dcp, fb->iova, fb->size);
	if (!dva) {
		sg_free_table(&sgt);
		return -ENOSPC;
	}
	mapped = iommu_map_sgtable(dcp->scanout_dom, dva, &sgt,
				   IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE);
	sg_free_table(&sgt);
	if (mapped < (ssize_t)fb->size)
		return mapped < 0 ? mapped : -ENOMEM;
	dcp->scan_dva[i] = dva;
	dev_info(dcp->dev, "scanout buffer %d: %zu bytes at %pad\n", i, fb->size, &dva);
	return 0;
}

/*
 * A fresh iBoot session has no surface. Whenever the pipe runs without one
 * (the link retrains when the DPTX services restart, SET_MODE turns it on),
 * it fetches from a null base and the firmware takes an NMI (IOMFB
 * axi_rd_err). Hand it a black buffer of the boot mode at once, sized from
 * the bootloader's simple-framebuffer.
 */
static int dcpib_boot_surface(struct dcpib *dcp)
{
	/*
	 * The boot display's framebuffer is /chosen's; the bootloader adds the
	 * second display's (HDMI) at the root.
	 */
	struct device_node *chosen __free(device_node) =
		of_find_node_by_path("/chosen");
	struct device_node *parent __free(device_node) =
		of_property_read_bool(dcp->dev->of_node, "apple,hdmi") ?
		of_find_node_by_path("/") : of_node_get(chosen);
	u32 width = 1280, height = 720, stride = 1280 * 4;
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
					resource_size(&res), dcpib_drm_driver.name);
			if (ret)
				dev_warn(dcp->dev, "simpledrm stays: %d\n", ret);
		}
		break;
	}
	/*
	 * Large enough for the largest mode: while a SET_MODE switches, the
	 * pipe briefly scans the current surface with the new mode's size.
	 */
	size_t size = (size_t)stride * height;

	for (unsigned int i = 0; i < dcp->nr_tmodes; i++)
		size = max_t(size_t, size, (size_t)le32_to_cpu(dcp->tmodes[i].width) *
				   le32_to_cpu(dcp->tmodes[i].height) * 4);
	dcp->boot_w = width;
	dcp->boot_h = height;
	dcp->boot_stride = stride;
	for (int i = 0; i < ARRAY_SIZE(dcp->scan); i++) {
		ret = dcpib_scan_alloc(dcp, i, size);
		if (ret)
			return ret;
	}
	dcp->front = 0;
	ret = dcpib_ib_set_surface(dcp, dcp->scan_dva[0], width, height,
				   stride);
	dev_info(dcp->dev, "boot surface %ux%u at %pad: %d\n", width, height,
		 &dcp->scan_dva[0], ret);
	return ret;
}

/* Wait until the firmware reports a sink with modes, as aura does. */
int dcpib_wait_hpd(struct dcpib *dcp)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(DCPIB_HPD_TIMEOUT_MS);
	u32 timings = 0, colors = 0;
	int hpd;

	do {
		hpd = dcpib_ib_get_hpd(dcp, &timings, &colors);
		if (hpd > 0 && timings && colors) {
			dev_info(dcp->dev, "sink: %u timing, %u color modes\n",
				 timings, colors);
			return 0;
		}
		msleep(100);
	} while (time_before(jiffies, timeout));

	dev_err(dcp->dev, "no sink (hpd %d, %u timing, %u color modes)\n", hpd,
		timings, colors);
	return hpd < 0 ? hpd : -ENODEV;
}

static int dcpib_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *hdmi_np __free(device_node) = dcpib_hdmi_node();
	int source = hdmi_np ? dcpib_hdmi_source(hdmi_np) : -ENODEV;
	struct dcpib *dcp;
	int ret;

	/*
	 * Without a USB-C display the bootloader drives HDMI from the main DCP
	 * (source 0) and never starts DCPEXT0.
	 */
	if (dev->of_node == hdmi_np && source == 0) {
		dev_info(dev, "HDMI is on the main DCP\n");
		return -ENODEV;
	}

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	dcp = devm_drm_dev_alloc(dev, &dcpib_drm_driver, struct dcpib, drm);
	if (IS_ERR(dcp))
		return PTR_ERR(dcp);
	dcp->dev = dev;
	platform_set_drvdata(pdev, dcp);
	mutex_init(&dcp->lock);
	of_property_read_u32(dev->of_node, "apple,die", &dcp->die);

	dcp->wq = devm_alloc_ordered_workqueue(dev, "dcpib", WQ_MEM_RECLAIM);
	if (!dcp->wq)
		return -ENOMEM;

	ret = dcpib_scanout_init(dcp);
	if (ret)
		return dev_err_probe(dev, ret, "no scanout DART stream\n");

	dcp->hdmi = dev->of_node == hdmi_np || source == 0;
	if (dcp->hdmi && dev->of_node != hdmi_np)
		dev_info(dev, "HDMI from the main DCP (no USB-C display at boot)\n");
	/* Read only: the pin belongs to a controller the AOP runs. */
	dcp->hpd_gpio = devm_fwnode_gpiod_get(dev,
		of_fwnode_handle(dcp->hdmi ? hdmi_np : dev->of_node), "hpd",
		GPIOD_ASIS, "hpd");
	if (PTR_ERR(dcp->hpd_gpio) == -ENOENT)
		dcp->hpd_gpio = NULL;
	if (IS_ERR(dcp->hpd_gpio))
		return dev_err_probe(dev, PTR_ERR(dcp->hpd_gpio), "no HPD GPIO\n");
	dcp->hpd = 1;
	ret = dcp->hdmi ? dcpib_hdmi_init(dcp, hdmi_np) : dcpib_atc_init(dcp);
	if (ret)
		return dev_err_probe(dev, ret, "no DisplayPort phy\n");

	ret = dcpib_boot(dcp);
	if (ret)
		return dev_err_probe(dev, ret, "cannot adopt the DCP\n");

	ret = dcpib_iboot_init(dcp);
	if (ret)
		return dev_err_probe(dev, ret, "no disp0-service\n");
	/*
	 * The new iBoot session has dropped the surface while the pipe still
	 * runs: it fetches from a null base (IOMFB axi_rd_err, firmware NMI)
	 * unless it is powered off at once. It comes back on below, after a
	 * surface is set.
	 */
	ret = dcpib_ib_set_power(dcp, false);
	if (ret)
		return dev_err_probe(dev, ret, "display power off failed\n");
	ret = dcpib_dpav_init(dcp);
	if (ret)
		return dev_err_probe(dev, ret, "no DPAV controllers\n");
	ret = dcpib_dptx_init(dcp);
	if (ret)
		return dev_err_probe(dev, ret, "no DPTX port service\n");

	ret = dcpib_dptx_connect(dcp, 0);
	if (ret)
		return dev_err_probe(dev, ret, "DPTX connect failed\n");
	ret = dcpib_dptx_set_hpd(dcp, 0, true);
	if (ret)
		return dev_err_probe(dev, ret, "DPTX hotplug failed\n");

	ret = dcpib_wait_hpd(dcp);
	if (ret)
		return ret;
	ret = dcpib_ib_get_modes(dcp);
	if (ret)
		return dev_err_probe(dev, ret, "no modes\n");
	ret = dcpib_boot_surface(dcp);
	if (ret)
		return dev_err_probe(dev, ret, "no boot surface\n");
	/*
	 * Without the bootloader's link (no display on the port at boot) the
	 * phy starts cold, the right way round for the plug.
	 */
	if (dcp->phy->detect && dcp->phy->set_orientation) {
		bool flipped;

		if (dcp->phy->detect(dcp, &flipped) > 0) {
			if (dcp->phy->select)
				dcp->phy->select(dcp);
			dcp->phy->set_orientation(dcp, flipped);
		}
	}
	ret = dcp->phy->activate(dcp);
	if (ret)
		return dev_err_probe(dev, ret, "phy activation failed\n");
	/* While the display is still off and the firmware does no AUX. */
	dcpib_update_edid(dcp);
	ret = dcpib_ib_set_power(dcp, true);
	if (ret)
		return dev_err_probe(dev, ret, "set_power failed\n");
	dcp->powered = true;
	dev_info(dev, "%u timing modes, %u color modes\n", dcp->nr_tmodes,
		 dcp->nr_cmodes);

	ret = dcpib_kms_init(dcp);
	if (ret)
		return ret;
	ret = dcpib_set_boot_mode(dcp);
	if (ret)
		dev_warn(dev, "boot mode not set again: %d\n", ret);
	ret = drm_dev_register(&dcp->drm, 0);
	if (ret)
		return ret;
	if (dcp->hpd_gpio || dcp->pd)
		drmm_kms_helper_poll_init(&dcp->drm);
	drm_client_setup(&dcp->drm, NULL);
	return 0;
}

static const struct of_device_id dcpib_of_match[] = {
	{ .compatible = "apple,t8152-dcp-iboot" },
	{}
};
MODULE_DEVICE_TABLE(of, dcpib_of_match);

static struct platform_driver dcpib_platform_driver = {
	.driver = {
		.name = "apple-dcpib",
		.of_match_table = dcpib_of_match,
		/* The DCP and the link cannot be handed back yet. */
		.suppress_bind_attrs = true,
	},
	.probe = dcpib_probe,
};
module_platform_driver(dcpib_platform_driver);

MODULE_DESCRIPTION("Apple T8152 DCP display driver (iBoot interface)");
MODULE_LICENSE("Dual MIT/GPL");
