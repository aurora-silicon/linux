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
 * Byte addresses in the H14 register image, exclusive ends: common, L2, PE,
 * NE, source tile DMA, destination tile DMA and kernel DMA. H14 objects
 * keep every block below 0x4000.
 */
static inline bool ane_h14_register_valid(u32 word)
{
	static const struct {
		u32 start;
		u32 end;
	} blocks[] = {
		{ 0x0000, 0x004c }, { 0x0500, 0x0564 }, { 0x0900, 0x0914 },
		{ 0x0d00, 0x0d14 }, { 0x1100, 0x11d4 }, { 0x1500, 0x1528 },
		{ 0x1900, 0x1a18 },
	};
	u32 addr = word * 4;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(blocks); i++)
		if (addr >= blocks[i].start && addr < blocks[i].end)
			return true;
	return false;
}

/*
 * The task framing of one HWX target: the fixed header words before the
 * first register record (eight on H14, nine on H16 and H17; one more when
 * the last of them has both low bits set) and the register image the
 * records may name.
 */
struct ane_hx_task_format {
	unsigned int header_words;
	bool (*register_valid)(u32 word);
};

#define ANE_H16_TASK_FORMAT \
	((const struct ane_hx_task_format) { 9, ane_h16_register_valid })

/*
 * Call @fn(@ctx, header) for every register record of every task, after the
 * walk proved each record lies in its task. Stops at the first nonzero
 * return and returns it.
 */
static inline int
ane_hx_task_records(const void *text, size_t size, const struct ane_hx_task_format *f,
		    int (*fn)(void *ctx, u32 header), void *ctx)
{
	const u8 *p = text;
	size_t pos = 16;
	int ret;

	while (pos + 16 <= size) {
		u32 words = (get_unaligned_le32(p + pos) >> 16) & 0x7ff;
		size_t end, rec;

		if (!words) {
			pos += 16;
			continue;
		}
		end = pos + words * 4;
		if (end > size)
			return -ENOEXEC;
		rec = pos + f->header_words * 4;
		if ((get_unaligned_le32(p + pos + (f->header_words - 1) * 4) & 3) == 3)
			rec += 4;
		while (rec < end) {
			u32 h = get_unaligned_le32(p + rec);

			ret = fn(ctx, h);
			if (ret)
				return ret;
			rec += 4 + 4 * (h & BIT(31) ? 1 + hweight32((h >> 15) & 0xffff) :
					((h >> 15) & 0x3f) + 1);
		}
		pos = ALIGN(end, 16);
	}
	return 0;
}

/*
 * A stream opens with a 16-byte frame: little-endian u32 @prologue, then
 * twelve reserved zero bytes. H16 programs open with 1; measured H17 programs
 * open with 0 or 1; H14 programs open with 0. The last task may end at the
 * end of the section; every other task is padded with zeros to 16 bytes.
 * This structural gate does not replace container/tensor/aperture
 * validation.
 */
static inline int
ane_hx_task_stream_check(const void *text, size_t size, u32 expected_tasks,
			 u32 prologue, const struct ane_hx_task_format *f,
			 u32 *record_limit, size_t *final_end)
{
	const u8 *p = text;
	size_t pos = 16, last = 0;
	u32 tasks = 0, limit = 0;

	if (!p || !f || !f->header_words || !expected_tasks || size < 16 ||
	    size > ANE_H16_TASK_MAX || (size & 3) || get_unaligned_le32(p) != prologue ||
	    memchr_inv(p + 4, 0, 12))
		return -ENOEXEC;
	while (pos < size) {
		u32 words, header_words = f->header_words;
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
		if (words < header_words || words > (size - pos) / 4)
			return -ENOEXEC;
		end = pos + words * 4;
		if ((get_unaligned_le32(p + pos + (header_words - 1) * 4) & 3) == 3)
			header_words++;
		if (words < header_words)
			return -ENOEXEC;
		rec = pos + header_words * 4;
		/* A record needs at least one header and one register value. */
		limit += (end - rec) / 8;
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
			if (count > (end - rec) / 4 || !f->register_valid(base))
				return -ENOEXEC;
			if (h & BIT(31)) {
				for (i = 0; i < 16; i++)
					if ((mask & BIT(i)) &&
					    !f->register_valid(base + i + 1))
						return -ENOEXEC;
			} else {
				for (i = 1; i < count; i++)
					if (!f->register_valid(base + i))
						return -ENOEXEC;
			}
			rec += count * 4;
		}
		padded = end == size ? end : ALIGN(end, 16);
		if (padded > size || memchr_inv(p + end, 0, padded - end))
			return -ENOEXEC;
		last = end;
		pos = padded;
		if (++tasks > expected_tasks)
			return -ENOEXEC;
	}
	if (tasks != expected_tasks)
		return -ENOEXEC;
	if (record_limit)
		*record_limit = limit;
	if (final_end)
		*final_end = last;
	return 0;
}

static inline int
ane_hx_task_stream_validate_limit(const void *text, size_t size, u32 expected_tasks,
				  u32 prologue, u32 *record_limit, size_t *final_end)
{
	return ane_hx_task_stream_check(text, size, expected_tasks, prologue,
					&ANE_H16_TASK_FORMAT, record_limit, final_end);
}

static inline int
ane_hx_task_stream_validate(const void *text, size_t size, u32 expected_tasks, u32 prologue)
{
	return ane_hx_task_stream_validate_limit(text, size, expected_tasks, prologue, NULL,
						 NULL);
}

static inline int
ane_h16_task_stream_validate(const void *text, size_t size, u32 expected_tasks)
{
	return ane_hx_task_stream_validate(text, size, expected_tasks, 1);
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
