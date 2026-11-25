/* SPDX-License-Identifier: GPL-2.0-only */
/* The display DARTs' lease for the firmware 27 display sleep */
#define V27_CLIENTS 8
struct apple_dart_v27_lease {
	struct apple_dart *dart;
	struct device *owner;
	struct module *module;
	struct device *client[V27_CLIENTS];
	struct iommu_group *group[V27_CLIENTS];
	struct iommu_domain *domain[V27_CLIENTS];
	bool domain_used[V27_CLIENTS];
	unsigned int clients;
	u32 enabled[BITS_TO_U32(DART_MAX_STREAMS)];
	u32 tcr[DART_MAX_STREAMS], ttbr[DART_MAX_STREAMS];
	u32 allowed, protect, lock, gapf_saved;
	/* The GAPF slice's value while the DART runs and once its display has slept */
	u32 gapf_live, gapf_off;
	void __iomem *gapf;
	resource_size_t gapf_base;
	bool own_gapf, retain, changed, stopped, failed, pm_held;
};

/*
 * This DART's GAPF slice from the DT: its offset in the GAPF bank (the sleep controller's
 * "gapf" region) and its two values. 0 when not described.
 */
static resource_size_t apple_dart_v27_gapf_slice(struct device *dev, u32 values[2])
{
	struct device_node *np;
	struct resource bank;
	u32 offset;
	int idx, ret;

	if (of_property_read_u32(dev->of_node, "apple,gapf-slice", &offset) ||
	    of_property_read_u32_array(dev->of_node, "apple,gapf-slice-values", values, 2))
		return 0;
	np = of_find_compatible_node(NULL, NULL, "apple,t6050-sleep");
	if (!np)
		return 0;
	idx = of_property_match_string(np, "reg-names", "gapf");
	ret = idx < 0 ? idx : of_address_to_resource(np, idx, &bank);
	of_node_put(np);
	if (ret || offset % 4 || resource_size(&bank) < 4 ||
	    offset > resource_size(&bank) - 4)
		return 0;
	return bank.start + offset;
}

static bool apple_dart_v27_same(struct apple_dart_v27_lease *l)
{
	struct apple_dart *d = l->dart;
	unsigned int sid, word;

	if (d->v27_lost || apple_dart_fault_pending(d) ||
	    readl(d->regs + DART_T8110_PROTECT) != l->protect ||
	    readl(d->regs + DART_T8110_PROTECT_LOCK) != l->lock)
		return false;
	for (sid = 0; sid < d->num_streams; sid++) {
		u32 ttbr = readl(d->regs + DART_TTBR(d, sid, 0));

		if (readl(d->regs + DART_TCR(d, sid)) != l->tcr[sid])
			return false;
		if (ttbr == l->ttbr[sid])
			continue;
		/*
		 * A stream that was off before the sleep and had no table can read a
		 * different table word after a real sleep, never with the valid bit. The
		 * word is behind the lock, so nothing can write it back, and with the
		 * stream off and the word not valid it names no table: nothing retained
		 * was lost. A later attach does not trust the word either: an unlocked
		 * DART writes a fresh one, and a locked one refuses a word without the
		 * valid bit. Anything else is a loss: a stream that is enabled, or a
		 * valid bit before or after.
		 */
		if ((l->enabled[sid / 32] & BIT(sid % 32)) ||
		    ((ttbr | l->ttbr[sid]) & DART_T8110_TTBR_VALID))
			return false;
		/* Once per stream per boot. */
		if (!(d->v27_stray[sid / 32] & BIT(sid % 32))) {
			d->v27_stray[sid / 32] |= BIT(sid % 32);
			dev_info(d->dev, "V27: stream %u is off and its table word is not valid: %#x before the sleep, %#x now; not a loss\n",
				 sid, l->ttbr[sid], ttbr);
		}
	}
	for (word = 0; word < BITS_TO_U32(d->num_streams); word++) {
		u32 now = readl(d->regs + DART_T8110_ENABLE_STREAMS + 4 * word);

		/*
		 * Stopped streams read enabled again after a real sleep, with the tables and
		 * locks as saved and the slice at its reset value; the finish below puts the
		 * slice back and enables them itself.
		 */
		if (now != (l->changed && !l->retain ? 0 : l->enabled[word]) &&
		    !(l->changed && pm_resume_via_firmware() && now == l->enabled[word]))
			return false;
	}
	return true;
}

