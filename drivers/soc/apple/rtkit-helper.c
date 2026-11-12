// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple Generic RTKit helper coprocessor
 * Copyright The Asahi Linux Contributors
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/rtkit-helper.h>
#include <linux/soc/apple/t8132.h>
#include <linux/string.h>
#include <linux/suspend.h>

#define APPLE_ASC_CPU_CONTROL		0x44
#define APPLE_ASC_CPU_CONTROL_RUN	BIT(4)

/* A running MTP session described by the boot stage that started it. */
#define APPLE_RTKIT_HELPER_HANDOFF	"mtp-live-v1"
#define APPLE_RTKIT_HELPER_MAX_BUFFERS	4
#define APPLE_RTKIT_HELPER_BUFFER_CELLS	5

struct apple_rtkit_helper {
	struct device *dev;
	struct apple_rtkit *rtk;
	/* Inherited from the boot stage; it cannot be restarted. */
	bool adopted;
	/* The producer already began the J713 DockChannel input session. */
	bool preserved;
	/*
	 * The adopted session has a buffer inventory: requests for buffers
	 * it does not list must not replace storage the firmware still owns.
	 */
	bool strict_buffers;
	/* The firmware was told the AP is going to sleep. */
	bool ap_idle;
	/* It was told, gave no answer, and wasn't told otherwise. */
	bool idle_unanswered;
	/* An idle notice that still had no answer when its sleep ended. */
	bool idle_pending;
	/* Ends a sleep whose .prepare failed, which gets no .complete. */
	struct notifier_block pm_nb;
	/* Boot kernel of a resume from disk: RX held for the image. */
	bool rx_held;
	/* This restore took over the session the boot kernel started. */
	bool readopted;
	/* This restore found the co-processor of our own session halted. */
	bool halted_underneath;

	void __iomem *asc_base;

	struct resource *sram;
	void __iomem *sram_base;
};

bool apple_rtkit_helper_preserves_input(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);

	return helper && helper->preserved;
}
EXPORT_SYMBOL_GPL(apple_rtkit_helper_preserves_input);

bool apple_rtkit_helper_readopted(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);

	return helper && helper->readopted;
}
EXPORT_SYMBOL_GPL(apple_rtkit_helper_readopted);

bool apple_rtkit_helper_halted_underneath(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);

	return helper && helper->halted_underneath;
}
EXPORT_SYMBOL_GPL(apple_rtkit_helper_halted_underneath);

static int apple_rtkit_helper_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_rtkit_helper *helper = cookie;
	struct resource res = {
		.start = bfr->iova,
		.end = bfr->iova + bfr->size - 1,
		.name = "rtkit_map",
	};

	/*
	 * An adopted session must keep the buffers negotiated by its first host.
	 * Exact repeats are handled by RTKit before reaching this callback. A new
	 * allocation, or an unrecorded request from a legacy producer, does not
	 * authorize replacing storage the firmware may still use.
	 */
	if (helper->adopted && helper->strict_buffers)
		return -ENODATA;

	if (!bfr->iova) {
		bfr->buffer = dma_alloc_coherent(helper->dev, bfr->size,
						    &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;
		return 0;
	}

	if (!helper->sram) {
		dev_err(helper->dev,
			"RTKit buffer request with no SRAM region: %pR\n", &res);
		return -EFAULT;
	}

	res.flags = helper->sram->flags;

	if (res.end < res.start || !resource_contains(helper->sram, &res)) {
		dev_err(helper->dev,
			"RTKit buffer request outside SRAM region: %pR\n", &res);
		return -EFAULT;
	}

	bfr->iomem = helper->sram_base + (res.start - helper->sram->start);
	bfr->is_mapped = true;

	return 0;
}

static void apple_rtkit_helper_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	// no-op
}

static const struct apple_rtkit_ops apple_rtkit_helper_ops = {
	.shmem_setup = apple_rtkit_helper_shmem_setup,
	.shmem_destroy = apple_rtkit_helper_shmem_destroy,
};

