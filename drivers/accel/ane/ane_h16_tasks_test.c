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
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	size = dma_task(text, 0x40, 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	size = dma_task(text, 0, 0x20000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	size = dma_task(text, 0, -0x70000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	size = dma_task(text, -1, 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* A larger buffer takes the same offset. */
	bound[7] = 0x40000;
	size = dma_task(text, 0, 0x20000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);

	/* A slot the operation section does not bind. */
	bound[7] = 0;
	size = dma_task(text, 0, 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	bound[7] = 0x20000;

	/* A base without relocation, or relocated on the custom BAR slot. */
	put_unaligned_le32(0x4d10 / 4 | 1 << 15, dma_record(text, 5));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(0x4d10 / 4 | 1 << 15 | BIT(29) | 4 << 23, dma_record(text, 5));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* A relocated record that is not exactly the [lo, hi] pair. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x4d0c / 4 | 1 << 15 | BIT(29) | 6 << 23, dma_record(text, 5));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* A scatter write of the hi word. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(BIT(31) | BIT(15) | 0x5108 / 4, dma_record(text, 6));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* A relocation on a stride, and an unused header bit. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x4d18 / 4 | 1 << 15 | BIT(29) | 6 << 23, dma_record(text, 1));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(0x4d18 / 4 | 1 << 15 | BIT(30), dma_record(text, 1));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);

	/* One byte more of plane stride crosses a line past the end. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x401, dma_record(text, 1) + 8);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* Rows, planes and depth slices whose sum wraps 64 bits to 32 bytes. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x80000001, dma_record(text, 0) + 8);
	put_unaligned_le32(0x80000001, dma_record(text, 0) + 12);
	put_unaligned_le32(3, dma_record(text, 0) + 16);
	put_unaligned_le32(U32_MAX, dma_record(text, 1) + 4);
	put_unaligned_le32(U32_MAX, dma_record(text, 1) + 8);
	put_unaligned_le32(0x80000000, dma_record(text, 1) + 12);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* A source 1 metadata address, which the bound does not model. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x4d40 / 4, dma_record(text, 2));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* A wrap configuration beside the cache hint. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x4d08 / 4, dma_record(text, 2));
	put_unaligned_le32(0x1ce, dma_record(text, 2) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(0xce, dma_record(text, 2) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
}

/* Record headers: @n dense words from @addr, and a relocated [lo, hi] pair on @slot. */
#define DMA_REC(addr, n) ((addr) / 4 | ((n) - 1) << 15)
#define DMA_RELOC(addr, slot) (DMA_REC(addr, 2) | BIT(29) | (slot) << 23)

/*
 * Parts of dma_task's one task, for streams of several tasks: its geometry
 * (dimensions with @rows source rows, strides with @plane source plane
 * stride, formats), its two bases (source 1 at @src, the destination at 0)
 * and the source 1 configuration word that enables it.
 */
enum { DMA_GEOMETRY = 1, DMA_BASES = 2, DMA_ENABLE = 4, DMA_EXTRA = 8 };

/* Append one task of @parts, then @n more words of @extra; returns the new end. */
static size_t dma_append(u8 *text, size_t at, unsigned int parts, u32 rows, u32 plane,
			 u64 src, const u32 *extra, unsigned int n)
{
	const u32 geometry[] = {
		DMA_REC(0x4, 7), 16, rows, 128, 0, 16, 16, 128,
		DMA_REC(0x4d18, 3), 0x40, plane, 0, DMA_REC(0x4d68, 1), 0x1002031,
		DMA_REC(0x5110, 2), 0x40, 0x400, DMA_REC(0x5138, 1), 0x1302031,
	};
	const u32 bases[] = {
		DMA_RELOC(0x4d10, 6), lower_32_bits(src), upper_32_bits(src),
		DMA_RELOC(0x5108, 7), 0, 0,
	};
	const u32 enable[] = { DMA_REC(0x4d00, 1) | BIT(29) | 4 << 23, 0x21 };
	const struct { unsigned int part; const u32 *w; unsigned int n; } part[] = {
		{ DMA_GEOMETRY, geometry, ARRAY_SIZE(geometry) },
		{ DMA_BASES, bases, ARRAY_SIZE(bases) },
		{ DMA_ENABLE, enable, ARRAY_SIZE(enable) },
		{ DMA_EXTRA, extra, n },
	};
	size_t task = at;
	unsigned int k, i;

	parts |= DMA_EXTRA;
	memset(text + at, 0, 9 * 4);
	at += 9 * 4;
	for (k = 0; k < ARRAY_SIZE(part); k++)
		for (i = 0; (parts & part[k].part) && i < part[k].n; i++, at += 4)
			put_unaligned_le32(part[k].w[i], text + at);
	put_unaligned_le32((at - task) / 4 << 16, text + task);
	return ALIGN(at, 16);
}

/* A stream of one complete task from dma_append() with @n words of @extra. */
static size_t dma_one(u8 *text, u32 plane, const u32 *extra, unsigned int n)
{
	memset(text, 0, 512);
	put_unaligned_le32(1, text);
	return dma_append(text, 16, 7, 16, plane, 0, extra, n);
}

/* A stream of two tasks, @first then @second, from dma_append(). */
static size_t dma_two(u8 *text, unsigned int first, unsigned int second, u32 plane)
{
	size_t at;

	memset(text, 0, 512);
	put_unaligned_le32(1, text);
	at = dma_append(text, 16, first, 16, 0x400, 0, NULL, 0);
	return dma_append(text, at, second, 16, plane, 0, NULL, 0);
}

/*
 * Each task runs from the reset values, so each is checked on its own words
 * over them, and every DMA a task enables has its base in that task. A word
 * a task leaves out holds its reset value, whatever an earlier task wrote.
 */
static void ane_hx_dma_reset_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [6] = 0x20000, [7] = 0x20000 };
	const u32 groups[] = { DMA_REC(0x24, 1), 2, DMA_REC(0x4d24, 1), 0x20000 };
	const u32 dst_only[] = {
		DMA_REC(0x4, 7), 16, 32, 128, 0, 16, 16, 128,
		DMA_REC(0x5110, 2), 0x40, 0x400, DMA_REC(0x5138, 1), 0x1302031,
		DMA_RELOC(0x5108, 7), 0, 0,
	};
	u8 text[512];
	size_t size;

	/* Both tasks complete and in range. */
	size = dma_two(text, 7, 7, 0x400);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	/*
	 * Task N: geometry with no base; task N+1: the bases and the enable
	 * alone, which enable source 1 with no size of its own.
	 */
	size = dma_two(text, DMA_GEOMETRY, DMA_BASES | DMA_ENABLE, 0x400);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* N+1 with larger planes and nothing enabled moves nothing: N's base does not reach it. */
	size = dma_two(text, 7, DMA_GEOMETRY, 0x800);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	/* ... but enabling source 1 there without its base is refused, */
	size = dma_two(text, 7, DMA_GEOMETRY | DMA_ENABLE, 0x800);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* ... and so is its base with those planes. */
	size = dma_two(text, 7, 7, 0x800);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);

	/*
	 * An L2-resident task: N+1 takes its input from L2 and writes only the
	 * destination, with 32 rows. Source 1 is not enabled in N+1, so N's
	 * source base is not read with N+1's rows (which would end past 128 KiB).
	 */
	memset(text, 0, 512);
	put_unaligned_le32(1, text);
	size = dma_append(text, 16, 7, 16, 0x400, 0, NULL, 0);
	size = dma_append(text, size, 0, 0, 0, 0, dst_only, ARRAY_SIZE(dst_only));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);

	/*
	 * A word left at its reset value after an earlier task set it: N reads
	 * two groups 128 KiB apart from a 256 KiB source; N+1 leaves the group
	 * count and stride out and reads one group from 128 KiB on.
	 */
	bound[6] = 0x40000;
	memset(text, 0, 512);
	put_unaligned_le32(1, text);
	size = dma_append(text, 16, 7, 16, 0x400, 0, groups, ARRAY_SIZE(groups));
	size = dma_append(text, size, 7, 16, 0x400, 0x20000, NULL, 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	/* N+1 writing the two groups itself reaches 128 KiB past the source. */
	memset(text, 0, 512);
	put_unaligned_le32(1, text);
	size = dma_append(text, 16, 7, 16, 0x400, 0, groups, ARRAY_SIZE(groups));
	size = dma_append(text, size, 7, 16, 0x400, 0x20000, groups, ARRAY_SIZE(groups));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
}