int apple_dart_v27_check(struct apple_dart_v27_lease *l)
{
	unsigned int i;

	if (!l || l->failed || l->dart->m5_sleep_owner != l->owner ||
	    atomic_read_acquire(&l->dart->v27_ops) || !apple_dart_v27_same(l))
		return -EUCLEAN;
	for (i = 0; i < l->clients; i++)
		if (iommu_get_domain_for_dev(l->client[i]) != l->domain[i])
			return -EUCLEAN;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_v27_check);

int apple_dart_v27_claim(struct device *dev, struct device *owner,
	struct device *scanout, bool display, bool retain, struct apple_dart_v27_lease **result)
{
	static const struct { resource_size_t base; bool display; } ids[] = {
		{ 0x48a340000ULL, false }, { 0x2b2340000ULL, false },
		{ 0x392340000ULL, false }, { 0x302340000ULL, false },
		{ 0x306340000ULL, false }, { 0x48a300000ULL, true },
		{ 0x2b2300000ULL, true }, { 0x392300000ULL, true },
		{ 0x302300000ULL, true }, { 0x306300000ULL, true },
	};
	struct apple_dart_v27_lease *l;
	struct apple_dart_stream_map *client;
	struct apple_dart *d;
	struct resource *reg, *gapf;
	resource_size_t slice = 0;
	u32 slice_values[2] = {};
	unsigned int i, sid, word;
	unsigned long flags;
	int ret = -EBUSY, pending;

