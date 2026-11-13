// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T6050 (M5 Pro) AOP and AOP2: started, and stopped for a system sleep, as macOS does.
 *
 * iBoot loads both firmwares and leaves their CPUs stopped. XNU starts each: the mailbox's receive
 * on, CPU_CONTROL 0 -> 0x10, then RTKit's boot.
 *
 * The AOP's service endpoints open from the firmware's side: after
 * the endpoint starts it sends INIT and GETBUF, the AP answers both, and it announces its rings.
 * For a sleep the AP sends STOP on each and waits for its answer, then AP IDLE; the CPU keeps
 * running.
 *
 * AOP2's service endpoints open from the AP's side: INIT, then
 * the firmware's GETBUF, its two rings, and the AP's START. For a sleep the AP puts one element on
 * each TX ring and waits for its answer, then AP IDLE.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/sleep.h>
#include <linux/suspend.h>

#define AOP_CPU_CONTROL		0x44
#define AOP_CPU_CONTROL_RUN	BIT(4)

#define AFK_TYPE		GENMASK_ULL(63, 48)
#define AFK_INIT		0x80
#define AFK_INIT_ACK		0xa0
#define AFK_GETBUF		0x89
#define AFK_GETBUF_ACK		0xa1
#define AFK_GETBUF_SIZE		GENMASK_ULL(31, 16)	/* 64-byte blocks */
#define AFK_GETBUF_IOVA		GENMASK_ULL(47, 0)
#define AFK_INIT_TX		0x8a
#define AFK_INIT_RX		0x8b
#define AFK_RING_OFFSET		GENMASK_ULL(47, 32)	/* 64-byte blocks */
#define AFK_RING_SIZE		GENMASK_ULL(31, 16)	/* 64-byte blocks */
#define AFK_START		0xa3
#define AFK_START_ACK		0x86
#define AFK_STOP		0xa4
#define AFK_STOP_ACK		0x87
#define AFK_RING		0x85
#define AFK_RING_NEW_WPTR	GENMASK_ULL(31, 16)
#define AFK_BLOCK_SHIFT		6
#define AFK_QE_MAGIC		0x20504f49	/* "IOP " */
#define AFK_QE_SLOT		0x80
#define AOP2_INIT_ARG		0x100

#define AOP_EP_FIRST		0x20
#define AOP_EP_LAST		0x2c

/* macOS's order: these eleven, then 0x20, then 0x22, each waiting for its answer. */
static const u8 t6050_aop_stop_order[] = {
	0x2b, 0x2c, 0x21, 0x2a, 0x24, 0x26, 0x27, 0x25, 0x28, 0x29, 0x23, 0x20, 0x22,
};

/* XNU's START_EP order: five before IOP power is acknowledged, these after. */
static const u8 t6050_aop_late_eps[] = { 0x22, 0x28, 0x2b, 0x2a, 0x27, 0x24, 0x26, 0x29 };

/* XNU's aop-spmi0 setup before AOP starts, register +0x420, bit by bit. */
static const u32 t6050_aop_spmi_420[] = {
	0x1, 0x2000001, 0x2000011, 0x2000031, 0x2000131, 0x2000331, 0x2000731, 0x2001731,
	0x2003731, 0x2803731, 0x3803731, 0x7803731, 0xf803731, 0x1f803731, 0x3f803731, 0x3f813731,
	0x3f833731, 0x3f833771, 0x3f8337f1, 0x3f833ff1, 0x3f833ff1,
};

/* macOS opens them in this order, and stops 0x22 first, then the rest. */
static const u8 t6050_aop2_open_order[] = { 0x20, 0x2c, 0x22, 0x21, 0x23, 0x24 };
static const u8 t6050_aop2_stop_order[] = { 0x22, 0x20, 0x2c, 0x21, 0x23, 0x24 };

struct t6050_aop_ring {
	void *buf;
	size_t size;
	u32 tx_off, tx_size;
	bool tx_ready;
};

struct t6050_aop {
	struct device *dev;
	void __iomem *asc;
	struct apple_rtkit *rtk;
	bool aop2;
	bool registered;
	unsigned long opened;	/* bit (ep - AOP_EP_FIRST): open as its side of the protocol needs */
	unsigned long seen;	/* bit (ep - AOP_EP_FIRST): the firmware sent something */
	struct t6050_aop_ring ring[AOP_EP_LAST - AOP_EP_FIRST + 1];
	unsigned int stopped, stop_errors;
	int idle_ret;
};

