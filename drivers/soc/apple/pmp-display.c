// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T6030 (M3 Pro: J514S, J516S) and T6031 (M3 Max: J514C, J516C) power votes
 * through pmp-core.rs's RTKit session.
 *
 * The display is inherited running from the boot loader. Before the PMP CPU
 * runs, its power chain is held and the PMP's DISP|ANS request is seeded in
 * the PTD, so the PMP keeps it on; the DCP driver then waits for the PMP to
 * acknowledge it. Where the table and the power chain sit differs per SoC
 * (struct pmp_display_soc). The GPU's AGX vote, T6030 only, is an untagged
 * RTKit request on the PMP endpoint: exactly one is in flight, with a
 * one-second deadline. Votes and supplier links are retained until reboot.
 * Malformed, unsolicited, late or duplicate replies and firmware crashes
 * latch a sticky error.
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
#define J514S_PMP_ASC		0x350c00000ULL
#define J514S_PMGR		0x350700000ULL

/* SYS, FE and CPU of the display power chain, parents first. */
static const u32 j514s_display_ps[] = { 0x1c0, 0x258, 0x10000 };

/*
 * J516C/J514C (T6031). PTD at 0x2903c0000 (the PMP's ptd-update window less
 * 0x10000): status +0x10; the SoC device request word at PTD index 0x200
 * (+0x2000), its acknowledgement at index 0x208 (+0x2080), the request-set
 * aperture at +0x10000 + 0x200*8. The internal display is SoC device 17,
 * request bit 16; storage (ANS) is device 34, bit 33. The display chain
 * sits in one PMGR block: DISP_SYS +0x200, DISP_FE +0x298, DISP_CPU +0x2a0.
 */
#define J516C_PTD		0x2903c0000ULL
#define J516C_PTD_SIZE		0x14000
#define J516C_PTD_REQ		(0x200 * 16)
#define J516C_PTD_ACK		(0x208 * 16)
#define J516C_PTD_SET		(0x10000 + 0x200 * 8)
#define J516C_PTD_DISP		BIT_ULL(16)
#define J516C_PTD_ANS		BIT_ULL(33)
#define J516C_PMP_ASC		0x290c00000ULL
#define J516C_PMGR		0x292280000ULL

static const u32 j516c_display_ps[] = { 0x200, 0x298, 0x2a0 };

/*
 * One SoC's PMP as the display hold sees it: the node the boot-time handoff
 * adds, its device-state table and the display power chain it must keep up.
 */
struct pmp_display_soc {
	const char *name;
	const char *pmp_path;
	const char *pmp_compat;
	u64 ptd;
	u32 ptd_size;
	u32 ptd_status;
	u32 ptd_req, ptd_ack, ptd_set;
	u64 ptd_disp, ptd_ans;
	u64 asc;
	u64 pmgr;
	u32 pmgr_size;
	const u32 *display_ps;
	unsigned int nr_display_ps;
	/* The CPU-client memory floor and the GPU vote are known on T6030 only. */
	bool memory_floor;
};

static const struct pmp_display_soc pmp_display_t6030 = {
	.name = "J514S",
	.pmp_path = "/soc/pmp@350500000",
	.pmp_compat = "apple,t6030-pmp-v2",
	.ptd = J514S_PTD,
	.ptd_size = J514S_PTD_SIZE,
	.ptd_status = J514S_PTD_STATUS,
	.ptd_req = J514S_PTD_REQ,
	.ptd_ack = J514S_PTD_ACK,
	.ptd_set = J514S_PTD_SET,
	.ptd_disp = J514S_PTD_DISP,
	.ptd_ans = J514S_PTD_ANS,
	.asc = J514S_PMP_ASC,
	.pmgr = J514S_PMGR,
	.pmgr_size = 0x14000,
	.display_ps = j514s_display_ps,
	.nr_display_ps = ARRAY_SIZE(j514s_display_ps),
	.memory_floor = true,
};