/*
 * A base written twice in one task must repeat its slot and offset: an
 * out-of-range first write followed by an in-range one is refused, as is
 * a second write on another slot. H17 programs repeat 0x4d28 unchanged.
 */
static void ane_hx_dma_rewrite_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [6] = 0x20000, [7] = 0x20000, [8] = 0x20000 };
	u32 again[] = { DMA_RELOC(0x4d10, 6), 0, 0 };
	u8 text[512];
	size_t size;

	memset(text, 0, 512);
	put_unaligned_le32(1, text);
	size = dma_append(text, 16, 7, 16, 0x400, 0, again, ARRAY_SIZE(again));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	memset(text, 0, 512);
	put_unaligned_le32(1, text);
	size = dma_append(text, 16, 7, 16, 0x400, 0x20000, again, ARRAY_SIZE(again));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	again[0] = DMA_RELOC(0x4d10, 8);
	memset(text, 0, 512);
	put_unaligned_le32(1, text);
	size = dma_append(text, 16, 7, 16, 0x400, 0, again, ARRAY_SIZE(again));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
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
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	/* ... with a zero channel count, */
	put_unaligned_le32(0, dma_record(text, 0) + 28);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(128, dma_record(text, 0) + 28);
	/* ... and without its base: record 6 rewrites the destination strides instead. */
	put_unaligned_le32(0x5110 / 4 | 1 << 15, dma_record(text, 6));
	put_unaligned_le32(0x40, dma_record(text, 6) + 4);
	put_unaligned_le32(0x400, dma_record(text, 6) + 8);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* A zero channel count on an enabled source. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0, dma_record(text, 0) + 12);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/*
	 * A zero width on an enabled stream: the J775D's H17 stalled on one and
	 * an M4 Pro ran one 256 wide, so it is not read as one.
	 */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0, dma_record(text, 0) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* The destination's width matters once the destination is enabled. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0, dma_record(text, 0) + 20);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	put_unaligned_le32(0x5100 / 4, dma_record(text, 7));
	put_unaligned_le32(0x1000021, dma_record(text, 7) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(16, dma_record(text, 0) + 20);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	/* An output channel group of 6, then 5: common word 0x40 in place of the source format. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x40 / 4, dma_record(text, 2));
	put_unaligned_le32(6, dma_record(text, 2) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(5, dma_record(text, 2) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	/* A source format bit the compiler never sets. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x1002033, dma_record(text, 2) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* 0x44d0, and a cache DMA word the compiler never writes, in place of the source format. */
	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x44d0 / 4, dma_record(text, 2));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(0x590c / 4, dma_record(text, 2));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(0x5904 / 4, dma_record(text, 2));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
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
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at, 9, bound), -ENOEXEC);
	bound[6] = 128;
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at, 9, bound), 0);
	/* Offset 0x41: the line at 0x40 through the one at 0x80. */
	put_unaligned_le32(0x41, text + 16 + 9 * 4 + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at, 9, bound), -ENOEXEC);
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
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at, 9, bound), 0);
	bound[8] = 0xfc0;
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at, 9, bound), -ENOEXEC);
	/* 3 at 0x40 is the default 128-channel group, 2 a 64-channel one: four groups. */
	bound[8] = 0x1000;
	put_unaligned_le32(0x40 / 4, text + at);
	put_unaligned_le32(2, text + at + 4);
	put_unaligned_le32((at + 8 - 16) / 4 << 16, text + 16);
	KUNIT_ASSERT_EQ(test, ane_h16_task_stream_validate(text, at + 8, 1), 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at + 8, 9, bound), -ENOEXEC);
	bound[8] = 0x2000;
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, at + 8, 9, bound), 0);
}