static void t6050_aop_crashed(void *cookie, const void *crashlog, size_t size)
{
	struct t6050_aop *aop = cookie;

	dev_err(aop->dev, "firmware crashed\n");
}

static void t6050_aop_send(struct t6050_aop *aop, u8 ep, u64 msg)
{
	int ret = apple_rtkit_send_message(aop->rtk, ep, msg, NULL, false);

	if (ret)
		dev_err(aop->dev, "endpoint %#x: send %#llx: %d\n", ep, msg, ret);
}

static void t6050_aop_getbuf(struct t6050_aop *aop, u8 ep, u64 msg)
{
	size_t size = (size_t)FIELD_GET(AFK_GETBUF_SIZE, msg) << AFK_BLOCK_SHIFT;
	dma_addr_t iova;
	void *buf;

	buf = dmam_alloc_coherent(aop->dev, size, &iova, GFP_KERNEL);
	if (!buf) {
		dev_err(aop->dev, "endpoint %#x: no %#zx byte buffer\n", ep, size);
		return;
	}
	aop->ring[ep - AOP_EP_FIRST].buf = buf;
	aop->ring[ep - AOP_EP_FIRST].size = size;
	t6050_aop_send(aop, ep, FIELD_PREP(AFK_TYPE, AFK_GETBUF_ACK) |
				FIELD_PREP(AFK_GETBUF_IOVA, iova));
	if (!aop->aop2)
		set_bit(ep - AOP_EP_FIRST, &aop->opened);
}

static void t6050_aop_recv(void *cookie, u8 ep, u64 msg)
{
	struct t6050_aop *aop = cookie;
	struct t6050_aop_ring *r;

	if (ep < AOP_EP_FIRST || ep > AOP_EP_LAST)
		return;
	r = &aop->ring[ep - AOP_EP_FIRST];
	set_bit(ep - AOP_EP_FIRST, &aop->seen);
	switch (FIELD_GET(AFK_TYPE, msg)) {
	case AFK_INIT:
		t6050_aop_send(aop, ep, FIELD_PREP(AFK_TYPE, AFK_INIT_ACK));
		break;
	case AFK_GETBUF:
		t6050_aop_getbuf(aop, ep, msg);
		break;
	case AFK_INIT_TX:
		r->tx_off = FIELD_GET(AFK_RING_OFFSET, msg) << AFK_BLOCK_SHIFT;
		r->tx_size = FIELD_GET(AFK_RING_SIZE, msg) << AFK_BLOCK_SHIFT;
		r->tx_ready = true;
		break;
	case AFK_INIT_RX:
		if (aop->aop2)
			t6050_aop_send(aop, ep, FIELD_PREP(AFK_TYPE, AFK_START));
		break;
	case AFK_START_ACK:
		if (aop->aop2)
			set_bit(ep - AOP_EP_FIRST, &aop->opened);
		break;
	default:
		/* The rings' announcements and the firmware's own traffic need no answer. */
		break;
	}
}

static const struct apple_rtkit_ops t6050_aop_rtkit_ops = {
	.flags = APPLE_RTKIT_FIRMWARE_OWNED_LOGS | APPLE_RTKIT_XNU_AOP_MESSAGES,
	.early_eps = { 0x25, 0x2c, 0x20, 0x21, 0x23 },
	.early_ep_count = 5,
	.crashed = t6050_aop_crashed,
	.recv_message = t6050_aop_recv,
};

/* AOP2 starts its endpoints from the AP's side, after boot. */
static const struct apple_rtkit_ops t6050_aop2_rtkit_ops = {
	.flags = APPLE_RTKIT_FIRMWARE_OWNED_LOGS,
	.crashed = t6050_aop_crashed,
	.recv_message = t6050_aop_recv,
};

/* Read only: the DART's enable word and the TCRs of the SIDs the AOP uses, for the log. */
static void t6050_aop_log_dart(struct t6050_aop *aop)
{
	struct device_node *np = of_parse_phandle(aop->dev->of_node, "iommus", 0);
	static const u8 sids[] = { 0, 1, 3, 8, 9, 10, 11, 16 };
	void __iomem *dart;
	unsigned int i;

	if (!np)
		return;
	dart = of_iomap(np, 0);
	of_node_put(np);
	if (!dart)
		return;
	dev_info(aop->dev, "dart-aop: enable %#x\n", readl(dart + 0xc00));
	for (i = 0; i < ARRAY_SIZE(sids); i++)
		dev_info(aop->dev, "dart-aop sid %u: tcr %#x ttbr %#x\n", sids[i],
			 readl(dart + 0x1000 + 4 * sids[i]), readl(dart + 0x1400 + 4 * sids[i]));
	iounmap(dart);
}

