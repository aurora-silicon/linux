// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Driver for SIO coprocessor on t8103 (M1) and other Apple SoCs
 *
 * Copyright (C) The Asahi Linux Contributors
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_dma.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/soc/apple/rtkit.h>

#include "dmaengine.h"
#include "virt-dma.h"

#define NCHANNELS_MAX	0x80

#define REG_CPU_CONTROL	0x44
#define CPU_CONTROL_RUN BIT(4)

#define SIOMSG_DATA	GENMASK(63, 32)
#define SIOMSG_TYPE	GENMASK(23, 16)
#define SIOMSG_PARAM	GENMASK(31, 24)
#define SIOMSG_TAG	GENMASK(13, 8)
#define SIOMSG_EP	GENMASK(7, 0)

#define EP_SIO		0x20

#define MSG_START	0x2
#define MSG_SETUP	0x3
#define MSG_CONFIGURE	0x5
#define MSG_ISSUE	0x6
#define MSG_TERMINATE	0x8
#define MSG_ACK		0x65
#define MSG_NACK	0x66
#define MSG_STARTED	0x67
#define MSG_REPORT	0x68

#define SIO_CALL_TIMEOUT_MS	100
#define SIO_SHMEM_SIZE		0x1000
#define SIO_NO_DESC_SLOTS	64

/*
 * There are two kinds of 'transaction descriptors' in play here.
 *
 * There's the struct sio_tx, and the struct dma_async_tx_descriptor embedded
 * inside, which jointly represent a transaction to the dmaengine subsystem.
 * At this time we only support those transactions to be cyclic.
 *
 * Then there are the coprocessor descriptors, which is what the coprocessor
 * knows and understands. These don't seem to have a cyclic regime, so we can't
 * map the dmaengine transaction on an exact coprocessor counterpart. Instead
 * we continually queue up many coprocessor descriptors to implement a cyclic
 * transaction.
 *
 * The number below is the maximum of how far ahead (how many) coprocessor
 * descriptors we should be queuing up, per channel, for a cyclic transaction.
 * Basically it's a made-up number.
 */
#define SIO_MAX_NINFLIGHT	4

struct sio_coproc_desc {
	u32 pad1;
	u32 flag;
	u64 unk;
	u64 iova;
	u64 size;
	u64 pad2;
	u64 pad3;
} __packed;
static_assert(sizeof(struct sio_coproc_desc) == 48);

struct sio_shmem_chan_config {
	u32 datashape;
	u32 timeout;
	u32 fifo;
	u32 threshold;
	u32 limit;
} __packed;

struct sio_data;
struct sio_tx;

struct sio_chan {
	unsigned int no;
	struct sio_data *host;
	struct virt_dma_chan vc;
	struct work_struct terminate_wq;

	bool configured;
	struct sio_shmem_chan_config cfg;

	struct sio_tx *current_tx;
};

#define SIO_NTAGS		16

typedef void (*sio_ack_callback)(struct sio_chan *, void *, bool);

struct sio_data {
	void __iomem *base;
	struct dma_device dma;
	struct device *dev;
	struct apple_rtkit *rtk;
	void *shmem;
	struct sio_coproc_desc *shmem_desc_base;
	unsigned long *desc_allocated;

	struct sio_tagdata {
		DECLARE_BITMAP(allocated, SIO_NTAGS);
		int last_tag;

		struct completion completions[SIO_NTAGS];
		bool atomic[SIO_NTAGS];
		bool acked[SIO_NTAGS];

		sio_ack_callback ack_callback[SIO_NTAGS];
		void *cookie[SIO_NTAGS];
	} tags;

	int nchannels;
#if IS_ENABLED(CONFIG_APPLE_SIO_T6030)
	bool t6030;
#endif
	struct sio_chan channels[];
};

struct sio_tx {
	struct virt_dma_desc vd;
	struct completion done;

	bool terminated;
	size_t period_len;
	int nperiods;
	int ninflight;
	int next;

	struct sio_coproc_desc *siodesc[];
};

static int sio_send_siomsg(struct sio_data *sio, u64 msg);
static int sio_send_siomsg_atomic(struct sio_data *sio, u64 msg,
				  sio_ack_callback ack_callback,
				  void *cookie);
static int sio_call(struct sio_data *sio, u64 msg);

static inline bool sio_is_t6030(struct sio_data *sio)
{
#if IS_ENABLED(CONFIG_APPLE_SIO_T6030)
	return sio->t6030;
#else
	return false;
#endif
}

#if IS_ENABLED(CONFIG_APPLE_SIO_T6030)
/*
 * Boot tables of the T6030 SIO firmware, protocol 10. The boot loader builds them from the ADT
 * and passes each one as two apple,sio-firmware-params: the table's IOVA page under its key and
 * its size in bytes under key + 1. Protocol 10 takes six 36-byte DMA shim records and nine device
 * types. A loader without protocol-10 support builds five 32-byte records and eight types, which
 * this firmware cannot use, so the SIO is not started with them.
 */