	if (!apple_sleep_v27_late() || !dev || !owner || !device_trylock(dev)) {
		if (dev && owner)
			dev_info(owner, "V27 DART claim: %s busy\n", dev_name(dev));
		return -EBUSY;
	}
	if (dev->driver != &apple_dart_driver.driver || !(d = dev_get_drvdata(dev)) ||
	    !d->v27_provider || down_trylock(&d->v27_lifecycle)) {
		device_unlock(dev);
		return -ENODEV;
	}
	if (!try_module_get(THIS_MODULE)) {
		up(&d->v27_lifecycle);
		device_unlock(dev);
		return -ENODEV;
	}
	get_device(dev);
	device_unlock(dev); /* Never held across this provider's own PM callback. */
	l = kzalloc(sizeof(*l), GFP_KERNEL);
	if (!l) { ret = -ENOMEM; goto unlock; }
	l->dart = d;
	l->owner = owner;
	l->module = THIS_MODULE;
	l->retain = retain;
	ret = apple_sleep_display_v27_power_ready(owner);
	if (ret)
		goto free;
	/* A clockless locked display provider may be runtime-suspended while
	 * firmware still uses its retained tables. Pin usage and finish existing
	 * PM callbacks without requesting a cold runtime resume/reset. Physical
	 * SYS/FE/CPU ON comes from the manager above, not the RPM status flag.
	 */
	pm_runtime_get_noresume(dev);
	l->pm_held = true;
	pm_runtime_barrier(dev);
	reg = platform_get_resource(to_platform_device(dev), IORESOURCE_MEM, 0);
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		if (reg->start == ids[i].base && display == ids[i].display)
			break;
	if (i < ARRAY_SIZE(ids))
		slice = apple_dart_v27_gapf_slice(dev, slice_values);
	/* Only an external display DART, which has no slice, is retained: the others need theirs. */
	if (i < ARRAY_SIZE(ids) && !retain && !slice) {
		dev_warn(owner, "V27 DART claim: %s has no GAPF slice in the DT\n", dev_name(dev));
		ret = -EOPNOTSUPP;
		goto free;
	}
	if (i == ARRAY_SIZE(ids) || retain != (display && !slice) ||
	    d->version != 0x0204 || !d->locked || !d->fw_tree || d->num_clks ||
	    d->preserve_display || d->managed_sids || d->commands_gated || d->display_dark ||
	    d->sleep_saved || d->host_resumes || d->v27_lost) {
		dev_info(owner, "V27 DART claim: %s not a locked V27 DART (id %u)\n", dev_name(dev), i);
		ret = -EOPNOTSUPP;
		goto free;
	}
	/* Close config/attach/map publication before pinning the actual users.
	 * Unmaps still perform real leaf retirement/invalidation; no fake unmap.
	 */
	spin_lock_irqsave(&d->lock, flags);
	if (d->m5_sleep_owner) {
		spin_unlock_irqrestore(&d->lock, flags);
		dev_info(owner, "V27 DART claim: %s already owned\n", dev_name(dev));
		ret = -EBUSY;
		goto free;
	}
	d->m5_sleep_owner = owner;
	list_for_each_entry(client, &d->v27_clients, v27_link) {
		struct device *c = client->v27_dev;
		u32 allowed = !display ? BIT(23) : (c == scanout ? BIT(0) : BIT(16));

		if ((!display && c != owner) ||
		    (display && c != scanout && c->parent != owner) ||
		    l->clients == V27_CLIENTS ||
		    bitmap_weight(client->sidmap, d->num_streams) != 1 ||
		    find_first_bit(client->sidmap, d->num_streams) >= 32 ||
		    !(allowed & BIT(find_first_bit(client->sidmap, d->num_streams)))) {
			ret = -EPERM;
			break;
		}
		l->allowed |= allowed;
		l->client[l->clients++] = get_device(c);
	}
	spin_unlock_irqrestore(&d->lock, flags);
	if (ret == -EPERM) {
		dev_info(owner, "V27 DART claim: %s has an unexpected client\n", dev_name(dev));
		goto undo;
	}
	if (!display && !l->clients) { ret = -ENODEV; goto undo; }
	/* SID16 belongs to the retained firmware PIODMA even with no Linux map.
	 * SID0 is permitted only with the actual suspended DRM master above.
	 */
	l->allowed |= display ? BIT(16) : BIT(23);
	/*
	 * Streams the firmware enables with no Linux client: 3 and 4 on the internal display
	 * DART, 24 on the DCP's own. The stop disables them with the rest.
	 */
	if (display && slice)
		l->allowed |= BIT(3) | BIT(4);
	/*
	 * An external display DART is retained, never written. SID 0 can be enabled on
	 * DCPEXT0's with no Linux client; the table check below still applies to it.
	 */
	if (display && retain)
		l->allowed |= BIT(0);
	if (!display)
		l->allowed |= BIT(24);
	ret = read_poll_timeout(atomic_read_acquire, pending, !pending, 100, 250000,
		false, &d->v27_ops);
	if (ret)
		goto undo;
	for (word = 0; word < BITS_TO_U32(d->num_streams); word++) {
		l->enabled[word] = readl(d->regs + DART_T8110_ENABLE_STREAMS + 4 * word);
		if (l->enabled[word] & ~(word ? 0U : l->allowed)) {
			dev_info(owner, "V27 DART claim: %s streams %#x (word %u) beyond %#x\n",
				 dev_name(dev), l->enabled[word], word, l->allowed);
			ret = -EBUSY;
			goto undo;
		}
	}
	if (!display && !(l->enabled[0] & BIT(23))) { ret = -ENODEV; goto undo; }
	for (sid = 0; sid < d->num_streams; sid++) {
		u32 ttbr;

		l->tcr[sid] = readl(d->regs + DART_TCR(d, sid));
		l->ttbr[sid] = ttbr = readl(d->regs + DART_TTBR(d, sid, 0));
		if (sid < 32 && (l->allowed & BIT(sid)) && (ttbr & d->hw->ttbr_valid)) {
			phys_addr_t phys = (u64)(ttbr >> d->hw->ttbr_addr_field_shift) << d->hw->ttbr_shift;

			if (!apple_dart_fw_tree_table(d, phys)) {
				dev_info(owner, "V27 DART claim: %s SID %u table not iBoot's\n",
					 dev_name(dev), sid);
				ret = -EPERM;
				goto undo;
			}
		}
	}
	l->protect = readl(d->regs + DART_T8110_PROTECT);
	l->lock = readl(d->regs + DART_T8110_PROTECT_LOCK);
	sid = display ? 16 : 23;
	if (!(l->protect & DART_T8110_PROTECT_TTBR_TCR) ||
	    (l->tcr[sid] & (d->hw->tcr_enabled | d->hw->tcr_4level)) !=
	    (d->hw->tcr_enabled | d->hw->tcr_4level) ||
	    !(l->ttbr[sid] & d->hw->ttbr_valid)) {
		ret = -EUCLEAN; goto undo;
	}
	for (word = 0; word < l->clients; word++) {
		struct device *c = l->client[word];

		/* Brief own-device lock here only, before its late/noirq callback. */
		if (!device_trylock(c)) {
			dev_info(owner, "V27 DART claim: client %s busy\n", dev_name(c));
			ret = -EBUSY;
			goto undo;
		}
		l->group[word] = iommu_group_get(c);
		ret = iommu_device_use_default_domain(c);
		if (!ret) {
			l->domain_used[word] = true;
			l->domain[word] = iommu_get_domain_for_dev(c);
		}
		device_unlock(c);
		if (ret || !l->group[word] || !l->domain[word]) { ret = ret ?: -ENODEV; goto undo; }
	}
	if (!retain) {
		gapf = platform_get_resource_byname(to_platform_device(dev), IORESOURCE_MEM, "gapf");
		if (d->gapf && gapf && gapf->start == slice && resource_size(gapf) == 4) {
			l->gapf = d->gapf;
		} else {
			if (d->gapf || !request_mem_region(slice, 4, "apple-dart-gapf")) {
				ret = -EBUSY; goto undo;
			}
			l->own_gapf = true;
			l->gapf_base = slice;
			l->gapf = ioremap(l->gapf_base, 4);
			if (!l->gapf) { ret = -ENOMEM; goto undo; }
		}
		l->gapf_saved = readl(l->gapf);
		l->gapf_live = slice_values[0];
		l->gapf_off = slice_values[1];
		/* Its live value under a running DART, its off value once its display has slept. */
		if (l->gapf_saved != l->gapf_live && l->gapf_saved != l->gapf_off) {
			dev_info(owner, "V27 DART claim: %s GAPF slice %#x\n", dev_name(dev), l->gapf_saved);
			ret = -EOPNOTSUPP;
			goto undo;
		}
	}
	ret = apple_dart_v27_check(l);
	if (ret)
		goto undo;
	*result = l;
	return 0;
undo:
	for (word = 0; word < l->clients; word++) {
		if (l->domain_used[word])
			iommu_device_unuse_default_domain(l->client[word]);
		iommu_group_put(l->group[word]);
		put_device(l->client[word]);
	}
	if (l->own_gapf) {
		if (l->gapf) iounmap(l->gapf);
		release_mem_region(l->gapf_base, 4);
	}
	WRITE_ONCE(d->m5_sleep_owner, NULL);
free:
	if (l->pm_held)
		pm_runtime_put_noidle(dev);
	kfree(l);
unlock:
	module_put(THIS_MODULE);
	put_device(dev);
	up(&d->v27_lifecycle);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_v27_claim);

