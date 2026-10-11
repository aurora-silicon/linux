/* SPDX-License-Identifier: GPL-2.0-only */
/* Resident ANS uses native table 6; Linux owns queue and DMA lifetimes. */
#define APPLE_NVME_APIF_PAGES 257
#define APPLE_NVME_APIF_LIST_SIZE SZ_4K

static inline void **apple_nvme_iod_list(struct request *req);
static inline struct apple_nvme *
queue_to_apple_nvme(struct apple_nvme_queue *q);

static int apple_nvme_apif_translate(struct apple_nvme *anv, u64 address,
				     size_t size, u64 *pa)
{
	struct aurora_apif_op op = {
		.selector = AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE,
						 AURORA_APIF_TRANSLATE),
		.arg = { address, size },
	};
	int ret = aurora_apif_submit(anv->apif, &op, 1, NULL);

	if (!ret)
		*pa = op.ret;
	return ret;
}

static int apple_nvme_apif_handoff(struct apple_nvme *anv)
{
	static const char *const names[] = {
		"admin-sq",
		"admin-cq",
		"io-sq",
		"io-cq",
	};
	struct device_node *np = anv->dev->of_node;
	phys_addr_t addresses[ARRAY_SIZE(names)];
	int i, j;

	/* Only admit an explicit stopped mailbox and four pinned queue pages.
	 * CC.EN/RDY describe controller state, not ownership of the RTKit session.
	 */
	if (!IS_ENABLED(CONFIG_AURORA_APIF_MMIO) ||
	    !of_property_read_bool(np, "asahi,rtkit-quiesced") ||
	    !dev_is_dma_coherent(anv->dev) ||
	    of_count_phandle_with_args(np, "memory-region", NULL) !=
		    ARRAY_SIZE(names) ||
	    of_property_count_strings(np, "memory-region-names") !=
		    ARRAY_SIZE(names))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct device_node *node;
		struct reserved_mem *rmem;
		int index = of_property_match_string(np, "memory-region-names",
						     names[i]);

		if (index < 0)
			return index;
		node = of_parse_phandle(np, "memory-region", index);
		if (!node)
			return -EINVAL;
		rmem = of_reserved_mem_lookup(node);
		if (!rmem || !of_property_read_bool(node, "no-map") ||
		    !IS_ALIGNED(rmem->base, SZ_16K) || rmem->size != SZ_16K) {
			of_node_put(node);
			return -EINVAL;
		}
		addresses[i] = rmem->base;
		of_node_put(node);
		for (j = 0; j < i; j++)
			if (addresses[j] == addresses[i])
				return -EINVAL;
	}
	return 0;
}

static void apple_nvme_apif_free(void *data)
{
	struct apple_nvme *anv = data;

	if (READ_ONCE(anv->quarantined))
		return;
	if (anv->apif_scratch)
		dma_free_coherent(anv->dev, SZ_16K, anv->apif_scratch,
				  anv->apif_scratch_dma);
	kfree(anv->apif_ops);
	aurora_apif_put(anv->apif);
}

static int apple_nvme_apif_init(struct apple_nvme *anv)
{
	int ret;

	anv->apif = aurora_apif_get(anv->dev);
	if (IS_ERR(anv->apif))
		return PTR_ERR(anv->apif);
	ret = devm_add_action_or_reset(anv->dev, apple_nvme_apif_free, anv);
	if (ret)
		return ret;
	anv->apif_ops = kcalloc(APPLE_NVME_APIF_PAGES, sizeof(*anv->apif_ops),
				GFP_KERNEL);
	anv->apif_scratch = dma_alloc_coherent(
		anv->dev, SZ_16K, &anv->apif_scratch_dma, GFP_KERNEL);
	return anv->apif_ops && anv->apif_scratch ? 0 : -ENOMEM;
}

static void apple_nvme_apif_queue_free(struct apple_nvme *anv,
				       struct apple_nvme_queue *q)
{
	if ((q->apif_grants & BIT(0)) &&
	    apple_sart_remove_allowed_region(anv->sart, q->sq_dma_addr, SZ_16K))
		dev_err(anv->dev,
			"retaining pinned SQ grant after failed SART unmap\n");
	if ((q->apif_grants & BIT(1)) &&
	    apple_sart_remove_allowed_region(anv->sart, q->cq_dma_addr, SZ_16K))
		dev_err(anv->dev,
			"retaining pinned CQ grant after failed SART unmap\n");
	if (q->tcbs && apple_sart_remove_allowed_region(
			       anv->sart, q->tcb_dma_addr, SZ_16K)) {
		dev_err(anv->dev,
			"retaining TCB page after failed SART unmap\n");
		return;
	}
	if (q->tcbs)
		dma_free_coherent(anv->dev, SZ_16K, q->tcbs, q->tcb_dma_addr);
	/* SQ/CQ pages remain reserved for the entire machine boot. */
}

