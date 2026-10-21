// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T6030 (M3 Pro: J514S, J516S) power votes through pmp-core.rs's RTKit session.
 *
 * The display is inherited running from the boot loader. Before the PMP CPU
 * runs, its power chain is held and the PMP's DISP|ANS request is seeded in
 * the PTD, so the PMP keeps it on; the DCP driver then waits for the PMP to
 * acknowledge it. The GPU's AGX vote is an untagged RTKit request on the PMP
 * endpoint: exactly one is in flight, with a one-second deadline. Votes and
 * supplier links are retained until reboot. Malformed, unsolicited, late or
 * duplicate replies and firmware crashes latch a sticky error.
 */
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>
#include <linux/soc/apple/pmp-display.h>

/* AGX: command 0x0f, logical device 5, state 1. The reply carries no tag. */
#define PMP_AGX_REQUEST		0x0020f50100000000ULL
#define PMP_POWER_REPLY		0x0021000000000000ULL
#define PMP_ENDPOINT		0x20

enum pmp_power_kind {
	PMP_POWER_NONE,
	PMP_POWER_AGX,
	PMP_POWER_COUNT,
};

/* Caller holds the bridge lock. Errors permanently invalidate untagged replies. */
struct pmp_power_vote {
	int error;
	enum pmp_power_kind pending;
	bool held[PMP_POWER_COUNT];
};

static void pmp_vote_fail(struct pmp_power_vote *s, int error)
{
	if (!s->error)
		s->error = error < 0 ? error : -EIO;
	s->pending = PMP_POWER_NONE;
}

/* One means send the request, zero means the same retained vote is held. */
static int pmp_vote_begin(struct pmp_power_vote *s, enum pmp_power_kind kind)
{
	if (kind <= PMP_POWER_NONE || kind >= PMP_POWER_COUNT)
		return -EINVAL;
	if (s->error)
		return s->error;
	if (s->pending)
		return -EBUSY;
	if (s->held[kind])
		return 0;
	s->pending = kind;
	return 1;
}

static void pmp_vote_reply(struct pmp_power_vote *s, u8 endpoint, u64 word)
{
	if (s->error)
		return;
	if (!s->pending || endpoint != PMP_ENDPOINT || word != PMP_POWER_REPLY) {
		pmp_vote_fail(s, -EPROTO);
		return;
	}
	s->held[s->pending] = true;
	s->pending = PMP_POWER_NONE;
}

struct apple_pmp_display {
	struct device *dev;
	struct device_driver *driver;
	const void *cookie;
	int (*send)(const void *, u64);
	struct mutex request;
	spinlock_t lock;
	struct completion reply;
	struct pmp_power_vote vote;
	unsigned long deadline;
	struct device_link *link[PMP_POWER_COUNT];
};

/* One qualified supplier; published once, never freed during its epoch. */
static struct apple_pmp_display *pmp_display;
static DEFINE_MUTEX(pmp_display_registration);

bool apple_pmp_display_retained_board(void)
{
	return apple_pmp_t6030_board();
}

/* The board's PMP node, added by the T6030 PMP handoff (no alias). */
static struct device_node *pmp_bridge_node(void)
{
	return of_find_node_by_path("/soc/pmp@350500000");
}

