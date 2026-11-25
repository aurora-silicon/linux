/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_TASKS_H__
#define __ANE_H16_TASKS_H__

#include <linux/align.h>
#include <linux/array_size.h>
#include <linux/bitmap.h>
#include <linux/errno.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/slab.h>
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
 * A stream opens with a 16-byte frame: little-endian u32 @prologue, then
 * twelve reserved zero bytes. H15/H16/H17 programs share this framing with
 * @header_words fixed words per task (8 on H15, 9 on H16 and H17). The last
 * task may end at the end of the section; every other task is padded with
 * zeros to 16 bytes. This structural gate does not replace container/tensor/
 * aperture validation.
 */
static inline int
ane_hx_task_stream_validate_hdr(const void *text, size_t size, u32 expected_tasks,
				u32 prologue, unsigned int fixed_header_words,
				u32 *record_limit, size_t *final_end)
{
	const u8 *p = text;
	size_t pos = 16, last = 0;
	u32 tasks = 0, limit = 0;

	if (!p || !expected_tasks || !fixed_header_words || size < 16 ||
	    size > ANE_H16_TASK_MAX || (size & 3) ||
	    get_unaligned_le32(p) != prologue || memchr_inv(p + 4, 0, 12))
		return -ENOEXEC;
	while (pos < size) {
		u32 words, header_words = fixed_header_words;
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
		if (words < fixed_header_words || words > (size - pos) / 4)
			return -ENOEXEC;
		end = pos + words * 4;
		if ((get_unaligned_le32(p + pos + (fixed_header_words - 1) * 4) & 3) == 3)
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
	return ane_hx_task_stream_validate_hdr(text, size, expected_tasks, prologue, 9,
					       record_limit, final_end);
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

/*
 * DMA bases. A task names each buffer the engine reads or writes as a
 * relocated [lo, hi] record: its 64-bit offset is taken relative to the DVA
 * of the buffer bound to the record's BAR slot, and nothing after this point
 * bounds the sum. The bases are the input DMA's sources 1 and 2, the output
 * DMA, the coefficient DMA and the lookup table.
 */
#define ANE_HX_DMA_SLOTS 64
#define ANE_HX_DMA_WORDS (23 + 81 + 21 + 72)

/* Image index of a common or DMA register byte address, or -1. */
static inline int ane_hx_dma_index(u32 addr)
{
	if (addr < 0x5c)
		return addr / 4;
	if (addr >= 0x4d00 && addr < 0x4e44)
		return 23 + (addr - 0x4d00) / 4;
	if (addr >= 0x5100 && addr < 0x5154)
		return 104 + (addr - 0x5100) / 4;
	if (addr >= 0x5500 && addr < 0x5620)
		return 125 + (addr - 0x5500) / 4;
	return -1;
}

static inline bool ane_hx_dma_base(u32 addr)
{
	switch (addr & ~4) {
	case 0x4d10: case 0x4d28: case 0x5108: case 0x5510: case 0x5618:
		return true;
	}
	return false;
}

/*
 * The DMA registers a task may set: the bases, their strides, formats and
 * coefficient lanes, and the words that only configure or hint a transfer.
 * The rest of each DMA block (metadata and compression, texture, crop and
 * wrap, the bias, post-scale and palette tables) moves addresses this bound
 * does not model, and no measured program sets it, so it stays zero.
 */
static inline bool ane_hx_dma_modelled(u32 addr)
{
	return (addr >= 0x4d00 && addr <= 0x4d3c) || addr == 0x4d68 || addr == 0x4d6c ||
	       addr == 0x5100 || (addr >= 0x5108 && addr <= 0x511c) || addr == 0x5138 ||
	       addr == 0x5500 || addr == 0x5504 || (addr >= 0x5510 && addr <= 0x55dc) ||
	       (addr >= 0x5610 && addr <= 0x561c);
}

/* Source 1, source 2 and destination DMA configuration and coefficient lanes. */
static inline bool ane_hx_dma_config(u32 addr)
{
	return addr == 0x4d00 || addr == 0x4d04 || addr == 0x5100 ||
	       (addr >= 0x5520 && addr <= 0x555c);
}

/* Byte address of value @i of the record with header @h. */
static inline u32 ane_hx_record_addr(u32 h, u32 i)
{
	u32 base = h & 0x7fff, mask = (h >> 15) & 0xffff, bit;

	if (!(h & BIT(31)) || !i)
		return (base + i) * 4;
	for (bit = 0; bit < 16; bit++)
		if ((mask & BIT(bit)) && !--i)
			break;
	return (base + bit + 1) * 4;
}

static inline u32 ane_hx_record_count(u32 h)
{
	return h & BIT(31) ? 1 + hweight32((h >> 15) & 0xffff) : ((h >> 15) & 0x3f) + 1;
}

/*
 * Bytes an input or output DMA stream touches from its base: the last
 * element of the last row, plane, depth slice and group. Interleave k packs
 * k channels into each element, so a plane holds k channels (the measured
 * matmul staging writes 128 channels interleaved 8 as 16 planes of 2 KiB, its
 * exact scratch).
 */
static inline int ane_hx_dma_tile(const u32 *img, u32 base, u64 *extent)
{
	static const u8 bytes[] = { 1, 1, 2, 4 };
	bool dst = base == 0x5108;
	u32 stride = base + 8, format = dst ? 0x5138 : base == 0x4d10 ? 0x4d68 : 0x4d6c;
	u32 dim = dst ? 0x14 : 0x4, f = img[ane_hx_dma_index(format)];
	u64 k = max(1U, (f >> 24) & 0xf), n[5], sum, term;
	unsigned int i;

	for (i = 0; i < 4; i++)
		n[i] = max(1U, img[ane_hx_dma_index(dim + 4 * i)]);
	n[4] = max(1U, img[ane_hx_dma_index(0x24)]);
	n[2] = DIV_ROUND_UP(n[2], k);
	sum = n[0] * bytes[(f >> 12) & 3] * k;
	for (i = 1; i < 5; i++)
		if (check_mul_overflow(n[i] - 1, (u64)img[ane_hx_dma_index(stride + 4 * (i - 1))],
				       &term) ||
		    check_add_overflow(sum, term, &sum))
			return -ENOEXEC;
	*extent = sum;
	return 0;
}

/*
 * Bytes the coefficient DMA reads from its base: the furthest of the 16 lanes
 * (offset 0x5560 + 4i, size 0x55a0 + 4i, at least the per-channel aligned
 * size 0x5504; an unset size is 64), then every group (stride 0x5518) and
 * every output channel group (stride 0x551c) after the first. An output
 * channel group is 16 << (common word 0x40 & 7) channels, 128 when that
 * word is unset.
 */
static inline int ane_hx_dma_kernel(const u32 *img, u64 *extent)
{
	u64 lanes = 0, groups, ocgs, term;
	unsigned int i;

	for (i = 0; i < 16; i++)
		lanes = max(lanes, (u64)img[ane_hx_dma_index(0x5560 + 4 * i)] +
			    max(img[ane_hx_dma_index(0x55a0 + 4 * i)],
				img[ane_hx_dma_index(0x5504)]));
	groups = max(1U, img[ane_hx_dma_index(0x24)]) - 1;
	ocgs = DIV_ROUND_UP((u64)max(1U, img[ane_hx_dma_index(0x1c)]),
			    16U << (img[ane_hx_dma_index(0x40)] & 7)) - 1;
	if (check_mul_overflow(groups, (u64)img[ane_hx_dma_index(0x5518)], &term) ||
	    check_add_overflow(lanes, term, &lanes) ||
	    check_mul_overflow(ocgs, (u64)img[ane_hx_dma_index(0x551c)], &term) ||
	    check_add_overflow(lanes, term, extent))
		return -ENOEXEC;
	return 0;
}

/* The five DMA bases, in the order of ane_hx_dma_state's arrays. */
static const u16 ane_hx_dma_base_addr[] = { 0x4d10, 0x4d28, 0x5108, 0x5510, 0x5618 };
#define ANE_HX_DMA_BASES ARRAY_SIZE(ane_hx_dma_base_addr)

/* A register image, and each base's slot and offset as last written into it. */
struct ane_hx_dma_state {
	u32 img[ANE_HX_DMA_WORDS];
	u64 offset[ANE_HX_DMA_BASES];
	u8 slot[ANE_HX_DMA_BASES];
	u8 written;		/* bit b: base b was written */
};

static inline void ane_hx_dma_defaults(struct ane_hx_dma_state *st)
{
	unsigned int i;

	memset(st, 0, sizeof(*st));
	st->img[ane_hx_dma_index(0x40)] = 3;
	for (i = 0; i < 16; i++)
		st->img[ane_hx_dma_index(0x55a0 + 4 * i)] = 64;
}

/*
 * Registers outside the common and DMA blocks that the bound does not model.
 * The L2 (0x4100), planar engine (0x4500) and neural engine (0x4900) blocks
 * configure on-chip buffers and arithmetic, and none of them can be
 * relocated, so none carries a buffer address. 0x44d0 is never written; of
 * the cache DMA block (0x5900), only the words the compiler writes, none of
 * them an address, may be non-zero.
 */
static inline bool ane_hx_other_allowed(u32 addr)
{
	if (addr == 0x44d0)
		return false;
	if (addr >= 0x5900 && addr < 0x5934)
		return addr == 0x5900 || addr == 0x5904 || addr == 0x5908 ||
		       addr == 0x5918 || addr == 0x5928 || addr == 0x592c;
	return true;
}

/*
 * Apply one task's records (bytes [rec, end) of @p) to @st, refusing the
 * header forms the task stream never uses. A relocated record covering a
 * base's [lo, hi] pair sets that base's slot and offset.
 */
static inline int ane_hx_dma_apply(const u8 *p, size_t rec, size_t end,
				   struct ane_hx_dma_state *st)
{
	unsigned int i, b;
	int index;

	while (rec < end) {
		u32 h = get_unaligned_le32(p + rec), count = ane_hx_record_count(h);
		bool reloc = !(h & BIT(31)) && (h & BIT(29));
		u32 slot = (h >> 23) & 0x3f, first = (h & 0x7fff) * 4;

		rec += 4;
		if (count > (end - rec) / 4)
			return -ENOEXEC;
		/* Dense header bits 21, 22 and 30 are unused, the slot only names a relocation. */
		if (!(h & BIT(31)) && ((h & (BIT(21) | BIT(22) | BIT(30))) || (!reloc && slot)))
			return -ENOEXEC;
		if (reloc && count == 2 && ane_hx_dma_base(first) && !(first & 4)) {
			for (b = 0; ane_hx_dma_base_addr[b] != first; b++)
				;
			st->offset[b] = get_unaligned_le32(p + rec) |
					(u64)get_unaligned_le32(p + rec + 4) << 32;
			st->slot[b] = slot;
			st->written |= BIT(b);
		} else if (reloc) {
			if (slot != 4)
				return -ENOEXEC;
			for (i = 0; i < count; i++)
				if (!ane_hx_dma_config(first + 4 * i))
					return -ENOEXEC;
		}
		for (i = 0; i < count; i++) {
			u32 addr = ane_hx_record_addr(h, i);

			if (!reloc && ane_hx_dma_base(addr))
				return -ENOEXEC;
			index = ane_hx_dma_index(addr);
			if (index >= 0)
				st->img[index] = get_unaligned_le32(p + rec + 4 * i);
			else if (get_unaligned_le32(p + rec + 4 * i) && !ane_hx_other_allowed(addr))
				return -ENOEXEC;
		}
		rec += count * 4;
	}
	return 0;
}

/*
 * The DMA words of an image: only modelled registers, and no wrap, offset,
 * crop or compression. Format bits other than the interleave, the element
 * size and the bits every program sets are refused, as is an output channel
 * group (common word 0x40 & 7) above 5, the largest the compiler writes.
 */
static inline int ane_hx_dma_image_check(const u32 *img)
{
	unsigned int i;

	for (i = 23; i < ANE_HX_DMA_WORDS; i++) {
		u32 addr = i < 104 ? 0x4d00 + (i - 23) * 4 : i < 125 ? 0x5100 + (i - 104) * 4 :
			   0x5500 + (i - 125) * 4;

		if (img[i] && !ane_hx_dma_modelled(addr))
			return -ENOEXEC;
	}
	if ((img[ane_hx_dma_index(0x4d08)] | img[ane_hx_dma_index(0x4d0c)]) & ~0xffU ||
	    (img[ane_hx_dma_index(0x4d68)] | img[ane_hx_dma_index(0x4d6c)]) & ~0x0f003031U ||
	    img[ane_hx_dma_index(0x5138)] & ~0x0f303031U ||
	    (img[ane_hx_dma_index(0x40)] & 7) > 5)
		return -ENOEXEC;
	return 0;
}

/*
 * What one task enables it must also locate: an input DMA source, the output
 * DMA or the lookup table with a non-zero configuration needs its relocated
 * base in the same task, and a height and channel count that are not zero.
 * A zero width, depth or group count is read as one: the compiler writes 0
 * for them, and a zero width is in the 256 x 256 matmul that runs correctly
 * on an M4 Pro. The coefficient DMA is not held to this: lookup-table tasks
 * configure it without a base (see ane_hx_task_stream_dma_validate()).
 */
static inline int ane_hx_dma_enabled_check(const struct ane_hx_dma_state *st)
{
	static const struct { u16 config, last, dim; u8 base; } dma[] = {
		{ 0x4d00, 0x4d00, 0x4, 0 }, { 0x4d04, 0x4d04, 0x4, 1 },
		{ 0x5100, 0x5100, 0x14, 2 }, { 0x5600, 0x5614, 0, 4 },
	};
	unsigned int i;
	u32 a, on;

	for (i = 0; i < ARRAY_SIZE(dma); i++) {
		for (a = dma[i].config, on = 0; a <= dma[i].last; a += 4)
			on |= st->img[ane_hx_dma_index(a)];
		if (!on)
			continue;
		if (!(st->written & BIT(dma[i].base)) ||
		    (dma[i].dim && (!st->img[ane_hx_dma_index(dma[i].dim + 4)] ||
				    !st->img[ane_hx_dma_index(dma[i].dim + 8)])))
			return -ENOEXEC;
	}
	return 0;
}

/*
 * Base @b of @st, with the extent its image gives, must lie inside the buffer
 * bound to its slot: the end of the last 64-byte line it touches,
 * ALIGN(offset + extent, 64), at most the buffer's size. The offset is
 * unsigned: a negative one is larger than any buffer.
 */
static inline int ane_hx_dma_fits(const struct ane_hx_dma_state *st, unsigned int b,
				  const u64 bound[ANE_HX_DMA_SLOTS])
{
	u32 base = ane_hx_dma_base_addr[b];
	u64 extent, last;
	int ret = 0;

	if (base == 0x5510)
		ret = ane_hx_dma_kernel(st->img, &extent);
	else if (base == 0x5618)
		extent = max(64U, st->img[ane_hx_dma_index(0x5614)]);
	else
		ret = ane_hx_dma_tile(st->img, base, &extent);
	if (ret || !bound[st->slot[b]] ||
	    check_add_overflow(st->offset[b], extent, &last) ||
	    check_add_overflow(last, 63ULL, &last) ||
	    (last & ~63ULL) > bound[st->slot[b]])
		return -ENOEXEC;
	return 0;
}

/*
 * Bound every DMA base of an already validated stream. @bound[s] is the size
 * of the buffer the operation section binds to BAR slot s, 0 when it binds
 * none. A base must be a relocated [lo, hi] record on a bound slot, and its
 * offset plus the extent of its transfer must stay inside that buffer.
 *
 * Whether the engine starts each task from its defaults or keeps registers
 * from the task before is not known, so both readings are checked:
 *  - each task over the defaults: the bases it writes, with its own words,
 *    and every DMA it enables has its base in the task;
 *  - a running image over the whole stream, from the defaults: after each
 *    task, every base written so far, with the geometry in force then. This
 *    also bounds a coefficient DMA configured without a base of its own.
 * Programs leave out words that hold the default even when an earlier task
 * set another value; such a stream fits only the first reading and is
 * refused.
 */
static inline int
ane_hx_task_stream_dma_validate(const void *text, size_t size,
				const u64 bound[ANE_HX_DMA_SLOTS])
{
	struct ane_hx_dma_state *task, *run;
	const u8 *p = text;
	size_t pos = 16;
	unsigned int b;
	int ret = 0;

	if (!p || !bound || size < 16 || size > ANE_H16_TASK_MAX || (size & 3))
		return -ENOEXEC;
	task = kmalloc_array(2, sizeof(*task), GFP_KERNEL);
	if (!task)
		return -ENOMEM;
	run = task + 1;
	ane_hx_dma_defaults(run);
	while (!ret && pos < size) {
		u32 words, header_words = 9;
		size_t end, rec;

		if (size - pos < 16) {
			ret = -ENOEXEC;
			break;
		}
		words = (get_unaligned_le32(p + pos) >> 16) & 0x7ff;
		if (!words) {
			pos += 16;
			continue;
		}
		if (words < 9 || words > (size - pos) / 4) {
			ret = -ENOEXEC;
			break;
		}
		end = pos + words * 4;
		if ((get_unaligned_le32(p + pos + 8 * 4) & 3) == 3)
			header_words++;
		if (words < header_words) {
			ret = -ENOEXEC;
			break;
		}
		rec = pos + header_words * 4;
		ane_hx_dma_defaults(task);
		ret = ane_hx_dma_apply(p, rec, end, task);
		if (!ret)
			ret = ane_hx_dma_image_check(task->img);
		if (!ret)
			ret = ane_hx_dma_enabled_check(task);
		if (!ret)
			ret = ane_hx_dma_apply(p, rec, end, run);
		if (!ret)
			ret = ane_hx_dma_image_check(run->img);
		for (b = 0; !ret && b < ANE_HX_DMA_BASES; b++) {
			if (task->written & BIT(b))
				ret = ane_hx_dma_fits(task, b, bound);
			if (!ret && (run->written & BIT(b)))
				ret = ane_hx_dma_fits(run, b, bound);
		}
		pos = end == size ? end : ALIGN(end, 16);
	}
	kfree(task);
	return ret;
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