#define SIO_T6030_PARAMS_MAX	64	/* key and value pairs */
#define SIO_T6030_WINDOWS_MAX	8
#define SIO_T6030_IOVA_LIMIT	BIT_ULL(42)
/*
 * A parameter goes to the firmware as an endpoint byte (key >> 8) and a
 * parameter byte (key & 0xff), so a wider key would alias another one.
 */
#define SIO_T6030_KEY_MAX	0xffff

struct sio_t6030_window {
	u64 start;
	u64 size;
};

static const struct {
	u32 key;
	u32 size;
} sio_t6030_tables[] = {
	{ 0x1a, 4 * 16 },	/* map ranges */
	{ 0x1c, 9 * 8 },	/* device types */
	{ 0x22, 6 * 36 },	/* DMA shims */
};

/*
 * 0 when the parameters hold each protocol-10 table once, at its exact size, entirely inside one
 * of the windows the boot loader mapped for the SIO; -EINVAL otherwise.
 */
static int sio_t6030_check_params(const u32 *params, unsigned int nparams,
				  const struct sio_t6030_window *windows, unsigned int nwindows)
{
	unsigned int i, j, t;

	if (!nparams || nparams % 2 || nparams > 2 * SIO_T6030_PARAMS_MAX)
		return -EINVAL;
	for (i = 0; i < nparams; i += 2)
		if (params[i] > SIO_T6030_KEY_MAX)
			return -EINVAL;
	for (i = 0; i < nparams; i += 2)
		for (j = i + 2; j < nparams; j += 2)
			if (params[i] == params[j])
				return -EINVAL;

	for (t = 0; t < ARRAY_SIZE(sio_t6030_tables); t++) {
		bool have_page = false, have_size = false, inside = false;
		u64 start = 0, size = 0, end;

		for (i = 0; i < nparams; i += 2) {
			if (params[i] == sio_t6030_tables[t].key) {
				start = (u64)params[i + 1] << 12;
				have_page = true;
			} else if (params[i] == sio_t6030_tables[t].key + 1) {
				size = params[i + 1];
				have_size = true;
			}
		}
		if (!have_page || !have_size || size != sio_t6030_tables[t].size)
			return -EINVAL;
		if (check_add_overflow(start, size, &end) || end > SIO_T6030_IOVA_LIMIT)
			return -EINVAL;
		for (j = 0; j < nwindows && !inside; j++) {
			u64 window_end;

			if (check_add_overflow(windows[j].start, windows[j].size, &window_end))
				continue;
			inside = start >= windows[j].start && end <= window_end;
		}
		if (!inside)
			return -EINVAL;
	}
	return 0;
}

/* The IOVA windows of this device's memory regions, from their iommu-addresses */
static int sio_t6030_windows(struct device_node *np, struct sio_t6030_window *windows,
			     unsigned int max)
{
	struct of_phandle_iterator it;
	unsigned int n = 0;
	int err;

	of_for_each_phandle(&it, err, np, "memory-region", NULL, 0) {
		const __be32 *maps, *end;
		int size;

		maps = of_get_property(it.node, "iommu-addresses", &size);
		if (!maps)
			continue;
		if (size % sizeof(*maps)) {
			of_node_put(it.node);
			return -EINVAL;
		}
		end = maps + size / sizeof(*maps);
		while (maps < end) {
			struct device_node *owner __free(device_node) =
				of_find_node_by_phandle(be32_to_cpup(maps++));
			phys_addr_t iova;
			size_t length;

			/* An entry whose owner does not resolve is malformed, as a short one is */
			if (!owner) {
				of_node_put(it.node);
				return -EINVAL;
			}
			maps = of_translate_dma_region_checked(owner, maps, end - maps, &iova,
							       &length);
			if (!maps) {
				of_node_put(it.node);
				return -EINVAL;
			}
			if (owner != np)
				continue;
			if (n == max) {
				of_node_put(it.node);
				return -EINVAL;
			}
			windows[n].start = iova;
			windows[n].size = length;
			n++;
		}
	}
	return err == -ENOENT ? n : err;
}

/* 0 when the node's boot tables are protocol-10 tables inside its mapped windows */
static int sio_t6030_check_node(struct device_node *np)
{
	struct sio_t6030_window windows[SIO_T6030_WINDOWS_MAX];
	u32 params[2 * SIO_T6030_PARAMS_MAX];
	int nparams, nwindows;

	nparams = of_property_count_u32_elems(np, "apple,sio-firmware-params");
	if (nparams <= 0 || nparams > ARRAY_SIZE(params) ||
	    of_property_read_u32_array(np, "apple,sio-firmware-params", params, nparams))
		return -EINVAL;
	nwindows = sio_t6030_windows(np, windows, ARRAY_SIZE(windows));
	if (nwindows < 0)
		return nwindows;
	return sio_t6030_check_params(params, nparams, windows, nwindows);
}

