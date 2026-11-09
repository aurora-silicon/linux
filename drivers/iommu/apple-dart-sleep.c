// SPDX-License-Identifier: GPL-2.0-only
/* Included by apple-dart.c: J713 radio ownership across system sleep. */
#include <linux/soc/apple/sleep-spin.h>
#include "apple-dart-power.h"

struct apple_dart_sleep_state {
	struct apple_dart *dart;
	struct apple_dart_sleep *record;
	u64 generation;
	u32 tcr[DART_MAX_STREAMS];
	u32 ttbr[DART_MAX_STREAMS][DART_MAX_TTBR];
	u32 enabled[BITS_TO_U32(DART_MAX_STREAMS)];
	u32 protect, protect_lock, error_mask, lock;
	struct regmap *gapf;
	struct dart_power_state power;
	bool gapf_on;
	bool power_off;
};

static int apple_dart_sleep_writer_get(struct apple_dart *dart)
{
	unsigned long flags;
	int ret = 0;

	if (!dart->managed_sids)
		return 0;
	spin_lock_irqsave(&dart->managed_lock, flags);
	if (dart->sleep_closed) {
		if (dart->sleep_record)
			WRITE_ONCE(dart->sleep_record->result, -EUCLEAN);
		ret = -EBUSY;
	} else if (dart->sleep_writers == UINT_MAX) {
		ret = -EOVERFLOW;
	} else {
		dart->sleep_writers++;
	}
	spin_unlock_irqrestore(&dart->managed_lock, flags);
	return ret;
}

static void apple_dart_sleep_writer_put(struct apple_dart *dart)
{
	unsigned long flags;

	if (!dart->managed_sids)
		return;
	spin_lock_irqsave(&dart->managed_lock, flags);
	dart->sleep_writers--;
	spin_unlock_irqrestore(&dart->managed_lock, flags);
}

/* Close only at zero users, so an admitted writer may nest a TLB operation. */
static int apple_dart_sleep_close(struct apple_dart *dart)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&dart->managed_lock, flags);
	if (dart->sleep_closed || dart->sleep_writers)
		ret = -EBUSY;
	else
		dart->sleep_closed = true;
	spin_unlock_irqrestore(&dart->managed_lock, flags);
	return ret;
}

/* A void unmap/free caller may release backing memory when we return. */
static void __noreturn apple_dart_sleep_release_failed(void)
{
	panic("J713 radio: DMA state cannot be released safely");
}

static int apple_dart_sleep_valid(struct apple_dart_sleep *sleep)
{
	struct apple_dart_sleep_state *s;

	if (!sleep || !sleep->private || !sleep->active || sleep->owner != current)
		return -ESTALE;
	s = sleep->private;
	if (s->record != sleep || s->dart->sleep_record != sleep || !sleep->generation ||
	    s->generation != sleep->generation)
		return -ESTALE;
	return READ_ONCE(sleep->result) ?: (READ_ONCE(s->dart->managed_failed) ? -EUCLEAN : 0);
}

static void apple_dart_sleep_unlink(void *data)
{
	device_link_del(data);
}

int apple_dart_sleep_bind(struct device *backend, struct device *client,
			  struct apple_dart_sleep *sleep)
{
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(client);
	struct device_link *link;
	struct apple_dart_sleep_state *s;
	struct apple_dart *dart;
	int ret;

