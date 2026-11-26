// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T8152 Kopsia bring-up: host submission and polled VP/PP completion.
 *
 * Completion is polled because no decoder interrupt is described: the ADT's
 * avd node lists only its PostOffice mailbox pair (AIC 308 and 268, mailbox
 * 9's send-empty and recv-not-empty), and with the MCU halted nothing is known
 * to raise them. The poll runs only while a frame is active.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include "avd.h"

#define KOPSIA_STATUS(n) (0x194 + 4 * (n))
#define KOPSIA_ERROR BIT(1)
#define KOPSIA_DONE BIT(2)
#define KOPSIA_PP 13

struct avd_kopsia_tunable { u32 off, mask, val; };
#include "avd-kopsia-tunables.h"

struct avd_kopsia {
	struct avd_dev *avd;
	void __iomem *wrap;
	void __iomem *dma;
	struct work_struct poll_work;
	bool running;
};

static void kopsia_update(void __iomem *reg, u32 mask, u32 value)
{
	writel((readl(reg) & ~mask) | value, reg);
}

static int avd_kopsia_boot(struct avd_dev *avd)
{
	struct avd_kopsia *k = avd->backend_priv;
	unsigned int i;

	if (readl(avd->ctrl) != 0x40030)
		return -ENODEV;

	/* Keep the MCU halted: this transport owns the hardware status bits. */
	writel(0xe, avd->mbox + 0x28);
	kopsia_update(avd->mbox + 0x40, BIT(24), BIT(24));
	writel(0, avd->mbox + 0x104);
	writel(1, k->wrap + 0x18);
	writel(0, k->dma);
	writel(0x20, avd->ctrl + 0x1cc);
	writel(U32_MAX, avd->ctrl + 0xe6d0);
	writel(U32_MAX, avd->ctrl + 0xe6d4);
	writel(U32_MAX, avd->ctrl + 0xe710);
	writel(U32_MAX, avd->ctrl + 0xe714);
	for (i = 0; i < ARRAY_SIZE(kopsia_tunables); i++) {
		const struct avd_kopsia_tunable *t = &kopsia_tunables[i];

		kopsia_update(avd->ctrl + t->off, t->mask, t->val);
	}
	kopsia_update(k->wrap + 0x14, 1, 1);
	kopsia_update(k->wrap + 0x18, 1, 1);
	kopsia_update(k->wrap + 0x4c, 1, 1);
	kopsia_update(k->dma, 2, 0);
	kopsia_update(k->dma + 0x10, 0x1ff8000f, 2);
	kopsia_update(k->dma + 0x14, 0x1ff8000f, 2);
	kopsia_update(k->dma + 0x18, 0x1ff8000f, 3);
	kopsia_update(k->dma + 0x1c, 0xf, 0);
	kopsia_update(k->dma + 0x20, 0xf, 0);
	for (i = 0; i <= KOPSIA_PP; i++)
		writel(KOPSIA_DONE | KOPSIA_ERROR, avd->ctrl + KOPSIA_STATUS(i));
	k->running = true;
	dev_info_once(avd->dev, "Kopsia host backend, decoder %#x (polled completion)\n",
		      readl(avd->ctrl));
	return 0;
}

static void avd_kopsia_shutdown(struct avd_dev *avd)
{
	struct avd_kopsia *k = avd->backend_priv;

	/* Serialized against polling by queue_mutex or the idle PM reference. */
	WRITE_ONCE(k->running, false);
}

static void avd_kopsia_poll(struct work_struct *work)
{
	struct avd_kopsia *k = container_of(work, struct avd_kopsia, poll_work);
	struct avd_dev *avd = k->avd;

	for (;;) {
		struct avd_frame *f;
		u32 vp, status;
		bool active;

		mutex_lock(&avd->queue_mutex);
		f = avd->compat_active;
		if (!READ_ONCE(k->running) || !f || avd->stopping || avd->faulted) {
			mutex_unlock(&avd->queue_mutex);
			break;
		}
		vp = avd->variant->vp_slot_base[f->cmd[0]];
		status = readl(avd->ctrl + KOPSIA_STATUS(vp));
		if (status & KOPSIA_ERROR) {
			writel(KOPSIA_ERROR, avd->ctrl + KOPSIA_STATUS(vp));
			avd_status(avd, vp);
			avd_open_complete(avd, vp);
			mutex_unlock(&avd->queue_mutex);
			break;
		}
		if (status & KOPSIA_DONE) {
			writel(KOPSIA_DONE, avd->ctrl + KOPSIA_STATUS(vp));
			avd_open_complete(avd, 0x100 | vp);
		}
		status = readl(avd->ctrl + KOPSIA_STATUS(KOPSIA_PP));
		if (status & KOPSIA_DONE) {
			writel(KOPSIA_DONE, avd->ctrl + KOPSIA_STATUS(KOPSIA_PP));
			avd_open_complete(avd, 0x1000);
		}
		active = avd->compat_active != NULL;
		mutex_unlock(&avd->queue_mutex);
		if (!active)
			break;
		usleep_range(100, 200);
	}
}

static int avd_kopsia_submit(struct avd_dev *avd, struct avd_frame *f)
{
	struct avd_kopsia *k = avd->backend_priv;
	int ret = avd_open_submit(avd, f);

	if (!ret)
		queue_work(system_dfl_wq, &k->poll_work);
	return ret;
}

static unsigned int avd_kopsia_max_inflight(struct avd_dev *avd)
{
	return 1;
}

static void avd_kopsia_cleanup(void *data)
{
	struct avd_kopsia *k = data;

	WRITE_ONCE(k->running, false);
	cancel_work_sync(&k->poll_work);
}

static int avd_kopsia_probe(struct avd_dev *avd)
{
	struct avd_kopsia *k = devm_kzalloc(avd->dev, sizeof(*k), GFP_KERNEL);

	if (!k)
		return -ENOMEM;
	k->avd = avd;
	k->wrap = devm_platform_ioremap_resource_byname(avd->pdev, "wrap");
	if (IS_ERR(k->wrap))
		return PTR_ERR(k->wrap);
	k->dma = devm_platform_ioremap_resource_byname(avd->pdev, "dma");
	if (IS_ERR(k->dma))
		return PTR_ERR(k->dma);
	INIT_WORK(&k->poll_work, avd_kopsia_poll);
	avd->backend_priv = k;
	return devm_add_action_or_reset(avd->dev, avd_kopsia_cleanup, k);
}

const struct avd_backend_ops avd_kopsia_backend = {
	.name = "kopsia-host",
	.probe = avd_kopsia_probe,
	.boot = avd_kopsia_boot,
	.shutdown = avd_kopsia_shutdown,
	.max_inflight = avd_kopsia_max_inflight,
	.submit = avd_kopsia_submit,
	.suspend = avd_pm_backend_suspend,
	.resume = avd_pm_backend_resume,
};