static void apple_rtkit_helper_release(void *data)
{
	struct apple_rtkit_helper *helper = data;

	/* The current session object: a hibernation restore may have replaced it. */
	apple_rtkit_free_retaining_buffers(helper->rtk);
}

/*
 * MTP firmware cannot always be restarted once it has been taken to QUIESCED
 * or SLEEP, so a boot stage that used it hands the session over running. It
 * reserves the buffers the firmware holds and maps them 1:1 through the
 * DART. Only adopt a session it described whose CPU still runs.
 */
static int apple_rtkit_helper_live(struct apple_rtkit_helper *helper)
{
	const char *handoff;
	int ret;

	if (!of_property_present(helper->dev->of_node, "apple,rtkit-handoff"))
		return 0;
	ret = of_property_read_string(helper->dev->of_node, "apple,rtkit-handoff",
				      &handoff);
	if (ret)
		return ret;

	if (strcmp(handoff, APPLE_RTKIT_HELPER_HANDOFF)) {
		dev_warn(helper->dev, "unknown RTKit handoff \"%s\"\n", handoff);
		return -EINVAL;
	}

	if (!(readl_relaxed(helper->asc_base + APPLE_ASC_CPU_CONTROL) &
	      APPLE_ASC_CPU_CONTROL_RUN)) {
		dev_warn(helper->dev, "RTKit handoff without a running CPU\n");
		return -ENODEV;
	}

	return 1;
}

static bool apple_rtkit_helper_reserved(struct device_node *np,
					const struct resource *buffer)
{
	struct device_node *mem;
	int i, count;

	count = of_count_phandle_with_args(np, "memory-region", NULL);
	for (i = 0; i < count; i++) {
		struct reserved_mem *rmem;
		bool contains = false;

		mem = of_parse_phandle(np, "memory-region", i);
		if (!mem)
			return false;
		rmem = of_reserved_mem_lookup(mem);
		if (rmem && of_property_present(mem, "no-map") && rmem->size &&
		    buffer->start >= rmem->base &&
		    resource_size(buffer) <= rmem->size &&
		    buffer->start - rmem->base <= rmem->size - resource_size(buffer))
			contains = true;
		of_node_put(mem);
		if (contains)
			return true;
	}
	return false;
}

static int apple_rtkit_helper_inherit_buffers(
	struct apple_rtkit_helper *helper,
	struct apple_rtkit_inherited_buffer *buffers)
{
	struct device_node *np = helper->dev->of_node;
	struct property *inventory;
	u32 cells[APPLE_RTKIT_HELPER_MAX_BUFFERS * APPLE_RTKIT_HELPER_BUFFER_CELLS];
	int count, i, length, ret;

	/* T8140 producers predating the buffer inventory remain compatible. */
	inventory = of_find_property(np, "apple,rtkit-buffers", &length);
	if (!inventory) {
		if (of_device_is_compatible(np, "apple,t8132-mtp") ||
		    (of_machine_is_compatible("apple,j614s") &&
		     of_device_is_compatible(np, "apple,t6041-mtp")))
			return -EINVAL;
		return 0;
	}
	if (!length)
		return 0;
	count = of_property_count_u32_elems(np, "apple,rtkit-buffers");
	if (count < 0 || count > ARRAY_SIZE(cells) ||
	    count % APPLE_RTKIT_HELPER_BUFFER_CELLS)
		return -EINVAL;
	if (!count)
		return 0;
	ret = of_property_read_u32_array(np, "apple,rtkit-buffers", cells, count);
	if (ret)
		return ret;

