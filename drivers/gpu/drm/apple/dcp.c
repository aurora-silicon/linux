// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#include <kunit/visibility.h>
#include <linux/align.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/device/driver.h>
#include <linux/dma-mapping.h>
#include <linux/dma-fence.h>
#include <linux/gpio/consumer.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/soc/apple/dart.h>
#include <linux/soc/apple/dp-tunnel.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/iopoll.h>
#include <linux/of_platform.h>
#include <linux/suspend.h>
#include <linux/property.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/sleep.h>
#include <linux/soc/apple/pmp-report.h>
#include <linux/soc/apple/sleep.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_edid.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_module.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "afk.h"
#include "av.h"
#include "dcp.h"
#include "dcp-edr.h"
#include "dcp-panel-adt.h"
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/sizes.h>
#include "dcp-fabric-effects.h"
#include "dcp-fabric.h"
#include "dcp-hdmi.h"
#include "dcp-syslog.h"
#include "dcpext_scanout.h"
#include "dcp-internal.h"
#include "dcp-external-bw.h"
#include "dptx-service.h"
#include "iomfb.h"
#include "iomfb_internal.h"
#include "iomfb_m4.h"
#include "ibootep.h"
#include "iomfb_internal.h"
#include "parser.h"
#include "trace.h"

#define APPLE_DCP_COPROC_CPU_CONTROL	 0x44
#define APPLE_DCP_COPROC_CPU_CONTROL_RUN BIT(4)
#define APPLE_DCP_COPROC_CPU_STATUS 0x48
#define APPLE_DCP_COPROC_CPU_STATUS_STOPPED BIT(1)
#define APPLE_DCP_COPROC_CPU_STATUS_IDLE BIT(5)

struct dcp_session_ref {
	struct list_head link;
	struct device *dev;
	struct iommu_group *group;
};

static LIST_HEAD(dcp_retained_sessions);
static DEFINE_MUTEX(dcp_retained_sessions_lock);

static bool dcp_session_retained(struct device *dev)
{
	struct dcp_session_ref *session;
	bool retained = false;

	mutex_lock(&dcp_retained_sessions_lock);
	list_for_each_entry(session, &dcp_retained_sessions, link) {
		if (dev_fwnode(session->dev) == dev_fwnode(dev)) {
			retained = true;
			break;
		}
	}
	mutex_unlock(&dcp_retained_sessions_lock);
	return retained;
}

static int dcp_pin_live_session(struct apple_dcp *dcp)
{
	struct dcp_session_ref *session;

	if ((!dcp->hw.adopt_live_session &&
	     dcp->hw.iomfb_method_profile !=
	     DCP_IOMFB_METHODS_H17P_EXTERNAL) ||
	    !dcp_fail_stop(dcp) ||
	    dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17G || dcp->retain_dma)
		return 0;
	session = kzalloc_obj(*session);
	if (!session)
		return -ENOMEM;

	/* Keep the DMA device, domain usage and driver code valid until reset. */
	if (!try_module_get(THIS_MODULE)) {
		kfree(session);
		return -ENODEV;
	}
	session->dev = get_device(dcp->dev);
	session->group = iommu_group_get(dcp->dev);
	WRITE_ONCE(dcp->retain_dma, true);
	mutex_lock(&dcp_retained_sessions_lock);
	list_add_tail(&session->link, &dcp_retained_sessions);
	mutex_unlock(&dcp_retained_sessions_lock);
	dev_dbg(dcp->dev, "live DCP DMA resources retained until reboot\n");
	return 0;
}

static void dcp_release_dma_domain(void *data)
{
	struct apple_dcp *dcp = data;

	if (!dcp->retain_dma)
		iommu_device_unuse_default_domain(dcp->dev);
}

static void dcp_release_context(void *data)
{
	struct apple_dcp *dcp = data;

	if (!dcp->retain_dma)
		kfree(dcp);
}

#define DCP_SECURE_EP		0x30
#define DCP_SECURE_TIMEOUT_MS	200

/*
 * The firmware's secure mailbox. No other software answers it under Linux, so the
 * driver does, as macOS's secure side does: 3 is answered with 2, and 4 needs nothing.
 * What these messages mean isn't known.
 */
static void dcp_secure_recv(void *cookie, u8 ep, u64 msg)
{
	struct apple_dcp *dcp = cookie;
	int ret;

	dev_dbg(dcp->dev, "secure port: ep %#x msg %#llx\n", ep, msg);
	if (ep != DCP_SECURE_EP || (msg >> 60) != 3)
		return;

	ret = apple_rtkit_send_message(dcp->secure_rtk, ep, 2ULL << 60, NULL, false);
	if (ret)
		dev_warn(dcp->dev, "secure port: no reply sent: %d\n", ret);
}

static const struct apple_rtkit_ops dcp_secure_rtkit_ops = {
	.recv_message = dcp_secure_recv,
};

/*
 * The port's interrupts aren't in the DT. Its handshakes, idle and restart poll it themselves;
 * this answers the rest. Every jiffy for a second after its start or a message, as more tend to
 * follow, otherwise every 100 ms: woken every jiffy, an idle CPU never reaches deep idle.
 */
#define DCP_SECURE_POLL_FAST_MS	1000
#define DCP_SECURE_POLL_SLOW_MS	100

static void dcp_secure_poll(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work), struct apple_dcp,
					     secure_poll_wq);
	bool fast = time_before(jiffies, dcp->secure_fast_until);

	if (!READ_ONCE(dcp->secure_polling))
		return;

	if (apple_rtkit_poll(dcp->secure_rtk) > 0) {
		if (!fast)
			dev_info_once(dcp->dev, "secure port: a message while idle\n");
		dcp->secure_fast_until = jiffies + msecs_to_jiffies(DCP_SECURE_POLL_FAST_MS);
		fast = true;
	}
	queue_delayed_work(system_dfl_wq, &dcp->secure_poll_wq,
			   fast ? 1 : msecs_to_jiffies(DCP_SECURE_POLL_SLOW_MS));
}

static void dcp_secure_poll_start(struct apple_dcp *dcp)
{
	dcp->secure_fast_until = jiffies + msecs_to_jiffies(DCP_SECURE_POLL_FAST_MS);
	WRITE_ONCE(dcp->secure_polling, true);
	queue_delayed_work(system_dfl_wq, &dcp->secure_poll_wq, 0);
}

static void dcp_secure_poll_stop(struct apple_dcp *dcp)
{
	WRITE_ONCE(dcp->secure_polling, false);
	cancel_delayed_work_sync(&dcp->secure_poll_wq);
}

/* After the firmware's HELLO on its main mailbox. */
static void dcp_secure_port_start(struct apple_dcp *dcp)
{
	int ret;

	if (!dcp->secure_rtk)
		return;

	ret = apple_rtkit_port_handshake(dcp->secure_rtk, DCP_SECURE_TIMEOUT_MS);
	if (ret) {
		dev_warn(dcp->dev, "secure port handshake not finished: %d\n", ret);
		return;
	}
	dcp->secure_answered = true;
	dcp_secure_poll_start(dcp);
	dev_info(dcp->dev, "secure port up\n");
}

/* Between the main mailbox's AP idle and its IOP idle. */
static void dcp_secure_port_idle(void *data)
{
	struct apple_dcp *dcp = data;
	int ret;

	if (!dcp->secure_rtk || !dcp->secure_answered)
		return;

	dcp_secure_poll_stop(dcp);
	ret = apple_rtkit_port_idle(dcp->secure_rtk, DCP_SECURE_TIMEOUT_MS);
	if (ret)
		dev_warn(dcp->dev, "secure port AP idle not acknowledged: %d\n", ret);
}

static void dcp_release_secure_rtkit(void *data)
{
	struct apple_dcp *dcp = data;

	dcp_secure_poll_stop(dcp);
	apple_rtkit_free(dcp->secure_rtk);
	dcp->secure_rtk = NULL;
}

/* Nothing is read from the port until its handshake is wanted. */
static int dcp_secure_port_init(struct apple_dcp *dcp)
{
	struct device *dev = dcp->dev;

	if (of_property_match_string(dev->of_node, "mbox-names", "secure") < 0)
		return 0;

	dcp->secure_rtk = apple_rtkit_init(dev, dcp, "secure", 0, &dcp_secure_rtkit_ops);
	if (IS_ERR(dcp->secure_rtk)) {
		int ret = PTR_ERR(dcp->secure_rtk);

		dcp->secure_rtk = NULL;
		return dev_err_probe(dev, ret, "Failed to initialize the secure port\n");
	}

	INIT_DELAYED_WORK(&dcp->secure_poll_wq, dcp_secure_poll);

	return devm_add_action_or_reset(dev, dcp_release_secure_rtkit, dcp);
}

/*
 * Stopping the firmware (DPMS off and system sleep with hw.s2r or hw.s2r_ext) powers the
 * display domains through apple,display-power, and the internal DCP only takes IOP idle
 * once its secure port has gone idle. A device tree without those keeps the firmware
 * running: DPMS is the backlight alone and the display stays up across a sleep.
 */
static void dcp_s2r_check_dt(struct apple_dcp *dcp)
{
	struct device_node *np = dcp->dev->of_node;
	bool power = of_property_present(np, "apple,display-power");
	bool secure = of_property_match_string(np, "mbox-names", "secure") >= 0;

	if (dcp->hw.s2r && (!power || !secure)) {
		dev_info(dcp->dev, "no %s in the device tree, the firmware keeps running\n",
			 !power ? "apple,display-power" : "secure mailbox");
		dcp->hw.s2r = false;
	}
	if (dcp->hw.s2r_ext && !power) {
		dev_info(dcp->dev, "no apple,display-power in the device tree, the firmware keeps running\n");
		dcp->hw.s2r_ext = false;
	}
}

static void dcp_release_rtkit(void *data)
{
	struct apple_dcp *dcp = data;

	if (dcp->retain_dma) {
		WRITE_ONCE(dcp->quiescing, true);
		apple_rtkit_free_retaining_buffers(dcp->rtk);
	} else {
		apple_rtkit_free(dcp->rtk);
	}
	dcp->rtk = NULL;
}

#define DCP_BOOT_TIMEOUT msecs_to_jiffies(1000)

bool dcp_h17p_external_supported(struct device_node *np)
{
	u32 firmware[DCP_EXTERNAL_FW_CELLS], compat[DCP_EXTERNAL_FW_CELLS];
	int firmware_cells, compat_cells;

	if (!of_machine_is_compatible("apple,j700") ||
	    !of_device_is_compatible(np, "apple,t8140-dcpext"))
		return false;
	firmware_cells = of_property_read_variable_u32_array(
		np, "apple,firmware-version", firmware,
		DCP_EXTERNAL_FW_CELLS, DCP_EXTERNAL_FW_CELLS);
	compat_cells = of_property_read_variable_u32_array(
		np, "apple,firmware-compat", compat,
		DCP_EXTERNAL_FW_CELLS, DCP_EXTERNAL_FW_CELLS);
	return apple_dcp_external_firmware_supported(firmware, firmware_cells,
						    compat, compat_cells);
}

bool dcp_h17p_external_ready(struct device_node *np)
{
	struct platform_device *pdev;
	struct apple_dcp *dcp;
	bool ready = false;

	if (!dcp_h17p_external_supported(np))
		return false;
	pdev = of_find_device_by_node(np);
	if (!pdev)
		return false;
	/*
	 * Called after component binding. component_add() may synchronously
	 * bind the master before the platform probe itself returns, so the
	 * driver core's bound flag is not a readiness condition here.
	 */
	dcp = platform_get_drvdata(pdev);
	ready = dcp && READ_ONCE(dcp->external_prepared) &&
		!READ_ONCE(dcp->quiescing) && !READ_ONCE(dcp->crashed);
	put_device(&pdev->dev);
	return ready;
}

static bool show_notch;
module_param(show_notch, bool, 0644);
MODULE_PARM_DESC(show_notch, "Use the full display height and show the notch (also when /chosen/asahi,show-notch is \"1\")");

/*
 * DP/HDMI audio of external displays, on by default as on macOS: a display
 * that takes audio, over HDMI or DisplayPort (USB-C alt mode included),
 * is an audio output while it is connected. hdmi_audio=0 leaves the audio
 * endpoints stopped and registers no sound cards.
 */
bool hdmi_audio = true;
module_param(hdmi_audio, bool, 0644);
MODULE_PARM_DESC(hdmi_audio, "DP/HDMI audio of external displays (default: on)");

/*
 * Firmware 27 (H17G) internal panel: how the first mode set reaches the panel
 * iBoot already lit.  On J815 the firmware advertises a single fixed timing and
 * rejects set_digital_out_mode for it (0x8000000b, "create_default_fb_surface
 * failed"); the pipe then never scans out and the panel stays black.
 *   -1 auto: keep the boot mode when the panel advertises one timing (default)
 *    0 always send set_digital_out_mode (the previous behaviour)
 *    1 always keep the boot mode on the internal panel
 */
int fw27_panel_boot_mode = -1;
module_param(fw27_panel_boot_mode, int, 0644);
MODULE_PARM_DESC(fw27_panel_boot_mode,
		 "FW27 internal panel: -1 keep the boot mode for a single fixed timing (default), 0 always mode-set, 1 always keep the boot mode");

/*
 * update_notify_clients_dcp (A031 on H17P, A033 on H17G) form in the H17
 * client-open sequence:
 *    0 client numbers in the first slots, 9 states (default)
 *    1 client flags (clients[c] = 1), 9 states: none, 6, 7, 8, 10, 11, 12,
 *      25, 26
 *    2 client flags, 14 states: the 9, then 13, 14, 15, 16, 17
 * fw27 firmware expects the flag form.
 */
int notify_clients_form;
module_param(notify_clients_form, int, 0644);
MODULE_PARM_DESC(notify_clients_form,
		 "H17 update_notify_clients_dcp: 0 client numbers (default), 1 client flags, 9 states, 2 client flags, 14 states");

static bool unstable_edid = true;
module_param(unstable_edid, bool, 0644);
MODULE_PARM_DESC(unstable_edid, "Enable unstable EDID retrival support");

/*
 * T6041 (M4 Pro/Max) external display processors, whose nodes the boot
 * environment enables before the module loads.  t6041_dcpext=0 keeps them
 * out of the display's DRM device and off the hardware.
 */
static bool t6041_dcpext = true;
module_param(t6041_dcpext, bool, 0444);
MODULE_PARM_DESC(t6041_dcpext,
		 "T6041: drive the external display processors (default 1; 0 leaves them alone)");

/*
 * After a wake that finds the internal panel off by DPMS, start its firmware from the
 * resume, as macOS does about 83 ms into a wake, so that DPMS on only has to light the
 * panel. If DPMS on hasn't come this long after, the firmware stops again. 0: the
 * firmware waits for DPMS on, as before.
 */
static unsigned int wake_prewarm_ms = 10000;
module_param(wake_prewarm_ms, uint, 0644);
MODULE_PARM_DESC(wake_prewarm_ms,
		 "Start the internal display's firmware at resume, stop it if DPMS stays off this long (ms, 0: off)");

/*
 * With wake_prewarm_ms set, on a full wake with the lid open, also power the panel from
 * the resume, A414(0) to set_power_state(1) as macOS's power path sends them, and keep it
 * dark until the compositor's first frame, which brings back the level from before the
 * sleep. A closed lid leaves the display and its firmware off. 0: firmware only.
 */
static bool wake_light = true;
module_param(wake_light, bool, 0644);
MODULE_PARM_DESC(wake_light,
		 "With wake_prewarm_ms, also power the internal panel at resume if the lid is open (default 1)");

bool dcp_t6041_ext_enabled(void)
{
	return t6041_dcpext;
}

#include "iomfb_v26_6_swap.h"
#include "dcp-peer-v26_6.inc"
/* The firmware 27 pair shares dcp_peer_lock and dcp_peer_forget() above. */
#include "dcp-peer-v27.inc"
#include "dcp-peer-probe-v27.inc"

/*
 * The backlight maximum of a desktop 26.x panel whose node has no
 * apple,max-brightness: the J616s panel's user-accessible SDR maximum (ADT
 * user-accessible-max-nits, 0x02580000 in 16.16).
 */
#define DCP_DESKTOP_MAX_NITS	600

static bool dcp_desktop_external(struct apple_dcp *dcp)
{
	return of_device_is_compatible(dcp->dev->of_node, "apple,t6041-dcpext");
}

/*
 * The register map of the T6041 internal DCP, from the macOS ADT capture
 * (/arm-io/dcp reg[0], /arm-io/disp0 reg[0..3]).  The firmware maps these
 * windows by index, so the device tree must describe exactly these.
 */
static const struct {
	const char *name;
	u64 start;
	u64 size;
} t6041_dcp_regs[] = {
	{ "coproc", 0x402e00000ULL, 0x4000 },
	{ "disp-0", 0x400000000ULL, 0x4000000 },
	{ "disp-1", 0x402240000ULL, 0x10000 },
	{ "disp-2", 0x4023a8000ULL, 0x4000 },
	{ "disp-3", 0x402800000ULL, 0x800000 },
};

/* Refuse to touch the coprocessor on any mismatch with the captured ADT. */
static int dcp_validate_t6041_dt(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *piodma __free(device_node) =
		of_get_child_by_name(dev->of_node, "piodma");
	int i;

	if (!of_device_is_compatible(dev->of_node, "apple,t6041-dcp"))
		return 0;

	if (pdev->num_resources != ARRAY_SIZE(t6041_dcp_regs))
		return dev_err_probe(dev, -ENODEV,
				     "T6041 DCP: %u register windows, the ADT has %zu\n",
				     pdev->num_resources, ARRAY_SIZE(t6041_dcp_regs));

	for (i = 0; i < ARRAY_SIZE(t6041_dcp_regs); i++) {
		struct resource *r = platform_get_resource(pdev, IORESOURCE_MEM, i);

		if (!r || r->start != t6041_dcp_regs[i].start ||
		    resource_size(r) != t6041_dcp_regs[i].size ||
		    of_property_match_string(dev->of_node, "reg-names",
					     t6041_dcp_regs[i].name) != i)
			return dev_err_probe(dev, -ENODEV,
					     "T6041 DCP: register window %d (%s) differs from the ADT\n",
					     i, t6041_dcp_regs[i].name);
	}

	if (!of_property_present(dev->of_node, "iommus") ||
	    !of_property_present(dev->of_node, "mboxes") || !piodma)
		return dev_err_probe(dev, -ENODEV,
				     "T6041 DCP: iommus, mboxes or piodma missing\n");

	return 0;
}

/*
 * Map a buffer the desktop 26.x firmware placed itself write-combined.
 * It may be accessed through a realtime path, and a write-back alias
 * creates AMCC directory state that can fault when the DCP then accesses
 * the same memory.  The buffers may lie in RAM or in carveouts without
 * struct pages.
 */
static int dcp_vmap_wc(phys_addr_t phys, size_t size,
		       void __iomem **iomem, void **map_base)
{
	unsigned long offset = offset_in_page(phys);
	unsigned long first_pfn = PHYS_PFN(phys);
	phys_addr_t last;
	size_t span, nr_pages;
	unsigned int count, i;
	void *base;

	if (!size)
		return -EINVAL;
	if (check_add_overflow(phys, (phys_addr_t)size - 1, &last) ||
	    check_add_overflow(size, (size_t)offset, &span))
		return -EOVERFLOW;
	if (last > PHYS_MASK)
		return -EOVERFLOW;
	nr_pages = (span - 1) / PAGE_SIZE + 1;
	if (nr_pages > UINT_MAX)
		return -E2BIG;
	count = nr_pages;

	if (pfn_valid(first_pfn)) {
		struct page **pages = kmalloc_array(count, sizeof(*pages), GFP_KERNEL);

		if (!pages)
			return -ENOMEM;
		for (i = 0; i < count; i++) {
			if (!pfn_valid(first_pfn + i)) {
				kfree(pages);
				return -EINVAL;
			}
			pages[i] = pfn_to_page(first_pfn + i);
		}
		base = vmap(pages, count, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
		kfree(pages);
	} else {
		unsigned long *pfns = kmalloc_array(count, sizeof(*pfns), GFP_KERNEL);

		if (!pfns)
			return -ENOMEM;
		for (i = 0; i < count; i++) {
			if (pfn_valid(first_pfn + i)) {
				kfree(pfns);
				return -EINVAL;
			}
			pfns[i] = first_pfn + i;
		}
		base = vmap_pfn(pfns, count, pgprot_writecombine(PAGE_KERNEL));
		kfree(pfns);
	}
	if (!base)
		return -ENOMEM;

	*map_base = base;
	*iomem = (void __iomem *)(base + offset);
	return 0;
}

static int dcp_check_iova_contiguous(struct iommu_domain *domain,
				     dma_addr_t iova, size_t size,
				     phys_addr_t phys);

static int dcp_desktop_shmem_setup(struct apple_dcp *dcp,
				   struct apple_rtkit_shmem *bfr)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dcp->dev);
	phys_addr_t phy_addr;
	int ret;

	if (!domain)
		return -ENODEV;
	if (!bfr->size)
		return -EINVAL;
	if (bfr->size - 1 > (dma_addr_t)-1 - bfr->iova)
		return -EOVERFLOW;
	phy_addr = iommu_iova_to_phys(domain, bfr->iova);
	if (!phy_addr)
		return -EINVAL;
	if (offset_in_page(phy_addr) != offset_in_page(bfr->iova))
		return -EINVAL;
	if (phy_addr > PHYS_MASK || bfr->size - 1 > PHYS_MASK - phy_addr)
		return -EOVERFLOW;
	/* The CPU alias is contiguous: check the whole mapping. */
	ret = dcp_check_iova_contiguous(domain, bfr->iova, bfr->size, phy_addr);
	if (ret)
		return ret;

	ret = dcp_vmap_wc(phy_addr, bfr->size, &bfr->iomem, &bfr->private);
	if (ret)
		return ret;

	bfr->is_mapped = true;
	dev_dbg(dcp->dev, "shmem_setup: iova %pad -> pa %pa\n", &bfr->iova,
		&phy_addr);
	return 0;
}

static void disconnected_hpd_event(struct apple_connector *connector);

bool dcp_is_external_h17p(struct apple_dcp *dcp)
{
	return dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL;
}

static int dcp_external_stop_locked(struct apple_dcp *dcp, u64 generation);
static int dcp_external_disconnect(struct apple_dcp *dcp, bool physical);

/* copied and simplified from drm_vblank.c */
static void send_vblank_event(struct drm_device *dev,
		struct drm_pending_vblank_event *e,
		u64 seq, ktime_t now)
{
	struct timespec64 tv;

	if (e->event.base.type != DRM_EVENT_FLIP_COMPLETE)
		return;

	tv = ktime_to_timespec64(now);
	e->event.vbl.sequence = seq;
	/*
		* e->event is a user space structure, with hardcoded unsigned
		* 32-bit seconds/microseconds. This is safe as we always use
		* monotonic timestamps since linux-4.15
		*/
	e->event.vbl.tv_sec = tv.tv_sec;
	e->event.vbl.tv_usec = tv.tv_nsec / 1000;

	/*
	 * Use the same timestamp for any associated fence signal to avoid
	 * mismatch in timestamps for vsync & fence events triggered by the
	 * same HW event. Frameworks like SurfaceFlinger in Android expects the
	 * retire-fence timestamp to match exactly with HW vsync as it uses it
	 * for its software vsync modeling.
	 */
	drm_send_event_timestamp_locked(dev, &e->base, now);
}

/**
 * dcp_crtc_send_page_flip_event - helper to send vblank event after pageflip
 * @crtc: CRTC whose flip completed
 * @e: pending vblank event
 * @now: swap completion timestamp
 * @start: swap submission timestamp
 *
 * Compensate for unknown slack between page flip and arrival of the
 * swap_complete callback. Minimal observed duration on DCP with HDMI output
 * was around 2.3 ms. If the fb swap was submitted closer to the expected
 * swap_complete it gets a penalty of one frame duration. This is on the border
 * of unreasonable considering that Apple advertises support for 240 Hz (frame
 * duration of 4.167 ms).
 * It is unreasonable considering kwin's kms commit scheduling. Kwin commits
 * 1.5 ms + the mode's vblank time before the expected next page flip
 * completion. This results in presenting at half the display's rate for HDMI
 * outputs.
 * This might be a difference between dcp and dcpext.
 */
/*
 * Advance the CRTC's flip sequence by the number of refresh periods since the
 * previous flip (at least one), so that userspace sees a non-zero, increasing
 * vblank sequence. Without a known refresh rate every flip counts as one.
 */
static u64 dcp_crtc_next_flip_seq(struct apple_crtc *crtc, ktime_t flip)
{
	int hz = drm_mode_vrefresh(&crtc->base.mode);
	u64 frames = 1;

	if (hz > 0 && crtc->flip_seq_time) {
		s64 ns = ktime_to_ns(ktime_sub(flip, crtc->flip_seq_time));
		u64 frame_ns = div_u64(NSEC_PER_SEC, hz);

		if (ns > 0)
			frames = max_t(u64, 1,
				       div64_u64(ns + frame_ns / 2, frame_ns));
	}
	crtc->flip_seq += frames;
	crtc->flip_seq_time = flip;

	return crtc->flip_seq;
}

static void dcp_crtc_send_page_flip_event(struct apple_crtc *crtc,
					  struct drm_pending_vblank_event *e,
					  ktime_t now, ktime_t start)
{
	struct drm_device *dev = crtc->base.dev;
	u64 seq;
	unsigned int pipe = drm_crtc_index(&crtc->base);
	ktime_t flip;

	if (start != KTIME_MIN) {
		s64 delta = ktime_us_delta(now, start);
		if (delta <= 500)
			flip = now;
		else if (delta >= 2500)
			flip = ktime_sub_us(now, 1000);
		else
			flip = ktime_sub_us(now, (delta - 500) / 2);
	} else {
		flip = now;
	}
	seq = dcp_crtc_next_flip_seq(crtc, flip);
	e->pipe = pipe;
	send_vblank_event(dev, e, seq, flip);
}

/* HACK: moved here to avoid circular dependency between apple_drv and dcp */
void dcp_drm_crtc_vblank(struct apple_crtc *crtc)
{
	unsigned long flags;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event) {
		drm_crtc_send_vblank_event(&crtc->base, crtc->event);
		crtc->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
}

/* Finish an accepted atomic commit even when scanout failed. DRM has no
 * failed-flip event encoding: report completion to unblock userspace and put
 * the error on the out-fence. The core owns fence/completion release.
 */
static void dcp_failed_page_flip(struct apple_crtc *crtc,
				struct drm_pending_vblank_event *event, int error)
{
	if (!event)
		return;
	lockdep_assert_held(&crtc->base.dev->event_lock);
	if (event->base.fence)
		dma_fence_set_error(event->base.fence, error);
	drm_crtc_send_vblank_event(&crtc->base, event);
}

void dcp_drm_crtc_error(struct apple_crtc *crtc,
			struct drm_crtc_state *state, int error)
{
	struct drm_device *drm = crtc->base.dev;
	struct drm_pending_vblank_event *pending, *state_event;
	unsigned long flags;

	spin_lock_irqsave(&drm->event_lock, flags);
	pending = crtc->event;
	crtc->event = NULL;
	state_event = state ? state->event : NULL;
	if (state)
		state->event = NULL;
	dcp_failed_page_flip(crtc, pending, error);
	if (state_event != pending)
		dcp_failed_page_flip(crtc, state_event, error);
	spin_unlock_irqrestore(&drm->event_lock, flags);
}

/*
 * The flip of the commit whose flip_done is @flip_done never completed: send
 * its still pending event as failed. Only that commit's event is sent; a newer
 * commit's stays for its own swap.
 *
 * The swap started last is that flip's. Its displaced framebuffers are
 * unarmed, as the swap watchdog does, so that the next swap arms them and its
 * completion releases them. Its completion may still come, after the next
 * commit installed its event: firmware that does not track its presents
 * ignores it (dcp_swap_lost()), so that it does not retire the newer flip.
 */
void dcp_drm_crtc_flip_lost(struct apple_crtc *crtc,
			    struct completion *flip_done, int error)
{
	struct apple_dcp *dcp = platform_get_drvdata(crtc->dcp);
	unsigned long flags;
	bool lost = false;
	u32 swap_id = 0;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event && crtc->event->base.completion == flip_done) {
		dcp_failed_page_flip(crtc, crtc->event, error);
		crtc->event = NULL;
		swap_id = READ_ONCE(dcp->started_swap_id);
		dcp->lost_swap_id = swap_id;
		dcp->swap_lost = true;
		lost = true;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);

	if (lost)
		dcp_unarm_retained_framebuffers(dcp, swap_id);
}

/*
 * Is @swap_id the swap whose flip dcp_drm_crtc_flip_lost() gave up on? On
 * firmware that does not track its presents, its completion would retire
 * whichever flip is pending; it is ignored instead. Swaps complete in order,
 * so once another swap completed, the lost one will not.
 */
bool dcp_swap_lost(struct apple_dcp *dcp, u32 swap_id)
{
	unsigned long flags;
	bool lost;

	spin_lock_irqsave(&dcp->crtc->base.dev->event_lock, flags);
	lost = dcp->swap_lost && dcp->lost_swap_id == swap_id;
	dcp->swap_lost = false;
	spin_unlock_irqrestore(&dcp->crtc->base.dev->event_lock, flags);

	if (lost)
		dev_warn_ratelimited(dcp->dev,
				     "completion for lost present %u ignored\n",
				     swap_id);
	return lost;
}

void dcp_drm_crtc_page_flip(struct apple_dcp *dcp, ktime_t now)
{
	unsigned long flags;
	struct apple_crtc *crtc = dcp->crtc;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event) {
		if (crtc->event->event.base.type == DRM_EVENT_FLIP_COMPLETE)
			dcp_crtc_send_page_flip_event(crtc, crtc->event, now, dcp->swap_start);
		else
			drm_crtc_send_vblank_event(&crtc->base, crtc->event);
		crtc->event = NULL;
		dcp->swap_start = KTIME_MIN;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
}

/*
 * A desktop 26.x external processor, and T6050's on firmware 27, does not
 * hand out the sink's EDID through its DCPAV service, but exports the
 * TimingElements and DisplayAttributes of every sink it drives, on HDMI and
 * Type-C alike.
 */
static bool dcp_sink_metadata(struct apple_dcp *dcp)
{
	return dcp->hw.sink_metadata_edid;
}

bool dcp_firmware_metadata_enabled(struct apple_dcp *dcp)
{
	return dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL ||
	       dcp_sink_metadata(dcp);
}

/*
 * Such metadata belongs to the sink the firmware reports, as these
 * processors have no cable generation of their own: each sink change starts
 * a new connection, which the metadata of an earlier sink can never match.
 */
static void dcp_desktop_metadata_new_sink(struct apple_dcp *dcp)
{
	unsigned long flags;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	dcp->metadata.attributes_valid = false;
	dcp->metadata.timing_valid = false;
	dcp->metadata.epoch = (struct dcp_metadata_epoch) {
		.generation = dcp->metadata.epoch.generation + 1,
		.connected = true,
	};
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
}

void dcp_firmware_metadata_connection(struct apple_dcp *dcp, u64 generation, bool connected)
{
	unsigned long flags;

	if (!dcp_firmware_metadata_enabled(dcp))
		return;
	if (dcp_sink_metadata(dcp)) {
		dcp_desktop_metadata_new_sink(dcp);
		return;
	}
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	if (generation != dcp->metadata.epoch.generation || !connected) {
		dcp->rate_proof.connection_sequence++;
		dcp->rate_proof.sequence++;
		dcp->rate_proof.rate.valid = false;
		dcp->rate_proof.lanes.valid = false;
		dcp->metadata.attributes_valid = false;
		dcp->metadata.timing_valid = false;
		dcp->metadata.hpd_seen = false;
	}
	dcp_metadata_epoch_connection(&dcp->metadata.epoch, generation, connected);
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
}

static void dcp_firmware_metadata_present_locked(struct apple_dcp *dcp)
{
	lockdep_assert_held(&dcp->metadata.lock);
	dcp->metadata.hpd_seen = true;
	/*
	 * Fresh proof may precede HPD assertion after a same-cable loss.
	 * Keep invalidated firmware metadata and old receipt tickets revoked.
	 */
	if (READ_ONCE(dcp->typec_cable_connected) &&
	    dcp->metadata.epoch.generation == READ_ONCE(dcp->typec_generation))
		dcp->metadata.epoch.connected = true;
}

/*
 * A fact a held connection waits for has changed: evaluate it again.  The
 * unlocked checks may miss a hold that has just started; such a hold ends at
 * its timeout instead.  A stopped announcement is evaluated again when its
 * work is enabled; teardown quiesces the callers before draining the worker.
 */
static void dcp_announce_kick(struct apple_dcp *dcp)
{
	struct apple_connector *con = READ_ONCE(dcp->connector);

	if (READ_ONCE(dcp->announce.holding) && !READ_ONCE(dcp->announce.stopped) && con &&
	    READ_ONCE(con->dcp) == to_platform_device(dcp->dev))
		schedule_work(&con->hotplug_wq);
}

/*
 * While its pipe is stopped an external display reports no hotplug to DRM
 * (see dcp_external_hotplug()). When the firmware announces the display
 * there, as when the link returns from system sleep with the CRTC off, the
 * announcement still holds for the modes that need it, so record it. The
 * connector is not notified, just as for an announcement on DPMS wake.
 *
 * Only a link requested for the current cable generation can be announced:
 * a callback that arrives before that belongs to a link already released.
 */
void dcp_firmware_metadata_announced(struct apple_dcp *dcp)
{
	unsigned long flags;

	if (!dcp_firmware_metadata_enabled(dcp) ||
	    !READ_ONCE(dcp->dptxport[0].connected))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	dcp_firmware_metadata_present_locked(dcp);
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	dcp_announce_kick(dcp);
}

void dcp_firmware_metadata_hotplug(struct apple_dcp *dcp, bool connected)
{
	struct apple_connector *con = READ_ONCE(dcp->connector);
	unsigned long flags;

	if (!dcp_firmware_metadata_enabled(dcp))
		return;
	/*
	 * A desktop 26.x link can bounce without the firmware describing its
	 * sink again: the cable and HDMI HPD paths report a sink change.
	 */
	if (dcp_sink_metadata(dcp))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	if (connected) {
		dcp_firmware_metadata_present_locked(dcp);
	} else if (dcp->metadata.hpd_seen) {
		dcp->metadata.hpd_seen = false;
		dcp->rate_proof.connection_sequence++;
		dcp->rate_proof.sequence++;
		dcp->rate_proof.rate.valid = false;
		dcp->rate_proof.lanes.valid = false;
		if (con && READ_ONCE(con->dcp) == to_platform_device(dcp->dev))
			WRITE_ONCE(con->edid_invalidation_pending, true);
		dcp_metadata_epoch_connection(&dcp->metadata.epoch,
					      dcp->metadata.epoch.generation, false);
		dcp->metadata.attributes_valid = false;
		dcp->metadata.timing_valid = false;
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (connected)
		dcp_180hz_notify(dcp);
}

void dcp_firmware_metadata_begin(struct apple_dcp *dcp)
{
	unsigned long flags;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	dcp->chunks.metadata_ticket = dcp_metadata_begin(&dcp->metadata.epoch);
	dcp->chunks.received = 0;
	dcp->chunks.contiguous = true;
	dcp->chunks.payload_complete = false;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
}

static void dcp_firmware_metadata_notify(struct apple_dcp *dcp)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	ready = dcp->metadata.attributes_valid && dcp->metadata.timing_valid &&
		dcp_metadata_matches(&dcp->metadata.epoch, dcp->metadata.attributes_ticket) &&
		dcp_metadata_matches(&dcp->metadata.epoch, dcp->metadata.timing_ticket);
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (ready && dcp->connector && READ_ONCE(dcp->connector->connected))
		schedule_work(&dcp->connector->hotplug_wq);
}

static bool dcp_firmware_metadata_complete(struct apple_dcp *dcp)
{
	return dcp->chunks.contiguous && dcp->chunks.payload_complete &&
	       dcp_metadata_matches(&dcp->metadata.epoch, dcp->chunks.metadata_ticket);
}

void dcp_firmware_metadata_attributes(struct apple_dcp *dcp,
				      const struct dcp_display_metadata *attributes)
{
	unsigned long flags;

	if (!dcp_firmware_metadata_enabled(dcp))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	if (dcp_metadata_matches(&dcp->metadata.epoch, dcp->chunks.metadata_ticket)) {
		dcp->metadata.attributes_valid = attributes && dcp_firmware_metadata_complete(dcp);
		if (dcp->metadata.attributes_valid) {
			dcp->metadata.attributes = *attributes;
			dcp->metadata.attributes_ticket = dcp->chunks.metadata_ticket;
		}
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	dcp_firmware_metadata_notify(dcp);
}

void dcp_firmware_metadata_timing(struct apple_dcp *dcp)
{
	unsigned long flags;
	unsigned int i, best = UINT_MAX;

	if (!dcp_firmware_metadata_enabled(dcp))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	if (!dcp_metadata_matches(&dcp->metadata.epoch, dcp->chunks.metadata_ticket))
		goto unlock;
	dcp->metadata.timing_valid = false;
	if (!dcp_firmware_metadata_complete(dcp))
		goto unlock;
	/*
	 * The EDID's detailed timing: the preferred mode when the firmware
	 * describes it fully, otherwise the highest-scored timing it marks
	 * IsPreferred, the one the sink's own EDID lists first.  An external
	 * display's preferred mode need not be one of those.
	 */
	for (i = 0; i < dcp->nr_modes; i++) {
		const struct dcp_display_mode *m = &dcp->modes[i];

		if (!m->metadata_timing_valid)
			continue;
		if (best == UINT_MAX || (m->mode.type & DRM_MODE_TYPE_PREFERRED) ||
		    (!(dcp->modes[best].mode.type & DRM_MODE_TYPE_PREFERRED) &&
		     m->score > dcp->modes[best].score))
			best = i;
		if (m->mode.type & DRM_MODE_TYPE_PREFERRED)
			break;
	}
	if (best != UINT_MAX) {
		dcp->metadata.preferred = dcp->modes[best].mode;
		/* Desktop 26.x publishes its modes without their polarity. */
		dcp->metadata.preferred.flags |= dcp->modes[best].metadata_sync_flags;
		dcp->metadata.timing_ticket = dcp->chunks.metadata_ticket;
		dcp->metadata.timing_valid = true;
	}
unlock:
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	dcp_firmware_metadata_notify(dcp);
}

bool dcp_firmware_metadata_current(struct apple_dcp *dcp, u64 generation)
{
	unsigned long flags;
	bool valid;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	valid = !dcp->metadata.epoch.invalidated && dcp->metadata.epoch.connected &&
		generation == dcp->metadata.epoch.generation;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	return valid;
}

const struct drm_edid *dcp_firmware_edid(struct apple_dcp *dcp, u64 *generation)
{
	struct dcp_display_metadata attributes;
	struct drm_display_mode preferred;
	const struct drm_edid *edid;
	unsigned long flags;
	bool valid;

	if (!dcp_firmware_metadata_enabled(dcp))
		return ERR_PTR(-ENODEV);
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	valid = dcp->metadata.attributes_valid && dcp->metadata.timing_valid &&
		dcp_metadata_matches(&dcp->metadata.epoch, dcp->metadata.attributes_ticket) &&
		dcp_metadata_matches(&dcp->metadata.epoch, dcp->metadata.timing_ticket);
	if (valid) {
		attributes = dcp->metadata.attributes;
		preferred = dcp->metadata.preferred;
		*generation = dcp->metadata.epoch.generation;
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (!valid)
		return ERR_PTR(-ENODATA);
	edid = dcp_metadata_edid(&attributes, &preferred);
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	valid = !dcp->metadata.epoch.invalidated && dcp->metadata.epoch.connected &&
		*generation == dcp->metadata.epoch.generation;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (!valid && !IS_ERR(edid)) {
		drm_edid_free(edid);
		return ERR_PTR(-ESTALE);
	}
	return edid;
}

/*
 * m1n1 reserves the original ADT as a phram node labelled "adt". Read it
 * without an MTD dependency.
 */
void dcp_panel_read_dimensions(struct apple_dcp *dcp, struct device_node *panel_np)
{
	of_property_read_u32(panel_np, "width-mm", &dcp->panel.width_mm);
	/* Read the full height first: adj-height-mm is optional. */
	of_property_read_u32(panel_np, "height-mm", &dcp->panel.height_mm);
	if (dcp->notch_height)
		of_property_read_u32(panel_np, "adj-height-mm", &dcp->panel.height_mm);
}

/*
 * Every internal panel gets an identity, so userspace can tell it apart and
 * keep per-display settings such as its brightness.  The identity state is
 * allocated at probe (bind may fail and run again); bind fills it in.
 */
static void dcp_panel_read_adt(struct apple_dcp *dcp)
{
	bool desktop = dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP;
	struct dcp_panel_adt panel = {
		/* The J616s panel kept the product code it was first published with. */
		.identity = { .manufacturer = "APP", .product = desktop ? 0x0616 : 0 },
		.width_mm = dcp->panel.width_mm,
		.height_mm = dcp->panel.height_mm,
	};
	struct device_node *np;
	const char *label;
	struct resource res;
	void *data;
	int ret = -ENODATA;

	/* Explicit generic label when no serial or product is provided. */
	strscpy(panel.identity.name, "Apple Panel", sizeof(panel.identity.name));
	for_each_compatible_node(np, NULL, "phram") {
		if (of_property_read_string(np, "label", &label) || strcmp(label, "adt"))
			continue;
		if (of_address_to_resource(np, 0, &res) ||
		    !resource_size(&res) || resource_size(&res) > SZ_16M)
			break;
		data = memremap(res.start, resource_size(&res), MEMREMAP_WB);
		if (!data)
			break;
		ret = dcp_panel_adt_parse(data, resource_size(&res), &panel);
		memunmap(data);
		break;
	}
	of_node_put(np);
	/*
	 * The board's OF panel dimensions remain valid if ADT has none.  With
	 * the notch hidden the modes are trimmed, so keep OF's adj-height-mm.
	 */
	dcp->panel.width_mm = panel.width_mm;
	if (!dcp->notch_height)
		dcp->panel.height_mm = panel.height_mm;
	if (dcp->panel_identity)
		dcp->panel_identity->adt = panel.identity;
	dcp->panel.color_target = ret ? NULL : dcp_panel_gamut_color_space(panel.gamut);
	if (desktop || dcp->fw_compat == DCP_FIRMWARE_V_27_0)
		dcp->panel.edr_ceiling = dcp_edr_ceiling(panel.edr_potential_headroom);
	/*
	 * Firmware 27 reports no peak (desktop 26.x: res_max_panel_brightness()).
	 * The ADT's LmaxProduct gives it (1600 nits on the M5 Pro).  Only a peak
	 * above the SDR range gives headroom.
	 */
	if (dcp->fw_compat == DCP_FIRMWARE_V_27_0) {
		u32 peak = panel.lmax_product >> 16;

		if (peak > dcp->brightness.maximum && peak <= 10000) {
			dev_info(dcp->dev, "panel peak luminance %u nits (ADT)\n", peak);
			WRITE_ONCE(dcp->panel.peak_nits, peak);
		}
	}
	dev_info(dcp->dev, "panel ADT: %s, panel node %s, dimensions %ux%u mm\n",
		 ret ? "unavailable" : "parsed", panel.found ? "present" : "absent",
		 dcp->panel.width_mm, dcp->panel.height_mm);
	if (dcp->panel.edr_ceiling)
		dev_info(dcp->dev, "panel EDR headroom up to %u.%03u\n",
			 dcp->panel.edr_ceiling / 1000, dcp->panel.edr_ceiling % 1000);
}

/* Internal panel identity has no detachable Type-C service or EDID transport. */
void dcp_panel_metadata_replace(struct apple_dcp *dcp,
				const struct dcp_display_metadata *metadata)
{
	struct dcp_panel_identity *id = dcp->panel_identity;
	struct dcp_display_metadata merged;
	unsigned long flags;
	bool changed = false;

	if (!metadata || !id || !dcp_has_panel(dcp))
		return;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	merged = *metadata;
	if (id->metadata_valid && merged.product == id->metadata.product &&
	    !memcmp(merged.manufacturer, id->metadata.manufacturer, 4)) {
		const struct dcp_display_metadata *old = &id->metadata;
		bool same_serial = !merged.serial || merged.serial == old->serial;

		/* Zero/empty optional fields mean unspecified, not a new panel. */
		if (!merged.name[0])
			memcpy(merged.name, old->name, sizeof(merged.name));
		if (!merged.serial)
			merged.serial = old->serial;
		if (same_serial && !merged.serial_text[0])
			memcpy(merged.serial_text, old->serial_text, sizeof(merged.serial_text));
		/* Standalone ProductAttributes setters omit the colour description. */
		if (same_serial && !merged.chromaticity[0][0] && !merged.white[0]) {
			memcpy(merged.chromaticity, old->chromaticity, sizeof(merged.chromaticity));
			memcpy(merged.white, old->white, sizeof(merged.white));
			merged.gamma = old->gamma;
		}
		if (!merged.year)
			merged.year = old->year;
		if (!merged.week)
			merged.week = old->week;
	}
	if (!id->metadata_valid || memcmp(&id->metadata, &merged, sizeof(merged))) {
		id->metadata = merged;
		id->metadata_valid = true;
		if (!id->raw_edid_size) {
			id->generation++;
			changed = true;
		}
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (changed && dcp->connector)
		dcp_queue_hotplug(dcp->connector);
}

/* A checksum alone does not guarantee usable userspace identity strings. */
static bool dcp_panel_edid_has_identity(const u8 *raw)
{
	u16 vendor = (u16)raw[8] << 8 | raw[9];
	bool model = raw[10] || raw[11];
	bool serial = raw[12] || raw[13] || raw[14] || raw[15];

	if (vendor & 0x8000)
		return false;
	for (u32 shift = 0; shift <= 10; shift += 5)
		if (!((vendor >> shift) & 31) || ((vendor >> shift) & 31) > 26)
			return false;
	for (u32 offset = 54; offset <= 108; offset += 18) {
		const u8 *desc = raw + offset;
		bool text = false;

		if (desc[0] || desc[1] || desc[2] || desc[4])
			continue;
		for (u32 i = 5; i < 18 && desc[i] != '\n'; i++) {
			if (desc[i] < 0x20 || desc[i] > 0x7e) {
				text = false;
				break;
			}
			text |= desc[i] != ' ';
		}
		if (desc[3] == EDID_DETAIL_MONITOR_NAME)
			model |= text;
		if (desc[3] == EDID_DETAIL_MONITOR_SERIAL)
			serial |= text;
	}
	return model && serial;
}

/* Preserve a validated real EDID ahead of any metadata-derived fallback. */
void dcp_panel_edid_receive(struct apple_dcp *dcp, const void *blob, size_t len)
{
	struct dcp_panel_identity *id = dcp->panel_identity;
	const struct drm_edid *edid;
	const u8 *raw;
	size_t size;
	unsigned long flags;
	bool changed;

	if (!id || !dcp_has_panel(dcp) || parse_panel_edid_data(blob, len, &raw, &size))
		return;
	edid = drm_edid_alloc(raw, size);
	if (!edid)
		return;
	if (!drm_edid_valid(edid) || !dcp_panel_edid_has_identity(raw)) {
		drm_edid_free(edid);
		return;
	}
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	changed = size != id->raw_edid_size || memcmp(raw, id->raw_edid, size);
	if (changed) {
		memcpy(id->raw_edid, raw, size);
		id->raw_edid_size = size;
		id->generation++;
	}
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	drm_edid_free(edid);
	if (changed && dcp->connector)
		dcp_queue_hotplug(dcp->connector);
}

bool dcp_panel_metadata_current(struct apple_dcp *dcp, u64 generation)
{
	unsigned long flags;
	bool valid;

	if (!dcp->panel_identity)
		return false;
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	valid = generation == dcp->panel_identity->generation;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	return valid;
}

const struct drm_edid *dcp_panel_edid_snapshot(struct apple_dcp *dcp,
					       const struct dcp_display_mode *modes,
					       unsigned int count, u64 *generation)
{
	struct dcp_panel_identity *id = dcp->panel_identity;
	struct dcp_display_metadata metadata;
	const struct drm_edid *edid;
	u8 *raw;
	size_t size;
	unsigned long flags;

	if (!id)
		return ERR_PTR(-ENODEV);
	raw = kmalloc_objs(u8, sizeof(id->raw_edid), GFP_KERNEL);
	if (!raw)
		return ERR_PTR(-ENOMEM);
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	size = id->raw_edid_size;
	memcpy(raw, id->raw_edid, size);
	metadata = id->metadata_valid ? id->metadata : id->adt;
	*generation = id->generation;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	/* PQ content is shown as EDR up to the peak (see dcp_edr_prepare()). */
	if (READ_ONCE(dcp->panel.peak_nits)) {
		metadata.pq = true;
		metadata.static_metadata_type1 = true;
		metadata.bt2020_rgb = true;
		metadata.max_luminance = dcp->panel.peak_nits << 16;
		metadata.max_fall = metadata.max_luminance;
		metadata.min_luminance = 0;
	}
	/* The caller supplies the same retained catalog it is publishing. */
	edid = size ? drm_edid_alloc(raw, size) :
		dcp_panel_edid(&metadata, dcp->panel.color_target, modes, count,
			       dcp->panel.width_mm, dcp->panel.height_mm);
	kfree(raw);
	if (!edid)
		return ERR_PTR(-ENOMEM);
	if (!IS_ERR(edid) && !dcp_panel_metadata_current(dcp, *generation)) {
		drm_edid_free(edid);
		return ERR_PTR(-ESTALE);
	}
	return edid;
}

void dcp_set_dimensions(struct apple_dcp *dcp, u64 generation)
{
	struct apple_connector *apple_connector = READ_ONCE(dcp->connector);
	struct drm_connector *connector = apple_connector ? &apple_connector->base : NULL;
	int width_mm, height_mm, i;

	if (connector)
		mutex_lock(&connector->dev->mode_config.mutex);
	mutex_lock(&dcp->modes_lock);
	if (generation != dcp->modes_generation ||
	    apple_connector != READ_ONCE(dcp->connector))
		goto out_unlock;
	width_mm = dcp->width_mm;
	height_mm = dcp->height_mm;
	if (!width_mm || !height_mm) {
		width_mm = dcp->panel.width_mm;
		height_mm = dcp->panel.height_mm;
	}
	if (connector) {
		connector->display_info.width_mm = width_mm;
		connector->display_info.height_mm = height_mm;
	}
	for (i = 0; i < dcp->nr_modes; ++i) {
		dcp->modes[i].mode.width_mm = width_mm;
		dcp->modes[i].mode.height_mm = height_mm;
	}
out_unlock:
	mutex_unlock(&dcp->modes_lock);
	if (connector)
		mutex_unlock(&connector->dev->mode_config.mutex);
}

void dcp_update_connector_dimensions(struct apple_dcp *dcp,
				     struct drm_connector *connector)
{
	int width_mm = READ_ONCE(dcp->width_mm);
	int height_mm = READ_ONCE(dcp->height_mm);

	lockdep_assert_held(&connector->dev->mode_config.mutex);
	if (!width_mm || !height_mm) {
		width_mm = dcp->panel.width_mm;
		height_mm = dcp->panel.height_mm;
	}
	connector->display_info.width_mm = width_mm;
	connector->display_info.height_mm = height_mm;
}

bool dcp_has_panel(struct apple_dcp *dcp)
{
	return iomfb_is_m4(dcp) || dcp->panel.width_mm > 0;
}

int dcp_set_crc(struct drm_crtc *crtc, bool enabled)
{
	struct apple_crtc *ac = to_apple_crtc(crtc);
	struct apple_dcp *dcp = platform_get_drvdata(ac->dcp);

	if (iomfb_is_m4(dcp))
		return enabled ? -EOPNOTSUPP : 0;

	WRITE_ONCE(dcp->crc_enabled, enabled);

	return 0;
}

/*
 * Helper to send a DRM vblank event. We do not know how call swap_submit_dcp
 * without surfaces. To avoid timeouts in drm_atomic_helper_wait_for_vblanks
 * send a vblank event via a workqueue.
 */
static void dcp_delayed_vblank(struct work_struct *work)
{
	struct apple_dcp *dcp;

	struct apple_crtc *crtc;

	dcp = container_of(work, struct apple_dcp, vblank_wq);
	/* RTKit can report a crash before the pipeline has a CRTC. */
	crtc = READ_ONCE(dcp->crtc);
	if (!crtc)
		return;
	mdelay(5);
	dcp_drm_crtc_vblank(crtc);
}

static struct apple_dcp_afkep *dcp_afkep(struct apple_dcp *dcp, u8 endpoint)
{
	/* The auxiliary endpoints of a desktop 26.x processor. */
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP &&
	    endpoint >= 0x20 && endpoint < 0x40) {
		struct apple_dcp_afkep *ep =
			READ_ONCE(dcp->desktop->graph[endpoint - 0x20]);

		if (ep)
			return ep;
	}
	/* The J700 DCPEXT's audio service sits on endpoint 0x2b, not AV 0x29. */
	if (!IS_ERR_OR_NULL(dcp->avep) && endpoint == dcp->avep->endpoint)
		return dcp->avep;
	/* DPAV 0x2a on H17P overlaps the legacy DPTX endpoint number. */
	if (dcp->dcpavservep && endpoint == dcp->dcpavservep->endpoint)
		return dcp->dcpavservep;
	if (dcp->dptxep && endpoint == dcp->dptxep->endpoint)
		return dcp->dptxep;
	switch (endpoint) {
	case AV_ENDPOINT:
		return dcp->avep;
	case SYSTEM_ENDPOINT:
		return dcp->systemep;
	case DISP0_ENDPOINT:
		return dcp->ibootep;
	case DPTX_CONTROLLER_ENDPOINT:
		return dcp->dptx_controller_ep;
	default:
		return NULL;
	}
}

/* RX and the watchdog may race to finish one present, never both. */
bool dcp_desktop_present_complete(struct apple_dcp *dcp, u32 id)
{
	return id == READ_ONCE(dcp->desktop->present_id) &&
		xchg(&dcp->desktop->present_pending, false);
}

#define DCP_SWAP_WATCHDOG_MS		1000
#define DCP_SWAP_WATCHDOG_RETRAINS	5

/*
 * DCP drops swaps without completing them while an external pipe is not
 * enabled, for instance after a modeset that raced a Type-C sink which had
 * not asserted HPD yet. Userspace would then wait for the flip until the
 * commit times out, stalling every output it drives. Complete the flip,
 * mark the mode invalid so later commits do not wait for DCP, and let the
 * hotplug worker re-apply the active CRTC.  On H17P the dropped present is
 * also retired, so that the commands queued behind it can run.
 */
VISIBLE_IF_KUNIT void dcp_swap_watchdog(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     swap_watchdog_wq);

	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP) {
		if (time_before(jiffies, dcp->swap_watchdog_deadline) ||
		    !xchg(&dcp->desktop->present_pending, false))
			return;
		/* Retain displaced buffers until a later present really completes. */
		dcp_unarm_retained_framebuffers(dcp, READ_ONCE(dcp->desktop->present_id));
		dcp_backlight_desktop_finish(dcp, READ_ONCE(dcp->desktop->present_id),
					     true, false);
		/*
		 * The firmware did not take a frame on the adopted loader mode:
		 * set the mode after all, through the hotplug worker.
		 */
		if (xchg(&dcp->desktop->takeover_adopted, false)) {
			dev_warn(dcp->dev, "no swap completed on the adopted mode, setting it\n");
			dcp->desktop->mode_live = false;
			dcp_mode_invalidate(&dcp->mode_state);
			dcp_drm_crtc_vblank(dcp->crtc);
			if (dcp->connector)
				dcp_queue_hotplug(dcp->connector);
			return;
		}
	}

	if (!iomfb_retire_dropped_present_h17p(dcp))
		return;

	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP &&
	    READ_ONCE(dcp->vrr_enabled)) {
		WRITE_ONCE(dcp->desktop->vrr_failed, true);
		WRITE_ONCE(dcp->vrr_enabled, false);
		dev_warn(dcp->dev, "adaptive swap timed out, restoring fixed refresh\n");
	}
	dev_warn(dcp->dev, "swap not completed, retraining the display\n");
	dcp_mode_invalidate(&dcp->mode_state);
	dcp_drm_crtc_vblank(dcp->crtc);
	if (dcp->connector &&
	    dcp->swap_watchdog_retrains++ < DCP_SWAP_WATCHDOG_RETRAINS)
		dcp_queue_hotplug(dcp->connector);
}

void dcp_swap_watchdog_arm(struct apple_dcp *dcp)
{
	if (dcp_is_typec_output(dcp) || dcp_desktop_swap_watched(dcp)) {
		dcp->swap_watchdog_deadline = jiffies +
			msecs_to_jiffies(DCP_SWAP_WATCHDOG_MS);
		mod_delayed_work(system_dfl_wq, &dcp->swap_watchdog_wq,
				 msecs_to_jiffies(DCP_SWAP_WATCHDOG_MS));
	}
}

void dcp_swap_watchdog_complete(struct apple_dcp *dcp)
{
	cancel_delayed_work(&dcp->swap_watchdog_wq);
	dcp->swap_watchdog_retrains = 0;
	/* A completed swap on the adopted mode proves it. */
	dcp_takeover_adopt_proven(dcp);
}

/* A sink is attached: a Type-C route is claimed, or HDMI HPD is high. */
bool dcp_external_has_sink(struct apple_dcp *dcp)
{
	return READ_ONCE(dcp->active_typec_route) ||
	       (dcp->hdmi_hpd && gpiod_get_value_cansleep(dcp->hdmi_hpd) > 0);
}

/*
 * The firmware reports that the DisplayPort sink stopped answering, or that
 * the link did not start.  On the external H17P display, the mode set
 * before, or being set, has no link any more: drop it, also across a mode
 * set in flight, and have the hotplug worker set the mode of the CRTC again
 * or report the display lost (dcp_external_relink()).  A mode set still in
 * flight reports this itself when it finishes, and before any mode was set
 * there is nothing to recover.
 */
void dcp_external_link_fault(struct apple_dcp *dcp)
{
	dcp_180hz_invalidate(dcp);
	if (dcp_is_external_h17p(dcp) && dcp_mode_invalidate(&dcp->mode_state))
		dcp_handle_hotplug_actions(dcp, DCP_HOTPLUG_NOTIFY);
}

static void dcp_recv_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;
	struct apple_dcp_afkep *ep;

	if (READ_ONCE(dcp->quiescing))
		return;
	/* Resume from disk: the host session is being rebuilt, see dcp.c. */
	if (dcp_hib_tx_gated(dcp))
		return;

	trace_dcp_recv_msg(dcp, endpoint, message);

	/* A failed T8122 dcpext: its firmware may still talk, nobody listens. */
	if (READ_ONCE(dcp->ext_dead))
		return;

	/*
	 * H17P starts endpoints that have no AFK instance behind them (the
	 * remote allocator endpoint, for one).  Log their messages rather than
	 * dereferencing a NULL afkep or warning about an unknown endpoint.
	 */
	if (dcp_fail_stop(dcp) && endpoint != IOMFB_ENDPOINT &&
	    !dcp_afkep(dcp, endpoint)) {
		dev_dbg_ratelimited(dcp->dev, "ep %#04x: %#llx\n", endpoint,
				    message);
		return;
	}

	if (endpoint == IOMFB_ENDPOINT)
		return iomfb_recv_msg(dcp, message);

	ep = dcp_afkep(dcp, endpoint);

	if (!ep) {
		dev_warn_ratelimited(dcp->dev,
				     "dropping message for unhandled endpoint %#x\n",
				     endpoint);
		return;
	}

	afk_receive_message(ep, message);
}

static void dcp_rtk_crashed(void *cookie, const void *crashlog, size_t crashlog_size)
{
	struct apple_dcp *dcp = cookie;

	if (READ_ONCE(dcp->quiescing))
		return;

	/*
	 * A failed T8122 dcpext: its start never ran (start_done is not set
	 * up) and it has no connector. Its firmware may still report a crash.
	 */
	if (READ_ONCE(dcp->ext_dead)) {
		dcp->crashed = true;
		dev_err(dcp->dev, "unusable external display processor has crashed\n");
		return;
	}
	dcp->crashed = true;
	dcp_peer_forget(dcp);
	dev_err(dcp->dev, "DCP has crashed\n");
	if (dcp_fail_stop(dcp) &&
	    dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G) {
		/* No in-place restart exists; the last scanout stays retained. */
		dev_err(dcp->dev, "a reboot is required to restore the display\n");
		/*
		 * Nothing will complete a present that was in flight.  Before
		 * the pipeline is linked there is no event to complete.
		 */
		if (READ_ONCE(dcp->crtc))
			schedule_work(&dcp->vblank_wq);
	}
	if (dcp->connector) {
		dcp->connector->connected = 0;
		apple_connector_edid_set_live(dcp->connector, false);
		dcp_queue_hotplug(dcp->connector);
	}
	complete(&dcp->start_done);
}

/*
 * Check that every IOMMU page of [iova, iova + size) translates to the same
 * offset from @phys as it has from @iova.
 */
static int dcp_check_iova_contiguous(struct iommu_domain *domain,
				     dma_addr_t iova, size_t size,
				     phys_addr_t phys)
{
	unsigned long pgsize = domain->pgsize_bitmap ?
			       1UL << __ffs(domain->pgsize_bitmap) : PAGE_SIZE;
	dma_addr_t end = iova + size;
	dma_addr_t addr = iova;

	while (addr < end) {
		if (iommu_iova_to_phys(domain, addr) != phys + (addr - iova))
			return -EINVAL;
		addr = ALIGN_DOWN(addr, pgsize) + pgsize;
	}

	return 0;
}

static int dcp_rtk_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;
	int ret;

	if (bfr->iova && dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP)
		return dcp_desktop_shmem_setup(dcp, bfr);

	if (bfr->iova) {
		struct iommu_domain *domain =
			iommu_get_domain_for_dev(dcp->dev);
		phys_addr_t phy_addr;

		if (!domain)
			return -ENOMEM;

		// TODO: get map from device-tree
		phy_addr = iommu_iova_to_phys(domain, bfr->iova);
		if (!phy_addr)
			return -ENOMEM;
		/* Firmware 27's DVAs resolve through its own retained mappings. */
		if (dcp->fw_compat == DCP_FIRMWARE_V_27_0 &&
		    !dcp_fw_phys_in_dram(phy_addr)) {
			dev_err(dcp->dev, "shmem_setup: iova %pad is not in DRAM\n",
				&bfr->iova);
			return -EACCES;
		}

		/* memremap() below needs the whole buffer physically contiguous */
		ret = dcp_check_iova_contiguous(domain, bfr->iova, bfr->size,
						phy_addr);
		if (ret) {
			dev_err(dcp->dev,
				"shmem_setup: iova %pad (%#zx bytes) is not physically contiguous\n",
				&bfr->iova, bfr->size);
			return ret;
		}

		/* Firmware writes without CPU cache maintenance. */
		bfr->buffer = memremap(phy_addr, bfr->size, MEMREMAP_WC);
		if (!bfr->buffer)
			return -ENOMEM;

		bfr->is_mapped = true;
		dev_info(dcp->dev,
			 "shmem_setup: iova: %lx -> pa: %lx -> iomem: %lx\n",
			 (uintptr_t)bfr->iova, (uintptr_t)phy_addr,
			 (uintptr_t)bfr->buffer);
	} else {
		bfr->buffer = dma_alloc_coherent(dcp->dev, bfr->size,
						 &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;

		dev_info(dcp->dev, "shmem_setup: iova: %lx, buffer: %lx\n",
			 (uintptr_t)bfr->iova, (uintptr_t)bfr->buffer);
	}

	return 0;
}

static void dcp_rtk_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	if (bfr->is_mapped && bfr->private)
		vunmap(bfr->private);
	else if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(dcp->dev, bfr->size, bfr->buffer, bfr->iova);
}

static struct apple_rtkit_ops rtkit_ops = {
	.crashed = dcp_rtk_crashed,
	.recv_message = dcp_recv_msg,
	.shmem_setup = dcp_rtk_shmem_setup,
	.shmem_destroy = dcp_rtk_shmem_destroy,
};

static bool dcp_rtk_syslog_is_debug(void *cookie, const char *context, const char *message)
{
	return dcp_h17p_syslog_is_debug(context, message);
}

static const struct apple_rtkit_ops external_h17p_rtkit_ops = {
	.flags = APPLE_RTKIT_COPROC_PLACES_BUFFERS,
	.syslog_is_debug = dcp_rtk_syslog_is_debug,
	.crashed = dcp_rtk_crashed,
	.recv_message = dcp_recv_msg,
	.shmem_setup = dcp_rtk_shmem_setup,
	.shmem_destroy = dcp_rtk_shmem_destroy,
};

void dcp_send_message(struct apple_dcp *dcp, u8 endpoint, u64 message)
{
	bool native_14_7 = dcp->fw_compat == DCP_FIRMWARE_V_14_7;
	int ret;

	if (READ_ONCE(dcp->quiescing))
		return;
	/* Resume from disk: nothing reaches a firmware session not yet rebuilt. */
	if (dcp_hib_tx_gated(dcp))
		return;
	trace_dcp_send_msg(dcp, endpoint, message);
	/*
	 * The adopted T6030 14.7 session shares this mailbox with the panel
	 * link. A non-sleeping send fails while that FIFO is full, and the
	 * DisplayPort handshake then waits for a reply that was never sent.
	 */
	ret = apple_rtkit_send_message(dcp->rtk, endpoint, message, NULL,
				       native_14_7 ? in_atomic() : true);
	if (ret && native_14_7)
		dev_warn_ratelimited(dcp->dev, "DCP send ep %02x failed: %d\n",
				     endpoint, ret);
}

int dcp_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_commit *commit)
{
	struct platform_device *pdev = to_apple_crtc(crtc)->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct drm_crtc_state *crtc_state;
	bool needs_modeset;

	if (dcp_crtc_check_hdr(dcp, crtc, commit))
		return -EINVAL;

	if (iomfb_is_m4(dcp)) {
		struct drm_display_mode mode;
		int ret = iomfb_m4_error(dcp);

		if (READ_ONCE(dcp->quiescing))
			return -ENODEV;
		if (ret)
			return ret;
		crtc_state = drm_atomic_get_new_crtc_state(commit, crtc);
		if (!crtc_state)
			return 0;
		if (crtc_state->vrr_enabled || crtc_state->ctm ||
		    crtc_state->degamma_lut || crtc_state->gamma_lut)
			return -EINVAL;
		if (!crtc_state->enable)
			return 0;
		if (!(crtc_state->plane_mask & drm_plane_mask(crtc->primary)))
			return -EINVAL;
		ret = iomfb_m4_get_mode(dcp, &mode);
		return ret ? ret : drm_mode_equal(&crtc_state->mode, &mode) ? 0 : -EINVAL;
	}
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6)
		return iomfb_v26_6_atomic_check(dcp, crtc, commit);
	if (dcp->fw_compat == DCP_FIRMWARE_V_14_7)
		return iomfb_v14_7_atomic_check(dcp, crtc, commit);

	crtc_state = drm_atomic_get_new_crtc_state(commit, crtc);

	/*
	 * A crashed DCP can't be lit, but a commit that leaves its CRTC off has to pass:
	 * the system suspend's disable-all includes every CRTC, and a stopped external DCP
	 * would refuse every sleep after the first.
	 */
	if (dcp->crashed)
		return crtc_state && crtc_state->enable ? -EINVAL : 0;

	/* External H17P does not program adaptive sync. */
	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL &&
	    crtc_state->vrr_enabled)
		return -EINVAL;

	/*
	 * On desktop 26.6 firmware the panel takes VRR_ENABLED with the next
	 * frame, without a mode set (see dcp_vrr_update()).  The external
	 * processors ignore a new minimum rate on a live mode, so they still
	 * change adaptive sync with a mode set, as firmware 27's externals do:
	 * macOS switches between a timing's fixed and variable modes with a
	 * full mode set.
	 */
	if ((dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP ||
	     (dcp->fw_compat == DCP_FIRMWARE_V_27_0 && !dcp->main_display &&
	      !dcp_has_panel(dcp))) &&
	    crtc_state->enable && !dcp_vrr_live(dcp)) {
		struct drm_crtc_state *old =
			drm_atomic_get_old_crtc_state(commit, crtc);

		if (old && old->vrr_enabled != crtc_state->vrr_enabled)
			crtc_state->mode_changed = true;
	}

	if (dcp->fw_compat == DCP_FIRMWARE_V_27_0) {
		int ret = dcp_peer_check_v27(dcp, commit, crtc_state);

		if (ret)
			return ret;
	}

	/* DRM core only caps the blob size; the gamma update copies a full table. */
	if (crtc_state->color_mgmt_changed && crtc_state->gamma_lut &&
	    drm_color_lut_size(crtc_state->gamma_lut) != dcp_gamma_lut_size(pdev))
		return -EINVAL;

	/*
	 * A Type-C display routed to another pipeline: its route follows
	 * this CRTC if it can (see dcp_typec_follow_check()), and the mode is
	 * one of the display's, which only the pipeline driving it knows.
	 */
	if (crtc_state->enable && drm_atomic_crtc_needs_modeset(crtc_state) &&
	    dcp_typec_follows_crtc(dcp)) {
		int ret = dcp_typec_follow_check(dcp, crtc, commit,
					      &crtc_state->mode);

		if (ret)
			return ret;
	}

	needs_modeset = drm_atomic_crtc_needs_modeset(crtc_state) ||
			!READ_ONCE(dcp->mode_state.valid);
	if (!needs_modeset && (!dcp->connector || !dcp->connector->connected)) {
		/*
		 * Resume restores the mode before the firmware reports the
		 * display back, so a plane-only commit lands here while the
		 * connector is still marked disconnected.  Rejecting it makes
		 * the compositor fail every flip and give up on the output;
		 * dcp_flush() defers the commit until the link returns.
		 */
		dev_dbg(dcp->dev,
			"crtc_atomic_check: deferring commit, link still down\n");
	}

	return 0;
}

int dcp_get_connector_type(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fixed_connector_type;
}

#define DPTX_CONNECT_TIMEOUT msecs_to_jiffies(2000)
#define DPTX_TUNNEL_CONNECT_TIMEOUT msecs_to_jiffies(8000)
#define DPTX_RECONNECT_DELAY msecs_to_jiffies(1000)
#define DPTX_RECONNECT_RETRIES 5
/* How long a T8132 dcpext's connect waits for late teardown answers. */
#define DCP_T8132_ORPHAN_WAIT msecs_to_jiffies(15000)

/*
 * The connection a DPTX connect or queued reconnect belongs to: the Type-C
 * attachment generation and, for a Thunderbolt tunnel, the cookie of the
 * binding DCP admitted (zero for direct DP-alt and HDMI).
 */
static struct dcp_fabric_session dcp_session_locked(struct apple_dcp *dcp)
{
	struct dcp_fabric_session session = { .generation = dcp->typec_generation };

	lockdep_assert_held(&dcp->hpd_mutex);
	scoped_guard(mutex, &dcp->tb_lock)
		session.cookie = dcp->tb_generation;
	return session;
}

/* This tree has no tb_retiring: a route follow is the retiring handoff. */
static bool dcp_session_valid_locked(struct apple_dcp *dcp,
				     struct dcp_fabric_session session, bool cable)
{
	return dcp_fabric_session_valid(session, dcp_session_locked(dcp),
					!cable || dcp->typec_cable_connected,
					READ_ONCE(dcp->typec_follow_retiring));
}

/*
 * Recovery must not wake a deliberately parked or disabled output. Without
 * a @session (H17P) nothing is checked.
 */
static bool dcp_connect_session_valid_locked(struct apple_dcp *dcp,
					    const struct dcp_fabric_session *session,
					    bool cable, bool recovery)
{
	lockdep_assert_held(&dcp->hpd_mutex);
	if (!session)
		return true;
	return (!recovery || (dcp->typec_work_enabled &&
			     !READ_ONCE(dcp->typec_crtc_off) &&
			     !READ_ONCE(dcp->crashed))) &&
	       dcp_session_valid_locked(dcp, *session, cable);
}

static bool dcp_connect_session_valid(struct apple_dcp *dcp,
				      const struct dcp_fabric_session *session,
				      bool cable, bool recovery)
{
	if (!session)
		return true;
	guard(mutex)(&dcp->hpd_mutex);
	return dcp_connect_session_valid_locked(dcp, session, cable, recovery);
}

/* H17P (J700, T8140, T8142) connects and reconnects as before, without one. */
static bool dcp_typec_sessionless(struct apple_dcp *dcp)
{
	return dcp->fw_compat == DCP_FIRMWARE_H17P || dcp_is_external_h17p(dcp);
}

/* Capture the session at enqueue, not when a stale worker eventually runs. */
static void dcp_queue_typec_reconnect_locked(struct apple_dcp *dcp,
					     unsigned long delay)
{
	lockdep_assert_held(&dcp->hpd_mutex);
	/* H17P keeps its cable-checked reconnect. */
	if (!dcp_typec_sessionless(dcp)) {
		if (!dcp->typec_cable_connected)
			return;
		dcp->typec_reconnect_session = dcp_session_locked(dcp);
	}
	mod_delayed_work(system_freezable_wq, &dcp->typec_reconnect_wq, delay);
}

void dcp_queue_typec_reconnect(struct apple_dcp *dcp, unsigned long delay)
{
	guard(mutex)(&dcp->hpd_mutex);
	dcp_queue_typec_reconnect_locked(dcp, delay);
}

/*
 * Before a T8132 dcpext connects, wait for the late answers of an
 * earlier teardown's timed-out calls, in short slices so that a cable that
 * goes meanwhile ends the wait: its release drains this connect under the
 * fabric lock. Answers still missing after DCP_T8132_ORPHAN_WAIT are given up.
 */
static int dcp_t8132_wait_teardown_answers(struct apple_dcp *dcp, u32 port,
					   const struct dcp_fabric_session *session,
					   bool cable, bool recovery)
{
	unsigned long deadline = jiffies + DCP_T8132_ORPHAN_WAIT;

	dev_dbg(dcp->dev, "DPTX port %u connect waits for late teardown answers\n", port);
	while (!afk_compact_orphans_wait(dcp->dptxep, msecs_to_jiffies(50))) {
		/* A revoked session's release drains this work under the fabric lock. */
		if (!READ_ONCE(dcp->typec_cable_connected) ||
		    !dcp_connect_session_valid(dcp, session, cable, recovery)) {
			dev_dbg(dcp->dev, "DPTX port %u connect abandoned: the tunnel went\n",
				port);
			return -ESTALE;
		}
		if (time_after_eq(jiffies, deadline)) {
			afk_compact_orphans_drop(dcp->dptxep);
			return 0;
		}
	}
	return 0;
}

/*
 * Connect DPTX port @port. On failure, @retryable (if given) tells whether
 * the attempt left nothing outstanding in the firmware, so that a later
 * connection may simply try again: the service was not published yet, or a
 * link that never came up was withdrawn and the firmware acknowledged that.
 * With a @session, every step checks that it is still the live one, and a
 * revoked session returns -ESTALE without touching the new one's state.
 */
static int dcp_dptx_connect_raw(struct apple_dcp *dcp, u32 port, bool *retryable,
				const struct dcp_fabric_session *session, bool cable,
				bool recovery)
{
	/* T8122 dcpext: HPD only after the link wait, see dcp_ext_dptx_607() */
	bool late_hpd = dcp_ext_dptx_607(dcp);
	bool t8132 = dcp_is_t8132_ext(dcp);
	bool compact;
	unsigned long timeout;
	u64 generation;
	u8 dfp_port;
	int ret = 0, err;

	if (retryable)
		*retryable = false;
	if (!dcp->phy) {
		dev_warn(dcp->dev, "dcp_dptx_connect: missing phy\n");
		return -ENODEV;
	}
	/* A native external processor starts here; its start retries this. */
	if (dcp->external_native && !iomfb_v14_7_external_ready(dcp))
		return -EAGAIN;
	/*
	 * A T8132 dcpext connects only once the firmware has answered
	 * the calls of an earlier teardown that timed out, so that none of its
	 * late callbacks lands in this connection.
	 */
	if (t8132 && dcp->dptxep) {
		if (afk_compact_orphans_pending(dcp->dptxep)) {
			ret = dcp_t8132_wait_teardown_answers(dcp, port, session, cable,
							      recovery);
			if (ret)
				return ret;
		}
		/* Nothing of an earlier teardown is outstanding any more. */
		WRITE_ONCE(dcp->t8132_release_late, false);
	}
	/* @port selects the upstream RemotePort service/core. dptx_dfp_port
	 * is the downstream address: dpphy=0, dpin0=1, dpin1=2.
	 */
	dev_dbg(dcp->dev,
		"%s(port=%d) die=%u atc=%u dfp_port=%u tunnel=%d typec=%d route=%s conn_type=%d connected=%d\n",
		__func__, port, dcp->dptx_die, dcp->dptx_phy, dcp->dptx_dfp_port,
		dcp->dptx_tunnel, dcp_is_typec_output(dcp),
		dcp->active_typec_route ? "borrowed" : "fixed",
		dcp->connector_type, dcp->dptxport[port].connected);

	mutex_lock(&dcp->hpd_mutex);
	compact = dptx_service_available(&dcp->dptxport[port]) &&
		  READ_ONCE(dcp->dptxport[port].service->ep->compact);
	if (!dcp_connect_session_valid_locked(dcp, session, cable, recovery)) {
		ret = -ESTALE;
		goto out_unlock;
	}
	generation = dcp->typec_generation;
	if (dcp->external && dcpext_scanout_terminal(dcp)) {
		mutex_unlock(&dcp->hpd_mutex);
		return -ESHUTDOWN;
	}
	if (!dptx_service_available(&dcp->dptxport[port])) {
		dev_warn(dcp->dev, "dcp_dptx_connect: dptx service for port %d not enabled\n", port);
		ret = -ENODEV;
		/* No firmware request has been made; the first publication can retry. */
		if (retryable && !READ_ONCE(dcp->dptxport[port].closed)) {
			*retryable = true;
			ret = -EAGAIN;
		}
		goto out_unlock;
	}

	if (dcp->dptxport[port].connected)
		goto out_unlock;
	if (dcp->external)
		smp_store_release(&dcp->external_link_ready, false);

	reinit_completion(&dcp->dptxport[port].linkcfg_completion);
	WRITE_ONCE(dcp->dptxport[port].tile_hint, false);
	dcp->dptxport[port].atcphy = dcp->phy;
	/* A desktop 26.x route powers its PHY when the display connects. */
	ret = dcp_typec_phy_on(dcp);
	if (ret)
		goto out_unlock;
	/* a tiled display's second half comes in on the port's dpin1 */
	dfp_port = port && READ_ONCE(dcp->split.active) ? 2 : dcp->dptx_dfp_port;
	/* Compact DPTX (desktop 26.x) has no connection validation call. */
	if (!dcp_ext_dptx_607(dcp) && !compact) {
		ret = dptxport_validate_connection(dcp->dptxport[port].service,
						   dfp_port,
						   dcp->dptx_phy, dcp->dptx_die);
		if (ret) {
			dev_err(dcp->dev,
				"dcp_dptx_connect: failed to validate DPTX target %u:%u: %d\n",
				dcp->dptx_die, dcp->dptx_phy, ret);
			goto out_unlock;
		}
	}

	if (!dcp_connect_session_valid_locked(dcp, session, cable, recovery)) {
		ret = -ESTALE;
		goto out_unlock;
	}
	ret = dptxport_connect(dcp->dptxport[port].service,
			       dfp_port,
			       dcp->dptx_phy, dcp->dptx_die,
			       dcp_is_typec_output(dcp) && !dcp_ext_dptx_607(dcp));
	if (ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: failed to connect DPTX target %u:%u: %d\n",
			dcp->dptx_die, dcp->dptx_phy, ret);
		goto out_unlock;
	}

	if (!dcp_connect_session_valid_locked(dcp, session, cable, recovery)) {
		ret = -ESTALE;
		goto out_unlock;
	}
	ret = dptxport_request_display(dcp->dptxport[port].service);
	if (ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: failed to request display: %d\n",
			ret);
		goto out_release;
	}
	if (!dcp_connect_session_valid_locked(dcp, session, cable, recovery)) {
		ret = -ESTALE;
		goto out_release;
	}
	dcp->dptxport[port].connected = true;
	if (dcp_is_typec_output(dcp) && !late_hpd) {
		if (dcp_uses_t6020_tunnel_flow(dcp))
			ret = dptxport_set_hpd_timeout(dcp->dptxport[port].service,
						       true, 8000);
		else
			ret = dptxport_set_hpd(dcp->dptxport[port].service, true);
		if (ret) {
			dev_err(dcp->dev,
				"dcp_dptx_connect: failed to assert Type-C HPD: %d\n",
				ret);
			dcp->dptxport[port].connected = false;
			goto out_release;
		}
	}

	mutex_unlock(&dcp->hpd_mutex);
	/*
	 * The display interface of a native external processor opens once a
	 * display is attached; the firmware then describes that display.
	 */
	if (dcp->external_native) {
		ret = iomfb_v14_7_external_open(dcp);
		if (ret)
			goto out_withdraw;
	}
	timeout = dcp_uses_t6020_tunnel_flow(dcp) ?
		  DPTX_TUNNEL_CONNECT_TIMEOUT : DPTX_CONNECT_TIMEOUT;
	ret = wait_for_completion_timeout(&dcp->dptxport[port].linkcfg_completion,
					  timeout);
	/* A revoked wait must not disconnect or publish readiness for a new session. */
	if (!dcp_connect_session_valid(dcp, session, cable, recovery))
		return -ESTALE;
	if (!ret && compact) {
		/* Without a sink the link is configured once one arrives. */
		dev_dbg(dcp->dev,
			"dcp_dptx_connect: no link configuration on port %u yet\n", port);
		ret = 0;
	} else if (!ret && late_hpd) {
		/* Without HPD this firmware does not train: assert it now. */
		dev_dbg(dcp->dev,
			"dcp_dptx_connect: no link configuration on port %u yet, asserting HPD\n",
			port);
	} else if (!ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: timed out waiting for port %u link configuration\n",
			port);
		ret = -ETIMEDOUT;
		goto out_withdraw;
	} else {
		dev_dbg(dcp->dev, "dcp_dptx_connect: waited %d ms for link\n",
			jiffies_to_msecs(timeout - ret));
	}

	usleep_range(5, 10);

	if (dcp->connector_type == DRM_MODE_CONNECTOR_DisplayPort || late_hpd) {
		ret = dptxport_set_hpd(dcp->dptxport[port].service, true);
		if (ret && late_hpd)
			dev_err(dcp->dev,
				"dcp_dptx_connect: failed to assert HPD after the link wait: %d\n",
				ret);
		/* T6030 external: out_withdraw drops the request as out_disconnect did. */
		if (ret && (dcp->external || late_hpd))
			goto out_withdraw;
	}
	if (!dcp_connect_session_valid(dcp, session, cable, recovery))
		return -ESTALE;
	if (dcp->external) {
		/*
		 * The port must still have its display: a Type-C cable, or HPD
		 * on the fixed HDMI output, which has no cable flag.
		 */
		mutex_lock(&dcp->hpd_mutex);
		if (!dcp->dptxport[port].connected ||
		    !(READ_ONCE(dcp->typec_cable_connected) || dcp_hdmi_port_attached(dcp)) ||
		    READ_ONCE(dcp->crashed) || dcpext_scanout_terminal(dcp)) {
			mutex_unlock(&dcp->hpd_mutex);
			return -ENOLINK;
		}
		smp_store_release(&dcp->external_link_ready, true);
		dcpext_scanout_link_restored(dcp);
		mutex_unlock(&dcp->hpd_mutex);
	}

	if (dcp->avep)
		av_service_connect(dcp);

	/* Do not timestamp a new attachment after an older link wait. */
	if (!port && dcp_is_typec_output(dcp) && !dcp_is_usb4_output(dcp) &&
	    dcp->fw_compat != DCP_FIRMWARE_H17P && !dcp->external_native &&
	    !dcp_is_external_h17p(dcp)) {
		guard(mutex)(&dcp->hpd_mutex);
		if (dcp->typec_generation == generation && dcp->typec_cable_connected) {
			scoped_guard(spinlock_irqsave, &dcp->relink_lock)
				dcp_relink_link_up(&dcp->relink, generation, jiffies);
		}
	}

	return 0;

out_withdraw:
	mutex_lock(&dcp->hpd_mutex);
	/*
	 * A T8132 dcpext's release lowers only the HPD of a connected port, so
	 * a withdrawn one lowers its HPD here.
	 */
	if (t8132 && dcp_is_typec_output(dcp) &&
	    dptx_service_available(&dcp->dptxport[port])) {
		err = dptxport_set_hpd(dcp->dptxport[port].service, false);
		if (err)
			dev_warn(dcp->dev, "DPTX port %u withdrawn: set_hpd(0) failed: %d\n",
				 port, err);
	}
	if (!dcp_is_external_h17p(dcp)) {
		dcp->dptxport[port].connected = false;
		goto out_release;
	}
	/*
	 * No link configuration arrived, typically because the sink went away
	 * while the firmware trained it.  Withdraw the request the way a
	 * disconnect does, HPD first.  Only once the firmware has acknowledged
	 * both is nothing of this attempt outstanding; until then the port
	 * stays held.
	 */
	err = -ENODEV;
	if (dptx_service_available(&dcp->dptxport[port])) {
		err = dcp_is_typec_output(dcp) ?
		      dptxport_set_hpd(dcp->dptxport[port].service, false) : 0;
		if (!err)
			err = dptxport_release_display(dcp->dptxport[port].service);
	}
	if (err) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: port %u not withdrawn, still held: %d\n",
			port, err);
		goto out_unlock;
	}
	dev_info(dcp->dev, "dcp_dptx_connect: port %u withdrawn, the next connection retries\n",
		 port);
	dcp->dptxport[port].connected = false;
	if (retryable)
		*retryable = true;
	goto out_unlock;

out_release:
	dptxport_release_display(dcp->dptxport[port].service);

out_unlock:
	mutex_unlock(&dcp->hpd_mutex);
	return ret;
}

static void dcp_external_report_disconnected(struct apple_dcp *dcp)
{
	unsigned int action;

	guard(mutex)(&dcp->iomfb.lock);
	/* A paused callback from the old sink must not be replayed on wake. */
	dcp->external_hotplug_pending = false;
	action = dcp_mode_hotplug(&dcp->mode_state, false,
				  dcp->connector ? &dcp->connector->connected : NULL);
	if (dcp->connector) {
		WRITE_ONCE(dcp->connector->edid_invalidation_pending, true);
		action |= DCP_HOTPLUG_NOTIFY;
	}
	/* The existing worker delivers DRM notifications outside our locks. */
	dcp_handle_hotplug_actions(dcp, action);
}

static void dcp_external_publish_generation(struct apple_dcp *dcp)
{
	lockdep_assert_held(&dcp->external_lock);
	guard(mutex)(&dcp->iomfb.lock);
	dcp->external_rx_generation = dcp->external_generation;
}

/* external_lock is never taken by the serialized IOMFB receive callbacks. */
static int dcp_external_link_locked(struct apple_dcp *dcp)
{
	bool retryable;
	int ret;

	lockdep_assert_held(&dcp->external_lock);
	if (!dcp->external_prepared)
		return -EAGAIN;
	if (dcp->external_error)
		return dcp->external_error;
	if (!dcp->external_cable)
		return -ENODEV;
	ret = dcp_dptx_connect_raw(dcp, 0, &retryable, NULL, false, false);
	if (ret && !retryable)
		dcp->external_error = ret;
	return ret;
}

static int dcp_external_start_locked(struct apple_dcp *dcp)
{
	int ret;

	lockdep_assert_held(&dcp->external_lock);
	/*
	 * Once its DPTX link was withdrawn, the external DCP announces its AV
	 * interfaces again under fresh interface numbers when the link comes
	 * back, without closing the old ones. A new generation lets each
	 * replace the one it supersedes (afk_compact_slot()) instead of piling
	 * up until the endpoint has no slot left for the audio interface.
	 */
	if (!dcp->external_running)
		WRITE_ONCE(dcp->afk_power_gen, dcp->afk_power_gen + 1);
	ret = dcp_external_link_locked(dcp);
	if (ret || dcp->external_running)
		return ret;
	/* A deferred stop sent nothing: the pipe is still powered. */
	if (dcp->external_stop_deferred) {
		dcp->external_stop_deferred = false;
		dcp->external_stop_retries = 0;
		dcp->external_running = true;
		iomfb_pause_presentation(dcp, false);
		return 0;
	}
	iomfb_pause_presentation(dcp, true);
	/* Even a timed-out first activation can have changed firmware state. */
	dcp->external_power_attempted = true;
	ret = iomfb_external_power_h17p(dcp, true);
	if (ret) {
		dcp->external_error = ret;
		return ret;
	}
	dcp->external_running = true;
	dcp->external_stopped = false;
	WRITE_ONCE(dcp->pipe_enabled_h17p, true);
	WRITE_ONCE(dcp->typec_crtc_off, false);
	iomfb_pause_presentation(dcp, false);
	return 0;
}

/*
 * A T6030 external processor runs as a pipe of the main DRM device when the
 * display gate handed it its display DART: a piodma child on stream 4 of
 * that DART, and stream 0 among the display subsystem's iommus, so that
 * framebuffers are mapped for it as for the panel.
 */
bool dcp_t6030_ext_native(const struct device_node *np)
{
	struct of_phandle_args dart, args;
	struct device_node *piodma, *display;
	bool native = false;
	u32 marker;
	int i, n;

	if (!of_device_is_compatible(np, "apple,t6030-dcpext"))
		return false;
	piodma = of_get_child_by_name(np, "piodma");
	if (!piodma || !of_device_is_available(piodma) ||
	    of_property_read_u32(piodma, "apple,t6030-dispext-handoff", &marker) ||
	    marker != 1 ||
	    of_parse_phandle_with_args(piodma, "iommus", "#iommu-cells", 0, &dart)) {
		of_node_put(piodma);
		return false;
	}
	of_node_put(piodma);
	if (dart.args_count != 1 || dart.args[0] != 4 || !of_device_is_available(dart.np))
		goto out;
	display = of_find_compatible_node(NULL, NULL, "apple,t6030-display-subsystem");
	n = display && of_device_is_available(display) ?
		of_count_phandle_with_args(display, "iommus", "#iommu-cells") : 0;
	for (i = 0; i < n && !native; i++) {
		if (of_parse_phandle_with_args(display, "iommus", "#iommu-cells", i, &args))
			break;
		native = args.np == dart.np && args.args_count == 1 && !args.args[0];
		of_node_put(args.np);
	}
	of_node_put(display);
out:
	of_node_put(dart.np);
	return native;
}

/*
 * A native external processor is running, or has announced a DPTX port:
 * connect the display that is already waiting for it.
 */
void dcp_external_ready(struct apple_dcp *dcp)
{
	if (READ_ONCE(dcp->typec_cable_connected)) {
		guard(mutex)(&dcp->hpd_mutex);
		dcp->typec_reconnect_tries = 0;
		dcp_queue_typec_reconnect_locked(dcp, 0);
	} else if (dcp->hdmi_hpd && dcp->active &&
		   gpiod_get_value_cansleep(dcp->hdmi_hpd)) {
		int ret = dcp_dptx_connect(dcp, 0);

		if (ret && ret != -EAGAIN && ret != -ESTALE && ret != -ESHUTDOWN)
			dcp_external_retry(dcp, "HDMI display link not set up", ret, 1000);
	}
}

#define DCP_EXTERNAL_RETRIES	3

static int dcp_dptx_release_locked(struct apple_dcp *dcp, u32 port);

/*
 * Bounded recovery for a native external pipe: re-apply the display mode,
 * or redo the display link, up to DCP_EXTERNAL_RETRIES times per attached
 * display, @base_ms, then twice and four times as long apart. A stopped
 * firmware session cannot be recovered, and is not retried.
 */
void dcp_external_retry(struct apple_dcp *dcp, const char *why, int error,
			unsigned int base_ms)
{
	unsigned int n, delay;

	if (!dcp->external_native)
		return;
	if (iomfb_v14_7_external_failed(dcp)) {
		dev_err(dcp->dev, "%s (%d): the external display processor stopped; no retry until reboot\n",
			why, error);
		return;
	}
	if (delayed_work_pending(&dcp->external_retry_wq)) {
		dev_info(dcp->dev, "%s (%d): a retry is already queued\n", why, error);
		return;
	}
	n = atomic_inc_return(&dcp->external_retries);
	if (n > DCP_EXTERNAL_RETRIES) {
		atomic_set(&dcp->external_retries, DCP_EXTERNAL_RETRIES);
		dev_warn(dcp->dev, "%s (%d): no retry left after %u; replug the display\n",
			 why, error, DCP_EXTERNAL_RETRIES);
		return;
	}
	delay = base_ms << (n - 1);
	dev_info(dcp->dev, "%s (%d): retry %u of %u in %u ms\n", why, error, n,
		 DCP_EXTERNAL_RETRIES, delay);
	/* An attach or detach after this makes it stale; see the work. */
	WRITE_ONCE(dcp->external_retry_generation, READ_ONCE(dcp->typec_generation));
	mod_delayed_work(system_freezable_wq, &dcp->external_retry_wq, msecs_to_jiffies(delay));
}

/* The active CRTC's mode is the one the firmware runs and shows swaps in. */
static bool dcp_external_crtc_showing(struct apple_dcp *dcp)
{
	struct drm_crtc *crtc = dcp->crtc ? &dcp->crtc->base : NULL;
	bool showing = false;

	if (!crtc)
		return false;
	drm_modeset_lock(&crtc->mutex, NULL);
	if (crtc->state && crtc->state->active)
		showing = iomfb_v14_7_external_showing(dcp, &crtc->state->mode);
	drm_modeset_unlock(&crtc->mutex);
	return showing;
}

void dcp_external_retry_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work), struct apple_dcp,
					     external_retry_wq);
	struct apple_connector *connector = READ_ONCE(dcp->connector);
	u64 generation = READ_ONCE(dcp->external_retry_generation);
	bool released;

	if (READ_ONCE(dcp->typec_generation) != generation) {
		dev_info(dcp->dev, "display retry: the display was attached or detached since; dropped\n");
		return;
	}
	/* Kept connected through a withdrawal, and not described again. */
	released = iomfb_v14_7_external_release(dcp);
	if (iomfb_v14_7_external_failed(dcp)) {
		dev_err(dcp->dev, "display retry: the external display processor stopped; no retry until reboot\n");
		return;
	}
	/* Described again: re-apply the mode if it is not set. */
	if (!released && connector && READ_ONCE(connector->connected)) {
		if (READ_ONCE(dcp->mode_state.valid)) {
			dev_info(dcp->dev, "display retry: the display is back and set\n");
			return;
		}
		if (dcp_external_crtc_showing(dcp)) {
			dev_info(dcp->dev, "display retry: the display is back in its mode and showing\n");
			dcp_mode_set_valid(&dcp->mode_state, true);
			return;
		}
		dev_info(dcp->dev, "display retry: setting the display mode again\n");
		dcp_mode_invalidate(&dcp->mode_state);
		dcp_queue_hotplug(connector);
		return;
	}
	/* An HDMI output connects through the fabric, as on its HPD. */
	if (!dcp_is_typec_output(dcp) && dcp->hdmi_hpd) {
		dcp_fabric_hdmi_retry(dcp);
		return;
	}
	if (!READ_ONCE(dcp->typec_cable_connected)) {
		dev_info(dcp->dev, "display retry: the port has no display any more\n");
		return;
	}
	/*
	 * The port holds a display that the firmware no longer describes:
	 * release the link if it is still up, and connect it again, which
	 * makes the firmware describe the display anew.
	 */
	scoped_guard(mutex, &dcp->hpd_mutex) {
		/* Not under a connect or release of another display. */
		if (dcp->typec_generation != generation || !dcp->typec_cable_connected) {
			dev_info(dcp->dev, "display retry: the display was attached or detached since; dropped\n");
			return;
		}
		dev_info(dcp->dev, "display retry: reconnecting the display link\n");
		if (dptx_service_available(&dcp->dptxport[0]) && dcp->dptxport[0].connected) {
			int ret = dptxport_set_hpd(dcp->dptxport[0].service, false);

			if (ret) {
				dev_warn(dcp->dev, "display retry: HPD deassert failed: %d\n", ret);
				return;
			}
			ret = dcp_dptx_release_locked(dcp, 0);
			if (ret)
				return;
		}
		dcp->typec_reconnect_tries = 0;
	}
	guard(mutex)(&dcp->hpd_mutex);
	dcp_queue_typec_reconnect_locked(dcp, 0);
}

/*
 * The sink of a native external pipe raised IRQ_HPD with HPD staying high,
 * as a DP branch device does when the display behind it changes. Pass the
 * request to the firmware, which services it as a DP source does. If it
 * cannot take the request, connect the display link anew, which makes the
 * firmware read the display again.
 */
void dcp_external_sink_irq(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	struct apple_epic_service *service = READ_ONCE(dcp->dptxport[0].service);
	int ret;

	/* With no link up, the next connect reads the display anyway. */
	if (!dcp->external_native || !service || !dptx_service_available(&dcp->dptxport[0]) ||
	    !READ_ONCE(dcp->dptxport[0].connected))
		return;
	ret = dptxport_sink_irq(service);
	if (!ret) {
		dev_info(dcp->dev, "display sink IRQ_HPD passed to the firmware\n");
		return;
	}
	dev_info(dcp->dev, "display sink IRQ_HPD not passed (%d): connecting the display anew\n",
		 ret);
	ret = dcp_dptx_disconnect_oob(pdev, 0);
	if (ret)
		return;
	dcp_dptx_connect_oob(pdev, 0);
}

/*
 * tiled_split: DPTX port 1 carries the second half of a tiled display on
 * this pipeline. Bring it back whenever port 0 (re)connects, as DCP pairs
 * the tiles only while both ports are connected.
 */
static int dcp_dptx_connect_tile(struct apple_dcp *dcp)
{
	bool split;
	int ret;

	scoped_guard(mutex, &dcp->tb_lock)
		split = dcp->split.active && dcp->split.ready &&
			dcp->split.generation;
	if (!split)
		return 0;
	ret = dcp_dptx_connect(dcp, 1);
	if (ret)
		dev_warn(dcp->dev, "tiled: DPTX port 1 reconnect failed: %d\n", ret);
	return ret;
}

/*
 * Desktop 26.x external processors (T6041 dcpext0 HDMI, dcpext1 USB-C).
 * The DPTX link is set up on HPD (HDMI) or when the Type-C display fabric
 * routes a port's DP alt mode to the processor (USB-C).
 */
static void dcp_hdmi_converter_power(struct apple_dcp *dcp, bool on);

static int dcp_desktop_dptx_connect(struct apple_dcp *dcp, u32 port)
{
	struct dptx_port *dptx = &dcp->dptxport[port];
	unsigned long left;
	int ret = 0;

	mutex_lock(&dcp->hpd_mutex);
	if (!dcp->phy) {
		dev_warn(dcp->dev, "dcp_dptx_connect: missing phy\n");
		ret = -ENODEV;
		goto out_unlock;
	}
	/* A fabric route powers its PHY here (a no-op for the other routes). */
	ret = dcp_typec_phy_on(dcp);
	if (ret)
		goto out_unlock;
	if (!dptx_service_available(dptx)) {
		dev_warn(dcp->dev, "dcp_dptx_connect: dptx service for port %u not enabled\n",
			 port);
		ret = -ENODEV;
		goto out_unlock;
	}
	if (dptx->connected)
		goto out_unlock;

	/*
	 * The DP-to-HDMI converter is powered for its link and each link starts
	 * from its power-up, as the port controller has it (it powers the
	 * converter when the port is connected to a pipe, before the port is
	 * activated). Not while the other port's link runs through it, and not
	 * again for a link dcp_hib_ext_relink() has just powered it up for.
	 */
	if (dcp->dp2hdmi_pwren && !dcp->dptxport[!port].connected) {
		if (!dcp->hdmi_converter_fresh)
			dcp_hdmi_converter_power(dcp, true);
		dcp->hdmi_converter_fresh = false;
	}

	reinit_completion(&dptx->linkcfg_completion);
	WRITE_ONCE(dptx->link_configured, false);
	dptx->atcphy = dcp->phy;
	ret = dptxport_connect(dptx->service, 0, dcp->dptx_phy, dcp->dptx_die, true);
	if (ret) {
		dev_warn(dcp->dev, "DPTX port %u: connect failed: %d\n", port, ret);
		goto out_unlock;
	}
	ret = dptxport_request_display(dptx->service);
	if (ret) {
		dev_warn(dcp->dev, "DPTX port %u: display request failed: %d\n",
			 port, ret);
		goto out_unlock;
	}
	dptx->connected = true;

	/*
	 * The firmware does not take HPD from the DP2HDMI converter on its
	 * own ("HPD not present, skipping mode export"): assert it right
	 * after the display request.
	 */
	if (READ_ONCE(dptx->service->ep->compact)) {
		ret = dptxport_set_hpd(dptx->service, true);
		if (ret)
			dev_warn(dcp->dev, "DPTX port %u: set_hpd(1): %d\n", port, ret);
		ret = 0;
	}
	/* Connected without its display: the link starts over when HPD comes. */
	WRITE_ONCE(dcp->hdmi_resync,
		   dcp->hdmi_hpd && gpiod_get_value_cansleep(dcp->hdmi_hpd) <= 0);
	mutex_unlock(&dcp->hpd_mutex);

	/* Without a sink the link is configured once one arrives. */
	left = wait_for_completion_timeout(&dptx->linkcfg_completion,
					   DPTX_CONNECT_TIMEOUT);
	if (!left)
		dev_dbg(dcp->dev, "DPTX port %u: no link configuration yet\n", port);

	usleep_range(5, 10);

	if (dcp->avep)
		av_service_connect(dcp);
	return 0;

out_unlock:
	mutex_unlock(&dcp->hpd_mutex);
	return ret;
}

static int dcp_desktop_dptx_disconnect_oob(struct apple_dcp *dcp, u32 port)
{
	int ret;

	disconnected_hpd_event(dcp->connector);
	if (dcp->avep)
		av_service_disconnect(dcp);
	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (dptx_service_available(&dcp->dptxport[port]))
			dptxport_set_hpd(dcp->dptxport[port].service, false);
	}
	ret = dcp_dptx_disconnect(dcp, port);
	if (!dcp->main_display)
		dcp->desktop->ext_pipe_off = true;
	return ret;
}

/*
 * HDMI HPD of a desktop 26.x external processor.  The firmware takes HPD
 * from the host only, with the connection: a display that arrives is
 * connected, one that leaves is released (nothing else reports an unplug:
 * the DP-to-HDMI converter keeps its DP link up without a sink).  Each plug
 * is one link start from the converter's power-up: the port is connected at
 * HPD and the converter is powered with the connection.
 *
 * The firmware must not be given HPD off and on again under a link it is
 * bringing up.  Once the link has trained and the firmware has set up the
 * converter's HDMI side, it takes a new HPD as the same link: it trains the
 * DP side again, skips the HDMI side and shows the display with a mode set,
 * and the display stays black until its link is released.  On the J616s a
 * 100 ms HPD pulse sent 0.6 s or later after the first training, and before
 * the display was shown, ended black every time; a release and a new
 * connection at the same moment did not.  So an HPD change under a connected
 * port starts the link over through a release, see dcp_desktop_hdmi_restart().
 */
/* The HDMI display gives its pipeline up: report it gone, release its link. */
void dcp_hdmi_yield_link(struct apple_dcp *dcp)
{
	int tries = 5, ret;

	disconnected_hpd_event(dcp->connector);
	/* A link the firmware keeps retrying is released between attempts. */
	while ((ret = dcp_dptx_disconnect(dcp, 0)) && --tries)
		msleep(600);
	if (ret)
		dev_warn(dcp->dev, "HDMI link not released: %d\n", ret);
}

#define DCP_HDMI_START_TRIES	3

/*
 * The fixed output is connected and its display asserts HPD, but the firmware
 * found no active sink behind the converter (seen when the port is connected
 * within about 2 s of the +5 V: the converter has not found the display
 * yet). It then waits for another HPD event, which only the host can give.
 * Release a display whose sink is inactive and request it again while HPD
 * stays: start the link again, a few times.
 */
static void dcp_desktop_hdmi_unpark(struct apple_dcp *dcp)
{
	struct dptx_port *dptx = &dcp->dptxport[0];
	int tries, ret;

	for (tries = 1; tries <= DCP_HDMI_START_TRIES; tries++) {
		if (!READ_ONCE(dptx->connected) || READ_ONCE(dptx->link_configured) ||
		    READ_ONCE(dcp->active_typec_route) || READ_ONCE(dcp->hdmi_yielded) ||
		    gpiod_get_value_cansleep(dcp->hdmi_hpd) <= 0)
			return;
		dev_info(dcp->dev, "HDMI: no sink behind the converter yet, link started again (%d/%d)\n",
			 tries, DCP_HDMI_START_TRIES);
		ret = dcp_desktop_dptx_disconnect_oob(dcp, 0);
		if (ret)
			dev_warn(dcp->dev, "HDMI link not released: %d\n", ret);
		dcp_dptx_connect(dcp, 0);
	}
}

/*
 * The display behind a connected port changed, or came after the port was
 * connected: release the link and connect again, converter power-up included.
 */
static void dcp_desktop_hdmi_restart(struct apple_dcp *dcp)
{
	int ret;

	dev_info(dcp->dev, "HDMI: HPD changed under the connected port, link started again\n");
	ret = dcp_desktop_dptx_disconnect_oob(dcp, 0);
	if (ret)
		dev_warn(dcp->dev, "HDMI link not released: %d\n", ret);
	dcp_dptx_connect(dcp, 0);
	dcp_desktop_hdmi_unpark(dcp);
}

irqreturn_t dcp_desktop_dp2hdmi_hpd(struct apple_dcp *dcp)
{
	struct dptx_port *dptx = &dcp->dptxport[0];
	bool connected;

	/*
	 * A level that is low now, or after the settle time, is a real change.
	 * An edge latched before the interrupt was enabled finds the level the
	 * port was connected at.
	 */
	if (gpiod_get_value_cansleep(dcp->hdmi_hpd) <= 0)
		WRITE_ONCE(dcp->hdmi_resync, true);
	msleep(500);
	connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
	dev_dbg(dcp->dev, "DP2HDMI HPD irq, connected:%d after 500 ms\n", connected);
	if (!connected) {
		WRITE_ONCE(dcp->hdmi_resync, true);
		/*
		 * A yield outlasts HDMI blinks (a monitor reconfiguring its
		 * inputs drops HPD on all of them): it ends with the Type-C
		 * display, not with HPD here.
		 */
		if (!READ_ONCE(dcp->active_typec_route) && dcp->connector &&
		    READ_ONCE(dcp->connector->connected)) {
			dev_info(dcp->dev, "HDMI display unplugged\n");
			disconnected_hpd_event(dcp->connector);
			dcp_dptx_disconnect(dcp, 0);
			dcp_hdmi_converter_power(dcp, false);
			dcp_fabric_hdmi_unplugged(dcp);
		}
		return IRQ_HANDLED;
	}
	/*
	 * The pipeline drives a Type-C display, or is about to: the HDMI
	 * display waits for it, and gets it back once that one fits one pipe.
	 */
	if (READ_ONCE(dcp->active_typec_route) || READ_ONCE(dcp->hdmi_yielded)) {
		dev_info(dcp->dev, "HDMI display waits: its pipeline drives a Type-C display\n");
		dcp_fabric_hybrid_kick(dcp, 1000);
		return IRQ_HANDLED;
	}

	mutex_lock(&dcp->hpd_mutex);
	if (dptx_service_available(dptx) && dptx->connected &&
	    READ_ONCE(dptx->service->ep->compact)) {
		mutex_unlock(&dcp->hpd_mutex);
		/*
		 * An edge from before the connection changes nothing, unless
		 * the firmware still waits for the display's sink.
		 */
		if (READ_ONCE(dcp->hdmi_resync))
			dcp_desktop_hdmi_restart(dcp);
		else
			dcp_desktop_hdmi_unpark(dcp);
		return IRQ_HANDLED;
	}
	mutex_unlock(&dcp->hpd_mutex);

	dcp_dptx_connect(dcp, 0);
	dcp_desktop_hdmi_unpark(dcp);
	return IRQ_HANDLED;
}

int dcp_dptx_connect(struct apple_dcp *dcp, u32 port)
{
	struct dcp_fabric_session session = {};
	int ret;

	/* The fixed HDMI output of a desktop 26.x processor has no Type-C session. */
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP && !dcp_is_typec_output(dcp))
		return dcp_desktop_dptx_connect(dcp, port);

	if (!dcp_is_external_h17p(dcp)) {
		if (dcp_typec_sessionless(dcp)) {
			ret = dcp_dptx_connect_raw(dcp, port, NULL, NULL, false, false);
		} else {
			scoped_guard(mutex, &dcp->hpd_mutex)
				session = dcp_session_locked(dcp);
			/* A tunnel's connection also needs its cable; DP-alt/HDMI do not. */
			ret = dcp_dptx_connect_raw(dcp, port, NULL, &session,
						   !!session.cookie, false);
		}
		if (!ret && !port)
			ret = dcp_dptx_connect_tile(dcp);
		return ret;
	}
	guard(mutex)(&dcp->external_lock);
	return dcp_external_link_locked(dcp);
}

#define DCP_EXTERNAL_STOP_RETRIES	3

/* The generation token belongs to the CRTC that was actually enabled. */
static int dcp_external_stop_locked(struct apple_dcp *dcp, u64 generation)
{
	const char *step;
	int ret;

	lockdep_assert_held(&dcp->external_lock);
	if (generation != dcp->external_generation)
		return 0;
	if (dcp->external_error)
		return dcp->external_error;
	if (dcp->external_stopped && !READ_ONCE(dcp->dptxport[0].connected))
		return 0;
	/*
	 * The sink's audio first, as around a 14.7 external mode set: a link
	 * the audio client still holds when the pipe powers off and the DPTX
	 * link is withdrawn left the next activation timing out (-ETIMEDOUT)
	 * and the display dark. It comes back with the link
	 * (dcp_dptx_connect_raw()), before the pipe is powered on again.
	 */
	if (dcp->avep)
		av_service_disconnect(dcp);
	iomfb_pause_presentation(dcp, true);
	dcp->external_running = false;
	/* Route setup can call this before IOMFB or its shared memory exists. */
	if (dcp->external_stopped || !dcp->external_power_attempted)
		goto release_link;
	step = "settling the queue";
	ret = iomfb_queue_settle(dcp, msecs_to_jiffies(500));
	if (ret == -ETIMEDOUT) {
		/*
		 * Nothing was sent, so no late reply can race a retry.  The
		 * session stays owned, but not latched: a SAFE transition, the
		 * retry work or the next enable of the CRTC takes it from here.
		 */
		dev_warn(dcp->dev, "external display stop deferred: IOMFB queue busy\n");
		dcp->external_stop_deferred = true;
		if (dcp->external_stop_retries++ < DCP_EXTERNAL_STOP_RETRIES)
			mod_delayed_work(system_dfl_wq, &dcp->external_stop_retry_wq,
					 msecs_to_jiffies(DCP_SWAP_WATCHDOG_MS));
		return ret;
	}
	dcp->external_stop_deferred = false;
	dcp->external_stop_retries = 0;
	if (!ret) {
		step = "powering the pipe off";
		ret = iomfb_external_power_h17p(dcp, false);
	}
	if (!ret) {
		step = "settling the queue after power-off";
		ret = iomfb_queue_settle(dcp, msecs_to_jiffies(500));
	}
	if (ret)
		goto failed;
	/* This is a transport acknowledgment, not permission to free scanout. */
	WRITE_ONCE(dcp->pipe_enabled_h17p, false);
release_link:
	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (dcp->dptxport[0].connected) {
			step = "withdrawing the DPTX link";
			if (!dptx_service_available(&dcp->dptxport[0])) {
				ret = -ENODEV;
				goto failed;
			}
			ret = dptxport_set_hpd(dcp->dptxport[0].service, false);
			if (!ret)
				ret = dptxport_release_display(dcp->dptxport[0].service);
			if (ret)
				goto failed;
			dcp->dptxport[0].connected = false;
		}
	}
	dcp->external_stopped = true;
	WRITE_ONCE(dcp->typec_crtc_off, true);
	return 0;
failed:
	/* A retry could race a late reply. Keep this session blocked and owned. */
	dev_err(dcp->dev, "external display stop failed %s, session held: %d\n", step, ret);
	dcp->external_error = ret;
	return ret;
}

static int dcp_external_disconnect(struct apple_dcp *dcp, bool physical)
{
	int ret;

	cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
	cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
	guard(mutex)(&dcp->external_lock);
	/* Invalidate physical EDID before waiting for firmware power/link teardown. */
	if (physical) {
		WRITE_ONCE(dcp->external_physically_absent, true);
		scoped_guard(mutex, &dcp->hpd_mutex) {
			WRITE_ONCE(dcp->typec_cable_connected, false);
			dcp->typec_generation++;
			dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
			dcp_announce_end(&dcp->announce);
		}
		/* HPD is fenced; retire the AV owner after releasing hpd_mutex. */
		dpavservep_detach(dcp);
	}
	ret = dcp_external_stop_locked(dcp, dcp->external_generation);
	if (physical) {
		dcp->external_cable = false;
		dcp->external_generation++;
		dcp_external_publish_generation(dcp);
		/* A missing cable is reported even when its hardware remains owned. */
		dcp_external_report_disconnected(dcp);
	}
	return ret;
}

/* Stop a deferred session again once the swap watchdog had its turn. */
static void dcp_external_stop_retry(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work), struct apple_dcp,
					     external_stop_retry_wq);

	guard(mutex)(&dcp->external_lock);
	if (dcp->external_stop_deferred)
		dcp_external_stop_locked(dcp, dcp->external_generation);
}

static void dcp_external_stop_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(work, struct apple_dcp, external_stop_work);

	u64 generation;

	scoped_guard(mutex, &dcp->iomfb.lock)
		generation = dcp->external_stop_generation;
	cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
	cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
	guard(mutex)(&dcp->external_lock);
	if (generation == dcp->external_generation) {
		/* The deferred disconnect also covers missing out-of-band HPD. */
		dpavservep_detach(dcp);
		dcp_external_stop_locked(dcp, generation);
		dcp_external_report_disconnected(dcp);
	}
}

bool dcp_external_hotplug(struct apple_dcp *dcp, bool connected)
{
	if (!dcp_is_external_h17p(dcp))
		return false;
	/* No lifecycle or HPD mutex may be acquired from the RX callback. */
	lockdep_assert_held(&dcp->iomfb.lock);
	if (READ_ONCE(dcp->quiescing) || READ_ONCE(dcp->external_physically_absent))
		return true;
	if (dcp->iomfb.presentation_paused) {
		dcp->external_hotplug_pending = true;
		dcp->external_hotplug_connected = connected;
		if (connected)
			dcp_firmware_metadata_announced(dcp);
		/* Publish initial/new-cable discovery, never same-cable DPMS wake. */
		if (connected && READ_ONCE(dcp->external_cable) &&
		    !READ_ONCE(dcp->external_error) &&
		    (!READ_ONCE(dcp->external_power_attempted) ||
		     (READ_ONCE(dcp->external_stopped) &&
		      READ_ONCE(dcp->external_rx_generation) !=
		      READ_ONCE(dcp->external_crtc_generation) &&
		      READ_ONCE(dcp->dptxport[0].connected)))) {
			unsigned int action = dcp_mode_hotplug(&dcp->mode_state, true,
							     dcp->connector ?
							     &dcp->connector->connected : NULL);

			dcp_handle_hotplug_actions(dcp, action);
		}
		return true;
	}
	if (!connected && !READ_ONCE(dcp->iomfb.presentation_paused)) {
		/* Fence admission in RX before a busy actor can delay teardown. */
		dcp_firmware_metadata_hotplug(dcp, false);
		dcp->external_stop_generation = dcp->external_rx_generation;
		schedule_work(&dcp->external_stop_work);
		return true;
	}
	return READ_ONCE(dcp->iomfb.presentation_paused);
}

static bool dcp_edid_is_placeholder(const struct drm_edid *drm_edid)
{
	const u8 *raw = (const u8 *)drm_edid_raw(drm_edid);
	static const u8 name[] = "Non-PnP";
	unsigned int i;

	if (!raw)
		return false;

	/* EDID contains interior NUL bytes, so this cannot use strnstr(). */
	for (i = 0; i + sizeof(name) - 1 <= sizeof(struct edid); i++) {
		if (!memcmp(raw + i, name, sizeof(name) - 1))
			return true;
	}

	return false;
}

void dcp_retry_placeholder_edid(struct apple_dcp *dcp,
				const struct drm_edid *drm_edid)
{
	guard(mutex)(&dcp->hpd_mutex);
	if (!dcp_is_typec_output(dcp) || !dcp->typec_cable_connected ||
	    dcp->placeholder_retried)
		return;
	if (!dcp_edid_is_placeholder(drm_edid))
		return;

	dcp->placeholder_retried = true;
	dcp->placeholder_generation = dcp->typec_generation;
	schedule_delayed_work(&dcp->placeholder_edid_wq, msecs_to_jiffies(300));
}

static void dcp_placeholder_edid_work(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     placeholder_edid_wq);
	struct apple_epic_service *service;
	u64 generation;
	int ret;

	mutex_lock(&dcp->hpd_mutex);
	generation = dcp->placeholder_generation;
	if (!dcp->typec_cable_connected || !dcp->dptxport[0].connected ||
	    !dptx_service_available(&dcp->dptxport[0]) || generation != dcp->typec_generation)
		goto out_unlock;
	service = dcp->dptxport[0].service;

	/*
	 * Some adapters answer the first connection with a 1024x768
	 * placeholder and publish the panel EDID only after HPD drops
	 * and returns. One pulse; a second placeholder is left alone.
	 */
	WRITE_ONCE(dcp->typec_hpd_held, true);
	ret = dptxport_set_hpd(service, false);
	if (ret) {
		dev_info(dcp->dev, "placeholder EDID: HPD drop failed: %d\n",
			 ret);
		goto out_unlock;
	}
	mutex_unlock(&dcp->hpd_mutex);

	msleep(1000);

	mutex_lock(&dcp->hpd_mutex);
	if (!dcp->typec_cable_connected || !dcp->dptxport[0].connected ||
	    generation != dcp->typec_generation)
		goto out_unlock;

	ret = dptxport_set_hpd(service, true);
	if (ret)
		dev_info(dcp->dev, "placeholder EDID: HPD assert failed: %d\n",
			 ret);
out_unlock:
	WRITE_ONCE(dcp->typec_hpd_held, false);
	mutex_unlock(&dcp->hpd_mutex);
}

#define DCP_LINK_RECOVERY_TRIES		4
#define DCP_LINK_RECOVERY_DELAY_MS	2000

/*
 * A retrain after a hotplug found no mode for the active CRTC: the sink's
 * EDID was incomplete (it had just woken from sleep and listed only its base
 * modes).  Nothing re-reads it until the next HPD event, which can be a
 * minute away, so a Type-C display stays at a fallback mode.  Connect the
 * link again, as a replug does, unless a mode set completed in between.
 */
void dcp_link_recovery_schedule(struct apple_dcp *dcp)
{
	if (dcp->fw_compat != DCP_FIRMWARE_V_26_6_DESKTOP || dcp->main_display ||
	    !dcp_is_typec_output(dcp) || !READ_ONCE(dcp->typec_cable_connected) ||
	    dcp->link_recovery_tries >= DCP_LINK_RECOVERY_TRIES ||
	    dcp_wake_reread_pending(dcp))
		return;
	dcp->link_recovery_snap = atomic_read(&dcp->modesets_done);
	mod_delayed_work(system_freezable_wq, &dcp->link_recovery_wq,
			 msecs_to_jiffies(DCP_LINK_RECOVERY_DELAY_MS));
}

#define DCP_WAKE_REREAD_DELAY_MS	500
#define DCP_WAKE_REREAD_MS		500

/* A display's list is being read again after a wake: dcp_wake_list_check(). */
bool dcp_wake_reread_pending(struct apple_dcp *dcp)
{
	unsigned long until = READ_ONCE(dcp->wake_reread);

	return until && time_before(jiffies, until);
}

/*
 * A display that the link of a wake has just woken can answer the first read
 * with the modes of its standby, and gives no new HPD when its own are there
 * a second later: a G95NC in picture-by-picture mode lists its full-width
 * modes and none of its half.  The desktop then finds no mode for it, and
 * dcp_link_recovery_schedule() reads it again only if a retrain happened to
 * run against that list.  So the first list after a wake is held against the
 * one the sleep found (dcp_hib_release_ext_link()), and a display that lists
 * other modes is connected again once, as a replug does, whatever it lists
 * then.  Called from the connector's hotplug work.
 */
void dcp_wake_list_check(struct apple_dcp *dcp)
{
	u32 sig;

	if (dcp->fw_compat != DCP_FIRMWARE_V_26_6_DESKTOP || dcp->main_display ||
	    !READ_ONCE(dcp->wake_list_sig) || !dcp->nr_modes)
		return;
	sig = xchg(&dcp->wake_list_sig, 0);
	if (!sig || !dcp_is_typec_output(dcp) ||
	    !READ_ONCE(dcp->typec_cable_connected) || dcp_modes_signature(dcp) == sig)
		return;
	dev_info(dcp->dev, "the display lists other modes than before the sleep (%u), reading it again\n",
		 dcp->nr_modes);
	/* Past the work's own wait by a second; nothing holds on to it longer. */
	WRITE_ONCE(dcp->wake_reread,
		   (jiffies + msecs_to_jiffies(DCP_WAKE_REREAD_DELAY_MS +
					       DCP_WAKE_REREAD_MS + 1000)) ?: 1);
	mod_delayed_work(system_freezable_wq, &dcp->link_recovery_wq,
			 msecs_to_jiffies(DCP_WAKE_REREAD_DELAY_MS));
}

static void dcp_link_recovery_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work),
					     struct apple_dcp, link_recovery_wq);
	struct platform_device *pdev = to_platform_device(dcp->dev);
	bool wake = dcp_wake_reread_pending(dcp);

	if (!READ_ONCE(dcp->typec_cable_connected) ||
	    (!wake && atomic_read(&dcp->modesets_done) != dcp->link_recovery_snap)) {
		WRITE_ONCE(dcp->wake_reread, 0);
		return;
	}
	if (!wake)
		dev_info(dcp->dev, "link recovery %u: connecting again after a retrain that found no mode\n",
			 ++dcp->link_recovery_tries);
	dcp_dptx_disconnect_oob(pdev, 0);
	/* What the display asked for with the list it had: the next one says. */
	WRITE_ONCE(dcp->peer_starved, false);
	msleep(DCP_WAKE_REREAD_MS);
	WRITE_ONCE(dcp->wake_reread, 0);
	dcp_dptx_connect_oob(pdev, 0);
}

/* The end of a stopped retrain window (dcp_hotplug_retrain_allowed()). */
static void dcp_hotplug_retrain_retry_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work), struct apple_dcp,
					     hotplug_retrain_retry_wq);
	struct apple_connector *connector = READ_ONCE(dcp->connector);

	if (connector && READ_ONCE(connector->connected))
		schedule_work(&connector->hotplug_wq);
}

static void dcp_typec_reconnect_work(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     typec_reconnect_wq);
	struct dcp_fabric_session session;
	int ret;

	/* A route following its CRTC hands the link over itself. */
	if ((dcp->external && dcpext_scanout_terminal(dcp)) ||
	    !READ_ONCE(dcp->typec_cable_connected) ||
	    READ_ONCE(dcp->typec_follow_retiring))
		return;

	if (dcp_typec_sessionless(dcp)) {
		ret = dcp_dptx_connect(dcp, 0);
	} else {
		/* Only the session this work was queued for may connect. */
		scoped_guard(mutex, &dcp->hpd_mutex) {
			session = dcp->typec_reconnect_session;
			if (!dcp_session_valid_locked(dcp, session, true))
				return;
		}
		ret = dcp_dptx_connect_raw(dcp, 0, NULL, &session, true, false);
		if (!ret)
			ret = dcp_dptx_connect_tile(dcp);
		if (dcp_is_t8132_ext(dcp) && ret && ret != -ESTALE)
			dev_warn(dcp->dev, "DPTX connect for the DP tunnel failed: %d\n", ret);
		scoped_guard(mutex, &dcp->hpd_mutex) {
			if (!dcp_session_valid_locked(dcp, session, true))
				return;
		}
	}
	if (!ret) {
		dcp->typec_reconnect_tries = 0;
		return;
	}
	/* A display held for this link will not get ready. */
	if (READ_ONCE(dcp->external_error))
		dcp_announce_kick(dcp);

	/* A T8132 dcpext connects once per tunnel. */
	if (++dcp->typec_reconnect_tries <
	    (dcp_uses_t6020_tunnel_flow(dcp) || dcp_is_t8132_ext(dcp) ?
	     1 : DPTX_RECONNECT_RETRIES)) {
		mod_delayed_work(system_freezable_wq, &dcp->typec_reconnect_wq,
				 DPTX_RECONNECT_DELAY);
		return;
	}

	dev_err(dcp->dev, "Type-C DPTX reconnect failed after %u retries: %d\n",
		dcp->typec_reconnect_tries, ret);
}

/**
 * dcp_hold_resume_event - hold the resume hotplug event of an external display
 * @pdev: the DCP of a CRTC resumed disabled
 *
 * System sleep releases the link of an external H17P display, and with it
 * the proof of modes such as 1080p at 180 Hz, so apple_drm_drop_stale_crtcs()
 * resumes the CRTC of such a mode disabled with its link marked bad.  A
 * hotplug event then has userspace set the mode again.  Sent at once, it has
 * userspace probe before the link is back, pick a fallback mode, and set its
 * own mode again once that is offered: two mode sets the monitor has to
 * resynchronize to.
 *
 * Hold that event, not the connection, until the display's modes are in for
 * the new cable generation, or for DCP_ANNOUNCE_RESUME_TIMEOUT_MS after the
 * link work can run again, then send it once from the hotplug worker.  No
 * lock is held while waiting; a probe in the meantime reports the display
 * connected with the modes it has.
 *
 * Returns false if the caller must send the event itself.
 */
bool dcp_hold_resume_event(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct apple_connector *con = READ_ONCE(dcp->connector);

	if (!dcp_is_external_h17p(dcp) || !con || READ_ONCE(con->dcp) != pdev)
		return false;

	guard(mutex)(&dcp->hpd_mutex);
	if (!READ_ONCE(con->connected) || !dcp->typec_cable_connected ||
	    !dcp_announce_hold(&dcp->announce, dcp->typec_generation))
		return false;
	/* Freezable, like the link work: the timeout starts after thaw. */
	dcp_announce_arm(dcp);
	return true;
}

/* A display is attached to the Type-C output: a new attachment. */
static void dcp_typec_attach_locked(struct apple_dcp *dcp)
{
	lockdep_assert_held(&dcp->hpd_mutex);
	dcp->typec_generation++;
	WRITE_ONCE(dcp->typec_cable_connected, true);
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, true);
	dcp->typec_reconnect_tries = 0;
	dcp->placeholder_retried = false;
	cancel_delayed_work(&dcp->typec_reconnect_wq);
	/* A newly attached display gets its own retries. */
	if (dcp->external_native) {
		atomic_set(&dcp->external_retries, 0);
		cancel_delayed_work(&dcp->external_retry_wq);
	}
}

/* Connect the link of an attached display, retrying later if that fails. */
static int dcp_dptx_connect_attached(struct apple_dcp *dcp, u32 port)
{
	int ret = dcp_dptx_connect(dcp, port);

	if (ret && ret != -ESHUTDOWN && dcp_is_typec_output(dcp))
		dcp_queue_typec_reconnect(dcp, DPTX_RECONNECT_DELAY);

	return ret;
}

/*
 * A USB-C monitor with a built-in hub can lose the DP link when the hub
 * brings its SuperSpeed link up right around DCP's link training for the
 * first modeset. DCP may then report the sink as unplugged about two
 * seconds after HPD was asserted for it, although the sink still asserts
 * HPD, and nothing else connects it again: replay an unplug and replug of
 * the sink ("relink"), which trains the link once the hub has settled, as
 * a cable replug does. A sink's IRQ_HPD gets a retrain, as before.
 *
 * The retries cover one attachment or one DPMS wake: the replay itself
 * does not reset them, dcp_poweron() does. dcp-relink.h holds the
 * decisions; relink_lock protects their state.
 */
/*
 * A T6050 tunnel keeps its Thunderbolt binding and its HPD when the firmware
 * drops the sink after servicing its IRQ_HPD (see dcp_tb_event_work()):
 * nothing else connects it again. Other tunnels are left to their owners.
 */
static bool dcp_tunnel_relinks(struct apple_dcp *dcp)
{
	return of_device_is_compatible(dcp->dev->of_node, "apple,t6050-dcpext") &&
	       READ_ONCE(dcp->tb_event_hpd);
}

static bool dcp_typec_relink_eligible(struct apple_dcp *dcp)
{
	struct apple_connector *connector = READ_ONCE(dcp->connector);

	/* H17P and native external pipes recover through their own lifecycle */
	if (dcp->fw_compat == DCP_FIRMWARE_H17P || dcp->external_native ||
	    !dcp_is_typec_output(dcp) ||
	    (dcp_is_usb4_output(dcp) && !dcp_tunnel_relinks(dcp)) ||
	    dcp_is_external_h17p(dcp) || !dcp->typec_connector)
		return false;
	/*
	 * Only a sink that is attached, shown and still connected for DRM:
	 * not one the driver itself let go, holds HPD low for (see
	 * dcp_placeholder_edid_work()) or moves to another pipeline.
	 */
	/* Most tunnels are excluded above; direct follows use the carried gate. */
	return READ_ONCE(dcp->typec_work_enabled) &&
	       READ_ONCE(dcp->typec_cable_connected) &&
	       !READ_ONCE(dcp->typec_crtc_off) &&
	       !READ_ONCE(dcp->typec_hpd_held) &&
	       !READ_ONCE(dcp->typec_follow_retiring) &&
	       connector && READ_ONCE(connector->connected);
}

/* DCP reported an unplug of a Type-C sink that still asserts HPD */
bool dcp_typec_relink(struct apple_dcp *dcp)
{
	unsigned long flags;
	bool start;
	u32 tries;

	if (!dcp_typec_relink_eligible(dcp))
		return false;

	spin_lock_irqsave(&dcp->relink_lock, flags);
	start = dcp_relink_unplug(&dcp->relink, READ_ONCE(dcp->typec_generation),
				  jiffies);
	if (start)
		mod_delayed_work(system_freezable_wq, &dcp->typec_relink_wq, 0);
	tries = dcp->relink.tries;
	spin_unlock_irqrestore(&dcp->relink_lock, flags);

	if (start)
		dev_info(dcp->dev,
			 "DCP dropped a sink that asserts HPD, relinking the Type-C sink (%u/%u)\n",
			 tries, DCP_RELINK_RETRIES);
	return start;
}

/*
 * The sink asked for attention: IRQ_HPD or a coalesced HPD pulse. The
 * caller retrains the link, as for any such request, unless a relink is
 * queued or runs: that connects the sink anew, and until it does, the
 * sink has no link to retrain.
 */
bool dcp_typec_sink_relink(struct apple_dcp *dcp)
{
	unsigned long flags;
	bool active;

	if (dcp->fw_compat == DCP_FIRMWARE_H17P)
		return false;

	spin_lock_irqsave(&dcp->relink_lock, flags);
	active = dcp->relink.active;
	spin_unlock_irqrestore(&dcp->relink_lock, flags);

	return active;
}

static void dcp_typec_relink_work(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     typec_relink_wq);
	struct platform_device *pdev = to_platform_device(dcp->dev);
	unsigned long flags;
	u64 generation;
	bool run;

	scoped_guard(mutex, &dcp->hpd_mutex) {
		generation = dcp->typec_generation;
		spin_lock_irqsave(&dcp->relink_lock, flags);
		run = dcp->relink.active && dcp->relink.generation == generation &&
		      dcp->typec_cable_connected;
		spin_unlock_irqrestore(&dcp->relink_lock, flags);
	}
	if (!run)
		goto out;

	dcp_dptx_disconnect_oob(pdev, 0);
	msleep(DCP_RELINK_UNPLUG_MS);

	/*
	 * Attach the sink again as a replug does, unless a real unplug or
	 * replug came meanwhile, which then takes over. Checked and done
	 * under one hold of hpd_mutex, so that neither can come in between.
	 */
	cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (dcp->typec_generation != generation + 1)
			goto out;
		dcp_typec_attach_locked(dcp);
		spin_lock_irqsave(&dcp->relink_lock, flags);
		dcp_relink_reattach(&dcp->relink, dcp->typec_generation, jiffies);
		spin_unlock_irqrestore(&dcp->relink_lock, flags);
	}
	dcp_dptx_connect_attached(dcp, 0);
out:
	spin_lock_irqsave(&dcp->relink_lock, flags);
	dcp_relink_end(&dcp->relink);
	spin_unlock_irqrestore(&dcp->relink_lock, flags);
}

static void disconnected_hpd_event(struct apple_connector *con)
{
	struct platform_device *pdev;

	if (!con)
		return;

	pdev = READ_ONCE(con->dcp);
	if (pdev) {
		struct apple_dcp *dcp = platform_get_drvdata(pdev);

		WRITE_ONCE(dcp->ext_backlight, false);
		/* Nothing else reports an HDMI unplug to the firmware metadata. */
		if (dcp_sink_metadata(dcp))
			dcp_desktop_metadata_new_sink(dcp);
	}

	/* May run with DRM modeset locks held; defer property/cache cleanup. */
	WRITE_ONCE(con->connected, false);
	WRITE_ONCE(con->edid_invalidation_pending, true);
	schedule_work(&con->hotplug_wq);
	/* Drop the display's backlight, outside the caller's locks. */
	schedule_work(&con->bl_sync_wq);
}

/*
 * A T8132 dcpext's teardown call of DPTX port @port returned @ret. For a
 * timed-out call, also the endpoint's ring pointers (receive: firmware to
 * host, transmit: host to firmware).
 */
static void dcp_t8132_dptx_teardown_log(struct apple_dcp *dcp, u32 port,
					const char *what, int ret)
{
	struct apple_dcp_afkep *ep = dcp->dptxep;

	if (!ret)
		return;
	if (ret != -ETIMEDOUT || !ep || !ep->rxbfr.ready || !ep->txbfr.ready) {
		dev_warn(dcp->dev, "DPTX port %u %s: %d\n", port, what, ret);
		return;
	}
	dev_warn(dcp->dev, "DPTX port %u %s: %d; EP %#x rx rptr %#x wptr %#x, tx rptr %#x wptr %#x\n",
		 port, what, ret, ep->endpoint,
		 le32_to_cpu(READ_ONCE(*ep->rxbfr.rptr)), le32_to_cpu(READ_ONCE(*ep->rxbfr.wptr)),
		 le32_to_cpu(READ_ONCE(*ep->txbfr.rptr)), le32_to_cpu(READ_ONCE(*ep->txbfr.wptr)));
}

static int dcp_dptx_release_locked(struct apple_dcp *dcp, u32 port)
{
	int ret;

	lockdep_assert_held(&dcp->hpd_mutex);
	if (dcp->external) {
		smp_store_release(&dcp->external_link_ready, false);
		dcpext_scanout_invalidate(dcp);
	}
	if (dptx_service_available(&dcp->dptxport[port]) && dcp->dptxport[port].connected) {
		/*
		 * A T8132 dcpext's DRM swaps stop with the route, before
		 * the release, which can take the firmware a while to answer.
		 */
		if (dcp_is_t8132_ext(dcp))
			iomfb_m4_external_link_gone(dcp);
		ret = dptxport_release_display(dcp->dptxport[port].service);
		/*
		 * A T8132 dcpext's display is released even if the
		 * firmware refuses or has not answered in time: the route of a
		 * tunnel that is gone must not stay owned, or the next tunnel's
		 * setup fails on the same release. A timed-out call stays as an
		 * orphan until the firmware answers it; until then the
		 * firmware's Activate/Deactivate are not acted on, and the next
		 * connect waits for it (afk_compact_orphans_wait()).
		 */
		if (dcp_is_t8132_ext(dcp)) {
			dcp_t8132_dptx_teardown_log(dcp, port, "release_display", ret);
			if (ret == -ETIMEDOUT)
				WRITE_ONCE(dcp->t8132_release_late, true);
			if (ret)
				dev_info(dcp->dev, "DPTX port %u taken as released (%s: %d)\n",
					 port, ret == -ETIMEDOUT ?
					 "the firmware answers late" : "release failed", ret);
			ret = 0;
			/* Its next sink starts from scratch. */
			iomfb_m4_external_link_gone(dcp);
		}
		if (ret)
			return ret;
		dcp->dptxport[port].connected = false;
	}
	/*
	 * T6030: the firmware takes a direct crossbar output down when it
	 * stops the link. Make sure of it for a display released here, also
	 * when the firmware did not get that far.
	 */
	if (dcp->external_native)
		dcp_direct_crossbar_link(dcp, false);
	return 0;
}

int dcp_dptx_disconnect(struct apple_dcp *dcp, u32 port)
{
	int ret;

	if (dcp_is_external_h17p(dcp))
		return dcp_external_disconnect(dcp, false);
	/* Release the caller's RemotePort service, not the downstream DFP port. */
	dev_info(dcp->dev, "%s(port=%d)\n", __func__, port);

	mutex_lock(&dcp->hpd_mutex);
	ret = dcp_dptx_release_locked(dcp, port);
	if (!ret)
		WRITE_ONCE(dcp->dptxport[port].tile_hint, false);
	mutex_unlock(&dcp->hpd_mutex);
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP && !port)
		dcp_peer_release_26_6(dcp);

	return ret;
}

int dcp_dptx_connect_oob(struct platform_device *pdev, u32 port)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP && !dcp_is_typec_output(dcp))
		return dcp_desktop_dptx_connect(dcp, port);

	if (dcp_is_external_h17p(dcp)) {
		bool new_cable;

		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
		cancel_work_sync(&dcp->external_stop_work);
		cancel_delayed_work_sync(&dcp->external_stop_retry_wq);
		guard(mutex)(&dcp->external_lock);
		new_cable = !dcp->external_cable;
		if (new_cable)
			dcp->external_generation++;
		dcp->external_cable = true;
		WRITE_ONCE(dcp->external_physically_absent, false);
		dcp_external_publish_generation(dcp);
		scoped_guard(mutex, &dcp->hpd_mutex) {
			WRITE_ONCE(dcp->typec_cable_connected, true);
			if (new_cable) {
				dcp->typec_generation++;
				dcp_firmware_metadata_connection(dcp, dcp->typec_generation, true);
				dcp_announce_end(&dcp->announce);
			}
			dcp->placeholder_retried = false;
		}
		return dcp_external_link_locked(dcp);
	}

	if (dcp_is_typec_output(dcp)) {
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
		guard(mutex)(&dcp->hpd_mutex);
		dcp_typec_attach_locked(dcp);
		scoped_guard(spinlock_irqsave, &dcp->relink_lock)
			dcp_relink_attach(&dcp->relink, dcp->typec_generation);
	}

	return dcp_dptx_connect_attached(dcp, port);
}

int dcp_dptx_disconnect_oob(struct platform_device *pdev, u32 port)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP && !dcp_is_typec_output(dcp))
		return dcp_desktop_dptx_disconnect_oob(dcp, port);

	if (dcp_is_external_h17p(dcp))
		return dcp_external_disconnect(dcp, true);

	if (dcp_is_typec_output(dcp)) {
		scoped_guard(mutex, &dcp->hpd_mutex) {
			WRITE_ONCE(dcp->typec_cable_connected, false);
			dcp->typec_generation++;
			dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
			dcp->typec_unplugs++;
			/* Nothing left to retry for the display that went. */
			if (dcp->external_native)
				cancel_delayed_work(&dcp->external_retry_wq);
		}
		WRITE_ONCE(dcp->typec_crtc_off, false);
		reinit_completion(&dcp->typec_iomfb_hpd_ready);
		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
	}

	disconnected_hpd_event(dcp->connector);

	if (dcp->avep)
		av_service_disconnect(dcp);

	/*
	 * A T8132 dcpext lowers only the HPD its connect raised, and, as
	 * desktop 26.x compact DPTX does, still releases the display when the
	 * firmware refuses: a refusal must not keep a released tunnel's route.
	 * An answer that comes after the call timed out is the call's orphan's
	 * (see dcp_dptx_release_locked()).
	 */
	if (dcp_is_t8132_ext(dcp)) {
		if (dptx_service_available(&dcp->dptxport[port]) &&
		    READ_ONCE(dcp->dptxport[port].connected)) {
			ret = dptxport_set_hpd(dcp->dptxport[port].service, false);
			dcp_t8132_dptx_teardown_log(dcp, port, "set_hpd(0)", ret);
			if (ret == -ETIMEDOUT)
				WRITE_ONCE(dcp->t8132_release_late, true);
		}
	} else if (dptx_service_available(&dcp->dptxport[port])) {
		ret = dptxport_set_hpd(dcp->dptxport[port].service, false);
		if (ret)
			return ret;
	}

	ret = dcp_dptx_disconnect(dcp, port);
	/* The firmware shuts a desktop 26.x external pipe down with its display. */
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP && !dcp->main_display)
		dcp->desktop->ext_pipe_off = true;
	return ret;
}

/*
 * A delivered sink IRQ can follow a firmware unplug with cable HPD still high.
 * Recovery is refused unless the Type-C work is enabled, the cable is routed
 * and no follow handoff is retiring the attachment; the session it revokes
 * older waits with is rechecked at every step.
 */
int dcp_dptx_recover_irq(struct apple_dcp *dcp)
{
	struct dcp_fabric_session session;
	const struct dcp_fabric_session *checked =
		dcp_typec_sessionless(dcp) ? NULL : &session;
	int ret;

	mutex_lock(&dcp->hpd_mutex);
	if (dcp->external_native || dcp_is_external_h17p(dcp) ||
	    !dcp_is_typec_output(dcp) || dcp_is_usb4_output(dcp) ||
	    !dcp->typec_connector ||
	    READ_ONCE(dcp->typec_connector->connected) ||
	    READ_ONCE(dcp->typec_crtc_off)) {
		ret = 0;
		goto out_unlock;
	}
	if (!dcp->typec_work_enabled || READ_ONCE(dcp->crashed) ||
	    !READ_ONCE(dcp->typec_cable_connected) ||
	    READ_ONCE(dcp->typec_follow_retiring) ||
	    !dcp->dptxport[0].enabled ||
	    !dptx_service_available(&dcp->dptxport[0])) {
		ret = -EAGAIN;
		goto out_unlock;
	}
	if (checked)
		session = dcp_session_locked(dcp);
	if (!dcp_connect_session_valid_locked(dcp, checked, true, true)) {
		ret = -ESTALE;
		goto out_unlock;
	}

	/* Revoke older reconnect waits without changing physical cable state. */
	dcp->typec_generation++;
	if (checked)
		session = dcp_session_locked(dcp);
	cancel_delayed_work(&dcp->typec_reconnect_wq);
	cancel_delayed_work(&dcp->placeholder_edid_wq);
	if (dcp->avep)
		av_service_disconnect(dcp);
	ret = dptxport_set_hpd(dcp->dptxport[0].service, false);
	if (ret)
		goto out_unlock;
	if (!dcp_connect_session_valid_locked(dcp, checked, true, true)) {
		ret = -ESTALE;
		goto out_unlock;
	}
	/* A failed earlier connect may also have left an unconfirmed release. */
	ret = dptxport_release_display(dcp->dptxport[0].service);
	if (ret)
		goto out_unlock;
	if (!dcp_connect_session_valid_locked(dcp, checked, true, true)) {
		ret = -ESTALE;
		goto out_unlock;
	}
	dcp->dptxport[0].connected = false;
	mutex_unlock(&dcp->hpd_mutex);

	/* Another delivered IRQ must confirm release before a failed attempt retries. */
	return dcp_dptx_connect_raw(dcp, 0, NULL, checked, true, true);

out_unlock:
	mutex_unlock(&dcp->hpd_mutex);
	return ret;
}

/*
 * Release the display link of a Type-C output while its display stays
 * attached, as a CRTC power-off does (see dcp_poweroff()): the firmware's
 * unplug for it is ignored, and dcp_poweron() connects the link again.
 */
int dcp_dptx_park(struct apple_dcp *dcp)
{
	int ret;

	scoped_guard(mutex, &dcp->hpd_mutex) {
		WRITE_ONCE(dcp->typec_crtc_off, true);
		dcp->typec_generation++;
	}
	/* Generation checks around reconnect waits reject the revoked one. */
	cancel_delayed_work(&dcp->typec_reconnect_wq);
	cancel_delayed_work(&dcp->placeholder_edid_wq);
	if (dcp->avep)
		av_service_disconnect(dcp);
	if (READ_ONCE(dcp->split.active) &&
	    dptx_service_available(&dcp->dptxport[1]) &&
	    dcp->dptxport[1].connected) {
		ret = dptxport_set_hpd(dcp->dptxport[1].service, false);
		if (ret)
			return ret;
		ret = dcp_dptx_disconnect(dcp, 1);
		if (ret)
			return ret;
	}
	if (dptx_service_available(&dcp->dptxport[0]) && dcp->dptxport[0].connected) {
		ret = dptxport_set_hpd(dcp->dptxport[0].service, false);
		if (ret) {
			dev_warn(dcp->dev, "failed to deassert Type-C DPTX HPD: %d\n", ret);
			return ret;
		}
		return dcp_dptx_disconnect(dcp, 0);
	}
	return 0;
}

bool dcp_fw_compat_is_12_x(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fw_compat == DCP_FIRMWARE_V_12_3;
}

/* A failed T8122 dcpext: no pipeline of the DRM device. */
bool dcp_ext_unusable(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return !dcp || READ_ONCE(dcp->ext_dead);
}

bool dcp_fw_compat_is_desktop(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP;
}

/* Firmware whose panel shows PQ content as EDR (dcp_panel_edr_supported()). */
bool dcp_fw_compat_panel_edr(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP ||
	       dcp->fw_compat == DCP_FIRMWARE_V_27_0;
}

bool dcp_fw_compat_is_14_7(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fw_compat == DCP_FIRMWARE_V_14_7 ||
	       dcp->fw_compat == DCP_FIRMWARE_V_26_6;
}

/*
 * GAMMA_LUT entries, 0 where the driver cannot program the gamma table.
 *
 * Desktop 26.x composes the LUT through the firmware's own table (see
 * dcp_gamma_set()), but no desktop 26.x DCP hands out a usable one: the
 * J616s panel's A420 table does not rise monotonically and the external
 * processors return none (status 2).  Offering GAMMA_LUT there meant
 * accepting a LUT and dropping it, so compositors that move work into it
 * got wrong output: KWin's HDR encoding curve went missing (PQ content
 * washed out) and night light did nothing.  Without the property KWin
 * applies its curves while rendering and mutter uses CTM for night light.
 */
unsigned int dcp_gamma_lut_size(struct platform_device *pdev)
{
	return 0;
}

unsigned long* dcp_get_iomfb_surfaces(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->iomfb_surfaces;
}

/* KMS registration owns the aperture before the first T8132 scanout swap. */
int dcp_activate(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return iomfb_is_m4(dcp) ? iomfb_m4_activate(dcp) : 0;
}

/*
 * Desktop 26.x (T6041) start.  The firmware does not tolerate being left
 * running with its endpoints unserviced, so RTKit is woken only here, once
 * every handler is in place, not at component bind.  The internal panel's
 * processor then starts the system endpoint, the IOMFB transport and the
 * auxiliary endpoints the firmware advertises, all speaking compact EPIC,
 * and binds the IOMFB shared memory last.  An auxiliary endpoint that fails
 * to start is skipped: IOMFB can run without it.
 */
static void dcp_compact_aux_init(struct apple_epic_service *service,
				 const char *name, const char *class, s64 unit)
{
	dev_dbg(service->ep->dcp->dev,
		"AFK[ep:%02x]: compact auxiliary service %s (unit %lld)\n",
		service->ep->endpoint, name ?: "", unit);
}

/*
 * These endpoints are only serviced: no call of theirs was ever seen.  A
 * call is answered with -EOPNOTSUPP and logged by the AFK core.
 */
static const struct apple_epic_service_ops dcp_compact_aux_ops[] = {
	{
		.name = "*",
		.init = dcp_compact_aux_init,
	},
	{},
};

/* The DCPAV service of an external processor: HDR10 static metadata. */
static void dcp_av_service_init(struct apple_epic_service *service,
				const char *name, const char *class, s64 unit)
{
	struct dcp_desktop_state *d = service->ep->dcp->desktop;
	struct apple_epic_service *old;

	service = afk_service_get(service);
	if (!service)
		return;
	scoped_guard(spinlock_irqsave, &d->hdr_lock) {
		old = d->av_service;
		d->av_service = service;
		d->hdr_sent_valid = false;
		d->hdr_infoframe = false;
	}
	if (old)
		afk_service_put(old);
}

static void dcp_av_service_teardown(struct apple_epic_service *service)
{
	struct dcp_desktop_state *d = service->ep->dcp->desktop;
	bool owned;

	scoped_guard(spinlock_irqsave, &d->hdr_lock) {
		owned = d->av_service == service;
		if (owned) {
			d->av_service = NULL;
			d->hdr_sent_valid = false;
			d->hdr_infoframe = false;
		}
	}
	if (owned)
		afk_service_put(service);
}

static const struct apple_epic_service_ops dcp_ext_av_service_ops[] = {
	{
		.name = "dcpav-service-epic",
		.reusable = true,
		.init = dcp_av_service_init,
		.teardown = dcp_av_service_teardown,
	},
	{
		.name = "*",
		.init = dcp_compact_aux_init,
	},
	{},
};

static void dcp_dpavctrl_service_init(struct apple_epic_service *service,
				      const char *name, const char *class,
				      s64 unit)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct dcp_desktop_state *d = dcp->desktop;
	const char *service_class = class ?: name;

	if (service_class && strstr(service_class, "dcpav-controller-epic"))
		d->dpavctrl_av_controller[!!(service->channel & BIT(2))] = service;

	/* Interface 7 is opened only once the AV controllers are. */
	if ((service->channel & 7) == 7) {
		service->defer_start = true;
		d->dpavctrl_late_service = service;
	}

	/*
	 * The internal panel announces only the AV controller on interface 5
	 * and the deferred DP controller on interface 7.
	 */
	if (dcp_has_panel(dcp)) {
		if (d->dpavctrl_av_controller[1] && d->dpavctrl_late_service)
			complete_all(&d->dpavctrl_ready);
	} else if (service->ep->num_channels >= 4) {
		complete_all(&d->dpavctrl_ready);
	}
}

static const struct apple_epic_service_ops dcp_dpavctrl_ops[] = {
	{
		.name = "AppleDCPDPTXController",
		.init = dcp_dpavctrl_service_init,
	},
	{
		.name = "dcpav-controller-epic",
		.init = dcp_dpavctrl_service_init,
	},
	{
		.name = "dcpdp-controller-epic",
		.init = dcp_dpavctrl_service_init,
	},
	{},
};

static int dcp_desktop_start_ep(struct apple_dcp *dcp, u32 endpoint,
				const struct apple_epic_service_ops *ops)
{
	struct apple_dcp_afkep *ep;
	int ret;

	ep = afk_init(dcp, endpoint, ops);
	if (IS_ERR(ep))
		return PTR_ERR(ep);

	/*
	 * Route its messages before the start: afk_start() waits for the
	 * firmware's reply on this endpoint.  The route stays if the start
	 * times out, the firmware may still talk.
	 */
	WRITE_ONCE(dcp->desktop->graph[endpoint - 0x20], ep);
	ret = afk_start(ep);
	return ret;
}

static int dcp_desktop_start_dpavctrl(struct apple_dcp *dcp)
{
	int ret;

	init_completion(&dcp->desktop->dpavctrl_ready);
	ret = dcp_desktop_start_ep(dcp, DPAV_CTRL_ENDPOINT, dcp_dpavctrl_ops);
	if (ret)
		return ret;

	if (!wait_for_completion_timeout(&dcp->desktop->dpavctrl_ready,
					 msecs_to_jiffies(1000)))
		return -ETIMEDOUT;
	return 0;
}

/* The auxiliary endpoints of the panel's processor, started if advertised. */
static const struct {
	u32 endpoint;
	const char *name;
} dcp_desktop_panel_eps[] = {
	{ DPAV_CTRL_ENDPOINT, "DPAVCTRL" },
	{ EPIC25_ENDPOINT, "EPIC25" },
	{ DPSAC_ENDPOINT, "DPSAC" },
	{ DPDEV_ENDPOINT, "DPDEV" },
	{ DPAVSERV_ENDPOINT, "DPAVSERV" },
	{ AV_ENDPOINT, "AV" },
	{ DPTX_ENDPOINT, "DPTX" },
	{ AV_VIDEO_ENDPOINT, "AVVIDEO" },
	{ EPIC2C_ENDPOINT, "EPIC2C" },
	{ DPTX_HDCP_ENDPOINT, "DPTXHDCP" },
};

static void dcp_desktop_graph_skip(struct apple_dcp *dcp, u32 endpoint,
				   const char *name, const char *reason, int rc)
{
	dcp->desktop->graph_skipped |= BIT(endpoint - 0x20);
	if (rc)
		dev_warn(dcp->dev, "endpoint %#04x (%s) not started: %s (%d)\n",
			 endpoint, name, reason, rc);
	else
		dev_dbg(dcp->dev, "endpoint %#04x (%s) not started: %s\n",
			endpoint, name, reason);
}

/*
 * The external processors (T6041 dcpext): DPDEV, DPAVCTRL (all four
 * controller interfaces), DPSAC, AV and DPTXHDCP as advertised, then
 * with a DP PHY or a Type-C route iBoot, the DPTX port services on EPIC2C,
 * DPAVSERV for the EDID, EXPERT and EPIC25, and the DPTX connect of port 0.
 * A failure fails this processor's start; the panel is not affected.
 */
static int dcp_desktop_start_ext_ep(struct apple_dcp *dcp, u32 map, u32 endpoint,
				    const char *name)
{
	int ret;

	if (!(map & BIT(endpoint - 0x20))) {
		dcp_desktop_graph_skip(dcp, endpoint, name, "not advertised", 0);
		return 0;
	}
	ret = dcp_desktop_start_ep(dcp, endpoint, dcp_compact_aux_ops);
	if (ret)
		return dev_err_probe(dcp->dev, ret, "Failed to start %s endpoint\n",
				     name);
	dcp->desktop->graph_started |= BIT(endpoint - 0x20);
	return 0;
}

/*
 * The link step of an external processor's start: a Type-C route powers the
 * PHY and selects the crossbar when it is claimed; until then there is no
 * link to bring up.  The fixed HDMI output is connected when its display is
 * there; one that comes later is connected at its HPD.
 */
static void dcp_start_desktop_ext_link(struct apple_dcp *dcp)
{
	int ret;

	if (!dptx_service_available(&dcp->dptxport[0]) ||
	    !(dcp->fixed_phy || dcp->active_typec_route))
		return;
	if (dcp->hdmi_hpd && !READ_ONCE(dcp->active_typec_route) &&
	    gpiod_get_value_cansleep(dcp->hdmi_hpd) <= 0)
		return;
	ret = dcp_dptx_connect(dcp, 0);
	if (ret)
		dev_info(dcp->dev, "no DPTX link at startup: %d\n", ret);
}

/*
 * Resume from disk: the restart work connects the link itself, once this
 * processor's DRM entry points are open again and the other external
 * processors run (see dcp_hib_ext_restore()).
 */
static bool dcp_hib_link_deferred(struct apple_dcp *dcp)
{
	return IS_ENABLED(CONFIG_HIBERNATION) &&
	       READ_ONCE(dcp->hib_phase) == DCP_HIB_RESTARTING;
}

static int dcp_start_desktop_external(struct apple_dcp *dcp, u32 map)
{
	struct dcp_desktop_state *d = dcp->desktop;
	int ret, dptx_ret;

	ret = dcp_desktop_start_ext_ep(dcp, map, DPDEV_ENDPOINT, "DPDEV");
	if (ret)
		return ret;
	ret = dcp_desktop_start_dpavctrl(dcp);
	if (ret)
		return dev_err_probe(dcp->dev, ret,
				     "Failed to initialize DPAVCTRL endpoint\n");
	d->graph_started |= BIT(DPAV_CTRL_ENDPOINT - 0x20);
	ret = dcp_desktop_start_ext_ep(dcp, map, DPSAC_ENDPOINT, "DPSAC");
	if (ret)
		return ret;
	ret = dcp_desktop_start_ext_ep(dcp, map, AV_ENDPOINT, "AV");
	if (ret)
		return ret;
#if IS_ENABLED(CONFIG_DRM_APPLE_AUDIO)
	if (hdmi_audio && (map & BIT(AV_AUDIO_ENDPOINT_DESKTOP - 0x20))) {
		ret = avep_init(dcp);
		if (ret)
			dcp_desktop_graph_skip(dcp, AV_AUDIO_ENDPOINT_DESKTOP,
					       "AUDIO", "audio init failed", ret);
		else if (dcp->avep)
			d->graph_started |= BIT(AV_AUDIO_ENDPOINT_DESKTOP - 0x20);
	}
#endif
	ret = dcp_desktop_start_ext_ep(dcp, map, DPTX_HDCP_ENDPOINT,
				       "DPTXHDCP");
	if (ret)
		return ret;

	/*
	 * DCPAV service (dcpav-service-epic, HDR10 static metadata).  Without
	 * it a PQ link still works, only without mastering data.
	 */
	if (map & BIT(DPAVSERV_ENDPOINT_V2 - 0x20)) {
		ret = dcp_desktop_start_ep(dcp, DPAVSERV_ENDPOINT_V2,
					   dcp_ext_av_service_ops);
		if (ret)
			dcp_desktop_graph_skip(dcp, DPAVSERV_ENDPOINT_V2,
					       "DPAVSERV", "start failed", ret);
		else
			d->graph_started |= BIT(DPAVSERV_ENDPOINT_V2 - 0x20);
	}

	/* An idle Type-C pipeline has no PHY until a route is claimed. */
	if (!dcp->phy && !dcp->nr_typec_routes) {
		dev_warn(dcp->dev, "no DP PHY and no Type-C route: no display link\n");
		return 0;
	}

	ret = ibootep_init(dcp);
	if (ret)
		dev_warn(dcp->dev, "Failed to start IBOOT endpoint: %d\n", ret);

	/* The DPTX port services live on EPIC2C here. */
	dptx_ret = dptxep_init_desktop(dcp, EPIC2C_ENDPOINT);
	if (dptx_ret)
		dev_warn(dcp->dev, "Failed to start DPTX port services: %d\n",
			 dptx_ret);

	if (unstable_edid) {
		ret = dpavservep_init(dcp);
		if (ret)
			return dev_err_probe(dcp->dev, ret,
					     "Failed to start DPAVSERV endpoint\n");
	}
	ret = dcp_desktop_start_ext_ep(dcp, map, DCP_EXPERT_ENDPOINT, "EXPERT");
	if (ret)
		return ret;
	ret = dcp_desktop_start_ext_ep(dcp, map, EPIC25_ENDPOINT, "EPIC25");
	if (ret)
		return ret;

	if (!dptx_ret && !dcp_hib_link_deferred(dcp))
		dcp_start_desktop_ext_link(dcp);
	return 0;
}

static int dcp_start_desktop(struct apple_dcp *dcp)
{
	struct dcp_desktop_state *d = dcp->desktop;
	u32 map = 0;
	int i, ep, ret;

	if (IS_ERR_OR_NULL(dcp->rtk))
		return -ENODEV;

	ret = apple_rtkit_wake(dcp->rtk);
	if (ret)
		return dev_err_probe(dcp->dev, ret, "Failed to boot RTKit: %d\n",
				     ret);

	for (ep = 0x20; ep < 0x40; ep++)
		if (apple_rtkit_has_endpoint(dcp->rtk, ep))
			map |= BIT(ep - 0x20);
	dev_dbg(dcp->dev, "firmware endpoints 0x20+: %#010x\n", map);

	ret = systemep_init(dcp);
	if (ret)
		dev_warn(dcp->dev, "Failed to start system endpoint: %d\n", ret);

	/*
	 * The IOMFB transport starts before the auxiliary endpoints; its
	 * shared memory is bound only after them (iomfb_start_rtkit()).
	 */
	ret = apple_rtkit_start_ep(dcp->rtk, IOMFB_ENDPOINT);
	if (ret)
		return dev_err_probe(dcp->dev, ret,
				     "Failed to start IOMFB transport endpoint\n");

	if (dcp_desktop_external(dcp)) {
		ret = dcp_start_desktop_external(dcp, map);
		if (ret)
			return ret;
		goto start_iomfb;
	}

	for (i = 0; i < ARRAY_SIZE(dcp_desktop_panel_eps); i++) {
		u32 endpoint = dcp_desktop_panel_eps[i].endpoint;
		const char *name = dcp_desktop_panel_eps[i].name;

		if (!(map & BIT(endpoint - 0x20))) {
			dcp_desktop_graph_skip(dcp, endpoint, name, "not advertised", 0);
			continue;
		}

		if (endpoint == DPAV_CTRL_ENDPOINT)
			ret = dcp_desktop_start_dpavctrl(dcp);
		else
			ret = dcp_desktop_start_ep(dcp, endpoint,
						   dcp_compact_aux_ops);
		if (ret) {
			dcp_desktop_graph_skip(dcp, endpoint, name, "start failed", ret);
			continue;
		}
		d->graph_started |= BIT(endpoint - 0x20);
	}
start_iomfb:
	dev_info(dcp->dev, "endpoints started %#010x, skipped %#010x\n",
		 d->graph_started, d->graph_skipped);

	ret = iomfb_start_rtkit(dcp);
	if (ret)
		dev_err(dcp->dev, "Failed to start IOMFB endpoint: %d\n", ret);
	return ret;
}

int dcp_start(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	if (iomfb_is_m4(dcp)) {
		iomfb_m4_configure_backlight(dcp);
		return iomfb_m4_health(dcp);
	}


	/* Even if nothing starts, dcp_wait_ready() must find it initialised. */
	init_completion(&dcp->start_done);
	init_completion(&dcp->s2r_dart_done);
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6)
		return iomfb_v26_6_start(dcp);
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP)
		return dcp_start_desktop(dcp);
	init_completion(&dcp->typec_display_ready);

	/*
	 * The T6030 firmware session is adopted for the internal panel.
	 * Open DPTX on that same RTKit only when the firmware advertised it.
	 */
	if (dcp->fw_compat == DCP_FIRMWARE_V_14_7) {
		if (dcp->external) {
			/* A native pipe is usable now; its firmware starts on first use. */
			if (dcp->external_native)
				dcp->active = true;
			complete(&dcp->start_done);
			return 0;
		}
		ret = iomfb_v14_7_start(dcp);
		if (ret)
			return ret;
		return 0;
	}

	if (IS_ERR_OR_NULL(dcp->rtk))
		return -ENODEV;

	/* Make the H17P allocator available before external service discovery. */
	if (dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL) {
		ret = apple_rtkit_start_ep(dcp->rtk, REMOTE_ALLOC_ENDPOINT);
		if (ret)
			return dev_err_probe(dcp->dev, ret, "Failed to start remote allocator\n");
	}

	/* start RTKit endpoints */
	ret = systemep_init(dcp);
	if (ret)
		dev_warn(dcp->dev, "Failed to start system endpoint: %d\n", ret);

	if (unstable_edid && !dcp_has_panel(dcp)) {
		ret = dpavservep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start DPAVSERV endpoint: %d",
				 ret);
	}

	if ((dcp->phy || (dcp->fw_compat == DCP_FIRMWARE_V_27_0 &&
			  dcp->hw.num_dptx_ports)) &&
	    dcp->fw_compat >= DCP_FIRMWARE_V_13_5) {
		ret = ibootep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start IBOOT endpoint: %d\n",
				 ret);

		if (dcp->hw.iomfb_method_profile ==
		    DCP_IOMFB_METHODS_H17P_EXTERNAL) {
			ret = dptx_controller_init(dcp);
			if (ret)
				return dev_err_probe(dcp->dev, ret,
						     "Failed to start DPTX controller\n");
		}

		ret = dptxep_init(dcp);
		if (ret) {
			dev_warn(dcp->dev, "Failed to start DPTX endpoint: %d\n",
				 ret);
#ifdef DCP_DPTX_DISCONNECT_ON_INIT
		/*
		 * This disconnect / connect cycle on init is only necessary
		 * when using dcp0 on j473, j474s and presumedly j475c.
		 * Since dcp0 is not used at the moment let's avoid this
		 * since it is possibly the cause for startup issues.
		 */
		} else if (dptx_service_available(&dcp->dptxport[0])) {
			bool connected;
			/* force disconnect on start - necessary if the display
			 * is already up from m1n1
			 */
			dptxport_set_hpd(dcp->dptxport[0].service, false);
			dptxport_release_display(dcp->dptxport[0].service);
			usleep_range(10 * USEC_PER_MSEC, 25 * USEC_PER_MSEC);

			connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
			dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

			// necessary on j473/j474 but not on j314c
			if (connected)
				dcp_dptx_connect(dcp, 0);
#endif
		}
	} else if (dcp->phy) {
		dev_warn(dcp->dev, "OS firmware incompatible with dptxport EP\n");
	}
	ret = iomfb_start_rtkit(dcp);
	if (ret) {
		dev_err(dcp->dev, "Failed to start IOMFB endpoint: %d\n", ret);
		return ret;
	}

#if IS_ENABLED(CONFIG_DRM_APPLE_AUDIO)
	if (hdmi_audio) {
		ret = avep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start AV endpoint: %d", ret);
		ret = 0;
	}
#endif

	return ret;
}

/*
 * A native T6030 external processor: its DP/HDMI audio endpoint (EP 0x29)
 * unless hdmi_audio is off, as the other DCPs start it, for a
 * processor whose port@0 links an audio device; once per session, and after
 * the first display's EDID was read: with the endpoint's interfaces
 * published, an EDID copy (dcpavserv_copy_edid()) has the firmware rebuild
 * them and re-plug the sink, so the endpoint starts only once the first
 * EDID is cached.
 */
static void dcp_external_audio_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(work, struct apple_dcp, external_audio_work);

	/* Queued after an EDID came through the running session: RTKit is up. */
	if (!IS_ENABLED(CONFIG_DRM_APPLE_AUDIO) || !hdmi_audio || !dcp->external_native ||
	    dcp->external_audio_started || READ_ONCE(dcp->crashed) || !dcp->rtk)
		return;
	dcp->external_audio_started = true;
	if (!apple_rtkit_has_endpoint(dcp->rtk, AV_ENDPOINT)) {
		dev_info(dcp->dev, "no AV audio endpoint\n");
		return;
	}
#if IS_ENABLED(CONFIG_DRM_APPLE_AUDIO)
	{
		int ret = avep_init(dcp);

		if (ret)
			dev_warn(dcp->dev, "AV audio endpoint not started: %d\n", ret);
	}
#endif
}

void dcp_external_audio_start(struct apple_dcp *dcp)
{
	if (!dcp->external_native || dcp->external_audio_started)
		return;
	queue_work(system_freezable_wq, &dcp->external_audio_work);
}

/* Probe: the audio work, disabled until a native external pipe's bind. */
static void dcp_external_audio_init(struct apple_dcp *dcp)
{
	INIT_WORK(&dcp->external_audio_work, dcp_external_audio_work);
	/*
	 * A native external pipe's bind enables it (its first EDID queues
	 * it, through the connector); unbind and suspend disable it again.
	 */
	disable_work(&dcp->external_audio_work);
}

static void _dcp_poweroff(struct apple_dcp *dcp)
{
	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_poweroff_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_poweroff_v13_3(dcp);
		break;
	case DCP_FIRMWARE_V_14_7_TEMPLATE:
		iomfb_poweroff_v14_7_0(dcp);
		break;
	case DCP_FIRMWARE_V_26_6_DESKTOP:
		iomfb_poweroff_v26_6_desktop(dcp);
		if (dcp->edr_hm)
			dcp->edr_restart = true;
		/* The link set up again starts without HDR10 metadata. */
		dcp_hdr_metadata_forget(dcp);
		break;
	case DCP_FIRMWARE_V_26_6:
		iomfb_v26_6_poweroff(dcp);
		break;
	case DCP_FIRMWARE_V_14_7:
		iomfb_v14_7_poweroff(dcp);
		break;
	case DCP_FIRMWARE_H17P:
		iomfb_poweroff_h17p(dcp);
		break;
	case DCP_FIRMWARE_V_27_0:
		iomfb_poweroff_v27(dcp);
		if (dcp->edr_hm)
			dcp->edr_restart = true;
		WRITE_ONCE(dcp->edr_ramping, false);
		cancel_delayed_work(&dcp->iomfb.edr_tick);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

static void dcp_resume_enable_irq(void *ctx)
{
	struct apple_dcp *dcp = ctx;

	enable_irq(dcp->hdmi_hpd_irq);
}

static void dcp_resume_sample(void *ctx)
{
	dcp_fabric_hdmi_resume(ctx);
}

static const struct dcp_fabric_resume_ops dcp_resume_ops = {
	.enable_irq = dcp_resume_enable_irq,
	.sample = dcp_resume_sample,
};

/*
 * @replay: the link is set up again after a sleep. A display that is not
 * back yet (one in standby raises HPD only once it has woken up) is connected
 * by the HPD interrupt, and the pipe is left to the DRM resume's commit,
 * which runs at the same time: a power-off from here collides with its mode
 * set and leaves the firmware's calls unanswered.
 */
static int __dcp_enable_dp2hdmi_hpd(struct apple_dcp *dcp, bool replay)
{
	if (dcp_is_typec_output(dcp)) {
		if (READ_ONCE(dcp->typec_cable_connected))
			dcp_dptx_connect(dcp, 0);
	} else if (dcp->hdmi_hpd) {
		/* Check HPD before enabling the edge-triggered IRQ. */
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

		if (connected && !READ_ONCE(dcp->hdmi_yielded)) {
			dcp_dptx_connect(dcp, 0);
			if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP)
				dcp_desktop_hdmi_unpark(dcp);
		} else if (!connected && !replay) {
			_dcp_poweroff(dcp);
		}
	}

	if (dcp->hdmi_hpd_irq)
		dcp_fabric_run_resume(&dcp_resume_ops, dcp);

	return 0;
}

static int dcp_enable_dp2hdmi_hpd(struct apple_dcp *dcp)
{
	return __dcp_enable_dp2hdmi_hpd(dcp, false);
}

int dcp_wait_ready(struct platform_device *pdev, u64 timeout)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	if (iomfb_is_m4(dcp))
		return iomfb_m4_health(dcp);

	if (dcp->crashed)
		return -ENODEV;
	if (dcp->active)
		return dcp_enable_dp2hdmi_hpd(dcp);
	if (timeout <= 0)
		return -ETIMEDOUT;

	ret = wait_for_completion_timeout(&dcp->start_done, timeout);
	if (ret < 0)
		return ret;

	if (dcp->crashed)
		return -ENODEV;

	if (dcp->active)
		dcp_enable_dp2hdmi_hpd(dcp);

	return dcp->active ? 0 : -ETIMEDOUT;
}

static void __maybe_unused dcp_sleep(struct apple_dcp *dcp)
{
	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_sleep_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_sleep_v13_3(dcp);
		break;
	case DCP_FIRMWARE_V_14_7_TEMPLATE:
		iomfb_sleep_v14_7_0(dcp);
		break;
	case DCP_FIRMWARE_V_26_6_DESKTOP:
		iomfb_sleep_v26_6_desktop(dcp);
		break;
	case DCP_FIRMWARE_V_26_6:
		iomfb_v26_6_poweroff(dcp);
		break;
	case DCP_FIRMWARE_V_14_7:
		iomfb_v14_7_poweroff(dcp);
		break;
	case DCP_FIRMWARE_H17P:
		iomfb_sleep_h17p(dcp);
		break;
	case DCP_FIRMWARE_V_27_0:
		iomfb_sleep_v27(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

#define DCP_BL_FALLBACK_DELAY msecs_to_jiffies(2000)

/*
 * H17G (M5) internal panel: a pipe stop never comes back (see
 * iomfb_blank_h17p()), so DPMS blanks the panel and keeps the pipe running.
 */
static bool dcp_h17g_panel(struct apple_dcp *dcp)
{
	return dcp->fw_compat == DCP_FIRMWARE_H17P &&
	       dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17G &&
	       dcp->connector_type == DRM_MODE_CONNECTOR_eDP &&
	       dcp_has_panel(dcp);
}

static void dcp_dpms_power_off(struct apple_dcp *dcp);
static void dcp_dpms_power_on(struct apple_dcp *dcp);
static void dcp_s2r_prewarm_work(struct work_struct *work);
static void dcp_s2r_prewarm_off_work(struct work_struct *work);

static bool dcp_uses_soft_dpms(struct apple_dcp *dcp)
{
	/* The 14.7 panel has its own known-level blank and restore hooks. */
	return (!iomfb_v14_7_panel_nits(dcp) &&
		dcp_backlight_active(dcp) && dcp_has_panel(dcp)) ||
	       ((dcp->fw_compat == DCP_FIRMWARE_H17P || dcp->fw_compat == DCP_FIRMWARE_V_27_0) &&
		dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G &&
		dcp->connector_type == DRM_MODE_CONNECTOR_eDP);
}

int dcp_typec_prepare_route(struct apple_dcp *dcp)
{
	u64 generation;
	unsigned long ready;
	int ret;

	if (!dcp_crtc_needs_route_start(dcp))
		return 0;
	scoped_guard(mutex, &dcp->hpd_mutex)
		generation = dcp->typec_generation;
	ret = dcp_dptx_connect(dcp, 0);
	if (ret)
		return ret;
	ready = wait_for_completion_timeout(&dcp->typec_iomfb_hpd_ready,
					    msecs_to_jiffies(3000));
	guard(mutex)(&dcp->hpd_mutex);
	if (generation != dcp->typec_generation || !dcp->typec_cable_connected)
		return -ESTALE;
	return ready ? 0 : -ETIMEDOUT;
}

void dcp_poweron(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	bool wait_for_typec_hpd = false;
	unsigned long remaining;
	int ret;

	/* Resume from disk: the restart powers the pipe, dcp_hib_recommit(). */
	if (dcp_hib_gated(dcp))
		return;

	/* The takeover trial powered it on before the eviction. */
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP &&
	    dcp->desktop->trial_powered) {
		dcp->desktop->trial_powered = false;
		return;
	}
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP)
		dcp->desktop->ext_pipe_off = false;

	if (iomfb_is_m4(dcp)) {
		ret = dcp_backlight_dpms(dcp, true);
		if (ret)
			dev_warn(dcp->dev, "panel brightness restore failed: %d\n", ret);
		return;
	}

	if (dcp_is_external_h17p(dcp)) {
		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
		cancel_work_sync(&dcp->external_stop_work);
		cancel_delayed_work_sync(&dcp->external_stop_retry_wq);
		guard(mutex)(&dcp->external_lock);
		ret = dcp_external_start_locked(dcp);
		if (!ret)
			dcp->external_crtc_generation = dcp->external_generation;
		else
			dev_err(dcp->dev, "external display activation failed: %d\n", ret);
		return;
	}

	if (dcp_uses_soft_dpms(dcp)) {
		dcp_dpms_power_on(dcp);
		if ((dcp->fw_compat == DCP_FIRMWARE_H17P || dcp->fw_compat == DCP_FIRMWARE_V_27_0) &&
		    dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G &&
		    !READ_ONCE(dcp->pipe_enabled_h17p)) {
			if (dcp->fw_compat == DCP_FIRMWARE_V_27_0)
				iomfb_poweron_v27(dcp);
			else
				iomfb_poweron_h17p(dcp);
			if (!READ_ONCE(dcp->pipe_enabled_h17p)) {
				dev_err(dcp->dev, "initial display pipe enable failed\n");
				WRITE_ONCE(dcp->crashed, true);
				return;
			}
			/* The powerlog takeover report normally precedes this. */
			if (dcp_has_panel(dcp))
				queue_delayed_work(system_freezable_wq,
						   &dcp->bl_fallback_wq,
						   DCP_BL_FALLBACK_DELAY);
		}
		ret = dcp_backlight_dpms(dcp, true);
		if (ret)
			dev_warn(dcp->dev, "backlight restore unavailable: %d\n", ret);
		return;
	}

	if (dcp_h17g_panel(dcp)) {
		/* The pipe never stopped; the next swap restores the level. */
		dcp->brightness.update = true;
		return;
	}

	if (dcp_is_typec_output(dcp)) {
		/* This CRTC now owns the current Type-C display session. */
		scoped_guard(mutex, &dcp->hpd_mutex)
			dcp->crtc_unplugs = dcp->typec_unplugs;
		wait_for_typec_hpd = READ_ONCE(dcp->typec_crtc_off) &&
				    READ_ONCE(dcp->typec_cable_connected);
		WRITE_ONCE(dcp->typec_crtc_off, false);
		WRITE_ONCE(dcp->typec_follow_start, false);
		/*
		 * A DPMS wake retrains the link like a replug: give it a fresh
		 * relink budget. No relink runs while the CRTC was off.
		 */
		if (wait_for_typec_hpd) {
			unsigned long flags;

			spin_lock_irqsave(&dcp->relink_lock, flags);
			dcp_relink_wake(&dcp->relink,
					READ_ONCE(dcp->typec_generation), jiffies);
			spin_unlock_irqrestore(&dcp->relink_lock, flags);
		}

		/*
		 * A Type-C CRTC disable releases its DPTX session. Re-establish it
		 * synchronously before IOMFB is powered back on.
		 */
		if (READ_ONCE(dcp->typec_cable_connected)) {
			cancel_delayed_work(&dcp->typec_reconnect_wq);
			dcp->typec_reconnect_tries = 0;
			/* the modeset of this commit follows, see dcpep_cb_hotplug() */
			WRITE_ONCE(dcp->typec_poweron_hpd, true);
			ret = dcp_dptx_connect(dcp, 0);
			if (ret)
				dcp_queue_typec_reconnect(dcp, DPTX_RECONNECT_DELAY);
			else if (wait_for_typec_hpd) {
				remaining = wait_for_completion_timeout(
					&dcp->typec_iomfb_hpd_ready,
					msecs_to_jiffies(3000));
				if (!remaining)
					dev_warn(dcp->dev,
						 "Type-C IOMFB hotplug not ready on wake\n");
			}
			WRITE_ONCE(dcp->typec_poweron_hpd, false);
		}
	} else if (dcp->hdmi_hpd) {
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

		/* A yielded hybrid is about to take a Type-C display. */
		if (connected && !READ_ONCE(dcp->hdmi_yielded))
			dcp_dptx_connect(dcp, 0);
	}

	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_poweron_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_poweron_v13_3(dcp);
		break;
	case DCP_FIRMWARE_V_14_7_TEMPLATE:
		iomfb_poweron_v14_7_0(dcp);
		break;
	case DCP_FIRMWARE_V_26_6_DESKTOP:
		iomfb_poweron_v26_6_desktop(dcp);
		break;
	case DCP_FIRMWARE_V_26_6:
		iomfb_v26_6_poweron(dcp);
		break;
	case DCP_FIRMWARE_V_14_7:
		iomfb_v14_7_poweron(dcp);
		break;
	case DCP_FIRMWARE_H17P:
		iomfb_poweron_h17p(dcp);
		break;
	case DCP_FIRMWARE_V_27_0:
		iomfb_poweron_v27(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
	if (dcp->avep)
		av_service_connect(dcp);
}

/*
 * A desktop 26.x external pipe that the firmware powered off with its display: an unplug, or
 * the link released for a sleep. A mode set alone leaves it off ("Modeset done, but pipe not
 * enabled") and the firmware swallows every swap after it. The mode is valid from then on,
 * so the flush that would power the pipe on for the first swap (dcp_flush()) leaves it too.
 * A mode set on a CRTC that stayed active, as the hotplug worker's when the display is back
 * before the compositor has flipped once, powers such a pipe on first.
 */
bool dcp_external_pipe_off(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP && !dcp->main_display &&
	       dcp->desktop->ext_pipe_off;
}

/* Same-CRTC helper dependencies order this token refresh before later disables. */
void dcp_external_recover(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp_is_external_h17p(dcp))
		dcp_poweron(pdev);
}

void dcp_poweroff(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	bool display_gone = false;
	int ret;

	/* Resume from disk: no firmware session to power off yet. */
	if (dcp_hib_gated(dcp))
		return;

	if (iomfb_is_m4(dcp)) {
		ret = dcp_backlight_dpms(dcp, false);
		if (!ret)
			ret = iomfb_m4_blank(dcp);
		if (ret)
			dev_warn(dcp->dev, "panel blank failed: %d\n", ret);
		return;
	}

	if (dcp_is_external_h17p(dcp)) {
		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
		guard(mutex)(&dcp->external_lock);
		ret = dcp_external_stop_locked(dcp, dcp->external_crtc_generation);
		if (ret && !dcp->external_stop_deferred)
			dev_err(dcp->dev, "external display stop failed: %d\n", ret);
		dcp_external_crtc_released(dcp);
		return;
	}

	cancel_delayed_work(&dcp->swap_watchdog_wq);

	/*
	 * Internal H17P DPMS is a brightness present, never a pipe stop. With hw.s2r the
	 * display then powers off as well, after the backlight.
	 */
	if (dcp_uses_soft_dpms(dcp)) {
		ret = dcp_backlight_dpms(dcp, false);
		if (ret)
			dev_warn(dcp->dev, "backlight blank unavailable: %d\n", ret);
		dcp_dpms_power_off(dcp);
		return;
	}

	if (dcp_h17g_panel(dcp)) {
		iomfb_blank_h17p(dcp);
		return;
	}

	if (dcp->avep)
		av_service_disconnect(dcp);

	/*
	 * The display this CRTC was powered on for has been unplugged since:
	 * its link went down with the cable (dcp_dptx_disconnect_oob()), and
	 * the desktop only now turns the CRTC off. By then the pipeline may
	 * already drive the next display, on this port or on another one that
	 * was waiting for it, and that connection is not the CRTC's to
	 * release: dropping its HPD makes the firmware report the new display
	 * inactive, and it stays dark.
	 *
	 * This applies to every Type-C output.  In particular, an M1/M2 dock can
	 * disappear and return while its old disconnect is still making the
	 * desktop turn the CRTC off.  Releasing the replacement session then
	 * leaves the dock in USB2-only mode with no later sink attention to
	 * trigger a relink.
	 */
	if (dcp_is_typec_output(dcp)) {
		scoped_guard(mutex, &dcp->hpd_mutex)
			display_gone = dcp->typec_unplugs != dcp->crtc_unplugs;
		if (display_gone) {
			dev_dbg(dcp->dev,
				"CRTC off after its display was unplugged: leaving the current connection alone\n");
			_dcp_poweroff(dcp);
			return;
		}
	}

	/*
	 * Powering a Type-C CRTC off drops DCP's synthetic HPD, and the firmware
	 * reports that as an unplug. The display is still attached: keep the
	 * connector connected (see dcpep_cb_hotplug()) and let dcp_poweron()
	 * re-establish the DPTX session. Recreating it here instead makes the
	 * display vanish and come back, and compositors light a returning
	 * display, so DPMS off never sticks. Cable removal is reported through
	 * the Type-C mux.
	 */
	if (dcp_is_typec_output(dcp)) {
		reinit_completion(&dcp->typec_iomfb_hpd_ready);
		if (READ_ONCE(dcp->typec_cable_connected))
			WRITE_ONCE(dcp->typec_crtc_off, true);
		/* dcp_poweron() reconnects the link on DPMS wake. */
		cancel_delayed_work(&dcp->typec_reconnect_wq);
	}

	_dcp_poweroff(dcp);

	if (dcp_is_typec_output(dcp)) {
		ret = 0;
		/* DCP owns a synthetic HPD for Type-C. Release it with the CRTC. */
		if (dptx_service_available(&dcp->dptxport[0]) && dcp->dptxport[0].connected) {
			ret = dptxport_set_hpd(dcp->dptxport[0].service, false);
			if (ret)
				dev_warn(dcp->dev,
					 "failed to deassert Type-C DPTX HPD: %d\n", ret);
			dcp_dptx_disconnect(dcp, 0);
		}
		/*
		 * A native external pipe whose display is no longer described
		 * (it went away during a hotplug bounce) is powered off by the
		 * desktop for good: with HPD still high, connect the link again
		 * so that the firmware describes the display anew.
		 */
		if (dcp->external_native && READ_ONCE(dcp->typec_cable_connected) &&
		    (ret || !dcp->connector || !READ_ONCE(dcp->connector->connected)))
			dcp_external_retry(dcp, ret ? "display link released with an HPD error" :
					   "display link released with no display described",
					   ret, 500);
		/* and the second tile's, which dcp_poweron() brings back too */
		if (READ_ONCE(dcp->split.active) && dcp->dptxport[1].enabled &&
		    dcp->dptxport[1].connected) {
			dptxport_set_hpd(dcp->dptxport[1].service, false);
			dcp_dptx_disconnect(dcp, 1);
		}
	} else if (dcp->hdmi_hpd) {
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		if (!connected) {
			disconnected_hpd_event(dcp->connector);
			dcp_dptx_disconnect(dcp, 0);
		}
	}
}

/* Desktop 26.x: the DRM device exists now, register the panel backlight. */
void dcp_backlight_takeover_registered(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (!dcp || dcp->fw_compat != DCP_FIRMWARE_V_26_6_DESKTOP ||
	    !dcp_has_panel(dcp) || !dcp->brightness.maximum)
		return;
	schedule_work(&dcp->bl_register_wq);
}

VISIBLE_IF_KUNIT void dcp_work_register_backlight(struct work_struct *work)
{
	int ret;
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, bl_register_wq);

	mutex_lock(&dcp->bl_register_mutex);
	if (READ_ONCE(dcp->quiescing) || dcp->brightness.bl_dev)
		goto out_unlock;

	/* try to register backlight device, */
	ret = dcp_backlight_register(dcp);
	if (ret == -ENODATA)
		goto out_unlock;
	if (ret) {
		dev_err(dcp->dev, "Unable to register backlight device: %d\n", ret);
		/*
		 * The H17P policy encodes the panel ceiling into every present
		 * and keeps working without a class device; only older
		 * firmware stops sending brightness here.
		 */
		if (!dcp_backlight_active(dcp))
			dcp->brightness.maximum = 0;
	}

out_unlock:
	mutex_unlock(&dcp->bl_register_mutex);
}

static void dcp_work_update_backlight(struct work_struct *work)
{
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, bl_update_wq);

	dcp_backlight_update(dcp);
}

/*
 * H17P never publishes the panel level, and the loader's level arrives only
 * as an optional powerlog report.  Without one, register the backlight at
 * the middle of the panel range.  That value is only reported: no level is
 * presented until userspace writes one, so the panel keeps the loader's
 * level.
 */
static void dcp_work_backlight_fallback(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work),
					     struct apple_dcp, bl_fallback_wq);
	u32 nits = dcp->brightness.maximum / 2;
	int ret;

	if (READ_ONCE(dcp->quiescing) || READ_ONCE(dcp->crashed) ||
	    dcp_backlight_active(dcp))
		return;

	ret = iomfb_configure_backlight_h17p(dcp, dcp->brightness.maximum,
					     false, 0, true, nits);
	if (!ret)
		dev_info(dcp->dev,
			 "no loader brightness reported; backlight starts at %u nits without a panel change\n",
			 nits);
	else if (ret != -EBUSY)
		dev_warn(dcp->dev, "backlight registration failed: %d\n", ret);
}

static void dcp_release_piodma_iommu_dev(struct apple_dcp *dcp)
{
	if (dcp->retain_dma) {
		WRITE_ONCE(dcp->quiescing, true);
		return;
	}
	if (dcp->piodma) {
		if (dcp->piodma_created)
			of_platform_device_destroy(&dcp->piodma->dev, NULL);
		else
			put_device(&dcp->piodma->dev);
	}
	dcp->piodma = NULL;
	dcp->iommu_dom = NULL;
	dcp->piodma_created = false;
}

void dcp_retain_framebuffer(struct platform_device *pdev,
			    struct dcp_fb_reference *entry)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	mutex_lock(&dcp->swapped_out_fbs_lock);
	list_add_tail(&entry->head, &dcp->swapped_out_fbs);
	mutex_unlock(&dcp->swapped_out_fbs_lock);
}

void dcp_arm_retained_framebuffers(struct apple_dcp *dcp, u32 swap_id)
{
	struct dcp_fb_reference *entry;

	mutex_lock(&dcp->swapped_out_fbs_lock);
	list_for_each_entry(entry, &dcp->swapped_out_fbs, head) {
		if (entry->armed)
			continue;
		entry->swap_id = swap_id;
		entry->armed = true;
	}
	mutex_unlock(&dcp->swapped_out_fbs_lock);
}

void dcp_unarm_retained_framebuffers(struct apple_dcp *dcp, u32 swap_id)
{
	struct dcp_fb_reference *entry;

	mutex_lock(&dcp->swapped_out_fbs_lock);
	list_for_each_entry(entry, &dcp->swapped_out_fbs, head)
		if (entry->armed && entry->swap_id == swap_id)
			entry->armed = false;
	mutex_unlock(&dcp->swapped_out_fbs_lock);
}

void dcp_release_retained_framebuffers(struct apple_dcp *dcp, u32 swap_id)
{
	struct dcp_fb_reference *entry, *tmp;
	LIST_HEAD(completed);

	mutex_lock(&dcp->swapped_out_fbs_lock);
	list_for_each_entry_safe(entry, tmp, &dcp->swapped_out_fbs, head) {
		if (!entry->armed || entry->swap_id != swap_id)
			continue;
		list_move_tail(&entry->head, &completed);
	}
	mutex_unlock(&dcp->swapped_out_fbs_lock);

	list_for_each_entry_safe(entry, tmp, &completed, head) {
		list_del(&entry->head);
		drm_framebuffer_put(entry->fb);
		kfree(entry);
	}
}

void dcp_release_all_retained_framebuffers(struct apple_dcp *dcp)
{
	struct dcp_fb_reference *entry, *tmp;
	LIST_HEAD(completed);

	mutex_lock(&dcp->swapped_out_fbs_lock);
	list_splice_init(&dcp->swapped_out_fbs, &completed);
	mutex_unlock(&dcp->swapped_out_fbs_lock);

	list_for_each_entry_safe(entry, tmp, &completed, head) {
		list_del(&entry->head);
		drm_framebuffer_put(entry->fb);
		kfree(entry);
	}
}

static int dcp_create_piodma_iommu_dev(struct apple_dcp *dcp)
{
	int ret;
	struct device_node *node __free(device_node) = of_get_child_by_name(dcp->dev->of_node, "piodma");

	if (!node)
		return dev_err_probe(dcp->dev, -ENODEV,
				     "Failed to get piodma child DT node\n");

	dcp->piodma = of_find_device_by_node(node);
	dcp->piodma_created = !dcp->piodma;
	if (dcp->piodma_created)
		dcp->piodma = of_platform_device_create(node, NULL, dcp->dev);
	if (!dcp->piodma)
		return dev_err_probe(dcp->dev, -ENODEV,
				     "Failed to create piodma pdev for %pOF\n", node);

	ret = dma_set_mask_and_coherent(&dcp->piodma->dev, DMA_BIT_MASK(42));
	if (ret)
		goto err_destroy_pdev;

	ret = of_dma_configure(&dcp->piodma->dev, node, true);
	if (ret) {
		ret = dev_err_probe(dcp->dev, ret,
			"Failed to configure IOMMU child DMA\n");
		goto err_destroy_pdev;
	}

	dcp->iommu_dom = iommu_get_domain_for_dev(&dcp->piodma->dev);
	if (IS_ERR(dcp->iommu_dom)) {
		ret = dev_err_probe(dcp->dev, PTR_ERR(dcp->iommu_dom),
				    "Failed to get default iommu domain for "
				    "piodma device\n");
		dcp->iommu_dom = NULL;
		goto err_destroy_pdev;
	}

	return 0;
err_destroy_pdev:
	dcp_release_piodma_iommu_dev(dcp);
	return ret;
}

static int dcp_get_bw_scratch_reg(struct apple_dcp *dcp, u32 expected)
{
	struct of_phandle_args ph_args;
	u32 addr_idx, disp_idx, offset;
	int ret;

	ret = of_parse_phandle_with_args(dcp->dev->of_node, "apple,bw-scratch",
				   "#apple,bw-scratch-cells", 0, &ph_args);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to read 'apple,bw-scratch': %d\n", ret);
		return ret;
	}

	if (ph_args.args_count != 3) {
		dev_err(dcp->dev, "Unexpected 'apple,bw-scratch' arg count %d\n",
			ph_args.args_count);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	addr_idx = ph_args.args[0];
	disp_idx = ph_args.args[1];
	offset = ph_args.args[2];

	if (disp_idx != expected || disp_idx >= MAX_DISP_REGISTERS) {
		dev_err(dcp->dev, "Unexpected disp_reg value in 'apple,bw-scratch': %d\n",
			disp_idx);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	ret = of_address_to_resource(ph_args.np, addr_idx, &dcp->disp_bw_scratch_res);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to get 'apple,bw-scratch' resource %d from %pOF\n",
			addr_idx, ph_args.np);
		goto err_of_node_put;
	}
	if (offset > resource_size(&dcp->disp_bw_scratch_res) - 4) {
		ret = -EINVAL;
		goto err_of_node_put;
	}

	dcp->disp_registers[disp_idx] = &dcp->disp_bw_scratch_res;
	dcp->disp_bw_scratch_index = disp_idx;
	dcp->disp_bw_scratch_offset = offset;
	ret = 0;

err_of_node_put:
	of_node_put(ph_args.np);
	return ret;
}

static int dcp_get_bw_doorbell_reg(struct apple_dcp *dcp, u32 expected)
{
	struct of_phandle_args ph_args;
	u32 addr_idx, disp_idx;
	int ret;

	ret = of_parse_phandle_with_args(dcp->dev->of_node, "apple,bw-doorbell",
				   "#apple,bw-doorbell-cells", 0, &ph_args);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to read 'apple,bw-doorbell': %d\n", ret);
		return ret;
	}

	if (ph_args.args_count != 2) {
		dev_err(dcp->dev, "Unexpected 'apple,bw-doorbell' arg count %d\n",
			ph_args.args_count);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	addr_idx = ph_args.args[0];
	disp_idx = ph_args.args[1];

	if (disp_idx != expected || disp_idx >= MAX_DISP_REGISTERS) {
		dev_err(dcp->dev, "Unexpected disp_reg value in 'apple,bw-doorbell': %d\n",
			disp_idx);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	ret = of_address_to_resource(ph_args.np, addr_idx, &dcp->disp_bw_doorbell_res);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to get 'apple,bw-doorbell' resource %d from %pOF\n",
			addr_idx, ph_args.np);
		goto err_of_node_put;
	}
	dcp->disp_bw_doorbell_index = disp_idx;
	dcp->disp_registers[disp_idx] = &dcp->disp_bw_doorbell_res;
	ret = 0;

err_of_node_put:
	of_node_put(ph_args.np);
	return ret;
}

static int dcp_get_disp_regs(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	int count = 0;
	int i, ret;
	char name[16];
	const char *reg_name;
	struct resource *res;

	if (of_property_present(dcp->dev->of_node, "reg-names")) {
		ret = of_property_count_strings(dcp->dev->of_node, "reg-names");
		if (ret < 0)
			return ret;

		for (i = 0; i < ret; i++) {
			if (of_property_read_string_index(dcp->dev->of_node,
							  "reg-names", i, &reg_name))
				return -EINVAL;
			if (!strncmp(reg_name, "disp-", 5))
				count++;
		}
	} else {
		count = pdev->num_resources - 1;
	}

	if (count <= 0 || count > MAX_DISP_REGISTERS)
		return -EINVAL;

	for (i = 0; i < count; ++i) {
		if (of_property_present(dcp->dev->of_node, "reg-names")) {
			snprintf(name, sizeof(name), "disp-%d", i);
			res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							   name);
		} else {
			res = platform_get_resource(pdev, IORESOURCE_MEM, 1 + i);
		}
		if (!res)
			return -EINVAL;
		dcp->disp_registers[i] = res;
	}

	if (dcp->hw.firmware_clock) {
		unsigned int windows = dcp->hw.firmware_display_windows;
		bool external = dcp->hw.iomfb_method_profile ==
			DCP_IOMFB_METHODS_H17P_EXTERNAL;

		if (!windows || windows >= MAX_DISP_REGISTERS)
			return -EINVAL;
		if (external) {
			/* The external clock aperture is already described by disp-4. */
			if (count != windows + 1)
				return -EINVAL;
			res = dcp->disp_registers[windows];
			if (!apple_dcp_external_clock_window(res->start,
							     resource_size(res),
							     dcp->hw.firmware_clock->start,
							     resource_size(dcp->hw.firmware_clock)))
				return -EINVAL;
			count = windows;
		}
		if (count != windows || resource_size(dcp->hw.firmware_clock) < 4 ||
		    dcp->hw.firmware_scratch > resource_size(dcp->hw.firmware_clock) - 4 ||
		    dcp->hw.firmware_request > resource_size(dcp->hw.firmware_clock) - 4)
			return -EINVAL;

		dcp->disp_bw_scratch_res = *dcp->hw.firmware_clock;
		dcp->disp_bw_scratch_index = count;
		dcp->disp_bw_scratch_offset = dcp->hw.firmware_scratch;
		dcp->disp_bw_doorbell_res = dcp->disp_bw_scratch_res;
		dcp->disp_bw_doorbell_res.start += dcp->hw.firmware_request;
		dcp->disp_bw_doorbell_res.end = dcp->disp_bw_doorbell_res.start + 3;
		dcp->disp_registers[count] = &dcp->disp_bw_scratch_res;
		dcp->nr_disp_registers = count + 1;
		return 0;
	}

	/* load pmgr bandwidth scratch resource and offset */
	ret = dcp_get_bw_scratch_reg(dcp, count);
	if (ret < 0)
		return ret;
	count += 1;

	/* load pmgr bandwidth doorbell resource if present (only on t8103) */
	if (of_property_present(dcp->dev->of_node, "apple,bw-doorbell")) {
		ret = dcp_get_bw_doorbell_reg(dcp, count);
		if (ret < 0)
			return ret;
		count += 1;
	}

	dcp->nr_disp_registers = count;
	for (i = 0; i < count; i++)
		dcp->disp_firmware_base[i] = dcp->disp_registers[i]->start;
	if (of_property_present(dcp->dev->of_node, "apple,firmware-physical-regs")) {
		if (of_property_count_u64_elems(dcp->dev->of_node,
					       "apple,firmware-physical-regs") != count)
			return -EINVAL;
		ret = of_property_read_u64_array(dcp->dev->of_node,
					       "apple,firmware-physical-regs",
					       dcp->disp_firmware_base, count);
		if (ret)
			return ret;
		for (i = 0; i < count; i++)
			if (!IS_ALIGNED(dcp->disp_firmware_base[i], 4) ||
			    resource_size(dcp->disp_registers[i]) >
			    U64_MAX - dcp->disp_firmware_base[i])
				return -EINVAL;
	}
	return 0;
}

#define DCP_FW_VERSION_MIN_LEN	3
#define DCP_FW_VERSION_MAX_LEN	5
#define DCP_FW_VERSION_STR_LEN	(DCP_FW_VERSION_MAX_LEN * 4)

static int dcp_read_fw_version(struct device *dev, const char *name,
			       char *version_str)
{
	u32 ver[DCP_FW_VERSION_MAX_LEN];
	int len_str;
	int len;

	len = of_property_read_variable_u32_array(dev->of_node, name, ver,
						  DCP_FW_VERSION_MIN_LEN,
						  DCP_FW_VERSION_MAX_LEN);

	switch (len) {
	case 3:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d", ver[0], ver[1], ver[2]);
		break;
	case 4:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d.%d", ver[0], ver[1], ver[2],
				    ver[3]);
		break;
	case 5:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d.%d.%d", ver[0], ver[1], ver[2],
				    ver[3], ver[4]);
		break;
	default:
		len_str = strscpy(version_str, "UNKNOWN",
				  DCP_FW_VERSION_STR_LEN);
		if (len >= 0)
			len = -EOVERFLOW;
		break;
	}

	if (len_str >= DCP_FW_VERSION_STR_LEN)
		dev_warn(dev, "'%s' truncated: '%s'\n", name, version_str);

	return len;
}

static enum dcp_firmware_version dcp_check_firmware_version(struct device *dev)
{
	const struct apple_dcp_hw_data *hw = of_device_get_match_data(dev);
	const struct dcp_v14_board *v14_board;
	char compat_str[DCP_FW_VERSION_STR_LEN];
	char fw_str[DCP_FW_VERSION_STR_LEN];
	unsigned int num_missing;
	const char *missing;
	int ret;

	/*
	 * T8142 (J813) on macOS 27.0: the H17P interface written for its 26.x
	 * firmware makes the 27.0 DCP firmware panic right after the first
	 * hotplug ("ap_link: callback failed with: 0x5"). It runs the T6050's
	 * firmware-27 generation.
	 */
	if (of_device_is_compatible(dev->of_node, "apple,t8142-dcp")) {
		ret = dcp_read_fw_version(dev, "apple,firmware-compat", compat_str);
		if (ret >= 0 && !strcmp(compat_str, "27.0.0"))
			return DCP_FIRMWARE_V_27_0;
	}

	if (hw->firmware_compat == DCP_FIRMWARE_V_27_0) {
		ret = dcp_read_fw_version(dev, "apple,firmware-compat", compat_str);
		if (ret < 0 || !iomfb_v27_methods(compat_str)) {
			dev_err(dev, "DCP firmware-compat %s has no 27.x method table\n",
				ret < 0 ? "missing" : compat_str);
			return DCP_FIRMWARE_UNKNOWN;
		}
		missing = iomfb_v27_missing_method(iomfb_v27_methods(compat_str),
						   &num_missing);
		if (missing) {
			dev_err(dev, "DCP firmware-compat %s not started: no ID known for %u methods (first: %s)\n",
				compat_str, num_missing, missing);
			return DCP_FIRMWARE_UNKNOWN;
		}
		return DCP_FIRMWARE_V_27_0;
	}

	/*
	 * SoCs introduced with H17-generation firmware pin its interface; the
	 * loader may not recognise their boot firmware version.
	 */
	if (hw->firmware_compat != DCP_FIRMWARE_UNKNOWN)
		return hw->firmware_compat;

	/* firmware version is just informative */
	ret = dcp_read_fw_version(dev, "apple,firmware-version", fw_str);

	/*
	 * T6041 (M4 Pro/Max) is driven only with the 26.5.2 and 26.6.2 firmware
	 * its desktop IOMFB instance was written for: the boot firmware and its
	 * compatibility version must both be exactly the same one of those.
	 */
	if (of_device_is_compatible(dev->of_node, "apple,t6041-dcp") ||
	    of_device_is_compatible(dev->of_node, "apple,t6041-dcpext")) {
		int compat_ret = dcp_read_fw_version(dev, "apple,firmware-compat",
						     compat_str);

		if (ret >= 0 && compat_ret >= 0 && !strcmp(fw_str, compat_str) &&
		    (!strcmp(fw_str, "26.5.2") || !strcmp(fw_str, "26.6.2")))
			return DCP_FIRMWARE_V_26_6_DESKTOP;
		dev_err(dev, "T6041 display not started: DCP firmware %s (compat %s) is not 26.5.2 or 26.6.2\n",
			ret >= 0 ? fw_str : "missing",
			compat_ret >= 0 ? compat_str : "missing");
		return DCP_FIRMWARE_UNKNOWN;
	}

	ret = dcp_read_fw_version(dev, "apple,firmware-compat", compat_str);
	if (ret < 0) {
		dev_err(dev, "Could not read 'apple,firmware-compat': %d\n", ret);
		return DCP_FIRMWARE_UNKNOWN;
	}

	/* 25G83 (J613; J615 experimental) has a distinct callback table, never a v14 fallback. */
	if (of_device_is_compatible(dev->of_node, "apple,t8122-dcp") &&
	    of_property_present(dev->of_node, "apple,j613-25g83-profile")) {
		const char *uuid;
		u32 profile;

		if (apple_t8122_25g83_board() &&
		    !of_property_read_u32(dev->of_node, "apple,j613-25g83-profile", &profile) &&
		    profile == 1 && !strcmp(compat_str, "26.6.2") &&
		    !of_property_read_string(dev->of_node, "apple,firmware-uuid", &uuid) &&
		    !strcmp(uuid, "C042E95C-B9D8-3F0E-94B3-582A08AA6FDD"))
			return DCP_FIRMWARE_V_26_6;
		return DCP_FIRMWARE_UNKNOWN;
	}

	/*
	 * T6030 and the internal panels with a board record (T6030, J613) run
	 * the 14.7 firmware session handed over by the boot loader natively
	 * (iomfb_v14_7.c); other 14.7.0 DCPs keep the template below.
	 * The T6030 external processors: the 14.x firmware IOMFB only.
	 */
	if (of_device_is_compatible(dev->of_node, "apple,t6030-dcpext")) {
		if (!strcmp(compat_str, "14.7.0"))
			return DCP_FIRMWARE_V_14_7;
		dev_err(dev, "T6030 display not started: DCP firmware-compat %s is not 14.7.0\n",
			compat_str);
		return DCP_FIRMWARE_UNKNOWN;
	}

	/*
	 * The T8122 external processor: Linux starts it and drives 14.7
	 * firmware through the classic IOMFB template, as on M1/M2.
	 */
	if (of_device_is_compatible(dev->of_node, "apple,t8122-dcpext")) {
		if (!strcmp(compat_str, "14.7.0"))
			return DCP_FIRMWARE_V_14_7_TEMPLATE;
		dev_err(dev, "T8122 external display not started: DCP firmware-compat %s is not 14.7.0\n",
			compat_str);
		return DCP_FIRMWARE_UNKNOWN;
	}

	/* The internal panels with a board record (T6030, J613): the same. */
	v14_board = iomfb_v14_7_board(dev);
	if (IS_ERR(v14_board))
		return DCP_FIRMWARE_UNKNOWN;
	if (v14_board) {
		if (!strcmp(compat_str, "14.7.0"))
			return DCP_FIRMWARE_V_14_7;
		dev_err(dev, "%s display not started: DCP firmware-compat %s is not 14.7.0\n",
			iomfb_v14_7_board_name(v14_board), compat_str);
		return DCP_FIRMWARE_UNKNOWN;
	}

	if (strncmp(compat_str, "12.3.0", sizeof(compat_str)) == 0)
		return DCP_FIRMWARE_V_12_3;
	/*
	 * m1n1 reports firmware version 13.5 as compatible with 13.3. This is
	 * only true for the iomfb endpoint. The interface for the dptx-port
	 * endpoint changed between 13.3 and 13.5. The driver will only support
	 * firmware 13.5. Check the actual firmware version for compat version
	 * 13.3 until m1n1 reports 13.5 as "firmware-compat".
	 */
	else if ((strncmp(compat_str, "13.3.0", sizeof(compat_str)) == 0) &&
		 (strncmp(fw_str, "13.5.0", sizeof(compat_str)) == 0))
		return DCP_FIRMWARE_V_13_5;
	else if (strncmp(compat_str, "13.5.0", sizeof(compat_str)) == 0)
		return DCP_FIRMWARE_V_13_5;
	else if (strncmp(compat_str, "14.7.0", sizeof(compat_str)) == 0 &&
		(strncmp(fw_str, "14.7.0", sizeof(compat_str)) == 0))
		return DCP_FIRMWARE_V_14_7_TEMPLATE;

	dev_err(dev, "DCP firmware-compat %s (FW: %s) is not supported\n",
		compat_str, fw_str);

	return DCP_FIRMWARE_UNKNOWN;
}

static int dcp_connector_type_from_dt(struct device_node *np)
{
	if (of_property_match_string(np, "apple,connector-type", "HDMI-A") >= 0)
		return DRM_MODE_CONNECTOR_HDMIA;
	if (of_property_match_string(np, "apple,connector-type", "DP") >= 0)
		return DRM_MODE_CONNECTOR_DisplayPort;
	if (of_property_match_string(np, "apple,connector-type", "USB-C") >= 0)
		return DRM_MODE_CONNECTOR_USB;

	return DRM_MODE_CONNECTOR_Unknown;
}

/*
 * The fixed route's crossbar selection and Type-C mux are taken at probe and
 * must be released with the device: a crossbar left selected blocks the next
 * probe's mux_control_select() for good.
 */
static void dcp_release_fixed_route(void *data)
{
	struct apple_dcp *dcp = data;

	if (dcp->fixed_route_selected) {
		mux_control_deselect(dcp->fixed_xbar);
		dcp->fixed_route_selected = false;
	}
	typec_mux_put(dcp->typec_mux);
	dcp->typec_mux = NULL;
}

VISIBLE_IF_KUNIT void dcp_disable_typec_work(struct apple_dcp *dcp, bool release_cable)
{
	scoped_guard(mutex, &dcp->hpd_mutex) {
		dcp->typec_work_enabled = false;
		if (release_cable)
			WRITE_ONCE(dcp->typec_cable_connected, false);
		dcp->typec_generation++;
		dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
		/* Nothing arms the timeout work past its drain below. */
		dcp->announce.stopped = true;
	}
	if (release_cable && dcp_is_external_h17p(dcp))
		dpavservep_detach(dcp);
	/* Block new enqueues as well as draining users of the AFK endpoints. */
	disable_delayed_work_sync(&dcp->typec_reconnect_wq);
	disable_delayed_work_sync(&dcp->placeholder_edid_wq);
	scoped_guard(spinlock_irqsave, &dcp->relink_lock)
		dcp_relink_enable(&dcp->relink, false);
	disable_delayed_work_sync(&dcp->typec_relink_wq);
	disable_delayed_work_sync(&dcp->announce_wq);
	cancel_work_sync(&dcp->external_stop_work);
	cancel_delayed_work_sync(&dcp->external_stop_retry_wq);
	disable_delayed_work_sync(&dcp->hdmi_settle_wq);
	disable_delayed_work_sync(&dcp->hdmi_recheck_wq);
	disable_delayed_work_sync(&dcp->link_recovery_wq);
	disable_work_sync(&dcp->tb_event_work);
	scoped_guard(mutex, &dcp->tb_event_lock) {
		dcp->tb_events = 0;
		dcp->tb_event_route = NULL;
		dcp->tb_event_generation = 0;
	}
	disable_delayed_work_sync(&dcp->hotplug_retrain_retry_wq);
	disable_delayed_work_sync(&dcp->hybrid_wq);
	dcp_hdmi_disable(dcp);
}

VISIBLE_IF_KUNIT void dcp_enable_typec_work(struct apple_dcp *dcp)
{
	enable_delayed_work(&dcp->typec_reconnect_wq);
	enable_delayed_work(&dcp->placeholder_edid_wq);
	enable_delayed_work(&dcp->typec_relink_wq);
	scoped_guard(spinlock_irqsave, &dcp->relink_lock)
		dcp_relink_enable(&dcp->relink, true);
	enable_delayed_work(&dcp->announce_wq);
	enable_delayed_work(&dcp->hdmi_settle_wq);
	enable_delayed_work(&dcp->hdmi_recheck_wq);
	enable_delayed_work(&dcp->link_recovery_wq);
	enable_work(&dcp->tb_event_work);
	enable_delayed_work(&dcp->hotplug_retrain_retry_wq);
	enable_delayed_work(&dcp->hybrid_wq);
	dcp_hdmi_enable(dcp);
	scoped_guard(mutex, &dcp->hpd_mutex)
		dcp->typec_work_enabled = true;
	/* A cable can be routed before the DRM component binds. */
	if (READ_ONCE(dcp->typec_cable_connected))
		dcp_queue_typec_reconnect(dcp, 0);
	if (dcp_is_external_h17p(dcp)) {
		struct apple_connector *con = READ_ONCE(dcp->connector);

		guard(mutex)(&dcp->hpd_mutex);
		dcp->announce.stopped = false;
		/* A hold that crossed system sleep waits for the new link from now. */
		dcp_announce_arm(dcp);
		/* Send an event that came due while stopped. */
		if (dcp_announce_owed(&dcp->announce) && con &&
		    READ_ONCE(con->dcp) == to_platform_device(dcp->dev))
			schedule_work(&con->hotplug_wq);
	}
}

static int dcp_firmware_metadata_v27(struct apple_dcp *dcp)
{
	struct device *dev = dcp->dev;
	struct device_node *fb;
	struct resource resource;
	int clocks, index, ret;

	/*
	 * The framebuffer region is the one the boot loader scanned out of; it
	 * backs the firmware's default framebuffer.  An external processor the
	 * boot loader drove no display from (T6050's dcpext4, the HDMI port's)
	 * has none: it binds without one, and its default-framebuffer requests
	 * are refused (dfb_region_size 0).
	 */
	index = of_property_match_string(dev->of_node, "memory-region-names",
					 "framebuffer");
	if (index == -ENODATA || index == -EINVAL) {
		dcp->dfb_region_size = 0;
		dev_info(dev, "no boot framebuffer\n");
		goto clocks;
	}
	if (index < 0)
		return index;
	fb = of_parse_phandle(dev->of_node, "memory-region", index);
	if (!fb)
		return -EINVAL;
	ret = of_address_to_resource(fb, 0, &resource);
	of_node_put(fb);
	if (ret)
		return ret;
	dcp->dfb_region_size = resource_size(&resource);
	if (!dcp->dfb_region_size)
		return -EINVAL;

clocks:
	clocks = of_property_count_u32_elems(dev->of_node, "apple,clock-ids");
	if (clocks == -ENODATA &&
	    of_property_present(dev->of_node, "apple,clock-ids"))
		clocks = 0;
	if (clocks < 0 || clocks > 64)
		return -EINVAL;
	/*
	 * A DCP that also boots earlier firmware (T8142) lists the clock those
	 * use first and names the provider clocks "firmware-<index>". Take only
	 * the named ones here.
	 */
	if (clocks &&
	    of_property_match_string(dev->of_node, "clock-names", "firmware-0") >= 0) {
		dcp->firmware_clocks = devm_kcalloc(dev, clocks,
						    sizeof(*dcp->firmware_clocks),
						    GFP_KERNEL);
		if (!dcp->firmware_clocks)
			return -ENOMEM;
		for (index = 0; index < clocks; index++) {
			dcp->firmware_clocks[index].id =
				devm_kasprintf(dev, GFP_KERNEL, "firmware-%d", index);
			if (!dcp->firmware_clocks[index].id)
				return -ENOMEM;
		}
		ret = devm_clk_bulk_get(dev, clocks, dcp->firmware_clocks);
		if (ret)
			return ret;
		dcp->num_firmware_clocks = clocks;
		return 0;
	}
	ret = devm_clk_bulk_get_all(dev, &dcp->firmware_clocks);
	if (ret < 0)
		return ret;
	if (ret != clocks)
		return -EINVAL;
	dcp->num_firmware_clocks = clocks;
	return 0;
}

static bool dcp_external_is_idle(struct apple_dcp *dcp)
{
	u32 control = readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	u32 status = readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS);

	/* Same externally inherited idle state accepted by the public loader. */
	return (control & APPLE_DCP_COPROC_CPU_CONTROL_RUN) &&
	       (status & APPLE_DCP_COPROC_CPU_STATUS_STOPPED) &&
	       (status & APPLE_DCP_COPROC_CPU_STATUS_IDLE);
}

/*
 * The boot loader keeps the notch rows in its framebuffer when the OS is to
 * drive the full panel (aura: chosen.asahi,show-notch=1). Follow it, so the
 * mode covers the same rows as the boot framebuffer did.
 */
static bool boot_show_notch(void)
{
	const char *val;

	return !of_property_read_string(of_chosen, "asahi,show-notch", &val) &&
	       !strcmp(val, "1");
}

/*
 * T8122 dcpext (DCP_FIRMWARE_V_14_7_TEMPLATE): if the PMP runs, the display
 * gate added its DISPEXT request and pointed the dcpext at it
 * (apple,pmp-report). As on T6030, run the processor only once the PMP has
 * acknowledged that request: the running PMP owns the SoC power and
 * performance states and treats a block whose request it has not taken as
 * idle. The internal display started the PMP long before this, so the wait
 * is short. It is bounded, and without an acknowledgment the processor is
 * not started: the bind fails, the dcpext becomes an unusable pipe and the
 * internal display goes on without it. A dcpext without the request (a
 * device tree from before it existed) starts as before.
 */
#define DCP_EXT_PMP_TIMEOUT_MS	3000

/*
 * The PMP report's wait-ready entry point is optional at link time: until a
 * provider implements it, behave as its !CONFIG_APPLE_PMP_REPORT stub does.
 */
static int dcp_ext_pmp_wait_ready(struct device_node *entry,
				  unsigned long timeout)
{
	typeof(&apple_pmp_report_wait_ready) wait;
	int ret;

	wait = symbol_get(apple_pmp_report_wait_ready);
	if (!wait)
		return -ENODEV;
	ret = wait(entry, timeout);
	symbol_put(apple_pmp_report_wait_ready);
	return ret;
}

static int dcp_ext_wait_pmp(struct apple_dcp *dcp)
{
	struct device_node *entry;
	unsigned long deadline;
	int ret;

	if (!dcp_ext_dptx_607(dcp))
		return 0;
	entry = of_parse_phandle(dcp->dev->of_node, "apple,pmp-report", 0);
	if (!entry) {
		dev_info(dcp->dev, "no PMP DISPEXT request; starting without one\n");
		return 0;
	}
	deadline = jiffies + msecs_to_jiffies(DCP_EXT_PMP_TIMEOUT_MS);
	for (;;) {
		ret = dcp_ext_pmp_wait_ready(entry,
					     msecs_to_jiffies(DCP_EXT_PMP_TIMEOUT_MS));
		/* -EIO: the PMP runs, but has not acknowledged the request yet. */
		if (ret != -EIO || time_after(jiffies, deadline))
			break;
		msleep(20);
	}
	of_node_put(entry);
	if (ret) {
		dev_err(dcp->dev,
			"the PMP has not acknowledged the DISPEXT request (%d); not starting the external display processor\n",
			ret);
		return ret;
	}
	dev_info(dcp->dev, "PMP acknowledged the DISPEXT request\n");
	return 0;
}

/* Runs the coprocessor CPU; returns CPU_CONTROL as it was before. */
static u32 dcp_coproc_assert_run(struct apple_dcp *dcp)
{
	u32 cpu_ctrl = readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);

	writel_relaxed(cpu_ctrl | APPLE_DCP_COPROC_CPU_CONTROL_RUN,
		       dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	return cpu_ctrl;
}

static int dcp_comp_bind_pipe(struct device *dev, struct device *main, void *data)
{
	struct device_node *panel_np;
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	u32 cpu_ctrl;
	int ret;

	if (dcp->fw_compat == DCP_FIRMWARE_V_27_0) {
		INIT_LIST_HEAD(&dcp->iomfb_boolean_properties);
		ret = devm_add_action_or_reset(dev,
				       iomfb_firmware_state_cleanup_v27, dcp);
		if (ret)
			return ret;
	}

	if (READ_ONCE(dcp->quiescing))
		return dev_err_probe(dev, -EBUSY,
				     "Live DCP session cannot be rebound; reboot required\n");

	if (iomfb_is_m4(dcp)) {
		ret = dcp_pin_live_session(dcp);
		if (ret)
			return ret;
		ret = iomfb_m4_start(dcp, main);
		if (!ret)
			WRITE_ONCE(dcp->active, true);
		return ret;
	}
	if (dcp->vblank_works_disabled) {
		enable_work(&dcp->vblank_wq);
		enable_delayed_work(&dcp->swap_watchdog_wq);
		dcp->vblank_works_disabled = false;
	}

	if (dcp->external_prepared) {
		av_audiosrv_reports_restart(dcp);
		return 0;
	}

	/* A timed-out prior session may still scan these mappings. */
	mutex_lock(&dcp->swapped_out_fbs_lock);
	ret = list_empty(&dcp->swapped_out_fbs) ? 0 : -EBUSY;
	mutex_unlock(&dcp->swapped_out_fbs_lock);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Previous scanout has not been stopped\n");

	ret = dma_set_mask_and_coherent(dev,
				DMA_BIT_MASK(dcp->hw.firmware_dma_bits ?: 42));
	if (ret)
		return ret;

	/* A native external processor mapped these at probe, for its start work. */
	if (!dcp->external_native) {
		dcp->coproc_reg = devm_platform_ioremap_resource_byname(to_platform_device(dev),
									"coproc");
		if (IS_ERR(dcp->coproc_reg))
			return PTR_ERR(dcp->coproc_reg);
	}

	/* Record the state the boot loader handed the coprocessor over in. */
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP)
		dev_info(dev, "coprocessor handoff: CPU_CONTROL %#x CPU_STATUS %#x\n",
			 readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL),
			 readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS));

	/*
	 * Display coprocessors running H17-generation firmware are left running
	 * by the bootloader with their firmware mapped through a locked DART.
	 * Linux attaches to that session with a standard RTKit INIT/HELLO
	 * exchange and must not stop or restart the ASC.
	 */
	if (dcp->hw.adopt_live_session) {
		cpu_ctrl = readl_relaxed(dcp->coproc_reg +
					 APPLE_DCP_COPROC_CPU_CONTROL);
		if (!(cpu_ctrl & APPLE_DCP_COPROC_CPU_CONTROL_RUN))
			return dev_err_probe(dev, -EOPNOTSUPP,
					     "DCP was not left running by the bootloader\n");
	}

	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL &&
	    !dcp_external_is_idle(dcp))
		return dev_err_probe(dev, -EBUSY,
				     "External DCP is not in the inherited idle state\n");

	if (dcp->index || dcp->dptx_phy || dcp->dptx_die)
		dev_info(dev, "DCP index:%u dptx target phy: %u dptx die: %u\n",
			 dcp->index, dcp->dptx_phy, dcp->dptx_die);

	if (!show_notch && !boot_show_notch())
		ret = of_property_read_u32(dev->of_node, "apple,notch-height",
					   &dcp->notch_height);

	if (dcp->notch_height > MAX_NOTCH_HEIGHT)
		dcp->notch_height = MAX_NOTCH_HEIGHT;
	if (dcp->notch_height > 0)
		dev_info(dev, "Detected display with notch of %u pixel\n", dcp->notch_height);

	/* initialize brightness scale to a sensible default to avoid divide by 0*/
	dcp->brightness.scale = 65536;
	panel_np = of_get_compatible_child(dev->of_node, "apple,panel-mini-led");
	if (panel_np)
		dcp->panel.has_mini_led = true;
	else
		panel_np = of_get_compatible_child(dev->of_node, "apple,panel");

	if (panel_np) {
		if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP) {
			/*
			 * Desktop 26.x swaps carry nits, and the panel node
			 * describes the panel whether or not the boot loader
			 * enabled it.  Without apple,max-brightness take the
			 * J616s panel's user-accessible SDR maximum.
			 */
			if (of_property_read_u32(panel_np, "apple,max-brightness",
						 &dcp->brightness.maximum))
				dcp->brightness.maximum = DCP_DESKTOP_MAX_NITS;
		} else if (of_device_is_available(panel_np)) {
			ret = of_property_read_u32(panel_np, "apple,max-brightness",
						   &dcp->brightness.maximum);
			if (ret)
				dev_err(dev, "Missing property 'apple,max-brightness'\n");
		}

		dcp_panel_read_dimensions(dcp, panel_np);
		of_node_put(panel_np);
		/* Read target metadata for H17P and desktop firmware alike. */
		dcp_panel_read_adt(dcp);
		dcp->fixed_connector_type = DRM_MODE_CONNECTOR_eDP;
		dcp->connector_type = DRM_MODE_CONNECTOR_eDP;
	}

	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6) {
		ret = iomfb_v26_6_bind(dcp);
		if (!ret) {
			enable_work(&dcp->dimensions_wq);
			av_audiosrv_reports_restart(dcp);
		}
		return ret;
	}

	/* The running T6030 firmware is adopted as is. */
	if (dcp->fw_compat == DCP_FIRMWARE_V_14_7) {
		if (dcp->external_native) {
			WRITE_ONCE(dcp->external_detached, false);
			enable_work(&dcp->vblank_wq);
			enable_work(&dcp->external_audio_work);
			enable_delayed_work(&dcp->swap_watchdog_wq);
			enable_delayed_work(&dcp->external_retry_wq);
			enable_work(&dcp->dimensions_wq);
			dcp_enable_typec_work(dcp);
			av_audiosrv_reports_restart(dcp);
			return 0;
		}
		if (dcp->external) {
			av_audiosrv_reports_restart(dcp);
			return 0;
		}
		ret = iomfb_v14_7_bind(dcp);
		/* Balances the disable at probe, as below. */
		if (!ret) {
			enable_work(&dcp->dimensions_wq);
			dcp_enable_typec_work(dcp);
			av_audiosrv_reports_restart(dcp);
		}
		return ret;
	}

	ret = dcp_create_piodma_iommu_dev(dcp);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to create PIODMA iommu child device\n");
	if (!dcp->iommu_dom) {
		ret = dev_err_probe(dev, -EPROBE_DEFER,
				    "PIODMA iommu domain is unavailable\n");
		goto err_piodma;
	}

	ret = dcp_get_disp_regs(dcp);
	if (ret) {
		dev_err(dev, "failed to find display registers\n");
		goto err_piodma;
	}

	if (dcp->fw_compat == DCP_FIRMWARE_V_27_0) {
		ret = dcp_firmware_metadata_v27(dcp);
		if (ret)
			goto err_piodma;
	}
	/*
	 * T6041: the device tree carries no DCP clock, and the boot loader
	 * left the coprocessor clocked; D408 then reports 0 Hz, as the
	 * driver this instance was ported from did.
	 */
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP ||
	    (dcp->fw_compat == DCP_FIRMWARE_V_27_0 && !dcp->num_firmware_clocks))
		dcp->clk = devm_clk_get_optional(dev, NULL);
	else
		dcp->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(dcp->clk)) {
		ret = dev_err_probe(dev, PTR_ERR(dcp->clk),
				    "Unable to find clock\n");
		goto err_piodma;
	}
	dcp->clk_194 = devm_clk_get_optional(dev, "clock-194");
	if (IS_ERR(dcp->clk_194)) {
		ret = dev_err_probe(dev, PTR_ERR(dcp->clk_194),
				    "Unable to find clock 0x194\n");
		goto err_piodma;
	}

	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL &&
	    (!dcp->clk_194 || !clk_get_rate(dcp->clk) ||
	     !clk_get_rate(dcp->clk_194))) {
		ret = dev_err_probe(dev, -EINVAL,
				    "External display clocks are unavailable\n");
		goto err_piodma;
	}

	bitmap_zero(dcp->memdesc_map, DCP_MAX_MAPPINGS);
	// TDOD: mem_desc IDs start at 1, for simplicity just skip '0' entry
	set_bit(0, dcp->memdesc_map);

	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL) {
		/* Pin before the first write or mailbox wake can expose DMA. */
		ret = dcp_pin_live_session(dcp);
		if (ret)
			goto err_piodma;
		if (!dcp_external_is_idle(dcp)) {
			ret = dev_err_probe(dev, -EBUSY,
					    "External DCP changed state before startup\n");
			goto err_piodma;
		}
	}

	ret = dcp_ext_wait_pmp(dcp);
	if (ret)
		goto err_piodma;

	if (!dcp->hw.adopt_live_session)
		cpu_ctrl = dcp_coproc_assert_run(dcp);

	/* Registering the mailbox can already expose RTKit buffer requests. */
	ret = dcp_pin_live_session(dcp);
	if (ret)
		goto err_piodma;
	/* External H17P supplies shared-buffer addresses during RTKit startup. */
	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL)
		dcp->rtk = apple_rtkit_init(dev, dcp, "mbox", 0,
					    &external_h17p_rtkit_ops);
	else
		dcp->rtk = apple_rtkit_init(dev, dcp, "mbox", 0, &rtkit_ops);
	if (IS_ERR(dcp->rtk)) {
		ret = dev_err_probe(dev, PTR_ERR(dcp->rtk),
				    "Failed to initialize RTKit\n");
		goto err_piodma;
	}
	ret = devm_add_action_or_reset(dev, dcp_release_rtkit, dcp);
	if (ret)
		goto err_piodma;
	dcp_s2r_check_dt(dcp);
	/* After a warm restart its log counters need echoing, or it refuses its next idle. */
	if (dcp->hw.s2r || dcp->hw.s2r_ext)
		apple_rtkit_set_oslog_echo(dcp->rtk, true);
	ret = dcp_secure_port_init(dcp);
	if (ret)
		goto err_piodma;

	if (dcp->hw.adopt_live_session)
		dev_dbg(dev, "negotiating RTKit with the running DCP\n");

	/* Announce host readiness before awaiting external IOP readiness. */
	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL)
		ret = apple_rtkit_wake_early_ap(dcp->rtk);
	else if (dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP)
		ret = 0;	/* woken in dcp_start_desktop() */
	else
		ret = apple_rtkit_wake(dcp->rtk);
	if (ret) {
		ret = dev_err_probe(dev, ret, "Failed to boot RTKit: %d\n", ret);
		goto err_piodma;
	}
	dcp_secure_port_start(dcp);
	enable_work(&dcp->dimensions_wq);
	schedule_work(&dcp->dimensions_wq);
	dcp_enable_typec_work(dcp);
	av_audiosrv_reports_restart(dcp);
	return 0;

err_piodma:
	dcp_release_piodma_iommu_dev(dcp);
	return ret;
}

static void dcp_ext_give_up(struct apple_dcp *dcp, int err, const char *when);

/*
 * The external processors whose failure must not take the DRM device down:
 * T8122's, T6041's and T6050's dcpext. One that fails to probe or bind joins
 * as an unusable pipe (ext_dead), and the other displays go on without it.
 * With every processor failed the DRM device has no pipe and does not bind.
 */
static bool dcp_ext_is_failsafe(struct device_node *np)
{
	return of_device_is_compatible(np, "apple,t8122-dcpext") ||
	       of_device_is_compatible(np, "apple,t6041-dcpext") ||
	       of_device_is_compatible(np, "apple,t6050-dcpext");
}

/*
 * A failsafe dcpext (dcp_ext_is_failsafe()) boots its firmware here. If that
 * fails, it must not take the other displays' DRM device down with it: keep
 * it as an unusable pipe. Its routes have registered by now and cannot be
 * taken back, so a deferral here would be final as well: probe waits for the
 * suppliers bind looks up (dcp_ext_bind_suppliers()) so that none comes here.
 */
static int dcp_comp_bind(struct device *dev, struct device *main, void *data)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	int ret;

	if (READ_ONCE(dcp->ext_dead))
		return 0;
	dcp_peer_bound_26_6(dcp);
	ret = dcp_comp_bind_pipe(dev, main, data);
	if (ret && dcp->ext_failsafe) {
		dcp_ext_give_up(dcp, ret, "bind");
		return 0;
	}
	return ret;
}

/*
 * We need to shutdown DCP before tearing down the display subsystem. Otherwise
 * the DCP will crash and briefly flash a green screen of death.
 */
VISIBLE_IF_KUNIT void dcp_comp_unbind(struct device *dev, struct device *main, void *data)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	if (!dcp)
		return;
	dcp_peer_forget(dcp);
	/*
	 * Stop the native initializer before looking up its owner, including
	 * the dead-session path. Owner allocation may have landed in an open
	 * bind devres group; finish its RPC work while endpoint RX is alive.
	 */
	if (dcp->external_native)
		disable_work_sync(&dcp->external_audio_work);
	av_audiosrv_quiesce(dcp);
	if (READ_ONCE(dcp->ext_dead))
		return;

	if (iomfb_is_m4(dcp)) {
		WRITE_ONCE(dcp->quiescing, true);
		dcp_backlight_stop(dcp);
		cancel_work_sync(&dcp->bl_register_wq);
		cancel_work_sync(&dcp->bl_update_wq);
		iomfb_m4_quiesce(dcp);
		if (dcp->crtc && !dcp->drm_retained) {
			drm_dev_get(dcp->crtc->base.dev);
			dcp->drm_retained = true;
		}
		dev_warn(dev, "retaining live DCP DMA resources after unbind; reboot required\n");
		return;
	}

	disable_work_sync(&dcp->dimensions_wq);
	dcp_peer_forget(dcp);
	if (dcp->fw_compat == DCP_FIRMWARE_V_26_6) {
		iomfb_v26_6_unbind(dcp);
		return;
	}
	if (dcp->fw_compat == DCP_FIRMWARE_V_14_7) {
		if (dcp->external_native) {
			/* Bind's wait_ready enabled it, as for any HDMI port. */
			if (dcp->hdmi_hpd_irq)
				disable_irq(dcp->hdmi_hpd_irq);
			dcp_disable_typec_work(dcp, true);
			disable_delayed_work_sync(&dcp->external_retry_wq);
			disable_delayed_work_sync(&dcp->swap_watchdog_wq);
			disable_work_sync(&dcp->vblank_wq);
		} else if (!dcp->external) {
			dcp_disable_typec_work(dcp, false);
		}
		iomfb_v14_7_unbind(dcp);
		return;
	}

	if (dcp->retain_dma) {
		/* No firmware stop is acknowledged: detach host users, never DMA. */
		scoped_guard(mutex, &dcp->iomfb.lock)
			WRITE_ONCE(dcp->quiescing, true);
		cancel_work_sync(&dcp->external_stop_work);
		cancel_delayed_work_sync(&dcp->external_stop_retry_wq);
		if (dcp->crtc && !dcp->drm_retained) {
			drm_dev_get(dcp->crtc->base.dev);
			dcp->drm_retained = true;
		}
		iomfb_queue_stop(dcp);
		afk_quiesce(dcp->avep);
		afk_quiesce(dcp->dptxep);
		afk_quiesce(dcp->dptx_controller_ep);
		afk_quiesce(dcp->ibootep);
		afk_quiesce(dcp->systemep);
		afk_quiesce(dcp->dcpavservep);
		cancel_delayed_work_sync(&dcp->bl_fallback_wq);
		dcp_backlight_stop(dcp);
		if (dcp->connector_type == DRM_MODE_CONNECTOR_eDP) {
			cancel_work_sync(&dcp->bl_register_wq);
			cancel_work_sync(&dcp->bl_update_wq);
		}
		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		/*
		 * Stop the announcement before draining the hotplug work: that
		 * work arms the timeout work, which in turn queues it.
		 */
		scoped_guard(mutex, &dcp->hpd_mutex)
			dcp->announce.stopped = true;
		disable_delayed_work_sync(&dcp->announce_wq);
		dcp_test_point(dcp, DCP_TEST_UNBIND_ANNOUNCE_STOPPED);
		cancel_work_sync(&dcp->vblank_wq);
		if (dcp->connector)
			cancel_work_sync(&dcp->connector->hotplug_wq);
		if (dcp->typec_connector && dcp->typec_connector != dcp->connector)
			cancel_work_sync(&dcp->typec_connector->hotplug_wq);
		dev_warn(dev, "retaining live DCP DMA resources after unbind; reboot required\n");
		return;
	}

	if (dcp->hdmi_hpd_irq)
		disable_irq(dcp->hdmi_hpd_irq);

	dcp_disable_typec_work(dcp, true);
	/* RTKit is released after this unbind callback, and can still deliver
	 * a final swap completion or hotplug while its receive queue drains.
	 * The works are set up at probe; the next bind enables them again.
	 */
	disable_delayed_work_sync(&dcp->swap_watchdog_wq);
	disable_work_sync(&dcp->vblank_wq);
	dcp->vblank_works_disabled = true;

	if (dcp->fixed_connector_type == DRM_MODE_CONNECTOR_eDP) {
		/* Registration runs asynchronously and its devres can belong
		 * to the platform device rather than the component bind group.
		 * Stop callbacks and userspace writes while the CRTC, RTKit and
		 * IOMMU they use are still available. The works are set up at
		 * probe, so cancel rather than disable them: a rebind uses them.
		 * Firmware callbacks may still queue them until the endpoints
		 * are shut down, so they are cancelled again below.
		 */
		cancel_delayed_work_sync(&dcp->bl_fallback_wq);
		dcp_backlight_stop(dcp);
		cancel_work_sync(&dcp->bl_register_wq);
		cancel_work_sync(&dcp->bl_update_wq);
		if (dcp->brightness.bl_dev) {
			devm_backlight_device_unregister(dev, dcp->brightness.bl_dev);
			dcp->brightness.bl_dev = NULL;
		}
	}

	if (!IS_ERR_OR_NULL(dcp->avep)) {
		av_service_disconnect(dcp);
		afk_shutdown(dcp->avep);
		dcp->avep = NULL;
	}

	dcp_fabric_shutdown_dptx(dcp);

	if (!IS_ERR_OR_NULL(dcp->dptx_controller_ep)) {
		afk_shutdown(dcp->dptx_controller_ep);
		dcp->dptx_controller_ep = NULL;
	}

	if (dcp->ibootep) {
		afk_shutdown(dcp->ibootep);
		dcp->ibootep = NULL;
	}

	if (dcp->systemep) {
		afk_shutdown(dcp->systemep);
		dcp->systemep = NULL;
	}

	/*
	 * The auxiliary endpoints a desktop 26.x start brought up, still
	 * routed while their shutdown is acknowledged.
	 */
	for (int i = 0; dcp->desktop && i < ARRAY_SIZE(dcp->desktop->graph); i++) {
		struct apple_dcp_afkep *ep = dcp->desktop->graph[i];

		if (ep) {
			afk_shutdown(ep);
			WRITE_ONCE(dcp->desktop->graph[i], NULL);
		}
	}

	if (dcp->dcpavservep) {
		afk_shutdown(dcp->dcpavservep);
		dpavservep_detach(dcp);
		dcp->dcpavservep = NULL;
	}

	if (dcp->shmem)
		iomfb_shutdown(dcp);

	iomfb_queue_stop(dcp);
	dcp_release_piodma_iommu_dev(dcp);

	cancel_delayed_work_sync(&dcp->bl_fallback_wq);
	dcp_backlight_stop(dcp);
	if (dcp->connector_type == DRM_MODE_CONNECTOR_eDP) {
		cancel_work_sync(&dcp->bl_register_wq);
		cancel_work_sync(&dcp->bl_update_wq);
	}
	devm_clk_put(dev, dcp->clk);
	dcp->clk = NULL;
	/* optional and possibly NULL, released with the bind's devres group */
	dcp->clk_194 = NULL;
}

static const struct component_ops dcp_comp_ops = {
	.bind	= dcp_comp_bind,
	.unbind	= dcp_comp_unbind,
};

/* How long probe leaves a converter found powered switched off. */
#define DCP_HDMI_POWER_OFF_MS	250

/* The first probe has looked at the converter's power (one per machine). */
static bool dcp_hdmi_power_checked;

/*
 * Power the DP-to-HDMI converter at probe. Its rails are SMC GPIOs, and the
 * SMC keeps running across a reboot: a converter the kernel before left on is
 * still on here, and requesting its GPIOs high changes nothing. Its HDMI link
 * and the display's input then start in whatever state they were left in.
 * Switch it off first in that case, as an unplug does, so that the link comes
 * up from a power-up. An unreadable rail counts as on. Only the first probe
 * looks: one deferred after this point finds the converter powered by the
 * attempt before. The rails must be stable before DCP requests the link.
 */
static int dcp_hdmi_power_up(struct apple_dcp *dcp)
{
	struct gpio_desc *rail[] = { dcp->hdmi_pwren, dcp->dp2hdmi_pwren };
	bool on = false;
	int i, ret;

	if (!dcp_hdmi_power_checked) {
		dcp_hdmi_power_checked = true;
		for (i = 0; i < ARRAY_SIZE(rail); i++)
			if (rail[i] && gpiod_get_value_cansleep(rail[i]) != 0)
				on = true;
	}
	if (on) {
		for (i = 0; i < ARRAY_SIZE(rail); i++) {
			if (!rail[i])
				continue;
			ret = gpiod_direction_output(rail[i], 0);
			if (ret)
				return ret;
		}
		msleep(DCP_HDMI_POWER_OFF_MS);
		dev_info(dcp->dev, "HDMI: converter was powered at probe, power-cycled\n");
	}
	for (i = 0; i < ARRAY_SIZE(rail); i++) {
		if (!rail[i])
			continue;
		ret = gpiod_direction_output(rail[i], 1);
		if (ret)
			return ret;
	}
	msleep(100);
	return 0;
}

/*
 * The converter's own rail goes and comes with its link: the port
 * controller powers the converter when its port is connected to a pipe and
 * takes the power away when the display is released, at an unplug and at
 * every system sleep (the enable is a count of users; J616s has no
 * "pcon-stays-on"). HDMI's +5 V stays on, HPD needs it.
 * A converter left powered through a T6040 S2R, its DP lanes gone and back
 * under it, locked to no link rate after the wake. One still on when a link
 * starts is switched off first, as probe does.
 */
static void dcp_hdmi_converter_power(struct apple_dcp *dcp, bool on)
{
	if (!dcp->dp2hdmi_pwren)
		return;
	if (on && gpiod_get_value_cansleep(dcp->dp2hdmi_pwren) != 0) {
		gpiod_set_value_cansleep(dcp->dp2hdmi_pwren, 0);
		msleep(DCP_HDMI_POWER_OFF_MS);
	}
	gpiod_set_value_cansleep(dcp->dp2hdmi_pwren, on);
	if (on)
		msleep(100);
	dcp->hdmi_converter_fresh = on;
}

/*
 * A T6030 external processor's fixed (HDMI) output is optional. Its Type-C
 * routes must register whatever happens to that output: every USB-C port
 * lists them and waits for them, as the panel's DRM device waits for this
 * processor. Keep deferring while the HDMI parts may still appear, then go
 * on as a Type-C-only pipe. Elsewhere the error ends the probe, as before.
 */
static int dcp_fixed_output_error(struct apple_dcp *dcp, int err, const char *what)
{
	if (!dcp->external_native)
		return err;
	if (err == -EPROBE_DEFER) {
		err = driver_deferred_probe_check_state(dcp->dev);
		if (err == -EPROBE_DEFER)
			return err;
	}
	dev_err(dcp->dev, "HDMI output unusable (%s: %d); USB-C displays are not affected\n",
		what, err);
	if (dcp->fixed_route_selected)
		mux_control_deselect(dcp->fixed_xbar);
	dcp->fixed_route_selected = false;
	dcp->fixed_xbar = NULL;
	dcp->xbar = NULL;
	if (!IS_ERR_OR_NULL(dcp->typec_mux))
		typec_mux_put(dcp->typec_mux);
	dcp->typec_mux = NULL;
	dcp->phy_managed_by_typec = false;
	/* A requested HPD interrupt stays disabled: nothing enables it now. */
	dcp->hdmi_hpd_irq = 0;
	dcp->hdmi_hpd = NULL;
	dcp->hdmi_pwren = NULL;
	dcp->dp2hdmi_pwren = NULL;
	dcp->phy = NULL;
	dcp->fixed_phy = NULL;
	dcp->fixed_connector_type = DRM_MODE_CONNECTOR_USB;
	dcp->connector_type = DRM_MODE_CONNECTOR_USB;
	return 0;
}

/*
 * A T6030 external processor whose Type-C routes did not register: disable
 * them in the device tree so that the ports stop waiting for them, and add
 * the processor to the DRM device as a pipe that never starts, so that the
 * panel is not held up either.
 */
static int dcp_native_routes_error(struct apple_dcp *dcp, int err)
{
	if (!dcp->external_native)
		return err;
	if (err == -EPROBE_DEFER) {
		err = driver_deferred_probe_check_state(dcp->dev);
		if (err == -EPROBE_DEFER)
			return err;
	}
	dev_err(dcp->dev, "Type-C display routes unusable: %d; USB-C ports go on without this processor\n",
		err);
	dcp_typec_routes_disable(dcp);
	return 0;
}

/* The fixed PHY probe powered, captured when its release was registered. */
struct dcp_desktop_phy_hold {
	struct apple_dcp *dcp;
	struct phy *phy;
};

static void dcp_desktop_phy_off(void *data)
{
	struct dcp_desktop_phy_hold *hold = data;

	/* Given back for hibernation and not powered again, see dcp.c. */
	if (hold->dcp->hib_fixed_phy_off)
		return;
	phy_power_off(hold->phy);
	phy_exit(hold->phy);
}

/*
 * The fixed HDMI output of a desktop 26.x external processor (T6041 dcpext0):
 * its DP PHY is powered here, as the boot loader's working sequence does. A
 * Type-C output (dcpext1) has no fixed PHY; the display fabric powers the
 * PHY of the route a port claims (see dcp_typec_phy_on()).
 */
static int dcp_desktop_probe_routes(struct apple_dcp *dcp)
{
	struct dcp_desktop_phy_hold *hold;
	struct device *dev = dcp->dev;
	int ret;

	if (!dcp_desktop_external(dcp) || !dcp->phy)
		return 0;

	hold = devm_kmalloc(dev, sizeof(*hold), GFP_KERNEL);
	if (!hold)
		return -ENOMEM;
	hold->dcp = dcp;
	hold->phy = dcp->phy;
	ret = phy_init(dcp->phy);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize dp-phy\n");
	ret = phy_power_on(dcp->phy);
	if (ret) {
		phy_exit(dcp->phy);
		return dev_err_probe(dev, ret, "Failed to power on dp-phy\n");
	}
	ret = devm_add_action_or_reset(dev, dcp_desktop_phy_off, hold);
	if (ret)
		return ret;
	msleep(25);
	return 0;
}

/*
 * An adopted T8132 dcpext offers its Type-C routes. It is no component of
 * the DRM device (its outputs are added by t8132_ext_drm.c): the routes
 * follow the port modes, refuse DP alt mode and get no apple_connector (see
 * dcp_typec_route_set() and dcp_typec_port_unusable()). Its route suppliers were there before the
 * adoption, see dcp_platform_probe_t8132_ext(). A failure leaves the routes
 * disabled, so the port controllers do not wait for them; the adopted
 * session stays.
 */
static void dcp_t8132_ext_routes(struct apple_dcp *dcp)
{
	struct device_node *routes __free(device_node) =
		of_get_child_by_name(dcp->dev->of_node, "typec-routes");
	int ret;

	if (!routes || !of_get_available_child_count(routes)) {
		dev_info(dcp->dev, "no Type-C display routes enabled\n");
		return;
	}
	/*
	 * A Thunderbolt DP tunnel's DPTX connect runs from the Type-C reconnect
	 * work, which component bind would otherwise enable (as for the manual
	 * T6030 external path). Enabled before the routes are, since a tunnel
	 * can claim one as soon as it is registered; disabled again on remove.
	 */
	enable_delayed_work(&dcp->typec_reconnect_wq);
	ret = dcp_register_typec_routes(dcp);
	if (ret) {
		dev_err(dcp->dev, "Type-C display routes not registered: %d; USB goes on without them\n",
			ret);
		disable_delayed_work_sync(&dcp->typec_reconnect_wq);
		dcp_typec_routes_disable(dcp);
		return;
	}
	dev_info(dcp->dev, "%u Type-C display route(s) registered, no DP alt mode\n",
		 dcp->nr_typec_routes);
}

static int dcp_s2r_pm_register(struct apple_dcp *dcp);
#ifdef CONFIG_HIBERNATION
static void dcp_hib_restore_work(struct work_struct *work);
static int dcp_hib_register(struct apple_dcp *dcp);
#endif

static int dcp_platform_probe_pipe(struct platform_device *pdev)
{
	const struct apple_dcp_hw_data *hw = of_device_get_match_data(&pdev->dev);
	enum dcp_firmware_version fw_compat;
	struct device *dev = &pdev->dev;
	struct apple_dcp *dcp;
	int ret, surf, num_surfs;
	bool routes_failed = false;
	u32 surf_en;
	u32 mux_index;

	if (dcp_session_retained(dev))
		return dev_err_probe(dev, -EBUSY, "Previous live DCP session requires a reboot\n");

	/* Reject unsupported external firmware before replacing the boot display. */
	if (of_device_is_compatible(dev->of_node, "apple,t8140-dcpext") &&
	    !dcp_h17p_external_supported(dev->of_node)) {
		dev_err(dev, "Unsupported external display hardware or firmware handoff\n");
		return -ENODEV;
	}

	fw_compat = dcp_check_firmware_version(dev);
	if (fw_compat == DCP_FIRMWARE_UNKNOWN)
		return -ENODEV;
	if (fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP) {
		ret = dcp_validate_t6041_dt(dev);
		if (ret)
			return ret;
	}

	/* Check for "apple,bw-scratch" to avoid probing appledrm with outdated
	 * device trees. This prevents replacing simpledrm and ending up without
	 * display.
	 */
	if (fw_compat != DCP_FIRMWARE_V_14_7 &&
	    fw_compat != DCP_FIRMWARE_V_26_6 &&
	    fw_compat != DCP_FIRMWARE_M4 &&
	    !of_property_present(dev->of_node, "apple,bw-scratch") &&
	    !hw->firmware_clock)
		return dev_err_probe(dev, -ENODEV, "Incompatible devicetree! "
			"Use devicetree matching this kernel.\n");

	dcp = kzalloc_obj(*dcp);
	if (!dcp)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, dcp_release_context, dcp);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&dcp->swapped_out_fbs);
	mutex_init(&dcp->swapped_out_fbs_lock);
	ret = dcp_backlight_init(dcp, dev);
	if (ret)
		return ret;
	iomfb_queue_init(dcp);

	dcp->fw_compat = fw_compat;
	/*
	 * Probe-lifetime like the desktop state: component bind opens a devres
	 * group on this device and releases it on failure or unbind.
	 */
	if (!iomfb_is_m4(dcp)) {
		struct device_node *panel_np =
			of_get_compatible_child(dev->of_node, "apple,panel-mini-led") ?:
			of_get_compatible_child(dev->of_node, "apple,panel");

		if (panel_np) {
			of_node_put(panel_np);
			dcp->panel_identity = devm_kzalloc(dev, sizeof(*dcp->panel_identity),
							   GFP_KERNEL);
			if (!dcp->panel_identity)
				return -ENOMEM;
		}
	}
	if (fw_compat == DCP_FIRMWARE_V_27_0) {
		char compat_str[DCP_FW_VERSION_STR_LEN];

		/* Checked by dcp_check_firmware_version(). */
		dcp_read_fw_version(dev, "apple,firmware-compat", compat_str);
		dcp->v27_methods = iomfb_v27_methods(compat_str);
	}
	if (fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP) {
		dcp->desktop = devm_kzalloc(dev, sizeof(*dcp->desktop), GFP_KERNEL);
		if (!dcp->desktop)
			return -ENOMEM;
		spin_lock_init(&dcp->desktop->hdr_lock);
		/*
		 * The opaque client identity of the IOMFB calls.  The firmware's
		 * own client passes a kernel pointer there; pass a random value
		 * of the same shape instead.
		 */
		dcp->desktop->client = GENMASK_ULL(63, 48) |
				       (get_random_u64() & GENMASK_ULL(47, 4));
	}
	dcp->ext_failsafe = dcp_ext_is_failsafe(dev->of_node);
	dcp->external = of_device_is_compatible(dev->of_node, "apple,t6030-dcpext");
	dcp->desktop_external = of_device_is_compatible(dev->of_node, "apple,t6041-dcpext");
	dcp->external_native = dcp->external && dcp_t6030_ext_native(dev->of_node);
	dcp_external_audio_init(dcp);
	dcp->dev = dev;
	/*
	 * Type-C and Thunderbolt routes can be activated as soon as they are
	 * registered below, before the DRM device binds.
	 */
	dcp_modes_init(dcp);
	/* Registered before callback resources, so they drain before the catalog. */
	ret = devm_add_action_or_reset(dev, dcp_modes_release, dcp);
	if (ret)
		return ret;
	mutex_init(&dcp->hpd_mutex);
	mutex_init(&dcp->external_lock);
	INIT_WORK(&dcp->external_stop_work, dcp_external_stop_work);
	INIT_DELAYED_WORK(&dcp->external_stop_retry_wq, dcp_external_stop_retry);
	mutex_init(&dcp->tb_lock);
	dcp_fabric_init(dcp);
	spin_lock_init(&dcp->mode_state.lock);
	spin_lock_init(&dcp->metadata.lock);
	mutex_init(&dcp->dcpavserv.lock);
	INIT_WORK(&dcp->dcpavserv.close_wq, dcpavserv_close_work);
	ret = dcpavserv_edid_retry_init(dcp);
	if (ret)
		return ret;
	dcp->hw = *(struct apple_dcp_hw_data *)of_device_get_match_data(dev);
	/*
	 * T8142 on firmware 27 runs as the T6050's firmware-27 instance, with
	 * the default method profile: with H17G every swap fails with status 4.
	 */
	if (fw_compat == DCP_FIRMWARE_V_27_0 &&
	    of_device_is_compatible(dev->of_node, "apple,t8142-dcp"))
		dcp->hw.iomfb_method_profile = DCP_IOMFB_METHODS_DEFAULT;
	/* The display such a processor finds at boot is its first sink. */
	if (dcp_sink_metadata(dcp))
		dcp_desktop_metadata_new_sink(dcp);
	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL) {
		if (!dcp->hw.firmware_clock ||
		    !apple_dcp_external_bw_valid(resource_size(dcp->hw.firmware_clock),
					 dcp->hw.firmware_scratch,
					 dcp->hw.firmware_request))
			return dev_err_probe(dev, -EINVAL,
					     "Invalid external profile bandwidth resources\n");
		iomfb_pause_presentation(dcp, true);
	}

	/* The domain use must survive driver-core DMA cleanup for a live session. */
	ret = iommu_device_use_default_domain(dev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, dcp_release_dma_domain, dcp);
	if (ret)
		return ret;
	dcp->fixed_connector_type = dcp_connector_type_from_dt(dev->of_node);
	dcp->connector_type = dcp->fixed_connector_type;
	of_property_read_u32(dev->of_node, "apple,dcp-index", &dcp->index);
	of_property_read_u32(dev->of_node, "apple,dcp-endpoint", &dcp->peer_endpoint);
	if (of_property_read_bool(dev->of_node, "apple,typec-hybrid-first"))
		dcp_typec_hybrid_first_dt();
	of_property_read_u32(dev->of_node, "apple,dptx-phy", &dcp->dptx_phy);
	of_property_read_u32(dev->of_node, "apple,dptx-die", &dcp->dptx_die);
	dcp->fixed_dptx_phy = dcp->dptx_phy;
	init_completion(&dcp->typec_iomfb_hpd_ready);
	INIT_DELAYED_WORK(&dcp->typec_reconnect_wq,
			  dcp_typec_reconnect_work);
	INIT_DELAYED_WORK(&dcp->link_recovery_wq, dcp_link_recovery_work);
	mutex_init(&dcp->tb_event_lock);
	INIT_WORK(&dcp->tb_event_work, dcp_tb_event_work);
	INIT_DELAYED_WORK(&dcp->hotplug_retrain_retry_wq,
			  dcp_hotplug_retrain_retry_work);
	INIT_DELAYED_WORK(&dcp->placeholder_edid_wq,
			  dcp_placeholder_edid_work);
	INIT_DELAYED_WORK(&dcp->typec_relink_wq, dcp_typec_relink_work);
	spin_lock_init(&dcp->relink_lock);
	INIT_DELAYED_WORK(&dcp->announce_wq, dcp_announce_work);
	INIT_DELAYED_WORK(&dcp->bl_fallback_wq, dcp_work_backlight_fallback);
	/* System sleep and shutdown cancel these even if no bind succeeded. */
	INIT_WORK(&dcp->vblank_wq, dcp_delayed_vblank);
#ifdef CONFIG_HIBERNATION
	INIT_WORK(&dcp->hib_restore_work, dcp_hib_restore_work);
	/*
	 * From probe: the notifier flushes a restart before the next sleep,
	 * replays a link given back for a hibernation that failed, and is in
	 * the image for the restore.
	 */
	if (fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP) {
		ret = dcp_hib_register(dcp);
		if (ret)
			return dev_err_probe(dev, ret, "no PM notifier\n");
	}
#endif
	INIT_DELAYED_WORK(&dcp->swap_watchdog_wq, dcp_swap_watchdog);
	INIT_WORK(&dcp->s2r_prewarm_work, dcp_s2r_prewarm_work);
	INIT_DELAYED_WORK(&dcp->s2r_prewarm_off_wq, dcp_s2r_prewarm_off_work);
	mutex_init(&dcp->s2r_prewarm_lock);
	INIT_WORK(&dcp->bl_register_wq, dcp_work_register_backlight);
	INIT_WORK(&dcp->bl_update_wq, dcp_work_update_backlight);
	mutex_init(&dcp->bl_register_mutex);
	/* Balanced by enable at successful component bind. */
	disable_delayed_work(&dcp->typec_reconnect_wq);
	disable_delayed_work(&dcp->placeholder_edid_wq);
	disable_delayed_work(&dcp->typec_relink_wq);
	disable_delayed_work(&dcp->announce_wq);
	disable_delayed_work(&dcp->link_recovery_wq);
	disable_work(&dcp->tb_event_work);
	disable_delayed_work(&dcp->hotplug_retrain_retry_wq);
	dcp->announce.stopped = true;
	/*
	 * A native external pipe's bind enables these; firmware callbacks may
	 * come before it. The 26.6 panel never uses them.
	 */
	if (dcp->external_native || fw_compat == DCP_FIRMWARE_V_26_6) {
		disable_work(&dcp->vblank_wq);
		disable_delayed_work(&dcp->swap_watchdog_wq);
	}

	platform_set_drvdata(pdev, dcp);
	ret = dcp_s2r_pm_register(dcp);
	if (ret)
		return ret;

	if (iomfb_is_m4(dcp)) {
		dcp->fixed_connector_type = DRM_MODE_CONNECTOR_eDP;
		dcp->connector_type = DRM_MODE_CONNECTOR_eDP;
		bitmap_zero(dcp->iomfb_surfaces, DCP_MAX_PLANES);
		set_bit(0, dcp->iomfb_surfaces);
		ret = iomfb_m4_init(dcp);
		if (ret)
			return ret;
		/*
		 * An external DCP is no component of the DRM device. Once
		 * adoption starts, its session outlives the device.
		 */
		if (of_device_is_compatible(dev->of_node, "apple,t8132-dcpext")) {
			ret = dcp_pin_live_session(dcp);
			if (!ret)
				ret = iomfb_m4_adopt_external(dcp);
			if (!ret)
				dcp_t8132_ext_routes(dcp);
			return ret;
		}
		return component_add(dev, &dcp_comp_ops);
	}
	if (fw_compat == DCP_FIRMWARE_V_26_6) {
		ret = iomfb_v26_6_probe(dcp);
		if (ret)
			return ret;
	}
	if (fw_compat == DCP_FIRMWARE_V_14_7 && !dcp->external) {
		ret = iomfb_v14_7_probe(dcp);
		if (ret)
			return ret;
	}
	if (dcp->external)
		dcp->hw.num_dptx_ports = 2;

	dcp->phy = devm_phy_optional_get(dev, "dp-phy");
	if (IS_ERR(dcp->phy) && dcp->external_native) {
		ret = dcp_fixed_output_error(dcp, PTR_ERR(dcp->phy), "dp-phy");
		if (ret)
			return ret;
	}
	if (IS_ERR(dcp->phy)) {
		dev_err(dev, "Failed to get dp-phy: %ld\n", PTR_ERR(dcp->phy));
		return PTR_ERR(dcp->phy);
	}
	dcp->fixed_phy = dcp->phy;

	if (fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP) {
		ret = dcp_desktop_probe_routes(dcp);
		if (ret)
			return ret;
	}

	bitmap_zero(dcp->iomfb_surfaces, DCP_MAX_PLANES);
	if (!of_property_present(dev->of_node, "apple,iomfb-surfaces"))
		num_surfs = 0;
	else
		num_surfs = of_property_count_elems_of_size(dev->of_node,
						    "apple,iomfb-surfaces",
						    sizeof(u32));

	if (fw_compat == DCP_FIRMWARE_V_14_7 || fw_compat == DCP_FIRMWARE_V_26_6) {
		set_bit(0, dcp->iomfb_surfaces);
	} else if (num_surfs == 0 || num_surfs == -ENODATA) {
		set_bit(0, dcp->iomfb_surfaces);
		set_bit(1, dcp->iomfb_surfaces);
	} else if (num_surfs < 0) {
		return num_surfs;
	} else if (num_surfs > DCP_MAX_PLANES) {
		dev_err(dev, "Number of iomfb-surfaces (%d) exceeds DCP_MAX_PLANES\n",
			num_surfs);
		return -EINVAL;
	}

	surf = 0;
	of_property_for_each_u32(dev->of_node, "apple,iomfb-surfaces", surf_en) {
		if (surf_en)
			set_bit(surf, dcp->iomfb_surfaces);
		surf++;
	}
	/* The J700 loader names surface zero rather than enabling it. */
	if (of_machine_is_compatible("apple,j700") &&
	    of_device_is_compatible(dev->of_node, "apple,t8140-dcp") &&
	    num_surfs == 1 &&
	    !of_property_read_u32(dev->of_node, "apple,iomfb-surfaces", &surf_en) &&
	    surf_en == 0)
		set_bit(0, dcp->iomfb_surfaces);

	if (dcp->phy) {
		enum gpiod_flags pwren_flags;
		int ret;
		/*
		 * Request DP2HDMI related GPIOs as optional for DP-altmode
		 * compatibility. J180D misses a dp2hdmi-pwren GPIO in the
		 * template ADT. TODO: check device ADT
		 */
		dcp->hdmi_hpd = devm_gpiod_get_optional(dev, "hdmi-hpd", GPIOD_IN);
		if (IS_ERR(dcp->hdmi_hpd) && dcp->external_native) {
			ret = dcp_fixed_output_error(dcp, PTR_ERR(dcp->hdmi_hpd), "hdmi-hpd");
			if (ret)
				return ret;
			goto fixed_done;
		}
		if (IS_ERR(dcp->hdmi_hpd))
			return PTR_ERR(dcp->hdmi_hpd);
		ret = dcp_hdmi_init(dcp);
		if (ret)
			return ret;
		if (dcp->hdmi_hpd) {
			int irq = gpiod_to_irq(dcp->hdmi_hpd);
			if (irq < 0 && dcp->external_native) {
				ret = dcp_fixed_output_error(dcp, irq, "hdmi-hpd interrupt");
				if (ret)
					return ret;
				goto fixed_done;
			}
			if (irq < 0) {
				dev_err(dev, "failed to translate HDMI hpd GPIO to IRQ\n");
				return irq;
			}
			dcp->hdmi_hpd_irq = irq;

			ret = devm_request_threaded_irq(dev, dcp->hdmi_hpd_irq,
						dcp_dp2hdmi_hpd_edge, dcp_dp2hdmi_hpd,
						IRQF_ONESHOT | IRQF_NO_AUTOEN |
						IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						"dp2hdmi-hpd-irq", dcp);
			if (ret < 0 && dcp->external_native) {
				ret = dcp_fixed_output_error(dcp, ret, "hdmi-hpd interrupt");
				if (ret)
					return ret;
				goto fixed_done;
			}
			if (ret < 0) {
				dev_err(dev, "failed to request HDMI hpd irq %d: %d\n",
					irq, ret);
				return ret;
			}
		}

		/*
		 * Power DP2HDMI on as it is required for the HPD irq.
		 * TODO: check if one is sufficient for the hpd to save power
		 *       on battery powered Macbooks.
		 */
		pwren_flags = fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP ?
			      GPIOD_ASIS : GPIOD_OUT_HIGH;
		dcp->hdmi_pwren = devm_gpiod_get_optional(dev, "hdmi-pwren", pwren_flags);
		if (IS_ERR(dcp->hdmi_pwren) && dcp->external_native) {
			ret = dcp_fixed_output_error(dcp, PTR_ERR(dcp->hdmi_pwren), "hdmi-pwren");
			if (ret)
				return ret;
			goto fixed_done;
		}
		if (IS_ERR(dcp->hdmi_pwren))
			return PTR_ERR(dcp->hdmi_pwren);

		dcp->dp2hdmi_pwren = devm_gpiod_get_optional(dev, "dp2hdmi-pwren", pwren_flags);
		if (IS_ERR(dcp->dp2hdmi_pwren) && dcp->external_native) {
			ret = dcp_fixed_output_error(dcp, PTR_ERR(dcp->dp2hdmi_pwren), "dp2hdmi-pwren");
			if (ret)
				return ret;
			goto fixed_done;
		}
		if (IS_ERR(dcp->dp2hdmi_pwren))
			return PTR_ERR(dcp->dp2hdmi_pwren);
		/* Requested as they are, powered here. */
		if (fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP &&
		    (dcp->hdmi_pwren || dcp->dp2hdmi_pwren)) {
			ret = dcp_hdmi_power_up(dcp);
			if (ret && dcp->external_native) {
				ret = dcp_fixed_output_error(dcp, ret, "hdmi power");
				if (ret)
					return ret;
				goto fixed_done;
			}
			if (ret)
				return ret;
		}

		/*
		 * A DCP may have both a fixed HDMI/DP route and allocatable Type-C
		 * routes. Keep the fixed route selected until the allocator borrows
		 * this otherwise-idle pipeline for a Type-C display.
		 */
		ret = dcp->fixed_phy ?
			of_property_read_u32(dev->of_node, "mux-index", &mux_index) :
			-ENODATA;
		if (!ret) {
			dcp->fixed_mux_index = mux_index;
			dcp->xbar = devm_mux_control_get(dev, "dp-xbar");
			if (IS_ERR(dcp->xbar) && dcp->external_native) {
				ret = dcp_fixed_output_error(dcp, PTR_ERR(dcp->xbar), "dp-xbar");
				if (ret)
					return ret;
				goto fixed_done;
			}
			if (IS_ERR(dcp->xbar)) {
				dev_err(dev, "Failed to get dp-xbar: %ld\n", PTR_ERR(dcp->xbar));
				return PTR_ERR(dcp->xbar);
			}
			ret = devm_add_action_or_reset(dev, dcp_release_fixed_route, dcp);
			if (ret)
				return ret;
			dcp->fixed_xbar = dcp->xbar;
			ret = mux_control_select(dcp->fixed_xbar, mux_index);
			if (ret)
				dev_warn(dev, "mux_control_select failed: %d\n", ret);
			else
				dcp->fixed_route_selected = true;

			/*
			 * Switch atcphy to DP-only. should move to a Macbook Pro
			 * 14-/16-inch specific DP-to-HDMI drm_bridge.
			 */
			dcp->typec_mux = fwnode_typec_mux_get(dev_fwnode(dcp->dev));
			if (!IS_ERR_OR_NULL(dcp->typec_mux)) {
				struct typec_altmode alt = {
					.svid = USB_TYPEC_DP_SID,
				};
				struct typec_mux_state state = {
					.alt = &alt,
					.mode = TYPEC_DP_STATE_C,
				};
				int ret = typec_mux_set(dcp->typec_mux, &state);
				dev_info(dev, "typec_mux_set() returned: %d\n", ret);
				if (!ret)
					dcp->phy_managed_by_typec = true;
			} else {
				dev_info(dev, "fwnode_typec_mux_get() returned: %ld\n",
						IS_ERR(dcp->typec_mux) ? PTR_ERR(dcp->typec_mux) : 0);
				dcp->typec_mux = NULL;
			}
		}
	}

fixed_done:
	ret = dcp_register_typec_routes(dcp);
	if (ret) {
		ret = dcp_native_routes_error(dcp, ret);
		if (ret)
			return ret;
		routes_failed = true;
	}

	/*
	 * The manual T6030 external path is not part of the panel's DRM
	 * device. Joining that component set would hold the internal screen
	 * until dcpext binds. Its tunnel bring-up runs the Type-C reconnect
	 * work, which component bind would otherwise enable. A native external
	 * processor joins it, and probe completes for it even when its handoff
	 * is unusable, so that the panel is never held up.
	 */
	if (dcp->external && !dcp->external_native) {
		enable_delayed_work(&dcp->typec_reconnect_wq);
		return iomfb_v14_7_external_start(dcp);
	}

	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL) {
		/* Complete optional startup before the DRM master can depend on it. */
		ret = dcp_comp_bind(dev, NULL, NULL);
		if (ret) {
			dcp_disable_typec_work(dcp, true);
			av_audiosrv_quiesce(dcp);
			return ret;
		}
		WRITE_ONCE(dcp->external_prepared, true);
	}
	if (dcp->external_native) {
		ret = iomfb_v14_7_external_prepare(dcp);
		if (ret) {
			disable_work_sync(&dcp->external_audio_work);
			av_audiosrv_quiesce(dcp);
			return ret;
		}
		if (routes_failed)
			iomfb_v14_7_external_refuse(dcp, "its Type-C display routes did not register");
	}

	ret = component_add(&pdev->dev, &dcp_comp_ops);
	if (ret && dcp->external_prepared) {
		dcp_disable_typec_work(dcp, true);
	}
	/* component_add failure does not unbind. Stop owner work first. */
	if (ret) {
		if (dcp->external_native)
			disable_work_sync(&dcp->external_audio_work);
		av_audiosrv_quiesce(dcp);
	}
	if (ret && dcp->fw_compat == DCP_FIRMWARE_V_26_6)
		iomfb_v26_6_unbind(dcp);
	/* A failed bind run from here may already have started RTKit. */
	if (ret && dcp->fw_compat == DCP_FIRMWARE_V_14_7)
		iomfb_v14_7_remove(dcp);
	return ret;
}

/*
 * Marks a T8122 dcpext unusable, for good: its Type-C routes are disabled in
 * the device tree, so that a USB-C port controller probing later does not
 * wait for them, and a route already registered ignores mode changes (see
 * dcp_typec_route_set()), so that USB on the port goes on.
 */
static void dcp_ext_give_up(struct apple_dcp *dcp, int err, const char *when)
{
	WRITE_ONCE(dcp->ext_dead, true);
	dev_err(dcp->dev, "external display processor unusable (%s failed: %d); the other displays go on without it\n",
		when, err);
	dcp_typec_routes_disable(dcp);
}

/*
 * Whether the DP PHY and display crossbar of every Type-C route are there.
 * Routes register one at a time, and one registered cannot be taken back,
 * since a port controller may hold it: a later route waiting for its
 * supplier would leave the dcpext unusable for the boot. Returns
 * -EPROBE_DEFER until all of them are; other errors are left to the route
 * probe, which reports them.
 */
static int dcp_ext_route_suppliers(struct device *dev)
{
	struct device_node *routes __free(device_node) =
		of_get_child_by_name(dev->of_node, "typec-routes");
	char name[16];
	u32 index;

	if (!routes)
		return 0;
	for_each_available_child_of_node_scoped(routes, route_np) {
		struct mux_control *xbar;
		struct phy *phy;

		if (of_property_read_u32(route_np, "reg", &index))
			continue;
		snprintf(name, sizeof(name), "typec%u", index);
		phy = of_phy_get(dev->of_node, name);
		if (PTR_ERR_OR_ZERO(phy) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		if (!IS_ERR(phy))
			of_phy_put(phy);
		xbar = mux_control_get(dev, name);
		if (PTR_ERR_OR_ZERO(xbar) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		if (!IS_ERR(xbar))
			mux_control_put(xbar);
	}
	return 0;
}

/* -EPROBE_DEFER while the device of available node @np has not bound. */
static int dcp_ext_supplier_bound(struct device_node *np)
{
	struct platform_device *pdev;
	int ret = 0;

	if (!np || !of_device_is_available(np))
		return 0;
	pdev = of_find_device_by_node(np);
	if (!pdev || !device_is_bound(&pdev->dev))
		ret = -EPROBE_DEFER;
	if (pdev)
		put_device(&pdev->dev);
	return ret;
}

/*
 * Whether the suppliers that component bind looks up, and that probe does not
 * otherwise wait for, are there: the PIODMA child's DART (a child node's
 * supplier does not hold up the parent's probe) and the mailbox. Bind runs
 * after the Type-C routes registered, where a deferral can only leave the
 * dcpext unusable for the boot (dcp_comp_bind()). Returns -EPROBE_DEFER until
 * they have bound; a missing one is left to bind, which reports it.
 */
static int dcp_ext_bind_suppliers(struct device *dev)
{
	struct device_node *piodma __free(device_node) =
		of_get_child_by_name(dev->of_node, "piodma");
	struct device_node *dart __free(device_node) =
		piodma ? of_parse_phandle(piodma, "iommus", 0) : NULL;
	struct device_node *mbox __free(device_node) =
		of_parse_phandle(dev->of_node, "mboxes", 0);
	int ret;

	ret = dcp_ext_supplier_bound(dart);
	if (!ret)
		ret = dcp_ext_supplier_bound(mbox);
	return ret;
}

/*
 * A T8132 dcpext's Type-C routes are only enabled for one the boot
 * loader handed over (see the display gate), and its USB-C port controllers
 * wait for them. Wait for the route suppliers before adopting, so that no
 * deferral comes after the adoption, and disable the routes on every
 * failure that leaves them unregistered, so USB on the ports goes on.
 */
static int dcp_platform_probe_t8132_ext(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	ret = dcp_ext_route_suppliers(dev);
	if (!ret)
		ret = dcp_platform_probe_pipe(pdev);
	if (ret == -EPROBE_DEFER) {
		int state = driver_deferred_probe_check_state(dev);

		if (state == -EPROBE_DEFER)
			return ret;
		ret = state;
	}
	if (ret)
		dcp_typec_routes_disable_dev(dev);
	return ret;
}

/*
 * A failsafe dcpext (dcp_ext_is_failsafe()) is a component of the other
 * displays' DRM device, so it must always register one, or they stay on the
 * boot framebuffer. Keep deferring while its suppliers, its routes' and its
 * bind's, may still appear (bounded by the deferred probe timeout); on any
 * other failure join as an unusable pipe.
 */
static int dcp_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_dcp *dcp;
	void *group;
	int ret;

	/* t6041_dcpext=0: stay off the external processors' hardware. */
	if (of_device_is_compatible(dev->of_node, "apple,t6041-dcpext") &&
	    !t6041_dcpext) {
		dev_info(dev, "T6041 external display processor left alone (t6041_dcpext=0)\n");
		return -ENODEV;
	}

	if (of_device_is_compatible(dev->of_node, "apple,t8132-dcpext"))
		return dcp_platform_probe_t8132_ext(pdev);
	if (!dcp_ext_is_failsafe(dev->of_node))
		return dcp_platform_probe_pipe(pdev);

	ret = dcp_ext_route_suppliers(dev);
	if (!ret)
		ret = dcp_ext_bind_suppliers(dev);
	if (ret)
		goto unusable;

	group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	ret = dcp_platform_probe_pipe(pdev);
	if (!ret) {
		devres_remove_group(dev, group);
		return 0;
	}

	dcp = platform_get_drvdata(pdev);
	if (dcp && dcp->nr_typec_routes) {
		/* Routes may be in use already: keep them, and the pipe, as they are. */
		devres_remove_group(dev, group);
		dcp_ext_give_up(dcp, ret, "probe");
		return component_add(dev, &dcp_comp_ops);
	}

	devres_release_group(dev, group);
	platform_set_drvdata(pdev, NULL);
unusable:
	if (ret == -EPROBE_DEFER) {
		ret = driver_deferred_probe_check_state(dev);
		if (ret == -EPROBE_DEFER)
			return ret;
	}

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;
	dcp->dev = dev;
	dcp->ext_failsafe = true;
	platform_set_drvdata(pdev, dcp);
	dcp_ext_give_up(dcp, ret, "probe");
	return component_add(dev, &dcp_comp_ops);
}

static void dcp_platform_remove(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp) {
		cancel_work_sync(&dcp->s2r_prewarm_work);
		cancel_delayed_work_sync(&dcp->s2r_prewarm_off_wq);
	}

	if (dcp && dcp->external && !dcp->external_native) {
		/* Enabled for the external start at probe. No component unbind. */
		disable_delayed_work_sync(&dcp->typec_reconnect_wq);
		av_audiosrv_quiesce(dcp);
		iomfb_v14_7_remove(dcp);
		return;
	}
	if (dcp && dcp->external_native)
		disable_work_sync(&dcp->external_audio_work);
	/*
	 * Finish audio RPCs before component_del's unbind flushes RX, and
	 * before iomfb_v14_7_remove() clears v14->dcp. Unbind repeats the
	 * helper; the second call does not disable the works again.
	 */
	if (dcp)
		av_audiosrv_quiesce(dcp);
	/* An adopted external DCP never joined the DRM device. */
	if (of_device_is_compatible(pdev->dev.of_node, "apple,t8132-dcpext")) {
		/* Enabled with its Type-C routes, see dcp_t8132_ext_routes(). */
		if (dcp)
			disable_delayed_work_sync(&dcp->typec_reconnect_wq);
		return;
	}
	component_del(&pdev->dev, &dcp_comp_ops);
	if (dcp && dcp->fw_compat == DCP_FIRMWARE_V_26_6)
		iomfb_v26_6_unbind(dcp);
	/* Unbind does not wait for RTKit callbacks, or run if bind failed late. */
	if (dcp && dcp->fw_compat == DCP_FIRMWARE_V_14_7)
		iomfb_v14_7_remove(dcp);
}

/* A refused takeover left the DCP running untouched, see apple_drv.c. */
#ifdef CONFIG_HIBERNATION
/*
 * From PM_HIBERNATION_PREPARE to PM_POST_HIBERNATION, whatever
 * /sys/power/disk says (shutdown, platform, reboot, suspend): see
 * dcp_hib_global_notify().  Set in the image as well.
 */
static bool dcp_hib_in_progress;

static bool dcp_hibernating(void)
{
	return READ_ONCE(dcp_hib_in_progress);
}

static int dcp_hib_prepare(struct apple_dcp *dcp);
static void dcp_hib_stop_ext_link_work(struct apple_dcp *dcp);
static void dcp_hib_ext_get(struct apple_dcp *dcp);
static void dcp_hib_ext_put(struct apple_dcp *dcp, bool start, bool idle);
#else
static bool dcp_hibernating(void)
{
	return false;
}
#endif

static bool dcp_takeover_was_aborted(struct apple_dcp *dcp)
{
	return dcp && dcp->desktop && READ_ONCE(dcp->desktop->takeover_aborted);
}

void dcp_takeover_aborted(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp && dcp->desktop)
		WRITE_ONCE(dcp->desktop->takeover_aborted, true);
}

VISIBLE_IF_KUNIT void dcp_platform_shutdown(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp_takeover_was_aborted(dcp))
		return;

	/*
	 * The S4 power-off after the image is on disk: the compositor is
	 * frozen, and the DRM/component teardown below waits on it.  The next
	 * boot restarts the firmware anyway, so only stop host submissions,
	 * as for a live session that has no stop sequence.
	 */
	if (dcp && dcp_hibernating()) {
		pr_emerg("dcp: hibernation power-off, skipping teardown\n");
		iomfb_queue_stop(dcp);
		cancel_delayed_work_sync(&dcp->bl_fallback_wq);
		dcp_backlight_stop(dcp);
		cancel_work_sync(&dcp->bl_register_wq);
		cancel_work_sync(&dcp->bl_update_wq);
		return;
	}

	if (dcp && iomfb_is_m4(dcp)) {
		dcp_comp_unbind(&pdev->dev, NULL, NULL);
		return;
	}
	if (dcp && dcp->external && !dcp->external_native)
		return;

	if (dcp && dcp->retain_dma && dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL) {
		dcp_comp_unbind(&pdev->dev, NULL, NULL);
		return;
	}

	if (dcp && dcp->hw.adopt_live_session && dcp_fail_stop(dcp) &&
	    dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G) {
		/*
		 * This firmware keeps scanning across soft DPMS and has no
		 * known stop sequence. Component unbind releases the RTKit
		 * devres group and the piodma domain while firmware may still
		 * access them. Stop host submissions but keep the component and
		 * its DMA resources alive until the system resets.
		 */
		iomfb_queue_stop(dcp);
		cancel_delayed_work_sync(&dcp->bl_fallback_wq);
		dcp_backlight_stop(dcp);
		cancel_work_sync(&dcp->bl_register_wq);
		cancel_work_sync(&dcp->bl_update_wq);
		return;
	}

	/* An adopted external DCP never joined the DRM device. */
	if (of_device_is_compatible(pdev->dev.of_node, "apple,t8132-dcpext"))
		return;
	component_del(&pdev->dev, &dcp_comp_ops);
}

/*
 * The display stays powered across system sleep, and the blanking present
 * queued by the DRM and backlight suspend handlers must reach the firmware
 * before its mailbox interrupt is suspended.
 */
static void dcp_drain_for_sleep(struct apple_dcp *dcp)
{
	int pass;

	if (!dcp->hw.adopt_live_session || !dcp_presents_nits(dcp))
		return;

	/*
	 * A completed present can schedule one more brightness present, and a
	 * rejected one a delayed retry; run a pending retry now instead of
	 * letting it fire after the mailbox has suspended.  Four passes cover
	 * the first attempt and the policy's three retries.  A fade still
	 * running ends at its target first.
	 */
	if (dcp_has_panel(dcp))
		dcp_backlight_finish_fade(dcp);
	for (pass = 0; pass < 4; pass++) {
		flush_delayed_work(&dcp->iomfb.backlight_retry);
		if (dcp_has_panel(dcp))
			flush_work(&dcp->bl_update_wq);
		if (!iomfb_queue_drain(dcp, msecs_to_jiffies(500))) {
			dev_warn(dcp->dev, "display updates still pending at suspend\n");
			return;
		}
		/*
		 * A crashed or stopped DCP takes no more presents, so a level
		 * still pending there can never be sent and is not waited for.
		 * Checking after the drain, which returns at once for such a
		 * DCP, also covers a crash during the pass.
		 */
		if (READ_ONCE(dcp->crashed) || READ_ONCE(dcp->iomfb.stopped))
			return;
		if (!dcp_backlight_active(dcp) || !dcp_backlight_pending(dcp))
			return;
		/*
		 * The consumer blanks the panel before this runs, so a change
		 * left pending while the panel is not lit is the blank itself,
		 * or a level set while DPMS was off: the firmware will not take
		 * it until the CRTC is active again, and resume restores it on
		 * the enable swap. It need not reach the firmware before the
		 * mailbox suspends, so stop here rather than wait it out.
		 */
		if (!dcp_backlight_lit(dcp))
			return;
	}
	dev_warn(dcp->dev, "brightness change still pending at suspend\n");
}

/*
 * DPMS off leaves the internal H17P pipe scanning out (see dcp_poweroff()),
 * and while it scans out the firmware publishes a property to the AP on every
 * frame.  The mailbox interrupt is not suspended, so each of those messages
 * wakes a CPU out of suspend-to-idle about 60 times a second.  Hold the
 * messages in the mailbox for system sleep and process them at resume.
 * apple-drm is a consumer of this device, so its suspend handler has blanked
 * the panel before this runs and its resume handler restores the CRTC after.
 */
static void dcp_hold_for_sleep(struct apple_dcp *dcp)
{
	/*
	 * A failed component bind leaves no coprocessor session to hold. Firmware that DPMS
	 * stopped sends nothing, and its presentation stays paused until DPMS restarts it.
	 */
	if (IS_ERR_OR_NULL(dcp->rtk) ||
	    !dcp_uses_soft_dpms(dcp) || !dcp_presents_nits(dcp) ||
	    READ_ONCE(dcp->crashed) || READ_ONCE(dcp->s2r_fw_down))
		return;

	iomfb_pause_presentation(dcp, true);
	if (iomfb_queue_settle(dcp, msecs_to_jiffies(500))) {
		iomfb_pause_presentation(dcp, false);
		return;
	}
	apple_rtkit_hold_rx(dcp->rtk);
	dcp->rx_held = true;
}

static void dcp_release_after_sleep(struct apple_dcp *dcp)
{
	if (!dcp->rx_held)
		return;
	dcp->rx_held = false;
	apple_rtkit_resume_rx(dcp->rtk);
	iomfb_pause_presentation(dcp, false);
}

/*
 * Why an external processor must keep the system awake, or NULL. The manual
 * path refuses sleep once its firmware was ever started. A native processor
 * refuses while its firmware starts or holds a display link, and while a
 * display is attached to it at all, lit or not: resume does not bring an
 * attached display back. Idle with nothing attached, its firmware stays
 * powered across s2idle like the panel's (the gate holds its CPU domain at
 * the active floor) and the next attach reconnects through it.
 *
 * A processor that failed (its start, or its session later) never refuses
 * sleep: it drives no display until reboot, attached or not, and its power
 * domains stay on across s2idle as they are, like those of a working one.
 */
static const char *dcp_external_sleep_vetoed(struct apple_dcp *dcp)
{
	const char *why;

	if (!dcp->external_native)
		return (atomic_read(&dcp->external_requested) ||
			(dcp->rtk && apple_rtkit_is_running(dcp->rtk)) ||
			dcpext_scanout_requested(dcp)) ?
		       "external firmware/scanout attempted" : NULL;
	why = iomfb_v14_7_external_busy(dcp);
	if (why)
		return why;
	if (iomfb_v14_7_external_failed(dcp)) {
		if (dcp->v14)
			dev_info(dcp->dev, "external display processor failed earlier: sleeping with it as it is\n");
		return NULL;
	}
	if (READ_ONCE(dcp->active_typec_route) || READ_ONCE(dcp->typec_cable_connected))
		return "a USB-C display is attached";
	if (dcp->hdmi_hpd && gpiod_get_value_cansleep(dcp->hdmi_hpd) > 0)
		return "an HDMI display is attached";
	return NULL;
}

/* dpm_prepare completes for every device before any dpm_suspend callback.
 * Veto early so Thunderbolt cannot tear down a working tunnel first. The
 * same HPD lock serializes this gate against explicit firmware startup.
 */
static int dcp_platform_prepare(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	const char *why;

	/*
	 * J713: a T8132 external DCP refuses system sleep only for what cannot
	 * go down with it (see iomfb_m4_ext_sleep_veto()): prepare runs before
	 * any freezing, so nothing is frozen or suspended yet and the sleep
	 * simply fails. An attached display goes down before the freeze and
	 * comes back after the thaw (t8132_ext_drm.c).
	 */
	if (dcp_is_t8132_ext(dcp)) {
		why = iomfb_m4_ext_sleep_veto(dcp);
		if (why) {
			dev_warn(dev, "refusing system sleep: external display attached (T8132): %s\n",
				 why);
			return -EBUSY;
		}
		return 0;
	}
#ifdef CONFIG_HIBERNATION
	/*
	 * A T6040 S2R takes the power from the DP PHYs as the cold boot of a resume from
	 * disk does: an external processor gives its link back the same way.
	 */
	if (dcp_hibernating() || (dcp_desktop_external(dcp) && apple_sleep_power_loss())) {
		int ret = dcp_hib_prepare(dcp);

		if (ret)
			return ret;
	}
#endif
	if (!dcp->external)
		return 0;
	mutex_lock(&dcp->hpd_mutex);
	why = dcp_external_sleep_vetoed(dcp);
	if (why) {
		mutex_unlock(&dcp->hpd_mutex);
		if (dcp->external_native)
			dev_warn(dev, "refusing system sleep: %s\n", why);
		else
			dev_warn(dev, "external firmware/scanout attempted: refusing PM prepare; reboot required for retained DMA\n");
		return -EBUSY;
	}
	WRITE_ONCE(dcp->external_suspended, true);
	mutex_unlock(&dcp->hpd_mutex);
	return 0;
}

static void dcp_s2r_dark_finish(struct apple_dcp *dcp);

static void dcp_platform_complete(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	dcp_s2r_dark_finish(dcp);
	/* The sleep failed, or wasn't one that stops the firmware: it still times out. */
	if (READ_ONCE(dcp->s2r_wake_lit))
		mod_delayed_work(system_freezable_wq, &dcp->s2r_prewarm_off_wq,
				 msecs_to_jiffies(READ_ONCE(wake_prewarm_ms)));
	if (!dcp->external)
		return;
	mutex_lock(&dcp->hpd_mutex);
	WRITE_ONCE(dcp->external_suspended, false);
	mutex_unlock(&dcp->hpd_mutex);
}

/*
 * Suspend-to-RAM with hw.s2r: the sleep engine waits for the DCP's domain to be off, so
 * the firmware has to stop first. Measured from macOS on T8140, in order:
 *
 *  1. A414(0), A478 x2, A476(1), A478, A412, A473(0).
 *  2. D572(0) from the firmware: the display DART's streams go off.
 *  3. A500, unnamed, then D107s and D575(0) from the firmware.
 *  4. RTKit idle, then the ASC's run bit off.
 *  5. The DCP DART's streams off. That's the coprocessor's own shutdown, apart
 *     from step 2. The sleep driver then lowers the domains.
 *
 * macOS also restarts the firmware 893 ms before the commit, calls A478, A473(0),
 * A115, A500, and stops it again. Why isn't known, and that's not done here.
 *
 * The firmware takes IOP idle only with its secure mailbox answered, see
 * dcp_s2r_answer_secure_port().
 */
#define DCP_S2R_DISP_STREAMS	(BIT(0) | BIT(3) | BIT(4) | BIT(16))
#define DCP_S2R_DCP_STREAMS	(BIT(23) | BIT(24))
#define DCP_S2R_D572_TIMEOUT_MS	1000	/* macOS: about 170 ms after A473(0) */
#define DCP_PS_MIN		GENMASK(19, 16)

static struct device *dcp_s2r_find_dart(struct device_node *np)
{
	struct device_node *dart;
	struct platform_device *pdev;

	dart = of_parse_phandle(np, "iommus", 0);
	if (!dart)
		return NULL;
	pdev = of_find_device_by_node(dart);
	of_node_put(dart);

	return pdev ? &pdev->dev : NULL;
}

static void dcp_s2r_put_darts(struct apple_dcp *dcp)
{
	if (dcp->s2r_dcp_dart)
		put_device(dcp->s2r_dcp_dart);
	if (dcp->s2r_disp_dart)
		put_device(dcp->s2r_disp_dart);
	dcp->s2r_dcp_dart = NULL;
	dcp->s2r_disp_dart = NULL;
}

static void dcp_s2r_find_darts(struct apple_dcp *dcp)
{
	struct device_node *piodma;

	/* A suspend that failed after an earlier lookup still holds those. */
	dcp_s2r_put_darts(dcp);
	dcp->s2r_dcp_dart = dcp_s2r_find_dart(dcp->dev->of_node);
	piodma = of_get_child_by_name(dcp->dev->of_node, "piodma");
	if (piodma) {
		dcp->s2r_disp_dart = dcp_s2r_find_dart(piodma);
		of_node_put(piodma);
	}
}

void dcp_s2r_dart_power(struct apple_dcp *dcp, u32 on)
{
	if (!READ_ONCE(dcp->s2r_active))
		return;

	if (dcp->s2r_disp_dart)
		apple_dart_display_streams(dcp->s2r_disp_dart, DCP_S2R_DISP_STREAMS, on, false);
	complete(&dcp->s2r_dart_done);
}

static bool dcp_s2r_capable(struct apple_dcp *dcp)
{
	return dcp->fw_compat == DCP_FIRMWARE_H17P && dcp->hw.s2r;
}

static void dcp_s2r_cpu_stop(struct apple_dcp *dcp)
{
	/* macOS writes it twice. */
	writel(0, dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	writel(0, dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
}

/* Up from bind, unless its handshake failed then. A bound firmware needs it to take IOP idle. */
static void dcp_s2r_answer_secure_port(struct apple_dcp *dcp)
{
	if (!dcp->secure_answered)
		dcp_secure_port_start(dcp);
}

/* After a refused idle: AP on to both mailboxes, as the GPU and the AOP send it. */
static void dcp_s2r_idle_undo(struct apple_dcp *dcp)
{
	int ret;

	ret = apple_rtkit_ap_power(dcp->rtk, true);
	if (ret)
		dev_warn(dcp->dev, "AP on after the refused idle: %d\n", ret);
	if (!dcp->secure_rtk || !dcp->secure_answered || READ_ONCE(dcp->secure_polling))
		return;
	ret = apple_rtkit_port_on(dcp->secure_rtk, DCP_SECURE_TIMEOUT_MS);
	if (ret)
		dev_warn(dcp->dev, "secure port AP on after the refused idle: %d\n", ret);
	dcp_secure_poll_start(dcp);
}

/*
 * macOS's order up to RTKit idle. On success the firmware is idle and its run bit
 * is still set.
 */
static int dcp_s2r_quiesce(struct apple_dcp *dcp, u32 *status)
{
	ktime_t start = ktime_get();
	/* Off since a refused idle: no second set_power_state(0), whose D572 may never come. */
	bool display_off = dcp->s2r_refused_off;
	bool idle_sent = false;
	s64 power_ms;
	bool idle;
	int ret;

	reinit_completion(&dcp->s2r_dart_done);
	dcp_s2r_find_darts(dcp);
	WRITE_ONCE(dcp->s2r_active, true);

	if (display_off) {
		dev_info(dcp->dev, "display still off from a refused idle\n");
		goto display_is_off;
	}

	/* In band, 3 of 3 on macOS. */
	ret = iomfb_s2r_power_h17p(dcp, false, true, false);
	if (ret) {
		dev_err(dcp->dev, "no reply to set_power_state(0): %d\n", ret);
		goto err;
	}
	power_ms = ktime_ms_delta(ktime_get(), start);

	/* Without D572 the display isn't known to be off, and nothing below can undo this. */
	if (!wait_for_completion_timeout(&dcp->s2r_dart_done,
					 msecs_to_jiffies(DCP_S2R_D572_TIMEOUT_MS))) {
		dev_err(dcp->dev, "no D572 %lld ms after set_power_state(0)\n",
			ktime_ms_delta(ktime_get(), start));
		ret = -ETIMEDOUT;
		goto err;
	}
	dev_dbg(dcp->dev, "display off: set_power_state(0) %lld ms, D572 %lld ms\n", power_ms,
		ktime_ms_delta(ktime_get(), start));
	display_off = true;

display_is_off:
	/* D572 itself is still being answered, and a busy channel can't be sent on. */
	usleep_range(5000, 6000);
	if (read_poll_timeout(iomfb_channels_idle, idle, idle, 1000, 100000, false, dcp))
		dev_warn(dcp->dev, "firmware calls still in flight before A500\n");

	ret = iomfb_s2r_500_h17p(dcp);
	if (ret) {
		dev_err(dcp->dev, "no reply to A500: %d\n", ret);
		goto err;
	}
	dev_dbg(dcp->dev, "A500 replied after %lld ms\n", ktime_ms_delta(ktime_get(), start));

	/* The firmware's callbacks after A500 have to be answered first. */
	usleep_range(5000, 6000);
	if (read_poll_timeout(iomfb_channels_idle, idle, idle, 1000, 100000, false, dcp))
		dev_warn(dcp->dev, "firmware calls still in flight before idle\n");

	idle_sent = true;
	ret = apple_rtkit_idle_with(dcp->rtk, dcp_secure_port_idle, dcp);
	if (ret) {
		/* Answer what it still has queued, its log counters among them, and ask again. */
		dev_warn(dcp->dev, "firmware refused idle after %lld ms: %d, control %#x status %#x: retrying once\n",
			 ktime_ms_delta(ktime_get(), start), ret,
			 readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL),
			 readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS));
		apple_rtkit_poll(dcp->rtk);
		msleep(30);
		ret = apple_rtkit_idle_with(dcp->rtk, dcp_secure_port_idle, dcp);
	}
	if (ret) {
		dev_err(dcp->dev, "firmware did not go idle after %lld ms: %d, control %#x status %#x\n",
			ktime_ms_delta(ktime_get(), start), ret,
			readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL),
			readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS));
		goto err;
	}

	/* Both set once the firmware's last message has been read; macOS waits for them. */
	if (readl_poll_timeout(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS, *status,
			       (*status & (APPLE_DCP_COPROC_CPU_STATUS_IDLE |
					   APPLE_DCP_COPROC_CPU_STATUS_STOPPED)) ==
			       (APPLE_DCP_COPROC_CPU_STATUS_IDLE |
				APPLE_DCP_COPROC_CPU_STATUS_STOPPED), 100, 5000))
		dev_warn(dcp->dev, "idle status not set: %#x\n", *status);
	dev_dbg(dcp->dev, "firmware idle after %lld ms, status %#x\n",
		ktime_ms_delta(ktime_get(), start), *status);

	WRITE_ONCE(dcp->s2r_active, false);
	dcp->s2r_refused_off = false;

	return 0;

err:
	WRITE_ONCE(dcp->s2r_active, false);
	dcp_s2r_put_darts(dcp);
	/*
	 * Refused with the display off: leave the firmware running, never stop it under its
	 * own feet. DPMS on lights the display again on the running firmware.
	 */
	if (display_off) {
		if (idle_sent)
			dcp_s2r_idle_undo(dcp);
		dcp->s2r_refused_off = true;
		dev_warn(dcp->dev, "display left off with the firmware running\n");
		return ret;
	}
	/* The display may be off and the firmware half idle. Nothing here brings it back. */
	WRITE_ONCE(dcp->crashed, true);

	return ret;
}

/* Its power state can only be lowered once this has finished. */
static int dcp_s2r_stop_streams(struct apple_dcp *dcp)
{
	if (!dcp->s2r_dcp_dart)
		return -ENODEV;

	return apple_dart_display_streams(dcp->s2r_dcp_dart, DCP_S2R_DCP_STREAMS, false, true);
}

static int dcp_s2r_stop_firmware(struct apple_dcp *dcp)
{
	u32 status;
	int ret;

	ret = dcp_s2r_quiesce(dcp, &status);
	if (ret)
		return ret;

	apple_rtkit_mailbox_off(dcp->rtk);
	dcp_s2r_cpu_stop(dcp);
	dcp->s2r_stopped = true;
	WRITE_ONCE(dcp->s2r_fw_down, true);
	dev_dbg(dcp->dev, "firmware stopped for sleep: control %#x status %#x\n",
		readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL),
		readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS));

	return dcp_s2r_stop_streams(dcp);
}

/*
 * macOS stops this firmware with one call the size of A500, then about 4 ms later AP
 * then IOP idle and the run bit off twice. Nothing has shown it needing a secure
 * port.
 *
 * An attached display goes first, as macOS's DP port releases its display at system
 * sleep: the pipe off, HPD low and the display released, as DPMS off does, then the
 * crossbar output. The route and the connector stay, and after the wake the link
 * connects again, as macOS replays the port's HPD.
 */
static bool dcp_s2r_ext_capable(struct apple_dcp *dcp)
{
	return dcp->fw_compat == DCP_FIRMWARE_H17P && dcp->hw.s2r_ext;
}

/*
 * Taken as stopped, but a start since raised its domains and failed after the run bit,
 * as one a signal ended. Left running, it refuses every sleep; stopped, it comes back
 * from the sleep's power cycle like any other.
 */
static void dcp_s2r_stop_failed_start(struct apple_dcp *dcp)
{
	if (!dcp->s2r_domains_up ||
	    !(readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL) &
	      APPLE_DCP_COPROC_CPU_CONTROL_RUN))
		return;
	dev_warn(dcp->dev, "a failed start left the firmware running: stopping it for the sleep\n");
	dcp_s2r_cpu_stop(dcp);
}

/*
 * pmc-dcp counts the powered DCPs, as macOS keeps it: 0, 1, 2, 1, 0 across a wake. A DCP
 * counts from when this driver powers it up until it powers it off or a sleep stops it.
 */
static DEFINE_MUTEX(dcp_pmc_lock);
static unsigned int dcp_pmc_count;

static void dcp_pmc_set(struct apple_dcp *dcp, void __iomem *pmc, bool up)
{
	guard(mutex)(&dcp_pmc_lock);
	if (up != dcp->pmc_up) {
		dcp->pmc_up = up;
		if (up)
			dcp_pmc_count++;
		else if (dcp_pmc_count)
			dcp_pmc_count--;
	}
	writel(dcp_pmc_count, pmc);
}

/* The sleep controller lowers the domains and pmc-dcp itself. */
static void dcp_pmc_forget(struct apple_dcp *dcp)
{
	guard(mutex)(&dcp_pmc_lock);
	if (dcp->pmc_up && dcp_pmc_count)
		dcp_pmc_count--;
	dcp->pmc_up = false;
}

static void dcp_s2r_restart_failed_stop(struct apple_dcp *dcp);

/*
 * Crashed firmware takes no stop sequence, and refusing would hold every later sleep off until
 * reboot. Its CPU stops cold, as for a failed start, and the wake starts it again, as macOS
 * recovers a crashed DCP. A system sleep only: DPMS off still refuses it.
 */
static bool dcp_s2r_stops_crashed(struct apple_dcp *dcp)
{
	return READ_ONCE(dcp->crashed) && !IS_ERR_OR_NULL(dcp->rtk) &&
	       pm_suspend_target_state != PM_SUSPEND_ON;
}

static int dcp_s2r_stop_crashed(struct apple_dcp *dcp)
{
	void __iomem *control = dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL;
	int ret;

	dev_warn(dcp->dev, "firmware crashed: stopped cold for the sleep, started again after it\n");
	apple_rtkit_mailbox_off(dcp->rtk);
	writel(readl(control) & ~APPLE_DCP_COPROC_CPU_CONTROL_RUN, control);
	writel(readl(control) & ~APPLE_DCP_COPROC_CPU_CONTROL_RUN, control);
	dcp_s2r_find_darts(dcp);
	dcp->s2r_stopped = true;
	WRITE_ONCE(dcp->s2r_fw_down, true);
	ret = dcp_s2r_stop_streams(dcp);
	if (ret) {
		dcp_s2r_restart_failed_stop(dcp);
		return ret == -ETIMEDOUT ? -EBUSY : ret;
	}
	dcp_pmc_forget(dcp);
	device_set_wakeup_path(dcp->dev);

	return 0;
}

static int dcp_s2r_ext_suspend(struct apple_dcp *dcp)
{
	int ret;

	if (READ_ONCE(dcp->s2r_fw_down)) {
		dcp_s2r_stop_failed_start(dcp);
		dcp_s2r_find_darts(dcp);
		dcp->s2r_stopped = true;
		dcp_typec_route_park(dcp);
		ret = dcp_s2r_stop_streams(dcp);
		if (ret) {
			dcp_typec_route_unpark(dcp);
			return ret == -ETIMEDOUT ? -EBUSY : ret;
		}
		dcp_pmc_forget(dcp);
		device_set_wakeup_path(dcp->dev);
		return 0;
	}

	/* No A500 and no disconnect: the link and its crossbar output just go down. */
	if (dcp_s2r_stops_crashed(dcp)) {
		dcp_disable_typec_work(dcp, false);
		dcp_typec_route_park(dcp);
		return dcp_s2r_stop_crashed(dcp);
	}
	if (READ_ONCE(dcp->crashed) || IS_ERR_OR_NULL(dcp->rtk))
		return -EBUSY;

	if (READ_ONCE(dcp->active_typec_route) || READ_ONCE(dcp->typec_cable_connected))
		dev_dbg(dcp->dev, "a display is attached: released for the sleep, connected again after it\n");

	dcp_disable_typec_work(dcp, false);
	ret = dcp_external_disconnect(dcp, false);
	if (ret) {
		dcp_enable_typec_work(dcp);
		return ret;
	}

	dcp_drain_for_sleep(dcp);
	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);
	iomfb_pause_presentation(dcp, true);
	if (iomfb_queue_settle(dcp, msecs_to_jiffies(500))) {
		iomfb_pause_presentation(dcp, false);
		dcp_enable_typec_work(dcp);
		dev_err(dcp->dev, "display updates still pending, refusing sleep\n");
		return -EBUSY;
	}

	/* The display's link went above: its crossbar output follows, as on macOS. */
	dcp_typec_route_park(dcp);

	ret = iomfb_s2r_500_h17p(dcp);
	if (ret) {
		dev_err(dcp->dev, "no reply to A500: %d\n", ret);
		WRITE_ONCE(dcp->crashed, true);
		dcp_typec_route_unpark(dcp);
		return ret == -ETIMEDOUT ? -EBUSY : ret;
	}
	usleep_range(4000, 5000);

	ret = apple_rtkit_sleep_stop(dcp->rtk, dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	if (ret) {
		dev_err(dcp->dev, "firmware did not go idle: %d, control %#x status %#x\n", ret,
			readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL),
			readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS));
		WRITE_ONCE(dcp->crashed, true);
		dcp_typec_route_unpark(dcp);
		return ret == -ETIMEDOUT ? -EBUSY : ret;
	}

	dcp_s2r_find_darts(dcp);
	dcp->s2r_stopped = true;
	WRITE_ONCE(dcp->s2r_fw_down, true);
	dev_dbg(dcp->dev, "firmware stopped for sleep: control %#x status %#x\n",
		readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL),
		readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS));

	ret = dcp_s2r_stop_streams(dcp);
	if (ret) {
		dcp_s2r_restart_failed_stop(dcp);
		return ret == -ETIMEDOUT ? -EBUSY : ret;
	}

	dcp_pmc_forget(dcp);
	device_set_wakeup_path(dcp->dev);

	return 0;
}

/* A failure before the firmware has stopped leaves it running, and refuses cleanly. */
static int dcp_s2r_suspend(struct apple_dcp *dcp)
{
	bool was_off;
	int ret;

	/* Still stopped from a suspend that never reached the commit. */
	if (READ_ONCE(dcp->s2r_fw_down)) {
		dcp_s2r_stop_failed_start(dcp);
		dcp_s2r_find_darts(dcp);
		dcp->s2r_stopped = true;
		ret = dcp_s2r_stop_streams(dcp);
		if (ret)
			return ret == -ETIMEDOUT ? -EBUSY : ret;
		dcp_pmc_forget(dcp);
		device_set_wakeup_path(dcp->dev);
		return 0;
	}

	if (dcp_s2r_stops_crashed(dcp))
		return dcp_s2r_stop_crashed(dcp);
	if (READ_ONCE(dcp->crashed) || IS_ERR_OR_NULL(dcp->rtk))
		return -EBUSY;

	dcp_s2r_answer_secure_port(dcp);

	dcp_drain_for_sleep(dcp);
	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);

	iomfb_pause_presentation(dcp, true);
	if (iomfb_queue_settle(dcp, msecs_to_jiffies(500))) {
		iomfb_pause_presentation(dcp, false);
		dev_err(dcp->dev, "display updates still pending, refusing sleep\n");
		return -EBUSY;
	}

	/* Presentation stays paused if this fails: see dcp_s2r_quiesce(). */
	was_off = dcp->s2r_refused_off;
	ret = dcp_s2r_stop_firmware(dcp);
	if (ret) {
		if (READ_ONCE(dcp->s2r_fw_down))
			dcp_s2r_restart_failed_stop(dcp);
		return ret == -ETIMEDOUT ? -EBUSY : ret;
	}
	/* Dark since a refused idle, as after DPMS off: it stays so until DPMS on. */
	if (was_off)
		dcp->dpms_powered_off = true;

	dcp_pmc_forget(dcp);
	device_set_wakeup_path(dcp->dev);

	return 0;
}

static void dcp_s2r_ps_lower(void __iomem *reg)
{
	u32 val = readl(reg);

	if (val & BIT(28)) {
		val &= ~(BIT(28) | GENMASK(9, 8));
		writel(val, reg);
	}
	writel((val & ~GENMASK(3, 0)) | GENMASK(9, 8), reg);
}

/* macOS's power-off, measured 3 of 3. The firmware is stopped and its DARTs are off. */
static int dcp_s2r_domains_off(struct apple_dcp *dcp)
{
	struct device_node *np;
	void __iomem *ps = NULL, *pwrgate = NULL, *pmc = NULL;
	u32 val;
	int ret = -ENODEV;

	dcp->s2r_domains_up = false;
	np = of_parse_phandle(dcp->dev->of_node, "apple,display-power", 0);
	if (!np)
		return -ENODEV;
	ps = of_iomap(np, of_property_match_string(np, "reg-names", "ps"));
	pwrgate = of_iomap(np, of_property_match_string(np, "reg-names", "pwrgate"));
	pmc = of_iomap(np, of_property_match_string(np, "reg-names", "pmc-dcp"));
	of_node_put(np);
	if (!ps || !pwrgate || !pmc)
		goto out;

	dcp_pmc_set(dcp, pmc, false);
	val = readl(ps + 0x10000);
	if (val & BIT(28))
		writel(val & ~(BIT(28) | GENMASK(9, 8)), ps + 0x10000);
	writel(3, pwrgate + 0xc8c);
	writel(0, pwrgate + 0xcd0);
	dcp_s2r_ps_lower(ps + 0x10000);
	dcp_s2r_ps_lower(ps + 0x318);
	dcp_s2r_ps_lower(ps + 0x280);

	ret = readl_poll_timeout(ps + 0x10000, val, !FIELD_GET(GENMASK(7, 4), val), 100, 50000);
	if (ret)
		dev_err(dcp->dev, "DISP_CPU did not power off: %#x\n", val);
	dev_dbg(dcp->dev, "display domains off: CPU %#x FE %#x SYS %#x\n", readl(ps + 0x10000),
		readl(ps + 0x318), readl(ps + 0x280));
out:
	if (ps)
		iounmap(ps);
	if (pwrgate)
		iounmap(pwrgate);
	if (pmc)
		iounmap(pmc);

	return ret;
}

/*
 * Exact mirror of the writes in macOS's power-off, measured 3 of 3. DISP_SYS and
 * DISP_FE first, then DISP_CPU with AUTO_ENABLE last.
 */
static int dcp_s2r_domains_on(struct apple_dcp *dcp)
{
	struct device_node *np;
	void __iomem *ps = NULL, *pwrgate = NULL, *pmc = NULL;
	u32 val;
	int ret = -ENODEV;

	np = of_parse_phandle(dcp->dev->of_node, "apple,display-power", 0);
	if (!np)
		return -ENODEV;
	ps = of_iomap(np, of_property_match_string(np, "reg-names", "ps"));
	pwrgate = of_iomap(np, of_property_match_string(np, "reg-names", "pwrgate"));
	pmc = of_iomap(np, of_property_match_string(np, "reg-names", "pmc-dcp"));
	of_node_put(np);
	if (!ps || !pwrgate || !pmc)
		goto out;

	val = readl(ps + 0x280) & ~(GENMASK(9, 8) | GENMASK(3, 0) | BIT(28));
	writel(val | 0xf, ps + 0x280);
	writel(val | 0xf | BIT(28), ps + 0x280);
	val = readl(ps + 0x318) & ~(GENMASK(9, 8) | GENMASK(3, 0));
	writel(val | 0xf, ps + 0x318);
	val = readl(ps + 0x10000) & ~(GENMASK(9, 8) | GENMASK(3, 0) | BIT(28));
	writel(val | 0xf, ps + 0x10000);
	writel(0, pwrgate + 0xc8c);
	writel(1, pwrgate + 0xcd0);
	writel(val | 0xf | BIT(28), ps + 0x10000);
	dcp_pmc_set(dcp, pmc, true);

	ret = readl_poll_timeout(ps + 0x10000, val, FIELD_GET(GENMASK(7, 4), val) == 0xf,
				 100, 50000);
	if (ret)
		dev_err(dcp->dev, "DISP_CPU did not power on: %#x\n", val);
	else
		dcp->s2r_domains_up = true;
out:
	if (ps)
		iounmap(ps);
	if (pwrgate)
		iounmap(pwrgate);
	if (pmc)
		iounmap(pmc);

	return ret;
}

/*
 * DISPEXT0 up as macOS does it after a wake: SYS and FE on, then the CPU with PS_MIN 4 first,
 * since the wake reset it, then on, and pmc-dcp up once it reads on.
 */
static int dcp_s2r_ext_domains_on(struct apple_dcp *dcp)
{
	static const u16 sys_fe[] = { 0x288, 0x320 };
	struct device_node *np;
	void __iomem *ps = NULL, *pmc = NULL;
	u32 val;
	int i, ret = -ENODEV;

	np = of_parse_phandle(dcp->dev->of_node, "apple,display-power", 0);
	if (!np)
		return -ENODEV;
	ps = of_iomap(np, of_property_match_string(np, "reg-names", "ps"));
	pmc = of_iomap(np, of_property_match_string(np, "reg-names", "pmc-dcp"));
	of_node_put(np);
	if (!ps || !pmc)
		goto out;

	for (i = 0; i < ARRAY_SIZE(sys_fe); i++) {
		val = readl(ps + sys_fe[i]) & ~(GENMASK(9, 8) | GENMASK(3, 0) | BIT(28));
		writel(val | 0xf, ps + sys_fe[i]);
		writel(val | 0xf | BIT(28), ps + sys_fe[i]);
	}
	val = readl(ps + 0x328) & ~(GENMASK(9, 8) | GENMASK(3, 0) | BIT(28) | DCP_PS_MIN);
	val |= FIELD_PREP(DCP_PS_MIN, 4);
	writel(val, ps + 0x328);
	writel(val | 0xf, ps + 0x328);
	writel(val | 0xf | BIT(28), ps + 0x328);

	ret = readl_poll_timeout(ps + 0x328, val, FIELD_GET(GENMASK(7, 4), val) == 0xf,
				 100, 50000);
	if (ret) {
		dev_err(dcp->dev, "DISPEXT0_CPU did not power on: %#x\n", val);
	} else {
		dcp_pmc_set(dcp, pmc, true);
		dcp->s2r_domains_up = true;
	}
out:
	if (ps)
		iounmap(ps);
	if (pmc)
		iounmap(pmc);

	return ret;
}

/*
 * A firmware stopped while crashed is about to start over: forget the calls and the
 * transaction the old one left unanswered, and let the new one's messages through. A failed
 * start marks it crashed again.
 */
static void dcp_s2r_forget_crash(struct apple_dcp *dcp)
{
	if (!READ_ONCE(dcp->crashed))
		return;
	iomfb_reset_transport(dcp);
	WRITE_ONCE(dcp->crashed, false);
}

/*
 * DCPEXT back after a real sleep, the same way in every macOS wake measured: its domains,
 * its DART's streams, and a warm restart, with no secure port and no display-on call. After
 * its own power cycle the start is the same, with its display DART lit again.
 */
static int dcp_s2r_ext_start(struct apple_dcp *dcp, bool cycled)
{
	void __iomem *control = dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL;
	int ret;

	dcp_s2r_forget_crash(dcp);
	if (!dcp->s2r_dcp_dart)
		return -ENODEV;
	ret = apple_dart_gapf_ready(dcp->s2r_dcp_dart);
	if (!ret)
		ret = dcp_s2r_ext_domains_on(dcp);
	if (ret)
		return ret;
	/* After the domains, whose writes it would reject, the DART protects them again. */
	apple_dart_display_streams(dcp->s2r_dcp_dart, DCP_S2R_DCP_STREAMS, true, false);
	if (!cycled)
		return apple_rtkit_sleep_return(dcp->rtk, control);

	if (dcp->s2r_disp_dart)
		apple_dart_power_dark(dcp->s2r_disp_dart, false);
	return apple_rtkit_power_cycled_return(dcp->rtk, control);
}

/* DISPEXT0 off in its own order, CPU first. Only the CPU has to power-gate to start again. */
static int dcp_s2r_ext_domains_off(struct apple_dcp *dcp)
{
	static const struct {
		u16 offset;
		u32 first, second;
		bool member;
	} steps[] = {
		{ 0x328, 0xf0400ff, 0xf0403f0 }, { 0x320, 0, 0x23f0, true }, { 0x288, 0, 0x23f0 },
	};
	struct device_node *np;
	void __iomem *ps = NULL, *pmc = NULL;
	int i, ret = -ENODEV;
	u32 val;

	np = of_parse_phandle(dcp->dev->of_node, "apple,display-power", 0);
	if (!np)
		return -ENODEV;
	ps = of_iomap(np, of_property_match_string(np, "reg-names", "ps"));
	pmc = of_iomap(np, of_property_match_string(np, "reg-names", "pmc-dcp"));
	of_node_put(np);
	if (!ps || !pmc)
		goto out;

	/*
	 * Its display DART goes off with DISPEXT0, and every framebuffer is mapped into it: a
	 * map during the hold would reach its registers. Dark until the domains are back.
	 */
	if (dcp->s2r_disp_dart)
		apple_dart_power_dark(dcp->s2r_disp_dart, true);
	dcp->s2r_domains_up = false;
	dcp_pmc_set(dcp, pmc, false);
	ret = 0;
	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		int gated;

		if (steps[i].first)
			writel(steps[i].first, ps + steps[i].offset);
		writel(steps[i].second, ps + steps[i].offset);
		gated = readl_poll_timeout(ps + steps[i].offset, val,
					   !(val & GENMASK(7, 4)) && !(val & BIT(11)) &&
					   (steps[i].member || (val & BIT(8))), 100, 50000);
		dev_dbg(dcp->dev, "DISPEXT0 power word %#x reads %#x: %d\n", steps[i].offset, val,
			gated);
		if (i == 0 && gated)
			ret = gated;
	}
out:
	if (ps)
		iounmap(ps);
	if (pmc)
		iounmap(pmc);

	return ret;
}

#define DCP_S2R_EXT_CYCLE_HOLD_MS	1000

static bool dcp_s2r_ext_cpu_on(struct apple_dcp *dcp)
{
	struct device_node *np;
	void __iomem *ps;
	bool on;

	np = of_parse_phandle(dcp->dev->of_node, "apple,display-power", 0);
	if (!np)
		return false;
	ps = of_iomap(np, of_property_match_string(np, "reg-names", "ps"));
	of_node_put(np);
	if (!ps)
		return false;
	on = FIELD_GET(GENMASK(7, 4), readl(ps + 0x328));
	iounmap(ps);

	return on;
}

/*
 * After a refused sleep DCPEXT has idled with its power kept, and an idled firmware doesn't
 * wake again: power-cycle it through DISPEXT0's own sequence, then start it as after a sleep.
 * The sleep driver lowers DISPEXT0 before refusals of its own, and with it off the CPU and the
 * display DART don't answer, so it's raised first.
 */
static int dcp_s2r_ext_cycle(struct apple_dcp *dcp)
{
	void __iomem *control = dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL;
	int ret;

	if (!dcp->s2r_dcp_dart)
		return -ENODEV;
	ret = apple_dart_gapf_ready(dcp->s2r_dcp_dart);
	if (ret)
		return ret;
	if (!dcp_s2r_ext_cpu_on(dcp)) {
		dev_info(dcp->dev, "DISPEXT0 lowered by the refused sleep: raised for the cycle\n");
		ret = dcp_s2r_ext_domains_on(dcp);
		if (ret)
			return ret;
	}

	writel(readl(control) & ~APPLE_DCP_COPROC_CPU_CONTROL_RUN, control);
	writel(readl(control) & ~APPLE_DCP_COPROC_CPU_CONTROL_RUN, control);
	ret = dcp_s2r_ext_domains_off(dcp);
	if (!ret) {
		msleep(DCP_S2R_EXT_CYCLE_HOLD_MS);
		ret = dcp_s2r_ext_start(dcp, true);
	}

	return ret;
}

/* Between the main mailbox's IOP report and its AP on, measured 2 of 2. */
static void dcp_secure_port_map(void *data)
{
	struct apple_dcp *dcp = data;
	ktime_t start = ktime_get();

	dcp->secure_map_ret = apple_rtkit_port_map(dcp->secure_rtk, DCP_SECURE_TIMEOUT_MS);
	dcp->secure_map_us = ktime_us_delta(ktime_get(), start);
}

static int dcp_s2r_warm_restart(struct apple_dcp *dcp)
{
	int ret;

	dcp_s2r_forget_crash(dcp);
	ret = dcp_s2r_domains_on(dcp);
	if (ret)
		return ret;

	/* After the domains, whose writes it would reject, the DART protects them again. */
	if (dcp->s2r_dcp_dart)
		apple_dart_display_streams(dcp->s2r_dcp_dart, DCP_S2R_DCP_STREAMS, true, false);

	if (dcp->secure_rtk) {
		dcp_secure_poll_stop(dcp);
		ret = apple_rtkit_port_restart(dcp->secure_rtk);
		if (ret)
			return ret;
	}

	ret = apple_rtkit_warm_restart(dcp->rtk);
	if (ret)
		return ret;

	writel(APPLE_DCP_COPROC_CPU_CONTROL_RUN, dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);

	ret = apple_rtkit_boot_with(dcp->rtk, dcp->secure_rtk ? dcp_secure_port_map : NULL, dcp);
	if (ret)
		return ret;

	if (dcp->secure_rtk) {
		ret = dcp->secure_map_ret ?:
		      apple_rtkit_port_on(dcp->secure_rtk, DCP_SECURE_TIMEOUT_MS);
		if (ret)
			dev_warn(dcp->dev, "secure port handshake after restart: %d\n", ret);
		else
			dev_dbg(dcp->dev, "secure port up after restart, map took %lld us\n",
				dcp->secure_map_us);
		dcp_secure_poll_start(dcp);
	}

	return 0;
}

/*
 * Display on after a sleep, and after DPMS off: the warm restart, then the named calls
 * macOS sends first, A414(0), A478 x2, A476(1), A478, A412, then A473(1).
 *
 * Not sent: A439, which macOS overlaps with A473(1), A438 around swaps, A461 per frame
 * and A352. They stay unnamed, and A439 and A438 hold macOS kernel pointers. Linux
 * already lights the panel without A438 at boot.
 */
static int dcp_s2r_display_on(struct apple_dcp *dcp)
{
	int ret;

	ret = dcp_s2r_warm_restart(dcp);
	if (ret) {
		dev_err(dcp->dev, "warm restart failed: %d\n", ret);
		return ret;
	}

	reinit_completion(&dcp->s2r_dart_done);
	WRITE_ONCE(dcp->s2r_active, true);

	/* Out of band; either channel answered on macOS. */
	ret = iomfb_s2r_power_h17p(dcp, true, true, true);
	if (ret) {
		dev_err(dcp->dev, "no reply to set_power_state(1): %d\n", ret);
		goto out;
	}
	if (!wait_for_completion_timeout(&dcp->s2r_dart_done,
					 msecs_to_jiffies(DCP_S2R_D572_TIMEOUT_MS)))
		dev_warn(dcp->dev, "no D572 after set_power_state(1), scanout DART left off\n");
out:
	WRITE_ONCE(dcp->s2r_active, false);

	return ret;
}

/* Power kept: lowered as DPMS off does. A failed flush left the GAPF slice faulting that. */
static int dcp_s2r_refused_domains_off(struct apple_dcp *dcp)
{
	int ret = dcp->s2r_dcp_dart ? apple_dart_gapf_ready(dcp->s2r_dcp_dart) : -ENODEV;

	return ret ?: dcp_s2r_domains_off(dcp);
}

static void dcp_s2r_restart_done(struct apple_dcp *dcp, int ret)
{
	bool ext = dcp_s2r_ext_capable(dcp);

	dcp_s2r_put_darts(dcp);
	/* An attached display's crossbar output; its link reconnects from the Type-C work. */
	if (ext)
		dcp_typec_route_unpark(dcp);
	if (ret) {
		WRITE_ONCE(dcp->crashed, true);
		dev_warn(dcp->dev, "firmware stopped for sleep, display stays off until reboot\n");
		return;
	}
	/* Back from a full restart, even after a failed start had marked it crashed. */
	WRITE_ONCE(dcp->crashed, false);
	WRITE_ONCE(dcp->s2r_fw_down, false);
	iomfb_pause_presentation(dcp, false);
	if (ext && !READ_ONCE(dcp->typec_work_enabled))
		dcp_enable_typec_work(dcp);
}

/*
 * If the display doesn't come back, the panel stays dark until reboot, and the driver
 * must not talk to the stopped firmware, so treat it as a crashed DCP. Nothing else in
 * the system depends on it.
 */
static int dcp_s2r_restart(struct apple_dcp *dcp, bool real)
{
	int ret = -EAGAIN;

	/*
	 * After a refused sleep the firmware has idled with its power kept, and an idled firmware
	 * doesn't wake again: the internal DCP comes back as DPMS on brings it, from a power
	 * cycle, and DCPEXT through its own.
	 */
	if (dcp_s2r_capable(dcp)) {
		ret = real ? 0 : dcp_s2r_refused_domains_off(dcp);
		if (!ret)
			ret = dcp_s2r_display_on(dcp);
		if (!real)
			dev_info(dcp->dev, "sleep refused: firmware restarted as for DPMS: %d\n",
				 ret);
	} else if (dcp_s2r_ext_capable(dcp)) {
		ret = real ? dcp_s2r_ext_start(dcp, false) : dcp_s2r_ext_cycle(dcp);
		if (!real)
			dev_info(dcp->dev, "sleep refused: firmware %s after a power cycle: %d\n",
				 ret ? "not back" : "back", ret);
		else if (ret)
			dev_err(dcp->dev, "warm restart failed: %d\n", ret);
	}
	dcp_s2r_restart_done(dcp, ret);

	return ret;
}

/*
 * A full wake that finds the panel off by DPMS. With wake_light, a lid read closed leaves
 * the display and its firmware off, as macOS keeps the internal display off with the lid
 * closed, and a lid read open lights the panel too. A lid not read only starts the firmware.
 */
static void dcp_s2r_wake_queue(struct apple_dcp *dcp)
{
	int closed = READ_ONCE(wake_light) ? apple_sleep_lid_closed_at_wake() : -ENODATA;

	if (closed > 0) {
		dev_info(dcp->dev, "wake: lid closed, display and firmware stay off\n");
		return;
	}
	dcp->s2r_wake_light = !closed;
	queue_work(system_highpri_wq, &dcp->s2r_prewarm_work);
}

static int dcp_s2r_resume(struct apple_dcp *dcp)
{
	bool real = pm_resume_via_firmware();

	dcp->s2r_stopped = false;
	/* A real sleep took the domains' power. */
	if (real)
		dcp->s2r_domains_up = false;
	/* A dark wake sleeps again with the panel never lit, as if DPMS had turned it off. */
	if (pm_dark_wake() && dcp_s2r_capable(dcp) && !dcp->dpms_powered_off) {
		dcp->dpms_powered_off = true;
		dev_info(dcp->dev, "dark wake: firmware and panel stay off\n");
	}
	/* DCPEXT too, its crossbar output parked: dcp_s2r_dark_finish() if no resume follows. */
	if (pm_dark_wake() && dcp_s2r_ext_capable(dcp)) {
		dcp->s2r_dark_held = true;
		dev_info(dcp->dev, "dark wake: firmware and output stay off\n");
		return 0;
	}
	dcp->s2r_dark_held = false;
	/* Off by DPMS before the sleep: DPMS on, or this wake, brings it back. */
	if (dcp->dpms_powered_off) {
		/* Off after a refused idle, its power may be on: DPMS on needs it lowered. */
		if (!real)
			dcp_s2r_refused_domains_off(dcp);
		dcp_s2r_put_darts(dcp);
		if (real) {
			/* Its last frame is from before the sleep, and may not be what comes next. */
			WRITE_ONCE(dcp->s2r_scanout_stale, true);
			WRITE_ONCE(dcp->s2r_scanout_hold,
				   READ_ONCE(wake_prewarm_ms) && READ_ONCE(wake_light));
		}
		/* Its firmware starts now, alongside the devices still resuming. */
		if (real && !pm_dark_wake() && READ_ONCE(wake_prewarm_ms))
			dcp_s2r_wake_queue(dcp);
		return 0;
	}
	dcp_s2r_restart(dcp, real);

	return 0;
}

/*
 * The suspend after a dark wake failed before suspending DCPEXT, or before preparing it. It's
 * still down from the real sleep, whose wake reset its CPU: start it as after one.
 */
static void dcp_s2r_dark_finish(struct apple_dcp *dcp)
{
	int ret;

	if (!dcp->s2r_dark_held || pm_dark_wake() || IS_ERR_OR_NULL(dcp->rtk))
		return;
	dcp->s2r_dark_held = false;
	dcp->s2r_stopped = false;
	dcp_s2r_find_darts(dcp);
	ret = dcp_s2r_ext_start(dcp, true);
	dev_info(dcp->dev, "dark wake over without a resume: firmware %s: %d\n",
		 ret ? "not back" : "back", ret);
	dcp_s2r_restart_done(dcp, ret);
}

static int dcp_s2r_pm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct apple_dcp *dcp = container_of(nb, struct apple_dcp, s2r_pm_nb);

	if (action == PM_POST_SUSPEND)
		dcp_s2r_dark_finish(dcp);

	return NOTIFY_DONE;
}

static void dcp_s2r_pm_unregister(void *data)
{
	struct apple_dcp *dcp = data;

	unregister_pm_notifier(&dcp->s2r_pm_nb);
}

static int dcp_s2r_pm_register(struct apple_dcp *dcp)
{
	int ret;

	if (!dcp_s2r_ext_capable(dcp))
		return 0;
	dcp->s2r_pm_nb.notifier_call = dcp_s2r_pm_notify;
	ret = register_pm_notifier(&dcp->s2r_pm_nb);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dcp->dev, dcp_s2r_pm_unregister, dcp);
}

/*
 * Its own suspend failed with the CPU stopped, and the PM core won't call .resume: start it
 * again as a refused sleep does, or the display stays dark until a later sleep gets through.
 */
static void dcp_s2r_restart_failed_stop(struct apple_dcp *dcp)
{
	dev_warn(dcp->dev, "stop failed with the CPU stopped: starting the firmware again\n");
	dcp_s2r_restart(dcp, false);
	dcp->s2r_stopped = false;
}

/*
 * DPMS on an internal panel with hw.s2r as macOS does a display sleep: the backlight to 0,
 * then the firmware stopped and the display domains off, and the reverse on wake. Not
 * during a system sleep: that path stops the firmware itself.
 */
static bool dcp_dpms_power_cycles(struct apple_dcp *dcp)
{
	return dcp_s2r_capable(dcp) && pm_suspend_target_state == PM_SUSPEND_ON;
}

static void __dcp_dpms_power_off(struct apple_dcp *dcp)
{
	ktime_t start = ktime_get();
	int ret;

	if (!dcp_dpms_power_cycles(dcp) || READ_ONCE(dcp->s2r_fw_down))
		return;

	ret = dcp_s2r_suspend(dcp);
	if (ret && !READ_ONCE(dcp->s2r_fw_down)) {
		dev_warn(dcp->dev, "DPMS: firmware stop refused, backlight only: %d\n", ret);
		return;
	}
	dcp->s2r_stopped = false;
	if (!ret)
		ret = dcp_s2r_domains_off(dcp);
	dcp_s2r_put_darts(dcp);
	dcp->dpms_powered_off = true;
	dev_dbg(dcp->dev, "DPMS: display powered off in %lld ms: %d\n",
		ktime_ms_delta(ktime_get(), start), ret);
}

/* After a refused idle the firmware still runs with the display off: light it again there. */
static void dcp_s2r_light_running(struct apple_dcp *dcp)
{
	int ret;

	dcp_s2r_find_darts(dcp);
	reinit_completion(&dcp->s2r_dart_done);
	WRITE_ONCE(dcp->s2r_active, true);
	ret = iomfb_s2r_power_h17p(dcp, true, true, true);
	if (!ret && !wait_for_completion_timeout(&dcp->s2r_dart_done,
						 msecs_to_jiffies(DCP_S2R_D572_TIMEOUT_MS)))
		dev_warn(dcp->dev, "no D572 after set_power_state(1), scanout DART left off\n");
	WRITE_ONCE(dcp->s2r_active, false);
	dcp_s2r_put_darts(dcp);
	if (ret) {
		WRITE_ONCE(dcp->crashed, true);
		dev_err(dcp->dev, "display not back on the running firmware: %d\n", ret);
		return;
	}
	iomfb_pause_presentation(dcp, false);
	dev_dbg(dcp->dev, "display back on the running firmware\n");
}

static void dcp_dpms_power_on(struct apple_dcp *dcp)
{
	ktime_t start = ktime_get();
	int ret;

	/* A firmware start from the resume finishes first; then only the panel is left. */
	flush_work(&dcp->s2r_prewarm_work);
	cancel_delayed_work_sync(&dcp->s2r_prewarm_off_wq);
	guard(mutex)(&dcp->s2r_prewarm_lock);
	/* Lit by the resume: only the mode set and the first frame are left, in the commit. */
	if (dcp->s2r_wake_lit) {
		dcp->s2r_wake_lit = false;
		dev_info(dcp->dev, "DPMS on: panel already on, %lld ms after the resume lit it\n",
			 ktime_ms_delta(start, dcp->s2r_wake_lit_at));
		return;
	}
	if (dcp->s2r_prewarmed) {
		dcp->s2r_prewarmed = false;
		dcp->dpms_powered_off = false;
		dcp->s2r_refused_off = false;
		dcp_s2r_light_running(dcp);
		dev_info(dcp->dev, "DPMS on: panel lit on the firmware the resume started, %lld ms\n",
			 ktime_ms_delta(ktime_get(), start));
		return;
	}
	if (dcp->s2r_refused_off) {
		dcp->s2r_refused_off = false;
		dcp_s2r_light_running(dcp);
		return;
	}
	if (!dcp->dpms_powered_off)
		return;
	dcp->dpms_powered_off = false;
	if (!READ_ONCE(dcp->s2r_fw_down))
		return;

	dcp_s2r_find_darts(dcp);
	ret = dcp_s2r_display_on(dcp);
	dcp_s2r_put_darts(dcp);
	if (ret) {
		WRITE_ONCE(dcp->crashed, true);
		dev_err(dcp->dev, "DPMS: display power-on failed, it stays off: %d\n", ret);
		return;
	}
	WRITE_ONCE(dcp->s2r_fw_down, false);
	iomfb_pause_presentation(dcp, false);
	dev_dbg(dcp->dev, "DPMS: display powered on in %lld ms\n",
		ktime_ms_delta(ktime_get(), start));
}

/*
 * From the resume: the firmware up with the panel dark, the state a refused idle leaves,
 * which DPMS on, DPMS off and the next sleep all handle. macOS has DCP's HELLO 83 ms into
 * a wake and its first display call about 10 ms later. With the lid open and wake_light,
 * the panel then powers up too, still dark, as DPMS on would power it.
 */
static void dcp_s2r_prewarm_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(work, struct apple_dcp, s2r_prewarm_work);
	ktime_t start = ktime_get();
	bool light;
	s64 fw_ms;
	int ret;

	guard(mutex)(&dcp->s2r_prewarm_lock);
	light = dcp->s2r_wake_light;
	dcp->s2r_wake_light = false;
	if (!dcp->dpms_powered_off || !READ_ONCE(dcp->s2r_fw_down) || READ_ONCE(dcp->crashed))
		return;
	dcp_s2r_find_darts(dcp);
	ret = dcp_s2r_warm_restart(dcp);
	if (ret) {
		/* Back as DPMS off left it, stopped and powered down: DPMS on starts it again. */
		dcp_s2r_stop_failed_start(dcp);
		dcp_s2r_stop_streams(dcp);
		dcp_s2r_domains_off(dcp);
		dcp_s2r_put_darts(dcp);
		dev_warn(dcp->dev, "wake: firmware start failed, DPMS on tries again: %d\n", ret);
		return;
	}
	dcp_s2r_put_darts(dcp);
	WRITE_ONCE(dcp->s2r_fw_down, false);
	dcp->s2r_refused_off = true;
	if (!light) {
		dcp->s2r_prewarmed = true;
		mod_delayed_work(system_freezable_wq, &dcp->s2r_prewarm_off_wq,
				 msecs_to_jiffies(READ_ONCE(wake_prewarm_ms)));
		dev_info(dcp->dev, "wake: firmware up in %lld ms, panel off until DPMS on\n",
			 ktime_ms_delta(ktime_get(), start));
		return;
	}

	/*
	 * Then what DPMS on would do: A414(0), A478 x2, A476(1), A478, A412, A473(1) and D572,
	 * as macOS's power path sends them 10 ms after DCP's HELLO. The backlight stays at its
	 * DPMS-off level, 0, until DPMS on: the compositor's first frame brings the old level.
	 */
	fw_ms = ktime_ms_delta(ktime_get(), start);
	dcp->s2r_refused_off = false;
	dcp->dpms_powered_off = false;
	dcp_s2r_light_running(dcp);
	if (READ_ONCE(dcp->crashed))
		return;
	dcp->s2r_wake_lit = true;
	dcp->s2r_wake_lit_at = ktime_get();
	mod_delayed_work(system_freezable_wq, &dcp->s2r_prewarm_off_wq,
			 msecs_to_jiffies(READ_ONCE(wake_prewarm_ms)));
	dev_info(dcp->dev, "wake: panel on in %lld ms (firmware %lld ms), dark until the first frame\n",
		 ktime_ms_delta(dcp->s2r_wake_lit_at, start), fw_ms);
}

/* DPMS stayed off after the wake, with the lid closed say: stopped again as DPMS off does. */
static void dcp_s2r_prewarm_off_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work), struct apple_dcp,
					     s2r_prewarm_off_wq);

	guard(mutex)(&dcp->s2r_prewarm_lock);
	/* Lit and never shown: off as DPMS off does it, set_power_state(0) first. */
	if (dcp->s2r_wake_lit) {
		dcp->s2r_wake_lit = false;
		dev_info(dcp->dev, "wake: DPMS stayed off, panel and firmware off again\n");
		__dcp_dpms_power_off(dcp);
		return;
	}
	if (!dcp->s2r_prewarmed)
		return;
	dcp->s2r_prewarmed = false;
	dev_info(dcp->dev, "wake: DPMS stayed off, display firmware stopped again\n");
	__dcp_dpms_power_off(dcp);
}

static void dcp_dpms_power_off(struct apple_dcp *dcp)
{
	flush_work(&dcp->s2r_prewarm_work);
	cancel_delayed_work_sync(&dcp->s2r_prewarm_off_wq);
	guard(mutex)(&dcp->s2r_prewarm_lock);
	dcp->s2r_prewarmed = false;
	dcp->s2r_wake_lit = false;
	__dcp_dpms_power_off(dcp);
}

bool dcp_s2r_firmware_stopped(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return READ_ONCE(dcp->s2r_fw_down);
}

/*
 * The display power domains across a T6040 S2R, the raw-MMIO fallback
 * adapted from the T8140 dcp_s2r_ext_domains_on/off. The bases come from an
 * apple,display-power node (reg-names "ps" + "pmc-dcp") the boot object would
 * publish; our static tree has none, so dcp_t6041_display_power() returns
 * -ENODEV, the entry lower is skipped (the commit's axi2af readcheck logs the
 * pipe still on) and the resume keeps the panel off -- both refusal-safe, so
 * this stays a sleep blocker until the window (or the preferred apple-pmgr-pwrstate
 * genpd extension for conditional-retention + the axi2af bridge) lands and an
 * S2R on hardware proves the sequence.
 *
 * The internal panel's (402e00000.dcp) display power-state registers, in the
 * SOC-East "ps" window aura publishes (apple,display-power, base 0x502280000):
 * DISP_SYS +0x258, DISP_FE +0x368, DISP_CPU +0x10000 (= pmgr_disp 0x502290000,
 * ps_disp_cpu@0). Confirmed against this tree's t6040-pmgr.dtsi power-controller
 * nodes: ps_disp_sys@258, ps_disp_fe@368, &pmgr_disp/ps_disp_cpu@0, versus
 * ps_dispext0_{sys@340,fe@348,cpu@350} (the old DISPEXT0 placeholders).
 * TODO(ADT): the conditional-retention disable/enable values are still from the
 * ADT, not handled here (refusal-safe without them for a first panel test).
 */
#define DCP_T6041_PS_SYS	0x258
#define DCP_T6041_PS_FE		0x368
#define DCP_T6041_PS_CPU	0x10000

/*
 * The two external DCPs share the DISPEXT power-state words in the west SOC-PS
 * block (the "ps" window aura publishes, base 0x502800000), each at its own
 * offsets: DISPEXT0 (HDMI, coproc dcp@3a2e00000) SYS/FE/CPU +0x340/0x348/0x350,
 * DISPEXT1 (USB-C DP, coproc dcp@602e00000) +0x490/0x498/0x4a0. From the
 * ADT's /arm-io/pmgr power-state devices (group 0) and this tree's
 * t6040-pmgr.dtsi ps_dispext{0,1}_* nodes, cross-checked by the sleep
 * controller's apple,check-ps-b4-sleep. The coproc base is the authoritative
 * per-DISPEXT discriminator (reg[0] = reg-name "coproc").
 */
#define DCP_T6041_DISPEXT0_COPROC	0x3a2e00000ULL	/* dcp@3a2e00000, HDMI */
#define DCP_T6041_DISPEXT1_COPROC	0x602e00000ULL	/* dcp@602e00000, USB-C DP */

struct dcp_t6041_ps_offsets {
	u32 sys, fe, cpu;	/* u32: the internal CPU (0x10000) overflows u16 */
};

/*
 * Resolve this DCP's SYS/FE/CPU offsets within its "ps" window. The internal
 * panel uses the SOC-East offsets; an external DCP uses its DISPEXT offsets,
 * picked by its coproc reg base. Returns false (and the caller skips the S2R
 * domain lower/raise, leaving the output off until reboot) for an external DCP
 * whose base is unknown -- refusal-safe, never a write at a guessed address.
 */
static bool dcp_t6041_ps_offsets(struct apple_dcp *dcp,
				 struct dcp_t6041_ps_offsets *o)
{
	struct resource *res;

	if (!dcp_desktop_external(dcp)) {
		o->sys = DCP_T6041_PS_SYS;
		o->fe = DCP_T6041_PS_FE;
		o->cpu = DCP_T6041_PS_CPU;
		return true;
	}
	res = platform_get_resource_byname(to_platform_device(dcp->dev),
					   IORESOURCE_MEM, "coproc");
	if (!res)
		return false;
	switch (res->start) {
	case DCP_T6041_DISPEXT0_COPROC:
		o->sys = 0x340;
		o->fe = 0x348;
		o->cpu = 0x350;
		return true;
	case DCP_T6041_DISPEXT1_COPROC:
		o->sys = 0x490;
		o->fe = 0x498;
		o->cpu = 0x4a0;
		return true;
	default:
		dev_warn_once(dcp->dev,
			      "DCPEXT coproc %pa has no known S2R power offsets: left off across sleep\n",
			      &res->start);
		return false;
	}
}

static int dcp_t6041_display_power(struct apple_dcp *dcp, void __iomem **ps,
				   void __iomem **pmc)
{
	struct device_node *np = of_parse_phandle(dcp->dev->of_node, "apple,display-power", 0);

	*ps = NULL;
	*pmc = NULL;
	if (!np)
		return -ENODEV;
	*ps = of_iomap(np, of_property_match_string(np, "reg-names", "ps"));
	*pmc = of_iomap(np, of_property_match_string(np, "reg-names", "pmc-dcp"));
	of_node_put(np);
	if (!*ps || !*pmc) {
		if (*ps)
			iounmap(*ps);
		if (*pmc)
			iounmap(*pmc);
		*ps = NULL;
		*pmc = NULL;
		return -ENODEV;
	}
	return 0;
}

/* W1: raise the display domains (SYS+FE on, CPU to PS_MIN then on) + pmc-dcp up. */
/*
 * After a real S2R the resume took an SError right after this raised SYS/FE with
 * automatic mode: with the DCP's CPU off nothing holds them up, AUTO gates them again (as
 * APCIE_SYS_GP did) and the display DART behind them faults. N: manual target 0xf,
 * ACTUAL polled, until the firmware runs again.
 */
#define DCP_S2R_MARK(id, step)	(0x7d000000 | ((step) & 0xf) << 16 | (id))
static void dcp_t6041_gapf_release_all(struct apple_dcp *dcp);
static bool s2r_gapf_all;

/*
 * The resume got past the domains and the display DART (mark 0x7d09) and took the
 * SError when the cold restart touched the DCP's ASC: s2r_cpu_off left the CPU word at target
 * 0 with automatic mode off, so nothing brings it back (the "automatic mode raises it" note
 * below predates s2r_cpu_off). s2r_cpu_on: after SYS/FE, release the GAPF clock protection as
 * the stop did and put the CPU word at manual target 0xf, ACTUAL and bit 11 polled; marks
 * 0x7d0a before, 0x7d0b after.
 */
static bool s2r_children_on = true;
module_param(s2r_children_on, bool, 0644);
MODULE_PARM_DESC(s2r_children_on, "T6040 S2R resume: the internal DCP CPU word's 11 children back as the stop found them");

/*
 * T6040 S2R wake (bring-up): the internal DCP's firmware, idled by the stop
 * (A500, RTKit idle), restarted warm as the platform restarts it (HELLO, the endpoint
 * map echoed, no endpoint started again) instead of the cold session restart.
 */
static bool s2r_warm = true;
module_param(s2r_warm, bool, 0644);
MODULE_PARM_DESC(s2r_warm, "T6040 S2R resume: warm RTKit restart of the idled internal DCP, not the cold session restart");

static bool s2r_dcp_dart_on = true;
module_param(s2r_dcp_dart_on, bool, 0644);
MODULE_PARM_DESC(s2r_dcp_dart_on, "T6040 S2R resume: the internal DCP's own DART streams back on before the cold restart");

static bool s2r_cpu_on = true;
module_param(s2r_cpu_on, bool, 0644);
MODULE_PARM_DESC(s2r_cpu_on, "T6040 S2R resume: raise the internal DCP's CPU power word that s2r_cpu_off lowered");

static bool s2r_domains_auto;
module_param(s2r_domains_auto, bool, 0644);
MODULE_PARM_DESC(s2r_domains_auto,
		 "T6040 S2R resume: raise the internal display's SYS/FE in automatic mode (N: manual, ACTUAL polled)");

static int dcp_t6041_s2r_domains_on(struct apple_dcp *dcp)
{
	struct dcp_t6041_ps_offsets o;
	void __iomem *ps, *pmc;
	bool autom = READ_ONCE(s2r_domains_auto);
	u32 val;
	int ret;

	if (!dcp_t6041_ps_offsets(dcp, &o))
		return -ENODEV;
	ret = dcp_t6041_display_power(dcp, &ps, &pmc);
	if (ret)
		return ret;

	/* TODO(ADT): conditional-retention enable + axi2af bridge bit0 set first. */
	val = readl(ps + o.sys) & ~(GENMASK(9, 8) | GENMASK(3, 0) | BIT(28));
	writel(val | 0xf, ps + o.sys);
	if (autom)
		writel(val | 0xf | BIT(28), ps + o.sys);
	else if (readl_poll_timeout_atomic(ps + o.sys, val, (val & GENMASK(7, 4)) == GENMASK(7, 4),
					   1, 10000))
		dev_warn(dcp->dev, "S2R resume: SYS power word stuck at %#x\n", val);
	val = readl(ps + o.fe) & ~(GENMASK(9, 8) | GENMASK(3, 0) | BIT(28));
	writel(val | 0xf, ps + o.fe);
	if (autom)
		writel(val | 0xf | BIT(28), ps + o.fe);
	else if (readl_poll_timeout_atomic(ps + o.fe, val, (val & GENMASK(7, 4)) == GENMASK(7, 4),
					   1, 10000))
		dev_warn(dcp->dev, "S2R resume: FE power word stuck at %#x\n", val);
	apple_sleep_bringup_mark(DCP_S2R_MARK(0, 0x8));
	/*
	 * The CPU's word refuses writes from the OS on T6040 (a no-op rewrite reset the
	 * SoC): it stays in automatic mode and comes up when the run bit is set.
	 */
	ret = 0;
	if (READ_ONCE(s2r_cpu_on) && !dcp->external && !(readl(ps + o.cpu) & 0xf0)) {
		void __iomem *g = ioremap_np(0x500578540ULL, 4);
		u32 v, cur;

		if (g) {
			writel(readl(g) & ~BIT(4), g);
			iounmap(g);
		}
		if (READ_ONCE(s2r_gapf_all))
			dcp_t6041_gapf_release_all(dcp);
		apple_sleep_bringup_mark(DCP_S2R_MARK(0, 0xa));
		v = readl(ps + o.cpu);
		writel((v & ~(0x30fU | BIT(28))) | 0xf, ps + o.cpu);
		mb();
		if (readl_poll_timeout_atomic(ps + o.cpu, cur, (cur & 0xf0) == 0xf0, 1, 8000) ||
		    readl_poll_timeout_atomic(ps + o.cpu, cur, !(cur & BIT(11)), 1, 8000))
			ret = -ETIMEDOUT;
		apple_sleep_bringup_mark(DCP_S2R_MARK(0, 0xb));
		dev_info(dcp->dev, "S2R resume: CPU power word %#x -> %#x%s\n", v,
			 readl(ps + o.cpu), ret ? " (timeout)" : "");
	}
	/*
	 * With the CPU back and the firmware restarted, still every endpoint start timed out:
	 * the 11 children of the CPU word (+0x8..+0x58) stayed at target 0 (t6040-sleep's .enter
	 * lowered them, nothing raised them). s2r_children_on writes back what the stop found,
	 * in order, before the GAPF protection returns; marks 0x7d0d before, 0x7d0e after.
	 */
	if (READ_ONCE(s2r_children_on) && !dcp->external && dcp->s2r_child_valid) {
		u32 cur, to = 0, i;

		apple_sleep_bringup_mark(DCP_S2R_MARK(0, 0xd));
		for (i = 0; i < ARRAY_SIZE(dcp->s2r_child_saved); i++) {
			void __iomem *cw = ps + o.cpu + 8 * (i + 1);
			u32 want = dcp->s2r_child_saved[i];

			if (!(want & 0xf) || (readl(cw) & 0xf0))
				continue;
			writel(want, cw);
			if (readl_poll_timeout_atomic(cw, cur, cur & 0xf0, 1, 2000))
				to |= BIT(i);
		}
		dcp->s2r_child_valid = false;
		apple_sleep_bringup_mark(DCP_S2R_MARK(0, 0xe));
		dev_info(dcp->dev, "S2R resume: CPU children back, saved %#x..%#x, not on %#x\n",
			 dcp->s2r_child_saved[0], dcp->s2r_child_saved[10], to);
	}
	dcp_pmc_set(dcp, pmc, true);	/* shared voter: counts every powered DCP */

	iounmap(ps);
	iounmap(pmc);
	return ret;
}

/* Entry: lower the display domains after the firmware stop, CPU first. */
/*
 * S2R bring-up marks (0x7D0SIIII): S the step of dcp_t6041_s2r_stop() (8/9: the resume's
 * domains on / display DART streams back), I which DCP (0 internal, 1 DCPEXT0/HDMI,
 * 2 DCPEXT1/USB-C DP, f unknown); DCP_S2R_MARK is defined above dcp_t6041_s2r_domains_on().
 */
static bool s2r_ext_domains_off;
module_param(s2r_ext_domains_off, bool, 0644);
MODULE_PARM_DESC(s2r_ext_domains_off,
		 "T6040 S2R: also lower an external DCP's domains after its stop (not measured, off)");
/*
 * The internal panel's off recipe panicked on T6040 at its first word, DISP_CPU: until the
 * display domains are lowered where nothing touches them any more, leave them on here.
 */
static bool s2r_int_domains_off;
module_param(s2r_int_domains_off, bool, 0644);
MODULE_PARM_DESC(s2r_int_domains_off,
		 "T6040 S2R: lower the internal DCP's domains after its stop (panicked on HW, off)");

/* The internal pipe off by its firmware before the T6040 S2R stop (the display sleep). */
static bool s2r_pipe_off = true;
module_param(s2r_pipe_off, bool, 0644);
MODULE_PARM_DESC(s2r_pipe_off, "T6040 S2R: power the internal pipe off through its firmware first");

/*
 * Keep the run bit through the T6040 S2R stop: the CPU's power word can't be written from
 * the OS, so the CPU has to idle in WFI for the PMGR's automatic mode to gate it.
 */
/*
 * T6040 S2R: the CPU power word down in the two steps right after the stop, while
 * its FE and SYS parents are still on. Linux's genpd takes a DCPEXT pipe's FE/SYS down at
 * suspend_noirq and leaves the CPU word (no Linux domain), which then refuses any access.
 * The platform lowers DISP_CPU 131 us after it stops the CPU.
 */
static bool s2r_cpu_off = true;
module_param(s2r_cpu_off, bool, 0644);
MODULE_PARM_DESC(s2r_cpu_off,
		 "T6040 S2R: lower the DCP's CPU power word right after its stop (needs s2r_keep_run=0)");

/*
 * T6040: the fabric rejects a write to a display coprocessor's CPU power word while
 * bit 4 ("clock protection") of its DART's GAPF slice is set (T8140's
 * apple_dart_gapf_ready, b769c9543b56). It is to be cleared when the CPU stops, before
 * the domain goes off; nothing clears it here. Slices from the live ADT:
 * dart-dcp 17, dart-dcpext0 29, dart-dcpext1 48 at GAPF 0x500578000 + 0x100 + 0x40 * n.
 */
#define DCP_T6041_GAPF_DCP	0x500578540ULL
#define DCP_T6041_GAPF_DCPEXT0	0x500578840ULL
#define DCP_T6041_GAPF_DCPEXT1	0x500578d00ULL
#define DCP_T6041_GAPF_PROTECT	BIT(4)
#define DCP_T6041_GAPF_BASE	0x500578000ULL
#define DCP_T6041_GAPF_SIZE	0x4000
#define DCP_T6041_GAPF_RSVD	3	/* /chosen dart-clock-gapf-rsvd-slices */

/*
 * With the DCP DART's own slice released (0x12 -> 0x02) the CPU word still
 * faulted. Discriminator: release every protected slice (bits 1 and 4 set) in the
 * bank, past the reserved ones, so a remaining fault rules GAPF out entirely.
 */
static bool s2r_gapf_all = true;	/* also declared above dcp_t6041_s2r_domains_on() */
module_param(s2r_gapf_all, bool, 0644);
MODULE_PARM_DESC(s2r_gapf_all,
		 "T6040 S2R: with s2r_gapf_release, clear clock protection on every protected GAPF slice");

/*
 * The platform writes the CPU word as full values with bits 27:24 = 0xf (measured: 0x1f0003ff
 * -> 0x0f0000ff -> 0x0f0003f0); this boot's words read 0x1000030f (bits 27:24 = 0),
 * set by the boot chain. Y: write the bits 27:24 instead of keeping ours.
 */
static bool s2r_ps_auto_f = true;
module_param(s2r_ps_auto_f, bool, 0644);
MODULE_PARM_DESC(s2r_ps_auto_f,
		 "T6040 S2R: write the CPU power word with bits 27:24 = 0xf, as the values");

/* Kept by an SError after it: 0x7de2RRNN, RR slices released, NN the bank's slice count. */
static void dcp_t6041_gapf_release_all(struct apple_dcp *dcp)
{
	void __iomem *b = ioremap_np(DCP_T6041_GAPF_BASE, DCP_T6041_GAPF_SIZE);
	u32 n, i, v, rel = 0;

	if (!b)
		return;
	n = readl(b) & 0x1ff;
	if (readl(b + 0x20) != 1 || readl(b + 0x24)) {
		dev_warn(dcp->dev, "S2R: GAPF cfg %#x lock %#x, not touched\n",
			 readl(b + 0x20), readl(b + 0x24));
		iounmap(b);
		return;
	}
	for (i = DCP_T6041_GAPF_RSVD; i < n && 0x100 + 0x40 * (i + 1) <= DCP_T6041_GAPF_SIZE; i++) {
		v = readl(b + 0x100 + 0x40 * i);
		if ((v & 0x12) != 0x12)
			continue;
		writel(v & ~DCP_T6041_GAPF_PROTECT, b + 0x100 + 0x40 * i);
		rel++;
	}
	apple_sleep_bringup_mark(0x7de20000 | min(rel, 0xffU) << 8 | min(n, 0xffU));
	dev_info(dcp->dev, "S2R: GAPF clock protection cleared on %u of %u slices\n", rel, n);
	iounmap(b);
}

static bool s2r_cpu_write = true;
module_param(s2r_cpu_write, bool, 0644);
MODULE_PARM_DESC(s2r_cpu_write,
		 "T6040 S2R: with s2r_cpu_off, also write the CPU power word here (N: DART + GAPF only, the word left to t6040-sleep's .enter)");

static bool s2r_cold_stop = true;
module_param(s2r_cold_stop, bool, 0644);
MODULE_PARM_DESC(s2r_cold_stop,
		 "T6040 S2R: CPU_CONTROL := 0 twice right before the CPU word, as T8140");

static bool s2r_dart_quiesce = true;
module_param(s2r_dart_quiesce, bool, 0644);
MODULE_PARM_DESC(s2r_dart_quiesce,
		 "T6040 S2R: stop the DCP DART's streams and flush its TLB (T8140's order) before the GAPF release");

static bool s2r_gapf_release = true;
module_param(s2r_gapf_release, bool, 0644);
MODULE_PARM_DESC(s2r_gapf_release,
		 "T6040 S2R: clear the DCP DART's GAPF clock protection before its CPU power word goes down");

static bool s2r_keep_run;
module_param(s2r_keep_run, bool, 0644);
MODULE_PARM_DESC(s2r_keep_run,
		 "T6040 S2R: leave the idle DCP CPU running (gated by the PMGR) instead of stopping it");

static u32 dcp_s2r_mark_id(struct apple_dcp *dcp)
{
	struct resource *res;

	if (!dcp->external)
		return 0;
	res = platform_get_resource_byname(to_platform_device(dcp->dev), IORESOURCE_MEM, "coproc");
	if (res && res->start == DCP_T6041_DISPEXT0_COPROC)
		return 1;
	if (res && res->start == DCP_T6041_DISPEXT1_COPROC)
		return 2;
	return 0xf;
}

static void dcp_t6041_s2r_domains_off(struct apple_dcp *dcp)
{
	struct dcp_t6041_ps_offsets o;
	/* DISP_CPU is 0x10000, too wide for u16 -- keep the offsets u32. */
	struct {
		u32 offset, first, second;
	} steps[3];
	void __iomem *ps, *pmc;
	u32 val, id = dcp_s2r_mark_id(dcp);
	int i;

	if (!dcp_t6041_ps_offsets(dcp, &o))
		return;
	/* CPU first, then FE, then SYS. */
	steps[0] = (typeof(steps[0])){ o.cpu, 0xf0400ff, 0xf0403f0 };
	steps[1] = (typeof(steps[0])){ o.fe,  0,         0x23f0 };
	steps[2] = (typeof(steps[0])){ o.sys, 0,         0x23f0 };

	if (dcp_t6041_display_power(dcp, &ps, &pmc))
		return;

	/*
	 * As on T8140, where the firmware's D572 does it: the display DART's scanout streams
	 * off before the domains go (the wake turns them back on first, W2).
	 */
	if (!dcp->external) {
		dcp_s2r_find_darts(dcp);
		if (dcp->s2r_disp_dart)
			apple_dart_display_streams(dcp->s2r_disp_dart, DCP_S2R_DISP_STREAMS,
						   false, false);
	}
	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 0xc));
	dcp_pmc_set(dcp, pmc, false);	/* shared voter: counts every powered DCP */
	/* TODO(ADT): conditional-retention disable + axi2af bridge bit0 clear here. */
	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		apple_sleep_bringup_mark(DCP_S2R_MARK(id, 0xd) | (i + 1) << 8);
		if (steps[i].first)
			writel(steps[i].first, ps + steps[i].offset);
		writel(steps[i].second, ps + steps[i].offset);
		readl_poll_timeout(ps + steps[i].offset, val,
				   !(val & GENMASK(7, 4)) && !(val & BIT(11)), 100, 50000);
	}
	iounmap(ps);
	iounmap(pmc);
}

/*
 * T6040 suspend-to-RAM: the desktop 26.x firmware's own stop, in the order
 * (the pipeline's power state, then the DCP power manager's and the
 * coprocessor's CPU stop), as measured on the J616s (A500 reached,
 * RTKit idle 0, the CPU stopped with status 0x6d): presentation drained, A473(0)
 * restartable sleep (dcp_sleep()), A500, RTKit idle (AP and IOP 0x201), the run bit
 * off, then bit 5. Only for a sleep that may lose power: with its power kept, a stopped
 * desktop firmware has no way back here (still 0x6d after the run bit; a
 * forced power cycle of its domain hung the SoC). So a dry run keeps it
 * running, its domains on, as suspend-to-idle does. The domain lower after the
 * stop (dcp_t6041_s2r_domains_off) and the cold restart on wake
 * (dcp_t6041_s2r_resume -> dcp_desktop_cold_restart) are built but NULL-guarded
 * on the apple,display-power window, absent in-tree: this stays gated until it (or
 * the genpd design) lands and a real power-loss S2R proves the restart.
 */
static bool dcp_t6041_s2r_capable(struct apple_dcp *dcp)
{
	return dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP &&
	       (of_device_is_compatible(dcp->dev->of_node, "apple,t6041-dcp") ||
		of_device_is_compatible(dcp->dev->of_node, "apple,t6041-dcpext"));
}

/* Past A473(0) nothing here brings the display back: a failure leaves it off until reboot. */
static void dcp_t6041_s2r_cpu_off(struct apple_dcp *dcp, u32 id)
{
	void __iomem *control = dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL;
	struct dcp_t6041_ps_offsets o;
	void __iomem *ps, *pmc, *w;
	u32 v, cur, after;
	int ret = 0;

	if (!dcp_t6041_ps_offsets(dcp, &o) || dcp_t6041_display_power(dcp, &ps, &pmc))
		return;
	w = ps + o.cpu;
	if (!dcp->external) {
		int i;

		for (i = 0; i < ARRAY_SIZE(dcp->s2r_child_saved); i++)
			dcp->s2r_child_saved[i] = readl(w + 8 * (i + 1));
		dcp->s2r_child_valid = true;
	}
	/* T8140's order (apple_dart_display_streams): streams off and TLB flushed, then GAPF. */
	if (READ_ONCE(s2r_dart_quiesce)) {
		int dret = -ENODEV;

		dcp_s2r_find_darts(dcp);
		apple_sleep_bringup_mark(0x7dd00000);
		if (dcp->s2r_dcp_dart)
			dret = apple_dart_display_streams(dcp->s2r_dcp_dart, DCP_S2R_DCP_STREAMS,
							  false, true);
		apple_sleep_bringup_mark(0x7dd10000 | (dret & 0xffff));
		dev_info(dcp->dev, "S2R: DCP DART streams off + flush: %d\n", dret);
	}
	if (READ_ONCE(s2r_gapf_release)) {
		struct resource *res = platform_get_resource_byname(to_platform_device(dcp->dev),
								     IORESOURCE_MEM, "coproc");
		phys_addr_t gpa = !dcp->external ? DCP_T6041_GAPF_DCP :
				  res && res->start == DCP_T6041_DISPEXT0_COPROC ? DCP_T6041_GAPF_DCPEXT0 :
				  res && res->start == DCP_T6041_DISPEXT1_COPROC ? DCP_T6041_GAPF_DCPEXT1 : 0;
		void __iomem *g = gpa ? ioremap_np(gpa, 4) : NULL;

		if (g) {
			u32 gv = readl(g), ga;

			writel(gv & ~DCP_T6041_GAPF_PROTECT, g);
			ga = readl(g);
			/* Kept by an SError on the CPU word: 0x7de0BBAA, BB before, AA after. */
			apple_sleep_bringup_mark(0x7de00000 | (gv & 0xff) << 8 | (ga & 0xff));
			dev_info(dcp->dev, "S2R: GAPF slice %pa %#x -> %#x\n", &gpa, gv, ga);
			iounmap(g);
		} else {
			apple_sleep_bringup_mark(DCP_S2R_MARK(id, 0xf));
		}
		if (READ_ONCE(s2r_gapf_all))
			dcp_t6041_gapf_release_all(dcp);
	} else {
		apple_sleep_bringup_mark(DCP_S2R_MARK(id, 0xf));
	}
	if (!READ_ONCE(s2r_cpu_write)) {
		iounmap(ps);
		iounmap(pmc);
		return;
	}
	if (READ_ONCE(s2r_cold_stop)) {
		writel(0, control);
		writel(0, control);
	}
	v = readl(w);
	/*
	 * Kept by an SError on the first write: 0x7dGPAvvv. G = 4 | the DCP slice's bit 4
	 * after the release, P = v[27:24], A = v[31:28], vvv = v[11:0].
	 */
	{
		struct resource *r = platform_get_resource_byname(to_platform_device(dcp->dev),
								   IORESOURCE_MEM, "coproc");
		phys_addr_t gp = !dcp->external ? DCP_T6041_GAPF_DCP :
				 r && r->start == DCP_T6041_DISPEXT0_COPROC ? DCP_T6041_GAPF_DCPEXT0 :
				 DCP_T6041_GAPF_DCPEXT1;
		void __iomem *g = ioremap_np(gp, 4);
		u32 ga = g ? readl(g) : 0xff;

		if (g)
			iounmap(g);
		apple_sleep_bringup_mark(0x7d000000 | (4 | !!(ga & DCP_T6041_GAPF_PROTECT)) << 20 |
					 ((v >> 24) & 0xf) << 16 | ((v >> 28) & 0xf) << 12 |
					 (v & 0xfff));
	}
	/* The platform: 0x1f0003ff -> W 0x0f0000ff, poll ACTUAL 15 -> W 0x0f0003f0. */
	{
		u32 keep = READ_ONCE(s2r_ps_auto_f) ? (v & 0x000f0000) | 0x0f000000 :
						     v & 0xeffffcff & ~0xffU;
		u32 w1 = keep | 0xff;

		/* Kept after the first write: 0x7dc1VVVV, the word's low half read back. */
		if ((v & BIT(28)) || READ_ONCE(s2r_ps_auto_f)) {
			writel(w1, w);
			mb();
			ret |= readl_poll_timeout_atomic(w, cur, (cur & 0xf0) == 0xf0, 1, 8000);
			apple_sleep_bringup_mark(0x7dc10000 | (readl(w) & 0xffff));
		}
		writel((readl(w) & 0xfffffcf0) | 0x300, w);
		mb();
		ret |= readl_poll_timeout_atomic(w, cur, !(cur & BIT(11)), 1, 8000);
		ret |= readl_poll_timeout_atomic(w, cur, !(cur & 0xf0), 1, 8000);
		/* Kept after the second write: 0x7dc2VVVV. */
		apple_sleep_bringup_mark(0x7dc20000 | (readl(w) & 0xffff));
	}
	after = readl(w);
	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 0x0));
	dev_info(dcp->dev, "S2R: CPU power word +%#x %#x -> %#x%s\n", o.cpu, v, after,
		 ret ? " (timeout)" : "");
	iounmap(ps);
	iounmap(pmc);
}

static bool s2r_afk_pause = true;
module_param(s2r_afk_pause, bool, 0644);
MODULE_PARM_DESC(s2r_afk_pause, "T6040 S2R: pause the DCP's AFK endpoints before its idle and resume them after the wake, as the platform (bring-up)");

#define DCP_S2R_AFK_TIMEOUT_MS	200

/*
 * As the platform around a DCP that idles for a sleep (the AFK endpoints' state machine on RTKit's
 * power notifications): every AFK endpoint is paused, highest first, before the IOP
 * idles, and resumed, lowest first, after its HELLO, before the display is powered.
 * Without the resume the firmware never announces its AV services again, its video
 * interface stays unplugged and every mode set is refused ("HPD is 0").
 */
static void dcp_t6041_s2r_afk(struct apple_dcp *dcp, bool resume)
{
	unsigned int i, n = ARRAY_SIZE(dcp->desktop->graph), done = 0, total = 0;
	int ret;

	if (!dcp->desktop || !READ_ONCE(s2r_afk_pause))
		return;
	for (i = 0; i < n; i++) {
		struct apple_dcp_afkep *ep = READ_ONCE(dcp->desktop->graph[resume ? i : n - 1 - i]);

		if (!ep)
			continue;
		total++;
		ret = resume ? afk_resume(ep, DCP_S2R_AFK_TIMEOUT_MS) :
			       afk_pause(ep, DCP_S2R_AFK_TIMEOUT_MS);
		if (ret)
			dev_warn(dcp->dev, "S2R: AFK endpoint %#x not %s: %d\n", ep->endpoint,
				 resume ? "resumed" : "paused", ret);
		else
			done++;
	}
	dev_info(dcp->dev, "S2R: %u of %u AFK endpoints %s\n", done, total,
		 resume ? "resumed" : "paused");
}

/*
 * No link work may reach a firmware that is about to idle. An external processor's was
 * stopped at prepare, when its link went (dcp_hib_prepare()), and stays stopped until the
 * wake replays the link.
 */
static void dcp_t6041_s2r_stop_link_work(struct apple_dcp *dcp)
{
#ifdef CONFIG_HIBERNATION
	if (dcp_desktop_external(dcp)) {
		dcp_hib_stop_ext_link_work(dcp);
		return;
	}
#endif
	dcp_disable_typec_work(dcp, false);
	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);
}

static int dcp_t6041_s2r_stop(struct apple_dcp *dcp)
{
	void __iomem *control = dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL;
	u32 status, id = dcp_s2r_mark_id(dcp);
	bool idle;
	int ret;

	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 1));
	/*
	 * An external firmware that did not come back from an earlier sleep, or that crashed,
	 * has nothing left to quiesce, and must not refuse every later sleep: its CPU off
	 * again, nothing else. The wake tries to start it once more.
	 */
	if (dcp_desktop_external(dcp) && !IS_ERR_OR_NULL(dcp->rtk) &&
	    (READ_ONCE(dcp->ext_dead) || READ_ONCE(dcp->crashed))) {
		apple_rtkit_mailbox_off(dcp->rtk);
		if (!READ_ONCE(s2r_keep_run)) {
			writel(readl(control) & ~APPLE_DCP_COPROC_CPU_CONTROL_RUN, control);
			writel(readl(control) & ~(APPLE_DCP_COPROC_CPU_CONTROL_RUN | BIT(5)),
			       control);
			if (READ_ONCE(s2r_cpu_off))
				dcp_t6041_s2r_cpu_off(dcp, id);
		}
		dcp->t6041_s2r_stopped = true;
		dev_info(dcp->dev, "dead external firmware: CPU off for the sleep\n");
		return 0;
	}
	if (READ_ONCE(dcp->crashed) || IS_ERR_OR_NULL(dcp->rtk))
		return -EBUSY;
	/* The DP port lets its display go first; that teardown isn't built here. */
	if (dcp->external && (READ_ONCE(dcp->active_typec_route) ||
			      READ_ONCE(dcp->typec_cable_connected))) {
		dev_err(dcp->dev, "refusing to sleep: a display is attached\n");
		return -EBUSY;
	}

	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 2));
	if (!dcp->external)
		iomfb_v14_7_backlight_suspend(dcp);
	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 3));
	dcp_drain_for_sleep(dcp);
	dcp_t6041_s2r_stop_link_work(dcp);

	/*
	 * The internal panel's DPMS is soft (backlight only), so its pipe is still lit here.
	 * The platform has the display off before the sleep: the firmware's set_power_state(0)
	 * lowers the pipe's eleven sub-blocks itself, and its PMGR panics if they are on
	 * when DISP_CPU goes down. Without it the pipe was fully on at the commit
	 * point and lowering under it raised an SError.
	 */
	if (!dcp->external && READ_ONCE(s2r_pipe_off)) {
		apple_sleep_bringup_mark(DCP_S2R_MARK(id, 0xe));
		_dcp_poweroff(dcp);
	}

	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 4));
	dcp_sleep(dcp);
	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 5));
	/*
	 * dcp_sleep() only warns when set_power_state(0) or a callback goes unanswered.
	 * A firmware still waiting on a call is stuck there after the warm restart and
	 * never lights the panel again: refuse the sleep instead.
	 */
	if (!READ_ONCE(dcp->crashed) &&
	    read_poll_timeout(iomfb_channels_idle, idle, idle, 1000, 100000, false, dcp))
		dev_err(dcp->dev, "calls in flight after the display sleep\n");
	if (READ_ONCE(dcp->crashed) || !iomfb_channels_idle(dcp)) {
		dev_err(dcp->dev, "the display sleep did not complete, the display stays off\n");
		goto lost;
	}
	ret = iomfb_s2r_500_v26_6_desktop(dcp);
	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 6));
	if (ret) {
		dev_err(dcp->dev, "no reply to A500: %d, the display stays off\n", ret);
		goto lost;
	}
	dcp_t6041_s2r_afk(dcp, false);
	/* The IOP may answer 0x201 with 1 or 2, asleep with its memory kept: -EINVAL is that. */
	ret = apple_rtkit_idle(dcp->rtk);
	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 7));
	if (ret && ret != -EINVAL) {
		dev_err(dcp->dev, "firmware did not go idle: %d, the display stays off\n", ret);
		goto lost;
	}
	/* The receive side (I2A_CONTROL bit0) goes off before the idle poll. */
	apple_rtkit_mailbox_off(dcp->rtk);
	if (readl_poll_timeout(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS, status,
			       status & 3, 100, 10000))
		dev_warn(dcp->dev, "CPU not idle (%#x), stopping it anyway\n", status);
	/*
	 * The platform clears the run bit, then bit 5, and lowers the CPU's
	 * power word itself. That word refuses writes from the OS here, so by default
	 * the run bit stays: the firmware idles in WFI and the PMGR's automatic mode gates it.
	 */
	if (!READ_ONCE(s2r_keep_run)) {
		writel(readl(control) & ~APPLE_DCP_COPROC_CPU_CONTROL_RUN, control);
		writel(readl(control) & ~(APPLE_DCP_COPROC_CPU_CONTROL_RUN | BIT(5)), control);
		/* Read now: once its CPU word is down, the ASC page faults. */
		status = readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS);
		if (READ_ONCE(s2r_cpu_off))
			dcp_t6041_s2r_cpu_off(dcp, id);
	} else {
		status = readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS);
	}
	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 8));
	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 9));
	dcp->t6041_s2r_stopped = true;
	dev_info(dcp->dev, "firmware stopped for the sleep: status %#x\n", status);

	/*
	 * Lower the display domains after the stop (NULL-guarded, see above). An
	 * external DCP's off sequence is the internal one's, not yet measured: by default its
	 * domains are left to the SoC power-down.
	 */
	if (dcp->external ? READ_ONCE(s2r_ext_domains_off) : READ_ONCE(s2r_int_domains_off)) {
		apple_sleep_bringup_mark(DCP_S2R_MARK(id, 0xa));
		dcp_t6041_s2r_domains_off(dcp);
	}
	apple_sleep_bringup_mark(DCP_S2R_MARK(id, 0xb));

	return 0;

lost:
	WRITE_ONCE(dcp->crashed, true);
	return -EBUSY;
}

/*
 * The desktop firmware brought back after a real power loss: W1 raises the
 * display domains, then the cold restart runs async in the reused
 * hib_restore_work so a 10 s dcp_wait_ready() does not stall .resume. The
 * internal panel re-points its scanout/display DART streams here (W2) and
 * shares dcp_desktop_cold_restart; an external DCP (HDMI/USB-C DP) rebuilds its
 * DART and link in dcp_hib_ext_restore(), so a docked desktop returns after the
 * sleep too. If W1 cannot run -- no apple,display-power window, or an external
 * DCP whose DISPEXT offsets are unresolved -- the panel/output is left off
 * until reboot, exactly as before: refusal-safe, the sleep already completed.
 */
static void dcp_t6041_s2r_resume(struct apple_dcp *dcp)
{
	dcp->t6041_s2r_stopped = false;

	/*
	 * An external DCP's domains come back only by the raise (retention pair, axi2af
	 * bridges), which isn't built for DISPEXT: touching it now raised an SError.
	 * Leave it dead, its USB-C/HDMI port's USB unaffected, until reboot.
	 */
	if (dcp->external) {
		WRITE_ONCE(dcp->ext_dead, true);
		WRITE_ONCE(dcp->crashed, true);
		dev_warn(dcp->dev, "external display left off after the sleep until reboot\n");
		return;
	}

	if (!dcp_t6041_s2r_domains_on(dcp)) {
#ifdef CONFIG_HIBERNATION
		if (!dcp_desktop_external(dcp)) {
			/* W2: the internal scanout/display DART streams back first. */
			dcp_s2r_find_darts(dcp);
			if (dcp->s2r_disp_dart)
				apple_dart_display_streams(dcp->s2r_disp_dart,
							   DCP_S2R_DISP_STREAMS, true, false);
			apple_sleep_bringup_mark(DCP_S2R_MARK(0, 0x9));
			/*
			 * The cold restart ran (RUN set, RTKit init) but every endpoint
			 * start timed out (-110) and the pipe never powered: the stop turned the
			 * DCP's own DART streams off (s2r_dart_quiesce) and only the display
			 * DART's came back. As dcp_s2r_ext_start() and dcp_s2r_warm_restart():
			 * after the domains, the DCP DART's streams on (GAPF protection again).
			 */
			if (READ_ONCE(s2r_dcp_dart_on) && dcp->s2r_dcp_dart)
				apple_dart_display_streams(dcp->s2r_dcp_dart,
							   DCP_S2R_DCP_STREAMS, true, false);
			apple_sleep_bringup_mark(DCP_S2R_MARK(0, 0xc));
		} else if (READ_ONCE(s2r_dcp_dart_on)) {
			/*
			 * An external DCP's stop turned its own DART's streams off as well:
			 * its firmware answers no endpoint start without them (-110).
			 */
			dcp_s2r_find_darts(dcp);
			if (dcp->s2r_dcp_dart)
				apple_dart_display_streams(dcp->s2r_dcp_dart,
							   DCP_S2R_DCP_STREAMS, true, false);
		}
		/*
		 * apple-drm resumes after this device and restores its CRTCs at once: its
		 * entry points leave the firmware alone until the restart is through, as
		 * after a resume from disk. A call of theirs into the returning firmware
		 * timed out, and one sent while another was unanswered crashed it.
		 */
		WRITE_ONCE(dcp->hib_phase, DCP_HIB_RESTARTING);
		if (dcp_desktop_external(dcp))
			dcp_hib_ext_get(dcp);
		dcp->s2r_cold_restart = true;
		queue_work(system_long_wq, &dcp->hib_restore_work);
		return;
#endif
	}

#ifdef CONFIG_HIBERNATION
	/* Not coming back: the other external processors do not wait for this one's link. */
	dcp_hib_ext_put(dcp, true, true);
#endif
	WRITE_ONCE(dcp->crashed, true);
	dev_crit(dcp->dev,
		 "the firmware stopped for the sleep could not restart (%s): the display stays off until reboot\n",
		 pm_resume_via_firmware() ? "after a real sleep" : "the sleep was refused");
}

static int dcp_platform_suspend(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	/* Its power sequences may crash the DCP and blank the panel. */
	if (dcp_takeover_was_aborted(dcp))
		return -EBUSY;

	/* A firmware start from the last resume finishes first; this sleep stops it again. */
	flush_work(&dcp->s2r_prewarm_work);
	cancel_delayed_work_sync(&dcp->s2r_prewarm_off_wq);
	dcp->s2r_prewarmed = false;

	if (pm_suspend_via_firmware() && dcp_s2r_capable(dcp)) {
		int ret = dcp_s2r_suspend(dcp);

		/* Lit by the last resume with its CRTC off: the next resume finds it off by DPMS. */
		if (!ret && dcp->s2r_wake_lit) {
			dcp->s2r_wake_lit = false;
			dcp->dpms_powered_off = true;
		}
		return ret;
	}
	if (pm_suspend_via_firmware() && dcp_s2r_ext_capable(dcp))
		return dcp_s2r_ext_suspend(dcp);
	if (pm_suspend_via_firmware() && dcp_t6041_s2r_capable(dcp)) {
		if (apple_sleep_power_loss())
			return dcp_t6041_s2r_stop(dcp);
		dev_info(dev, "dry run: firmware kept running and its domains on, as for suspend-to-idle\n");
	}

	/*
	 * T8132: setPowerState powers the display down; the coprocessor keeps
	 * running and its power domains stay on, as below, unless the internal
	 * display is blanked and this is a deep sleep, or appledrm.power_gate_ms
	 * is set: then the coprocessor and its domains are powered down too
	 * (iomfb_m4_suspend()).
	 */
	if (iomfb_is_m4(dcp)) {
		int ret = iomfb_m4_suspend(dcp);

		if (!ret)
			device_set_wakeup_path(dev);
		return ret;
	}
	if (READ_ONCE(dcp->ext_dead))
		return 0;
	/* PM_SUSPEND_PREPARE flushed it; a restart still running is refused. */
	if (dcp_hib_busy(dcp)) {
		dev_warn(dev, "suspend refused: firmware restart after resume from disk still running\n");
		return -EBUSY;
	}
	iomfb_v14_7_backlight_suspend(dcp);

	/* Serialize PM against explicit bring-up, including queued startup work.
	 * Before a startup attempt the device remains suspendable. After any
	 * attempt we cannot prove DMA/firmware quiescence, even on failure.
	 */
	if (dcp->external) {
		const char *why;

		mutex_lock(&dcp->hpd_mutex);
		why = dcp_external_sleep_vetoed(dcp);
		if (why) {
			mutex_unlock(&dcp->hpd_mutex);
			if (dcp->external_native)
				dev_warn(dev, "suspend refused: %s\n", why);
			else
				dev_warn(dev, "external firmware/scanout attempted: suspend refused; retained DMA requires reboot\n");
			return -EBUSY;
		}
		WRITE_ONCE(dcp->external_suspended, true);
		mutex_unlock(&dcp->hpd_mutex);
	}

	if (dcp_is_external_h17p(dcp)) {
		int ret;

		dcp_disable_typec_work(dcp, false);
		ret = dcp_external_disconnect(dcp, false);
		if (ret) {
			dcp_enable_typec_work(dcp);
			return ret;
		}
		/*
		 * The crossbar shares the ATC power domain, which an s2idle can take
		 * down too: with deep sleep selected, the USB controller stops for any
		 * sleep and stops holding it. The crossbar then wakes with its route
		 * lost, while the mux core still holds the selection, and the link
		 * never trains again (DEVICE_NOT_RESPONDING). Take the output down
		 * while it has power and select it again on resume, as s2r does.
		 */
		dcp_typec_route_park(dcp);
		device_set_wakeup_path(dev);
		return 0;
	}

	dcp_drain_for_sleep(dcp);
	/*
	 * The Type-C route reports cable removal through
	 * dcp_dptx_disconnect_oob(). A DP tunnel kept through the sleep stays
	 * connected, and resume powers the CRTC back up through dcp_poweron()
	 * as after DPMS off.
	 */
	dcp_disable_typec_work(dcp, false);
	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);
	if (dcp->external_native)
		disable_work_sync(&dcp->external_audio_work);

	if (dcp->avep)
		av_service_disconnect(dcp);

	if (dcp->hdmi_hpd_irq) {
		disable_irq(dcp->hdmi_hpd_irq);
		if (!dcp->active_typec_route) {
			disconnected_hpd_event(dcp->connector);
			dcp_dptx_disconnect(dcp, 0);
		}
	}
	/*
	 * Set the device as a wakeup device, which forces its power
	 * domains to stay on. We need this as we do not support full
	 * shutdown properly yet.
	 */
	device_set_wakeup_path(dev);
	/* Last: the steps above still talk to the coprocessor. */
	dcp_hold_for_sleep(dcp);

	return 0;
}

static int dcp_platform_resume(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	if (dcp_takeover_was_aborted(dcp))
		return 0;

	if (dcp->s2r_stopped)
		return dcp_s2r_resume(dcp);
	if (dcp->t6041_s2r_stopped) {
		dcp_t6041_s2r_resume(dcp);
		return 0;
	}

	if (iomfb_is_m4(dcp))
		return iomfb_m4_resume(dcp);
	if (READ_ONCE(dcp->ext_dead))
		return 0;

	dcp->link_recovery_tries = 0;
	dcp_release_after_sleep(dcp);
	iomfb_v14_7_backlight_resume(dcp);
	/* Its crossbar output, parked by the suspend, before the link connects again. */
	if (dcp_is_external_h17p(dcp))
		dcp_typec_route_unpark(dcp);
	dcp_enable_typec_work(dcp);
	/* The HDMI output's PHY and crossbar may have lost their setup in sleep. */
	if (dcp->external_native)
		dcp_fabric_hdmi_reinit(dcp, "after system sleep");
	/* Observe future edges before sampling any edges lost in sleep. */
	if (dcp->hdmi_hpd_irq)
		dcp_fabric_run_resume(&dcp_resume_ops, dcp);
	if (dcp->external_native)
		enable_work(&dcp->external_audio_work);

	if (dcp->avep)
		av_service_connect(dcp);

	return 0;
}

#ifdef CONFIG_HIBERNATION
/*
 * Hibernation.
 *
 * The image is written with the display left as it is: the firmware's own
 * power sequence is not run (it may crash the DCP), so the panel keeps
 * showing the last frame until the machine powers off.
 *
 * A resume from disk is a cold boot: the boot loader restarts the DCP
 * firmware and lights the panel with its boot framebuffer, and the boot
 * kernel (which does not load this driver) jumps into the image.  For the
 * desktop 26.x panel (T6041, not adopted: probe itself runs the coprocessor
 * and does the RTKit handshake) the restored driver rebuilds the firmware
 * session in place, the way probe built it:
 *
 *  freeze:   hold RTKit receive, so the image holds it held: what the new
 *            firmware sends before .restore (the AIC masks come back at
 *            syscore resume and the mailbox IRQ is IRQF_NO_SUSPEND) stays in
 *            the mailbox FIFO instead of reaching the old RTKit session.
 *  restore:  drop the old session on the host side (dcp_hib_quiesce()):
 *            mode invalid, endpoints quiesced, IOMFB channels, firmware
 *            mappings, retained framebuffers and pending flips released.
 *            From here the DRM entry points leave the firmware alone and a
 *            flip completes without a swap (dcp_hib_gated()).
 *  PM_POST_HIBERNATION (after the PMP is rebuilt), in a work item:
 *            CPU_CONTROL.RUN, apple_rtkit_reinit(), receive released, then
 *            dcp_start_desktop() as at probe (RTKit wake/HELLO/EPMAP, system
 *            and IOMFB endpoints, panel endpoints, set_shmem with the same
 *            arena), the IOMFB start sequence, pipe power on and a modeset
 *            commit of the current DRM state, which also swaps the frame.
 *
 * Every devm allocation, the DMA arena, the iommu mappings, clocks and
 * registers are reused; no probe primitive runs again.  The old endpoint
 * objects are quiesced and stay allocated until unbind (a bounded leak per
 * resume) because their services may still be referenced.  Any failing step
 * falls back to the previous behaviour: IOMFB traffic stopped, display left
 * to the boot loader, until the reboot.
 *
 * The T6041 external processors (dcpext: HDMI on dcpext0, the Type-C
 * routes on all of them, the dual-pipe peer of a display wider than one
 * pipe) are restarted the same way, and then their display link is set up
 * again through the paths a display plugged at runtime takes:
 *
 *  prepare (dcp_hib_prepare(), only for hibernation: dcp_hib_in_progress
 *            is set by a PM_HIBERNATION_PREPARE notifier), with every DCP's
 *            receive still live: the link work (dcp_disable_typec_work(),
 *            as for s2idle) and the HDMI HPD interrupt are stopped, a merged
 *            dual pipe is stopped as a pair, and the display link and the
 *            DP PHYs are given back as at a runtime unplug
 *            (dcp_hib_release_ext_link()): the T6040 PHY has no PM ops and
 *            the cold boot resets it, so the image must record it off.
 *  freeze:   as above.  The link stays given back through thaw and the
 *            image write; if this kernel goes on (the hibernation failed,
 *            or disk=suspend woke), PM_POST_HIBERNATION replays it, as
 *            below, once every DCP is thawed.
 *  restore_early, before any .restore (the Type-C port controllers'
 *            .restore may report their mode again, the crossbar rewrites
 *            its routes in its own): the old session is dropped on the
 *            host side as above, its DMA (firmware mappings, framebuffers)
 *            only in .restore, after the DARTs restored theirs; and also
 *            its DPTX port services, IBOOT, DPAVSERV and the HDR
 *            service, the mode list, the dual-pipe pairing and the
 *            HDR/rate state of the link.  A display that is shown is
 *            reported gone (disconnected_hpd_event(), as an unplug), so
 *            that its return is a hotplug to which the compositor answers
 *            with a full modeset; a Type-C display under DPMS off stays
 *            connected, as at runtime, and its wake connects it.  The Type-C
 *            route itself (crossbar selection, cable, generation) is kept.
 *  PM_POST_HIBERNATION, in the restart work: the firmware restart as
 *            above, with the start's DPTX connect held back
 *            (dcp_hib_link_deferred()), then the DRM entry points open
 *            (phase NONE).  A processor without a display then connects
 *            what its probe connects (the fixed HDMI output, sinkless);
 *            one with a display waits until every external processor's
 *            firmware runs and every idle one has done that, so that a
 *            display wider than one pipe finds its peer (the firmware asks
 *            for one during link training, dcp_peer_hints_26_6()), and
 *            then connects: the fixed PHY powered as at probe, the start's
 *            link step (its connect powers the route's PHY afresh,
 *            dcp_typec_phy_on()), dcp_enable_dp2hdmi_hpd()
 *            as dcp_wait_ready() does at probe (Type-C connect, HDMI HPD,
 *            HPD interrupt on) and the link work enabled again (the
 *            reconnect retries).  The firmware's hotplug then reports the
 *            display, the hotplug worker sets the link status BAD and
 *            resets an active CRTC, and userspace gets the event.
 *
 * Failure before the link step leaves the processor dead (ext_dead), as a
 * failed probe does: USB on its ports goes on.  A failing link is left to
 * the runtime reconnect and relink machinery.
 */
#define DCP_HIB_READY_TIMEOUT	msecs_to_jiffies(10000)

static bool dcp_hib_can_restart(struct apple_dcp *dcp)
{
	return dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP && dcp->desktop &&
	       !dcp->external &&
	       !dcp->hw.adopt_live_session && !READ_ONCE(dcp->retain_dma) &&
	       !IS_ERR_OR_NULL(dcp->rtk) && dcp->coproc_reg && dcp->shmem;
}

/*
 * External processors still starting their firmware, and idle ones (no
 * display) still connecting their link, in this restore.
 */
static atomic_t dcp_hib_ext_starting = ATOMIC_INIT(0);
static atomic_t dcp_hib_ext_idle = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(dcp_hib_ext_wq);

static void dcp_hib_ext_get(struct apple_dcp *dcp)
{
	if (dcp->hib_ext_start_pending)
		return;
	dcp->hib_ext_start_pending = true;
	atomic_inc(&dcp_hib_ext_starting);
}

static void dcp_hib_ext_put(struct apple_dcp *dcp, bool start, bool idle)
{
	bool wake = false;

	if (start && dcp->hib_ext_start_pending) {
		dcp->hib_ext_start_pending = false;
		atomic_dec(&dcp_hib_ext_starting);
		wake = true;
	}
	if (idle && dcp->hib_ext_idle_pending) {
		dcp->hib_ext_idle_pending = false;
		atomic_dec(&dcp_hib_ext_idle);
		wake = true;
	}
	if (wake)
		wake_up_all(&dcp_hib_ext_wq);
}

static bool dcp_hib_ext_settled(void)
{
	return !atomic_read(&dcp_hib_ext_starting) &&
	       !atomic_read(&dcp_hib_ext_idle);
}

static void dcp_hib_log_coproc(struct apple_dcp *dcp, const char *when)
{
	u32 ctl = readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	u32 sts = readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS);

	dev_info(dcp->dev,
		 "hibernation restore: %s: CPU_CONTROL %#x (RUN %d) CPU_STATUS %#x\n",
		 when, ctl, !!(ctl & APPLE_DCP_COPROC_CPU_CONTROL_RUN), sts);
}

/*
 * The descriptors the old firmware allocated or mapped: nothing uses them
 * any more, and the new firmware asks for its own.  As release_mem_desc.
 */
static void dcp_hib_release_memdescs(struct apple_dcp *dcp)
{
	unsigned int id = 1, n = 0;

	/* Bit 0 is reserved at bind: descriptor IDs start at 1. */
	for_each_set_bit_from(id, dcp->memdesc_map, DCP_MAX_MAPPINGS) {
		struct dcp_mem_descriptor *memdesc = &dcp->memdesc[id];
		size_t size = ALIGN(memdesc->size, SZ_16K);

		if (memdesc->piodma_mapped) {
			if (!dcp->iommu_dom ||
			    iommu_unmap(dcp->iommu_dom, memdesc->dva, size) != size) {
				/* Keep pages the display could still read. */
				dev_warn(dcp->dev,
					 "hibernation restore: buffer %u stays mapped for piodma\n",
					 id);
				continue;
			}
			memdesc->piodma_mapped = false;
		}
		clear_bit(id, dcp->memdesc_map);
		if (memdesc->buf) {
			sg_free_table(&memdesc->map);
			dma_free_coherent(dcp->dev, size, memdesc->buf, memdesc->dva);
			memdesc->buf = NULL;
			memset(&memdesc->map, 0, sizeof(memdesc->map));
		} else if (memdesc->size) {
			dma_unmap_resource(dcp->dev, memdesc->dva, memdesc->size,
					   DMA_BIDIRECTIONAL, 0);
			memdesc->reg = 0;
		}
		memdesc->dva = 0;
		memdesc->size = 0;
		n++;
	}
	dev_dbg(dcp->dev, "hibernation restore: released %u firmware mappings\n", n);
}

/* Host side of the old firmware session; called with userspace frozen. */
static void dcp_hib_quiesce(struct apple_dcp *dcp)
{
	struct dcp_desktop_state *d = dcp->desktop;
	int i;

	WRITE_ONCE(dcp->hib_phase, DCP_HIB_QUIESCED);
	WRITE_ONCE(dcp->active, false);
	dcp_mode_invalidate(&dcp->mode_state);

	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);
	dcp_backlight_stop(dcp);

	/* Endpoint objects stay allocated: their services may be referenced. */
	afk_quiesce(dcp->systemep);
	dcp->systemep = NULL;
	for (i = 0; i < ARRAY_SIZE(d->graph); i++) {
		struct apple_dcp_afkep *ep = READ_ONCE(d->graph[i]);

		if (ep) {
			afk_quiesce(ep);
			WRITE_ONCE(d->graph[i], NULL);
		}
	}
	d->graph_started = 0;
	d->graph_skipped = 0;
	d->dpavctrl_av_controller[0] = NULL;
	d->dpavctrl_av_controller[1] = NULL;
	d->dpavctrl_late_service = NULL;

	/* Calls in flight on the old session are never answered. */
	memset(&dcp->ch_cmd, 0, sizeof(dcp->ch_cmd));
	memset(&dcp->ch_oobcmd, 0, sizeof(dcp->ch_oobcmd));
	memset(&dcp->ch_cb, 0, sizeof(dcp->ch_cb));
	memset(&dcp->ch_oobcb, 0, sizeof(dcp->ch_oobcb));
	memset(&dcp->ch_async, 0, sizeof(dcp->ch_async));
	memset(&dcp->ch_oobasync, 0, sizeof(dcp->ch_oobasync));
	kfree(dcp->chunks.data);
	memset(&dcp->chunks, 0, sizeof(dcp->chunks));

	/* Start over as from probe: clear the surfaces on the first swap. */
	dcp->surfaces_cleared = false;
	dcp->have_swap_complete = false;
	dcp->last_swap_id = 0;
	d->mode_live = false;
	d->modeset_active = false;
	d->modeset_callback_error = false;
	d->trial_powered = false;
	d->trial_modeset_ok = false;
	d->poweron_result = 0;
	d->present_pending = false;
	d->vrr_min_sent = 0;
}

/*
 * The old session's DMA: the firmware mappings and the framebuffers its
 * swaps held.  In .restore, not before: the DARTs restore their state in the
 * normal phase (pm_runtime_force_resume()), and these unmap through them.
 */
static void dcp_hib_release_dma(struct apple_dcp *dcp)
{
	dcp_hib_release_memdescs(dcp);
	/* Swaps of the old session never complete: drop what they held. */
	dcp_release_all_retained_framebuffers(dcp);

	/* A flip the old session took before the image completes now. */
	if (READ_ONCE(dcp->crtc))
		schedule_work(&dcp->vblank_wq);
}

/*
 * External processor at freeze (and, if freeze did not, at restore): no link
 * work and no HDMI HPD edge may reach the old session, nor, after the
 * restore, the new one before it runs.  As dcp_platform_suspend() stops them
 * for s2idle, without its firmware calls.
 */
static void dcp_hib_stop_ext_link_work(struct apple_dcp *dcp)
{
	if (!dcp->hib_typec_disabled) {
		dcp_disable_typec_work(dcp, false);
		dcp->hib_typec_disabled = true;
	}
	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);
	if (dcp->hdmi_hpd_irq && !dcp->hib_irq_disabled) {
		disable_irq(dcp->hdmi_hpd_irq);
		dcp->hib_irq_disabled = true;
	}
}

/*
 * External processor at hibernation prepare (dcp_hib_prepare()), after the
 * link work is stopped, while every DCP's RTKit receive is still live:
 * release the display link and give back the DP PHYs, so that the image
 * records them off.  The T6040 PHY (apple,t6040-atcphy)
 * has no PM ops: the S4 cold boot resets it while its driver state, the PHY
 * core's init/power counts and phy_powered come back from the image as on,
 * and a later phy_power_on() would not set it up again.
 *
 * The link goes as at a runtime unplug of the sink (the HDMI
 * dcp_desktop_dptx_disconnect_oob(), which s2idle also runs for HDMI):
 * a shown display is reported gone, a merged dual pipe stopped as a pair
 * first (dcp_peer_stop_26_6(), as the pipe power-off does: a single-pipe
 * teardown of a merged pair loops the firmware), the AV service
 * disconnected, HPD dropped, the display released.  The peer pipe of a
 * pair has its owner stop the pair before its own link goes, whichever of
 * the two prepares first.
 * Unlike a cable unplug the Type-C route, its crossbar selection and the
 * cable flag stay.  A Type-C display under DPMS off has no link (its CRTC
 * disable released it) and stays connected, as at runtime.  Then the
 * route's PHY is given back as a route deactivation does, and the fixed
 * (HDMI) PHY, which probe powered for the life of the device, likewise.
 * The next connect powers the route's PHY again (dcp_typec_phy_on()),
 * dcp_hib_ext_relink() the fixed one.
 */
static void dcp_hib_release_ext_link(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	struct apple_connector *con = READ_ONCE(dcp->connector);
	bool typec = dcp_is_typec_output(dcp), shown, route_phy;
	struct apple_dcp *owner;
	int ret = 0;

	/* What a Type-C display lists now, for the wake: dcp_wake_list_check(). */
	WRITE_ONCE(dcp->wake_reread, 0);
	WRITE_ONCE(dcp->wake_list_sig,
		   typec && READ_ONCE(dcp->active_typec_route) && con &&
		   READ_ONCE(con->connected) && READ_ONCE(con->dcp) == pdev &&
		   dcp->nr_modes ? dcp_modes_signature(dcp) : 0);
	if (!dcp->fixed_phy && !READ_ONCE(dcp->active_typec_route))
		return;

	shown = con && READ_ONCE(con->connected) && READ_ONCE(con->dcp) == pdev &&
		!(typec && READ_ONCE(dcp->typec_crtc_off));
	if (shown)
		disconnected_hpd_event(con);
	/* A crashed session answers nothing: only the PHYs are given back. */
	if (READ_ONCE(dcp->crashed))
		goto phys;
	scoped_guard(mutex, &dcp_peer_lock)
		owner = dcp->peer_owner;
	if (owner)
		ret = dcp_peer_stop_26_6(owner);
	if (ret >= 0)
		ret = dcp_peer_stop_26_6(dcp);
	if (ret < 0)
		dev_warn(dcp->dev, "hibernation: dual-pipe pair not stopped: %d\n", ret);
	else if (ret > 0)
		dev_info(dcp->dev, "hibernation: dual-pipe pair stopped\n");
	ret = 0;
	if (dcp->avep)
		av_service_disconnect(dcp);
	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (dptx_service_available(&dcp->dptxport[0]) &&
		    dcp->dptxport[0].connected)
			ret = dptxport_set_hpd(dcp->dptxport[0].service, false);
	}
	if (ret)
		dev_warn(dcp->dev, "hibernation: DPTX HPD not dropped: %d\n", ret);
	ret = dcp_dptx_disconnect(dcp, 0);
	if (ret)
		dev_warn(dcp->dev, "hibernation: display link not released: %d\n", ret);
	if (!dcp->main_display)
		dcp->desktop->ext_pipe_off = true;
	if (typec)
		reinit_completion(&dcp->typec_iomfb_hpd_ready);

phys:
	route_phy = dcp_typec_phy_release(dcp);
	if (dcp->fixed_phy && !dcp->hib_fixed_phy_off) {
		phy_power_off(dcp->fixed_phy);
		phy_exit(dcp->fixed_phy);
		dcp->hib_fixed_phy_off = true;
	}
	dcp_hdmi_converter_power(dcp, false);
	dev_info(dcp->dev,
		 "hibernation: display link released (display %s), DP PHY off: route %d, fixed %d\n",
		 shown ? "reported gone" :
		 (con && READ_ONCE(con->connected)) ? "kept (DPMS off)" : "none",
		 route_phy, dcp->hib_fixed_phy_off);
}

/* The fixed PHY back on, as probe powers it (dcp_desktop_probe_routes()). */
static void dcp_hib_fixed_phy_on(struct apple_dcp *dcp)
{
	int ret;

	if (!dcp->hib_fixed_phy_off)
		return;
	ret = phy_init(dcp->fixed_phy);
	if (!ret) {
		ret = phy_power_on(dcp->fixed_phy);
		if (ret)
			phy_exit(dcp->fixed_phy);
	}
	if (ret) {
		/*
		 * Stays off: the device action at unbind must not power off a
		 * PHY that is not on.  The HDMI output has no link then.
		 */
		dev_err(dcp->dev, "hibernation: fixed DP PHY not powered again: %d\n",
			ret);
		return;
	}
	dcp->hib_fixed_phy_off = false;
	msleep(25);
}

/*
 * The dual-pipe pairing (dcp-peer-v26_6.inc) belongs to the two firmware
 * sessions: both restart, and the next link training of the wide display
 * reserves its peer again.  peer_unbinding is the component's, kept.
 */
static void dcp_hib_peer_reset(struct apple_dcp *dcp)
{
	struct apple_dcp *peer, *owner;

	guard(mutex)(&dcp_peer_lock);
	peer = dcp->peer_reserved;
	owner = dcp->peer_owner;
	if (peer || owner)
		dev_info(dcp->dev, "hibernation restore: dual-pipe pairing with %s dropped\n",
			 dev_name((peer ?: owner)->dev));
	if (peer) {
		peer->peer_owner = NULL;
		memset(&peer->peer_tiling, 0, sizeof(peer->peer_tiling));
		peer->peer_complete_logged = false;
	}
	if (owner) {
		owner->peer_reserved = NULL;
		owner->peer_active = false;
		memset(&owner->peer_tiling, 0, sizeof(owner->peer_tiling));
	}
	dcp->peer_reserved = NULL;
	dcp->peer_owner = NULL;
	memset(&dcp->peer_tiling, 0, sizeof(dcp->peer_tiling));
	dcp->peer_active = false;
	dcp->peer_failed = false;
	dcp->peer_complete_logged = false;
	WRITE_ONCE(dcp->peer_starved, false);
}

/*
 * The external processor's part of the old session, after dcp_hib_quiesce():
 * the endpoints its start brought up beside the panel's, the DPTX ports
 * (so that the new firmware's port services bind: dptx_service_bind()
 * refuses a port that still has one), the HDR service, the mode list (as
 * at probe, so that the IOMFB start does not report the display connected
 * from it), and the display, reported gone.
 */
static void dcp_hib_quiesce_ext(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	struct apple_connector *con = READ_ONCE(dcp->connector);
	struct dcp_desktop_state *d = dcp->desktop;
	struct apple_epic_service *av;
	unsigned long flags;
	bool shown;
	int i;

	dcp_hib_stop_ext_link_work(dcp);

	/*
	 * Shown: report it gone now, as an unplug.  A Type-C display whose
	 * CRTC is off (DPMS) stays connected, as it does at runtime: the wake
	 * connects its link (dcp_poweron()).
	 */
	shown = con && READ_ONCE(con->connected) && READ_ONCE(con->dcp) == pdev &&
		!(dcp_is_typec_output(dcp) && READ_ONCE(dcp->typec_crtc_off));
	if (shown)
		disconnected_hpd_event(con);

	afk_quiesce(dcp->dptxep);
	dcp->dptxep = NULL;
	afk_quiesce(dcp->ibootep);
	dcp->ibootep = NULL;
	afk_quiesce(dcp->avep);
	dcp->avep = NULL;
	if (dcp->dcpavservep) {
		afk_quiesce(dcp->dcpavservep);
		dpavservep_detach(dcp);
		dcp->dcpavservep = NULL;
	}

	scoped_guard(mutex, &dcp->hpd_mutex) {
		for (i = 0; i < ARRAY_SIZE(dcp->dptxport); i++) {
			struct dptx_port *port = &dcp->dptxport[i];

			smp_store_release(&port->enabled, false);
			WRITE_ONCE(port->connected, false);
			WRITE_ONCE(port->closed, false);
			WRITE_ONCE(port->tile_hint, false);
			port->service = NULL;
			reinit_completion(&port->enable_completion);
			reinit_completion(&port->linkcfg_completion);
		}
		WRITE_ONCE(dcp->typec_poweron_hpd, false);
	}
	reinit_completion(&dcp->typec_iomfb_hpd_ready);
	dcp_180hz_invalidate(dcp);

	scoped_guard(spinlock_irqsave, &d->hdr_lock) {
		av = d->av_service;
		d->av_service = NULL;
	}
	if (av)
		afk_service_put(av);
	dcp_hdr_metadata_forget(dcp);

	/* No modes until the new firmware describes the display. */
	scoped_guard(mutex, &dcp->modes_lock) {
		dcp->modes_generation++;
		dcp->modes_admitted = false;
		dcp->modes_provisional = false;
		/* The replaced array is retained, as for dcp_modes_replace(). */
		spin_lock_irqsave(&dcp->metadata.lock, flags);
		dcp->modes = NULL;
		dcp->nr_modes = 0;
		spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	}

	/* The new firmware has the pipe off, as after an unplug. */
	d->ext_pipe_off = true;
	dcp_hib_peer_reset(dcp);

	dev_info(dcp->dev,
		 "hibernation restore: external session dropped: route %s, cable %d, display %s\n",
		 READ_ONCE(dcp->active_typec_route) ? "Type-C" :
		 dcp->fixed_phy ? "fixed" : "none",
		 READ_ONCE(dcp->typec_cable_connected),
		 shown ? "reported gone" :
		 (con && READ_ONCE(con->connected)) ? "kept (DPMS off)" : "none");
}

/* Commits that started before the locks were taken finish first. */
static void dcp_hib_wait_commits(struct drm_crtc *crtc)
{
	struct drm_crtc_commit *commit;

	spin_lock(&crtc->commit_lock);
	commit = list_first_entry_or_null(&crtc->commit_list,
					  struct drm_crtc_commit, commit_entry);
	if (commit)
		drm_crtc_commit_get(commit);
	spin_unlock(&crtc->commit_lock);
	if (!commit)
		return;
	if (!wait_for_completion_timeout(&commit->cleanup_done, 10 * HZ))
		dev_warn(crtc->dev->dev,
			 "hibernation restore: earlier commit did not finish\n");
	drm_crtc_commit_put(commit);
}

/*
 * Power the pipe on and commit the current state of the CRTC with a mode
 * set and its planes: the firmware gets the mode and the frame, and the
 * compositor's next flip goes to the new session.  A CRTC switched off by
 * soft DPMS gets the pipe back with the backlight off; its enable sets the
 * mode.
 */
static int dcp_hib_recommit(struct apple_dcp *dcp)
{
	struct apple_crtc *acrtc = READ_ONCE(dcp->crtc);
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_crtc_state *cs;
	struct drm_crtc *crtc;
	bool powered = false;
	int ret;

	if (!acrtc) {
		/* No DRM pipeline: nothing to set, the firmware is up. */
		WRITE_ONCE(dcp->hib_phase, DCP_HIB_NONE);
		dev_info(dcp->dev, "hibernation restore: no CRTC, no mode to set\n");
		return 0;
	}
	crtc = &acrtc->base;

	DRM_MODESET_LOCK_ALL_BEGIN(crtc->dev, ctx, 0, ret);
	dcp_hib_wait_commits(crtc);

	if (!crtc->state->active) {
		if (!dcp_uses_soft_dpms(dcp)) {
			/* dcp_poweron() powers the pipe on the enable. */
			WRITE_ONCE(dcp->hib_phase, DCP_HIB_NONE);
			dev_info(dcp->dev,
				 "hibernation restore: CRTC off, pipe and mode set on enable\n");
			goto unlock;
		}
		if (!powered) {
			iomfb_poweron_v26_6_desktop(dcp);
			powered = true;
		}
		if (READ_ONCE(dcp->crashed) || dcp->desktop->poweron_result) {
			ret = -EIO;
			goto unlock;
		}
		WRITE_ONCE(dcp->hib_phase, DCP_HIB_NONE);
		ret = dcp_backlight_dpms(dcp, false);
		if (ret)
			dev_warn(dcp->dev,
				 "hibernation restore: backlight blank unavailable: %d\n",
				 ret);
		ret = 0;
		dev_info(dcp->dev,
			 "hibernation restore: CRTC off, pipe powered, mode set on enable\n");
		goto unlock;
	}

	if (!powered) {
		iomfb_poweron_v26_6_desktop(dcp);
		powered = true;
	}
	if (READ_ONCE(dcp->crashed) || dcp->desktop->poweron_result) {
		ret = -EIO;
		goto unlock;
	}

	state = drm_atomic_commit_alloc(crtc->dev);
	if (!state) {
		ret = -ENOMEM;
		goto unlock;
	}
	state->acquire_ctx = &ctx;
	cs = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(cs)) {
		ret = PTR_ERR(cs);
		goto put;
	}
	/* The same mode, set again on a firmware that has none. */
	cs->mode_changed = true;
	ret = drm_atomic_add_affected_connectors(state, crtc);
	if (!ret)
		ret = drm_atomic_add_affected_planes(state, crtc);
	if (ret)
		goto put;

	WRITE_ONCE(dcp->hib_phase, DCP_HIB_NONE);
	ret = drm_atomic_commit(state);
	/* The commit succeeds even when the firmware refused the mode set in it. */
	if (!ret && READ_ONCE(dcp->mode_state.valid) && !READ_ONCE(dcp->crashed))
		dev_info(dcp->dev,
			 "hibernation restore: mode " DRM_MODE_FMT " set\n",
			 DRM_MODE_ARG(&cs->mode));
	else if (!ret)
		dev_err(dcp->dev,
			"hibernation restore: mode " DRM_MODE_FMT " committed but not set (crashed %d)\n",
			DRM_MODE_ARG(&cs->mode), READ_ONCE(dcp->crashed));
	else if (ret != -EDEADLK)
		WRITE_ONCE(dcp->hib_phase, DCP_HIB_RESTARTING);
put:
	drm_atomic_commit_put(state);
unlock:
	if (ret == -EDEADLK)
		WRITE_ONCE(dcp->hib_phase, DCP_HIB_RESTARTING);
	DRM_MODESET_LOCK_ALL_END(crtc->dev, ctx, ret);
	return ret;
}

static void dcp_hib_give_up(struct apple_dcp *dcp, const char *step, int ret)
{
	dev_err(dcp->dev,
		"hibernation restore: firmware restart failed at %s (%d). Display left to the boot loader, IOMFB traffic stopped\n",
		step, ret);
	WRITE_ONCE(dcp->hib_phase, DCP_HIB_FAILED);
	WRITE_ONCE(dcp->ext_dead, true);
	if (READ_ONCE(dcp->crtc))
		schedule_work(&dcp->vblank_wq);
	/* A display processor waiting for this one goes on without it. */
	dcp_hib_ext_put(dcp, true, true);
}

/* A display is attached: a Type-C route is claimed, or HDMI HPD is high. */
static bool dcp_hib_ext_has_display(struct apple_dcp *dcp)
{
	return READ_ONCE(dcp->active_typec_route) ||
	       (dcp->hdmi_hpd && gpiod_get_value_cansleep(dcp->hdmi_hpd) > 0);
}

/*
 * The link set up again as at probe and after s2idle, see the overview
 * above.  The HPD interrupt that dcp_enable_dp2hdmi_hpd() enables was
 * disabled at freeze, or at restore.
 */
static void dcp_hib_ext_relink(struct apple_dcp *dcp)
{
	dcp->hib_link_released = false;
	/*
	 * The HDMI converter is powered through SMC GPIOs. Its own rail went
	 * with the link and comes back for this one. The +5 V stays on; the cold
	 * boot of a resume from disk turned it off behind the restored kernel's
	 * back, and without it HPD stays low and no plug is ever seen. The rails
	 * settle before the link (as the T6030 output set-up has it).
	 */
	if (dcp->hdmi_pwren || dcp->dp2hdmi_pwren) {
		if (dcp->hdmi_pwren)
			gpiod_set_value_cansleep(dcp->hdmi_pwren, 1);
		if (dcp->dp2hdmi_pwren)
			dcp_hdmi_converter_power(dcp, true);
		else
			msleep(100);
		dev_info(dcp->dev, "HDMI converter powered again\n");
	}
	dcp_hib_fixed_phy_on(dcp);
	if (dcp->hib_typec_disabled) {
		dcp->hib_typec_disabled = false;
		dcp_enable_typec_work(dcp);
	}
	dcp_start_desktop_ext_link(dcp);
	if (dcp->hdmi_hpd_irq && !dcp->hib_irq_disabled)
		disable_irq(dcp->hdmi_hpd_irq);
	dcp->hib_irq_disabled = false;
	__dcp_enable_dp2hdmi_hpd(dcp, true);
}

/*
 * The link connected again in the order the dual-pipe pairing needs: a
 * processor without a display at once, one with a display once every
 * external processor's firmware runs and every idle one is done (see the
 * overview above).  After the restart in the restored kernel, or, in the
 * kernel that hibernated, after a hibernation that failed or a disk=suspend
 * wake (PM_POST_HIBERNATION: every DCP is thawed, userspace runs).
 */
static void dcp_hib_ext_link_up(struct apple_dcp *dcp, const char *when)
{
	bool display = dcp_hib_ext_has_display(dcp);

	dcp_fabric_hybrid_woken(dcp);
	dcp_hib_ext_put(dcp, true, display);
	if (display &&
	    !wait_event_timeout(dcp_hib_ext_wq, dcp_hib_ext_settled(),
				DCP_HIB_READY_TIMEOUT))
		dev_warn(dcp->dev,
			 "%s: other display processors not up after 10 s (%d starting, %d idle), connecting anyway\n",
			 when, atomic_read(&dcp_hib_ext_starting),
			 atomic_read(&dcp_hib_ext_idle));

	dcp_hib_ext_relink(dcp);
	dcp_hib_ext_put(dcp, false, true);
	dev_info(dcp->dev,
		 "%s: external link replayed: display %d, route %s, cable %d, DPTX port 0 connected %d\n",
		 when, display,
		 READ_ONCE(dcp->active_typec_route) ? "Type-C" :
		 dcp->fixed_phy ? "fixed" : "none",
		 READ_ONCE(dcp->typec_cable_connected),
		 READ_ONCE(dcp->dptxport[0].connected));
}

/*
 * After dcp_start_desktop(): dcp_wait_ready() without its link step, the
 * DRM entry points opened, then the link (dcp_hib_ext_link_up()).
 */
static void dcp_hib_ext_restore(struct apple_dcp *dcp)
{
	unsigned long left;

	left = wait_for_completion_timeout(&dcp->start_done, DCP_HIB_READY_TIMEOUT);
	if (!left || READ_ONCE(dcp->crashed) || !READ_ONCE(dcp->active)) {
		dcp_hib_give_up(dcp, "iomfb-ready", left ? -ENODEV : -ETIMEDOUT);
		return;
	}
	dev_info(dcp->dev,
		 "hibernation restore: external firmware session up, endpoints %#010x, DPTX ports %d/%d\n",
		 dcp->desktop->graph_started,
		 dptx_service_available(&dcp->dptxport[0]),
		 dptx_service_available(&dcp->dptxport[1]));

	WRITE_ONCE(dcp->hib_phase, DCP_HIB_NONE);
	dcp_hib_ext_link_up(dcp, "hibernation restore");
}

/*
 * The firmware-facing restart prefix shared by the internal and external cold
 * restarts, and by S2R: RUN asserted, RTKit re-initialised, the receive
 * ring released, then dcp_start_desktop() as at probe (RTKit wake/HELLO/EPMAP
 * through set_shmem). Returns 0 with the session starting; on failure it has
 * already called dcp_hib_give_up() and returns the error.
 */
static int dcp_cold_session_start(struct apple_dcp *dcp)
{
	u32 cpu_ctrl;
	int ret;

	dcp_hib_log_coproc(dcp, "firmware restart");
	cpu_ctrl = dcp_coproc_assert_run(dcp);
	if (!(cpu_ctrl & APPLE_DCP_COPROC_CPU_CONTROL_RUN))
		dcp_hib_log_coproc(dcp, "CPU was halted, RUN asserted");

	WRITE_ONCE(dcp->hib_phase, DCP_HIB_RESTARTING);
	WRITE_ONCE(dcp->crashed, false);
	ret = apple_rtkit_reinit(dcp->rtk);
	/* Balances the hold at freeze; the new session reads the queued HELLO. */
	if (dcp->hib_rx_held) {
		dcp->hib_rx_held = false;
		apple_rtkit_resume_rx(dcp->rtk);
	}
	if (ret) {
		dcp_hib_give_up(dcp, "rtkit-reinit", ret);
		return ret;
	}

	init_completion(&dcp->start_done);
	ret = dcp_start_desktop(dcp);
	if (ret)
		dcp_hib_give_up(dcp, "start", ret);
	return ret;
}

/*
 * The desktop firmware brought back from a cold stop after a full power loss:
 * the probe-identical session restart (above), the ready wait, then a DRM
 * modeset that re-lights the panel. Factored out of the S4 hibernation restore
 * (dcp_hib_restore_work) so an S2R wake reuses the identical cold restart. The
 * internal panel only; the external link has its own dcp_hib_ext_restore().
 * Lifted out of the hibernation work item so S2R can call it on its own.
 */
static void dcp_desktop_cold_restart(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	int ret;

	if (dcp_cold_session_start(dcp))
		return;
	ret = dcp_wait_ready(pdev, DCP_HIB_READY_TIMEOUT);
	if (ret) {
		dcp_hib_give_up(dcp, "iomfb-ready", ret);
		return;
	}
	dev_info(dcp->dev,
		 "firmware session up, endpoints %#010x, main display %d\n",
		 dcp->desktop->graph_started, dcp->main_display);

	ret = dcp_hib_recommit(dcp);
	if (ret) {
		dcp_hib_give_up(dcp, "modeset", ret);
		return;
	}
	/* What the display got is logged by dcp_hib_recommit(). */
	dev_info(dcp->dev, "firmware back after the cold restart\n");
}

/*
 * The internal DCP after a real sleep, brought back as a restarted DCP: the
 * firmware the stop idled keeps its buffers and rings, answers HELLO, gets its
 * endpoint map echoed and has no endpoint started again
 * (apple_rtkit_power_cycled_return()). Then the modeset that lights the panel.
 * The cold session restart re-inits RTKit and starts every endpoint: after the
 * idle the firmware answered none of those starts (-110 each).
 */
static void dcp_t6041_s2r_warm_restart(struct apple_dcp *dcp)
{
	int ret;

	WRITE_ONCE(dcp->hib_phase, DCP_HIB_RESTARTING);
	WRITE_ONCE(dcp->crashed, false);
	ret = apple_rtkit_power_cycled_return(dcp->rtk,
					      dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	if (dcp->hib_rx_held) {
		dcp->hib_rx_held = false;
		apple_rtkit_resume_rx(dcp->rtk);
	}
	apple_sleep_bringup_mark(DCP_S2R_MARK(0, 0xf));
	dev_info(dcp->dev, "S2R resume: warm restart of the idled firmware: %d (CPU_CONTROL %#x STATUS %#x)\n",
		 ret, readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL),
		 readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS));
	if (ret) {
		dcp_hib_give_up(dcp, "warm-restart", ret);
		return;
	}
	dcp_t6041_s2r_afk(dcp, true);
	/* As the platform powers the framebuffer after the wake: the hard power state back on. */
	ret = iomfb_system_power_v26_6_desktop(dcp, true);
	dev_info(dcp->dev, "S2R resume: system power state 1: %d\n", ret);

	ret = dcp_hib_recommit(dcp);
	if (ret) {
		dcp_hib_give_up(dcp, "modeset", ret);
		return;
	}
	/* What the display got is logged by dcp_hib_recommit(). */
	dev_info(dcp->dev, "firmware back after the warm restart\n");
}

/*
 * An external DCP after a real sleep: the same warm return as the internal one. Its
 * link went at prepare (dcp_hib_prepare()) and its firmware stopped idle with no display,
 * so there is no mode to set: with the firmware back the DRM entry points open and the
 * link is replayed as after a resume from disk, the DP PHY set up again and a display
 * reported by the firmware's hotplug. One that had crashed before the sleep is tried once
 * more, without its old calls.
 */
static void dcp_t6041_s2r_warm_restart_ext(struct apple_dcp *dcp)
{
	int ret;

	dcp_s2r_forget_crash(dcp);
	WRITE_ONCE(dcp->crashed, false);
	ret = apple_rtkit_power_cycled_return(dcp->rtk,
					      dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	if (dcp->hib_rx_held) {
		dcp->hib_rx_held = false;
		apple_rtkit_resume_rx(dcp->rtk);
	}
	dev_info(dcp->dev, "S2R resume: warm restart of the idled firmware: %d (CPU_CONTROL %#x STATUS %#x)\n",
		 ret, readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL),
		 readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS));
	if (ret) {
		dcp_hib_give_up(dcp, "warm-restart", ret);
		return;
	}
	dcp_t6041_s2r_afk(dcp, true);
	ret = iomfb_system_power_v26_6_desktop(dcp, true);
	dev_info(dcp->dev, "S2R resume: system power state 1: %d\n", ret);

	dcp->link_recovery_tries = 0;
	dcp_release_after_sleep(dcp);
	WRITE_ONCE(dcp->hib_phase, DCP_HIB_NONE);
	dcp_hib_ext_link_up(dcp, "S2R wake");
	dev_info(dcp->dev, "external firmware back after the warm restart\n");
}

static void dcp_hib_restore_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(work, struct apple_dcp,
					     hib_restore_work);

	/*
	 * An S2R wake reuses this work item for the cold restart: the
	 * internal panel shares dcp_desktop_cold_restart; an external DCP has its
	 * own session start + link tail, as the hibernation restore below does.
	 */
	if (dcp->s2r_cold_restart) {
		dcp->s2r_cold_restart = false;
		if (dcp_desktop_external(dcp)) {
			if (READ_ONCE(s2r_warm))
				dcp_t6041_s2r_warm_restart_ext(dcp);
			else if (!dcp_cold_session_start(dcp))
				dcp_hib_ext_restore(dcp);
		} else if (READ_ONCE(s2r_warm)) {
			dcp_t6041_s2r_warm_restart(dcp);
		} else {
			dcp_desktop_cold_restart(dcp);
		}
		return;
	}

	/* The kernel that hibernated: the hibernation failed, or disk=suspend woke. */
	if (READ_ONCE(dcp->hib_phase) == DCP_HIB_NONE && dcp->hib_link_released &&
	    !READ_ONCE(dcp->ext_dead)) {
		dcp_hib_ext_link_up(dcp, "sleep aborted");
		return;
	}
	if (READ_ONCE(dcp->hib_phase) != DCP_HIB_QUIESCED) {
		dcp_hib_ext_put(dcp, true, true);
		return;
	}

	/* The external link has its own tail; the internal panel shares the cold restart. */
	if (dcp_desktop_external(dcp)) {
		if (dcp_cold_session_start(dcp))
			return;
		dcp_hib_ext_restore(dcp);
		return;
	}
	dcp_desktop_cold_restart(dcp);
}

/*
 * The firmware restart waits for PM_POST_HIBERNATION: the PMP is rebuilt by
 * its own notifier (priority 10, pmp-export.c), and this one runs after it.
 * PM_POST_RESTORE: a restore through the snapshot device.  In the kernel
 * that hibernated, PM_POST_HIBERNATION comes only after a failure or a
 * disk=suspend wake: the link given back at prepare is replayed then.
 * PM_POST_SUSPEND: a T6040 S2R gives the link back at prepare as well, and
 * one that failed before the firmware stopped has no wake to replay it.
 * Before the next system sleep the restart work has finished.
 */
static int dcp_hib_pm_notify(struct notifier_block *nb, unsigned long action,
			     void *unused)
{
	struct apple_dcp *dcp = container_of(nb, struct apple_dcp, hib_nb);

	switch (action) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		flush_work(&dcp->hib_restore_work);
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		if (READ_ONCE(dcp->hib_phase) == DCP_HIB_QUIESCED ||
		    (READ_ONCE(dcp->hib_phase) == DCP_HIB_NONE &&
		     dcp->hib_link_released))
			queue_work(system_long_wq, &dcp->hib_restore_work);
		break;
	}
	return NOTIFY_DONE;
}

/*
 * Hibernation prepare (dpm_prepare(), before any freeze callback and before
 * the kernel threads freeze; every DCP's mailbox is live): an external
 * processor stops its link work and gives back its link and DP PHYs.  The
 * PM_HIBERNATION_PREPARE notifier set dcp_hib_in_progress; s2idle never
 * gets here.  The second prepare, for the power-off, finds it done.
 * A T6040 S2R does the same at its prepare (dcp_platform_prepare()): its
 * wake replays the link (dcp_t6041_s2r_warm_restart_ext()).
 */
static int dcp_hib_prepare(struct apple_dcp *dcp)
{
	if (!dcp || dcp_takeover_was_aborted(dcp) || READ_ONCE(dcp->ext_dead))
		return 0;
	if (dcp_hib_busy(dcp)) {
		dev_warn(dcp->dev,
			 "refusing hibernation: firmware restart after the last resume still running\n");
		return -EBUSY;
	}
	if (!dcp_hib_can_restart(dcp) || !dcp_desktop_external(dcp))
		return 0;
	dcp_hib_stop_ext_link_work(dcp);
	if (!dcp->hib_link_released) {
		dcp_hib_release_ext_link(dcp);
		dcp->hib_link_released = true;
	}
	/* Its relink is waited for by the processors with a display. */
	if (!dcp->hib_ext_idle_pending) {
		dcp->hib_ext_idle_pending = true;
		atomic_inc(&dcp_hib_ext_idle);
	}
	return 0;
}

static void dcp_hib_release(void *data)
{
	struct apple_dcp *dcp = data;

	unregister_pm_notifier(&dcp->hib_nb);
	cancel_work_sync(&dcp->hib_restore_work);
	dcp_hib_ext_put(dcp, true, true);
}

static int dcp_hib_register(struct apple_dcp *dcp)
{
	int ret;

	if (dcp->hib_nb_registered)
		return 0;
	dcp->hib_nb.notifier_call = dcp_hib_pm_notify;
	dcp->hib_nb.priority = -5;	/* after PMP (10), before the GPU drain (-10) */
	ret = register_pm_notifier(&dcp->hib_nb);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dcp->dev, dcp_hib_release, dcp);
	if (ret)
		return ret;
	dcp->hib_nb_registered = true;
	return 0;
}

static int dcp_platform_freeze(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	/*
	 * The T8132 power-down keeps the retained session, which a
	 * hibernation image cannot carry across the boot that restores it.
	 */
	if (iomfb_is_m4(dcp))
		return -EBUSY;
	if (dcp_takeover_was_aborted(dcp) || READ_ONCE(dcp->ext_dead))
		return 0;
	if (dcp->external)
		return dcp_platform_suspend(dev);
	/* PM_HIBERNATION_PREPARE flushed it; a restart still running is refused. */
	if (dcp_hib_busy(dcp))
		return -EBUSY;
	/* Done at prepare (dcp_hib_prepare()); a no-op then. */
	if (dcp_hib_can_restart(dcp) && dcp_desktop_external(dcp))
		dcp_hib_stop_ext_link_work(dcp);
	if (dcp_hib_can_restart(dcp) && !READ_ONCE(dcp->crashed) &&
	    !dcp->hib_rx_held) {
		apple_rtkit_hold_rx(dcp->rtk);
		apple_rtkit_flush_rx(dcp->rtk);
		dcp->hib_rx_held = true;
	}
	return 0;
}

/*
 * After the image is written.  Remembered so that a .restore in this same
 * kernel (the power-off failed) is told apart from one in the restored
 * kernel, whose image was taken before this ran.
 */
static int dcp_platform_poweroff(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	dcp->hib_poweroff_entered = true;
	return dcp_platform_freeze(dev);
}

static int dcp_platform_thaw(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	/* The image is written, or hibernation failed: the firmware ran on. */
	if (dcp->hib_rx_held) {
		dcp->hib_rx_held = false;
		apple_rtkit_resume_rx(dcp->rtk);
	}
	if (dcp_takeover_was_aborted(dcp) || READ_ONCE(dcp->ext_dead))
		return 0;
	/*
	 * An external processor's link stays given back while the image is
	 * written: PM_POST_HIBERNATION replays it if this kernel goes on.
	 */
	if (dcp->external)
		return dcp_platform_resume(dev);
	return 0;
}

/*
 * External processors drop their old session before any .restore: the
 * Type-C port controllers' .restore may report their mode again
 * (dcp_typec_route_set()), and every firmware call from there must fail
 * at once on the quiesced endpoints rather than wait on a session that is
 * gone.  The panel keeps its .restore (dcp_platform_restore()).
 */
static int dcp_platform_restore_early(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	int ret;

	/*
	 * The power-off failed, or hibernation was aborted before the image
	 * (devices get PMSG_RESTORE then too): the firmware ran on, .restore
	 * thaws.
	 */
	if (!dcp || dcp->hib_poweroff_entered || !pm_hibernate_is_restored())
		return 0;
	if (dcp_takeover_was_aborted(dcp) || READ_ONCE(dcp->ext_dead) ||
	    !dcp_desktop_external(dcp) || !dcp_hib_can_restart(dcp) ||
	    READ_ONCE(dcp->hib_phase) != DCP_HIB_NONE)
		return 0;

	dcp_hib_log_coproc(dcp, "entry");
	if (!dcp->hib_rx_held)
		dev_warn(dev,
			 "hibernation restore: RTKit receive was not held in the image\n");
	ret = dcp_hib_register(dcp);
	if (ret) {
		/* .restore refuses, as for a processor it cannot restart. */
		dev_err(dev, "hibernation restore: no PM notifier (%d)\n", ret);
		return 0;
	}
	/* Host side only: the DMA of the old session goes in .restore. */
	dcp_hib_quiesce(dcp);
	dcp_hib_quiesce_ext(dcp);
	dcp_hib_ext_get(dcp);
	/* Counted at prepare, in the image, unless prepare did not run. */
	if (!dcp->hib_ext_idle_pending) {
		dcp->hib_ext_idle_pending = true;
		atomic_inc(&dcp_hib_ext_idle);
	}
	dev_info(dev,
		 "hibernation restore: old firmware session dropped; restart after PM_POST_HIBERNATION\n");
	return 0;
}

static int dcp_platform_restore(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	int ret;

	/*
	 * The power-off failed, or hibernation was aborted before the image:
	 * the firmware ran on, as after a thaw.
	 */
	if (dcp->hib_poweroff_entered || !pm_hibernate_is_restored()) {
		dev_info(dev, "hibernation %s: firmware session kept\n",
			 dcp->hib_poweroff_entered ? "power-off failed" : "aborted");
		dcp->hib_poweroff_entered = false;
		return dcp_platform_thaw(dev);
	}

	if (dcp_takeover_was_aborted(dcp) || READ_ONCE(dcp->ext_dead))
		return 0;
	/* Dropped at restore_early (dcp_platform_restore_early()); its DMA now. */
	if (dcp_desktop_external(dcp) &&
	    READ_ONCE(dcp->hib_phase) == DCP_HIB_QUIESCED) {
		dcp_hib_release_dma(dcp);
		return 0;
	}

	if (dcp_hib_can_restart(dcp) && !dcp_desktop_external(dcp)) {
		dcp_hib_log_coproc(dcp, "entry");
		if (!dcp->hib_rx_held)
			dev_warn(dev,
				 "hibernation restore: RTKit receive was not held in the image\n");
		ret = dcp_hib_register(dcp);
		if (!ret) {
			dcp_hib_quiesce(dcp);
			dcp_hib_release_dma(dcp);
			dev_info(dev,
				 "hibernation restore: old firmware session dropped; restart after PM_POST_HIBERNATION\n");
			return 0;
		}
		dev_err(dev, "hibernation restore: no PM notifier (%d)\n", ret);
	}

	if (READ_ONCE(dcp->retain_dma) || dcp->hw.adopt_live_session)
		dev_err(dev,
			"hibernation restore: the DCP firmware was restarted by the boot loader; its session was pinned for this boot and cannot be re-adopted without a reboot. Display left to the boot loader, IOMFB traffic stopped\n");
	else
		dev_err(dev,
			"hibernation restore: the DCP firmware was restarted by the boot loader and in-place re-adoption is not implemented for this processor. Display left to the boot loader, IOMFB traffic stopped\n");

	WRITE_ONCE(dcp->ext_dead, true);
	return 0;
}
#endif

static const struct dev_pm_ops dcp_platform_pm_ops = {
	.prepare = pm_sleep_ptr(dcp_platform_prepare),
	.complete = pm_sleep_ptr(dcp_platform_complete),
	.suspend = pm_sleep_ptr(dcp_platform_suspend),
	.resume = pm_sleep_ptr(dcp_platform_resume),
#ifdef CONFIG_HIBERNATION
	.freeze = dcp_platform_freeze,
	.thaw = dcp_platform_thaw,
	.poweroff = dcp_platform_poweroff,
	.restore_early = dcp_platform_restore_early,
	.restore = dcp_platform_restore,
#else
	.freeze = pm_sleep_ptr(dcp_platform_suspend),
	.thaw = pm_sleep_ptr(dcp_platform_resume),
	.poweroff = pm_sleep_ptr(dcp_platform_suspend),
	.restore = pm_sleep_ptr(dcp_platform_resume),
#endif
};

static const struct apple_dcp_hw_data apple_dcp_hw_t6020 = {
	.num_dptx_ports = 1,
	.t6020_tunnel_flow = true,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t6020_dcpext = {
	.num_dptx_ports = 2,
	.t6020_tunnel_flow = true,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t8112 = {
	.num_dptx_ports = 2,
};

/* The internal T8140 endpoint uses the H17P method profile. */
static const struct resource t8140_firmware_clock =
	DEFINE_RES_MEM(0x302800000, 0xbc000);

static const struct apple_dcp_hw_data apple_dcp_hw_t8140 = {
	.num_dptx_ports = 0,
	.adopt_live_session = true,
	.firmware_compat = DCP_FIRMWARE_H17P,
	.firmware_clock = &t8140_firmware_clock,
	.firmware_display_windows = 5,
	.firmware_scratch = 0x20000,
	.firmware_request = 0x68000,
	.s2r = true,
};

/* Separate from the established internal-panel H17P profile. */
static const struct apple_dcp_hw_data apple_dcp_hw_t8140_external = {
	.num_dptx_ports = 1,
	.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL,
	.adopt_live_session = false,
	.firmware_compat = DCP_FIRMWARE_H17P,
	.firmware_clock = &t8140_firmware_clock,
	.firmware_display_windows = 4,
	/* Native PMGR BWR agent 2 and boost agent 3 for DISPEXT0. */
	.firmware_scratch = 0x28000,
	.firmware_request = 0x70000,
	.s2r_ext = true,
};

/*
 * M5 (T8142) runs H17-generation DCP firmware with the H17G method numbering
 * and is left running by the bootloader.
 */
static const struct apple_dcp_hw_data apple_dcp_hw_t8142 = {
	.num_dptx_ports = 0,
	.iomfb_method_profile = DCP_IOMFB_METHODS_H17G,
	.adopt_live_session = true,
	.firmware_compat = DCP_FIRMWARE_H17P,
};

static const struct apple_dcp_hw_data apple_dcp_hw_dcp = {
	.num_dptx_ports = 0,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t6030 = {
	.num_dptx_ports = 1,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t8122 = {
	.num_dptx_ports = 2,
};

/* The internal panel only, through the 14.x IOMFB, as on T6030. */
static const struct apple_dcp_hw_data apple_dcp_hw_t6031 = {
	.num_dptx_ports = 1,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t6030_dcpext = {
	.num_dptx_ports = 1,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t8122_dcpext = {
	.num_dptx_ports = 2,
	.fw_14_7_endpoints = true,
};

static const struct apple_dcp_hw_data apple_dcp_hw_dcpext = {
	.num_dptx_ports = 2,
};

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T8132)
static const struct apple_dcp_hw_data apple_dcp_hw_t8132 = {
	.adopt_live_session = true,
	.firmware_compat = DCP_FIRMWARE_M4,
};

/* An external DCP: two DPTX port services, dcpdptx-port-epic units 0 and 1. */
static const struct apple_dcp_hw_data apple_dcp_hw_t8132_dcpext = {
	.adopt_live_session = true,
	.firmware_compat = DCP_FIRMWARE_M4,
	.num_dptx_ports = 2,
};
#endif

/* T6041 (M4 Pro/Max), desktop 26.6 firmware */
static const struct apple_dcp_hw_data apple_dcp_hw_t6041 = {
	.num_dptx_ports = 2,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t6041_dcpext = {
	.num_dptx_ports = 2,
	.sink_metadata_edid = true,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t6050 = {
	.firmware_dma_bits = 41,
	.num_dptx_ports = 0,
	.adopt_live_session = true,
	.firmware_compat = DCP_FIRMWARE_V_27_0,
	.only_120hz_modes = true,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t6050_dcpext = {
	.firmware_dma_bits = 41,
	.num_dptx_ports = 2,
	.adopt_live_session = true,
	.firmware_compat = DCP_FIRMWARE_V_27_0,
	.sink_metadata_edid = true,
};

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,t6041-dcp", .data = &apple_dcp_hw_t6041, },
	{ .compatible = "apple,t6041-dcpext", .data = &apple_dcp_hw_t6041_dcpext, },
	{ .compatible = "apple,t6050-dcp", .data = &apple_dcp_hw_t6050 },
	{ .compatible = "apple,t6050-dcpext", .data = &apple_dcp_hw_t6050_dcpext },
	{ .compatible = "apple,t6020-dcp", .data = &apple_dcp_hw_t6020,  },
	{ .compatible = "apple,t6020-dcpext", .data = &apple_dcp_hw_t6020_dcpext, },
	{ .compatible = "apple,t8112-dcp", .data = &apple_dcp_hw_t8112,  },
	{ .compatible = "apple,t8140-dcp", .data = &apple_dcp_hw_t8140,  },
	{ .compatible = "apple,t8140-dcpext", .data = &apple_dcp_hw_t8140_external, },
	{ .compatible = "apple,t8142-dcp", .data = &apple_dcp_hw_t8142,  },
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T8132)
	{ .compatible = "apple,t8132-dcp", .data = &apple_dcp_hw_t8132, },
	/* Same firmware as the internal DCP; adopted from the boot loader. */
	{ .compatible = "apple,t8132-dcpext", .data = &apple_dcp_hw_t8132_dcpext, },
#endif
	{ .compatible = "apple,t6030-dcp", .data = &apple_dcp_hw_t6030, },
	{ .compatible = "apple,t6030-dcpext", .data = &apple_dcp_hw_t6030_dcpext, },
	{ .compatible = "apple,t8122-dcp", .data = &apple_dcp_hw_t8122, },
	{ .compatible = "apple,t6031-dcp", .data = &apple_dcp_hw_t6031, },
	{ .compatible = "apple,t8122-dcpext", .data = &apple_dcp_hw_t8122_dcpext, },
	{ .compatible = "apple,dcp",       .data = &apple_dcp_hw_dcp,    },
	{ .compatible = "apple,dcpext",    .data = &apple_dcp_hw_dcpext, },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver apple_platform_driver = {
	.driver_managed_dma = true,
	.probe		= dcp_platform_probe,
	.remove		= dcp_platform_remove,
	.shutdown	= dcp_platform_shutdown,
	.driver	= {
		.name = "apple-dcp",
		.dev_groups = dcp_peer_probe_groups,
		.of_match_table	= of_match,
		.pm = pm_sleep_ptr(&dcp_platform_pm_ops),
	},
};

#ifdef CONFIG_HIBERNATION
/* Ahead of the per-DCP notifiers (priority -5). */
static int dcp_hib_global_notify(struct notifier_block *nb, unsigned long action,
				 void *unused)
{
	switch (action) {
	case PM_HIBERNATION_PREPARE:
		WRITE_ONCE(dcp_hib_in_progress, true);
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		WRITE_ONCE(dcp_hib_in_progress, false);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block dcp_hib_global_nb = {
	.notifier_call = dcp_hib_global_notify,
	.priority = 10,
};
#endif

int __init dcp_register(void)
{
	int ret;

#ifdef CONFIG_HIBERNATION
	ret = register_pm_notifier(&dcp_hib_global_nb);
	if (ret)
		return ret;
#endif
	ret = platform_driver_register(&apple_platform_driver);
#ifdef CONFIG_HIBERNATION
	if (ret)
		unregister_pm_notifier(&dcp_hib_global_nb);
#endif
	return ret;
}

void dcp_unregister(void)
{
	platform_driver_unregister(&apple_platform_driver);
#ifdef CONFIG_HIBERNATION
	unregister_pm_notifier(&dcp_hib_global_nb);
#endif
}

#if IS_ENABLED(CONFIG_DRM_APPLE_DISPLAY_KUNIT_TEST)
#include "dcp_external_audio_test.c"
#endif