/*
 * A matmul's coefficient DMA from slot 3: 16 lanes of 0x1000 bytes 0x1000
 * apart, 256 output channels in two groups of 128 (common word 0x40 unset)
 * @ocgs apart, the lane configuration words set to @lane (none when 0) and
 * the base written when @base.
 */
static size_t dma_kernel(u8 *text, u32 lane, bool base, u32 ocgs)
{
	size_t at = 16 + 9 * 4;
	unsigned int i;

	memset(text, 0, 512);
	put_unaligned_le32(1, text);
#define PUT(v) do { put_unaligned_le32(v, text + at); at += 4; } while (0)
	PUT(DMA_REC(0x1c, 1));
	PUT(256);
	PUT(DMA_REC(0x3c, 1));
	PUT(0x20200004);
	PUT(DMA_REC(0x551c, 1));
	PUT(ocgs);
	PUT(DMA_REC(0x5560, 32));
	for (i = 0; i < 16; i++)
		PUT(i * 0x1000);
	for (i = 0; i < 16; i++)
		PUT(0x1000);
	if (lane) {
		/* Enabled lanes take the custom BAR value; idle ones are not relocated. */
		PUT(DMA_REC(0x5520, 16) | ((lane & 0xffff) == 0x20 ? 0 : BIT(29) | 4 << 23));
		for (i = 0; i < 16; i++)
			PUT(lane);
	}
	if (base) {
		PUT(DMA_RELOC(0x5510, 3));
		PUT(0);
		PUT(0);
	}
#undef PUT
	put_unaligned_le32((at - 16) / 4 << 16, text + 16);
	return at;
}

