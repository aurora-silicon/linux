/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_HWX_H__
#define __ANE_H16_HWX_H__

#include <linux/sizes.h>
#include <linux/types.h>
#include <crypto/sha2.h>

#define ANE_H16_SURFACES 4
#define ANE_H16_SECTIONS 8
#define ANE_H16_HWX_MAX (SZ_256M + SZ_1M)

struct ane_h16_hwx_section {
	u64 vm;
	u64 size;
	u32 offset;
	u32 reloff;
	u32 nreloc;
	u32 ordinal;
};

struct ane_h16_hwx_surface {
	struct ane_h16_hwx_section section;
	u64 allocation;
	u64 span;
	u32 direction;
	u32 symbol_offset;
	char name[8];
};

struct ane_h16_hwx {
	struct ane_h16_hwx_section text, constants, kernel, scratch;
	struct ane_h16_hwx_surface surfaces[ANE_H16_SURFACES];
	u32 surface_count;
	u32 tasks;
	u32 program_offset;
};

/* Input must be one immutable kernel-owned snapshot of the original file. */
int ane_h16_hwx_validate(const void *bytes, size_t size, struct ane_h16_hwx *info);

struct ane_h16_hwx_snapshot {
	void *bytes;
	size_t size;
	u8 digest[SHA256_DIGEST_SIZE];
	struct ane_h16_hwx info;
};

/* Snapshot before validation; callers never publish or mutate snapshot bytes. */
int ane_h16_hwx_snapshot_create(const void *bytes, size_t size,
				struct ane_h16_hwx_snapshot *snapshot);
int ane_h16_hwx_snapshot_check(const struct ane_h16_hwx_snapshot *snapshot);
void ane_h16_hwx_snapshot_destroy(struct ane_h16_hwx_snapshot *snapshot);

#endif /* __ANE_H16_HWX_H__ */
