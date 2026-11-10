// SPDX-License-Identifier: GPL-2.0-only
/* Included by apple-dart.c: owner-driven ACIO, PCIe-C and external display DART power cycles. */
/*
 * Called before publishing the IOMMU: writer accounting cannot start mid-map.
 * A DART whose host can power-cycle it names its clock protection slice in the
 * device tree. The internal display DARTs (@skip) name theirs for their DCP's
 * own power-down instead.
 */
static int apple_dart_cio_init(struct apple_dart *dart, resource_size_t size, bool skip)
{
	struct device_node *np = dart->dev->of_node;
	struct apple_dart_cio *s;
	struct regmap *gapf;
	unsigned int slice;

	if (skip || dart->hw->type != DART_T8110 ||
	    !of_property_present(np, "apple,clock-protection"))
		return 0;
	if (size < 0x4064)
		return -EINVAL;
	gapf = syscon_regmap_lookup_by_phandle_args(np, "apple,clock-protection", 1, &slice);
	if (IS_ERR(gapf))
		return dev_err_probe(dart->dev, PTR_ERR(gapf), "clock-protection bank\n");
	if (slice >= APPLE_PS_PROT_SLICES)
		return -EINVAL;
	s = devm_kzalloc(dart->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dart = dart;
	s->gapf = gapf;
	s->slice = slice;
	dart->cio = s;
	return 0;
}

static void apple_dart_cio_unbind(void *data)
{
	struct apple_dart_cio *s = data;

	if (READ_ONCE(s->dart->cio_active))
		panic("Apple CIO: removing owner with unresolved DMA state");
	s->owner = NULL;
	module_put(THIS_MODULE);
}

int apple_dart_cio_bind(struct device *owner, struct device *provider,
			struct apple_dart_cio **state)
{
	struct apple_dart *dart;
	struct apple_dart_cio *s;
	int ret;

	if (!provider || provider->driver != &apple_dart_driver.driver)
		return -EPROBE_DEFER;
	dart = dev_get_drvdata(provider);
	if (!dart || !READ_ONCE(dart->iommu.ready))
		return -EPROBE_DEFER;
	s = dart->cio;
	if (!s || dart->managed_sids || dart->hw->ttbr_count != 1)
		return -EOPNOTSUPP;
	if (s->owner)
		return -EBUSY;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	s->link = device_link_add(owner, provider, DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!s->link) {
		module_put(THIS_MODULE);
		return -EINVAL;
	}
	s->owner = owner;
	ret = devm_add_action_or_reset(owner, apple_dart_cio_unbind, s);
	if (!ret)
		*state = s;
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_cio_bind);

int apple_dart_cio_prepare(struct apple_dart_cio *s)
{
	struct apple_dart *dart;
	unsigned long flags;
	int ret;

	if (!s || !s->owner || pm_suspend_target_state != PM_SUSPEND_MEM)
		return -EINVAL;
	dart = s->dart;
	ret = pm_runtime_resume_and_get(dart->dev);
	if (ret < 0)
		return ret;
	spin_lock_irqsave(&dart->managed_lock, flags);
	if (s->dead) {
		ret = -EIO;
	} else if (dart->cio_active || dart->sleep_closed || dart->commands_gated) {
		ret = -EBUSY;
	} else {
		s->saved.valid = false;
		s->changed = false;
		s->restored = false;
		s->tlb_stale = false;
		dart->cio_active = true;
		ret = 0;
	}
	spin_unlock_irqrestore(&dart->managed_lock, flags);
	if (ret)
		pm_runtime_put(dart->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_cio_prepare);

static int apple_dart_cio_gapf(struct apple_dart_cio *s, bool on)
{
	u32 value;
	int ret;

	ret = regmap_read(s->gapf, APPLE_PS_PROT_SLICE(s->slice), &value);
	/*
	 * An external display DART's slice word does not survive the sleep: give
	 * it the value it had at power-down back, before its protection goes on.
	 */
	if (!ret && on && !(value & BIT(1)) && (s->gapf_saved & BIT(1))) {
		u32 restore = s->gapf_saved | BIT(4);

		dev_info(s->dart->dev, "CIO DART slice %u lost (%#x): restoring %#x\n",
			 s->slice, value, restore);
		ret = regmap_write(s->gapf, APPLE_PS_PROT_SLICE(s->slice), restore);
		if (!ret)
			ret = regmap_read(s->gapf, APPLE_PS_PROT_SLICE(s->slice), &value);
	}
	if (ret || !(value & BIT(1)))
		return ret ?: -EPERM;
	ret = regmap_update_bits(s->gapf, APPLE_PS_PROT_SLICE(s->slice), BIT(4),
				 on ? BIT(4) : 0);
	if (!ret)
		ret = regmap_read(s->gapf, APPLE_PS_PROT_SLICE(s->slice), &value);
	return ret ?: (!!(value & BIT(4)) == on ? 0 : -EIO);
}

/* Returns whether a page table changed while it was closed. */
/*
 * Reopen a DART whose registers hold its state. Writers that updated page
 * tables in memory only finish first, and if there were any the whole TLB is
 * flushed before the DART opens, so no stale translation outlives the
 * closure. On failure it stays closed.
 */
static int apple_dart_cio_reopen(struct apple_dart_cio *s)
{
	struct apple_dart_stream_map all = { .dart = s->dart };
	struct apple_dart *dart = s->dart;
	unsigned long flags;
	unsigned int tries;
	bool stale;
	int ret;

	/* Under the lock its other writers hold; the restore was ordered by dma_wmb(). */
	scoped_guard(spinlock_irqsave, &dart->lock)
		WRITE_ONCE(dart->commands_gated, false);
	bitmap_fill(all.sidmap, dart->num_streams);
	for (tries = 0; tries < 10000; tries++) {
		stale = false;
		spin_lock_irqsave(&dart->managed_lock, flags);
		if (!s->closed_writers) {
			stale = s->tlb_stale;
			s->tlb_stale = false;
			if (!stale) {
				WRITE_ONCE(dart->sleep_closed, false);
				spin_unlock_irqrestore(&dart->managed_lock, flags);
				return 0;
			}
		}
		spin_unlock_irqrestore(&dart->managed_lock, flags);
		if (stale) {
			ret = dart->hw->invalidate_tlb(&all);
			if (ret)
				return ret;
			continue;
		}
		usleep_range(10, 20);
	}
	return -EBUSY;
}

int apple_dart_cio_off(struct apple_dart_cio *s)
{
	u32 value, params0, params3;
	struct apple_dart *dart;
	unsigned int streams;
	bool partitions;
	int ret;

	if (!s || !READ_ONCE(s->dart->cio_active) || s->changed || s->restored)
		return -EINVAL;
	dart = s->dart;
	ret = apple_dart_sleep_close(dart);
	if (ret)
		return ret;
	/* The bank size comes from the hardware, not from the stream allocator. */
	params0 = readl(dart->regs);
	params3 = readl(dart->regs + 12);
	streams = FIELD_GET(GENMASK(8, 0), params3);
	partitions = (params0 & BIT(29)) &&
		     FIELD_GET(GENMASK(24, 16), params3) >= DART_CIO_PARTITIONS;
	/* Drains the fault IRQ and any hardware command admitted before closure. */
	apple_dart_quiesce_commands(dart->dev);
	dma_wmb();
	ret = regmap_read(s->gapf, APPLE_PS_PROT_SLICE(s->slice), &value);
	if (!ret && !(value & BIT(1)))
		ret = -EPERM;
	if (!ret)
		s->gapf_saved = value;
	/* A display DART's firmware may have left its clock protection off. */
	if (!ret && !(value & APPLE_PS_PROT_SLICE_ON))
		ret = apple_dart_cio_gapf(s, true);
	/*
	 * PCIe-C allocates from only 64 SIDs, but its register bank has 256.
	 * Preserve the entire bank, including the reserved bypass SIDs.
	 */
	if (!ret)
		ret = dart_cio_capture(dart->regs, &s->saved, streams, partitions);
	if (ret) {
		dev_err(dart->dev, "CIO DART snapshot failed: %d, params=%08x/%08x/%08x/%08x\n",
			ret, s->saved.params[0], s->saved.params[1],
			s->saved.params[2], s->saved.params[3]);
		/* Its registers were not changed: reopen it, flushing what changed meanwhile. */
		if (apple_dart_cio_reopen(s)) {
			s->dead = true;
			dev_err(dart->dev, "CIO DART not reopened: closed until reboot\n");
		}
		return ret;
	}
	dev_info(dart->dev, "CIO DART stop: version=%#x streams=%u partitions=%u slice=%u\n",
		 s->saved.common.version, s->saved.streams, s->saved.partitions, s->slice);
	/* Any later failure needs on(), even if the domain never went off. */
	s->changed = true;
	ret = dart_cio_disable(dart->regs, &s->saved);
	return ret ?: apple_dart_cio_gapf(s, false);
}
EXPORT_SYMBOL_GPL(apple_dart_cio_off);

int apple_dart_cio_on(struct apple_dart_cio *s)
{
	int ret;

	if (!s || !READ_ONCE(s->dart->cio_active))
		return -EINVAL;
	if (!s->changed || s->restored)
		return 0;
	ret = apple_dart_cio_gapf(s, true);
	if (ret)
		return ret;
	ret = dart_cio_restore(s->dart->regs, &s->saved);
	if (ret) {
		void __iomem *regs = s->dart->regs;
		unsigned int tz = 0, tcr = 0, ttbr = 0;

		for (unsigned int i = 0; s->saved.common.has_tz && i < DART_POWER_TZ_WORDS; i++)
			if ((readl(regs + dart_power_tz[i].offset) & dart_power_tz[i].mask) !=
			    s->saved.common.tz[i])
				tz |= BIT(i);
		for (unsigned int sid = 0; sid < s->saved.streams; sid++) {
			u32 now_tcr = readl(regs + 0x1000 + 4 * sid);
			u32 now_ttbr = readl(regs + 0x1400 + 4 * sid);

			tcr += now_tcr != s->saved.tcr[sid];
			ttbr += now_ttbr != s->saved.ttbr[sid];
			if (now_tcr != s->saved.tcr[sid] || now_ttbr != s->saved.ttbr[sid] ||
			    (s->saved.enabled[sid / 32] & BIT(sid % 32)))
				dev_err(s->dart->dev,
					"CIO DART SID%u: TCR %08x had %08x, TTBR %08x had %08x\n",
					sid, now_tcr, s->saved.tcr[sid], now_ttbr,
					s->saved.ttbr[sid]);
		}
		dev_err(s->dart->dev,
			"CIO DART restore failed: %d, params %08x/%08x/%08x/%08x had %08x/%08x/%08x/%08x\n",
			ret, readl(regs), readl(regs + 4), readl(regs + 8), readl(regs + 12),
			s->saved.params[0], s->saved.params[1], s->saved.params[2],
			s->saved.params[3]);
		dev_err(s->dart->dev,
			"CIO DART state: protect %08x lock %08x had %08x, streams %08x had %08x, error %08x/%08x, tz%s differs %#x, tcr/ttbr differ %u/%u\n",
			readl(regs + 0x200), readl(regs + 0x208), s->saved.common.lock,
			readl(regs + 0xc00), s->saved.enabled[0], readl(regs + 0x4000),
			readl(regs + 0x704), s->saved.common.has_tz ? "" : " (none)", tz, tcr,
			ttbr);
		return ret;
	}
	dma_wmb();
	/* Updates that came after the restore's flush are flushed; requesters still wait. */
	ret = apple_dart_cio_reopen(s);
	if (ret) {
		dev_err(s->dart->dev, "CIO DART reopen failed: %d\n", ret);
		return ret;
	}
	s->restored = true;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_cio_on);

/*
 * Always ends the transaction. If the DART was not restored it stays closed for
 * the rest of the boot, its streams off: page-table updates go to memory only
 * and its registers are never used again, so its consumers can still unmap,
 * be released and be removed. Returns -EIO then, and later prepares refuse.
 */
int apple_dart_cio_finish(struct apple_dart_cio *s)
{
	struct apple_dart *dart;
	unsigned long flags;
	int ret = 0;

	if (!s || !READ_ONCE(s->dart->cio_active))
		return -EINVAL;
	dart = s->dart;
	spin_lock_irqsave(&dart->managed_lock, flags);
	if (s->changed && !s->restored) {
		s->dead = true;
		ret = -EIO;
	}
	dart->cio_active = false;
	spin_unlock_irqrestore(&dart->managed_lock, flags);
	if (ret)
		dev_err(dart->dev, "CIO DART not restored: closed until reboot\n");
	pm_runtime_put(dart->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_cio_finish);