static int sio_t6030_check_boot_tables(struct device *dev)
{
	if (sio_t6030_check_node(dev->of_node)) {
		dev_err(dev, "boot tables are not for SIO protocol 10; leaving the SIO stopped\n");
		return -ENODEV;
	}
	return 0;
}
#endif

static struct sio_chan *to_sio_chan(struct dma_chan *chan)
{
	return container_of(chan, struct sio_chan, vc.chan);
}

static struct sio_tx *to_sio_tx(struct dma_async_tx_descriptor *tx)
{
	return container_of(tx, struct sio_tx, vd.tx);
}

static int sio_alloc_tag(struct sio_data *sio)
{
	struct sio_tagdata *tags = &sio->tags;
	int tag, i;

	/*
	 * Because tag number 0 is special, the usable tag range
	 * is 1...(SIO_NTAGS - 1). So, to pick the next usable tag,
	 * we do modulo (SIO_NTAGS - 1) *then* plus one.
	 */

#define SIO_USABLE_TAGS (SIO_NTAGS - 1)
	tag = (READ_ONCE(tags->last_tag) % SIO_USABLE_TAGS) + 1;

	for (i = 0; i < SIO_USABLE_TAGS; i++) {
		if (!test_and_set_bit(tag, tags->allocated))
			break;

		tag = (tag % SIO_USABLE_TAGS) + 1;
	}

	WRITE_ONCE(tags->last_tag, tag);

	if (i < SIO_USABLE_TAGS)
		return tag;
	else
		return -EBUSY;
#undef SIO_USABLE_TAGS
}

static void sio_free_tag(struct sio_data *sio, int tag)
{
	struct sio_tagdata *tags = &sio->tags;

	if (WARN_ON(tag >= SIO_NTAGS))
		return;

	tags->atomic[tag] = false;
	tags->ack_callback[tag] = NULL;

	WARN_ON(!test_and_clear_bit(tag, tags->allocated));
}

static void sio_set_tag_atomic(struct sio_data *sio, int tag,
			       sio_ack_callback ack_callback,
			       void *cookie)
{
	struct sio_tagdata *tags = &sio->tags;

	tags->atomic[tag] = true;
	tags->ack_callback[tag] = ack_callback;
	tags->cookie[tag] = cookie;
}

static struct sio_coproc_desc *sio_alloc_desc(struct sio_data *sio)
{
	int i;

	for (i = 0; i < SIO_NO_DESC_SLOTS; i++)
		if (!test_and_set_bit(i, sio->desc_allocated))
			return sio->shmem_desc_base + i;

	return NULL;
}

static void sio_free_desc(struct sio_data *sio, struct sio_coproc_desc *desc)
{
	clear_bit(desc - sio->shmem_desc_base, sio->desc_allocated);
}

static int sio_coproc_desc_slot(struct sio_data *sio, struct sio_coproc_desc *desc)
{
	return (desc - sio->shmem_desc_base) * 4;
}

static enum dma_transfer_direction sio_chan_direction(int channo)
{
	/* Channel directions are fixed based on channel number */
	return (channo & 1) ? DMA_DEV_TO_MEM : DMA_MEM_TO_DEV;
}

static void sio_tx_free(struct virt_dma_desc *vd)
{
	struct sio_data *sio = to_sio_chan(vd->tx.chan)->host;
	struct sio_tx *siotx = to_sio_tx(&vd->tx);
	int i;

	for (i = 0; i < siotx->nperiods; i++)
		if (siotx->siodesc[i])
			sio_free_desc(sio, siotx->siodesc[i]);
	kfree(siotx);
}

static struct dma_async_tx_descriptor *sio_prep_dma_cyclic(
		struct dma_chan *chan, dma_addr_t buf_addr, size_t buf_len,
		size_t period_len, enum dma_transfer_direction direction,
		unsigned long flags)
{
	struct sio_chan *siochan = to_sio_chan(chan);
	struct sio_tx *siotx = NULL;
	int i, nperiods = buf_len / period_len;

	if (direction != sio_chan_direction(siochan->no))
		return NULL;

	siotx = kzalloc(struct_size(siotx, siodesc, nperiods), GFP_NOWAIT);
	if (!siotx)
		return NULL;

	init_completion(&siotx->done);
	siotx->period_len = period_len;
	siotx->nperiods = nperiods;

	for (i = 0; i < nperiods; i++) {
		struct sio_coproc_desc *d;

		siotx->siodesc[i] = d = sio_alloc_desc(siochan->host);
		if (!d) {
			siotx->vd.tx.chan = &siochan->vc.chan;
			sio_tx_free(&siotx->vd);
			return NULL;
		}

		/*
		 * T6030: publish the whole descriptor, its other fields zero;
		 * a slot may come back from an earlier transfer.
		 */
		if (sio_is_t6030(siochan->host))
			memset(d, 0, sizeof(*d));
		d->flag = 1; /* not sure what's up with this */
		d->iova = buf_addr + period_len * i;
		d->size = period_len;
	}
	dma_wmb();

	return vchan_tx_prep(&siochan->vc, &siotx->vd, flags);
}

