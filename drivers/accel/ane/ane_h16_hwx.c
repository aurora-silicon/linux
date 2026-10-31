// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

#include "ane_h16_hwx.h"
#include "ane_h16_tasks.h"

#define HWX_COMMANDS_MAX 64
#define HWX_SYMBOLS_MAX 4096
#define HWX_SEGMENTS_MAX 8

struct hwx_range {
	u64 start, size;
};

struct hwx_parser {
	const u8 *bytes;
	size_t size, command_end;
	struct ane_h16_hwx info;
	struct hwx_range vm[HWX_SEGMENTS_MAX], file[HWX_SEGMENTS_MAX + 4];
	struct ane_h16_hwx_section sections[ANE_H16_SECTIONS];
	u32 tensors[ANE_H16_SURFACES], refs[ANE_H16_SURFACES];
	u32 tensor_count, ref_count, segment_count, file_count, section_count;
	u32 symoff, nsyms, stroff, strsize;
	bool guard, symtab, metadata;
};

static bool hwx_fits(u64 start, u64 length, u64 size)
{
	return start <= size && length <= size - start;
}

static bool hwx_name(const u8 *p, const char *name)
{
	size_t length = strlen(name);

	return length < 16 && !memcmp(p, name, length) &&
	       !memchr_inv(p + length, 0, 16 - length);
}

static bool hwx_zero(const u8 *p, size_t start, size_t end)
{
	return !memchr_inv(p + start, 0, end - start);
}

static int hwx_range_add(struct hwx_range *ranges, u32 *count, u32 max,
			 u64 start, u64 size)
{
	u64 end;
	unsigned int i;

	if (!size || check_add_overflow(start, size, &end) || *count == max)
		return -ENOEXEC;
	for (i = 0; i < *count; i++)
		if (start < ranges[i].start + ranges[i].size &&
		    ranges[i].start < end)
			return -ENOEXEC;
	ranges[(*count)++] = (struct hwx_range) { start, size };
	return 0;
}

static int hwx_file_add(struct hwx_parser *p, u64 start, u64 size)
{
	if (start < p->command_end || !hwx_fits(start, size, p->size))
		return -ENOEXEC;
	return hwx_range_add(p->file, &p->file_count, ARRAY_SIZE(p->file), start, size);
}

