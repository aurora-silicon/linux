/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_TASKS_H__
#define __ANE_H16_TASKS_H__

#include <linux/align.h>
#include <linux/array_size.h>
#include <linux/bitmap.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define ANE_H16_TASK_MAX 0x60000

/* Byte addresses in the source-backed H16 register image, exclusive ends. */
static inline bool ane_h16_register_valid(u32 word)
{
	static const struct {
		u32 start;
		u32 end;
	} blocks[] = {
		{ 0x0000, 0x005c }, { 0x4100, 0x41a4 }, { 0x44d0, 0x44d4 },
		{ 0x4500, 0x453c }, { 0x4900, 0x4930 }, { 0x4d00, 0x4e44 },
		{ 0x5100, 0x5154 }, { 0x5500, 0x5620 }, { 0x5900, 0x5930 },
	};
	u32 addr = word * 4;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(blocks); i++)
		if (addr >= blocks[i].start && addr < blocks[i].end)
			return true;
	return false;
}

/*
 * H16 streams begin with little-endian u32 1 and twelve reserved zero bytes.
 * This structural gate does not replace container/tensor/aperture validation.
 */
static inline int
ane_h16_task_stream_validate(const void *text, size_t size, u32 expected_tasks)
{
	const u8 *p = text;
	size_t pos = 16;
	u32 tasks = 0;

	if (!p || !expected_tasks || size < 16 || size > ANE_H16_TASK_MAX ||
	    (size & 3) || get_unaligned_le32(p) != 1 ||
	    memchr_inv(p + 4, 0, 12))
		return -ENOEXEC;
	while (pos < size) {
		u32 words, header_words = 9;
		size_t end, padded, rec;

		if (size - pos < 16)
			return -ENOEXEC;
		words = (get_unaligned_le32(p + pos) >> 16) & 0x7ff;
		if (!words) {
			if (memchr_inv(p + pos, 0, 16))
				return -ENOEXEC;
			pos += 16;
			continue;
		}
		if (words < 9 || words > (size - pos) / 4)
			return -ENOEXEC;
		end = pos + words * 4;
		if ((get_unaligned_le32(p + pos + 8 * 4) & 3) == 3)
			header_words++;
		if (words < header_words)
			return -ENOEXEC;
		rec = pos + header_words * 4;
		while (rec < end) {
			u32 h = get_unaligned_le32(p + rec);
			u32 base = h & 0x7fff;
			u32 mask = (h >> 15) & 0xffff;
			u32 count, i;

			rec += 4;
			if (h & BIT(31))
				count = 1 + hweight32(mask);
			else
				count = ((h >> 15) & 0x3f) + 1;
			if (count > (end - rec) / 4 || !ane_h16_register_valid(base))
				return -ENOEXEC;
			if (h & BIT(31)) {
				for (i = 0; i < 16; i++)
					if ((mask & BIT(i)) &&
					    !ane_h16_register_valid(base + i + 1))
						return -ENOEXEC;
			} else {
				for (i = 1; i < count; i++)
					if (!ane_h16_register_valid(base + i))
						return -ENOEXEC;
			}
			rec += count * 4;
		}
		/* Tasks start 16-byte aligned; the last one may end the stream. */
		padded = min_t(size_t, ALIGN(end, 16), size);
		if (memchr_inv(p + end, 0, padded - end))
			return -ENOEXEC;
		pos = padded;
		if (++tasks > expected_tasks)
			return -ENOEXEC;
	}
	return tasks == expected_tasks ? 0 : -ENOEXEC;
}

/* Section ordinal is supplied only by the validated complete section table.
 * This checks Mach-O relocation entries, not the distinct aperture records.
 */
static inline int
ane_h16_relocations_validate(const void *text, size_t text_size,
			     const void *relocs, size_t count, u32 ordinal,
			     u64 kernel_vm, u64 kernel_size,
			     unsigned long *seen, size_t seen_bits)
{
	const u8 *r = relocs;
	u64 kernel_end;
	size_t i;

	if (!count)
		return 0;
	if (!text || !relocs || !seen || !ordinal || ordinal > 0xffffff ||
	    !kernel_size || text_size > ANE_H16_TASK_MAX || (text_size & 3) ||
	    count > text_size / 4 || seen_bits < text_size / 4 ||
	    check_add_overflow(kernel_vm, kernel_size, &kernel_end))
		return -ENOEXEC;
	bitmap_zero(seen, text_size / 4);
	for (i = 0; i < count; i++) {
		u32 patch = get_unaligned_le32(r + i * 8);
		u32 info = get_unaligned_le32(r + i * 8 + 4);
		u32 addend;
		u64 target;

		if ((patch & 3) || text_size < 4 || patch > text_size - 4 ||
		    (info >> 24) != 7 || (info & 0xffffff) != ordinal ||
		    test_and_set_bit(patch / 4, seen))
			return -ENOEXEC;
		addend = get_unaligned_le32((const u8 *)text + patch);
		if (addend >= kernel_size ||
		    check_add_overflow(kernel_vm, (u64)addend, &target) ||
		    target >= kernel_end)
			return -ENOEXEC;
	}
	return 0;
}

#endif /* __ANE_H16_TASKS_H__ */