static enum dma_status sio_tx_status(struct dma_chan *chan, dma_cookie_t cookie,
				     struct dma_tx_state *txstate)
{
	struct sio_chan *siochan = to_sio_chan(chan);
	struct virt_dma_desc *vd;
	struct sio_tx *siotx;
	enum dma_status ret;
	unsigned long flags;
	int periods_residue;
	size_t residue;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret == DMA_COMPLETE || !txstate)
		return ret;

	spin_lock_irqsave(&siochan->vc.lock, flags);
	siotx = siochan->current_tx;

	if (siotx && siotx->vd.tx.cookie == cookie) {
		ret = DMA_IN_PROGRESS;
		periods_residue = siotx->next - siotx->ninflight;
		while (periods_residue < 0)
			periods_residue += siotx->nperiods;
		residue = (siotx->nperiods - periods_residue) * siotx->period_len;
	} else {
		ret = DMA_IN_PROGRESS;
		residue = 0;
		vd = vchan_find_desc(&siochan->vc, cookie);
		if (vd) {
			siotx = to_sio_tx(&vd->tx);
			residue = siotx->period_len * siotx->nperiods;
		}
	}
	spin_unlock_irqrestore(&siochan->vc.lock, flags);
	dma_set_residue(txstate, residue);

	return ret;
}

static bool sio_fill_in_locked(struct sio_chan *siochan);

static void sio_handle_issue_ack(struct sio_chan *siochan, void *cookie, bool ok)
{
	dma_cookie_t tx_cookie = (unsigned long) cookie;
	unsigned long flags;
	struct sio_tx *tx;

	if (!ok) {
		dev_err(siochan->host->dev, "nacked issue on chan %d\n", siochan->no);
		return;
	}

	spin_lock_irqsave(&siochan->vc.lock, flags);
	if (!siochan->current_tx || tx_cookie != siochan->current_tx->vd.tx.cookie ||
			siochan->current_tx->terminated)
		goto out;

	tx = siochan->current_tx;
	tx->next = (tx->next + 1) % tx->nperiods;
	tx->ninflight++;
	sio_fill_in_locked(siochan);

out:
	spin_unlock_irqrestore(&siochan->vc.lock, flags);
}

static bool sio_fill_in_locked(struct sio_chan *siochan)
{
	struct sio_data *sio = siochan->host;
	struct sio_tx *tx = siochan->current_tx;
	struct sio_coproc_desc *d = tx->siodesc[tx->next];
	int ret;

	if (tx->ninflight >= SIO_MAX_NINFLIGHT || tx->terminated)
		return false;

	static_assert(sizeof(dma_cookie_t) <= sizeof(void *));
	ret = sio_send_siomsg_atomic(sio, FIELD_PREP(SIOMSG_EP, siochan->no) |
				     FIELD_PREP(SIOMSG_TYPE, MSG_ISSUE) |
				     FIELD_PREP(SIOMSG_DATA, sio_coproc_desc_slot(sio, d)),
				     sio_handle_issue_ack, (void *) (uintptr_t) tx->vd.tx.cookie);
	if (ret < 0)
		dev_err_ratelimited(sio->dev, "can't issue on chan %d ninflight %d: %d\n",
				    siochan->no, tx->ninflight, ret);
	return true;
}

static void sio_update_current_tx_locked(struct sio_chan *siochan)
{
	struct virt_dma_desc *vd = vchan_next_desc(&siochan->vc);

	if (vd && !siochan->current_tx) {
		list_del(&vd->node);
		siochan->current_tx = to_sio_tx(&vd->tx);
		sio_fill_in_locked(siochan);
	}
}

static void sio_issue_pending(struct dma_chan *chan)
{
	struct sio_chan *siochan = to_sio_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&siochan->vc.lock, flags);
	vchan_issue_pending(&siochan->vc);
	sio_update_current_tx_locked(siochan);
	spin_unlock_irqrestore(&siochan->vc.lock, flags);
}

