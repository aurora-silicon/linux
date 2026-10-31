// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "ane_h16_hwx.h"
#include "ane_h16_tasks.h"
#include "ane_h16_load.h"

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

static void hwx_forms_test(struct kunit *test)
{
	unsigned int variant;

	for (variant = 0; variant < 4; variant++) {
		struct hwx_fixture *f = fixture_create(test, variant & 1, variant & 2);
		struct ane_h16_hwx info = {};

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), 0);
		KUNIT_EXPECT_EQ(test, info.surface_count, 2U);
		KUNIT_EXPECT_EQ(test, info.surfaces[0].span, 256ULL);
		KUNIT_EXPECT_EQ(test, info.tasks, 1U);
		KUNIT_EXPECT_EQ(test, info.text.ordinal, (variant & 1) ? 4U : 3U);
		KUNIT_EXPECT_EQ(test, info.text.nreloc, (variant & 2) ? 1U : 0U);
	}
}

static void hwx_header_test(struct kunit *test)
{
	struct hwx_fixture *f = fixture_create(test, false, false);
	struct ane_h16_hwx info;
	u32 offset;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (offset = 0; offset < 32; offset += 4) {
		u32 saved = get_unaligned_le32(f->bytes + offset);

		put_unaligned_le32(U32_MAX, f->bytes + offset);
		KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
		put_unaligned_le32(saved, f->bytes + offset);
	}
	for (offset = 0; offset < 32; offset++)
		KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, offset, &info), -ENOEXEC);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size - 1, &info), -ENOEXEC);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, ANE_H16_HWX_MAX + 1, &info), -ENOEXEC);
	/* A truncated command must not inherit a previous command's success. */
	put_unaligned_le32(f->cursor - 33, f->bytes + 20);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
}

static void hwx_command_test(struct kunit *test)
{
	struct hwx_fixture *f = fixture_create(test, false, false);
	struct ane_h16_hwx info;
	u32 pos;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (pos = 32; pos < f->cursor;) {
		u32 command = get_unaligned_le32(f->bytes + pos);
		u32 length = get_unaligned_le32(f->bytes + pos + 4);

		put_unaligned_le32(0xffffffff, f->bytes + pos);
		KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
		put_unaligned_le32(command, f->bytes + pos);
		put_unaligned_le32(0, f->bytes + pos + 4);
		KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
		put_unaligned_le32(length, f->bytes + pos + 4);
		pos += length;
	}
}

static void hwx_tensor_test(struct kunit *test)
{
	static const u32 fields[] = {
		0xc, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c,
		0x30, 0x34, 0x38, 0x3c, 0x50, 0x58, 0x60, 0x68, 0x70,
		0x78, 0x7c, 0x80, 0x88, 0xd38, 0xd44,
	};
	struct hwx_fixture *f = fixture_create(test, false, false);
	struct ane_h16_hwx info;
	u8 *tensor;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	tensor = f->bytes + f->tensor[0];
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		u32 saved = get_unaligned_le32(tensor + fields[i]);

		put_unaligned_le32(U32_MAX, tensor + fields[i]);
		KUNIT_EXPECT_EQ_MSG(test, ane_h16_hwx_validate(f->bytes, f->size, &info),
				    -ENOEXEC, "tensor field %#x", fields[i]);
		put_unaligned_le32(saved, tensor + fields[i]);
	}
	/* Four batches fit in the allocation; span is not just one batch. */
	put_unaligned_le32(4, tensor + 0x28);
	KUNIT_ASSERT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), 0);
	KUNIT_EXPECT_EQ(test, info.surfaces[0].span, 1024ULL);
	put_unaligned_le64(U64_MAX, tensor + 0x50);
	put_unaligned_le64(U64_MAX, tensor + 0x70);
	put_unaligned_le64(U64_MAX, tensor + 0x80);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
}