/*
 * One element on an AOP2 TX ring: the "IOP " header, then u32 0, 4, 2 (the AP's counter is 0: it
 * writes nothing there before the sleep), the ring laid out as the DCP's: size, read pointer,
 * write pointer, a block each, then the data. Answered with code 3.
 */
static int t6050_aop2_element(struct t6050_aop *aop, u8 ep)
{
	struct t6050_aop_ring *r = &aop->ring[ep - AOP_EP_FIRST];
	u8 *hdr = (u8 *)r->buf + r->tx_off;
	u32 bufsz = le32_to_cpu(READ_ONCE(*(__le32 *)hdr)), stride;
	__le32 *qe;

	if (!r->tx_ready || bufsz >= r->tx_size || (r->tx_size - bufsz) % 3)
		return -EINVAL;
	stride = (r->tx_size - bufsz) / 3;
	qe = (__le32 *)(hdr + 3 * stride);
	qe[0] = cpu_to_le32(AFK_QE_MAGIC);
	qe[1] = cpu_to_le32(0xc);
	qe[2] = 0;
	qe[3] = 0;
	qe[4] = 0;
	qe[5] = cpu_to_le32(4);
	qe[6] = cpu_to_le32(2);
	dma_wmb();
	WRITE_ONCE(*(__le32 *)(hdr + 2 * stride), cpu_to_le32(AFK_QE_SLOT));
	dma_wmb();

	return apple_rtkit_polled_exchange(aop->rtk, ep, FIELD_PREP(AFK_TYPE, AFK_RING) |
					   FIELD_PREP(AFK_RING_NEW_WPTR, AFK_QE_SLOT), AFK_TYPE,
					   FIELD_PREP(AFK_TYPE, AFK_RING));
}

/* The sleep's first pass, interrupts off: the CPU keeps running. */
static int t6050_aop_sleep_stop(void *data)
{
	struct t6050_aop *aop = data;
	unsigned int i, us;

	aop->stopped = aop->stop_errors = 0;
	apple_rtkit_polled_begin(aop->rtk);
	for (i = 0; !aop->aop2 && i < ARRAY_SIZE(t6050_aop_stop_order); i++) {
		u8 ep = t6050_aop_stop_order[i];

		if (!test_bit(ep - AOP_EP_FIRST, &aop->opened))
			continue;
		if (apple_rtkit_polled_exchange(aop->rtk, ep, FIELD_PREP(AFK_TYPE, AFK_STOP), AFK_TYPE,
						FIELD_PREP(AFK_TYPE, AFK_STOP_ACK)))
			aop->stop_errors++;
		else
			aop->stopped++;
	}
	for (i = 0; aop->aop2 && i < ARRAY_SIZE(t6050_aop2_stop_order); i++) {
		u8 ep = t6050_aop2_stop_order[i];

		if (!test_bit(ep - AOP_EP_FIRST, &aop->opened))
			continue;
		if (t6050_aop2_element(aop, ep))
			aop->stop_errors++;
		else
			aop->stopped++;
	}
	aop->idle_ret = apple_rtkit_polled_ap_idle(aop->rtk, &us);
	apple_rtkit_polled_end(aop->rtk);
	dev_info(aop->dev, "sleep stop: endpoints seen %#lx, open %#lx: %u stopped, %u missed, AP IDLE %d in %u us\n",
		 aop->seen, aop->opened, aop->stopped, aop->stop_errors, aop->idle_ret, us);

	return aop->stop_errors || aop->idle_ret;
}

static int t6050_aop_suspend(struct device *dev)
{
	struct t6050_aop *aop = dev_get_drvdata(dev);

	if (!pm_suspend_via_firmware() || !aop->registered)
		return 0;
	apple_sleep_coproc_set_ready(dev, true);

	return 0;
}