static int sio_terminate_all(struct dma_chan *chan)
{
	struct sio_chan *siochan = to_sio_chan(chan);
	unsigned long flags;
	LIST_HEAD(to_free);

	spin_lock_irqsave(&siochan->vc.lock, flags);
	if (siochan->current_tx && !siochan->current_tx->terminated) {
		dma_cookie_complete(&siochan->current_tx->vd.tx);
		siochan->current_tx->terminated = true;
		schedule_work(&siochan->terminate_wq);
	}
	vchan_get_all_descriptors(&siochan->vc, &to_free);
	spin_unlock_irqrestore(&siochan->vc.lock, flags);

	vchan_dma_desc_free_list(&siochan->vc, &to_free);

	return 0;
}

static void sio_terminate_work(struct work_struct *wq)
{
	struct sio_chan *siochan = container_of(wq, struct sio_chan, terminate_wq);
	struct sio_tx *tx;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&siochan->vc.lock, flags);
	tx = siochan->current_tx;
	spin_unlock_irqrestore(&siochan->vc.lock, flags);

	if (WARN_ON(!tx))
		return;

	ret = sio_call(siochan->host, FIELD_PREP(SIOMSG_EP, siochan->no) |
				      FIELD_PREP(SIOMSG_TYPE, MSG_TERMINATE));
	if (ret < 0)
		dev_err(siochan->host->dev, "terminate call on chan %d failed: %d\n",
			siochan->no, ret);

	ret = wait_for_completion_timeout(&tx->done, msecs_to_jiffies(500));
	if (!ret)
		dev_err(siochan->host->dev, "terminate descriptor wait timed out\n");

	tasklet_kill(&siochan->vc.task);

	spin_lock_irqsave(&siochan->vc.lock, flags);
	WARN_ON(siochan->current_tx != tx);
	siochan->current_tx = NULL;
	sio_update_current_tx_locked(siochan);
	spin_unlock_irqrestore(&siochan->vc.lock, flags);

	sio_tx_free(&tx->vd);
}

static void sio_synchronize(struct dma_chan *chan)
{
	struct sio_chan *siochan = to_sio_chan(chan);

	flush_work(&siochan->terminate_wq);
}

static void sio_free_chan_resources(struct dma_chan *chan)
{
	sio_terminate_all(chan);
	sio_synchronize(chan);
	vchan_free_chan_resources(&to_sio_chan(chan)->vc);
}

static struct dma_chan *sio_dma_of_xlate(struct of_phandle_args *dma_spec,
					 struct of_dma *ofdma)
{
	struct sio_data *sio = (struct sio_data *) ofdma->of_dma_data;
	unsigned int index = dma_spec->args[0];

	if (dma_spec->args_count != 1 || index >= sio->nchannels)
		return ERR_PTR(-EINVAL);

	return dma_get_slave_channel(&sio->channels[index].vc.chan);
}

static void sio_rtk_crashed(void *cookie, const void *crashlog, size_t crashlog_size)
{
	struct sio_data *sio = cookie;

	dev_err(sio->dev, "SIO down (crashed)");
}

static void sio_process_report(struct sio_chan *siochan)
{
	unsigned long flags;

	spin_lock_irqsave(&siochan->vc.lock, flags);
	if (siochan->current_tx) {
		struct sio_tx *tx = siochan->current_tx;

		if (tx->ninflight)
			tx->ninflight--;
		vchan_cyclic_callback(&tx->vd);
		if (!sio_fill_in_locked(siochan) && !tx->ninflight)
			complete(&tx->done);
	}
	spin_unlock_irqrestore(&siochan->vc.lock, flags);
}

static void sio_recv_msg(void *cookie, u8 ep, u64 msg)
{
	struct sio_data *sio = cookie;
	struct sio_tagdata *tags = &sio->tags;
	u32 data;
	u8 type, tag, sioep;

	if (ep != EP_SIO)
		goto unknown;

	data  = FIELD_GET(SIOMSG_DATA, msg);
	// param = FIELD_GET(SIOMSG_PARAM, msg);
	type  = FIELD_GET(SIOMSG_TYPE, msg);
	tag   = FIELD_GET(SIOMSG_TAG, msg);
	sioep = FIELD_GET(SIOMSG_EP, msg);

	switch (type) {
	case MSG_STARTED:
		dev_info(sio->dev, "SIO protocol v%u\n", data);
		type = MSG_ACK; /* Pretend this is an ACK */
		fallthrough;
	case MSG_ACK:
	case MSG_NACK:
		if (WARN_ON(tag >= SIO_NTAGS))
			break;

		if (tags->atomic[tag]) {
			sio_ack_callback callback = tags->ack_callback[tag];

			if (callback && !WARN_ON(sioep >= sio->nchannels))
				callback(&sio->channels[sioep],
					 tags->cookie[tag], type == MSG_ACK);
			if (type == MSG_NACK)
				dev_err(sio->dev, "got a NACK on channel %d\n", sioep);
			sio_free_tag(sio, tag);
		} else {
			tags->acked[tag] = (type == MSG_ACK);
			complete(&tags->completions[tag]);
		}
		break;

	case MSG_REPORT:
		if (WARN_ON(sioep >= sio->nchannels))
			break;

		sio_process_report(&sio->channels[sioep]);
		break;

	default:
		goto unknown;
	}
	return;

unknown:
	dev_warn(sio->dev, "received unknown message: ep %x data %016llx\n",
		 ep, msg);
}

