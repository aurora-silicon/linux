// SPDX-License-Identifier: ISC
/*
 * Copyright (c) 2014 Broadcom Corporation
 */
#ifndef BRCMFMAC_PCIE_H
#define BRCMFMAC_PCIE_H

#include <linux/types.h>

struct brcmf_pciedev {
	struct brcmf_bus *bus;
	struct brcmf_pciedev_info *devinfo;
};

/* Whether @len bytes at @offset lie within a TCM mapping of @tcm_size bytes. */
static inline bool brcmf_pcie_tcm_range_valid(size_t tcm_size, u32 offset,
					      u32 len)
{
	return len <= tcm_size && offset <= tcm_size - len;
}

#endif /* BRCMFMAC_PCIE_H */
