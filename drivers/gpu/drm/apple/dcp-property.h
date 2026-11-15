/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_DCP_PROPERTY_H
#define _APPLE_DCP_PROPERTY_H

#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>

/* Bound the allocation for one property dictionary. */
static inline bool dcp_property_size_valid(size_t size)
{
	return size && size <= SZ_16M;
}

static inline bool dcp_property_chunk_valid(size_t size, u32 offset,
					    u32 length, size_t source_size)
{
	return length <= source_size && offset <= size && length <= size - offset;
}

static inline bool dcp_property_key_valid(const char *key, size_t size)
{
	return memchr(key, '\0', size);
}

#endif