struct apple_pmp_display *apple_pmp_display_register(struct device *dev,
		const void *cookie, int (*send)(const void *, u64))
{
	struct apple_pmp_display *p = NULL;
	struct device_node *node;
	bool match;

	if (!apple_pmp_t6030_board() || !dev->of_node ||
	    !of_device_is_compatible(dev->of_node, "apple,t6030-pmp-v2"))
		return NULL;
	node = pmp_bridge_node();
	match = node == dev->of_node;
	of_node_put(node);
	/* The handoff copies the soc-device table from the ADT; require it. */
	if (!match || !of_property_present(dev->of_node, "apple,tunable-soc-device") ||
	    !cookie || !send)
		return NULL;
	mutex_lock(&pmp_display_registration);
	if (pmp_display)
		goto out;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		goto out;
	p->dev = get_device(dev);
	p->driver = dev->driver;
	p->cookie = cookie;
	p->send = send;
	mutex_init(&p->request);
	spin_lock_init(&p->lock);
	init_completion(&p->reply);
	/* The bridge has no teardown: keep it and the PMP until reboot. */
	__module_get(THIS_MODULE);
	pm_runtime_get_noresume(dev);
	smp_store_release(&pmp_display, p);
	dev_info(dev, "J514S PMP vote bridge ready; retained existing PMP owner\n");
out:
	mutex_unlock(&pmp_display_registration);
	return p;
}

void apple_pmp_display_fail(struct apple_pmp_display *p, int error)
{
	unsigned long flags;

	if (!p)
		return;
	spin_lock_irqsave(&p->lock, flags);
	pmp_vote_fail(&p->vote, error);
	spin_unlock_irqrestore(&p->lock, flags);
	complete_all(&p->reply);
}

bool apple_pmp_display_receive(struct apple_pmp_display *p, u8 endpoint, u64 word)
{
	unsigned long flags;

	if (!p || ((word >> 48) & 0xff) != 0x21)
		return false; /* Normal registry traffic belongs to pmp-core.rs. */
	spin_lock_irqsave(&p->lock, flags);
	if (p->vote.pending && time_after_eq(jiffies, p->deadline))
		pmp_vote_fail(&p->vote, -ETIMEDOUT);
	else
		pmp_vote_reply(&p->vote, endpoint, word);
	spin_unlock_irqrestore(&p->lock, flags);
	complete_all(&p->reply);
	return true; /* This is a reply; never send an ACK for it. */
}

static struct apple_pmp_display *pmp_power_consumer(struct device *consumer,
						    enum pmp_power_kind kind)
{
	struct apple_pmp_display *p = smp_load_acquire(&pmp_display);
	struct device_node *pmp, *supplier;
	bool match;

	/* The AGX vote only, for the GPU node linked to the PMP. */
	if (!apple_pmp_t6030_board() || kind != PMP_POWER_AGX || !consumer->of_node ||
	    !of_device_is_compatible(consumer->of_node, "apple,agx-t6030"))
		return ERR_PTR(-ENODEV);
	pmp = of_parse_phandle(consumer->of_node, "apple,pmp", 0);
	supplier = pmp_bridge_node();
	match = pmp && supplier == pmp;
	of_node_put(pmp);
	of_node_put(supplier);
	if (!match)
		return ERR_PTR(-ENODEV);
	return p ? p : ERR_PTR(-EPROBE_DEFER);
}

/* Called during consumer probe, so device-link publication cannot race suspend. */
static int pmp_power_prepare(struct device *consumer, enum pmp_power_kind kind)
{
	struct apple_pmp_display *p = pmp_power_consumer(consumer, kind);
	struct platform_device *pdev;
	struct device_node *np;
	struct device_link *link;
	int ret = -EPROBE_DEFER;

	if (IS_ERR(p) && PTR_ERR(p) != -EPROBE_DEFER)
		return PTR_ERR(p);
	np = pmp_bridge_node();
	if (!np)
		return -ENODEV;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -EPROBE_DEFER;
	device_lock(&pdev->dev);
	if (!pdev->dev.driver || strcmp(pdev->dev.driver->name, "apple_pmp") ||
	    !dev_get_drvdata(&pdev->dev))
		goto out;
	p = smp_load_acquire(&pmp_display);
	if (p && p->link[kind]) {
		ret = 0;
		goto out;
	}
	/* Allocate the fallible link before publishing/pinning the lazy provider. */
	link = device_link_add(consumer, &pdev->dev, DL_FLAG_STATELESS);
	if (!link) {
		ret = -ENOMEM;
		goto out;
	}
	/* Rust checks the actual drvdata type; no provider exists before full binding. */
	p = apple_pmp_display_bound_owner(&pdev->dev);
	if (!p) {
		device_link_del(link);
		ret = -ENODEV;
		goto out;
	}
	mutex_lock(&p->request);
	p->link[kind] = link;
	ret = 0;
	mutex_unlock(&p->request);
out:
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}