static void hwx_program_test(struct kunit *test)
{
	static const u32 fields[] = {
		0xc, 0x10, 0x18, 0x20, 0x28, 0x40, 0x48, 0x70, 0x78, 0x80, 0x88,
		0x90, 0xa0, 0xb0, 0xb8, 0xc0, 0x80c, 0x810, 0x818, 0x820, 0x824,
		0x828, 0x830, 0x834, 0x838, 0x83c, 0x840, 0x844, 0x848, 0x84c,
		0x858, 0x85c, 0x860, 0x864, 0x868, 0x870, 0x878, 0x880, 0x890, 0x89c,
	};
	struct hwx_fixture *f = fixture_create(test, false, false);
	struct ane_h16_hwx info;
	u8 *program;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	program = f->bytes + f->program;
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		u32 saved = get_unaligned_le32(program + fields[i]);

		put_unaligned_le32(saved ^ U32_MAX, program + fields[i]);
		KUNIT_EXPECT_EQ_MSG(test, ane_h16_hwx_validate(f->bytes, f->size, &info),
				    -ENOEXEC, "program field %#x", fields[i]);
		put_unaligned_le32(saved, program + fields[i]);
	}
}

static void hwx_geometry_test(struct kunit *test)
{
	struct hwx_fixture *f = fixture_create(test, false, false);
	struct ane_h16_hwx info;
	u8 *segment;
	unsigned int variant;

	KUNIT_ASSERT_NOT_NULL(test, f);
	segment = f->bytes + f->segment[0];
	for (variant = 0; variant < 8; variant++) {
		u32 fields[] = { 24, 32, 40, 48, 56, 60, 64, 72 + 52 };
		u32 saved = get_unaligned_le32(segment + fields[variant]);

		put_unaligned_le32(U32_MAX, segment + fields[variant]);
		KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
		put_unaligned_le32(saved, segment + fields[variant]);
	}
	put_unaligned_le64(U64_MAX - SZ_16K + 1, segment + 24);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
}

static void hwx_symbols_test(struct kunit *test)
{
	struct hwx_fixture *f = fixture_create(test, false, false);
	struct ane_h16_hwx info;
	u32 field;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (field = 8; field < 24; field += 4) {
		u32 saved = get_unaligned_le32(f->bytes + f->symtab + field);

		put_unaligned_le32(U32_MAX, f->bytes + f->symtab + field);
		KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
		put_unaligned_le32(saved, f->bytes + f->symtab + field);
	}
	f->bytes[f->symbols + 5] = 0;
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
	f->bytes[f->symbols + 5] = 1;
	f->bytes[f->strings + 1] = 'X';
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
}

static void hwx_relocations_test(struct kunit *test)
{
	struct hwx_fixture *f = fixture_create(test, true, true);
	struct ane_h16_hwx info;

	KUNIT_ASSERT_NOT_NULL(test, f);
	/* Scratch changes the section ordinal; a fixed ordinal is invalid. */
	put_unaligned_le32(0x07000005, f->bytes + f->reloc + 4);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
	put_unaligned_le32(0x07000006, f->bytes + f->reloc + 4);
	put_unaligned_le32(64, f->bytes + SZ_16K + 56);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
	put_unaligned_le32(32, f->bytes + SZ_16K + 56);
	put_unaligned_le32(57, f->bytes + f->reloc);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
	/* A small addend in a header or the prefix must not be patched. */
	put_unaligned_le32(20, f->bytes + f->reloc);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
	put_unaligned_le32(4, f->bytes + f->reloc);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_validate(f->bytes, f->size, &info), -ENOEXEC);
}

static void hwx_snapshot_test(struct kunit *test)
{
	struct hwx_fixture *f = fixture_create(test, false, false);
	struct ane_h16_hwx_snapshot snapshot = {};

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, ane_h16_hwx_snapshot_create(f->bytes, f->size, &snapshot), 0);
	f->bytes[f->tensor[0] + 0x24] = 0;
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_snapshot_check(&snapshot), 0);
	/* Even padding and original command bytes are covered by the digest. */
	((u8 *)snapshot.bytes)[f->program + 0x100] = 1;
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_snapshot_check(&snapshot), -ENOEXEC);
	ane_h16_hwx_snapshot_destroy(&snapshot);
	KUNIT_EXPECT_PTR_EQ(test, snapshot.bytes, NULL);
}

static void hwx_snapshot_free(void *snapshot)
{
	ane_h16_hwx_snapshot_destroy(snapshot);
}

static struct ane_h16_hwx_snapshot *fixture_snapshot(struct kunit *test,
						     struct hwx_fixture *f)
{
	struct ane_h16_hwx_snapshot *snapshot;

