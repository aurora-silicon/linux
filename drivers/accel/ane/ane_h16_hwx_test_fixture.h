/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_HWX_TEST_FIXTURE_H__
#define __ANE_H16_HWX_TEST_FIXTURE_H__

/* Synthetic H16G containers shared by the HWX and program KUnit suites. */
#include <kunit/test.h>
#include <linux/string.h>
#include <linux/unaligned.h>

struct hwx_fixture {
	u8 *bytes;
	size_t size, cursor;
	u32 commands, program, tensor[2], segment[2], text_command, symtab, reloc;
	u32 symbols, strings;
};

static u8 *fixture_command(struct hwx_fixture *f, u32 command, u32 length)
{
	u8 *c = f->bytes + f->cursor;

	put_unaligned_le32(command, c);
	put_unaligned_le32(length, c + 4);
	f->cursor += length;
	f->commands++;
	return c;
}

static u8 *fixture_segment(struct hwx_fixture *f, const char *name, u64 vm,
			   u64 size, u32 offset, u32 prot, u32 flags, u32 sections)
{
	u8 *c = fixture_command(f, 0x19, 72 + sections * 80);

	strscpy(c + 8, name, 16);
	put_unaligned_le64(vm, c + 24);
	put_unaligned_le64(size, c + 32);
	put_unaligned_le64(offset, c + 40);
	put_unaligned_le64(offset ? size : 0, c + 48);
	put_unaligned_le32(prot, c + 56);
	put_unaligned_le32(prot, c + 60);
	put_unaligned_le32(sections, c + 64);
	put_unaligned_le32(flags, c + 68);
	return c + 72;
}

static void fixture_section(u8 *s, const char *segment, const char *name,
			    u64 vm, u64 size, u32 offset, u32 alignment, u32 flags)
{
	strscpy(s, name, 16);
	strscpy(s + 16, segment, 16);
	put_unaligned_le64(vm, s + 32);
	put_unaligned_le64(size, s + 40);
	put_unaligned_le32(offset, s + 48);
	put_unaligned_le32(alignment, s + 52);
	put_unaligned_le32(flags, s + 64);
}