static int pmp_power_check(struct device *consumer, enum pmp_power_kind kind)
{
	struct apple_pmp_display *p = pmp_power_consumer(consumer, kind);
	unsigned long flags;
	int ret;

	if (IS_ERR(p))
		return PTR_ERR(p);
	spin_lock_irqsave(&p->lock, flags);
	ret = p->vote.error ? p->vote.error : p->vote.held[kind] ? 0 : -ENODEV;
	spin_unlock_irqrestore(&p->lock, flags);
	return ret;
}

static int pmp_power_get(struct device *consumer, enum pmp_power_kind kind)
{
	struct apple_pmp_display *p = pmp_power_consumer(consumer, kind);
	u64 request = PMP_AGX_REQUEST;
	unsigned long flags, now, deadline;
	int ret;

	if (IS_ERR(p))
		return PTR_ERR(p);
	device_lock(p->dev);
	mutex_lock(&p->request);
	if (p->dev->driver != p->driver || !dev_get_drvdata(p->dev) || !p->link[kind]) {
		ret = -ENODEV;
		goto failed;
	}
	spin_lock_irqsave(&p->lock, flags);
	/* Reset before publishing pending: an immediate reply must not be lost. */
	reinit_completion(&p->reply);
	ret = pmp_vote_begin(&p->vote, kind);
	deadline = p->deadline = jiffies + msecs_to_jiffies(1000);
	spin_unlock_irqrestore(&p->lock, flags);
	if (ret <= 0)
		goto out;
	/* send() releases the Rust RTKit mutex before we wait for reception. */
	ret = p->send(p->cookie, request);
	if (ret)
		goto failed;
	now = jiffies;
	if (time_after_eq(now, deadline) ||
	    !wait_for_completion_timeout(&p->reply, deadline - now)) {
		ret = -ETIMEDOUT;
		goto failed;
	}
	ret = pmp_power_check(consumer, kind);
	if (ret)
		goto failed;
	dev_info(p->dev, "Retained AGX enable accepted: request=%016llx reply=%016llx\n",
		 request, PMP_POWER_REPLY);
	goto out;
failed:
	apple_pmp_display_fail(p, ret);
out:
	mutex_unlock(&p->request);
	device_unlock(p->dev);
	return ret;
}

int apple_pmp_j514s_gpu_prepare(struct device *consumer)
{
	return pmp_power_prepare(consumer, PMP_POWER_AGX);
}
EXPORT_SYMBOL_GPL(apple_pmp_j514s_gpu_prepare);

int apple_pmp_j514s_gpu_get(struct device *consumer)
{
	return pmp_power_get(consumer, PMP_POWER_AGX);
}
EXPORT_SYMBOL_GPL(apple_pmp_j514s_gpu_get);

int apple_pmp_j514s_gpu_check(struct device *consumer)
{
	return pmp_power_check(consumer, PMP_POWER_AGX);
}
EXPORT_SYMBOL_GPL(apple_pmp_j514s_gpu_check);

/*
 * J514S (T6030). PTD at 0x3503c0000: status +0x10; DISP request word +280*16
 * and ACK +284*16 (PTD ranges 10/11); non-posted request-set aperture
 * +0x10000 + 280*8. DISP is table index 7, ANS index 16.
 */