static int apple_nvme_apif_queue_alloc(struct apple_nvme *anv,
				       struct apple_nvme_queue *q)
{
	struct device_node *np = anv->dev->of_node;
	const char *names[] = { q->is_adminq ? "admin-sq" : "io-sq",
				q->is_adminq ? "admin-cq" : "io-cq" };
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct device_node *node;
		struct reserved_mem *rmem;
		void *buffer;
		int index = of_property_match_string(np, "memory-region-names",
						     names[i]);

		if (index < 0)
			return index;
		node = of_parse_phandle(np, "memory-region", index);
		rmem = of_reserved_mem_lookup(node);
		of_node_put(node);
		if (!rmem)
			return -EINVAL;
		buffer = devm_memremap(anv->dev, rmem->base, SZ_16K,
				       MEMREMAP_WB);
		if (IS_ERR(buffer))
			return PTR_ERR(buffer);
		ret = apple_sart_add_allowed_region(
			anv->sart, phys_to_dma(anv->dev, rmem->base), SZ_16K);
		if (ret)
			return ret;
		q->apif_grants |= BIT(i);
		memset(buffer, 0, SZ_16K);
		if (!i) {
			q->sqes = buffer;
			q->sq_dma_addr = phys_to_dma(anv->dev, rmem->base);
		} else {
			q->cqes = buffer;
			q->cq_dma_addr = phys_to_dma(anv->dev, rmem->base);
		}
	}
	q->tcbs = dma_alloc_coherent(anv->dev, SZ_16K, &q->tcb_dma_addr,
				     GFP_KERNEL);
	if (!q->tcbs)
		return -ENOMEM;
	ret = apple_sart_add_allowed_region(anv->sart, q->tcb_dma_addr, SZ_16K);
	if (ret) {
		dma_free_coherent(anv->dev, SZ_16K, q->tcbs, q->tcb_dma_addr);
		q->tcbs = NULL;
		return ret;
	}
	q->cq_phase = 1;
	return 0;
}

static int apple_nvme_apif_admin(struct apple_nvme *anv)
{
	struct aurora_apif_op ops[] = {
		{ .selector = AURORA_APIF_SELECTOR(6, 3), .arg = { 253, 2 } },
		{ .selector = AURORA_APIF_SELECTOR(6, 0) },
		{ .selector = AURORA_APIF_SELECTOR(6, 4),
		  .arg = { 0, APPLE_NVME_AQ_DEPTH - 1, 0,
			   APPLE_NVME_AQ_DEPTH - 1 } },
		{ .selector = AURORA_APIF_SELECTOR(6, 5),
		  .arg = { anv->hw->max_queue_depth - 1,
			   anv->hw->max_queue_depth - 1 } },
		{ .selector = AURORA_APIF_SELECTOR(6, 7) },
		{ .selector = AURORA_APIF_SELECTOR(6, 6) },
	};
	int ret;

	ret = apple_nvme_apif_translate(
		anv, dma_to_phys(anv->dev, anv->adminq.sq_dma_addr), SZ_16K,
		&ops[2].arg[0]);
	if (ret)
		return ret;
	ret = apple_nvme_apif_translate(
		anv, dma_to_phys(anv->dev, anv->adminq.cq_dma_addr), SZ_16K,
		&ops[2].arg[2]);
	if (ret)
		return ret;
	ret = apple_nvme_apif_translate(
		anv, dma_to_phys(anv->dev, anv->ioq.cq_dma_addr), SZ_16K,
		&ops[4].arg[0]);
	if (ret)
		return ret;
	ret = apple_nvme_apif_translate(
		anv, dma_to_phys(anv->dev, anv->ioq.sq_dma_addr), SZ_16K,
		&ops[5].arg[0]);
	if (ret)
		return ret;
	/* These queue control endpoints have void native results. */
	return aurora_apif_submit(anv->apif, ops, ARRAY_SIZE(ops), NULL);
}

