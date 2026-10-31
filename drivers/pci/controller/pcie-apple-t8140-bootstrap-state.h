/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PCIE_APPLE_T8140_BOOTSTRAP_STATE_H
#define PCIE_APPLE_T8140_BOOTSTRAP_STATE_H

#include <linux/align.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/types.h>

#define PIODMA_PACKET_MASK	GENMASK_ULL(35, 0)
#define PIODMA_POINTER_MASK	GENMASK_ULL(41, 0)
#define PIODMA_POINTER_PREFIX	BIT_ULL(40)
#define PIODMA_IRQ_COMPLETE	(BIT(0) | BIT(6))
#define PIODMA_IRQ_LIMIT		16

/* Only RELEASE before publication can end guard ownership. No reset event. */
enum apple_piodma_state {
	PIODMA_IDLE,
	PIODMA_GUARDED,
	PIODMA_ATTEMPTED,
	PIODMA_FAILED,
	PIODMA_REJECTED,
	PIODMA_RETAINED,
	PIODMA_READY,
	PIODMA_ADMITTED,
	PIODMA_FAILED_RETAINED,
	PIODMA_STATE_COUNT,
};

enum apple_piodma_event {
	PIODMA_GUARD,
	PIODMA_RELEASE,
	PIODMA_PRIME,
	PIODMA_PUBLISH,
	PIODMA_COMPLETE,
	PIODMA_ADMIT,
	PIODMA_FAIL,
	PIODMA_EVENT_COUNT,
};

static inline bool apple_piodma_retained(enum apple_piodma_state state)
{
	return state >= PIODMA_RETAINED && state < PIODMA_STATE_COUNT;
}

static inline bool apple_piodma_guarded(enum apple_piodma_state state)
{
	return state == PIODMA_GUARDED || state == PIODMA_ATTEMPTED ||
		state == PIODMA_FAILED || apple_piodma_retained(state);
}

static inline bool apple_piodma_attempted(enum apple_piodma_state state)
{
	return state == PIODMA_ATTEMPTED || state == PIODMA_FAILED ||
		state == PIODMA_REJECTED || apple_piodma_retained(state);
}

static inline bool apple_piodma_ready(enum apple_piodma_state state)
{
	return state == PIODMA_READY || state == PIODMA_ADMITTED;
}

static inline bool apple_piodma_sleep_allowed(enum apple_piodma_state state,
					      bool suspend_to_idle)
{
	return !apple_piodma_guarded(state) ||
		(state == PIODMA_ADMITTED && suspend_to_idle);
}

static inline int apple_piodma_next(enum apple_piodma_state state,
				    enum apple_piodma_event event)
{
	switch (event) {
	case PIODMA_GUARD:
		if (state == PIODMA_IDLE)
			return PIODMA_GUARDED;
		break;
	case PIODMA_RELEASE:
		if (state == PIODMA_GUARDED)
			return PIODMA_IDLE;
		if (state == PIODMA_FAILED)
			return PIODMA_REJECTED;
		break;
	case PIODMA_PRIME:
		if (state == PIODMA_GUARDED)
			return PIODMA_ATTEMPTED;
		break;
	case PIODMA_PUBLISH:
		if (state == PIODMA_ATTEMPTED)
			return PIODMA_RETAINED;
		break;
	case PIODMA_COMPLETE:
		if (state == PIODMA_RETAINED)
			return PIODMA_READY;
		break;
	case PIODMA_ADMIT:
		if (state == PIODMA_READY)
			return PIODMA_ADMITTED;
		break;
	case PIODMA_FAIL:
		if (state == PIODMA_ATTEMPTED)
			return PIODMA_FAILED;
		if (state == PIODMA_RETAINED)
			return PIODMA_FAILED_RETAINED;
		break;
	default:
		break;
	}
	return -EINVAL;
}

static inline bool apple_piodma_arena_valid(u64 dma, u64 size, u64 prefix,
					    bool force_aperture, u64 start, u64 end)
{
	u64 last, pointer;

	return size && IS_ALIGNED(dma, SZ_16K) &&
		prefix == PIODMA_POINTER_PREFIX &&
		!check_add_overflow(dma, size - 1, &last) &&
		last <= PIODMA_PACKET_MASK &&
		!check_add_overflow(last, prefix, &pointer) &&
		pointer <= PIODMA_POINTER_MASK && force_aperture &&
		!start && end == PIODMA_PACKET_MASK;
}

static inline bool apple_piodma_irq_invalid(unsigned int count, u32 status,
					    u32 tag, u32 active)
{
	return count >= PIODMA_IRQ_LIMIT || status & ~PIODMA_IRQ_COMPLETE ||
		(status && (!tag || tag > active));
}

static inline bool apple_piodma_irq_matches(u32 status, u32 tag, u32 active)
{
	return status & PIODMA_IRQ_COMPLETE && tag == active && active;
}

#endif