static const struct pmp_display_soc pmp_display_t6031 = {
	.name = "J516C",
	.pmp_path = "/soc/pmp@290700000",
	.pmp_compat = "apple,t6031-pmp-v2",
	.ptd = J516C_PTD,
	.ptd_size = J516C_PTD_SIZE,
	.ptd_status = J514S_PTD_STATUS,
	.ptd_req = J516C_PTD_REQ,
	.ptd_ack = J516C_PTD_ACK,
	.ptd_set = J516C_PTD_SET,
	.ptd_disp = J516C_PTD_DISP,
	.ptd_ans = J516C_PTD_ANS,
	.asc = J516C_PMP_ASC,
	.pmgr = J516C_PMGR,
	.pmgr_size = 0xc000,
	.display_ps = j516c_display_ps,
	.nr_display_ps = ARRAY_SIZE(j516c_display_ps),
};

/* The running board's PMP description, or NULL where no display is held. */
static const struct pmp_display_soc *pmp_display_soc(void)
{
	if (apple_pmp_t6030_board())
		return &pmp_display_t6030;
	if (apple_pmp_t6031_board())
		return &pmp_display_t6031;
	return NULL;
}

bool apple_pmp_display_retained_board(void)
{
	return !!pmp_display_soc();
}

/* The board's PMP node, added by the boot-time PMP handoff (no alias). */
static struct device_node *pmp_bridge_node(void)
{
	const struct pmp_display_soc *soc = pmp_display_soc();

	return soc ? of_find_node_by_path(soc->pmp_path) : NULL;
}

bool apple_pmp_display_present(void)
{
	struct device_node *np = pmp_bridge_node();

	of_node_put(np);
	return !!np;
}
EXPORT_SYMBOL_GPL(apple_pmp_display_present);