static int _sio_send_siomsg(struct sio_data *sio, u64 msg, bool atomic,
			    sio_ack_callback ack_callback, void *cookie)
{
	int tag, ret;

	tag = sio_alloc_tag(sio);
	if (tag < 0)
		return tag;

	if (atomic)
		sio_set_tag_atomic(sio, tag, ack_callback, cookie);
	else
		reinit_completion(&sio->tags.completions[tag]);

	msg &= ~SIOMSG_TAG;
	msg |= FIELD_PREP(SIOMSG_TAG, tag);
	ret = apple_rtkit_send_message(sio->rtk, EP_SIO, msg, NULL,
				       atomic);
	if (ret < 0) {
		sio_free_tag(sio, tag);
		return ret;
	}

	return tag;
}

static int sio_send_siomsg(struct sio_data *sio, u64 msg)
{
	return _sio_send_siomsg(sio, msg, false, NULL, NULL);
}

static int sio_send_siomsg_atomic(struct sio_data *sio, u64 msg,
				  sio_ack_callback ack_callback,
				  void *cookie)
{
	return _sio_send_siomsg(sio, msg, true, ack_callback, cookie);
}

static int sio_call(struct sio_data *sio, u64 msg)
{
	int tag, ret;

	tag = sio_send_siomsg(sio, msg);
	if (tag < 0)
		return tag;

	ret = wait_for_completion_timeout(&sio->tags.completions[tag],
					  msecs_to_jiffies(SIO_CALL_TIMEOUT_MS));
	if (!ret) {
		dev_warn(sio->dev, "call %8llx timed out\n", msg);
		sio_free_tag(sio, tag);
		return -ETIME;
	}

	ret = sio->tags.acked[tag];
	sio_free_tag(sio, tag);

	return ret;
}

static const struct apple_rtkit_ops sio_rtkit_ops = {
	.crashed = sio_rtk_crashed,
	.recv_message = sio_recv_msg,
};

static int sio_device_config(struct dma_chan *chan,
			     struct dma_slave_config *config)
{
	struct sio_chan *siochan = to_sio_chan(chan);
	struct sio_data *sio = siochan->host;
	bool is_tx = sio_chan_direction(siochan->no) == DMA_MEM_TO_DEV;
	struct sio_shmem_chan_config *cfg_shmem = sio->shmem;
	struct sio_shmem_chan_config cfg;
	int ret;

	switch (is_tx ? config->dst_addr_width : config->src_addr_width) {
	case DMA_SLAVE_BUSWIDTH_1_BYTE:
		cfg.datashape = 0;
		break;
	case DMA_SLAVE_BUSWIDTH_2_BYTES:
		cfg.datashape = 1;
		break;
	case DMA_SLAVE_BUSWIDTH_4_BYTES:
		cfg.datashape = 2;
		break;
	default:
		return -EINVAL;
	}

	cfg.timeout = 0;
	cfg.fifo = 0x800;
	cfg.limit = 0x800;
	/*
	 * The ADT records each client's channel configuration with it (the
	 * dp-audioN dma-channels: channel, data shape, timeout, FIFO size,
	 * threshold). T6030's DisplayPort audio channels refill at 0x600.
	 */
	cfg.threshold = sio_is_t6030(sio) ? 0x600 : 0x800;

	/*
	 * Dmaengine prescribes we ought to apply the new configuration only
	 * to newly-queued descriptors.
	 *
	 * To comply with dmaengine's interface we take the lazy path here:
	 * we apply the configuration right away, we only allow the channel
	 * to be configured once, which means subsequent calls to `device_config`
	 * either return -EBUSY if the configuration differs, or they are
	 * a no-op if the configuration is the same as the starting one.
	 *
	 * This is the reasonable thing to do given that these sio channels
	 * are tied to fixed peripherals, and what's more given that the
	 * only planned consumer of this dmaengine driver in the kernel is
	 * diplayport audio support, where the DMA configuration is fixed,
	 * and no more than a single descriptor (a cyclic one) gets ever issued
	 * at the same time.
	 *
	 * The code complexity cost of tracking to which descriptor
	 * the configuration relates would be significant here, especially
	 * since we need to do a non-atomic operation to apply it (a call to
	 * the coprocessor) and dmaengine has its bunch of atomicity
	 * restrictions. And this complexity would be for naught since it
	 * doesn't even get exercised by the only planned consumer.
	 */
	if (siochan->configured && memcmp(&siochan->cfg, &cfg, sizeof(cfg)))
		return -EBUSY;

