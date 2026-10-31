// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "ane_h16_hwx.h"
#include "ane_h16_tasks.h"
#include "ane_h16_load.h"

#include "ane_h16_hwx_test_fixture.h"

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