/*
 * The coefficient DMA is held to the same rule as the others: lanes that
 * enable it need its base in the task. A lookup-table task writes its lanes
 * idle (0x20 in the low half) and fetches nothing. The second
 * output channel group of 128 x 256 fp16 coefficients starts 0x10000 into
 * them; a group stride of 0x40000 reads 0x30000 past a 0x20000 buffer.
 */
static void ane_hx_dma_kernel_enable_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [3] = 0x20000 };
	u8 text[512];
	size_t size;

	size = dma_kernel(text, 0x10041, true, 0x10000);
	KUNIT_ASSERT_EQ(test, ane_h16_task_stream_validate(text, size, 1), 0);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	size = dma_kernel(text, 0x10041, true, 0x40000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* Enabled lanes without a base. */
	size = dma_kernel(text, 0x10041, false, 0x10000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* Idle lanes without a base fetch nothing; any other lane word enables the DMA. */
	size = dma_kernel(text, 0x50020, false, 0x40000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	size = dma_kernel(text, 0x50022, false, 0x40000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	size = dma_kernel(text, 0x50120, false, 0x40000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* A base written beside idle lanes is still bounded. */
	size = dma_kernel(text, 0x50020, true, 0x40000);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	/* Idle lanes that take the custom BAR value could become enabled lanes. */
	size = dma_kernel(text, 0x50020, false, 0x10000);
	put_unaligned_le32(get_unaligned_le32(text + size - 17 * 4) | BIT(29) | 4 << 23,
			   text + size - 17 * 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
}

/*
 * Counts the bound cannot read as one, and a lane that reads nothing. In
 * dma_kernel()'s task, 52, 60 and 68 hold the records of the output channel
 * count, common word 0x3c and the output channel group stride; 80 + 4i
 * holds lane i's offset and 144 + 4i its size.
 */
static void ane_hx_dma_kernel_count_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [3] = 0x20000 };
	u8 text[512];
	size_t size;

	/* No group count with a group stride, then two groups 0x1000 apart. */
	size = dma_kernel(text, 0x10041, true, 0x10000);
	put_unaligned_le32(DMA_REC(0x5518, 1), text + 60);
	put_unaligned_le32(0x1000, text + 64);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(DMA_REC(0x24, 1), text + 68);
	put_unaligned_le32(2, text + 72);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	/* No output channel count with an output channel group stride. */
	size = dma_kernel(text, 0x10041, true, 0x10000);
	put_unaligned_le32(0, text + 56);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	size = dma_kernel(text, 0x10041, true, 0);
	put_unaligned_le32(0, text + 56);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	/* A lane of size 0 at the end of the buffer, then one 0x5504 sizes. */
	size = dma_kernel(text, 0x10041, true, 0);
	put_unaligned_le32(0x20000, text + 80 + 15 * 4);
	put_unaligned_le32(0, text + 144 + 15 * 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(DMA_REC(0x5504, 1), text + 60);
	put_unaligned_le32(0x80, text + 64);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(0x1f000, text + 80 + 15 * 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
}

/*
 * A zero depth or group count on a source or the destination is one only
 * with a zero stride. dma_task()'s source has depth 0 and writes its depth
 * stride at record 1 + 12.
 */
static void ane_hx_dma_count_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [6] = 0x20000, [7] = 0x20000 };
	static const u32 dst_depth[] = { DMA_REC(0x5118, 1), 0x400 };
	static const u32 dst_depths[] = { DMA_REC(0x5118, 1), 0x400, DMA_REC(0x20, 1), 1 };
	static const u32 src_group[] = { DMA_REC(0x4d24, 1), 0x20000 };
	static const u32 dst_group[] = { DMA_REC(0x511c, 1), 0x20000 };
	static const u32 src_groups[] = { DMA_REC(0x24, 1), 1, DMA_REC(0x4d24, 1), 0x20000 };
	u8 text[512];
	size_t size;

	size = dma_task(text, 0, 0);
	put_unaligned_le32(0x400, dma_record(text, 1) + 12);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	put_unaligned_le32(1, dma_record(text, 0) + 16);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	put_unaligned_le32(2, dma_record(text, 0) + 16);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);

	size = dma_one(text, 0x400, dst_depth, ARRAY_SIZE(dst_depth));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	size = dma_one(text, 0x400, dst_depths, ARRAY_SIZE(dst_depths));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	size = dma_one(text, 0x400, src_group, ARRAY_SIZE(src_group));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	size = dma_one(text, 0x400, dst_group, ARRAY_SIZE(dst_group));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
	size = dma_one(text, 0x400, src_groups, ARRAY_SIZE(src_groups));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
}

/*
 * A word written twice in one task repeats its value: a plane stride that
 * would end past the source, then one that would not, is refused. A
 * configuration word on the custom BAR must already enable its DMA.
 */
static void ane_hx_dma_repeat_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [6] = 0x20000, [7] = 0x20000 };
	static const u32 plane[] = { DMA_REC(0x4d1c, 1), 0x400 };
	u8 text[512];
	size_t size;

	size = dma_one(text, 0x400, plane, ARRAY_SIZE(plane));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), 0);
	size = dma_one(text, 0x800, plane, ARRAY_SIZE(plane));
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);

	size = dma_task(text, 0, 0);
	put_unaligned_le32(0, dma_record(text, 7) + 4);
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 9, bound), -ENOEXEC);
}