struct apple_pmp_display *apple_pmp_display_register(struct device *dev,
		const void *cookie, int (*send)(const void *, u64))
{
	const struct pmp_display_soc *soc = pmp_display_soc();
	struct apple_pmp_display *p = NULL;
	struct device_node *node;
	bool match;

	if (!soc || !dev->of_node ||
	    !of_device_is_compatible(dev->of_node, soc->pmp_compat))
		return NULL;
	node = pmp_bridge_node();
	match = node == dev->of_node;
	of_node_put(node);
	/* The handoff copies the soc-device table from the boot loader; require it. */
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
	dev_info(dev, "%s PMP vote bridge ready; retained existing PMP owner\n", soc->name);
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
 * Memory floor (T6030): CPU-client EACC0/PACC0 PTD indices 288/289 request
 * DCS4 and FAB2 (valid bit 61, DCS bits 51:48, FAB bits 35:32), other fields
 * kept. DVFS state entries 8 and 10 (PTD range 3) publish the firmware's
 * response. The T6031 table's CPU-client entries have not been read, so no
 * floor is requested there.
 */
#define J514S_PTD_FLOOR_IDX0	288
#define J514S_PTD_FLOOR_MASK	(BIT_ULL(61) | GENMASK_ULL(51, 48) | GENMASK_ULL(35, 32))
#define J514S_PTD_FLOOR		(BIT_ULL(61) | (4ULL << 48) | (2ULL << 32))
#define J514S_PTD_FLOOR_SET(i)	(0x10000 + (J514S_PTD_FLOOR_IDX0 + (i)) * 8)
#define J514S_PTD_DVFS_STATE(i)	((8 + 2 * (i)) * 16)

/*
 * Hold the inherited display epoch: minimum state 15 (bits 19:16), W1C history
 * bits 9:8 written as zero, other controls preserved. Must precede PMP RUN,
 * or the PMP powers the display down and the DCP CPU stops.
 */
static int pmp_display_hold(struct device *dev, const struct pmp_display_soc *soc)
{
	void __iomem *pmgr = ioremap_np(soc->pmgr, soc->pmgr_size);
	int ret = 0;
	u32 val;

	if (!pmgr)
		return -ENOMEM;
	for (unsigned int i = 0; i < soc->nr_display_ps; i++) {
		val = readl(pmgr + soc->display_ps[i]);
		if ((val & 0xff) != 0xff || (val & BIT(31))) {
			dev_err(dev, "%s display domain %#x not inherited on: %#x\n",
				soc->name, soc->display_ps[i], val);
			ret = -EIO;
			break;
		}
		writel((val & ~0x000f0300) | 0x000f0000, pmgr + soc->display_ps[i]);
		val = readl(pmgr + soc->display_ps[i]);
		dev_info(dev, "%s display domain %#x held: %#x\n", soc->name,
			 soc->display_ps[i], val);
		/* Not every domain implements the minimum field (SYS reads 0). */
		if ((val & 0x000f0000) != 0x000f0000)
			dev_warn(dev, "%s display domain %#x minimum not latched\n",
				 soc->name, soc->display_ps[i]);
	}
	iounmap(pmgr);
	return ret;
}

int apple_pmp_display_pre_run(struct device *dev)
{
	const struct pmp_display_soc *soc = pmp_display_soc();
	void __iomem *asc = NULL, *ptd = NULL;
	u64 status, req, seed;
	u32 run;
	int ret;

	if (!soc || !dev->of_node ||
	    !of_device_is_compatible(dev->of_node, soc->pmp_compat))
		return 0;
	/* The display floor first: the DCP must survive PMP startup. */
	ret = pmp_display_hold(dev, soc);
	if (ret)
		return ret;
	asc = ioremap_np(soc->asc, 0x4000);
	ptd = ioremap_np(soc->ptd, soc->ptd_size);
	if (!asc || !ptd) {
		ret = -ENOMEM;
		goto out;
	}
	run = readl(asc + 0x44);
	status = readq(ptd + soc->ptd_status);
	req = readq(ptd + soc->ptd_req);
	/* Cold start only: no adoption of a running or pre-seeded PMP. */
	if ((run & BIT(4)) || status || req) {
		dev_err(dev, "%s PMP not cold (run=%#x status=%#llx req=%#llx)\n",
			soc->name, run, status, req);
		ret = -EBUSY;
		goto out;
	}
	/* One 64-bit set transaction; the readback must be exactly DISP|ANS. */
	seed = soc->ptd_disp | soc->ptd_ans;
	writeq(seed, ptd + soc->ptd_set);
	req = readq(ptd + soc->ptd_req);
	if (req != seed) {
		dev_err(dev, "%s PTD seed not accepted: req=%#llx\n", soc->name, req);
		ret = -EIO;
		goto out;
	}
	dev_info(dev, "%s display held, PTD seeded DISP|ANS: req=%#llx\n", soc->name, req);
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

static int pmp_display_acked(const struct pmp_display_soc *soc, bool wait)
{
	void __iomem *ptd = ioremap_np(soc->ptd, soc->ptd_size);
	u64 status;
	int ret;

	if (!ptd)
		return -ENOMEM;
	ret = readq_poll_timeout(ptd + soc->ptd_status, status, status == 1,
				 1000, wait ? 2000000 : 1);
	/*
	 * DISP must be acknowledged (an ACK implies the request; ANS needs
	 * none). The request entry is not read: that may consume the PMP's
	 * change notification of an external controller's request.
	 */
	if (!ret && !(readq(ptd + soc->ptd_ack) & soc->ptd_disp))
		ret = -EIO;
	iounmap(ptd);
	if (!ret && wait && soc->memory_floor)
		j514s_memory_floor();
	return ret;
}

int apple_pmp_display_prepare(struct device *consumer)
{
	struct device_node *np;
	struct platform_device *pmp;
	bool bound;

	if (!pmp_display_soc())
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
EXPORT_SYMBOL_GPL(apple_pmp_display_prepare);

int apple_pmp_display_get(struct device *consumer)
{
	const struct pmp_display_soc *soc = pmp_display_soc();

	return soc ? pmp_display_acked(soc, true) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_display_get);

int apple_pmp_display_check(struct device *consumer)
{
	const struct pmp_display_soc *soc = pmp_display_soc();

	return soc ? pmp_display_acked(soc, false) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_display_check);
