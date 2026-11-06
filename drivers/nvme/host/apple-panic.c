// SPDX-License-Identifier: GPL-2.0-only
/*
 * Parser for Apple FUNK panic records
 *
 * The v6 record layout and checksum are public in Apple XNU:
 *
 * https://github.com/apple-oss-distributions/xnu/blob/
 * f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/osfmk/kern/debug.h#L404-L452
 * https://github.com/apple-oss-distributions/xnu/blob/
 * f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/pexpert/arm/pe_init.c#L640-L664
 */

#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/unaligned.h>

#include "apple-panic.h"

struct apple_panic_funk_v6 {
	__le32 magic;
	__le32 crc;
	__le32 version;
	__le64 panic_flags;
	__le32 panic_offset;
	__le32 panic_length;
	__le32 stackshot_offset;
	__le32 stackshot_length;
	__le32 other_log_offset;
	__le32 other_log_length;
	u8 legacy_x86_state[8];
	u8 os_version[32];
	u8 macos_version[32];
	u8 boot_session_uuid[37];
	__le64 installed_roots;
	__le32 extensible_log_offset;
	__le32 extensible_log_length;
	__le32 panic_initiator_offset;
	__le32 panic_initiator_length;
} __packed;

static_assert(sizeof(struct apple_panic_funk_v6) ==
	      APPLE_PANIC_FUNK_V6_HDR_SIZE);

struct apple_panic_range {
	size_t start;
	size_t end;
};

static int apple_panic_validate_segment(const u8 *data, size_t data_size,
					u32 offset_field, u32 length_field,
					struct apple_panic_range *ranges,
					unsigned int nr_ranges,
					struct apple_panic_segment *segment)
{
	size_t offset = offset_field;
	size_t length = length_field;
	size_t end;
	unsigned int i;

	if (!length)
		return 0;
	if (offset < APPLE_PANIC_FUNK_V6_HDR_SIZE ||
	    check_add_overflow(offset, length, &end) || end > data_size)
		return -EINVAL;

	for (i = 0; i < nr_ranges; i++) {
		if (offset < ranges[i].end && ranges[i].start < end)
			return -EINVAL;
	}

	ranges[nr_ranges].start = offset;
	ranges[nr_ranges].end = end;
	segment->data = data + offset;
	segment->size = length;
	return 1;
}

/**
 * apple_panic_parse_funk_v6() - validate a complete direct FUNK v6 record
 * @data: bytes read from the panic-log namespace
 * @size: exact number of bytes in @data
 * @record: validated segment views into @data
 *
 * The checksum covers every byte from the version field through @size,
 * including unused padding. Empty segments are ignored. Every populated
 * segment must be wholly outside the header and may not overlap another.
 *
 * Return: 0 on success, -EBADMSG for a bad magic or checksum,
 * -EPROTONOSUPPORT for a non-v6 record, or -EINVAL for malformed bounds.
 */
int apple_panic_parse_funk_v6(const void *data, size_t size,
			      struct apple_panic_record *record)
{
	const struct apple_panic_funk_v6 *header = data;
	struct apple_panic_record parsed = {};
	struct apple_panic_range ranges[5];
	struct apple_panic_segment *segments[5];
	u32 offsets[5];
	u32 lengths[5];
	unsigned int i, nr_ranges = 0;
	u32 expected_crc;
	int ret;

	if (!data || !record || size < sizeof(*header))
		return -EINVAL;
	if (get_unaligned_le32(&header->magic) != APPLE_PANIC_FUNK_MAGIC)
		return -EBADMSG;
	if (get_unaligned_le32(&header->version) != APPLE_PANIC_FUNK_VERSION)
		return -EPROTONOSUPPORT;

	expected_crc = get_unaligned_le32(&header->crc);
	if (crc32_le(0, (const u8 *)data + offsetof(typeof(*header), version),
		     size - offsetof(typeof(*header), version)) != expected_crc)
		return -EBADMSG;

	offsets[0] = get_unaligned_le32(&header->panic_offset);
	lengths[0] = get_unaligned_le32(&header->panic_length);
	offsets[1] = get_unaligned_le32(&header->stackshot_offset);
	lengths[1] = get_unaligned_le32(&header->stackshot_length);
	offsets[2] = get_unaligned_le32(&header->other_log_offset);
	lengths[2] = get_unaligned_le32(&header->other_log_length);
	offsets[3] = get_unaligned_le32(&header->extensible_log_offset);
	lengths[3] = get_unaligned_le32(&header->extensible_log_length);
	offsets[4] = get_unaligned_le32(&header->panic_initiator_offset);
	lengths[4] = get_unaligned_le32(&header->panic_initiator_length);
	segments[0] = &parsed.panic_text;
	segments[1] = &parsed.stackshot;
	segments[2] = &parsed.other_log;
	segments[3] = &parsed.extensible_log;
	segments[4] = &parsed.panic_initiator;

	for (i = 0; i < ARRAY_SIZE(segments); i++) {
		ret = apple_panic_validate_segment(data, size, offsets[i], lengths[i],
						   ranges, nr_ranges, segments[i]);
		if (ret < 0)
			return ret;
		if (ret > 0)
			nr_ranges++;
	}

	*record = parsed;
	return 0;
}
