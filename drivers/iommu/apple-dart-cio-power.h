/* SPDX-License-Identifier: GPL-2.0-only */
/* Register state for a T8132 CIO DART. Its host must exclude every requester. */
#ifndef __APPLE_DART_CIO_POWER_H
#define __APPLE_DART_CIO_POWER_H

#define DART_CIO_STREAMS 256
#define DART_CIO_PARTITIONS 16
#define DART_CIO_PARTITION_MASK 0x000f003f

struct dart_cio_power {
	struct dart_power_state common;
	u32 tcr[DART_CIO_STREAMS], ttbr[DART_CIO_STREAMS];
	u32 enabled[DART_CIO_STREAMS / 32];
	u32 partition[DART_CIO_PARTITIONS];
	u32 params[4];
	unsigned int streams, partitions;
	bool valid;
};

static bool dart_cio_fault(void __iomem *regs, const struct dart_cio_power *s)
{
	for (unsigned int i = 0; i < DIV_ROUND_UP(s->streams, 32); i++)
		if (readl(regs + 0x4000 + 4 * i))
			return true;
	return readl(regs + 0x704) & 0x777;
}

static int dart_cio_idle(void __iomem *regs, const struct dart_cio_power *s)
{
	unsigned int ports = max(FIELD_GET(GENMASK(18, 16), s->params[1]),
				 FIELD_GET(GENMASK(22, 20), s->params[1]));
	u32 value;
	int ret;

	if (!ports || ports > 4 || dart_cio_fault(regs, s))
		return -EIO;
	ret = dart_power_tlb_idle(regs);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < ports; i++) {
		ret = readl_poll_timeout_atomic(regs + 0x230 + 4 * i, value, !value,
						1, 10000);
		if (ret)
			return ret;
	}
	return dart_cio_fault(regs, s) ? -EIO : 0;
}

/* Only the v2.2/v2.3 layout is admitted; do not guess at a newer revision. */
static int dart_cio_capture(void __iomem *regs, struct dart_cio_power *s,
			    unsigned int streams, bool partitions)
{
	struct dart_power_state *p = &s->common;
	u32 version;
	int ret;

	s->valid = false;
	if (!streams || streams > DART_CIO_STREAMS)
		return -EINVAL;
	for (unsigned int i = 0; i < ARRAY_SIZE(s->params); i++)
		s->params[i] = readl(regs + 4 * i);
	version = s->params[2] & 0xffff;
	if (version < 0x202 || version > 0x203 ||
	    FIELD_GET(GENMASK(8, 0), s->params[3]) != streams)
		return -EOPNOTSUPP;
	if (partitions && (!(s->params[0] & BIT(29)) ||
			   FIELD_GET(GENMASK(24, 16), s->params[3]) < DART_CIO_PARTITIONS))
		return -EOPNOTSUPP;
	s->streams = streams;
	s->partitions = partitions ? DART_CIO_PARTITIONS : 0;
	ret = dart_cio_idle(regs, s);
	if (ret)
		return ret;
	/* This owner restores the normal unmasked error policy on wake. */
	if (readl(regs + DART_POWER_ERROR_MASK))
		return -EBUSY;
	p->version = version;
	p->lock = readl(regs + 0x208);
	p->diag = readl(regs + 0x210);
	p->pair[0] = readl(regs + 0x228);
	p->pair[1] = readl(regs + 0x22c);
	p->has_tz = s->params[1] & BIT(2);
	if (p->has_tz) {
		if ((p->lock & GENMASK(4, 3)) != GENMASK(4, 3))
			return -EPERM;
		for (unsigned int i = 0; i < DART_POWER_TZ_WORDS; i++)
			p->tz[i] = readl(regs + dart_power_tz[i].offset) &
				   dart_power_tz[i].mask;
	}
	for (unsigned int i = 0; i < DART_POWER_TUNABLES; i++)
		p->tunable[i] = readl(regs + dart_power_tunables[i].offset);
	for (unsigned int i = 0; i < s->partitions; i++)
		s->partition[i] = readl(regs + 0x800 + 4 * i);
	for (unsigned int sid = 0; sid < streams; sid++) {
		s->tcr[sid] = readl(regs + 0x1000 + 4 * sid);
		s->ttbr[sid] = readl(regs + 0x1400 + 4 * sid);
	}
	for (unsigned int i = 0; i < DIV_ROUND_UP(streams, 32); i++)
		s->enabled[i] = readl(regs + 0xc00 + 4 * i);
	if (streams % 32 && (s->enabled[streams / 32] & ~GENMASK(streams % 32 - 1, 0)))
		return -EUCLEAN;
	s->valid = true;
	return 0;
}

