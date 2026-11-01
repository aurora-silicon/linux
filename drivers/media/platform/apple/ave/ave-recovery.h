/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_AVE_RECOVERY_H_
#define _APPLE_AVE_RECOVERY_H_

#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/types.h>

#define AVE_RECOVERY_ATTEMPTS	3
#define AVE_RECOVERY_WINDOW	(10 * 60 * HZ)

struct ave_recovery_history {
	unsigned long attempts[AVE_RECOVERY_ATTEMPTS];
	unsigned int next;
	unsigned int count;
};

/* A sliding window, including across jiffies wrap. Caller serialises access. */
static inline bool ave_recovery_allow(struct ave_recovery_history *h, unsigned long now)
{
	if (h->count == AVE_RECOVERY_ATTEMPTS &&
	    time_before(now, h->attempts[h->next] + AVE_RECOVERY_WINDOW))
		return false;
	h->attempts[h->next] = now;
	h->next = (h->next + 1) % AVE_RECOVERY_ATTEMPTS;
	if (h->count < AVE_RECOVERY_ATTEMPTS)
		h->count++;
	return true;
}

/*
 * The encoder may only run on a strict translating DMA domain: an unmap
 * invalidates the DART before the pages go back. A flush queue (DMA-FQ),
 * a bypass or no domain at all would let a late firmware write reach memory
 * the kernel has reused.
 */
static inline bool ave_domain_type_safe(unsigned int type)
{
	return type == IOMMU_DOMAIN_DMA;
}

/* A coprocessor-set BOOT_ARG: nonzero while it has power, zero after reset. */
static inline bool ave_witness_armed(u32 boot_arg)
{
	return boot_arg != 0;
}

/* T8110 DART stream control bits, as the audit reads them */
#define AVE_DART_TCR_TRANSLATE	0x001U
#define AVE_DART_TCR_BYPASS	0x002U
#define AVE_DART_TCR_BYPASS_DAPF	0x004U
#define AVE_DART_TCR_REMAP_EN	0x080U
#define AVE_DART_TCR_REMAP	0xf00U

enum ave_dart_stream {
	AVE_DART_STREAM_OFF,	/* no translation, no bypass: faults */
	AVE_DART_STREAM_OURS,	/* Linux's domain (or remapped onto it) */
	AVE_DART_STREAM_UNSAFE,	/* bypass, or tables that are not Linux's */
};

/*
 * @own is the stream Linux attached, @own_ttbr its table base. Any bypass
 * is unsafe. Translation is ours only on @own, or on another stream using
 * the same table base. A remap is ours only onto @own.
 */
static inline enum ave_dart_stream ave_dart_classify(u32 tcr, u32 ttbr, bool is_own,
						     u32 own, u32 own_ttbr)
{
	if (tcr & (AVE_DART_TCR_BYPASS | AVE_DART_TCR_BYPASS_DAPF))
		return AVE_DART_STREAM_UNSAFE;
	if (tcr & AVE_DART_TCR_REMAP_EN)
		return ((tcr & AVE_DART_TCR_REMAP) >> 8) == own ?
			AVE_DART_STREAM_OURS : AVE_DART_STREAM_UNSAFE;
	if (!(tcr & AVE_DART_TCR_TRANSLATE))
		return AVE_DART_STREAM_OFF;
	if (is_own || ttbr == own_ttbr)
		return AVE_DART_STREAM_OURS;
	return AVE_DART_STREAM_UNSAFE;
}

#endif