static int hwx_segment(struct hwx_parser *p, const u8 *c, u32 length)
{
	struct ane_h16_hwx_section *dest;
	u64 vm, size, file, filesz;
	u32 nsects, prot, i;
	bool text, kernel, scratch, surface, guard;

	if (length < 72)
		return -ENOEXEC;
	vm = get_unaligned_le64(c + 24);
	size = get_unaligned_le64(c + 32);
	file = get_unaligned_le64(c + 40);
	filesz = get_unaligned_le64(c + 48);
	prot = get_unaligned_le32(c + 60);
	nsects = get_unaligned_le32(c + 64);
	if (nsects > ANE_H16_SECTIONS || length != 72 + nsects * 80 ||
	    prot != get_unaligned_le32(c + 56) ||
	    hwx_range_add(p->vm, &p->segment_count, ARRAY_SIZE(p->vm), vm, size))
		return -ENOEXEC;
	text = hwx_name(c + 8, "__TEXT");
	kernel = hwx_name(c + 8, "__KERN_0");
	scratch = hwx_name(c + 8, "__DATA");
	surface = hwx_name(c + 8, "__FVMLIB");
	guard = hwx_name(c + 8, "__PAGEZERO");
	if (!text && !kernel && !scratch && !surface && !guard)
		return -ENOEXEC;
	if (guard) {
		if (p->guard || vm || size != SZ_16K || file || filesz || prot || nsects)
			return -ENOEXEC;
		if (get_unaligned_le32(c + 68) != 4)
			return -ENOEXEC;
		p->guard = true;
		return 0;
	}
	if (surface || scratch) {
		if (file || filesz || !IS_ALIGNED(vm, SZ_16K) ||
		    !IS_ALIGNED(size, SZ_16K) || nsects != 1 || size > SZ_256M)
			return -ENOEXEC;
	} else if (!filesz || filesz > size || size > SZ_256M ||
		   !IS_ALIGNED(vm, SZ_16K) || !IS_ALIGNED(file, SZ_16K) ||
		   hwx_file_add(p, file, filesz)) {
		return -ENOEXEC;
	}
	if (get_unaligned_le32(c + 68) != (surface ? 6 : text ?
		(p->info.scratch.size ? 4 : 0) : 4))
		return -ENOEXEC;
	if ((text && (prot != 5 || nsects != 2 || p->info.text.size)) ||
	    (kernel && (prot != 1 || nsects != 1 || p->info.kernel.size)) ||
	    (scratch && (prot != 3 || p->info.scratch.size)) ||
	    (surface && (prot < 1 || prot > 2 ||
			 p->info.surface_count == ANE_H16_SURFACES)))
		return -ENOEXEC;

	for (i = 0; i < nsects; i++) {
		const u8 *s = c + 72 + i * 80;
		struct ane_h16_hwx_section section = {
			.vm = get_unaligned_le64(s + 32),
			.size = get_unaligned_le64(s + 40),
			.offset = get_unaligned_le32(s + 48),
			.reloff = get_unaligned_le32(s + 56),
			.nreloc = get_unaligned_le32(s + 60),
			.ordinal = p->section_count + 1,
		};
		u32 alignment = get_unaligned_le32(s + 52), j;
		bool task = text && hwx_name(s, "__text");

		if (p->section_count == ANE_H16_SECTIONS || !section.size ||
		    memcmp(s + 16, c + 8, 16) || alignment > 31 ||
		    !IS_ALIGNED(section.vm, 1ULL << alignment) ||
		    section.vm < vm || !hwx_fits(section.vm - vm, section.size, size) ||
		    !hwx_zero(s, 68, 80) || section.reloff > p->size)
			return -ENOEXEC;
		for (j = 0; j < p->section_count; j++) {
			const struct ane_h16_hwx_section *a = &p->sections[j];

			if (section.vm < a->vm + a->size && a->vm < section.vm + section.size)
				return -ENOEXEC;
		}
		if (surface || scratch) {
			if (section.offset || section.reloff || section.nreloc || section.vm != vm)
				return -ENOEXEC;
		} else if (section.offset < file ||
			   !IS_ALIGNED(section.offset, 1ULL << alignment) ||
			   !hwx_fits(section.offset - file, section.size, filesz) ||
			   section.offset - file != section.vm - vm) {
			return -ENOEXEC;
		}
		if (section.nreloc) {
			if (!task || section.nreloc > ANE_H16_TASK_MAX / 4 ||
			    hwx_file_add(p, section.reloff, (u64)section.nreloc * 8))
				return -ENOEXEC;
		}
		if (text) {
			if (task)
				dest = &p->info.text;
			else if (hwx_name(s, "__const"))
				dest = &p->info.constants;
			else
				return -ENOEXEC;
		} else if (kernel && hwx_name(s, "__kern_0")) {
			dest = &p->info.kernel;
			if (!IS_ALIGNED(section.vm, 64) || section.size > SZ_256M)
				return -ENOEXEC;
		} else if (scratch && hwx_name(s, "__bss")) {
			dest = &p->info.scratch;
		} else if (surface && hwx_name(s, prot == 1 ? "__const" : "__data")) {
			struct ane_h16_hwx_surface *b = &p->info.surfaces[p->info.surface_count++];

			b->allocation = size;
			b->direction = prot;
			dest = &b->section;
		} else {
			return -ENOEXEC;
		}
		if (get_unaligned_le32(s + 64) != (task ?
		    (section.nreloc ? 0x128 : 0x28) : surface ?
		    (prot == 1 ? 0x21 : 0x23) : scratch ? 0x25 : 0x26))
			return -ENOEXEC;
		if (dest->size)
			return -ENOEXEC;
		*dest = section;
		p->sections[p->section_count++] = section;
	}
	if (p->info.region_count == ANE_H16_REGIONS)
		return -ENOEXEC;
	p->info.regions[p->info.region_count++] = (struct ane_h16_hwx_region) {
		.vm = vm,
		.size = size,
		.file_offset = file,
		.file_size = filesz,
		.permissions = prot,
		.kind = surface ? ANE_H16_REGION_SURFACE : text ? ANE_H16_REGION_TEXT :
			kernel ? ANE_H16_REGION_KERNEL : ANE_H16_REGION_SCRATCH,
	};
	return 0;
}

