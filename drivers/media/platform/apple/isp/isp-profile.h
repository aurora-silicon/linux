/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ISP_PROFILE_H__
#define __ISP_PROFILE_H__

#include <linux/types.h>
#include <linux/bits.h>

#define ISP_T6040_PROFILE "t6040-25G76"
#define ISP_T6040_UUID "4C8FBA5E-45CC-308E-9BE5-053518C19D76"
#define ISP_T6040_PAGE_SIZE 0x4000U
#define ISP_T6040_IOVA_BASE BIT_ULL(40)
#define ISP_T6040_IOVA_LIMIT BIT_ULL(42)
#define ISP_T6040_IPC_SIZE 0x1c000U
#define ISP_T6040_BOOT_SIZE 0x290U

struct isp_profile {
	const char *name;
	const char *uuid;
	u32 chip, board, platform, scheme, stream, alias_tcr;
	u32 aliases[5];
	u32 meta_count, output_count, meta_size, start_argument;
};

struct isp_profile_geometry {
	u32 width, height, stride, luma_size, chroma_size, total_size;
};

struct isp_profile_boot {
	u64 ipc_iova, args_iova, command_iova, extra_iova, extra_size;
	u64 shared_base, shared_size;
	u32 args_offset;
};

struct isp_t6040_dsid_command {
	u64 opcode;
	u32 count, range;
	u64 bases[8];
} __packed;

extern const struct isp_profile isp_profile_t6040_25g76;
const struct isp_profile *isp_profile_select(u32 chip, u32 board,
					     const char *name, const char *uuid);
int isp_profile_p010_geometry(u32 width, u32 height,
			      struct isp_profile_geometry *geometry);
int isp_profile_t6040_boot(u64 ipc_iova, u32 args_offset, u64 extra_iova,
			   u64 extra_size, struct isp_profile_boot *boot);
int isp_profile_t6040_bootargs(void *target, const struct isp_profile_boot *boot);
int isp_profile_t6040_dsid(u32 active_mcc_mask,
			   struct isp_t6040_dsid_command *command);

#endif /* __ISP_PROFILE_H__ */
