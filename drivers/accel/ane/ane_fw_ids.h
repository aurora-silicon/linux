/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_IDS_H__
#define __ANE_FW_IDS_H__

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/types.h>

/* Protected by the firmware lock; never reset when firmware restarts. */
struct ane_fw_ids {
	u64 next;
};

static inline int ane_fw_id_alloc(struct ane_fw_ids *ids, u32 *id)
{
	if (!ids->next)
		ids->next = 1;
	/* Keep zero and the protocol's U32_MAX sentinel out of the namespace. */
	if (ids->next >= U32_MAX)
		return -ENOSPC;
	*id = ids->next++;
	return 0;
}

#endif /* __ANE_FW_IDS_H__ */
