// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Retained J713 DISP/AGX votes through the existing Rust PMP owner.
 * Exactly one untagged request is in flight; invalid replies fail the epoch.
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
#include <linux/unaligned.h>
#include <linux/soc/apple/pmp-display.h>

/* AGX: command 0x0f, logical device 5, state 1. The reply carries no tag. */
#define PMP_AGX_REQUEST		0x0020f50100000000ULL
#define PMP_DISP_REQUEST	0x0020f80100000000ULL
/* DISP, state 0: released while its DCP has powered the display domains down. */
#define PMP_DISP_RELEASE	0x0020f80000000000ULL
#define PMP_POWER_REPLY		0x0021000000000000ULL
#define PMP_ENDPOINT		0x20

enum pmp_power_kind {
	PMP_POWER_NONE,
	PMP_POWER_AGX,
	PMP_POWER_DISP,
	PMP_POWER_COUNT,
};

/* Caller holds the bridge lock. Errors permanently invalidate untagged replies. */
struct pmp_power_vote {
	/* Ambiguous untagged replies close admission, not accepted votes. */
	int error;
	/* A failed PMP owner invalidates every accepted vote as well. */
	int provider_error;
	enum pmp_power_kind pending;
	/* The pending request releases its vote rather than taking it. */
	bool releasing;
	bool held[PMP_POWER_COUNT];
};

static void pmp_vote_fail(struct pmp_power_vote *s, int error)
{
	if (!s->error)
		s->error = error < 0 ? error : -EIO;
	/* A release with an unknown outcome no longer counts as held. */
	if (s->pending && s->releasing)
		s->held[s->pending] = false;
	s->pending = PMP_POWER_NONE;
	s->releasing = false;
}

/* The Rust owner latches every ambiguous transaction as a provider failure.
 * An error without that latch happened before transmission (for example a
 * supplier that has not probed yet); release only the unpublished request.
 */
static void pmp_vote_send_failed(struct pmp_power_vote *s)
{
	if (s->provider_error) {
		pmp_vote_fail(s, s->provider_error);
	} else {
		s->pending = PMP_POWER_NONE;
		s->releasing = false;
	}
}

/* One means send the request, zero means the same retained vote is held. */
static int pmp_vote_begin(struct pmp_power_vote *s, enum pmp_power_kind kind)
{
	if (kind <= PMP_POWER_NONE || kind >= PMP_POWER_COUNT)
		return -EINVAL;
	if (s->provider_error)
		return s->provider_error;
	if (s->held[kind])
		return 0;
	if (s->error)
		return s->error;
	if (s->pending)
		return -EBUSY;
	s->pending = kind;
	return 1;
}

/* One means send the release, zero means the vote is not held. */
static int pmp_vote_begin_release(struct pmp_power_vote *s, enum pmp_power_kind kind)
{
	if (kind <= PMP_POWER_NONE || kind >= PMP_POWER_COUNT)
		return -EINVAL;
	if (s->provider_error)
		return s->provider_error;
	if (!s->held[kind])
		return 0;
	if (s->error)
		return s->error;
	if (s->pending)
		return -EBUSY;
	s->pending = kind;
	s->releasing = true;
	return 1;
}

/*
 * Whether a vote can be released now and taken again later, changing
 * nothing: held, no request in flight, and admission open (a failed untagged
 * request closes it, and the later get is a new request).
 */
static int pmp_vote_check_release(struct pmp_power_vote *s, enum pmp_power_kind kind)
{
	if (kind <= PMP_POWER_NONE || kind >= PMP_POWER_COUNT)
		return -EINVAL;
	if (s->provider_error)
		return s->provider_error;
	if (!s->held[kind])
		return -ENODEV;
	if (s->error)
		return s->error;
	return s->pending ? -EBUSY : 0;
}

/* Released, or the outcome is unknown and admission is closed. */
static int pmp_vote_finish_release(struct pmp_power_vote *s, enum pmp_power_kind kind)
{
	if (s->provider_error)
		return s->provider_error;
	if (s->pending == kind && s->releasing)
		pmp_vote_fail(s, -ETIMEDOUT);
	return s->held[kind] || s->error ? s->error ?: -EIO : 0;
}