/*
 * One H15 task: eight header words (nine when @extra sets the last one's
 * low bits), then a source 1 row stride of @row, three L2 records and
 * dma_task()'s geometry, bases and enable. Read with nine header words, the
 * row stride record would be a header word and the L2 records would hide
 * it, so the stride goes unchecked.
 */
static size_t dma_h15(u8 *text, u32 row, bool extra)
{
	static const u32 tail[] = {
		DMA_REC(0x4104, 1), DMA_REC(0x4108, 2), DMA_REC(0x410c, 1), 0,
		DMA_REC(0x4, 7), 16, 16, 128, 0, 16, 16, 128,
		DMA_REC(0x4d1c, 2), 0x400, 0, DMA_REC(0x4d68, 1), 0x1002031,
		DMA_REC(0x5110, 2), 0x40, 0x400, DMA_REC(0x5138, 1), 0x1302031,
		DMA_RELOC(0x4d10, 6), 0, 0, DMA_RELOC(0x5108, 7), 0, 0,
		DMA_REC(0x4d00, 1) | BIT(29) | 4 << 23, 0x21,
	};
	size_t at = 16 + 8 * 4;
	unsigned int i;

	memset(text, 0, 256);
	put_unaligned_le32(1, text);
	if (extra) {
		put_unaligned_le32(3, text + at - 4);
		at += 4;
	}
	put_unaligned_le32(DMA_REC(0x4d18, 1), text + at);
	put_unaligned_le32(row, text + at + 4);
	at += 8;
	for (i = 0; i < ARRAY_SIZE(tail); i++, at += 4)
		put_unaligned_le32(tail[i], text + at);
	put_unaligned_le32((at - 16) / 4 << 16, text + 16);
	return at;
}

/* The DMA bound walks a stream with its target's header words, as the structural gate does. */
static void ane_hx_dma_h15_test(struct kunit *test)
{
	u64 bound[ANE_HX_DMA_SLOTS] = { [6] = 0x20000, [7] = 0x20000 };
	u8 text[256];
	size_t size;
	bool extra;

	for (extra = false; ; extra = true) {
		size = dma_h15(text, 0x40, extra);
		KUNIT_ASSERT_EQ(test, ane_hx_task_stream_validate_hdr(text, size, 1, 1, 8,
								      NULL, NULL), 0);
		KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 8, bound), 0);
		/* 15 rows of 0x1040 end past the source. */
		size = dma_h15(text, 0x1040, extra);
		KUNIT_ASSERT_EQ(test, ane_hx_task_stream_validate_hdr(text, size, 1, 1, 8,
								      NULL, NULL), 0);
		KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 8, bound),
				-ENOEXEC);
		if (extra)
			break;
	}
	KUNIT_EXPECT_EQ(test, ane_hx_task_stream_dma_validate(text, size, 0, bound), -ENOEXEC);
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
	KUNIT_CASE(ane_hx_dma_kernel_enable_test),
	KUNIT_CASE(ane_hx_dma_kernel_count_test),
	KUNIT_CASE(ane_hx_dma_count_test),
	KUNIT_CASE(ane_hx_dma_repeat_test),
	KUNIT_CASE(ane_hx_dma_h15_test),
	KUNIT_CASE(ane_hx_dma_reset_test),
	KUNIT_CASE(ane_hx_dma_rewrite_test),
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