/* The shared scratch page is protected by anv->lock, alongside CQ processing. */
static int apple_nvme_apif_request(struct apple_nvme_queue *q,
				   struct request *req, bool map)
{
	struct apple_nvme *anv = queue_to_apple_nvme(q);
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);
	u32 tag = nvme_tag_from_cid(iod->cmd.common.command_id);
	struct apple_nvmmu_tcb *copy =
		anv->apif_scratch + APPLE_NVME_APIF_LIST_SIZE;
	u64 *list = anv->apif_scratch;
	struct aurora_apif_op op;
	u64 first, second, scratch_pa;
	u32 count, i, done;
	int ret;

	lockdep_assert_held(&anv->lock);
	if (tag >= anv->hw->max_queue_depth)
		return -EINVAL;
	if (!map) {
		if (!iod->apif_registered)
			return 0;
		op = (struct aurora_apif_op){
			.selector = AURORA_APIF_SELECTOR(6, 2),
			.arg = { q->is_adminq ? 0 : 1, tag, 0 },
		};
		ret = aurora_apif_submit(anv->apif, &op, 1, NULL);
		if (ret || op.ret != 1)
			return ret ?: -EIO;
		iod->apif_registered = false;
		return 0;
	}
	if (anv->apif_uncertain)
		return -EIO;
	first = le64_to_cpu(iod->cmd.common.dptr.prp1);
	second = le64_to_cpu(iod->cmd.common.dptr.prp2);
	count = blk_rq_payload_bytes(req) ?
			DIV_ROUND_UP((first & (SZ_4K - 1)) +
					     blk_rq_payload_bytes(req),
				     SZ_4K) :
			!!first;
	if (count > APPLE_NVME_APIF_PAGES ||
	    (count == 1 && !IS_ALIGNED(first, SZ_4K)))
		return -EINVAL;
	if (count) {
		list[0] = first;
		if (count == 2)
			list[1] = second;
		else if (count > 2) {
			__le64 *prps = apple_nvme_iod_list(req)[0];

			for (i = 1; i < count; i++)
				list[i] = le64_to_cpu(prps[i - 1]);
		}
	}
	for (i = 0; i < count; i++) {
		if (i && !IS_ALIGNED(list[i], SZ_4K))
			return -EINVAL;
		anv->apif_ops[i] = (struct aurora_apif_op){
			.selector = AURORA_APIF_SELECTOR(AURORA_APIF_SERVICE,
							 AURORA_APIF_TRANSLATE),
			.arg = { dma_to_phys(anv->dev, list[i] & ~(SZ_4K - 1)),
				 SZ_4K },
		};
	}
	if (count) {
		ret = aurora_apif_submit(anv->apif, anv->apif_ops, count, NULL);
		if (ret)
			return ret;
		/* Commands and PRP chains carry DMA addresses directly. Until ANS
		 * supports another aperture, require the identity handoff contract.
		 */
		for (i = 0; i < count; i++)
			if (anv->apif_ops[i].ret != (list[i] & ~(SZ_4K - 1)))
				return -EINVAL;
	}
	ret = apple_nvme_apif_translate(
		anv, dma_to_phys(anv->dev, anv->apif_scratch_dma), SZ_16K,
		&scratch_pa);
	if (ret)
		return ret;
	memcpy(copy, &q->tcbs[tag], sizeof(*copy));
	copy->length = cpu_to_le16(
		count ? count - ((first & (SZ_4K - 1)) ? 2 : 1) : 0);
	op = (struct aurora_apif_op){
		.selector = AURORA_APIF_SELECTOR(6, 1),
		.arg = { q->is_adminq ? 0 : 1, tag,
			 scratch_pa + APPLE_NVME_APIF_LIST_SIZE, scratch_pa,
			 count },
	};
	dma_wmb();
	/* Mark before dispatch: a lost response cannot prove MAP did not run. */
	iod->apif_registered = true;
	ret = aurora_apif_submit(anv->apif, &op, 1, &done);
	if (ret)
		anv->apif_uncertain = true;
	return ret;
}

struct apple_nvme_apif_drain {
	int error;
};

static bool apple_nvme_apif_drain_request(struct request *req, void *data)
{
	struct apple_nvme_apif_drain *drain = data;
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);

	if (iod->apif_registered && apple_nvme_apif_request(iod->q, req, false))
		drain->error = -EIO;
	return true;
}

static int apple_nvme_apif_drain(struct apple_nvme *anv)
{
	struct apple_nvme_apif_drain drain = {};

	blk_mq_tagset_busy_iter(&anv->tagset, apple_nvme_apif_drain_request,
				&drain);
	blk_mq_tagset_busy_iter(&anv->admin_tagset,
				apple_nvme_apif_drain_request, &drain);
	if (!drain.error)
		anv->apif_uncertain = false;
	return drain.error;
}