static bool hwx_format(u32 code)
{
	static const u8 known[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 15, 16, 17,
		18, 20, 21, 23, 27, 30, 31, 36, 41, 43, 72, 145,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(known); i++)
		if (known[i] == code)
			return true;
	return false;
}

static int hwx_program(struct hwx_parser *p)
{
	const u8 *c = p->bytes + p->info.program_offset;
	struct ane_h16_hwx *info = &p->info;
	u32 length = get_unaligned_le32(c + 4), records, i;
	bool mixed = length == 0x8c0;
	/* Nonzero cells only; every other byte is checked below. */
	static const u16 common[] = {
		0, 4, 8, 0xc, 0x10, 0x14, 0x20, 0x24, 0x40, 0x44,
		0x810, 0x814, 0x820, 0x824, 0x830, 0x838, 0x83c,
		0x844, 0x848, 0x858, 0x860,
	};
	u8 *expected;
	int ret = -ENOEXEC;

	if (length != 0x8a0 && !mixed)
		return -ENOEXEC;
	if (get_unaligned_le32(c + 0xc) != (mixed ? 0x22a : 0x222) ||
	    get_unaligned_le64(c + 0x10) != info->text.vm ||
	    get_unaligned_le64(c + 0x810) != info->text.vm ||
	    get_unaligned_le64(c + 0x20) != info->constants.vm ||
	    get_unaligned_le32(c + 0x820) != 4 ||
	    (u64)get_unaligned_le32(c + 0x824) * 4 != info->text.size ||
	    get_unaligned_le32(c + 0x838) != U32_MAX ||
	    get_unaligned_le32(c + 0x83c) != U32_MAX ||
	    get_unaligned_le32(c + 0x844) != 0xffff ||
	    get_unaligned_le32(c + 0x848) != (mixed ? 0x8b8 : 0x898))
		return -ENOEXEC;
	records = get_unaligned_le32(c + 0x860);
	if (records != 0x10 && records != 0x12 && records != 0x1f && records != 0x20)
		return -ENOEXEC;
	info->tasks = get_unaligned_le32(c + 0x830);
	if (mixed) {
		u32 usable = get_unaligned_le32(c + 0x858);

		if (!info->scratch.size || !usable || usable > info->scratch.size ||
		    get_unaligned_le64(c + 0x40) != info->scratch.vm ||
		    get_unaligned_le32(c + 0x880) != usable)
			return -ENOEXEC;
	} else if (info->scratch.size || get_unaligned_le64(c + 0x40) ||
		   get_unaligned_le32(c + 0x858)) {
		return -ENOEXEC;
	}
	for (i = 0; i < 5; i++) {
		u64 base = i < info->surface_count ? info->surfaces[i].section.vm :
			   i == info->surface_count ? info->kernel.vm : 0;

		if (get_unaligned_le64(c + 0x70 + i * 16) != base ||
		    get_unaligned_le64(c + 0x78 + i * 16))
			return -ENOEXEC;
	}
	if (!hwx_format(get_unaligned_le32(c + (mixed ? 0x8b0 : 0x890))))
		return -ENOEXEC;

	/* Construct the exact fixed trailer and reserved-space expectation. */
	expected = kzalloc(length, GFP_KERNEL);
	if (!expected)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(common); i++)
		memcpy(expected + common[i], c + common[i], 4);
	for (i = 0; i < 5; i++)
		memcpy(expected + 0x70 + i * 16, c + 0x70 + i * 16, 8);
	put_unaligned_le32(4, expected + 0x868);
	put_unaligned_le32(mixed ? 4 : 2, expected + 0x870);
	put_unaligned_le32(mixed ? 2 : 7, expected + 0x878);
	put_unaligned_le32(8, expected + 0x87c);
	put_unaligned_le32(mixed ? get_unaligned_le32(c + 0x858) : 0x461c4000,
			   expected + 0x880);
	put_unaligned_le32(mixed ? 3 : 9, expected + 0x888);
	put_unaligned_le32(8, expected + 0x88c);
	if (mixed) {
		put_unaligned_le32(0x08020004, expected + 0x890);
		put_unaligned_le32(8, expected + 0x894);
		put_unaligned_le32(7, expected + 0x898);
		put_unaligned_le32(8, expected + 0x89c);
		put_unaligned_le32(0x461c4000, expected + 0x8a0);
		put_unaligned_le32(9, expected + 0x8a8);
		put_unaligned_le32(8, expected + 0x8ac);
	}
	memcpy(expected + (mixed ? 0x8b0 : 0x890), c + (mixed ? 0x8b0 : 0x890), 4);
	memcpy(expected + (mixed ? 0x8b8 : 0x898), "main", 5);
	if (!memcmp(c, expected, length))
		ret = ane_h16_task_stream_validate(p->bytes + info->text.offset,
						   info->text.size, info->tasks);
	kfree(expected);
	return ret;
}

