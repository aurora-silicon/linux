/* SPDX-License-Identifier: GPL-2.0-only */
/* Validated DATA access (25F84/25G83 profile) through the native PMP owner. */
#include "pmp-v2-data.h"

struct pmp_data_field {
	const char *name;
	u16 offset;
	u8 width;
};

static const struct pmp_data_field pmp_data_timer[] = {
	{ "frequency", 0x20, 4 }, { "offset_magnitude", 0x38, 8 }, { "subtract", 0x5c, 1 },
};

static bool pmp_data_live(struct pmp_v2 *pmp)
{
	return pmp->private_profile && pmp->attempted && pmp->pinned &&
		pmp->running && !pmp->protocol.failed;
}

static int pmp_data_read(struct pmp_v2 *pmp, u64 va, unsigned int width, u64 *out)
{
	u64 physical;
	void __iomem *p;

	if (!pmp_data_live(pmp) || !is_power_of_2(width) || width > 8 ||
	    !IS_ALIGNED(va, width) || pmp_v2_data_range(va, width, &physical))
		return -ERANGE;
	p = pmp->resident + physical - pmp->firmware.start;
	switch (width) {
	case 1:
		*out = readb(p);
		break;
	case 2:
		*out = readw(p);
		break;
	case 4:
		*out = readl(p);
		break;
	case 8:
		*out = readq(p);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* Stable observed tuple only; there is no firmware seqlock/coherence claim. */
static int pmp_data_tuple(struct pmp_v2 *pmp, u64 base,
			   const struct pmp_data_field *fields, unsigned int count, u64 *out)
{
	u64 second[16], address;
	unsigned int attempt, pass, i;
	int ret;

	if (count > ARRAY_SIZE(second))
		return -EINVAL;
	for (attempt = 0; attempt < 3; attempt++) {
		for (pass = 0; pass < 2; pass++) {
			for (i = 0; i < count; i++) {
				if (check_add_overflow(base, (u64)fields[i].offset, &address))
					return -ERANGE;
				ret = pmp_data_read(pmp, address, fields[i].width,
						    pass ? &second[i] : &out[i]);
				if (ret)
					return ret;
			}
			/* Order the two observations; does not flush firmware caches. */
			rmb();
		}
		if (!memcmp(out, second, count * sizeof(*out)))
			return 0;
	}
	return -EAGAIN;
}
