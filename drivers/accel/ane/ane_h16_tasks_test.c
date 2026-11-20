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

/* An H17 stream opens with 0 or 1 (the caller names which), and the last task may end the section unpadded. */
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
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_validate(text, sizeof(text), 1, 1), 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_validate(text, sizeof(text), 1, 0), -ENOEXEC);
	/* A task that overruns the section is still refused. */
	put_unaligned_le32(12 << 16, text + 16);
	KUNIT_EXPECT_EQ(test, ane_h16_task_stream_validate(text, sizeof(text), 1), -ENOEXEC);
}

/*
 * One 16x16x128 fp16 add task as mil-hwx-compiler writes it: source 1 on BAR
 * slot 6 and the destination on slot 7, each a relocated [lo, hi] record,
 * rows of 0x40 and planes of 0x400 bytes, a source depth stride of 0.
 * Returns the stream size.
 */
static size_t dma_task(u8 *text, u64 src, u64 dst)
{
	static const u32 dims[] = { 16, 16, 128, 0, 16, 16, 128 };
	size_t at = 16 + 9 * 4;
	unsigned int i;

#define PUT(v) do { put_unaligned_le32(v, text + at); at += 4; } while (0)
	memset(text, 0, 256);
	put_unaligned_le32(1, text);
	PUT(0x4 / 4 | 6 << 15);
	for (i = 0; i < ARRAY_SIZE(dims); i++)
		PUT(dims[i]);
	PUT(0x4d18 / 4 | 2 << 15);
	PUT(0x40);
	PUT(0x400);
	PUT(0);
	PUT(0x4d68 / 4);
	PUT(0x1002031);
	PUT(0x5110 / 4 | 1 << 15);
	PUT(0x40);
	PUT(0x400);
	PUT(0x5138 / 4);
	PUT(0x1302031);
	PUT(0x4d10 / 4 | 1 << 15 | BIT(29) | 6 << 23);
	PUT(lower_32_bits(src));
	PUT(upper_32_bits(src));
	PUT(0x5108 / 4 | 1 << 15 | BIT(29) | 7 << 23);
	PUT(lower_32_bits(dst));
	PUT(upper_32_bits(dst));
	/* The cache-allocation word takes the slot 4 custom BAR value. */
	PUT(0x4d00 / 4 | BIT(29) | 4 << 23);
	PUT(0x21);
#undef PUT
	put_unaligned_le32((at - 16) / 4 << 16, text + 16);
	return at;
}

/* Record @n (0-based) of dma_task's one task. */
static u8 *dma_record(u8 *text, unsigned int n)
{
	static const u8 words[] = { 8, 4, 2, 3, 2, 3, 3, 2 };
	size_t at = 16 + 9 * 4;

	while (n--)
		at += words[n] * 4;
	return text + at;
}