static int hwx_tensor(struct hwx_parser *p, u32 index)
{
	struct ane_h16_hwx_surface *b = &p->info.surfaces[index];
	const u8 *c = p->bytes + p->tensors[index];
	const u8 *ref = p->bytes + p->refs[index];
	u32 length = get_unaligned_le32(c + 4), shortoff;
	u64 n, channels, height, width, row, plane, batch, minimum;
	size_t symbol_length, short_length;
	unsigned int i;

	if (length < 0xd48 || length > SZ_16K || get_unaligned_le32(c + 0xc) != 0x34a ||
	    get_unaligned_le32(c + 0x10) != 3 ||
	    get_unaligned_le32(c + 0x14) != b->direction ||
	    get_unaligned_le32(c + 0x18) != 0xd38 || !hwx_zero(c, 0x1c, 0x20) ||
	    get_unaligned_le32(c + 0x20) != 0xd3d ||
	    get_unaligned_le32(c + 0x24) != 5 ||
	    get_unaligned_le32(c + 0x38) != 1 || !hwx_zero(c, 0x3c, 0x50) ||
	    get_unaligned_le64(c + 0x68) != 2 ||
	    get_unaligned_le32(c + 0x78) != 1 || !hwx_zero(c, 0x88, 0xd38) ||
	    memcmp(c + 0xd38, "main", 5))
		return -ENOEXEC;
	n = get_unaligned_le32(c + 0x28);
	channels = get_unaligned_le32(c + 0x2c);
	height = get_unaligned_le32(c + 0x30);
	width = get_unaligned_le32(c + 0x34);
	batch = get_unaligned_le64(c + 0x50);
	plane = get_unaligned_le64(c + 0x58);
	row = get_unaligned_le64(c + 0x60);
	if (!n || !channels || !height || !width ||
	    check_mul_overflow(width, 2ULL, &minimum) || row < minimum ||
	    check_mul_overflow(height, row, &minimum) || plane < minimum ||
	    check_mul_overflow(channels, plane, &minimum) || batch < minimum ||
	    get_unaligned_le64(c + 0x70) != batch ||
	    get_unaligned_le64(c + 0x80) != batch ||
	    check_mul_overflow(n, batch, &b->span) || b->span > b->section.size)
		return -ENOEXEC;
	symbol_length = strnlen(c + 0xd3d, min_t(u32, 256, length - 0xd3d));
	if (!symbol_length || symbol_length == min_t(u32, 256, length - 0xd3d))
		return -ENOEXEC;
	shortoff = get_unaligned_le32(c + 0x7c);
	if (shortoff != 0xd3d + symbol_length + 1 || shortoff >= length)
		return -ENOEXEC;
	short_length = strnlen(c + shortoff, length - shortoff);
	if (!short_length || short_length == length - shortoff ||
	    short_length >= sizeof(b->name) ||
	    !hwx_zero(c, shortoff + short_length + 1, length))
		return -ENOEXEC;
	memcpy(b->name, c + shortoff, short_length);
	b->symbol_offset = p->tensors[index] + 0xd3d;
	if (get_unaligned_le64(ref + 8) != 0x18 ||
	    get_unaligned_le64(ref + 0x10) != b->section.vm ||
	    memcmp(ref + 0x18, b->name, sizeof(b->name)))
		return -ENOEXEC;
	for (i = 0; i < index; i++)
		if (!strcmp(b->name, p->info.surfaces[i].name) ||
		    !strcmp(p->bytes + b->symbol_offset,
			    p->bytes + p->info.surfaces[i].symbol_offset))
			return -ENOEXEC;
	return 0;
}