/* Accepted votes are retained even if a different untagged request fails. */
static int pmp_vote_check(struct pmp_power_vote *s, enum pmp_power_kind kind)
{
	if (s->provider_error)
		return s->provider_error;
	if (s->held[kind])
		return 0;
	return s->error ?: -ENODEV;
}

/* Caller arbitrates reply and timeout while holding the vote lock. */
static int pmp_vote_finish(struct pmp_power_vote *s, enum pmp_power_kind kind)
{
	int ret = pmp_vote_check(s, kind);

	if (ret == -ENODEV) {
		pmp_vote_fail(s, -ETIMEDOUT);
		ret = -ETIMEDOUT;
	}
	return ret;
}

static void pmp_vote_reply(struct pmp_power_vote *s, u8 endpoint, u64 word)
{
	if (s->error)
		return;
	if (!s->pending || endpoint != PMP_ENDPOINT || word != PMP_POWER_REPLY) {
		pmp_vote_fail(s, -EPROTO);
		return;
	}
	s->held[s->pending] = !s->releasing;
	s->pending = PMP_POWER_NONE;
	s->releasing = false;
}

static inline bool j713_disp_table(const void *data, size_t size)
{
	const u8 *p = data;

	return p && size == 34 * 0x7c && get_unaligned_le32(p + 15 * 0x7c) == 8 &&
		!memcmp(p + 15 * 0x7c + 0x74, "DISP\0\0\0\0", 8);
}

static inline bool j713_agx_table(const void *data, size_t size)
{
	const u8 *p = data;
	unsigned int offset;
	u32 expected;

	if (!p || size != 34 * 0x7c)
		return false;
	p += 12 * 0x7c;
	/* Qualified raw identity fields, not inferred meanings or writable controls. */
	for (offset = 0; offset <= 0x70; offset += 4) {
		switch (offset) {
		case 0: expected = 5; break;
		case 4: expected = 0x4746585f; break;
		case 8: expected = 3; break;
		case 0xc: case 0x1c: case 0x2c: expected = 2; break;
		default: expected = 0; break;
		}
		if (get_unaligned_le32(p + offset) != expected)
			return false;
	}
	return !memcmp(p + 0x74, "AGX\0\0\0\0\0", 8);
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
	struct device_link *link[PMP_POWER_COUNT];
	bool agx_table_valid;
};

/* One qualified supplier; published once, never freed during its epoch. */
static struct apple_pmp_display *pmp_display;
static DEFINE_MUTEX(pmp_display_registration);

bool apple_pmp_display_retained_board(void)
{
	return apple_pmp_j713_board();
}

/* The qualified loader publishes the existing PMP through its alias. */
static struct device_node *pmp_bridge_node(void)
{
	return of_find_node_by_path("pmp");
}

struct apple_pmp_display *apple_pmp_display_register(struct device *dev,
		const void *cookie, int (*send)(const void *, u64))
{
	struct apple_pmp_display *p = NULL;
	struct device_node *node, *qualified;
	const void *table;
	int size;
	bool match, j713 = apple_pmp_j713_board();

	if (!j713 || !dev->of_node ||
	    !of_device_is_compatible(dev->of_node,
		"apple,t8132-pmp-v2"))
		return NULL;
	node = pmp_bridge_node();
	match = node == dev->of_node;
	of_node_put(node);
	if (j713) {
		qualified = of_find_node_by_path("/soc/pmp@380500000");
		match = match && qualified == dev->of_node;
		of_node_put(qualified);
	}
	/* J713 has a qualified fixed table shape. */
	table = of_get_property(dev->of_node, "apple,tunable-soc-device", &size);
	if (!match || !of_property_present(dev->of_node, "apple,tunable-soc-device") ||
	    !cookie || !send || (j713 && !j713_disp_table(table, size)))
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
	p->agx_table_valid = !j713 || j713_agx_table(table, size);
	mutex_init(&p->request);
	spin_lock_init(&p->lock);
	init_completion(&p->reply);
	/* The bridge has no teardown: keep it and the PMP until reboot. */
	__module_get(THIS_MODULE);
	pm_runtime_get_noresume(dev);
	smp_store_release(&pmp_display, p);
	dev_info(dev, "%s PMP vote bridge ready; retained existing PMP owner\n",
		 j713 ? "J713" : "J514S");
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
	if (!p->vote.provider_error)
		p->vote.provider_error = error < 0 ? error : -EIO;
	pmp_vote_fail(&p->vote, p->vote.provider_error);
	spin_unlock_irqrestore(&p->lock, flags);
	complete_all(&p->reply);
}

