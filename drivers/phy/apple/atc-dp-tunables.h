/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Copyright 2026 Aurora Silicon */
#ifndef APPLE_ATC_DP_TUNABLES_H
#define APPLE_ATC_DP_TUNABLES_H

#include <linux/types.h>

static inline bool atc_dp_list_length_valid(int length)
{
	return length >= 0 && length <= 65536 && length % 12 == 0;
}

#endif