	if (!f)
		return NULL;
	snapshot = kunit_kzalloc(test, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot || ane_h16_hwx_snapshot_create(f->bytes, f->size, snapshot))
		return NULL;
	if (kunit_add_action_or_reset(test, hwx_snapshot_free, snapshot))
		return NULL;
	return snapshot;
}

static void hwx_load_plan_test(struct kunit *test)
{
	unsigned int mixed, kernel;

	for (mixed = 0; mixed < 2; mixed++) {
		for (kernel = 0; kernel < 2; kernel++) {
			struct hwx_fixture *f = fixture_create(test, mixed, kernel);
			struct ane_h16_hwx_snapshot *snapshot = fixture_snapshot(test, f);
			struct ane_h16_load_plan plan;
			unsigned int text = 2 + mixed;

			KUNIT_ASSERT_NOT_NULL(test, snapshot);
			KUNIT_ASSERT_EQ(test, ane_h16_load_plan_create(snapshot, &plan), 0);
			KUNIT_EXPECT_EQ(test, plan.count, 3 + mixed + kernel);
			KUNIT_EXPECT_EQ(test, plan.total, (u64)(4 + mixed + kernel) * SZ_16K);
			KUNIT_EXPECT_EQ(test, plan.regions[text].kind, ANE_H16_REGION_TEXT);
			KUNIT_EXPECT_EQ(test, plan.regions[text].permissions, 5);
			KUNIT_EXPECT_EQ(test, plan.regions[text].file_offset, (u64)SZ_16K);
			KUNIT_EXPECT_EQ(test, plan.regions[text].file_size, (u64)SZ_32K);
			/* One TEXT allocation includes task and constant sections. */
			KUNIT_EXPECT_EQ(test, plan.allocation[text], (u64)SZ_32K);
			if (mixed) {
				KUNIT_EXPECT_EQ(test, plan.regions[0].kind, ANE_H16_REGION_SCRATCH);
				KUNIT_EXPECT_EQ(test, plan.regions[0].file_size, 0);
				KUNIT_EXPECT_EQ(test, plan.regions[0].permissions, 3);
			}
		}
	}
}

static void hwx_load_original_test(struct kunit *test)
{
	struct hwx_fixture *f = fixture_create(test, false, false);
	struct ane_h16_hwx_snapshot *snapshot = fixture_snapshot(test, f);
	struct ane_h16_load_plan plan;

	KUNIT_ASSERT_NOT_NULL(test, snapshot);
	/* Cached metadata cannot replace the original authenticated container. */
	memset(&snapshot->info, 0xff, sizeof(snapshot->info));
	KUNIT_ASSERT_EQ(test, ane_h16_load_plan_create(snapshot, &plan), 0);
	KUNIT_EXPECT_EQ(test, plan.count, 3);
	((u8 *)snapshot->bytes)[f->program] ^= 1;
	plan.count = 0x1234;
	KUNIT_EXPECT_EQ(test, ane_h16_load_plan_create(snapshot, &plan), -ENOEXEC);
	KUNIT_EXPECT_EQ(test, plan.count, 0x1234);
}

static void fixture_mappings(const struct ane_h16_load_plan *plan,
			     struct ane_h16_load_mapping *mappings)
{
	unsigned int i;

	for (i = 0; i < plan->count; i++) {
		mappings[i].dva = 0x20000000000ULL + (u64)i * SZ_1M;
		mappings[i].size = plan->allocation[i];
		mappings[i].permissions = plan->regions[i].permissions;
	}
}