	count /= APPLE_RTKIT_HELPER_BUFFER_CELLS;
	for (i = 0; i < count; i++) {
		u32 *tuple = &cells[i * APPLE_RTKIT_HELPER_BUFFER_CELLS];
		struct apple_rtkit_shmem *b = &buffers[i].shmem;
		struct resource res = { .flags = IORESOURCE_MEM };

		/* These are the system endpoints with shared buffers in RTKit. */
		if ((tuple[0] != 1 && tuple[0] != 2 && tuple[0] != 4 && tuple[0] != 8) ||
		    !tuple[3] || tuple[4] > 1)
			return -EINVAL;
		buffers[i].endpoint = tuple[0];
		b->iova = ((u64)tuple[1] << 32) | tuple[2];
		b->size = tuple[3];
		b->is_mapped = tuple[4];
		res.start = b->iova;
		res.end = res.start + b->size - 1;
		if (res.end < res.start || res.end > DMA_BIT_MASK(44))
			return -EINVAL;

		if (helper->sram && resource_contains(helper->sram, &res)) {
			b->iomem = helper->sram_base + res.start - helper->sram->start;
		} else {
			/* DVA == PA is part of this retained-bypass handoff contract. */
			if (!apple_rtkit_helper_reserved(np, &res))
				return -EINVAL;
			b->iomem = devm_ioremap(helper->dev, res.start, resource_size(&res));
			if (!b->iomem)
				return -ENOMEM;
		}
	}
	return count;
}

static int apple_rtkit_helper_pm_register(struct apple_rtkit_helper *helper);

static int apple_rtkit_helper_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_rtkit_helper *helper;
	struct apple_rtkit_inherited_buffer buffers[APPLE_RTKIT_HELPER_MAX_BUFFERS] = {};
	int count = 0, live, ret;

	/* 44 bits for addresses in standard RTKit requests */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(44));
	if (ret)
		return ret;

	helper = devm_kzalloc(dev, sizeof(*helper), GFP_KERNEL);
	if (!helper)
		return -ENOMEM;

	helper->dev = dev;
	platform_set_drvdata(pdev, helper);

	helper->asc_base = devm_platform_ioremap_resource_byname(pdev, "asc");
	if (IS_ERR(helper->asc_base))
		return PTR_ERR(helper->asc_base);

	helper->sram = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	if (helper->sram) {
		helper->sram_base = devm_ioremap_resource(dev, helper->sram);
		if (IS_ERR(helper->sram_base))
			return dev_err_probe(dev, PTR_ERR(helper->sram_base),
					"Failed to map SRAM region\n");
	}

	/* Reject an unknown retained-session contract instead of cold-starting it. */
	live = apple_rtkit_helper_live(helper);
	if (live < 0)
		return dev_err_probe(dev, live, "Invalid live RTKit handoff\n");
	if (live) {
		count = apple_rtkit_helper_inherit_buffers(helper, buffers);
		if (count < 0)
			return dev_err_probe(dev, count, "Invalid inherited RTKit buffers\n");
		if ((apple_t8132_live_mtp() &&
		     of_device_is_compatible(dev->of_node, "apple,t8132-mtp")) ||
		    (of_machine_is_compatible("apple,j614s") &&
		     of_device_is_compatible(dev->of_node, "apple,t6041-mtp")) ||
		    of_device_is_compatible(dev->of_node, "apple,t8142-mtp"))
			helper->preserved = true;
	}

	if (!live && apple_t8132_live_mtp() && of_machine_is_compatible("apple,t8132")) {
		struct property *prop;
		u32 inherited;
		int len;

		prop = of_find_property(dev->of_node,
				"linux-enablement-mac,rtkit-inherited", &len);
		if (prop) {
			if (!len) {
				inherited = 1;
			} else if (len == sizeof(inherited)) {
				ret = of_property_read_u32(dev->of_node,
					"linux-enablement-mac,rtkit-inherited",
					&inherited);
				if (ret || inherited > 1)
					return dev_err_probe(dev, -EINVAL,
						"Invalid inherited helper state\n");
			} else {
				return dev_err_probe(dev, -EINVAL,
						"Invalid inherited helper length\n");
			}
			if (inherited) {
				if (!(readl(helper->asc_base + APPLE_ASC_CPU_CONTROL) &
				      APPLE_ASC_CPU_CONTROL_RUN))
					return dev_err_probe(dev, -ENODEV,
						"Inherited helper is stopped\n");
				helper->preserved = true;
				live = 1;
				dev_warn(dev,
					 "Legacy MTP handoff: servicing notifications without a buffer inventory\n");
			}
		}
	}

	if (live) {
		/*
		 * Set before RX starts: with an inventory, or on T8132, no new
		 * buffer allocation is safe here. Producers that predate the
		 * inventory (T8140) keep mapping IOVAs inside the SRAM region.
		 */
		helper->adopted = true;
		helper->strict_buffers = count > 0 ||
			of_device_is_compatible(dev->of_node, "apple,t8132-mtp") ||
			(of_machine_is_compatible("apple,j614s") &&
			 of_device_is_compatible(dev->of_node, "apple,t6041-mtp"));
		helper->rtk = apple_rtkit_init_adopted_with_buffers(
			dev, helper, NULL, 0, &apple_rtkit_helper_ops, buffers, count);
		if (IS_ERR(helper->rtk))
			return dev_err_probe(dev, PTR_ERR(helper->rtk),
					     "Failed to adopt RTKit\n");

		ret = devm_add_action_or_reset(dev, apple_rtkit_helper_release,
					       helper);
		if (ret)
			return ret;
		ret = apple_rtkit_helper_pm_register(helper);
		if (ret)
			return ret;

		/* There is no safe module-unload boundary for the retained session. */
		__module_get(THIS_MODULE);
		dev_info(dev, "adopted the running RTKit session (%d retained buffers)\n", count);
		return 0;
	}

	helper->rtk =
		devm_apple_rtkit_init(dev, helper, NULL, 0, &apple_rtkit_helper_ops);
	if (IS_ERR(helper->rtk))
		return dev_err_probe(dev, PTR_ERR(helper->rtk),
				     "Failed to initialize RTKit\n");

	writel_relaxed(APPLE_ASC_CPU_CONTROL_RUN,
		       helper->asc_base + APPLE_ASC_CPU_CONTROL);

	/* Works for both wake and boot */
	if (of_device_is_compatible(dev->of_node, "apple,t8142-mtp"))
		ret = apple_rtkit_wake_early_ap(helper->rtk);
	else
		ret = apple_rtkit_wake(helper->rtk);
	if (ret != 0)
		return dev_err_probe(dev, ret, "Failed to wake up coprocessor\n");

	return apple_rtkit_helper_pm_register(helper);
}