	if (!of_machine_is_compatible("apple,j713") || !cfg || !cfg->stream_maps[0].dart ||
	    cfg->stream_maps[1].dart)
		return -ENODEV;
	dart = cfg->stream_maps[0].dart;
	if ((!dart->managed_sids) || dart->hw->type != DART_T8110 || !READ_ONCE(dart->iommu.ready))
		return -EINVAL;
	link = device_link_add(backend, dart->dev, DL_FLAG_STATELESS);
	if (!link)
		return -EINVAL;
	ret = devm_add_action_or_reset(backend, apple_dart_sleep_unlink, link);
	if (ret)
		return ret;
	s = devm_kzalloc(backend, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dart = dart;
	s->record = sleep;
	if (dart->managed_sids) {
		struct resource res;

		if (of_address_to_resource(dart->dev->of_node, 0, &res) ||
		    res.start != 0x490000000ULL || resource_size(&res) != 0x20000)
			return -EINVAL;
		s->gapf = syscon_regmap_lookup_by_compatible("apple,t8132-ps-prot");
		if (IS_ERR(s->gapf))
			return PTR_ERR(s->gapf);
	}
	*sleep = (struct apple_dart_sleep){
		.private = s,
		.provider = dart->dev,
		.client = client,
	};
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_sleep_bind);

int apple_dart_sleep_begin(struct apple_dart_sleep *sleep, u64 generation)
{
	struct apple_dart_sleep_state *s;
	struct apple_dart *dart;
	unsigned long flags;
	int ret = 0;

	if (!sleep || !sleep->private || !generation || sleep->active || in_interrupt() ||
	    irqs_disabled() || pm_suspend_target_state != PM_SUSPEND_MEM)
		return -EINVAL;
	s = sleep->private;
	dart = s->dart;
	if (s->record != sleep)
		return -ESTALE;
	spin_lock_irqsave(&dart->managed_lock, flags);
	if (dart->sleep_record || dart->sleep_closed || dart->managed_failed) {
		ret = -EBUSY;
	} else {
		s->generation = generation;
		sleep->generation = generation;
		sleep->owner = current;
		sleep->result = 0;
		sleep->sealed = false;
		sleep->verified = false;
		sleep->active = true;
		s->power.valid = s->power_off = false;
		dart->sleep_record = sleep;
	}
	spin_unlock_irqrestore(&dart->managed_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_sleep_begin);

static int apple_dart_sleep_snapshot_error(struct apple_dart *dart, const char *field,
					   unsigned int index, u32 value, u32 expected)
{
	/* Leave diagnostics silent if a caller has disabled interrupts. */
	if (!irqs_disabled())
		dev_err(dart->dev, "sleep %s[%u]: observed=%#x expected=%#x\n", field, index, value,
			expected);
	return -EUCLEAN;
}

static int apple_dart_sleep_snapshot(struct apple_dart_sleep_state *s, bool verify)
{
	struct apple_dart *dart = s->dart;
	unsigned int sid, idx, count = dart->hw->ttbr_count;
	u32 value;

	if (READ_ONCE(dart->managed_failed))
		return apple_dart_sleep_snapshot_error(dart, "managed-failed", 0, 1, 0);
	if (dart->managed_sids &&
	    (apple_dart_fault_pending(dart) ||
	     (apple_dart_readl(dart, DART_T8110_TLB_CMD) & DART_T8110_TLB_CMD_BUSY)))
		return -EBUSY;
	value = apple_dart_readl(dart, dart->hw->lock);
	if (verify && value != s->lock)
		return apple_dart_sleep_snapshot_error(dart, "lock", 0, value, s->lock);
	if (!verify)
		s->lock = value;
	for (sid = 0; sid < dart->num_streams; sid++) {
		value = apple_dart_readl(dart, DART_TCR(dart, sid));
		if (verify && value != s->tcr[sid])
			return apple_dart_sleep_snapshot_error(dart, "tcr", sid, value,
							       s->tcr[sid]);
		if (!verify)
			s->tcr[sid] = value;
		for (idx = 0; idx < count; idx++) {
			value = apple_dart_readl(dart, DART_TTBR(dart, sid, idx));
			if (verify && value != s->ttbr[sid][idx])
				return apple_dart_sleep_snapshot_error(
					dart, "ttbr", sid * count + idx, value, s->ttbr[sid][idx]);
			if (!verify)
				s->ttbr[sid][idx] = value;
		}
	}
	if (!dart->managed_sids)
		return 0;
	for (idx = 0; idx < BITS_TO_U32(dart->num_streams); idx++) {
		value = apple_dart_readl(dart, dart->hw->enable_streams + 4 * idx);
		if (verify && value != s->enabled[idx])
			return apple_dart_sleep_snapshot_error(dart, "enabled", idx, value,
							       s->enabled[idx]);
		if (!verify)
			s->enabled[idx] = value;
	}
	value = apple_dart_readl(dart, DART_T8110_PROTECT);
	if (verify && value != s->protect)
		return apple_dart_sleep_snapshot_error(dart, "protect", 0, value, s->protect);
	if (!verify)
		s->protect = value;
	value = apple_dart_readl(dart, DART_T8110_PROTECT_LOCK);
	if (verify && value != s->protect_lock)
		return apple_dart_sleep_snapshot_error(dart, "protect-lock", 0, value,
						       s->protect_lock);
	if (!verify)
		s->protect_lock = value;
	value = apple_dart_readl(dart, dart->version >= 0x0202 ? DART_T8110_V2_ERROR_MASK :
								 DART_T8110_ERROR_MASK);
	if (verify && value != s->error_mask)
		return apple_dart_sleep_snapshot_error(dart, "error-mask", 0, value, s->error_mask);
	if (!verify)
		s->error_mask = value;
	if (s->error_mask)
		return apple_dart_sleep_snapshot_error(dart, "error-mask-admission", 0,
						       s->error_mask, 0);
	if (s->protect & DART_T8110_PROTECT_TTBR_TCR)
		return apple_dart_sleep_snapshot_error(dart, "protect-admission", 0, s->protect, 0);
	for (sid = 1; sid <= 2; sid++) {
		bool live = false;

		/* Admit both supported table depths, but no bypass or remapping. */
		value = s->tcr[sid];
		if (dart->four_level)
			value &= ~dart->hw->tcr_4level;
		if (value != DART_T8110_TCR_TRANSLATE_ENABLE)
			return apple_dart_sleep_snapshot_error(dart, "translation-admission", sid,
							       s->tcr[sid],
							       DART_T8110_TCR_TRANSLATE_ENABLE);
		for (idx = 0; idx < count; idx++)
			live |= !!(s->ttbr[sid][idx] & dart->hw->ttbr_valid);
		if (!live)
			return apple_dart_sleep_snapshot_error(
				dart, "live-root", sid, s->ttbr[sid][0], dart->hw->ttbr_valid);
	}
	return 0;
}

/* The native PCIe DART uses slice 28; no other slice or policy bit is changed. */
static int apple_dart_sleep_gapf(struct apple_dart_sleep_state *s, bool on)
{
	u32 value;
	int ret;

	ret = regmap_read(s->gapf, APPLE_PS_PROT_SLICE(28), &value);
	if (ret)
		return ret;
	if (!(value & BIT(1)))
		return -EPERM;
	ret = regmap_update_bits(s->gapf, APPLE_PS_PROT_SLICE(28), BIT(4), on ? BIT(4) : 0);
	if (!ret)
		ret = regmap_read(s->gapf, APPLE_PS_PROT_SLICE(28), &value);
	return ret ?: (!!(value & BIT(4)) == on ? 0 : -EIO);
}

static int apple_dart_sleep_power_capture(struct apple_dart_sleep_state *s)
{
	struct apple_dart *dart = s->dart;
	struct dart_power_state *p = &s->power;
	u32 value;
	int ret;

	p->valid = false;
	if (!s->gapf || dart->managed_sids != (BIT(1) | BIT(2)) || dart->hw->type != DART_T8110 ||
	    dart->hw->ttbr_count != 1 || dart->num_streams < 19 || dart->num_streams > 32 ||
	    dart->version < 0x202 || dart->version > 0x203)
		return -EOPNOTSUPP;
	ret = regmap_read(s->gapf, APPLE_PS_PROT_SLICE(28), &value);
	if (ret)
		return ret;
	if (!(value & BIT(1)))
		return -EPERM;
	/* A working native DART may inherit clock protection disabled. */
	s->gapf_on = !!(value & BIT(4));
	p->version = dart->version;
	if (dart_power_fault_pending(dart->regs, p))
		return -EBUSY;
	p->lock = s->protect_lock;
	p->diag = apple_dart_readl(dart, 0x210);
	p->pair[0] = apple_dart_readl(dart, 0x228);
	p->pair[1] = apple_dart_readl(dart, 0x22c);
	p->has_tz = apple_dart_readl(dart, 0x4) & BIT(2);
	if (p->has_tz) {
		if ((p->lock & GENMASK(4, 3)) != GENMASK(4, 3))
			return -EPERM;
		for (unsigned int i = 0; i < DART_POWER_TZ_WORDS; i++)
			p->tz[i] = apple_dart_readl(dart, dart_power_tz[i].offset) &
				   dart_power_tz[i].mask;
	}
	for (unsigned int i = 0; i < DART_POWER_TUNABLES; i++)
		p->tunable[i] = apple_dart_readl(dart, dart_power_tunables[i].offset);
	p->valid = true;
	return 0;
}

int apple_dart_sleep_power_ready(struct apple_dart_sleep *sleep)
{
	struct apple_dart_sleep_state *s;
	int ret = apple_dart_sleep_valid(sleep);

	if (ret)
		return ret;
	s = sleep->private;
	if (!sleep->sealed || sleep->verified || !s->dart->sleep_closed || s->power_off)
		return -EPERM;
	return s->power.valid ? 0 : -ENODATA;
}
EXPORT_SYMBOL_GPL(apple_dart_sleep_power_ready);

static int apple_dart_sleep_tlb_idle(void __iomem *regs)
{
	return apple_sleep_spin(regs + 0x80, BIT(31), 0, 10000) ?:
		       apple_sleep_spin(regs + 0x604, BIT(0), 0, 10000);
}

static int apple_dart_sleep_idle(struct apple_dart_sleep_state *s)
{
	struct apple_dart *dart = s->dart;
	u32 params = readl(dart->regs + DART_PARAMS2);
	unsigned int ports = max(FIELD_GET(DART_T8110_PARAMS2_READ_PORTS, params),
				 FIELD_GET(DART_T8110_PARAMS2_WRITE_PORTS, params));
	int ret;

	if (!ports || ports > 4 || dart_power_fault_pending(dart->regs, &s->power))
		return -EIO;
	ret = apple_dart_sleep_tlb_idle(dart->regs);
	if (ret)
		return ret;
	for (unsigned int port = 0; port < ports; port++) {
		ret = apple_sleep_spin(dart->regs + DART_T8110_OUTSTANDING(port), ~0U, 0,
				       DART_T8110_IDLE_TIMEOUT_US);
		if (ret)
			return ret;
	}
	return dart_power_fault_pending(dart->regs, &s->power) ? -EIO : 0;
}

/* The owner has stopped its requesters; port clocks still make the DART usable. */
int apple_dart_sleep_power_off(struct apple_dart_sleep *sleep)
{
	struct apple_dart_sleep_state *s;
	struct apple_dart *dart;
	int ret = apple_dart_sleep_power_ready(sleep);

	if (ret)
		return ret;
	if (in_interrupt() || irqs_disabled())
		return -EPERM;
	s = sleep->private;
	dart = s->dart;
	ret = apple_dart_sleep_snapshot(s, true);
	if (ret)
		return ret;
	ret = apple_dart_sleep_idle(s);
	if (ret)
		return ret;
	if (dart_power_fault_pending(dart->regs, &s->power))
		return -EBUSY;
	/* The fault IRQ must not access the window after the owner drops clocks. */
	WRITE_ONCE(dart->commands_gated, true);
	dart_power_mask_errors(dart->regs, true);
	for (unsigned int sid = 0; sid < dart->num_streams; sid++) {
		if (s->enabled[sid / 32] & BIT(sid % 32)) {
			apple_dart_writel(dart, BIT(sid % 32),
					  DART_T8110_DISABLE_STREAMS + 4 * (sid / 32));
			/* Complete each stream-disable command before the next one. */
			mb();
			apple_dart_readl(dart, DART_T8110_DISABLE_STREAMS + 4 * (sid / 32));
		}
	}
	for (unsigned int i = 0; i < BITS_TO_U32(dart->num_streams); i++)
		if (apple_dart_readl(dart, dart->hw->enable_streams + 4 * i)) {
			ret = -EIO;
			goto fail;
		}
	ret = apple_dart_sleep_tlb_idle(dart->regs);
	if (ret)
		goto fail;
	writel(0xa000, dart->regs + 0x80);
	ret = apple_dart_sleep_tlb_idle(dart->regs) ?: apple_dart_sleep_gapf(s, false);
	if (ret)
		goto fail;
	s->power_off = true;
	return 0;
fail:
	sleep->result = ret;
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_sleep_power_off);

/* Called after parent and port clocks, while PERST and endpoint BME stay held. */
int apple_dart_sleep_power_restore(struct apple_dart_sleep *sleep)
{
	struct apple_dart_sleep_state *s;
	struct apple_dart *dart;
	struct dart_power_state *p;
	u32 protect, lock;
	bool locked;
	int ret = apple_dart_sleep_valid(sleep);

	if (ret)
		return ret;
	s = sleep->private;
	dart = s->dart;
	p = &s->power;
	if (in_interrupt() || irqs_disabled() || !sleep->sealed || sleep->verified ||
	    !s->power_off || !p->valid || !READ_ONCE(dart->commands_gated) ||
	    !READ_ONCE(dart->sleep_closed))
		return -EPERM;
	ret = apple_dart_sleep_gapf(s, s->gapf_on);
	if (ret)
		goto fail;
	for (unsigned int i = 0; i < BITS_TO_U32(dart->num_streams); i++)
		if (apple_dart_readl(dart, dart->hw->enable_streams + 4 * i)) {
			ret = -EBUSY;
			goto fail;
		}
	if (dart_power_fault_pending(dart->regs, p)) {
		ret = -EBUSY;
		goto fail;
	}
	protect = apple_dart_readl(dart, DART_T8110_PROTECT);
	lock = apple_dart_readl(dart, DART_T8110_PROTECT_LOCK);
	locked = protect & lock & BIT(0);
	if ((lock & GENMASK(4, 3)) != (p->lock & GENMASK(4, 3)) ||
	    !!(apple_dart_readl(dart, 0x4) & BIT(2)) != p->has_tz) {
		ret = -EUCLEAN;
		goto fail;
	}
	dart_power_mask_errors(dart->regs, true);
	ret = dart_power_write(dart->regs, 0x210, p->diag);
	if (ret)
		goto fail;
	if (p->has_tz)
		for (unsigned int i = 0; i < DART_POWER_TZ_WORDS; i++)
			if ((apple_dart_readl(dart, dart_power_tz[i].offset) &
			     dart_power_tz[i].mask) != p->tz[i]) {
				ret = -EUCLEAN;
				goto fail;
			}
	for (unsigned int i = 0; i < DART_POWER_TUNABLES; i++) {
		bool apf_locked = dart_power_tunables[i].offset >= 0x300 &&
				  (protect & lock & BIT(2));

		ret = dart_power_config(dart->regs, dart_power_tunables[i].offset,
					dart_power_tunables[i].mask, p->tunable[i],
					locked || apf_locked);
		if (ret)
			goto fail;
	}
	for (unsigned int sid = 0; sid < dart->num_streams; sid++) {
		ret = dart_power_config(dart->regs, DART_TCR(dart, sid), ~0U,
					s->tcr[sid], locked) ?:
			      dart_power_config(dart->regs, DART_TTBR(dart, sid, 0), ~0U,
						s->ttbr[sid][0], locked);
		if (ret)
			goto fail;
	}
	ret = dart_power_flush(dart->regs, false)				    ?:
		      dart_power_config(dart->regs, 0x228, ~0U, p->pair[0], locked) ?:
		      dart_power_config(dart->regs, 0x22c, ~0U, p->pair[1], locked);
	if (ret)
		goto fail;
	dart_power_mask_errors(dart->regs, false);
	for (unsigned int i = 0; i < BITS_TO_U32(dart->num_streams); i++) {
		ret = dart_power_write(dart->regs, dart->hw->enable_streams + 4 * i, s->enabled[i]);
		if (ret)
			goto fail;
	}
	dma_wmb();
	sleep->verified = true;
	/* Publish the complete translation context before IRQ or mapping users. */
	smp_store_release(&dart->commands_gated, false);
	/* Mapping writers acquire the same completed restore through this gate. */
	smp_store_release(&dart->sleep_closed, false);
	return 0;
fail:
	sleep->result = ret;
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dart_sleep_power_restore);

int apple_dart_sleep_seal(struct apple_dart_sleep *sleep)
{
	struct apple_dart_sleep_state *s;
	int ret = apple_dart_sleep_valid(sleep);

	if (ret)
		return ret;
	if (irqs_disabled() || sleep->sealed)
		return -EPERM;
	s = sleep->private;
	ret = apple_dart_sleep_close(s->dart);
	if (ret)
		return ret;
	/* No table publication/invalidation is still in flight beyond this gate. */
	dma_wmb();
	ret = apple_dart_sleep_snapshot(s, false);
	if (!ret && s->dart->managed_sids)
		ret = apple_dart_sleep_power_capture(s);
	if (ret) {
		/* Corrupt live roots must not regain DMA even on pre-entry unwind. */
		if (ret == -EUCLEAN) {
			sleep->result = -EUCLEAN;
			return ret;
		}
		/* Publish cancellation before allowing table writers again. */
		smp_store_release(&s->dart->sleep_closed, false);
		return ret;
	}
	sleep->sealed = true;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_sleep_seal);

int apple_dart_sleep_abort(struct apple_dart_sleep *sleep)
{
	struct apple_dart_sleep_state *s;
	int ret = apple_dart_sleep_valid(sleep);

	if (ret)
		return ret;
	if (irqs_disabled() || sleep->verified)
		return -EPERM;
	s = sleep->private;
	if (s->power_off || READ_ONCE(s->dart->commands_gated))
		return -EPERM;
	if (sleep->sealed) {
		ret = apple_dart_sleep_snapshot(s, true);
		if (ret) {
			sleep->result = -EUCLEAN;
			return -EUCLEAN;
		}
	}
	/* The saved translation state was checked before reopening writers. */
	smp_store_release(&s->dart->sleep_closed, false);
	sleep->sealed = false;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_sleep_abort);

int apple_dart_sleep_finish(struct apple_dart_sleep *sleep)
{
	struct apple_dart_sleep_state *s;
	unsigned long flags;
	int ret = apple_dart_sleep_valid(sleep);

	if (ret)
		return ret;
	s = sleep->private;
	if (irqs_disabled() || READ_ONCE(s->dart->sleep_closed))
		return -EPERM;
	spin_lock_irqsave(&s->dart->managed_lock, flags);
	s->dart->sleep_record = NULL;
	sleep->active = false;
	sleep->owner = NULL;
	spin_unlock_irqrestore(&s->dart->managed_lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dart_sleep_finish);
