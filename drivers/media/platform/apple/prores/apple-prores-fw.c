// SPDX-License-Identifier: GPL-2.0-only
/* Apple ProRes APRI initialization firmware parser. */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <kunit/visibility.h>

#include "apple-prores.h"

#define APRI_HEADER_SIZE	16
#define APRI_BLOCK_HEADER_SIZE	8
#define APRI_ENTRY_SIZE		12
#define APRI_VERSION		1
#define APRI_SOC_T6041		0x6041

static_assert(APRI_T6041_FIRMWARE_SIZE ==
	      APRI_HEADER_SIZE + APRI_BLOCK_COUNT * APRI_BLOCK_HEADER_SIZE +
	      (APRI_SYSTEM_COUNT + APRI_CORE_COUNT + APRI_WRAPPER_COUNT) *
	      APRI_ENTRY_SIZE);

static const u32 apri_block_counts[APRI_BLOCK_COUNT] = {
	[APRI_SYSTEM_BLOCK] = APRI_SYSTEM_COUNT,
	[APRI_CORE_BLOCK] = APRI_CORE_COUNT,
	[APRI_WRAPPER_BLOCK] = APRI_WRAPPER_COUNT,
};

static const u32 apri_offset_limits[APRI_BLOCK_COUNT] = {
	[APRI_SYSTEM_BLOCK] = 0x4000,
	[APRI_CORE_BLOCK] = 0x4000,
	[APRI_WRAPPER_BLOCK] = 0x1000000,
};

static bool apri_offset_seen(const struct apr_init_block *block, u32 offset)
{
	u32 i;

	for (i = 0; i < block->count; i++)
		if (block->entries[i].offset == offset)
			return true;

	return false;
}

int apr_parse_t6041_init(const u8 *data, size_t size,
			 struct apr_init_data *init)
{
	size_t pos = APRI_HEADER_SIZE;
	u32 block_index;

	if (!data || !init || size != APRI_T6041_FIRMWARE_SIZE)
		return -EINVAL;
	if (memcmp(data, "APRI", 4) ||
	    get_unaligned_le32(data + 4) != APRI_VERSION ||
	    get_unaligned_le32(data + 8) != APRI_SOC_T6041 ||
	    get_unaligned_le32(data + 12) != APRI_BLOCK_COUNT)
		return -EINVAL;

	memset(init, 0, sizeof(*init));
	for (block_index = 0; block_index < APRI_BLOCK_COUNT; block_index++) {
		struct apr_init_block *block = &init->blocks[block_index];
		size_t entries_size, end;
		u32 entry_index;

		if (check_add_overflow(pos, (size_t)APRI_BLOCK_HEADER_SIZE, &end) ||
		    end > size)
			return -EINVAL;
		if (get_unaligned_le32(data + pos) != block_index + 1 ||
		    get_unaligned_le32(data + pos + 4) != apri_block_counts[block_index])
			return -EINVAL;
		pos = end;

		if (check_mul_overflow((size_t)apri_block_counts[block_index],
				       (size_t)APRI_ENTRY_SIZE, &entries_size) ||
		    check_add_overflow(pos, entries_size, &end) || end > size)
			return -EINVAL;

		for (entry_index = 0;
		     entry_index < apri_block_counts[block_index]; entry_index++) {
			struct apr_tunable *entry = &block->entries[entry_index];

			entry->offset = get_unaligned_le32(data + pos);
			entry->mask = get_unaligned_le32(data + pos + 4);
			entry->value = get_unaligned_le32(data + pos + 8);
			if (!IS_ALIGNED(entry->offset, sizeof(u32)) ||
			    entry->offset >= apri_offset_limits[block_index] ||
			    !entry->mask || entry->value & ~entry->mask ||
			    apri_offset_seen(block, entry->offset))
				return -EINVAL;
			block->count++;
			pos += APRI_ENTRY_SIZE;
		}
	}

	return pos == size ? 0 : -EINVAL;
}
EXPORT_SYMBOL_IF_KUNIT(apr_parse_t6041_init);