static void apple_rtkit_helper_remove(struct platform_device *pdev)
{
	struct apple_rtkit_helper *helper = platform_get_drvdata(pdev);

	/* An inherited session would not come back from QUIESCED. */
	if (helper->adopted || helper->preserved)
		return;

	/*
	 * After a resume from disk the boot loader left the coprocessor halted
	 * with fresh firmware loaded, while the restored host session still
	 * believes it runs: nothing would answer the quiesce. Drop the session
	 * without a handshake so that the next probe starts the CPU again.
	 */
	if (!(readl_relaxed(helper->asc_base + APPLE_ASC_CPU_CONTROL) &
	      APPLE_ASC_CPU_CONTROL_RUN)) {
		dev_info(&pdev->dev, "co-processor halted underneath (resume from disk): removed without quiesce\n");
		return;
	}

	if (apple_rtkit_is_running(helper->rtk))
		apple_rtkit_quiesce(helper->rtk);

	writel_relaxed(0, helper->asc_base + APPLE_ASC_CPU_CONTROL);
}

static int apple_rtkit_helper_ap_power(struct apple_rtkit_helper *helper, bool on)
{
	int ret = apple_rtkit_ap_power(helper->rtk, on);

	if (ret)
		dev_err(helper->dev, "AP power notice (%s) not answered: %d\n",
			on ? "on" : "idle", ret);
	/*
	 * Nothing is sent to undo an unanswered idle notice, which macOS never does:
	 * the end of the sleep looks whether a late answer came, and only then sends AP on.
	 */
	if (ret && !on)
		helper->idle_unanswered = true;
	return ret;
}