	*cfg_shmem = cfg;
	dma_wmb();

	ret = sio_call(sio, FIELD_PREP(SIOMSG_TYPE, MSG_CONFIGURE) |
			    FIELD_PREP(SIOMSG_EP, siochan->no));

	if (ret == 1)
		ret = 0;
	else if (ret == 0)
		ret = -EINVAL;

	if (ret == 0) {
		siochan->configured = true;
		siochan->cfg = cfg;
	}

	return ret;
}

static int sio_alloc_shmem(struct sio_data *sio)
{
	dma_addr_t iova;
	int err;

	sio->shmem = dma_alloc_coherent(sio->dev, SIO_SHMEM_SIZE,
					&iova, GFP_KERNEL | __GFP_ZERO);
	if (!sio->shmem)
		return -ENOMEM;

	sio->shmem_desc_base = (struct sio_coproc_desc *) (sio->shmem + 56);
	sio->desc_allocated = devm_kzalloc(sio->dev, SIO_NO_DESC_SLOTS / 32,
					   GFP_KERNEL);
	if (!sio->desc_allocated)
		return -ENOMEM;

	err = sio_call(sio, FIELD_PREP(SIOMSG_TYPE, MSG_SETUP) |
			    FIELD_PREP(SIOMSG_PARAM, 1) |
			    FIELD_PREP(SIOMSG_DATA, iova >> 12));
	if (err != 1) {
		if (err == 0)
			err = -EINVAL;
		return err;
	}

	err = sio_call(sio, FIELD_PREP(SIOMSG_TYPE, MSG_SETUP) |
			    FIELD_PREP(SIOMSG_PARAM, 2) |
			    FIELD_PREP(SIOMSG_DATA, SIO_SHMEM_SIZE));
	if (err != 1) {
		if (err == 0)
			err = -EINVAL;
		return err;
	}

	return 0;
}

static int sio_send_dt_params(struct sio_data *sio)
{
	struct device_node *np = sio->dev->of_node;
	const char *propname = "apple,sio-firmware-params";
	int nparams, err, i;

	nparams = of_property_count_u32_elems(np, propname);
	if (nparams < 0) {
		err = nparams;
		goto badprop;
	}

	for (i = 0; i < nparams / 2; i++) {
		u32 key, val;

		err = of_property_read_u32_index(np, propname, 2 * i, &key);
		if (err)
			goto badprop;
		err = of_property_read_u32_index(np, propname, 2 * i + 1, &val);
		if (err)
			goto badprop;

		err = sio_call(sio, FIELD_PREP(SIOMSG_TYPE, MSG_SETUP) |
				    FIELD_PREP(SIOMSG_PARAM, key & 0xff) |
				    FIELD_PREP(SIOMSG_EP, key >> 8) |
				    FIELD_PREP(SIOMSG_DATA, val));
		if (err < 1) {
			if (err == 0)
				err = -ENXIO;
			return dev_err_probe(sio->dev, err, "sending SIO parameter %#x value %#x\n",
					     key, val);
		}
	}

	return 0;

badprop:
	return dev_err_probe(sio->dev, err, "failed to read '%s'\n", propname);
}

static int sio_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct sio_data *sio;
	struct dma_device *dma;
	int nchannels;
	int err, i;

	/*
	 * Only APPLE_SIO_T6030 checks a T6030's boot tables and configures its
	 * channels: without it, leave the SIO stopped rather than start it as
	 * the generic one.
	 */
	if (!IS_ENABLED(CONFIG_APPLE_SIO_T6030) &&
	    of_device_is_compatible(np, "apple,t6030-sio"))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "T6030 SIO support (APPLE_SIO_T6030) not built; leaving the SIO stopped\n");

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(42));
	if (err)
		return dev_err_probe(&pdev->dev, err, "Failed to set DMA mask\n");

	err = of_property_read_u32(np, "dma-channels", &nchannels);
	if (err || nchannels > NCHANNELS_MAX)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing or invalid dma-channels property\n");

	sio = devm_kzalloc(&pdev->dev, struct_size(sio, channels, nchannels), GFP_KERNEL);
	if (!sio)
		return -ENOMEM;

	platform_set_drvdata(pdev, sio);
	sio->dev = &pdev->dev;
	sio->nchannels = nchannels;
#if IS_ENABLED(CONFIG_APPLE_SIO_T6030)
	sio->t6030 = of_device_is_compatible(np, "apple,t6030-sio");
	/* Before any register, power or firmware access */
	if (sio->t6030) {
		err = sio_t6030_check_boot_tables(&pdev->dev);
		if (err)
			return err;
	}
