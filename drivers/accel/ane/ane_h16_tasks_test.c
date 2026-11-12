// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "ane_h16_tasks.h"

static void ane_h16_dense_task_test(struct kunit *test)
{
	u8 text[64] = {};

	put_unaligned_le32(1, text);
	/* Nine header words, one dense record and its one-word payload. */
	put_unaligned_le32(11 << 16, text + 16);
	put_unaligned_le32(0x4100 / 4, text + 16 + 9 * 4);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), 0);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 2), -ENOEXEC);
	put_unaligned_le32(0x41a4 / 4, text + 16 + 9 * 4);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
	put_unaligned_le32(0x419c / 4 | (1 << 15), text + 16 + 9 * 4);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
	put_unaligned_le32(0x4100 / 4, text + 16 + 9 * 4);
	/* The last task may end the stream without padding (16 + 44 bytes). */
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, 16 + 11 * 4, 1), 0);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, 16 + 10 * 4, 1), -ENOEXEC);
	text[63] = 1;
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
}

static void ane_h16_scatter_task_test(struct kunit *test)
{
	u8 text[64] = {};

	put_unaligned_le32(1, text);
	put_unaligned_le32(12 << 16, text + 16);
	put_unaligned_le32(BIT(31) | BIT(15) | 21, text + 16 + 9 * 4);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), 0);
	/* The second scatter write would leave the common register block. */
	put_unaligned_le32(BIT(31) | BIT(15) | 22, text + 16 + 9 * 4);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
}

static void ane_h16_extra_header_test(struct kunit *test)
{
	u8 text[64] = {};

	put_unaligned_le32(1, text);
	put_unaligned_le32(10 << 16, text + 16);
	put_unaligned_le32(3, text + 16 + 8 * 4);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), 0);
	put_unaligned_le32(9 << 16, text + 16);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
	KUNIT_EXPECT_FALSE(test, ane_h16_register_valid(0x44d4 / 4));
	KUNIT_EXPECT_TRUE(test, ane_h16_register_valid(0x44d0 / 4));
	KUNIT_EXPECT_FALSE(test, ane_h16_register_valid(0x5930 / 4));
}

static void ane_h16_prologue_test(struct kunit *test)
{
	u8 text[64] = {};
	unsigned int i;

	put_unaligned_le32(11 << 16, text + 16);
	put_unaligned_le32(0x4100 / 4, text + 16 + 9 * 4);
	/* A zero prologue is not the H16 stream marker. */
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
	put_unaligned_le32(1, text);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), 0);
	put_unaligned_le32(2, text);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
	put_unaligned_le32(0x01000000, text);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
	put_unaligned_le32(1, text);
	for (i = 4; i < 16; i++) {
		text[i] = 1;
		KUNIT_EXPECT_EQ(test,
				ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
		text[i] = 0;
	}
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, 15, 1), -ENOEXEC);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, 16, 1), -ENOEXEC);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(NULL, sizeof(text), 1), -ENOEXEC);
}

static void ane_h16_relocation_test(struct kunit *test)
{
	u8 text[64] = {};
	u8 relocs[16] = {};
	DECLARE_BITMAP(seen, 16);
	int ret;

	put_unaligned_le32(4, relocs);
	put_unaligned_le32(0x07000003, relocs + 4);
	put_unaligned_le32(0x20, text + 4);
	ret = ane_h16_relocations_validate(text, sizeof(text),
					   relocs, 1, 3, 0x30020000, 0x100, seen, 16);
	KUNIT_EXPECT_EQ(test, ret, 0);
	memcpy(relocs + 8, relocs, 8);
	ret = ane_h16_relocations_validate(text, sizeof(text),
					   relocs, 2, 3, 0x30020000, 0x100, seen, 16);
	KUNIT_EXPECT_EQ(test, ret, -ENOEXEC);
	ret = ane_h16_relocations_validate(text, sizeof(text),
					   relocs, 1, 4, 0x30020000, 0x100, seen, 16);
	KUNIT_EXPECT_EQ(test, ret, -ENOEXEC);
	put_unaligned_le32(0x0f000003, relocs + 4);
	ret = ane_h16_relocations_validate(text, sizeof(text),
					   relocs, 1, 3, 0x30020000, 0x100, seen, 16);
	KUNIT_EXPECT_EQ(test, ret, -ENOEXEC);
	put_unaligned_le32(0x07000003, relocs + 4);
	put_unaligned_le32(0x100, text + 4);
	ret = ane_h16_relocations_validate(text, sizeof(text),
					   relocs, 1, 3, 0x30020000, 0x100, seen, 16);
	KUNIT_EXPECT_EQ(test, ret, -ENOEXEC);
	ret = ane_h16_relocations_validate(text, sizeof(text),
					   relocs, 1, 3, U64_MAX - 8, 0x100, seen, 16);
	KUNIT_EXPECT_EQ(test, ret, -ENOEXEC);
	ret = ane_h16_relocations_validate(text, sizeof(text),
					   relocs, 1, 3, 0x30020000, 0, seen, 16);
	KUNIT_EXPECT_EQ(test, ret, -ENOEXEC);
}

/* H17 streams open with 0, and the last task may end the section unpadded. */
static void ane_h17_prologue_test(struct kunit *test)
{
	u8 text[60] = {};

	put_unaligned_le32(11 << 16, text + 16);
	put_unaligned_le32(0x4100 / 4, text + 16 + 9 * 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_validate(text, sizeof(text), 1, 0), 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_validate(text, sizeof(text), 1, 1), -ENOEXEC);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
	put_unaligned_le32(1, text);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), 0);
	/* A task that overruns the section is still refused. */
	put_unaligned_le32(12 << 16, text + 16);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
}

static struct kunit_case ane_h16_task_cases[] = {
	KUNIT_CASE(ane_h16_prologue_test),
	KUNIT_CASE(ane_h17_prologue_test),
	KUNIT_CASE(ane_h16_dense_task_test),
	KUNIT_CASE(ane_h16_scatter_task_test),
	KUNIT_CASE(ane_h16_extra_header_test),
	KUNIT_CASE(ane_h16_relocation_test),
	{}
};

static struct kunit_suite ane_h16_task_suite = {
	.name = "ane-h16-tasks",
	.test_cases = ane_h16_task_cases,
};

kunit_test_suite(ane_h16_task_suite);
MODULE_DESCRIPTION("KUnit tests for H16 task and relocation validation");
MODULE_LICENSE("Dual MIT/GPL");
