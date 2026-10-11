// SPDX-License-Identifier: GPL-2.0-only
#include <linux/align.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "isp-profile.h"

/* Own 25G76 image SHA256:
 * 4990fa8e00c31be970166238d7953246ee5b9db3ab0085aca578c4198dbad433.
 * The ADT UUID is the runtime selector; this hash is source provenance.
 */
const struct isp_profile isp_profile_t6040_25g76 = {
	.name = ISP_T6040_PROFILE,
	.uuid = ISP_T6040_UUID,
	.chip = 0x6040, .board = 6, .platform = 5, .scheme = 16,
	.stream = 0, .alias_tcr = 0x80, .aliases = { 1, 6, 7, 10, 11 },
	.meta_count = 8, .output_count = 2, .meta_size = 0x8000,
	.start_argument = 0x11,
};

const struct isp_profile *isp_profile_select(u32 chip, u32 board,
					     const char *name, const char *uuid)
{
	const struct isp_profile *profile = &isp_profile_t6040_25g76;

	if (!name || !uuid || chip != profile->chip || board != profile->board ||
	    strcmp(name, profile->name) || strcmp(uuid, profile->uuid))
		return NULL;
	return profile;
}

int isp_profile_p010_geometry(u32 width, u32 height,
			      struct isp_profile_geometry *geometry)
{
	struct isp_profile_geometry result;

	if (!geometry || !((width == 1280 && height == 720) ||
			   (width == 1920 && height == 1080)))
		return -EINVAL;
	result.width = width;
	result.height = height;
	result.stride = ALIGN(width * 2, 64);
	result.luma_size = ALIGN(result.stride * height, ISP_T6040_PAGE_SIZE);
	result.chroma_size = ALIGN(result.stride * height / 2, ISP_T6040_PAGE_SIZE);
	result.total_size = result.luma_size + result.chroma_size;
	*geometry = result;
	return 0;
}

int isp_profile_t6040_boot(u64 ipc_iova, u32 args_offset, u64 extra_iova,
			   u64 extra_size, struct isp_profile_boot *boot)
{
	struct isp_profile_boot result = {};
	u64 args_end;

	/* Fixed segment sizes, fresh physical backing: never historical PAs. */
	if (!boot || ipc_iova != ISP_T6040_IOVA_BASE + 0x1ff0000 ||
	    args_offset % 0x40 || !extra_size || extra_size > 0x7000000 ||
	    extra_size % ISP_T6040_PAGE_SIZE ||
	    extra_iova != ipc_iova + ISP_T6040_IPC_SIZE + ISP_T6040_PAGE_SIZE)
		return -EINVAL;
	result.ipc_iova = ipc_iova;
	result.args_offset = args_offset;
	result.args_iova = ipc_iova + (u64)args_offset + 0x40;
	result.command_iova = result.args_iova + ISP_T6040_BOOT_SIZE + 0x40;
	args_end = result.command_iova + 0x400;
	if (args_end > ipc_iova + ISP_T6040_IPC_SIZE ||
	    extra_iova + extra_size > ISP_T6040_IOVA_LIMIT)
		return -EINVAL;
	result.extra_iova = extra_iova;
	result.extra_size = extra_size;
	result.shared_base = 0x1fec000;
	result.shared_size = 0x10000000 - result.shared_base;
	*boot = result;
	return 0;
}

int isp_profile_t6040_bootargs(void *target, const struct isp_profile_boot *boot)
{
	struct isp_profile_boot checked = {};
	u8 *bytes = target;

	if (!target || !boot)
		return -EINVAL;
	if (isp_profile_t6040_boot(boot->ipc_iova, boot->args_offset,
				   boot->extra_iova, boot->extra_size, &checked) ||
	    boot->args_iova != checked.args_iova ||
	    boot->command_iova != checked.command_iova ||
	    boot->shared_base != checked.shared_base ||
	    boot->shared_size != checked.shared_size)
		return -EINVAL;
	memset(bytes, 0, ISP_T6040_BOOT_SIZE);
	put_unaligned_le64(boot->ipc_iova, bytes + 8);
	put_unaligned_le64(boot->shared_base, bytes + 0x10);
	put_unaligned_le64(boot->shared_size, bytes + 0x18);
	put_unaligned_le64(boot->extra_iova, bytes + 0x20);
	put_unaligned_le64(boot->extra_size, bytes + 0x28);
	put_unaligned_le32(5, bytes + 0x30);
	put_unaligned_le64((u64)boot->args_offset + 1, bytes + 0x50);
	put_unaligned_le32(0x40, bytes + 0x68);
	put_unaligned_le64(16, bytes + 0xd0);
	put_unaligned_le64(1, bytes + 0x120);
	return 0;
}

int isp_profile_t6040_dsid(u32 active_mcc_mask,
			   struct isp_t6040_dsid_command *command)
{
	static_assert(sizeof(*command) == 0x50);
	if (!command || active_mcc_mask != 15)
		return -EINVAL;
	memset(command, 0, sizeof(*command));
	command->opcode = 0x3206ULL << 32;
	command->count = 8;
	command->range = 0x2fc;
	for (unsigned int i = 0; i < 8; i++)
		command->bases[i] = i < 4 ? 0x2201d4000ULL + i * 0x2000000ULL : U64_MAX;
	return 0;
}
