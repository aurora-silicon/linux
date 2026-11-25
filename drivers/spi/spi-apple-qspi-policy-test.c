// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the Apple QSPIMC NVRAM mutation policy, at the J700 and
 * J613/J713/J616s nvram partitions.
 */

#include <kunit/test.h>
#include <linux/string.h>

#include "spi-apple-qspi-policy.h"

/* The J700 nvram partition. */
#define NV_START	0x24000ULL
#define NV_END		0x1f4000ULL

static int nv(unsigned int opcode, unsigned int addr_bytes,
	      unsigned long long addr, unsigned int len, int data_out)
{
	return apple_qspi_mutation_allowed(opcode, addr_bytes, addr, len,
					   data_out, NV_START, NV_END);
}

static void qspi_policy_write_enable(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, nv(0x06, 0, 0, 0, 0));
	KUNIT_EXPECT_TRUE(test, nv(0x04, 0, 0, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv(0x06, 3, NV_START, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv(0x06, 0, 0, 1, 1));
	/* Write-enable control is virtual and needs no window. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_mutation_allowed(0x06, 0, 0, 0, 0, 0, 0));
}

static void qspi_policy_program(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, nv(0x02, 3, NV_START, 256, 1));
	KUNIT_EXPECT_TRUE(test, nv(0x02, 3, NV_START + 0x1234, 1, 1));
	KUNIT_EXPECT_TRUE(test, nv(0x02, 3, NV_END - 256, 256, 1));
	/* Any byte outside the partition is refused. */
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, NV_START - 256, 256, 1));
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, NV_START - 1, 2, 1));
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, NV_END, 1, 1));
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, NV_END - 1, 2, 1));
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, 0, 256, 1));
	/* A program stays within one page and needs data out. */
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, NV_START + 0xff, 2, 1));
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, NV_START, 257, 1));
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, NV_START, 0, 1));
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, NV_START, 256, 0));
	/* Four-byte programs are not admitted. */
	KUNIT_EXPECT_FALSE(test, nv(0x12, 4, NV_START, 256, 1));
}