static void ane_hx_dma_bound_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [6] = 0x20000, [7] = 0x20000 };
	u8 text[256];
	size_t size;

	/* 15 rows of 0x40, 127 planes of 0x400 and one 32-byte row: exactly 128 KiB. */
	size = dma_task(text, 0, 0);
	KUNIT_ASSERT_EQ(test, ane_h16_task_stream_validate(text, size, 1), 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), 0);
	size = dma_task(text, 0x40, 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	size = dma_task(text, 0, 0x20000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	size = dma_task(text, 0, -0x70000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	size = dma_task(text, -1, 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* A larger buffer takes the same offset. */
	bound[7] = 0x40000;
	size = dma_task(text, 0, 0x20000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), 0);

	/* A slot the operation section does not bind. */
	bound[7] = 0;
	size = dma_task(text, 0, 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	bound[7] = 0x20000;

	/* A base without relocation, or relocated on the custom BAR slot. */
	put_unaligned_le32(0x4d10 / 4 | 1 << 15, dma_record(text, 5));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	put_unaligned_le32(0x4d10 / 4 | 1 << 15 | BIT(29) | 4 << 23, dma_record(text, 5));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* A relocated record that is not exactly the [lo, hi] pair. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x4d0c / 4 | 1 << 15 | BIT(29) | 6 << 23, dma_record(text, 5));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* A scatter write of the hi word. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(BIT(31) | BIT(15) | 0x5108 / 4, dma_record(text, 6));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* A relocation on a stride, and an unused header bit. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x4d18 / 4 | 1 << 15 | BIT(29) | 6 << 23, dma_record(text, 1));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	put_unaligned_le32(0x4d18 / 4 | 1 << 15 | BIT(30), dma_record(text, 1));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);

	/* One byte more of plane stride crosses a line past the end. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x401, dma_record(text, 1) + 8);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* Rows, planes and depth slices whose sum wraps 64 bits to 32 bytes. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x80000001, dma_record(text, 0) + 8);
	put_unaligned_le32(0x80000001, dma_record(text, 0) + 12);
	put_unaligned_le32(3, dma_record(text, 0) + 16);
	put_unaligned_le32(U32_MAX, dma_record(text, 1) + 4);
	put_unaligned_le32(U32_MAX, dma_record(text, 1) + 8);
	put_unaligned_le32(0x80000000, dma_record(text, 1) + 12);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* A source 1 metadata address, which the bound does not model. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x4d40 / 4, dma_record(text, 2));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* A wrap configuration beside the cache hint. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x4d08 / 4, dma_record(text, 2));
	put_unaligned_le32(0x1ce, dma_record(text, 2) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	put_unaligned_le32(0xce, dma_record(text, 2) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), 0);
}

/*
 * Two tasks from dma_task's records: the geometry records (dimensions, strides
 * and formats, records 0-4) and the base records (records 5-7), each task
 * given @first then @second of them (bit 0 geometry, bit 1 bases), with the
 * source plane stride of the second task's geometry set to @plane.
 */
static size_t dma_two(u8 *text, unsigned int first, unsigned int second, u32 plane)
{
	static const unsigned int parts[2][2] = {
		{ 0, 28 * 4 - 9 * 4 }, { 28 * 4 - 9 * 4, 8 * 4 },
	};
	u8 full[256];
	size_t at = 16, task, rec = 16 + 9 * 4;
	unsigned int t, k;

	dma_task(full, 0, 0);
	memset(text, 0, 512);
	put_unaligned_le32(2, text);
	for (t = 0; t < 2; t++) {
		unsigned int want = t ? second : first;

		task = at;
		memcpy(text + at, full + 16, 9 * 4);
		at += 9 * 4;
		for (k = 0; k < 2; k++) {
			if (!(want & BIT(k)))
				continue;
			memcpy(text + at, full + rec + parts[k][0], parts[k][1]);
			if (k == 0 && t == 1)
				/* record 1 (source strides) starts 8 words in: row, plane, depth */
				put_unaligned_le32(plane, text + at + 8 * 4 + 8);
			at += parts[k][1];
		}
		put_unaligned_le32((at - task) / 4 << 16, text + task);
		if (!t)
			at = ALIGN(at, 16);
	}
	return at;
}

/*
 * Whether the engine keeps registers from one task to the next is not known,
 * so a stream must fit both ways. Geometry in one task and bases in the next
 * fits only per task when the geometry is too large; a base kept from an
 * earlier task fits only per task when a later task enlarges its geometry.
 */
static void ane_hx_dma_carry_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [6] = 0x20000, [7] = 0x20000 };
	u8 text[512];
	size_t size;

	/* Both tasks complete and in range. */
	size = dma_two(text, 3, 3, 0x400);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), 0);
	/*
	 * Task N: geometry (and large planes) with no base; task N+1: the bases
	 * alone, which enable the source with no size of its own.
	 */
	size = dma_two(text, 3, 2, 0x400);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	size = dma_two(text, 1, 2, 0x400);
	put_unaligned_le32(0x800, text + 16 + 9 * 4 + 8 * 4 + 8);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* The reverse: the bases kept from task N, larger planes in task N+1. */
	size = dma_two(text, 3, 1, 0x800);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	size = dma_two(text, 3, 1, 0x400);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), 0);
}

/*
 * A task that enables a DMA must locate it and give it a real size; the
 * channel group, the format bits and the registers outside the modelled
 * blocks stay within what the compiler writes.
 */
static void ane_hx_dma_strict_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [6] = 0x20000, [7] = 0x20000 };
	u8 text[256];
	size_t size;

	/* The destination enabled: the cache word (record 7) becomes its configuration 0x5100. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x5100 / 4, dma_record(text, 7));
	put_unaligned_le32(0x1000021, dma_record(text, 7) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), 0);
	/* ... with a zero channel count, */
	put_unaligned_le32(0, dma_record(text, 0) + 28);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	put_unaligned_le32(128, dma_record(text, 0) + 28);
	/* ... and without its base: record 6 rewrites the destination strides instead. */
	put_unaligned_le32(0x5110 / 4 | 1 << 15, dma_record(text, 6));
	put_unaligned_le32(0x40, dma_record(text, 6) + 4);
	put_unaligned_le32(0x400, dma_record(text, 6) + 8);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* A zero channel count on an enabled source. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0, dma_record(text, 0) + 12);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* A zero width reads as one. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0, dma_record(text, 0) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), 0);
	/* An output channel group of 6, then 5: common word 0x40 in place of the source format. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x40 / 4, dma_record(text, 2));
	put_unaligned_le32(6, dma_record(text, 2) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	put_unaligned_le32(5, dma_record(text, 2) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), 0);
	/* A source format bit the compiler never sets. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x1002033, dma_record(text, 2) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	/* 0x44d0, and a cache DMA word the compiler never writes, in place of the source format. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x44d0 / 4, dma_record(text, 2));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	put_unaligned_le32(0x590c / 4, dma_record(text, 2));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), -ENOEXEC);
	put_unaligned_le32(0x5904 / 4, dma_record(text, 2));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, bound), 0);
}

/*
 * The end of a transfer is the end of the last 64-byte line it touches: a
 * 64-byte lookup table at offset 1 reads [0, 128).
 */