static int hwx_symbols(struct hwx_parser *p)
{
	u32 found = 0, i, j;

	if (!p->symtab || !p->nsyms || p->nsyms > HWX_SYMBOLS_MAX ||
	    !p->strsize || p->strsize > SZ_256K ||
	    hwx_file_add(p, p->symoff, (u64)p->nsyms * 16) ||
	    hwx_file_add(p, p->stroff, p->strsize) || p->bytes[p->stroff])
		return -ENOEXEC;
	for (i = 0; i < p->nsyms; i++) {
		const u8 *s = p->bytes + p->symoff + i * 16;
		u32 strx = get_unaligned_le32(s);
		u64 value = get_unaligned_le64(s + 8);
		const struct ane_h16_hwx_section *section;
		const char *name;

		if (strx >= p->strsize || !s[5] || s[5] > p->section_count ||
		    (s[4] & ~1) != 0xe)
			return -ENOEXEC;
		name = p->bytes + p->stroff + strx;
		if (!*name || strnlen(name, min_t(u32, 256, p->strsize - strx)) ==
		    min_t(u32, 256, p->strsize - strx))
			return -ENOEXEC;
		for (j = 0; j < i; j++) {
			u32 previous = get_unaligned_le32(p->bytes + p->symoff + j * 16);

			if (!strcmp(name, p->bytes + p->stroff + previous))
				return -ENOEXEC;
		}
		section = &p->sections[s[5] - 1];
		if (value < section->vm || value - section->vm >= section->size)
			return -ENOEXEC;
		for (j = 0; j < p->info.surface_count; j++) {
			const struct ane_h16_hwx_surface *b = &p->info.surfaces[j];

			if (strcmp(name, b->name))
				continue;
			if ((found & BIT(j)) || s[5] != b->section.ordinal ||
			    value != b->section.vm)
				return -ENOEXEC;
			found |= BIT(j);
		}
	}
	return found == BIT(p->info.surface_count) - 1 ? 0 : -ENOEXEC;
}

/* The stream has already passed the complete structural walk. Clear every
 * payload site from the relocation bitmap: any remaining bit names a prefix,
 * header, register selector or padding byte, which must never be relocated.
 */
static bool hwx_relocations_in_payload(const struct hwx_parser *p, unsigned long *sites)
{
	const u8 *text = p->bytes + p->info.text.offset;
	size_t pos = 16;

	while (pos < p->info.text.size) {
		u32 words = (get_unaligned_le32(text + pos) >> 16) & 0x7ff;
		size_t end, record;

		if (!words) {
			pos += 16;
			continue;
		}
		end = pos + words * 4;
		record = pos + 9 * 4;
		if ((get_unaligned_le32(text + pos + 8 * 4) & 3) == 3)
			record += 4;
		while (record < end) {
			u32 header = get_unaligned_le32(text + record);
			u32 count = header & BIT(31) ?
				1 + hweight32((header >> 15) & 0xffff) :
				1 + ((header >> 15) & 0x3f);

			record += 4;
			bitmap_clear(sites, record / 4, count);
			record += count * 4;
		}
		pos = ALIGN(end, 16);
	}
	return bitmap_empty(sites, p->info.text.size / 4);
}