#define J514S_PTD		0x3503c0000ULL
#define J514S_PTD_SIZE		0x14000
#define J514S_PTD_STATUS	0x10
#define J514S_PTD_REQ		(280 * 16)
#define J514S_PTD_ACK		(284 * 16)
#define J514S_PTD_SET		(0x10000 + 280 * 8)
#define J514S_PTD_DISP		BIT_ULL(7)
#define J514S_PTD_ANS		BIT_ULL(16)
/*
 * Memory floor: CPU-client EACC0/PACC0 PTD indices 288/289 request DCS4 and
 * FAB2 (valid bit 61, DCS bits 51:48, FAB bits 35:32), other fields kept.
 * DVFS state entries 8 and 10 (PTD range 3) publish the firmware's response.
 */
#define J514S_PTD_FLOOR_IDX0	288
#define J514S_PTD_FLOOR_MASK	(BIT_ULL(61) | GENMASK_ULL(51, 48) | GENMASK_ULL(35, 32))
#define J514S_PTD_FLOOR		(BIT_ULL(61) | (4ULL << 48) | (2ULL << 32))
#define J514S_PTD_FLOOR_SET(i)	(0x10000 + (J514S_PTD_FLOOR_IDX0 + (i)) * 8)
#define J514S_PTD_DVFS_STATE(i)	((8 + 2 * (i)) * 16)
#define J514S_PMP_ASC		0x350c00000ULL
#define J514S_PMGR		0x350700000ULL

/* SYS, FE and CPU of the display power chain, parents first. */
static const u32 j514s_display_ps[] = { 0x1c0, 0x258, 0x10000 };

/*
 * Hold the inherited display epoch: minimum state 15 (bits 19:16), W1C history
 * bits 9:8 written as zero, other controls preserved. Must precede PMP RUN,
 * or the PMP powers the display down and the DCP CPU stops.
 */
static int j514s_display_hold(struct device *dev)
{
	void __iomem *pmgr = ioremap_np(J514S_PMGR, 0x14000);
	int ret = 0;
	u32 val;

	if (!pmgr)
		return -ENOMEM;
	for (unsigned int i = 0; i < ARRAY_SIZE(j514s_display_ps); i++) {
		val = readl(pmgr + j514s_display_ps[i]);
		if ((val & 0xff) != 0xff || (val & BIT(31))) {
			dev_err(dev, "J514S display domain %#x not inherited on: %#x\n",
				j514s_display_ps[i], val);
			ret = -EIO;
			break;
		}
		writel((val & ~0x000f0300) | 0x000f0000, pmgr + j514s_display_ps[i]);
		val = readl(pmgr + j514s_display_ps[i]);
		dev_info(dev, "J514S display domain %#x held: %#x\n", j514s_display_ps[i], val);
		/* Not every domain implements the minimum field (SYS reads 0). */
		if ((val & 0x000f0000) != 0x000f0000)
			dev_warn(dev, "J514S display domain %#x minimum not latched\n",
				 j514s_display_ps[i]);
	}
	iounmap(pmgr);
	return ret;
}

int apple_pmp_display_pre_run(struct device *dev)
{
	void __iomem *asc = NULL, *ptd = NULL;
	u64 status, req;
	u32 run;
	int ret;

	if (!apple_pmp_t6030_board() || !dev->of_node ||
	    !of_device_is_compatible(dev->of_node, "apple,t6030-pmp-v2"))
		return 0;
	/* The display floor first: the DCP must survive PMP startup. */
	ret = j514s_display_hold(dev);
	if (ret)
		return ret;
	asc = ioremap_np(J514S_PMP_ASC, 0x4000);
	ptd = ioremap_np(J514S_PTD, J514S_PTD_SIZE);
	if (!asc || !ptd) {
		ret = -ENOMEM;
		goto out;
	}
	run = readl(asc + 0x44);
	status = readq(ptd + J514S_PTD_STATUS);
	req = readq(ptd + J514S_PTD_REQ);
	/* Cold start only: no adoption of a running or pre-seeded PMP. */
	if ((run & BIT(4)) || status || req) {
		dev_err(dev, "J514S PMP not cold (run=%#x status=%#llx req=%#llx)\n",
			run, status, req);
		ret = -EBUSY;
		goto out;
	}
	/* One 64-bit set transaction; the readback must be exactly DISP|ANS. */
	writeq(J514S_PTD_DISP | J514S_PTD_ANS, ptd + J514S_PTD_SET);
	req = readq(ptd + J514S_PTD_REQ);
	if (req != (J514S_PTD_DISP | J514S_PTD_ANS)) {
		dev_err(dev, "J514S PTD seed not accepted: req=%#llx\n", req);
		ret = -EIO;
		goto out;
	}
	dev_info(dev, "J514S display held, PTD seeded DISP|ANS: req=%#llx\n", req);
out:
	if (ptd)
		iounmap(ptd);
	if (asc)
		iounmap(asc);
	return ret;
}