#endif

	sio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sio->base))
		return PTR_ERR(sio->base);

	pm_runtime_get_noresume(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	err = devm_pm_runtime_enable(&pdev->dev);
	if (err < 0)
		return dev_err_probe(&pdev->dev, err,
				     "pm_runtime_enable failed: %d\n", err);

	sio->rtk = devm_apple_rtkit_init(&pdev->dev, sio, NULL, 0, &sio_rtkit_ops);
	if (IS_ERR(sio->rtk)) {
		err = PTR_ERR(sio->rtk);
		dev_err(&pdev->dev, "couldn't initialize rtkit\n");
		goto rpm_put;
	}
	for (i = 1; i < SIO_NTAGS; i++)
		init_completion(&sio->tags.completions[i]);

	dma = &sio->dma;
	dma_cap_set(DMA_PRIVATE, dma->cap_mask);
	dma_cap_set(DMA_CYCLIC, dma->cap_mask);

	dma->dev = &pdev->dev;
	dma->device_free_chan_resources = sio_free_chan_resources;
	dma->device_tx_status = sio_tx_status;
	dma->device_issue_pending = sio_issue_pending;
	dma->device_terminate_all = sio_terminate_all;
	dma->device_synchronize = sio_synchronize;
	dma->device_prep_dma_cyclic = sio_prep_dma_cyclic;
	dma->device_config = sio_device_config;

	dma->directions = BIT(DMA_MEM_TO_DEV);
	dma->residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	dma->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
			       BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
			       BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);

	INIT_LIST_HEAD(&dma->channels);
	for (i = 0; i < nchannels; i++) {
		struct sio_chan *siochan = &sio->channels[i];

		siochan->host = sio;
		siochan->no = i;
		siochan->vc.desc_free = sio_tx_free;
		INIT_WORK(&siochan->terminate_wq, sio_terminate_work);
		vchan_init(&siochan->vc, dma);
	}

	writel(CPU_CONTROL_RUN, sio->base + REG_CPU_CONTROL);

	err = apple_rtkit_boot(sio->rtk);
	if (err)
		return dev_err_probe(&pdev->dev, err, "SIO did not boot\n");

	err = apple_rtkit_start_ep(sio->rtk, EP_SIO);
	if (err)
		return dev_err_probe(&pdev->dev, err, "starting SIO endpoint\n");

	err = sio_call(sio, FIELD_PREP(SIOMSG_TYPE, MSG_START));
	if (err < 1) {
		if (err == 0)
			err = -ENXIO;
		return dev_err_probe(&pdev->dev, err, "starting SIO service\n");
	}

	err = sio_send_dt_params(sio);
	if (err < 0)
		return dev_err_probe(&pdev->dev, err, "failed to send boot-up parameters\n");

	err = sio_alloc_shmem(sio);
	if (err < 0)
		return err;

	err = dma_async_device_register(&sio->dma);
	if (err)
		return dev_err_probe(&pdev->dev, err, "failed to register DMA device\n");

	err = of_dma_controller_register(pdev->dev.of_node, sio_dma_of_xlate, sio);
	if (err) {
		dma_async_device_unregister(&sio->dma);
		return dev_err_probe(&pdev->dev, err, "failed to register with OF\n");
	}

rpm_put:
	pm_runtime_put(&pdev->dev);

	return err;
}

static void sio_remove(struct platform_device *pdev)
{
	struct sio_data *sio = platform_get_drvdata(pdev);

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&sio->dma);
}

static const struct of_device_id sio_of_match[] = {
	{ .compatible = "apple,sio", },
	{ }
};
MODULE_DEVICE_TABLE(of, sio_of_match);

static __maybe_unused int sio_suspend(struct device *dev)
{
	/*
	 * TODO: SIO coproc sleep state
	 */
	return 0;
}

static __maybe_unused int sio_resume(struct device *dev)
{
	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(sio_pm_ops, sio_suspend, sio_resume, NULL);

static struct platform_driver apple_sio_driver = {
	.driver = {
		.name = "apple-sio",
		.of_match_table = sio_of_match,
		.pm             = pm_ptr(&sio_pm_ops),
	},
	.probe = sio_probe,
	.remove = sio_remove,
};
module_platform_driver(apple_sio_driver);

MODULE_AUTHOR("Martin Povišer <povik+lin@cutebit.org>");
MODULE_DESCRIPTION("Driver for SIO coprocessor on Apple SoCs");
MODULE_LICENSE("Dual MIT/GPL");

#if IS_ENABLED(CONFIG_APPLE_SIO_T6030_KUNIT_TEST)
#include "apple-sio-test.c"
#endif

#if IS_ENABLED(CONFIG_APPLE_SIO_KUNIT_TEST)
#include "apple-sio-probe-test.c"
#endif
