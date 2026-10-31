// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the Apple QSPIMC NVRAM mutation policy, at the J700 and
 * J613/J713/J616s nvram partitions.
 */

#include <kunit/test.h>

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
	{}
};

static struct kunit_suite apple_qspi_policy_suite = {
	.name = "apple-qspi-nvram-policy",
	.test_cases = apple_qspi_policy_cases,
};
kunit_test_suite(apple_qspi_policy_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple QSPIMC NVRAM mutation policy");
MODULE_LICENSE("GPL");