static int apple_rtkit_helper_ap_idle(struct apple_rtkit_helper *helper)
{
	int ret;

	if (helper->ap_idle)
		return 0;
	/*
	 * A firmware that never answered the last idle notice still holds it: T8142's MTP
	 * asserts on a second ("Invalid AP power") and takes the keyboard and trackpad with
	 * it. Refuse this sleep instead, unless the answer has come since; then the AP on
	 * the last sleep's end skipped goes first.
	 */
	if (helper->idle_pending) {
		if (apple_rtkit_is_running(helper->rtk)) {
			dev_err(helper->dev, "the last AP idle notice is still unanswered: no sleep\n");
			return -EBUSY;
		}
		helper->idle_pending = false;
		dev_info(helper->dev, "the last AP idle notice was answered late, sending AP on first\n");
		if (apple_rtkit_ap_power(helper->rtk, true)) {
			dev_err(helper->dev, "AP on after the late answer not answered: no sleep\n");
			helper->idle_pending = true;
			return -EBUSY;
		}
	}
	ret = apple_rtkit_helper_ap_power(helper, false);
	if (ret)
		return ret;
	helper->ap_idle = true;
	dev_dbg(helper->dev, "AP idle notice answered for a sleep\n");
	return 0;
}

static void apple_rtkit_helper_ap_on(struct apple_rtkit_helper *helper)
{
	if (!helper->ap_idle)
		return;
	helper->ap_idle = false;
	if (!apple_rtkit_helper_ap_power(helper, true))
		dev_dbg(helper->dev, "AP on notice answered after a sleep\n");
}

/*
 * macOS tells this coprocessor first of all. Told before any driver stops its own,
 * an unanswered notice refuses a sleep that has stopped nothing yet.
 */
static int __maybe_unused apple_rtkit_helper_prepare(struct device *dev)
{
	if (!pm_suspend_via_firmware())
		return 0;
	return apple_rtkit_helper_ap_idle(dev_get_drvdata(dev));
}

static void apple_rtkit_helper_sleep_end(struct apple_rtkit_helper *helper)
{
	if (helper->idle_unanswered) {
		bool late = !apple_rtkit_is_running(helper->rtk);

		helper->idle_unanswered = false;
		dev_info(helper->dev, "after the unanswered idle notice: %s\n",
			 late ? "its answer came late, sending AP on" :
				"still no answer, nothing sent");
		helper->ap_idle = late;
		/* Its answer may still come: the next sleep looks again before sending another. */
		helper->idle_pending = !late;
	}
	apple_rtkit_helper_ap_on(helper);
}

/* A sleep refused before this device's .suspend never calls its .resume. */
static void __maybe_unused apple_rtkit_helper_complete(struct device *dev)
{
	apple_rtkit_helper_sleep_end(dev_get_drvdata(dev));
}

/* The PM core calls no .complete after a failed .prepare: end that sleep here. */
static int apple_rtkit_helper_pm_notify(struct notifier_block *nb, unsigned long action,
					void *data)
{
	struct apple_rtkit_helper *helper = container_of(nb, struct apple_rtkit_helper, pm_nb);

	if (action == PM_POST_SUSPEND && helper->idle_unanswered)
		apple_rtkit_helper_sleep_end(helper);
	return NOTIFY_DONE;
}

static void apple_rtkit_helper_pm_unregister(void *data)
{
	struct apple_rtkit_helper *helper = data;

	unregister_pm_notifier(&helper->pm_nb);
}

static int apple_rtkit_helper_pm_register(struct apple_rtkit_helper *helper)
{
	int ret;

	helper->pm_nb.notifier_call = apple_rtkit_helper_pm_notify;
	ret = register_pm_notifier(&helper->pm_nb);
	if (ret)
		return ret;
	return devm_add_action_or_reset(helper->dev, apple_rtkit_helper_pm_unregister, helper);
}