static struct hwx_fixture *fixture_create(struct kunit *test, bool mixed, bool kernel)
{
	struct hwx_fixture *f;
	u64 surface = 0x30000000 + (mixed ? SZ_16K : 0);
	u64 text = surface + 2 * SZ_16K, kern = text + SZ_32K;
	u32 i, strings_length = 1;
	u8 *c, *s, *text_section;
	static const char * const names[] = { "in", "out" };
	static const char * const symbols[] = { "input", "output" };

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->size = kernel ? SZ_64K : SZ_32K + SZ_16K;
	f->bytes = kunit_kzalloc(test, f->size, GFP_KERNEL);
	if (!f->bytes)
		return NULL;
	f->cursor = 32;
	fixture_segment(f, "__PAGEZERO", 0, SZ_16K, 0, 0, 4, 0);
	if (mixed) {
		s = fixture_segment(f, "__DATA", 0x30000000, SZ_16K, 0, 3, 4, 1);
		fixture_section(s, "__DATA", "__bss", 0x30000000, SZ_16K, 0, 6, 0x25);
	}
	for (i = 0; i < 2; i++) {
		f->segment[i] = f->cursor;
		s = fixture_segment(f, "__FVMLIB", surface + i * SZ_16K, SZ_16K,
				    0, i + 1, 6, 1);
		fixture_section(s, "__FVMLIB", i ? "__data" : "__const",
				surface + i * SZ_16K, SZ_16K, 0, 14, i ? 0x23 : 0x21);
	}
	f->text_command = f->cursor;
	text_section = fixture_segment(f, "__TEXT", text, SZ_32K, SZ_16K, 5,
				       mixed ? 4 : 0, 2);
	fixture_section(text_section, "__TEXT", "__text", text, 64, SZ_16K,
			14, kernel ? 0x128 : 0x28);
	fixture_section(text_section + 80, "__TEXT", "__const", text + 64,
			SZ_16K, SZ_16K + 64, 6, 0x26);
	if (kernel) {
		s = fixture_segment(f, "__KERN_0", kern, SZ_16K, SZ_32K + SZ_16K, 1, 4, 1);
		fixture_section(s, "__KERN_0", "__kern_0", kern, 64,
				SZ_32K + SZ_16K, 6, 0x26);
	}
	for (i = 0; i < 2; i++) {
		c = fixture_command(f, 0x40, 32);
		put_unaligned_le64(0x18, c + 8);
		put_unaligned_le64(surface + i * SZ_16K, c + 0x10);
		strscpy(c + 0x18, names[i], 8);
	}
	f->program = f->cursor;
	c = fixture_command(f, 4, mixed ? 0x8c0 : 0x8a0);
	put_unaligned_le32(4, c + 8);
	put_unaligned_le32(mixed ? 0x22a : 0x222, c + 0xc);
	put_unaligned_le64(text, c + 0x10);
	put_unaligned_le64(text + 64, c + 0x20);
	if (mixed)
		put_unaligned_le64(0x30000000, c + 0x40);
	for (i = 0; i < 2; i++)
		put_unaligned_le64(surface + i * SZ_16K, c + 0x70 + i * 16);
	if (kernel)
		put_unaligned_le64(kern, c + 0x90);
	put_unaligned_le64(text, c + 0x810);
	put_unaligned_le32(4, c + 0x820);
	put_unaligned_le32(16, c + 0x824);
	put_unaligned_le32(1, c + 0x830);
	put_unaligned_le32(U32_MAX, c + 0x838);
	put_unaligned_le32(U32_MAX, c + 0x83c);
	put_unaligned_le32(0xffff, c + 0x844);
	put_unaligned_le32(mixed ? 0x8b8 : 0x898, c + 0x848);
	put_unaligned_le32(0x10, c + 0x860);
	put_unaligned_le32(4, c + 0x868);
	put_unaligned_le32(mixed ? 4 : 2, c + 0x870);
	put_unaligned_le32(mixed ? 2 : 7, c + 0x878);
	put_unaligned_le32(8, c + 0x87c);
	put_unaligned_le32(mixed ? 256 : 0x461c4000, c + 0x880);
	put_unaligned_le32(mixed ? 3 : 9, c + 0x888);
	put_unaligned_le32(8, c + 0x88c);
	if (mixed) {
		put_unaligned_le32(256, c + 0x858);
		put_unaligned_le32(0x08020004, c + 0x890);
		put_unaligned_le32(8, c + 0x894);
		put_unaligned_le32(7, c + 0x898);
		put_unaligned_le32(8, c + 0x89c);
		put_unaligned_le32(0x461c4000, c + 0x8a0);
		put_unaligned_le32(9, c + 0x8a8);
		put_unaligned_le32(8, c + 0x8ac);
	}
	memcpy(c + (mixed ? 0x8b8 : 0x898), "main", 5);
	for (i = 0; i < 2; i++) {
		u32 shortoff = 0xd3d + strlen(symbols[i]) + 1;

		f->tensor[i] = f->cursor;
		c = fixture_command(f, 4, 0xd48);
		put_unaligned_le32(3, c + 8);
		put_unaligned_le32(0x34a, c + 0xc);
		put_unaligned_le32(3, c + 0x10);
		put_unaligned_le32(i + 1, c + 0x14);
		put_unaligned_le32(0xd38, c + 0x18);
		put_unaligned_le32(0xd3d, c + 0x20);
		put_unaligned_le32(5, c + 0x24);
		put_unaligned_le32(1, c + 0x28);
		put_unaligned_le32(1, c + 0x2c);
		put_unaligned_le32(1, c + 0x30);
		put_unaligned_le32(128, c + 0x34);
		put_unaligned_le32(1, c + 0x38);
		put_unaligned_le64(256, c + 0x50);
		put_unaligned_le64(256, c + 0x58);
		put_unaligned_le64(256, c + 0x60);
		put_unaligned_le64(2, c + 0x68);
		put_unaligned_le64(256, c + 0x70);
		put_unaligned_le32(1, c + 0x78);
		put_unaligned_le32(shortoff, c + 0x7c);
		put_unaligned_le64(256, c + 0x80);
		memcpy(c + 0xd38, "main", 5);
		memcpy(c + 0xd3d, symbols[i], strlen(symbols[i]) + 1);
		memcpy(c + shortoff, names[i], strlen(names[i]) + 1);
	}
	f->symtab = f->cursor;
	c = fixture_command(f, 2, 24);
	f->symbols = f->cursor;
	f->strings = f->symbols + 2 * 16;
	for (i = 0; i < 2; i++) {
		u8 *symbol = f->bytes + f->symbols + i * 16;

		put_unaligned_le32(strings_length, symbol);
		symbol[4] = 0xf;
		symbol[5] = (mixed ? 2 : 1) + i;
		put_unaligned_le64(surface + i * SZ_16K, symbol + 8);
		memcpy(f->bytes + f->strings + strings_length, names[i], strlen(names[i]) + 1);
		strings_length += strlen(names[i]) + 1;
	}
	put_unaligned_le32(f->symbols, c + 8);
	put_unaligned_le32(2, c + 12);
	put_unaligned_le32(f->strings, c + 16);
	put_unaligned_le32(strings_length, c + 20);
	f->reloc = ALIGN(f->strings + strings_length, 8);
	if (kernel) {
		put_unaligned_le32(f->reloc, text_section + 56);
		put_unaligned_le32(1, text_section + 60);
		put_unaligned_le32(56, f->bytes + f->reloc);
		put_unaligned_le32(0x07000000 | (mixed ? 6 : 5), f->bytes + f->reloc + 4);
		put_unaligned_le32(32, f->bytes + SZ_16K + 56);
	}
	/* Fixed H16 task prologue: u32 1, then twelve zero bytes. */
	put_unaligned_le32(1, f->bytes + SZ_16K);
	put_unaligned_le32(11 << 16, f->bytes + SZ_16K + 16);
	put_unaligned_le32(0x4100 / 4, f->bytes + SZ_16K + 52);
	put_unaligned_le32(0xbeefface, f->bytes);
	put_unaligned_le32(0x80, f->bytes + 4);
	put_unaligned_le32(7, f->bytes + 8);
	put_unaligned_le32(2, f->bytes + 12);
	put_unaligned_le32(f->commands, f->bytes + 16);
	put_unaligned_le32(f->cursor - 32, f->bytes + 20);
	put_unaligned_le32(0x200000, f->bytes + 24);
	return f;
}

#endif /* __ANE_H16_HWX_TEST_FIXTURE_H__ */