/* One command per SID; DISABLE_STREAMS has no readback equality contract. */
static void dart_cio_disable_streams(void __iomem *regs, const struct dart_cio_power *s)
{
	for (unsigned int sid = 0; sid < s->streams; sid++)
		if (s->enabled[sid / 32] & BIT(sid % 32))
			writel(BIT(sid % 32), regs + 0xc20 + 4 * (sid / 32));
	/* Complete the stream commands before flushing or releasing GAPF. */
	mb();
}

static int dart_cio_disable(void __iomem *regs, const struct dart_cio_power *s)
{
	int ret;

	if (!s->valid)
		return -ENODATA;
	ret = dart_cio_idle(regs, s);
	if (ret)
		return ret;
	dart_power_mask_errors(regs, true);
	dart_cio_disable_streams(regs, s);
	ret = dart_power_flush(regs, true);
	return ret ?: (dart_cio_fault(regs, s) ? -EIO : 0);
}

/* GAPF is already on; all requesters remain stopped until this returns zero. */
static int dart_cio_restore(void __iomem *regs, const struct dart_cio_power *s)
{
	const struct dart_power_state *p = &s->common;
	u32 protect, lock;
	bool locked;
	int ret;

	if (!s->valid)
		return -ENODATA;
	for (unsigned int i = 0; i < ARRAY_SIZE(s->params); i++)
		if (readl(regs + 4 * i) != s->params[i])
			return -EUCLEAN;
	protect = readl(regs + 0x200);
	lock = readl(regs + 0x208);
	locked = protect & lock & BIT(0);
	if ((lock & GENMASK(4, 3)) != (p->lock & GENMASK(4, 3)) || dart_cio_fault(regs, s))
		return -EUCLEAN;
	/* A requester outside the saved set means ownership was lost. */
	for (unsigned int i = 0; i < DIV_ROUND_UP(s->streams, 32); i++)
		if (readl(regs + 0xc00 + 4 * i) & ~s->enabled[i])
			return -EUCLEAN;
	dart_power_mask_errors(regs, true);
	/* Also handles an aborted shutdown with some streams still enabled. */
	dart_cio_disable_streams(regs, s);
	ret = dart_power_write(regs, 0x210, p->diag);
	if (ret)
		return ret;
	if (p->has_tz)
		for (unsigned int i = 0; i < DART_POWER_TZ_WORDS; i++)
			if ((readl(regs + dart_power_tz[i].offset) & dart_power_tz[i].mask) !=
			    p->tz[i])
				return -EUCLEAN;
	for (unsigned int i = 0; i < DART_POWER_TUNABLES; i++) {
		bool apf_locked = dart_power_tunables[i].offset >= 0x300 &&
				  (protect & lock & BIT(2));

		ret = dart_power_config(regs, dart_power_tunables[i].offset,
					dart_power_tunables[i].mask, p->tunable[i],
					locked || apf_locked);
		if (ret)
			return ret;
	}
	for (unsigned int i = 0; i < s->partitions; i++) {
		ret = dart_power_config(regs, 0x800 + 4 * i, DART_CIO_PARTITION_MASK,
					s->partition[i], locked);
		if (ret)
			return ret;
	}
	/* Include reserved bypass SIDs: they are part of the port's DMA context. */
	for (unsigned int sid = 0; sid < s->streams; sid++) {
		ret = dart_power_config(regs, 0x1000 + 4 * sid, ~0U, s->tcr[sid], locked);
		if (ret)
			return ret;
		/*
		 * A locked DART can come back from a sleep with other bits in the
		 * root of a stream that is not enabled, a root that was and still
		 * is invalid: no translation either way.
		 */
		if (locked && !(s->enabled[sid / 32] & BIT(sid % 32)) &&
		    !((s->ttbr[sid] | readl(regs + 0x1400 + 4 * sid)) & BIT(0)))
			continue;
		ret = dart_power_config(regs, 0x1400 + 4 * sid, ~0U, s->ttbr[sid], locked);
		if (ret)
			return ret;
	}
	ret = dart_power_flush(regs, false) ?:
	      dart_power_config(regs, 0x228, ~0U, p->pair[0], locked) ?:
	      dart_power_config(regs, 0x22c, ~0U, p->pair[1], locked);
	if (ret)
		return ret;
	dart_power_mask_errors(regs, false);
	for (unsigned int i = 0; i < DIV_ROUND_UP(s->streams, 32); i++) {
		ret = dart_power_write(regs, 0xc00 + 4 * i, s->enabled[i]);
		if (ret) {
			dart_power_mask_errors(regs, true);
			dart_cio_disable_streams(regs, s);
			/* The caller keeps requesters stopped even if cleanup fails. */
			dart_power_flush(regs, true);
			return ret;
		}
	}
	return 0;
}

#endif