static int __maybe_unused apple_rtkit_helper_suspend(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);

	/*
	 * A firmware-driven suspend keeps this coprocessor running. It hears the
	 * AP's power state and nothing else, so an adopted session stays live.
	 * That is measured on T8140 and unverified on other SoCs.
	 */
	if (pm_suspend_via_firmware())
		return apple_rtkit_helper_ap_idle(helper);

	if (helper->adopted && pm_suspend_target_state != PM_SUSPEND_TO_IDLE)
		return -EBUSY;
	if (!helper->adopted || !of_machine_is_compatible("apple,j613"))
		return 0;
	if (!(readl(helper->asc_base + APPLE_ASC_CPU_CONTROL) & APPLE_ASC_CPU_CONTROL_RUN))
		return -EHOSTDOWN;
	/* mtp-live-v1 cannot be restarted: keep its ASC suppliers awake. */
	device_set_awake_path(dev);
	dev_dbg(dev, "adopted MTP s2idle: retaining ASC RUN and firmware session\n");
	return 0;
}

static int __maybe_unused apple_rtkit_helper_resume(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);

	/* After a firmware-driven suspend's idle notice; nothing otherwise. */
	apple_rtkit_helper_ap_on(helper);
	if (!helper->adopted || !of_machine_is_compatible("apple,j613"))
		return 0;
	if (!(readl(helper->asc_base + APPLE_ASC_CPU_CONTROL) & APPLE_ASC_CPU_CONTROL_RUN)) {
		dev_err(dev, "adopted MTP lost ASC RUN; refusing firmware restart\n");
		return -EHOSTDOWN;
	}
	dev_dbg(dev, "adopted MTP s2idle: ASC RUN retained, no INIT\n");
	return 0;
}

/*
 * What the boot kernel of a resume from disk leaves for the restored kernel
 * about a session it started itself: the shared buffers the firmware holds.
 */
struct apple_rtkit_helper_hib {
	u32 count;
	u32 reserved;
	struct apple_rtkit_buffer_desc buf[APPLE_RTKIT_BUF_COUNT];
};
static_assert(sizeof(struct apple_rtkit_helper_hib) <= APPLE_RTKIT_HIB_HANDOFF_MAX);

static void apple_rtkit_helper_hib_tag(struct device *dev, char *tag, size_t len)
{
	snprintf(tag, len, "rtkh:%s", dev_name(dev));
}

static bool apple_rtkit_helper_cpu_running(struct apple_rtkit_helper *helper)
{
	return readl_relaxed(helper->asc_base + APPLE_ASC_CPU_CONTROL) &
	       APPLE_ASC_CPU_CONTROL_RUN;
}

/*
 * Hibernation of an adopted session. The image is written while the firmware
 * keeps running; its buffers live in SRAM or in no-map reserved RAM, so they
 * are not part of the image and the host side has nothing to quiesce. The
 * restore re-adopts the instance the boot loader started for the resume boot,
 * exactly as probe adopted the first one.
 *
 * A session this driver started itself is left running as well: the MTP
 * firmware does not come back from QUIESCED (it does not answer the endpoint
 * discovery of the next start). When this is the boot kernel of a resume from
 * disk (it started MTP so that the disk passphrase can be typed), it hands
 * the running firmware to the image: mailbox RX is held, so that what the
 * firmware sends now stays queued for the restored kernel, the messages
 * already received are handled, and the shared buffers are described for the
 * restored kernel, which can only take the session over if its own buffers
 * sit at the same IOVAs (the firmware keeps writing to the IOVAs it was
 * given, and after the restore the DART translates them through the image's
 * page tables).
 */