static bool j514s_memory_floor_enabled = true;
module_param_named(j514s_memory_floor, j514s_memory_floor_enabled, bool, 0444);
MODULE_PARM_DESC(j514s_memory_floor, "J514S: request the DCS4/FAB2 memory floor once the PMP runs");

/*
 * The PMP applies a CPU-client request on its change notification, and a read
 * of the request entry may consume that notification. A floor written and
 * echoed at once stayed latched without effect: DVFS state 0x100, CPU copy
 * bandwidth at the no-floor level. So, from a delayed work once the PMP has
 * settled: clear both requests (the floor is always a fresh transition),
 * write both floors back to back, and confirm on the DVFS state entries
 * without reading the requests; retry if the firmware did not respond.
 */
#define J514S_FLOOR_SETTLE_MS	100
#define J514S_FLOOR_TRIES	10

static void j514s_memory_floor_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(j514s_floor_work, j514s_memory_floor_work);
static atomic_t j514s_floor_started = ATOMIC_INIT(0);
static unsigned int j514s_floor_try;
static u64 j514s_floor_prior[2];

/*
 * The floor is for CPU and GPU memory throughput, but the PMP also keeps it
 * when idle: with it the fabric and the DCS stay at FAB 5 / DCS 8, without it
 * they settle at FAB 3 / DCS 4 as on macOS. Withdraw it for system sleep and
 * request it again, as a fresh transition, on resume.
 */
static void j514s_memory_floor_set(bool on)
{
	void __iomem *ptd = ioremap_np(J514S_PTD, J514S_PTD_SIZE);

	if (!ptd)
		return;
	if (readq(ptd + J514S_PTD_STATUS) == 1) {
		for (u32 i = 0; i < 2; i++)
			writeq(j514s_floor_prior[i], ptd + J514S_PTD_FLOOR_SET(i));
		if (on) {
			msleep(J514S_FLOOR_SETTLE_MS);
			for (u32 i = 0; i < 2; i++)
				writeq(j514s_floor_prior[i] | J514S_PTD_FLOOR,
				       ptd + J514S_PTD_FLOOR_SET(i));
		}
	}
	iounmap(ptd);
}

static int j514s_memory_floor_pm(struct notifier_block *nb, unsigned long action,
				 void *data)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
		j514s_memory_floor_set(false);
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
		j514s_memory_floor_set(true);
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block j514s_floor_pm_nb = {
	.notifier_call = j514s_memory_floor_pm,
};