static void qspi_policy_erase(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, nv(0x20, 3, NV_START, 0, 0));
	KUNIT_EXPECT_TRUE(test, nv(0x20, 3, NV_END - 0x1000, 0, 0));
	/* Only aligned 4 KiB sector erases inside the partition. */
	KUNIT_EXPECT_FALSE(test, nv(0x20, 3, NV_START + 0x123, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv(0x20, 3, NV_START - 0x1000, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv(0x20, 3, NV_END, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv(0x21, 4, NV_START, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv(0x52, 3, 0x28000, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv(0xd8, 3, 0x30000, 0, 0));
	/* An erase carries no data. */
	KUNIT_EXPECT_FALSE(test, nv(0x20, 3, NV_START, 1, 0));
	KUNIT_EXPECT_FALSE(test, nv(0x20, 3, NV_START, 0, 1));
}

static void qspi_policy_refused_commands(struct kunit *test)
{
	static const u8 refused[] = {
		0x01, 0x31, 0x11,	/* status register writes */
		0x60, 0xc7,		/* chip erase */
		0xb7, 0xe9, 0xc5,	/* address mode, extended address */
		0x66, 0x99, 0xf0,	/* reset */
		0x42, 0x44, 0x98,	/* OTP program and erase, global unlock */
		0x32, 0x34, 0x38,	/* multi-line page programs */
		0x7a, 0x75, 0xb9, 0xab,	/* suspend, resume, power down */
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(refused); i++) {
		KUNIT_EXPECT_FALSE(test, nv(refused[i], 0, 0, 0, 0));
		KUNIT_EXPECT_FALSE(test, nv(refused[i], 3, NV_START, 0, 0));
		KUNIT_EXPECT_FALSE(test, nv(refused[i], 3, NV_START, 1, 1));
	}
}

static void qspi_policy_address_width(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, nv(0x02, 2, NV_START, 1, 1));
	KUNIT_EXPECT_FALSE(test, nv(0x02, 4, NV_START, 1, 1));
	KUNIT_EXPECT_FALSE(test, nv(0x20, 0, NV_START, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv(0x20, 4, NV_START, 0, 0));
	/* Addresses are not truncated to the wire width before the check. */
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, (1ULL << 24) + NV_START, 1, 1));
	KUNIT_EXPECT_FALSE(test, nv(0x20, 3, (1ULL << 24) + NV_START, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv(0x02, 3, ~0ULL, 1, 1));
}

static void qspi_policy_empty(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, apple_qspi_mutation_allowed(0x02, 3, 0, 1, 1, 0, 0));
	KUNIT_EXPECT_FALSE(test, apple_qspi_mutation_allowed(0x20, 3, 0, 0, 0, 0, 0));
	KUNIT_EXPECT_FALSE(test, apple_qspi_mutation_allowed(0x20, 3, NV_START, 0, 0,
							     NV_START, NV_START));
}

/* The J613, J713 and J616s nvram partition: 0x700000, 1 MiB. */
#define NV7_START	0x700000ULL
#define NV7_END		0x800000ULL

static int nv7(unsigned int opcode, unsigned int addr_bytes,
	       unsigned long long addr, unsigned int len, int data_out)
{
	return apple_qspi_mutation_allowed(opcode, addr_bytes, addr, len,
					   data_out, NV7_START, NV7_END);
}

static void qspi_policy_nvram_700000(struct kunit *test)
{
	/* First and last page and sector of the partition. */
	KUNIT_EXPECT_TRUE(test, nv7(0x02, 3, NV7_START, 256, 1));
	KUNIT_EXPECT_TRUE(test, nv7(0x02, 3, NV7_END - 256, 256, 1));
	KUNIT_EXPECT_TRUE(test, nv7(0x20, 3, NV7_START, 0, 0));
	KUNIT_EXPECT_TRUE(test, nv7(0x20, 3, NV7_END - 0x1000, 0, 0));
	/* syscfg ends at 0x700000 and the NOR is 8 MiB: both stay out. */
	KUNIT_EXPECT_FALSE(test, nv7(0x02, 3, NV7_START - 1, 1, 1));
	KUNIT_EXPECT_FALSE(test, nv7(0x02, 3, NV7_START - 256, 256, 1));
	KUNIT_EXPECT_FALSE(test, nv7(0x20, 3, NV7_START - 0x1000, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv7(0x02, 3, NV7_END, 1, 1));
	KUNIT_EXPECT_FALSE(test, nv7(0x02, 3, NV7_END - 1, 2, 1));
	KUNIT_EXPECT_FALSE(test, nv7(0x20, 3, NV7_END, 0, 0));
	/* iBoot at 0 and the J700 window are outside this partition. */
	KUNIT_EXPECT_FALSE(test, nv7(0x02, 3, 0, 256, 1));
	KUNIT_EXPECT_FALSE(test, nv7(0x20, 3, NV_START, 0, 0));
	/* Larger erases and four-byte forms are still refused here. */
	KUNIT_EXPECT_FALSE(test, nv7(0x52, 3, NV7_START, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv7(0xd8, 3, NV7_START, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv7(0x21, 4, NV7_START, 0, 0));
	KUNIT_EXPECT_FALSE(test, nv7(0x12, 4, NV7_START, 256, 1));
	/* An address above 16 MiB is not aliased onto the partition. */
	KUNIT_EXPECT_FALSE(test, nv7(0x02, 3, (1ULL << 24) + NV7_START, 1, 1));
	KUNIT_EXPECT_FALSE(test, nv7(0x20, 3, (1ULL << 24) + NV7_START, 0, 0));
}

static void qspi_policy_status(struct kunit *test)
{
	/* BUSY and WEL are never admitted. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_status_allows_write(0x01, 0, 0, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_status_allows_write(0x02, 0, 0, 0x1c));
	/* Block protect only where the board allows it. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_status_allows_write(0x04, 0, 0, 0));
	KUNIT_EXPECT_TRUE(test, apple_qspi_status_allows_write(0x04, 0x02, 0, 0x1c));
	/* The J813 W25Q64: QE and the SR3 drive strength, no block protection. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_status_allows_write(0x00, 0x02, 0x60, 0));
	/* CMP, SUS and WPS are refused everywhere. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_status_allows_write(0, 0x40, 0, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_status_allows_write(0, 0x80, 0, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_status_allows_write(0, 0, 0x04, 0x1c));
}

static void qspi_policy_mx_status(struct kunit *test)
{
	/* The J616s Macronix part: no block protection is tolerated. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_mx_status_allows_write(0x00, 0));
	/* QE and SRWD stay as found and do not block a write. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_mx_status_allows_write(0x40, 0));
	KUNIT_EXPECT_TRUE(test, apple_qspi_mx_status_allows_write(0x80, 0));
	/* WIP and WEL are never admitted. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_mx_status_allows_write(0x01, 0x3c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_mx_status_allows_write(0x02, 0x3c));
	/* Each of BP0 to BP3, including BP3 at bit 5, which Winbond calls TB. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_mx_status_allows_write(0x04, 0));
	KUNIT_EXPECT_FALSE(test, apple_qspi_mx_status_allows_write(0x08, 0));
	KUNIT_EXPECT_FALSE(test, apple_qspi_mx_status_allows_write(0x10, 0));
	KUNIT_EXPECT_FALSE(test, apple_qspi_mx_status_allows_write(0x20, 0));
	KUNIT_EXPECT_TRUE(test, apple_qspi_mx_status_allows_write(0x3c, 0x3c));
}

static void qspi_policy_gd_status(struct kunit *test)
{
	/* The J700 GD25UF16E part: QE is set for good and BP0 by iBoot. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_gd_status_allows_write(0x00, 0x02, 0x1c));
	KUNIT_EXPECT_TRUE(test, apple_qspi_gd_status_allows_write(0x04, 0x02, 0x1c));
	/* The J775D GD25UF64E part: QE only, no block protection tolerated. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_gd_status_allows_write(0x00, 0x02, 0));
	KUNIT_EXPECT_FALSE(test, apple_qspi_gd_status_allows_write(0x04, 0x02, 0));
	/* SRP0, SRP1 and LB1 to LB3 stay as found and do not block a write. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_gd_status_allows_write(0x80, 0x3b, 0x1c));
	/* WIP and WEL are never admitted. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_gd_status_allows_write(0x01, 0, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_gd_status_allows_write(0x02, 0, 0x1c));
	/* BP3 and BP4, which Winbond calls TB and SEC. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_gd_status_allows_write(0x20, 0, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_gd_status_allows_write(0x40, 0, 0x1c));
	/* SUS1, CMP and SUS2, which is reserved on Winbond parts. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_gd_status_allows_write(0, 0x80, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_gd_status_allows_write(0, 0x40, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_gd_status_allows_write(0, 0x04, 0x1c));
}

static void qspi_policy_part_status(struct kunit *test)
{
	/* The checks follow the manufacturer the flash reports. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_part_status_allows_write(0xef, 0x20, 0, 0, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_part_status_allows_write(0xc8, 0x20, 0, 0, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_part_status_allows_write(0xef, 0, 0, 0x04, 0x1c));
	/* GigaDevice SR3 bit 2 is LPE, a power setting; J775D leaves SR3 0x20. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_part_status_allows_write(0xc8, 0, 0, 0x04, 0x1c));
	KUNIT_EXPECT_TRUE(test, apple_qspi_part_status_allows_write(0xc8, 0x00, 0x02, 0x20, 0));
	KUNIT_EXPECT_FALSE(test, apple_qspi_part_status_allows_write(0xc8, 0x04, 0x02, 0x20, 0));
	KUNIT_EXPECT_TRUE(test, apple_qspi_part_status_allows_write(0xef, 0, 0x04, 0, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_part_status_allows_write(0xc8, 0, 0x04, 0, 0x1c));
	KUNIT_EXPECT_FALSE(test, apple_qspi_part_status_allows_write(0xc2, 0x20, 0, 0, 0x1c));
}

static void qspi_policy_window(struct kunit *test)
{
	/* The J813 and J700 partitions. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_window_valid(1, 2, 0, 0x700000, 0x100000));
	KUNIT_EXPECT_TRUE(test, apple_qspi_window_valid(1, 2, 0, 0x24000, 0x1d0000));
	/* Two nvram labels, or none (a disabled one is not counted). */
	KUNIT_EXPECT_FALSE(test, apple_qspi_window_valid(2, 2, 0, 0x700000, 0x100000));
	KUNIT_EXPECT_FALSE(test, apple_qspi_window_valid(0, 2, 0, 0x700000, 0x100000));
	/* Marked read-only. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_window_valid(1, 2, 1, 0x700000, 0x100000));
	/* Three reg cells, or none. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_window_valid(1, 3, 0, 0x700000, 0x100000));
	KUNIT_EXPECT_FALSE(test, apple_qspi_window_valid(1, 0, 0, 0x700000, 0x100000));
	/* A misaligned start, a part of a bank, an empty partition. */
	KUNIT_EXPECT_FALSE(test, apple_qspi_window_valid(1, 2, 0, 0x700800, 0x100000));
	KUNIT_EXPECT_FALSE(test, apple_qspi_window_valid(1, 2, 0, 0x700000, 0xff000));
	KUNIT_EXPECT_FALSE(test, apple_qspi_window_valid(1, 2, 0, 0x700000, 0));
	/* Past three-byte addressing. */
	KUNIT_EXPECT_TRUE(test, apple_qspi_window_valid(1, 2, 0, 0xf00000, 0x100000));
	KUNIT_EXPECT_FALSE(test, apple_qspi_window_valid(1, 2, 0, 0xf10000, 0x100000));
	KUNIT_EXPECT_FALSE(test, apple_qspi_window_valid(1, 2, 0, 0xfffff000, 0x10000));
}

static void qspi_policy_bank_state(struct kunit *test)
{
	unsigned char h[24];

	memset(h, 0xff, sizeof(h));
	KUNIT_EXPECT_EQ(test, apple_qspi_nvram_bank_state(h), 0);
	memset(h, 0, sizeof(h));
	memcpy(h, "3VVN", 4);
	h[5] = 0x40;	/* store size 0x4000 */
	h[14] = 1;
	KUNIT_EXPECT_EQ(test, apple_qspi_nvram_bank_state(h), 1);
	h[14] = 2;	/* another version */
	KUNIT_EXPECT_EQ(test, apple_qspi_nvram_bank_state(h), -1);
	h[14] = 1;
	h[5] = 0;
	h[6] = 1;	/* 0x10000: a full bank */
	KUNIT_EXPECT_EQ(test, apple_qspi_nvram_bank_state(h), 1);
	h[4] = 1;	/* past the bank */
	KUNIT_EXPECT_EQ(test, apple_qspi_nvram_bank_state(h), -1);
	h[4] = 23;
	h[6] = 0;	/* smaller than the header */
	KUNIT_EXPECT_EQ(test, apple_qspi_nvram_bank_state(h), -1);
	h[4] = 24;
	KUNIT_EXPECT_EQ(test, apple_qspi_nvram_bank_state(h), 1);
	h[0] = '2';
	KUNIT_EXPECT_EQ(test, apple_qspi_nvram_bank_state(h), -1);
	/* Firmware, not NVRAM, and a half-erased bank. */
	memset(h, 0, sizeof(h));
	KUNIT_EXPECT_EQ(test, apple_qspi_nvram_bank_state(h), -1);
	memset(h, 0xff, sizeof(h));
	h[23] = 0xfe;
	KUNIT_EXPECT_EQ(test, apple_qspi_nvram_bank_state(h), -1);
}

static void qspi_policy_parts(struct kunit *test)
{
	static const unsigned char known[][3] = {
		{ 0xef, 0x65, 0x17 }, { 0xc2, 0x2b, 0x27 },
		{ 0xef, 0x65, 0x15 }, { 0xc8, 0x83, 0x15 },
		{ 0xc8, 0x83, 0x17 },
	};
	static const unsigned int sizes[] = { 0x800000, 0x800000, 0x200000, 0x200000,
					      0x800000 };
	static const unsigned char bp_ok[] = { 0, 0, 0x1c, 0x1c, 0 };
	static const unsigned char unknown[][3] = {
		{ 0xef, 0x65, 0x16 }, { 0xef, 0x40, 0x17 }, { 0xc2, 0x2b, 0x28 },
		{ 0xc8, 0x60, 0x15 }, { 0xc8, 0x83, 0x16 }, { 0xc8, 0x83, 0x18 },
		{ 0, 0, 0 }, { 0xff, 0xff, 0xff },
	};
	const struct apple_qspi_part *part;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(known); i++) {
		part = apple_qspi_find_part(known[i]);
		KUNIT_ASSERT_NOT_NULL(test, part);
		KUNIT_EXPECT_EQ(test, part->size, sizes[i]);
		KUNIT_EXPECT_EQ(test, part->bp_ok, bp_ok[i]);
	}
	for (i = 0; i < ARRAY_SIZE(unknown); i++)
		KUNIT_EXPECT_NULL(test, apple_qspi_find_part(unknown[i]));
	/* The size bounds the window: the J700's fits 2 MiB, the J813's does not. */
	part = apple_qspi_find_part(known[2]);
	KUNIT_EXPECT_LE(test, 0x1f4000ULL, (unsigned long long)part->size);
	KUNIT_EXPECT_GT(test, 0x800000ULL, (unsigned long long)part->size);
}

static void qspi_policy_protected(struct kunit *test)
{
	unsigned long long s, e;

	KUNIT_EXPECT_EQ(test, apple_qspi_protected_range(0xef, 0, 0x200000, &s, &e), 0);
	/* BP0 on the J700's parts: the top 64 KiB, inside the window's last bank. */
	KUNIT_ASSERT_EQ(test, apple_qspi_protected_range(0xef, 0x04, 0x200000, &s, &e), 1);
	KUNIT_EXPECT_EQ(test, s, 0x1f0000ULL);
	KUNIT_EXPECT_EQ(test, e, 0x200000ULL);
	KUNIT_ASSERT_EQ(test, apple_qspi_protected_range(0xc8, 0x04, 0x200000, &s, &e), 1);
	KUNIT_EXPECT_EQ(test, s, 0x1f0000ULL);
	/* BP0+BP2: the top half; BP1+BP2: all of it. */
	KUNIT_ASSERT_EQ(test, apple_qspi_protected_range(0xef, 0x14, 0x200000, &s, &e), 1);
	KUNIT_EXPECT_EQ(test, s, 0x100000ULL);
	KUNIT_ASSERT_EQ(test, apple_qspi_protected_range(0xef, 0x18, 0x200000, &s, &e), 1);
	KUNIT_EXPECT_EQ(test, s, 0ULL);
	KUNIT_EXPECT_EQ(test, e, 0x200000ULL);
	/* Winbond TB: the bottom 64 KiB, below the J700's window. */
	KUNIT_ASSERT_EQ(test, apple_qspi_protected_range(0xef, 0x24, 0x200000, &s, &e), 1);
	KUNIT_EXPECT_EQ(test, s, 0ULL);
	KUNIT_EXPECT_EQ(test, e, 0x10000ULL);
	/* Winbond SEC: 4 KiB to 32 KiB. */
	KUNIT_ASSERT_EQ(test, apple_qspi_protected_range(0xef, 0x44, 0x200000, &s, &e), 1);
	KUNIT_EXPECT_EQ(test, s, 0x1ff000ULL);
	KUNIT_ASSERT_EQ(test, apple_qspi_protected_range(0xef, 0x54, 0x200000, &s, &e), 1);
	KUNIT_EXPECT_EQ(test, s, 0x1f8000ULL);
	/* GigaDevice SR1 bit 5 is BP3, not TB (the status policy refuses it). */
	KUNIT_ASSERT_EQ(test, apple_qspi_protected_range(0xc8, 0x24, 0x200000, &s, &e), 1);
	KUNIT_EXPECT_EQ(test, s, 0x1f0000ULL);
}

static void qspi_policy_unprotected_banks(struct kunit *test)
{
	unsigned long long s, e;
	int ret;

	/* Nothing protected inside the window: all of it. */
	ret = apple_qspi_unprotected_banks(0x24000, 0x1f4000, 0, 0x10000, &s, &e);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test, s, 0x24000ULL);
	KUNIT_EXPECT_EQ(test, e, 0x1f4000ULL);
	ret = apple_qspi_unprotected_banks(0x700000, 0x800000, 0, 0x700000, &s, &e);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test, s, 0x700000ULL);
	/* BP0 on the J700: its last bank [0x1e4000, 0x1f4000) is a quarter covered, and dropped. */
	ret = apple_qspi_unprotected_banks(0x24000, 0x1f4000, 0x1f0000, 0x200000, &s, &e);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test, s, 0x24000ULL);
	KUNIT_EXPECT_EQ(test, e, 0x1e4000ULL);
	/* TB with BP0+BP1: the bottom 256 KiB takes the first two banks. */
	ret = apple_qspi_unprotected_banks(0x24000, 0x1f4000, 0, 0x40000, &s, &e);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test, s, 0x44000ULL);
	KUNIT_EXPECT_EQ(test, e, 0x1f4000ULL);
	/* A range ending on a bank boundary takes exactly those banks. */
	ret = apple_qspi_unprotected_banks(0x24000, 0x1f4000, 0, 0x44000, &s, &e);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test, s, 0x44000ULL);
	ret = apple_qspi_unprotected_banks(0x24000, 0x1f4000, 0x1e4000, 0x200000, &s, &e);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test, e, 0x1e4000ULL);
	/* The top half, or all of the part: nothing left. */
	ret = apple_qspi_unprotected_banks(0x700000, 0x800000, 0x400000, 0x800000, &s, &e);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = apple_qspi_unprotected_banks(0x24000, 0x1f4000, 0, 0x200000, &s, &e);
	KUNIT_EXPECT_EQ(test, ret, 0);
	/* A range inside the window. */
	ret = apple_qspi_unprotected_banks(0x24000, 0x1f4000, 0x100000, 0x110000, &s, &e);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static void qspi_policy_sfdp(struct kunit *test)
{
	unsigned char h[16] = { 'S', 'F', 'D', 'P', 6, 1, 1, 0xff,
				0x00, 6, 1, 16, 0x30, 0, 0, 0xff };

	KUNIT_EXPECT_EQ(test, apple_qspi_sfdp_basic_table(h), 0x30L);
	h[3] = 'Q';
	KUNIT_EXPECT_EQ(test, apple_qspi_sfdp_basic_table(h), -1L);
	h[3] = 'P';
	h[8] = 0x84;	/* not the basic table */
	KUNIT_EXPECT_EQ(test, apple_qspi_sfdp_basic_table(h), -1L);
	/* 64 Mbit and 16 Mbit as bit counts minus one, and as a power of two. */
	KUNIT_EXPECT_EQ(test, apple_qspi_sfdp_size(0x03ffffff), 0x800000ULL);
	KUNIT_EXPECT_EQ(test, apple_qspi_sfdp_size(0x00ffffff), 0x200000ULL);
	KUNIT_EXPECT_EQ(test, apple_qspi_sfdp_size(0x80000000 | 26), 0x800000ULL);
	KUNIT_EXPECT_EQ(test, apple_qspi_sfdp_size(0x80000000 | 1), 0ULL);
}