static int t6050_aop_resume(struct device *dev)
{
	struct t6050_aop *aop = dev_get_drvdata(dev);
	unsigned int ep;
	int ret;

	if (!aop->registered)
		return 0;
	apple_sleep_coproc_set_ready(dev, false);
	if (!apple_sleep_coproc_stopped(dev))
		return 0;
	/* As macOS after the wake: AP ON, then START on every endpoint. */
	ret = apple_rtkit_ap_power(aop->rtk, true);
	if (ret)
		dev_err(dev, "AP ON not answered: %d\n", ret);
	for_each_set_bit(ep, &aop->opened, AOP_EP_LAST - AOP_EP_FIRST + 1)
		t6050_aop_send(aop, ep + AOP_EP_FIRST, FIELD_PREP(AFK_TYPE, AFK_START));

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(t6050_aop_pm_ops, t6050_aop_suspend, t6050_aop_resume);

static int t6050_aop_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t6050_aop *aop;
	unsigned int i, ep, started = 0;
	u32 control;
	int ret;

	aop = devm_kzalloc(dev, sizeof(*aop), GFP_KERNEL);
	if (!aop)
		return -ENOMEM;
	aop->dev = dev;
	aop->aop2 = of_device_is_compatible(dev->of_node, "apple,t6050-aop2");
	platform_set_drvdata(pdev, aop);

	aop->asc = devm_platform_ioremap_resource_byname(pdev, "asc");
	if (IS_ERR(aop->asc))
		return PTR_ERR(aop->asc);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;

	/* Only a stopped CPU, as iBoot leaves it, is started here. */
	control = readl(aop->asc + AOP_CPU_CONTROL);
	if (control & AOP_CPU_CONTROL_RUN)
		return dev_err_probe(dev, -EBUSY, "already running (control %#x)\n", control);

	/* XNU sets up the AOP's own SPMI controller first. */
	if (!aop->aop2) {
		void __iomem *spmi = devm_platform_ioremap_resource_byname(pdev, "spmi");
		unsigned int n;

		if (IS_ERR(spmi))
			return PTR_ERR(spmi);
		for (n = 0; n < ARRAY_SIZE(t6050_aop_spmi_420); n++)
			writel(t6050_aop_spmi_420[n], spmi + 0x420);
		writel(0x20, spmi + 0x41c);
		writel(0x28, spmi + 0x41c);
	}

	/* The RTKit's mailbox turns its receive on; then the run bit, as XNU. */
	aop->rtk = devm_apple_rtkit_init(dev, aop, "mbox", 0, aop->aop2 ? &t6050_aop2_rtkit_ops :
					 &t6050_aop_rtkit_ops);
	if (IS_ERR(aop->rtk))
		return dev_err_probe(dev, PTR_ERR(aop->rtk), "no RTKit\n");
	writel(AOP_CPU_CONTROL_RUN, aop->asc + AOP_CPU_CONTROL);

	ret = apple_rtkit_boot(aop->rtk);
	if (ret)
		return dev_err_probe(dev, ret, "firmware didn't start\n");

	t6050_aop_log_dart(aop);
	for (i = 0; !aop->aop2 && i < ARRAY_SIZE(t6050_aop_late_eps); i++)
		if (!apple_rtkit_start_ep(aop->rtk, t6050_aop_late_eps[i]))
			started++;
	for (i = 0; aop->aop2 && i < ARRAY_SIZE(t6050_aop2_open_order); i++) {
		ep = t6050_aop2_open_order[i];
		if (apple_rtkit_start_ep(aop->rtk, ep))
			continue;
		started++;
		t6050_aop_send(aop, ep, FIELD_PREP(AFK_TYPE, AFK_INIT) | AOP2_INIT_ARG);
	}

	ret = apple_sleep_coproc_register_first(dev, t6050_aop_sleep_stop, aop);
	if (ret)
		dev_warn(dev, "not told of a system sleep: %d\n", ret);
	else
		aop->registered = true;
	dev_info(dev, "firmware started (control %#x), %u endpoints started\n",
		 readl(aop->asc + AOP_CPU_CONTROL), started);

	return 0;
}

static const struct of_device_id t6050_aop_of_match[] = {
	/* AOP itself is the Rust apple_aop driver's (its setup port); AOP2 stays here. */
	{ .compatible = "apple,t6050-aop2" },
	{}
};

static struct platform_driver t6050_aop_driver = {
	.driver = {
		.name = "t6050-aop",
		.of_match_table = t6050_aop_of_match,
		.pm = pm_sleep_ptr(&t6050_aop_pm_ops),
		.suppress_bind_attrs = true,
	},
	.probe = t6050_aop_probe,
};
builtin_platform_driver(t6050_aop_driver);
