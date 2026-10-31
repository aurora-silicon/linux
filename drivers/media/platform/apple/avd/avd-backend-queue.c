// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Tagged open-firmware queue. VP/FIFO setup and PP chaining run on the CM3. */
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include "avd.h"
#include "avd-inst.h"
#include "avd-open-protocol.h"
#include "avd-regs.h"

#define OPEN_MBOX0_STATUS 0x50
#define OPEN_MBOX0_TX 0x54
#define OPEN_MBOX0_EMPTY BIT(0)

struct avd_open_queue {
	void __iomem *sram;
	resource_size_t sram_size;
	bool enabled;
	u32 ring, header;
	struct avd_buf packets[AVD_MAX_FRAMES];
	struct avd_buf fifo[AVD_MAX_FRAMES];
};

void avd_open_queue_cleanup(struct avd_dev *avd)
{
	struct avd_open_queue *q = avd->open_priv;
	unsigned int i;

	if (!q)
		return;
	for (i = 0; i < AVD_MAX_FRAMES; i++) {
		avd_buf_free(avd, &q->packets[i]);
		avd_buf_free(avd, &q->fifo[i]);
	}
}

static void avd_open_queue_release(void *data)
{
	avd_open_queue_cleanup(data);
}

int avd_open_queue_probe(struct avd_dev *avd)
{
	struct avd_open_queue *q;
	struct resource *sram;

	if (avd->open_priv)
		return 0;
	q = devm_kzalloc(avd->dev, sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	avd->open_priv = q;
	sram = platform_get_resource_byname(avd->pdev, IORESOURCE_MEM, "sram");
	if (sram) {
		q->sram = devm_ioremap_resource(avd->dev, sram);
		if (IS_ERR(q->sram))
			return PTR_ERR(q->sram);
		q->sram_size = resource_size(sram);
	}
	return devm_add_action_or_reset(avd->dev, avd_open_queue_release, avd);
}

bool avd_open_is_queued(struct avd_dev *avd)
{
	struct avd_open_queue *q = avd->open_priv;

	return q && READ_ONCE(q->enabled);
}

int avd_open_queue_boot(struct avd_dev *avd)
{
	struct avd_open_queue *q = avd->open_priv;
	u32 info[16], dst, header;
	unsigned int i;
	int ret;

	WRITE_ONCE(q->enabled, false);
	/* Stop old DMA/IRQs and drain stale notifications before a new generation. */
	avd_shutdown(avd);
	for (i = 0; i < AVD_MAX_FRAMES + 1; i++) {
		if (readl(avd->mbox + AVD_REG_MBOX1_STATUS) & AVD_MBOX1_EMPTY)
			break;
		readl(avd->mbox + AVD_REG_MBOX1_RETRIEVE);
		writel(AVD_MBOX1_NOT_EMPTY, avd->mbox + AVD_REG_MBOX_IRQ_CLR);
	}
	ret = avd_boot(avd);
	if (ret)
		return ret;
	/* Old DT/firmware retains the serial compatibility transport. */
	if (!q->sram || q->sram_size < 0x9400)
		return 0;
	for (i = 0; i < ARRAY_SIZE(info); i++)
		info[i] = readl(q->sram + AVD_OPEN_INFO + i * 4);
	if (info[0] != AVD_OPEN_MAGIC)
		return 0;
	/* ABI 1 images retain the same legacy IRQ path until a queue doorbell. */
	if (info[1] == 1) {
		dev_info_once(avd->dev, "queue ABI 1 image uses the open compatibility transport\n");
		return 0;
	}
	if (info[1] != AVD_OPEN_VERSION || info[2] != AVD_MAX_FRAMES ||
	    info[3] != sizeof(struct avd_open_desc) || info[5] != AVD_OPEN_RING ||
	    info[6] != AVD_OPEN_PACKET_WORDS || info[10] != avd->variant->fifo_slots ||
	    info[4] > U32_MAX - AVD_OPEN_RING - AVD_MAX_FRAMES * AVD_OPEN_DESC_BYTES) {
		ret = -EPROTO;
		goto shutdown;
	}
	dst = info[4] + 0x9000;
	header = (dst & 0xfffeU) | ((dst & 0x10000U) << 1) | 1U;
	if (info[7] != header) {
		ret = -EPROTO;
		goto shutdown;
	}
	/* Both DART stream IDs must be attached to the device's DMA domain. */
	if (!avd->pm.piodma || !avd->domain ||
	    of_count_phandle_with_args(avd->dev->of_node, "iommus", "#iommu-cells") < 2) {
		dev_info_once(avd->dev, "queued PIODMA unavailable; using the open compatibility transport\n");
		return 0;
	}
	for (i = 0; i < 4; i++) {
		u32 base = avd->variant->vp_slot_base[i];

		if (!avd->variant->vp_slots[i])
			continue;
		if ((info[11 + i] & 0xff) != base ||
		    (info[11 + i] >> 8) != avd->variant->vp_slots[i] ||
		    base + avd->variant->vp_slots[i] > info[8]) {
			ret = -EPROTO;
			goto shutdown;
		}
	}
	avd->capabilities &= info[9];
	q->ring = info[4] + AVD_OPEN_RING;
	q->header = info[7];
	WRITE_ONCE(q->enabled, true);
	writel(AVD_MBOX_ENABLE, avd->mbox + OPEN_MBOX0_STATUS);
	return 0;
shutdown:
	avd_shutdown(avd);
	return ret;
}

unsigned int avd_open_queue_max(struct avd_dev *avd)
{
	return avd_open_is_queued(avd) ? avd->variant->fifo_slots : 1;
}

int avd_open_queue_submit(struct avd_dev *avd, struct avd_frame *f)
{
	struct avd_open_queue *q = avd->open_priv;
	struct avd_buf *packet = &q->packets[f->idx], *fifo = &q->fifo[f->idx];
	struct avd_open_desc desc = {};
	u32 *src = f->prog->cpu, *dst;
	size_t packets, bytes, p, n;
	unsigned int codec = f->cmd[0];
	void __iomem *record;
	int ret;

	static_assert(sizeof(desc) == AVD_OPEN_DESC_BYTES);
	if (f->idx >= avd->variant->fifo_slots || codec > AVD_CODEC_AV1 ||
	    !(avd->capabilities & BIT(codec)) || !src || !f->prog_words ||
	    f->prog_words > 0x100000 || f->prog_words > f->prog->size / sizeof(u32) ||
	    !f->cmd[1] || f->cmd[1] > f->prog_words)
		return -EINVAL;
	if (!(readl(avd->mbox + OPEN_MBOX0_STATUS) & OPEN_MBOX0_EMPTY))
		return -EBUSY;
	packets = DIV_ROUND_UP(f->prog_words, AVD_OPEN_PACKET_WORDS);
	if (check_mul_overflow(packets, (size_t)AVD_OPEN_PACKET_BYTES, &bytes))
		return -EOVERFLOW;
	if (packet->size < bytes) {
		ret = avd_buf_alloc(avd, packet, bytes);
		if (ret)
			return ret;
	}
	if (!fifo->cpu) {
		ret = avd_buf_alloc(avd, fifo, fifo_size());
		if (ret)
			return ret;
	}
	dst = packet->cpu;
	memset(dst, 0, bytes);
	for (p = 0; p < packets; p++) {
		size_t words = min_t(size_t, AVD_OPEN_PACKET_WORDS,
				    f->prog_words - p * AVD_OPEN_PACKET_WORDS);

		*dst++ = cpu_to_le32(q->header);
		for (n = 0; n < words; n++)
			dst[n] = cpu_to_le32(src[p * AVD_OPEN_PACKET_WORDS + n]);
		dst += AVD_OPEN_PACKET_WORDS;
	}
	desc.magic = cpu_to_le32(AVD_OPEN_MAGIC);
	desc.tag = cpu_to_le32(f->tag);
	desc.codec = cpu_to_le32(codec);
	desc.fifo = cpu_to_le32(f->idx);
	desc.program_lo = cpu_to_le32(lower_32_bits(packet->addr));
	desc.program_hi = cpu_to_le32(upper_32_bits(packet->addr));
	desc.program_words = cpu_to_le32(f->prog_words);
	desc.num_segments = cpu_to_le32(f->cmd[1]);
	desc.state = cpu_to_le32(1);
	desc.context = cpu_to_le32(f->ctx->client_id);
	desc.dependency = cpu_to_le32(f->dep);
	desc.fifo_lo = cpu_to_le32(lower_32_bits(fifo->addr));
	desc.fifo_hi = cpu_to_le32(upper_32_bits(fifo->addr));
	record = q->sram + AVD_OPEN_RING + f->idx * sizeof(desc);
	dma_wmb();
	memcpy_toio(record, &desc, sizeof(desc));
	/* Flush the descriptor aperture before ringing its doorbell. */
	wmb();
	readl(record + 0x20);
	writel(q->ring + f->idx * sizeof(desc), avd->mbox + OPEN_MBOX0_TX);
	return 0;
}

static void avd_open_queue_fault(struct avd_dev *avd)
{
	/* Reuse the core's whole-device reset, before any mappings can retire. */
	avd->queue_error = -EIO;
	mod_delayed_work(system_wq, &avd->queue_work, 0);
}

irqreturn_t avd_open_queue_irq(struct avd_dev *avd)
{
	struct avd_open_queue *q = avd->open_priv;
	unsigned int budget = AVD_MAX_FRAMES + 1;

	while (budget-- && !(readl(avd->mbox + AVD_REG_MBOX1_STATUS) & AVD_MBOX1_EMPTY)) {
		u32 word = readl(avd->mbox + AVD_REG_MBOX1_RETRIEVE);
		struct avd_frame *f = &avd->frames[word & 15];
		void __iomem *record = q->sram + AVD_OPEN_RING + f->idx * AVD_OPEN_DESC_BYTES;
		u32 state, status, tag;

		writel(AVD_MBOX1_NOT_EMPTY, avd->mbox + AVD_REG_MBOX_IRQ_CLR);
		if ((word & 0xffff0000U) != AVD_OPEN_NOTIFY) {
			avd_open_queue_fault(avd);
			break;
		}
		state = readl(record + 0x20);
		status = readl(record + 0x24);
		tag = readl(record + 0x28);
		if (state != 2 || !avd_open_completion(word, tag) || tag != f->tag ||
		    f->state != AVD_FRAME_SUBMITTED || status > 2) {
			avd_open_queue_fault(avd);
			break;
		}
		/* Fatal completions do not establish hardware/DMA quiescence. */
		if (status == 2) {
			avd_open_queue_fault(avd);
			break;
		}
		avd_frame_complete(avd, tag, status ? -EINVAL : 0);
	}
	return IRQ_HANDLED;
}