static void j514s_memory_floor_work(struct work_struct *work)
{
	void __iomem *ptd = ioremap_np(J514S_PTD, J514S_PTD_SIZE);
	u64 before[2], after[2];
	bool confirmed = false;

	if (!ptd || readq(ptd + J514S_PTD_STATUS) != 1)
		goto retry;
	/* The first try reads the requests while no change of ours is pending. */
	if (!j514s_floor_try)
		for (u32 i = 0; i < 2; i++)
			j514s_floor_prior[i] = readq(ptd + (J514S_PTD_FLOOR_IDX0 + i) * 16) &
					       ~J514S_PTD_FLOOR_MASK;
	for (u32 i = 0; i < 2; i++)
		writeq(j514s_floor_prior[i], ptd + J514S_PTD_FLOOR_SET(i));
	msleep(J514S_FLOOR_SETTLE_MS);
	for (u32 i = 0; i < 2; i++)
		before[i] = readq(ptd + J514S_PTD_DVFS_STATE(i));
	for (u32 i = 0; i < 2; i++)
		writeq(j514s_floor_prior[i] | J514S_PTD_FLOOR, ptd + J514S_PTD_FLOOR_SET(i));
	msleep(J514S_FLOOR_SETTLE_MS);
	for (u32 i = 0; i < 2; i++)
		after[i] = readq(ptd + J514S_PTD_DVFS_STATE(i));
	confirmed = before[0] != after[0] || before[1] != after[1];
	pr_info("pmp-display: J514S memory floor try %u: DVFS state %#llx/%#llx -> %#llx/%#llx%s\n",
		j514s_floor_try + 1, before[0], before[1], after[0], after[1],
		confirmed ? "" : ", no response");
retry:
	if (ptd)
		iounmap(ptd);
	if (!confirmed && ++j514s_floor_try < J514S_FLOOR_TRIES) {
		schedule_delayed_work(&j514s_floor_work, HZ);
		return;
	}
	if (!confirmed)
		pr_warn("pmp-display: J514S memory floor not confirmed by the PMP\n");
	else if (register_pm_notifier(&j514s_floor_pm_nb))
		pr_warn("pmp-display: J514S memory floor stays on through system sleep\n");
	else
		return;	/* The notifier keeps the module. */
	module_put(THIS_MODULE);
}

/* Once, with the PMP running; the pending work holds the module. */
static void j514s_memory_floor(void)
{
	if (!j514s_memory_floor_enabled || atomic_xchg(&j514s_floor_started, 1) ||
	    !try_module_get(THIS_MODULE))
		return;
	schedule_delayed_work(&j514s_floor_work, HZ);
}

static int j514s_display_acked(bool wait)
{
	void __iomem *ptd = ioremap_np(J514S_PTD, J514S_PTD_SIZE);
	u64 status;
	int ret;

	if (!ptd)
		return -ENOMEM;
	ret = readq_poll_timeout(ptd + J514S_PTD_STATUS, status, status == 1,
				 1000, wait ? 2000000 : 1);
	/*
	 * DISP must be acknowledged (an ACK implies the request; ANS needs
	 * none). The request entry is not read: that may consume the PMP's
	 * change notification of an external controller's request.
	 */
	if (!ret && !(readq(ptd + J514S_PTD_ACK) & J514S_PTD_DISP))
		ret = -EIO;
	iounmap(ptd);
	if (!ret && wait)
		j514s_memory_floor();
	return ret;
}

int apple_pmp_j514s_display_prepare(struct device *consumer)
{
	struct device_node *np;
	struct platform_device *pmp;
	bool bound;

	if (!apple_pmp_t6030_board())
		return -ENODEV;
	np = pmp_bridge_node();
	pmp = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (!pmp)
		return -EPROBE_DEFER;
	bound = device_is_bound(&pmp->dev);
	if (bound && !device_link_add(consumer, &pmp->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		bound = false;
	put_device(&pmp->dev);
	return bound ? 0 : -EPROBE_DEFER;
}
EXPORT_SYMBOL_GPL(apple_pmp_j514s_display_prepare);

int apple_pmp_j514s_display_get(struct device *consumer)
{
	return apple_pmp_t6030_board() ? j514s_display_acked(true) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_j514s_display_get);

int apple_pmp_j514s_display_check(struct device *consumer)
{
	return apple_pmp_t6030_board() ? j514s_display_acked(false) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_j514s_display_check);