int apple_dart_v27_stop_atomic(struct apple_dart_v27_lease *l)
{
	struct apple_dart *d;
	u32 val;
	int ret, idle;

	ret = apple_dart_v27_check(l);
	if (ret || !irqs_disabled())
		return ret ?: -EPERM;
	d = l->dart;
	ret = read_poll_timeout_atomic(apple_dart_m5_idle, idle, idle != -EBUSY,
		1, DART_T8110_IDLE_TIMEOUT_US, false, d);
	if (ret || idle)
		goto fail;
	if (l->retain) {
		/* Display roots/streams/GAPF untouched, FE/SYS retained by manager.
		 * Caller has ACKed display-off + FW IDLE + RUN-clear, APs parked.
		 */
		l->stopped = true;
		return 0;
	}
	l->failed = true; /* partial mutation can never take a returning undo */
	l->changed = true;
	for (val = 0; val < d->num_streams; val++)
		__assign_bit(val, d->display_tcr_four, l->tcr[val] & d->hw->tcr_4level);
	WRITE_ONCE(d->display_dark, true);
	/* Before the streams go: every error masked, the exception word and the error status
	 * cleared.
	 */
	writel(U32_MAX, d->regs + DART_T8110_V2_ERROR_MASK);
	writel(0, d->regs + DART_T8110_EXCEPTION(0));
	writel(U32_MAX, d->regs + DART_T8110_V24_ERR_IRQ_STATUS);
	/* One write per enabled stream. */
	for (val = 0; val < 32; val++)
		if (l->enabled[0] & BIT(val))
			writel(BIT(val), d->regs + DART_T8110_DISABLE_STREAMS);
	if (apple_dart_m5_streams_unowned(d, 0)) { ret = -EIO; goto fail; }
	writel(DART_T8110_TLB_CMD_POWERDOWN, d->regs + DART_T8110_TLB_CMD);
	ret = readl_poll_timeout_atomic(d->regs + DART_T8110_TLB_CMD, val,
		!(val & DART_T8110_TLB_CMD_BUSY), 1, DART_T8110_FLUSH_BUSY_TIMEOUT);
	if (ret)
		goto fail;
	ret = read_poll_timeout_atomic(apple_dart_m5_idle, idle, idle != -EBUSY,
		1, DART_T8110_IDLE_TIMEOUT_US, false, d);
	if (ret || idle)
		goto fail;
	writel(l->gapf_off, l->gapf);
	if (readl(l->gapf) != l->gapf_off) {
		ret = -EIO; goto fail;
	}
	l->failed = false;
	l->stopped = true;
	return 0;
fail:
	l->failed = true;
	return ret ?: idle;
}
EXPORT_SYMBOL_GPL(apple_dart_v27_stop_atomic);