bool apple_pmp_display_receive(struct apple_pmp_display *p, u8 endpoint, u64 word)
{
	unsigned long flags;

	if (!p || ((word >> 48) & 0xff) != 0x21)
		return false; /* Normal registry traffic belongs to pmp-core.rs. */
	spin_lock_irqsave(&p->lock, flags);
	/* The waiter arbitrates timeout under this same lock. An accepted
	 * reply must not be overwritten by the waiter reaching its deadline.
	 */
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

	if (apple_pmp_j713_board()) {
		/* Both consumers must name the exact boot-profile node. */
		if ((kind != PMP_POWER_AGX && kind != PMP_POWER_DISP) || !consumer->of_node ||
		    !of_device_is_compatible(consumer->of_node,
			kind == PMP_POWER_AGX ? "apple,agx-t8132" : "apple,t8132-dcp"))
			return ERR_PTR(-ENODEV);
		pmp = of_find_node_by_path(kind == PMP_POWER_AGX ? "gpu" : "dcp");
		match = pmp == consumer->of_node;
		of_node_put(pmp);
		if (!match)
			return ERR_PTR(-ENODEV);
	} else {
		return ERR_PTR(-ENODEV);
	}
	if (kind == PMP_POWER_AGX) {
		pmp = of_parse_phandle(consumer->of_node, "apple,pmp", 0);
		supplier = pmp_bridge_node();
		match = pmp && supplier == pmp;
		of_node_put(pmp);
		of_node_put(supplier);
		if (!match || (p && !p->agx_table_valid))
			return ERR_PTR(-ENODEV);
	}
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
	if (!p || (kind == PMP_POWER_AGX && !p->agx_table_valid)) {
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

static int pmp_power_check(struct device *consumer, enum pmp_power_kind kind, bool release)
{
	struct apple_pmp_display *p = pmp_power_consumer(consumer, kind);
	unsigned long flags;
	int ret;

	if (IS_ERR(p))
		return PTR_ERR(p);
	spin_lock_irqsave(&p->lock, flags);
	ret = release ? pmp_vote_check_release(&p->vote, kind) : pmp_vote_check(&p->vote, kind);
	spin_unlock_irqrestore(&p->lock, flags);
	return ret;
}

static int pmp_power_get(struct device *consumer, enum pmp_power_kind kind)
{
	struct apple_pmp_display *p = pmp_power_consumer(consumer, kind);
	u64 request = kind == PMP_POWER_AGX ? PMP_AGX_REQUEST : PMP_DISP_REQUEST;
	unsigned long flags;
	int ret;

	if (IS_ERR(p))
		return PTR_ERR(p);
	device_lock(p->dev);
	mutex_lock(&p->request);
	if (p->dev->driver != p->driver || !dev_get_drvdata(p->dev)) {
		ret = -ENODEV;
		apple_pmp_display_fail(p, ret);
		goto out;
	}
	if (!p->link[kind]) {
		ret = -ENODEV;
		goto out;
	}
	spin_lock_irqsave(&p->lock, flags);
	/* Reset before publishing pending: an immediate reply must not be lost. */
	reinit_completion(&p->reply);
	ret = pmp_vote_begin(&p->vote, kind);
	spin_unlock_irqrestore(&p->lock, flags);
	if (ret <= 0)
		goto out;
	/* The shared PMP owner serializes requests, checks readiness and waits
	 * for the exact firmware reply before returning success. No second
	 * receiver may correlate that same untagged stream independently.
	 */
	ret = p->send(p->cookie, request);
	if (ret)
		goto failed;
	spin_lock_irqsave(&p->lock, flags);
	pmp_vote_reply(&p->vote, PMP_ENDPOINT, PMP_POWER_REPLY);
	ret = pmp_vote_finish(&p->vote, kind);
	spin_unlock_irqrestore(&p->lock, flags);
	if (ret)
		goto out;
	dev_info(p->dev, "Retained %s enable accepted: request=%016llx reply=%016llx\n",
		 kind == PMP_POWER_AGX ? "AGX" : "DISP", request, PMP_POWER_REPLY);
	goto out;
failed:
	spin_lock_irqsave(&p->lock, flags);
	pmp_vote_send_failed(&p->vote);
	spin_unlock_irqrestore(&p->lock, flags);
out:
	mutex_unlock(&p->request);
	device_unlock(p->dev);
	return ret;
}

/*
 * Release the DISP vote while the consumer has powered the display domains
 * down; apple_pmp_j713_display_get() takes it again before power-up. An
 * ambiguous release closes admission: the vote is then neither held nor
 * taken again.
 */
static int pmp_power_put(struct device *consumer, enum pmp_power_kind kind)
{
	struct apple_pmp_display *p = pmp_power_consumer(consumer, kind);
	unsigned long flags;
	int ret;

	if (IS_ERR(p))
		return PTR_ERR(p);
	if (kind != PMP_POWER_DISP)
		return -EINVAL;
	device_lock(p->dev);
	mutex_lock(&p->request);
	if (p->dev->driver != p->driver || !dev_get_drvdata(p->dev)) {
		ret = -ENODEV;
		apple_pmp_display_fail(p, ret);
		goto out;
	}
	spin_lock_irqsave(&p->lock, flags);
	reinit_completion(&p->reply);
	ret = pmp_vote_begin_release(&p->vote, kind);
	spin_unlock_irqrestore(&p->lock, flags);
	if (ret <= 0)
		goto out;
	ret = p->send(p->cookie, PMP_DISP_RELEASE);
	if (ret) {
		spin_lock_irqsave(&p->lock, flags);
		pmp_vote_send_failed(&p->vote);
		spin_unlock_irqrestore(&p->lock, flags);
		goto out;
	}
	spin_lock_irqsave(&p->lock, flags);
	pmp_vote_reply(&p->vote, PMP_ENDPOINT, PMP_POWER_REPLY);
	ret = pmp_vote_finish_release(&p->vote, kind);
	spin_unlock_irqrestore(&p->lock, flags);
	if (!ret)
		dev_dbg(p->dev, "Retained DISP vote released: request=%016llx\n",
			PMP_DISP_RELEASE);
out:
	mutex_unlock(&p->request);
	device_unlock(p->dev);
	return ret;
}

/* Probe creates the retained supplier links; get/check never change owners. */
int apple_pmp_j713_display_prepare(struct device *consumer)
{
	return apple_pmp_j713_board() ? pmp_power_prepare(consumer, PMP_POWER_DISP) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_j713_display_prepare);

int apple_pmp_j713_gpu_prepare(struct device *consumer)
{
	return apple_pmp_j713_board() ? pmp_power_prepare(consumer, PMP_POWER_AGX) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_j713_gpu_prepare);

int apple_pmp_j713_display_check(struct device *consumer)
{
	return apple_pmp_j713_board() ? pmp_power_check(consumer, PMP_POWER_DISP, false) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_j713_display_check);

int apple_pmp_j713_display_check_release(struct device *consumer)
{
	return apple_pmp_j713_board() ? pmp_power_check(consumer, PMP_POWER_DISP, true) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_j713_display_check_release);

int apple_pmp_j713_gpu_check(struct device *consumer)
{
	return apple_pmp_j713_board() ? pmp_power_check(consumer, PMP_POWER_AGX, false) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_j713_gpu_check);

int apple_pmp_j713_display_get(struct device *consumer)
{
	return apple_pmp_j713_board() ? pmp_power_get(consumer, PMP_POWER_DISP) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_j713_display_get);

int apple_pmp_j713_display_put(struct device *consumer)
{
	return apple_pmp_j713_board() ? pmp_power_put(consumer, PMP_POWER_DISP) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_j713_display_put);

int apple_pmp_j713_gpu_get(struct device *consumer)
{
	return apple_pmp_j713_board() ? pmp_power_get(consumer, PMP_POWER_AGX) : -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_pmp_j713_gpu_get);