static int apple_rtkit_helper_freeze(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);
	struct apple_rtkit_helper_hib rec = {};
	char tag[32];
	int n, ret;

	helper->readopted = false;
	helper->halted_underneath = false;
	if (helper->adopted || !apple_rtkit_hib_restoring())
		return 0;
	if (!apple_rtkit_helper_cpu_running(helper) ||
	    !apple_rtkit_is_running(helper->rtk))
		return 0;

	apple_rtkit_hold_rx(helper->rtk);
	apple_rtkit_flush_rx(helper->rtk);
	helper->rx_held = true;

	n = apple_rtkit_describe_buffers(helper->rtk, rec.buf, ARRAY_SIZE(rec.buf));
	if (n < 0) {
		dev_warn(dev, "resume from disk: cannot describe the RTKit buffers: %d\n", n);
		return 0;
	}
	rec.count = n;
	apple_rtkit_helper_hib_tag(dev, tag, sizeof(tag));
	ret = apple_rtkit_hib_handoff_put(tag, &rec, sizeof(rec));
	if (ret)
		dev_warn(dev, "resume from disk: no hand-off record for the image: %d\n", ret);
	else
		dev_dbg(dev, "resume from disk: RTKit session left running for the image (%d buffers)\n",
			n);
	return 0;
}

static int apple_rtkit_helper_thaw(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);
	char tag[32];

	/* The jump into the image failed: this kernel keeps the session. */
	if (helper->rx_held) {
		helper->rx_held = false;
		apple_rtkit_helper_hib_tag(dev, tag, sizeof(tag));
		apple_rtkit_hib_handoff_drop(tag);
		apple_rtkit_resume_rx(helper->rtk);
	}
	return 0;
}

/*
 * A session this driver started itself, after a resume from disk. Taken over
 * in place when the boot kernel left it running and its buffers match ours;
 * otherwise left as before (stale until the driver is reloaded).
 */
static int apple_rtkit_helper_restore_own(struct apple_rtkit_helper *helper)
{
	struct device *dev = helper->dev;
	struct apple_rtkit_helper_hib rec = {}, own = {};
	char tag[32];
	int i, n, ret;

	apple_rtkit_helper_hib_tag(dev, tag, sizeof(tag));
	ret = apple_rtkit_hib_handoff_take(tag, &rec, sizeof(rec));
	if (ret < 0) {
		if (apple_rtkit_helper_cpu_running(helper)) {
			dev_warn(dev, "resume from disk: co-processor runs without a hand-off from the boot kernel (%d): not taken over\n",
				 ret);
		} else {
			helper->halted_underneath = true;
			dev_info(dev, "resume from disk: self-started RTKit session left for a reload\n");
		}
		return 0;
	}
	if (!apple_rtkit_helper_cpu_running(helper)) {
		helper->halted_underneath = true;
		dev_warn(dev, "resume from disk: hand-off present but the co-processor is halted: not taken over\n");
		return 0;
	}

	n = apple_rtkit_describe_buffers(helper->rtk, own.buf, ARRAY_SIZE(own.buf));
	own.count = n < 0 ? 0 : n;
	if (n < 0 || rec.count > ARRAY_SIZE(rec.buf) || own.count != rec.count ||
	    memcmp(own.buf, rec.buf, rec.count * sizeof(rec.buf[0]))) {
		dev_err(dev, "resume from disk: RTKit buffers of the boot kernel differ from the image's: not taken over\n");
		for (i = 0; i < min_t(u32, rec.count, ARRAY_SIZE(rec.buf)); i++)
			dev_err(dev, "  boot  kind %u iova %#llx size %#llx\n",
				rec.buf[i].kind, rec.buf[i].iova, rec.buf[i].size);
		for (i = 0; i < own.count; i++)
			dev_err(dev, "  image kind %u iova %#llx size %#llx\n",
				own.buf[i].kind, own.buf[i].iova, own.buf[i].size);
		return 0;
	}

	ret = apple_rtkit_readopt(helper->rtk);
	if (ret) {
		dev_err(dev, "resume from disk: RTKit session not taken over: %d\n", ret);
		return 0;
	}
	helper->readopted = true;
	dev_dbg(dev, "resume from disk: took over the RTKit session the boot kernel started (%d buffers at the same IOVAs)\n",
		n);
	return 0;
}

