/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by apple-dart.c before its writer gate, which reads this state. */
#ifndef __APPLE_DART_CIO_OWNER_H
#define __APPLE_DART_CIO_OWNER_H

#include "apple-dart-power.h"
#include "apple-dart-cio-power.h"

struct apple_dart_cio {
	struct apple_dart *dart;
	struct device *owner;
	struct device_link *link;
	struct regmap *gapf;
	struct dart_cio_power saved;
	unsigned int slice;
	u32 gapf_saved;
	/*
	 * Under managed_lock: writers updating page tables in memory only while
	 * the DART is closed, and whether any did.
	 */
	unsigned int closed_writers;
	bool tlb_stale;
	bool changed, restored, dead;
};

#endif