int ane_h16_hwx_validate(const void *bytes, size_t size, struct ane_h16_hwx *info)
{
	struct hwx_parser *p;
	const void *relocations;
	const u8 *c = bytes;
	size_t pos = 32;
	u32 count, length, i;
	unsigned long *seen = NULL;
	int ret = -ENOEXEC;

	if (!c || !info || size < 32 || size > ANE_H16_HWX_MAX ||
	    get_unaligned_le32(c) != 0xbeefface || get_unaligned_le32(c + 4) != 0x80 ||
	    get_unaligned_le32(c + 8) != 7 || get_unaligned_le32(c + 12) != 2 ||
	    get_unaligned_le32(c + 24) != 0x200000 || get_unaligned_le32(c + 28))
		return -ENOEXEC;
	count = get_unaligned_le32(c + 16);
	length = get_unaligned_le32(c + 20);
	if (!count || count > HWX_COMMANDS_MAX || !hwx_fits(32, length, size))
		return -ENOEXEC;
	p = kzalloc_obj(*p);
	if (!p)
		return -ENOMEM;
	p->bytes = bytes;
	p->size = size;
	p->command_end = 32 + (size_t)length;
	for (i = 0; i < count; i++) {
		u32 cmd, len;

		if (!hwx_fits(pos, 8, p->command_end))
			goto invalid;
		c = p->bytes + pos;
		cmd = get_unaligned_le32(c);
		len = get_unaligned_le32(c + 4);
		if (len < 8 || (len & 7) || !hwx_fits(pos, len, p->command_end))
			goto invalid;
		switch (cmd) {
		case 0x19:
			ret = hwx_segment(p, c, len);
			if (ret)
				goto out;
			break;
		case 2:
			if (p->symtab || len != 24)
				goto invalid;
			p->symtab = true;
			p->symoff = get_unaligned_le32(c + 8);
			p->nsyms = get_unaligned_le32(c + 12);
			p->stroff = get_unaligned_le32(c + 16);
			p->strsize = get_unaligned_le32(c + 20);
			break;
		case 4:
			if (len < 16)
				goto invalid;
			if (get_unaligned_le32(c + 8) == 3) {
				if (p->tensor_count == ANE_H16_SURFACES || len < 0xd48)
					goto invalid;
				p->tensors[p->tensor_count++] = pos;
			} else if (get_unaligned_le32(c + 8) == 4) {
				if (p->info.program_offset || (len != 0x8a0 && len != 0x8c0))
					goto invalid;
				p->info.program_offset = pos;
			} else {
				goto invalid;
			}
			break;
		case 0x40:
			if (len != 32 || p->ref_count == ANE_H16_SURFACES)
				goto invalid;
			p->refs[p->ref_count++] = pos;
			break;
		case 8: {
			size_t annotation;

			/* Compiler annotation has no authority over mappings. */
			if (p->metadata || len > SZ_4K || len == 8)
				goto invalid;
			annotation = strnlen(c + 8, len - 8);
			if (annotation == len - 8 || !hwx_zero(c, 8 + annotation, len))
				goto invalid;
			p->metadata = true;
			break;
		}
		default:
			goto invalid;
		}
		pos += len;
	}
	if (pos != p->command_end || !p->guard || !p->info.program_offset ||
	    !p->info.text.size || !p->info.constants.size ||
	    p->info.surface_count < 2 || p->ref_count != p->info.surface_count ||
	    p->tensor_count != p->info.surface_count)
		goto invalid;
	for (i = 0; i < p->info.surface_count; i++) {
		/* Inputs in segment order, exactly one output last. */
		if (p->info.surfaces[i].direction != (i == p->info.surface_count - 1 ? 2 : 1))
			goto invalid;
		ret = hwx_tensor(p, i);
		if (ret)
			goto out;
	}
	ret = hwx_program(p);
	if (ret)
		goto out;
	ret = hwx_symbols(p);
	if (ret)
		goto out;
	if (p->info.text.nreloc) {
		seen = bitmap_zalloc(p->info.text.size / 4, GFP_KERNEL);
		if (!seen) {
			ret = -ENOMEM;
			goto out;
		}
	}
	relocations = p->info.text.nreloc ? p->bytes + p->info.text.reloff : NULL;
	ret = ane_h16_relocations_validate(p->bytes + p->info.text.offset,
					   p->info.text.size, relocations,
					   p->info.text.nreloc, p->info.kernel.ordinal,
					   p->info.kernel.vm, p->info.kernel.size,
					   seen, p->info.text.size / 4);
	if (!ret && p->info.text.nreloc && !hwx_relocations_in_payload(p, seen))
		ret = -ENOEXEC;
	if (!ret)
		*info = p->info;
	goto out;
invalid:
	ret = -ENOEXEC;
out:
	bitmap_free(seen);
	kfree(p);
	return ret;
}
EXPORT_SYMBOL_GPL(ane_h16_hwx_validate);