static int apple_rtkit_helper_restore(struct device *dev)
{
	struct apple_rtkit_helper *helper = dev_get_drvdata(dev);
	struct apple_rtkit_inherited_buffer buffers[APPLE_RTKIT_HELPER_MAX_BUFFERS] = {};
	struct apple_rtkit *old, *rtk;
	int count, live;

	/* Hibernation aborted before the image (PMSG_RESTORE then too): nothing restarted. */
	if (!pm_hibernate_is_restored())
		return apple_rtkit_helper_thaw(dev);

	if (!helper->adopted)
		return apple_rtkit_helper_restore_own(helper);

	/*
	 * The restored tree still carries the handoff the image's boot stage
	 * wrote. The resume boot's loader hands the same firmware over at the
	 * same SRAM and no-map addresses (the inventory comes from the firmware
	 * carveouts, not from the kernel allocator), so re-read it and check
	 * that a CPU is running behind it.
	 */
	live = apple_rtkit_helper_live(helper);
	if (live < 0) {
		dev_err(dev, "hibernation restore: invalid live RTKit handoff: %d\n", live);
		return live;
	}
	if (!live) {
		dev_err(dev, "hibernation restore: no live RTKit session to re-adopt\n");
		return -ENODEV;
	}
	count = apple_rtkit_helper_inherit_buffers(helper, buffers);
	if (count < 0) {
		dev_err(dev, "hibernation restore: invalid inherited RTKit buffers: %d\n", count);
		return count;
	}

	rtk = apple_rtkit_init_adopted_with_buffers(dev, helper, NULL, 0,
						    &apple_rtkit_helper_ops, buffers, count);
	if (IS_ERR(rtk)) {
		dev_err(dev, "hibernation restore: failed to re-adopt RTKit: %pe\n", rtk);
		return PTR_ERR(rtk);
	}

	/* The old object described an instance that no longer exists. */
	old = helper->rtk;
	helper->rtk = rtk;
	helper->strict_buffers = count > 0 ||
		of_device_is_compatible(dev->of_node, "apple,t8132-mtp");
	apple_rtkit_free_retaining_buffers(old);
	dev_dbg(dev, "re-adopted the running RTKit session after hibernation (%d retained buffers)\n",
		count);
	return 0;
}

static const struct dev_pm_ops apple_rtkit_helper_pm_ops = {
	.prepare = pm_sleep_ptr(apple_rtkit_helper_prepare),
	.complete = pm_sleep_ptr(apple_rtkit_helper_complete),
	.suspend = pm_sleep_ptr(apple_rtkit_helper_suspend),
	.resume = pm_sleep_ptr(apple_rtkit_helper_resume),
	.freeze = pm_sleep_ptr(apple_rtkit_helper_freeze),
	.poweroff = pm_sleep_ptr(apple_rtkit_helper_freeze),
	.thaw = pm_sleep_ptr(apple_rtkit_helper_thaw),
	.restore = pm_sleep_ptr(apple_rtkit_helper_restore),
};

static const struct of_device_id apple_rtkit_helper_of_match[] = {
	{ .compatible = "apple,rtk-helper-asc4" },
	/* J613/J615 25G83 DTs omit the generic fallback (see t8122-j613-25g83.dts). */
	{ .compatible = "apple,t8122-rtk-helper-asc4" },
	{},
};
MODULE_DEVICE_TABLE(of, apple_rtkit_helper_of_match);

static struct platform_driver apple_rtkit_helper_driver = {
	.driver = {
		.name = "rtkit-helper",
		.of_match_table = apple_rtkit_helper_of_match,
		/* The live MTP input session is announced only once per boot. */
		.suppress_bind_attrs = true,
		.pm = pm_sleep_ptr(&apple_rtkit_helper_pm_ops),
	},
	.probe = apple_rtkit_helper_probe,
	.remove = apple_rtkit_helper_remove,
};
module_platform_driver(apple_rtkit_helper_driver);

MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple RTKit helper driver");