int apple_dart_v27_finish(struct apple_dart_v27_lease *l, bool stopped)
{
	struct apple_dart *d;
	struct device *provider;
	u32 val;
	unsigned int i;
	int ret, pending;

	if (!l || l->failed || (stopped && !l->stopped))
		return -EUCLEAN;
	d = l->dart;
	provider = d->dev;
	scoped_guard(spinlock_irqsave, &d->lock)
		d->v27_restoring = true;
	ret = read_poll_timeout(atomic_read_acquire, pending, !pending, 100, 250000,
		false, &d->v27_ops);
	if (ret)
		goto lost;
	if (!apple_dart_v27_same(l))
		goto lost;
	if (l->changed) {
		l->failed = true;
		writel(l->gapf_saved, l->gapf);
		if (readl(l->gapf) != l->gapf_saved)
			goto lost;
		writel(DART_T8110_TLB_CMD_POWERDOWN, d->regs + DART_T8110_TLB_CMD);
		ret = readl_poll_timeout(d->regs + DART_T8110_TLB_CMD, val,
			!(val & DART_T8110_TLB_CMD_BUSY), 1, DART_T8110_FLUSH_BUSY_TIMEOUT);
		if (ret)
			goto lost;
		writel(l->enabled[0], d->regs + DART_T8110_ENABLE_STREAMS);
		l->changed = false;
		if (!apple_dart_v27_same(l))
			goto lost;
		WRITE_ONCE(d->display_tlb_stale, false);
		WRITE_ONCE(d->display_dark, false);
		l->failed = false;
	}
	for (i = 0; i < l->clients; i++)
		if (!device_trylock(l->client[i])) {
			while (i--)
				device_unlock(l->client[i]);
			goto lost;
		}
	for (i = 0; i < l->clients; i++) {
		iommu_device_unuse_default_domain(l->client[i]);
		l->domain_used[i] = false;
		device_unlock(l->client[i]);
		iommu_group_put(l->group[i]);
		put_device(l->client[i]);
	}
	if (l->own_gapf) { iounmap(l->gapf); release_mem_region(l->gapf_base, 4); }
	WRITE_ONCE(d->m5_sleep_owner, NULL);
	WRITE_ONCE(d->v27_restoring, false);
	pm_runtime_put_noidle(d->dev);
	up(&d->v27_lifecycle);
	module_put(l->module);
	put_device(provider);
	kfree(l);
	return 0;
lost:
	{
		unsigned int sid, first = d->num_streams;

		for (sid = 0; sid < d->num_streams; sid++)
			if (readl(d->regs + DART_TCR(d, sid)) != l->tcr[sid] ||
			    readl(d->regs + DART_TTBR(d, sid, 0)) != l->ttbr[sid]) {
				first = sid;
				break;
			}
		dev_info(d->dev, "V27 finish refused (wait %d, lost %d, fault %d, changed %d): PROTECT %#x/%#x LOCK %#x/%#x ENABLE %#x/%#x GAPF %#x/%#x; first stream differing %u: TCR %#x/%#x TTBR %#x/%#x\n",
			 ret, d->v27_lost, apple_dart_fault_pending(d), l->changed,
			 readl(d->regs + DART_T8110_PROTECT), l->protect,
			 readl(d->regs + DART_T8110_PROTECT_LOCK), l->lock,
			 readl(d->regs + DART_T8110_ENABLE_STREAMS), l->enabled[0],
			 l->gapf ? readl(l->gapf) : 0, l->gapf_saved, first,
			 first < d->num_streams ? readl(d->regs + DART_TCR(d, first)) : 0,
			 first < d->num_streams ? l->tcr[first] : 0,
			 first < d->num_streams ? readl(d->regs + DART_TTBR(d, first, 0)) : 0,
			 first < d->num_streams ? l->ttbr[first] : 0);
	}
	l->failed = true;
	d->v27_lost = true;
	/* Lifetime mutex/module/domain/resource reservations stay pinned. No
	 * restart or buffer/table free after uncertain retained-state loss.
	 */
	return -EUCLEAN;
}
EXPORT_SYMBOL_GPL(apple_dart_v27_finish);