static int hwx_snapshot_finish(struct ane_h16_hwx_snapshot *copy,
			       struct ane_h16_hwx_snapshot *snapshot)
{
	int ret;

	ret = ane_h16_hwx_validate(copy->bytes, copy->size, &copy->info);
	if (ret) {
		kvfree(copy->bytes);
		return ret;
	}
	sha256(copy->bytes, copy->size, copy->digest);
	*snapshot = *copy;
	return 0;
}

int ane_h16_hwx_snapshot_create(const void *bytes, size_t size,
				struct ane_h16_hwx_snapshot *snapshot)
{
	struct ane_h16_hwx_snapshot copy = {};

	if (!bytes || !snapshot || size < 32 || size > ANE_H16_HWX_MAX)
		return -EINVAL;
	if (snapshot->bytes)
		return -EBUSY;
	copy.bytes = kvmalloc(size, GFP_KERNEL);
	if (!copy.bytes)
		return -ENOMEM;
	memcpy(copy.bytes, bytes, size);
	copy.size = size;
	return hwx_snapshot_finish(&copy, snapshot);
}
EXPORT_SYMBOL_GPL(ane_h16_hwx_snapshot_create);

/* Import exactly once before parsing or hashing. No userspace digest,
 * section list or resource IDs can substitute for this original container.
 * Callers supply an empty destination and retain it until all firmware
 * references end; importing alone grants no process or execution authority.
 */
int ane_h16_hwx_snapshot_from_user(const void __user *bytes, u64 size,
				   struct ane_h16_hwx_snapshot *snapshot)
{
	struct ane_h16_hwx_snapshot copy = {};

	if (!bytes || !snapshot || size < 32 || size > ANE_H16_HWX_MAX)
		return -EINVAL;
	if (snapshot->bytes)
		return -EBUSY;
	copy.bytes = vmemdup_user(bytes, size);
	if (IS_ERR(copy.bytes))
		return PTR_ERR(copy.bytes);
	copy.size = size;
	return hwx_snapshot_finish(&copy, snapshot);
}
EXPORT_SYMBOL_GPL(ane_h16_hwx_snapshot_from_user);

int ane_h16_hwx_snapshot_check(const struct ane_h16_hwx_snapshot *snapshot)
{
	u8 digest[SHA256_DIGEST_SIZE];

	if (!snapshot || !snapshot->bytes || snapshot->size < 32 ||
	    snapshot->size > ANE_H16_HWX_MAX)
		return -ENOEXEC;
	sha256(snapshot->bytes, snapshot->size, digest);
	return memcmp(digest, snapshot->digest, sizeof(digest)) ? -ENOEXEC : 0;
}
EXPORT_SYMBOL_GPL(ane_h16_hwx_snapshot_check);

void ane_h16_hwx_snapshot_destroy(struct ane_h16_hwx_snapshot *snapshot)
{
	kvfree(snapshot->bytes);
	memset(snapshot, 0, sizeof(*snapshot));
}
EXPORT_SYMBOL_GPL(ane_h16_hwx_snapshot_destroy);

MODULE_DESCRIPTION("H16 original HWX validation");
MODULE_LICENSE("Dual MIT/GPL");
