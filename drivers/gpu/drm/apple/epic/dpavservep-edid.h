/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_DPAVSERV_EDID_H
#define _APPLE_DPAVSERV_EDID_H

#include <asm/byteorder.h>
#include <linux/errno.h>
#include <linux/sizes.h>
#include <linux/types.h>

#define DPAVSERV_EDID_LEADING_SIZE	8
#define DPAVSERV_EDID_BLOCK_SIZE		128
#define DPAVSERV_EDID_EXT_COUNT_OFFSET	0x7e
#define DPAVSERV_EDID_MAX_SIZE		SZ_32K
#define DPAVSERV_EDID_BUF_SIZE		(DPAVSERV_EDID_LEADING_SIZE + \
					 DPAVSERV_EDID_MAX_SIZE)

struct dpavserv_copy_edid_resp {
	__le64 max_size;
	u8 _pad1[24];
	__le64 used_size;
	u8 _pad2[8];
	u8 data[];
} __packed;

/* Only complete EDID blocks within the received reply may be copied. */
static inline int
dcpavserv_edid_size(const struct dpavserv_copy_edid_resp *resp,
		    size_t reply_size, size_t *edid_size)
{
	u64 data_size;
	u8 extensions;

	if (reply_size < sizeof(*resp))
		return -EIO;
	if (le64_to_cpu(resp->max_size) != DPAVSERV_EDID_BUF_SIZE)
		return -EIO;

	data_size = le64_to_cpu(resp->used_size);
	if (data_size < DPAVSERV_EDID_LEADING_SIZE + DPAVSERV_EDID_BLOCK_SIZE ||
	    data_size > DPAVSERV_EDID_BUF_SIZE ||
	    data_size > reply_size - sizeof(*resp))
		return -EIO;

	data_size -= DPAVSERV_EDID_LEADING_SIZE;
	extensions = resp->data[DPAVSERV_EDID_LEADING_SIZE +
				DPAVSERV_EDID_EXT_COUNT_OFFSET];
	/* HF-EEODB can announce more blocks than the base block does. */
	if ((1U + extensions) * DPAVSERV_EDID_BLOCK_SIZE > data_size ||
	    data_size % DPAVSERV_EDID_BLOCK_SIZE)
		return -EIO;

	*edid_size = data_size;
	return 0;
}

#endif
