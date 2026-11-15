/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _DRM_APPLE_AFK_REPLY_H
#define _DRM_APPLE_AFK_REPLY_H

#include <linux/errno.h>
#include <linux/types.h>

/* Allocation capacity must never be replaced with firmware's returned length. */
static inline int afk_reply_size_valid(bool valid, size_t received,
				     size_t capacity, size_t minimum)
{
	if (!valid || received < minimum)
		return -EPROTO;
	if (received > capacity)
		return -EMSGSIZE;
	return 0;
}

static inline int afk_service_body_size_valid(size_t received, size_t header,
					    size_t claimed, size_t capacity)
{
	if (received < header || claimed > received - header)
		return -EPROTO;
	if (claimed > capacity)
		return -EMSGSIZE;
	return 0;
}
#endif