static struct kunit_case apple_qspi_policy_cases[] = {
	KUNIT_CASE(qspi_policy_write_enable),
	KUNIT_CASE(qspi_policy_program),
	KUNIT_CASE(qspi_policy_erase),
	KUNIT_CASE(qspi_policy_refused_commands),
	KUNIT_CASE(qspi_policy_address_width),
	KUNIT_CASE(qspi_policy_empty),
	KUNIT_CASE(qspi_policy_nvram_700000),
	KUNIT_CASE(qspi_policy_status),
	KUNIT_CASE(qspi_policy_mx_status),
	KUNIT_CASE(qspi_policy_gd_status),
	KUNIT_CASE(qspi_policy_part_status),
	KUNIT_CASE(qspi_policy_window),
	KUNIT_CASE(qspi_policy_bank_state),
	KUNIT_CASE(qspi_policy_parts),
	KUNIT_CASE(qspi_policy_protected),
	KUNIT_CASE(qspi_policy_unprotected_banks),
	KUNIT_CASE(qspi_policy_sfdp),
	{}
};

static struct kunit_suite apple_qspi_policy_suite = {
	.name = "apple-qspi-nvram-policy",
	.test_cases = apple_qspi_policy_cases,
};
kunit_test_suite(apple_qspi_policy_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple QSPIMC NVRAM mutation policy");
MODULE_LICENSE("GPL");