static void hwx_load_resources_test(struct kunit *test)
{
	struct hwx_fixture *f = fixture_create(test, true, true);
	struct ane_h16_hwx_snapshot *snapshot = fixture_snapshot(test, f);
	struct ane_h16_load_mapping mappings[ANE_H16_REGIONS] = {};
	struct ane_h16_process_binding bindings[2];
	struct ane_h16_resource resources[2] = {};
	struct ane_h16_load_plan plan;
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, snapshot);
	KUNIT_ASSERT_EQ(test, ane_h16_load_plan_create(snapshot, &plan), 0);
	fixture_mappings(&plan, mappings);
	for (i = 0; i < 2; i++) {
		bindings[i].hwx_vm = snapshot->info.surfaces[i].section.vm;
		bindings[i].index = 123 + 11 * i;
		bindings[i].class = i;
	}
	ret = ane_h16_load_resources_create(snapshot, mappings, plan.count,
					    bindings, 2, resources);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, resources[0].buffer.mapping_dva, mappings[1].dva);
	KUNIT_EXPECT_EQ(test, resources[1].buffer.index, 134);
	KUNIT_EXPECT_EQ(test, resources[1].permissions, 2);
	for (i = 0; i < 7; i++) {
		struct ane_h16_load_mapping saved = mappings[1];

		switch (i) {
		case 0:
			mappings[1].dva++;
			break;
		case 1:
			mappings[1].size++;
			break;
		case 2:
			mappings[1].permissions = 3;
			break;
		case 3:
			mappings[1].dva = mappings[3].dva;
			break;
		case 4:
			mappings[1].dva = 0x10000000000ULL;
			break;
		case 5:
			mappings[1].dva = U64_MAX - SZ_16K + 1;
			break;
		case 6:
			mappings[1].dva = 0;
			break;
		}
		resources[0].buffer.index = 0x1234;
		ret = ane_h16_load_resources_create(snapshot, mappings, plan.count,
						    bindings, 2, resources);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
		KUNIT_EXPECT_EQ(test, resources[0].buffer.index, 0x1234);
		mappings[1] = saved;
	}
	bindings[1].index = bindings[0].index;
	ret = ane_h16_load_resources_create(snapshot, mappings, plan.count,
					    bindings, 2, resources);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	bindings[1].index++;
	bindings[1].class = 8;
	ret = ane_h16_load_resources_create(snapshot, mappings, plan.count,
					    bindings, 2, resources);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	bindings[1].class = 1;
	bindings[1].hwx_vm += SZ_16K;
	ret = ane_h16_load_resources_create(snapshot, mappings, plan.count,
					    bindings, 2, resources);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = ane_h16_load_resources_create(snapshot, mappings, plan.count - 1,
					    bindings, 2, resources);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = ane_h16_load_resources_create(snapshot, mappings, plan.count,
					    bindings, 1, resources);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void hwx_import_bounds_test(struct kunit *test)
{
	const void __user *invalid = (const void __user *)1UL;
	struct ane_h16_hwx_snapshot snapshot = {};
	struct hwx_fixture *f = fixture_create(test, false, false);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ret = ane_h16_hwx_snapshot_from_user(invalid, U64_MAX, &snapshot);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_snapshot_from_user(invalid, 31, &snapshot), -EINVAL);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_snapshot_from_user(invalid, 32, &snapshot), -EFAULT);
	KUNIT_EXPECT_PTR_EQ(test, snapshot.bytes, NULL);
	KUNIT_ASSERT_EQ(test, ane_h16_hwx_snapshot_create(f->bytes, f->size, &snapshot), 0);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_snapshot_create(f->bytes, f->size, &snapshot), -EBUSY);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_snapshot_from_user(invalid, 32, &snapshot), -EBUSY);
	KUNIT_EXPECT_EQ(test, ane_h16_hwx_snapshot_check(&snapshot), 0);
	ane_h16_hwx_snapshot_destroy(&snapshot);
}

static struct kunit_case hwx_cases[] = {
	KUNIT_CASE(hwx_forms_test),
	KUNIT_CASE(hwx_header_test),
	KUNIT_CASE(hwx_command_test),
	KUNIT_CASE(hwx_tensor_test),
	KUNIT_CASE(hwx_program_test),
	KUNIT_CASE(hwx_geometry_test),
	KUNIT_CASE(hwx_symbols_test),
	KUNIT_CASE(hwx_relocations_test),
	KUNIT_CASE(hwx_snapshot_test),
	KUNIT_CASE(hwx_load_plan_test),
	KUNIT_CASE(hwx_load_original_test),
	KUNIT_CASE(hwx_load_resources_test),
	KUNIT_CASE(hwx_import_bounds_test),
	{}
};

static struct kunit_suite hwx_suite = {
	.name = "ane-h16-hwx",
	.test_cases = hwx_cases,
};

kunit_test_suite(hwx_suite);
MODULE_LICENSE("Dual MIT/GPL");