static void ane_hx_dma_align_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [6] = 100 };
	u8 text[64] = {};
	size_t at = 16 + 9 * 4;

	put_unaligned_le32(1, text);
	put_unaligned_le32(0x5618 / 4 | 1 << 15 | BIT(29) | 6 << 23, text + at);
	put_unaligned_le32(1, text + at + 4);
	put_unaligned_le32(0, text + at + 8);
	at += 12;
	put_unaligned_le32((at - 16) / 4 << 16, text + 16);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at, bound), -ENOEXEC);
	bound[6] = 128;
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at, bound), 0);
	/* Offset 0x41: the line at 0x40 through the one at 0x80. */
	put_unaligned_le32(0x41, text + 16 + 9 * 4 + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at, bound), -ENOEXEC);
}

/*
 * Coefficient DMA: 16 lanes of 0x80 bytes at 0x80 apart from a base on slot
 * 8, then a second output channel group 0x800 on (256 channels, 128 per
 * group).
 */
static void ane_hx_dma_kernel_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [8] = 0x1000 };
	u8 text[512] = {};
	size_t at = 16 + 9 * 4;
	unsigned int i;

#define PUT(v) do { put_unaligned_le32(v, text + at); at += 4; } while (0)
	put_unaligned_le32(1, text);
	PUT(0x1c / 4);
	PUT(256);
	PUT(0x551c / 4);
	PUT(0x800);
	PUT(0x5560 / 4 | 31 << 15);
	for (i = 0; i < 16; i++)
		PUT(i * 0x80);
	for (i = 0; i < 16; i++)
		PUT(0x80);
	PUT(0x5510 / 4 | 1 << 15 | BIT(29) | 8 << 23);
	PUT(0);
	PUT(0);
#undef PUT
	put_unaligned_le32((at - 16) / 4 << 16, text + 16);
	KUNIT_ASSERT_EQ(test, ane_h16_task_stream_validate(text, at, 1), 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at, bound), 0);
	bound[8] = 0xfc0;
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at, bound), -ENOEXEC);
	/* 3 at 0x40 is the default 128-channel group, 2 a 64-channel one: four groups. */
	bound[8] = 0x1000;
	put_unaligned_le32(0x40 / 4, text + at);
	put_unaligned_le32(2, text + at + 4);
	put_unaligned_le32((at + 8 - 16) / 4 << 16, text + 16);
	KUNIT_ASSERT_EQ(test, ane_h16_task_stream_validate(text, at + 8, 1), 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at + 8, bound), -ENOEXEC);
	bound[8] = 0x2000;
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at + 8, bound), 0);
}

static struct kunit_case ane_h16_task_cases[] = {
	KUNIT_CASE(ane_h16_prologue_test),
	KUNIT_CASE(ane_h17_prologue_test),
	KUNIT_CASE(ane_h16_dense_task_test),
	KUNIT_CASE(ane_h16_scatter_task_test),
	KUNIT_CASE(ane_h16_extra_header_test),
	KUNIT_CASE(ane_h16_relocation_test),
	KUNIT_CASE(ane_hx_dma_bound_test),
	KUNIT_CASE(ane_hx_dma_kernel_test),
	KUNIT_CASE(ane_hx_dma_carry_test),
	KUNIT_CASE(ane_hx_dma_strict_test),
	KUNIT_CASE(ane_hx_dma_align_test),
	{}
};

static struct kunit_suite ane_h16_task_suite = {
	.name = "ane-h16-tasks",
	.test_cases = ane_h16_task_cases,
};

kunit_test_suite(ane_h16_task_suite);
MODULE_DESCRIPTION("KUnit tests for H16 task, relocation and DMA base validation");
MODULE_LICENSE("Dual MIT/GPL");
