/* SPDX-License-Identifier: GPL-2.0 */
#ifndef APPLE_QSPI_POLICY_H
#define APPLE_QSPI_POLICY_H

#define APPLE_QSPI_PAGE_SIZE   256U
#define APPLE_QSPI_ERASE_SIZE  4096U

/*
 * [start, end) is the write window, the device tree's nvram partition.
 * Subtraction avoids overflow; addr is not truncated to wire width first.
 */
static inline int apple_qspi_range_allowed(unsigned long long addr,
		unsigned int len, unsigned long long start,
		unsigned long long end)
{
	return len && start < end && addr >= start && addr < end &&
		len <= end - addr;
}

/*
 * May a write-path request reach the boot NOR? Write enable control is
 * virtual and admitted. A single-wire, three-byte page program that stays
 * within one 256-byte page, or an aligned 4 KiB sector erase, is admitted
 * only when every byte it can change lies inside [start, end). Everything
 * else (status and configuration writes, larger erases, chip erase,
 * address-mode, reset and OTP commands) is refused.
 */
static inline int apple_qspi_mutation_allowed(unsigned int opcode,
		unsigned int addr_bytes, unsigned long long addr,
		unsigned int len, int data_out,
		unsigned long long start, unsigned long long end)
{
	switch (opcode) {
	case 0x06: /* Virtual WREN, never sent independently. */
	case 0x04: /* Virtual WRDI. */
		return !addr_bytes && !len && !data_out;
	case 0x02: /* PP: prevent page wrap, even if SPI-NOR gets geometry wrong. */
		return addr_bytes == 3 && data_out && len <= APPLE_QSPI_PAGE_SIZE &&
			apple_qspi_range_allowed(addr, len, start, end) &&
			len <= APPLE_QSPI_PAGE_SIZE - (addr & (APPLE_QSPI_PAGE_SIZE - 1));
	case 0x20: /* SE: hard-code the entire affected 4 KiB range. */
		return addr_bytes == 3 && !len && !data_out &&
			!(addr & (APPLE_QSPI_ERASE_SIZE - 1)) &&
			apple_qspi_range_allowed(addr, APPLE_QSPI_ERASE_SIZE,
						 start, end);
	default:
		return 0;
	}
}

/*
 * Protection and activity bits must permit a write; JEDEC ID is checked
 * separately. BUSY/WEL, CMP/SUS and WPS are always refused. bp_ok names the
 * SR1 block-protect bits a board's firmware leaves set at boot: they stay
 * unchanged, the flash ignores programs and erases in the range they protect,
 * and the driver's readback then latches it read-only. SRP, TB, SEC, QE and
 * LB stay unchanged.
 */
static inline int apple_qspi_status_allows_write(unsigned int sr1,
		unsigned int sr2, unsigned int sr3, unsigned int bp_ok)
{
	return !(sr1 & 0x03) && !(sr1 & 0x1c & ~bp_ok) &&
		!(sr2 & 0xc0) && !(sr3 & 0x04);
}

/* JEDEC manufacturer ID of Macronix parts. */
#define APPLE_QSPI_MFR_MACRONIX	0xc2

/*
 * The Macronix status register holds WIP, WEL and four block-protect bits,
 * BP[3:0] at bits 5:2. BUSY and WEL are always refused, and block-protect
 * bits only where bp_ok allows them. QE and SRWD stay unchanged.
 */
static inline int apple_qspi_mx_status_allows_write(unsigned int sr,
						    unsigned int bp_ok)
{
	return !(sr & 0x03) && !(sr & 0x3c & ~bp_ok);
}

/* JEDEC manufacturer ID of GigaDevice parts. */
#define APPLE_QSPI_MFR_GIGADEVICE	0xc8

/*
 * GigaDevice GD25UF16E and GD25UF64E. SR1 holds WIP, WEL, five block-protect
 * bits BP[4:0] at bits 6:2 and SRP0. SR2 holds SRP1, QE, SUS2 at bit 2,
 * LB[3:1], CMP and SUS1 at bit 7. SR3 holds output drive, dummy-cycle and
 * low-power settings and has no write-protect selection. BUSY and WEL, both
 * suspend bits and CMP are always refused. BP[2:0] sit where they do on
 * Winbond parts and are tolerated only where bp_ok allows them; BP3 and BP4
 * move or shrink the protected range and are refused unless bp_ok names them.
 * SRP0, SRP1, QE and LB stay unchanged.
 */
static inline int apple_qspi_gd_status_allows_write(unsigned int sr1,
						    unsigned int sr2,
						    unsigned int bp_ok)
{
	return !(sr1 & 0x03) && !(sr1 & 0x7c & ~bp_ok) && !(sr2 & 0xc4);
}

/* Pick the status checks from the manufacturer the flash itself reports. */
static inline int apple_qspi_part_status_allows_write(unsigned int mfr,
						      unsigned int sr1,
						      unsigned int sr2,
						      unsigned int sr3,
						      unsigned int bp_ok)
{
	switch (mfr) {
	case APPLE_QSPI_MFR_MACRONIX:
		return apple_qspi_mx_status_allows_write(sr1, bp_ok);
	case APPLE_QSPI_MFR_GIGADEVICE:
		return apple_qspi_gd_status_allows_write(sr1, sr2, bp_ok);
	default:
		return apple_qspi_status_allows_write(sr1, sr2, sr3, bp_ok);
	}
}

/* The size of one NVRAM v3 bank ("3VVN" store). */
#define APPLE_QSPI_NVRAM_BANK	0x10000U

/*
 * Whether an nvram partition can be the write window: the only one with that
 * label, one address and one size cell, not marked read-only, a whole number
 * of NVRAM banks from a 4 KiB boundary, and within three-byte addressing.
 */
static inline int apple_qspi_window_valid(unsigned int found,
					  unsigned int reg_cells, int read_only,
					  unsigned long long start,
					  unsigned long long size)
{
	return found == 1 && reg_cells == 2 && !read_only && size &&
		!(start % APPLE_QSPI_ERASE_SIZE) &&
		!(size % APPLE_QSPI_NVRAM_BANK) && start + size <= 1ULL << 24;
}

/*
 * The first 24 bytes of an NVRAM bank: 1 for a v3 store header ("3VVN",
 * version 1, a store size from the header itself up to the bank), 0 for a
 * bank that may be erased (the caller checks the rest of it), -1 for
 * anything else. The v3 header has no checksum of its own; each variable in
 * the store carries a CRC32.
 */
static inline int apple_qspi_nvram_bank_state(const unsigned char h[24])
{
	unsigned int size = h[4] | h[5] << 8 | h[6] << 16 | (unsigned int)h[7] << 24;
	int i;

	for (i = 0; i < 24 && h[i] == 0xff; i++)
		;
	if (i == 24)
		return 0;
	if (h[0] == '3' && h[1] == 'V' && h[2] == 'V' && h[3] == 'N' &&
	    h[14] == 1 && size >= 24 && size <= APPLE_QSPI_NVRAM_BANK)
		return 1;
	return -1;
}

/*
 * The boot NOR parts whose nvram partition may be written: the JEDEC ID, the
 * size, which bounds the window (a three-byte address past it wraps around)
 * and must match the part's SFDP density, and the SR1 block-protect bits the
 * part's firmware leaves set. Those stay set; apple_qspi_protected_range()
 * says what they protect.
 */
struct apple_qspi_part {
	unsigned char jedec[3];
	unsigned int size;
	unsigned char bp_ok;
};

#define APPLE_QSPI_JEDEC_W25Q64		{ 0xef, 0x65, 0x17 }
#define APPLE_QSPI_JEDEC_W25Q16		{ 0xef, 0x65, 0x15 }
#define APPLE_QSPI_JEDEC_MX_2B27	{ 0xc2, 0x2b, 0x27 }
#define APPLE_QSPI_JEDEC_GD25UF16E	{ 0xc8, 0x83, 0x15 }
#define APPLE_QSPI_JEDEC_GD25UF64E	{ 0xc8, 0x83, 0x17 }

static const struct apple_qspi_part apple_qspi_parts[] = {
	/* J613, J713, J813; also on the bus of some J616s */
	{ APPLE_QSPI_JEDEC_W25Q64, 0x800000, 0 },
	/* J616s, the ID macOS reports */
	{ APPLE_QSPI_JEDEC_MX_2B27, 0x800000, 0 },
	/* J700: iBoot leaves BP0 set, over the top 64 KiB */
	{ APPLE_QSPI_JEDEC_W25Q16, 0x200000, 0x1c },
	/* Some J700s; BP0 covers the same range */
	{ APPLE_QSPI_JEDEC_GD25UF16E, 0x200000, 0x1c },
	/* J775D (M5 Ultra): no block protection left set, SR1 0x00 */
	{ APPLE_QSPI_JEDEC_GD25UF64E, 0x800000, 0 },
};

static inline const struct apple_qspi_part *
apple_qspi_find_part(const unsigned char id[3])
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(apple_qspi_parts); i++)
		if (id[0] == apple_qspi_parts[i].jedec[0] &&
		    id[1] == apple_qspi_parts[i].jedec[1] &&
		    id[2] == apple_qspi_parts[i].jedec[2])
			return &apple_qspi_parts[i];
	return NULL;
}

/*
 * What SR1 protects on a part that tolerates BP bits (the 2 MiB parts, whose
 * status policy leaves BP0-BP2 and, on Winbond, TB and SEC as found; CMP, and
 * BP3/BP4 on GigaDevice, are refused before this). BP0-BP2 of 1 to 5 protect
 * 1/32 to 1/2 of the part, or with SEC 4 to 32 KiB; 6 and 7 protect all of it.
 * TB puts the range at the bottom instead of the top. Sets [*start, *end) and
 * returns 1, or returns 0 when nothing is protected.
 */
static inline int apple_qspi_protected_range(unsigned int mfr, unsigned int sr1,
					     unsigned long long size,
					     unsigned long long *start,
					     unsigned long long *end)
{
	unsigned int bp = (sr1 >> 2) & 7, shift;
	int bottom = 0, sec = 0;
	unsigned long long len;

	if (!bp)
		return 0;
	if (mfr != APPLE_QSPI_MFR_GIGADEVICE) {
		bottom = !!(sr1 & 0x20);
		sec = !!(sr1 & 0x40);
	}
	if (bp >= 6) {
		len = size;
	} else if (sec) {
		shift = bp - 1 < 3 ? bp - 1 : 3;
		len = 0x1000ULL << shift;
	} else {
		len = (size >> 5) << (bp - 1);
	}
	*start = bottom ? 0 : size - len;
	*end = *start + len;
	return 1;
}

/*
 * The banks of the window [start, end) that a protected range [ps, pe) does
 * not reach, as [*wstart, *wend): a bank the range only partly covers is
 * dropped whole, so an erase of it never stops halfway. The decoded range
 * always reaches one end of the part, so it takes a run of banks from one
 * end of the window. Returns 0 when no bank is left, or for a range inside
 * the window with banks on both sides of it, which no decode produces.
 */
static inline int apple_qspi_unprotected_banks(unsigned long long start,
					       unsigned long long end,
					       unsigned long long ps,
					       unsigned long long pe,
					       unsigned long long *wstart,
					       unsigned long long *wend)
{
	unsigned long long bank = APPLE_QSPI_NVRAM_BANK;

	*wstart = start;
	*wend = end;
	if (pe <= start || ps >= end)
		return 1;
	if (ps <= start)
		*wstart = start + (pe - start + bank - 1) / bank * bank;
	else if (pe >= end)
		*wend = start + (ps - start) / bank * bank;
	else
		return 0;
	return *wstart < *wend;
}

/*
 * The JEDEC basic flash parameter table from the first 16 bytes of the SFDP
 * space: "SFDP", then the first parameter header, which must be the basic
 * table (ID 0x00, MSB 0xff). Returns the table's address, or -1.
 */
static inline long apple_qspi_sfdp_basic_table(const unsigned char h[16])
{
	if (h[0] != 'S' || h[1] != 'F' || h[2] != 'D' || h[3] != 'P' ||
	    h[8] != 0x00 || h[15] != 0xff || h[11] < 2)
		return -1;
	return h[12] | h[13] << 8 | h[14] << 16;
}

/* The part size in bytes from the basic table's second DWORD, or 0. */
static inline unsigned long long apple_qspi_sfdp_size(unsigned int dword2)
{
	unsigned int n = dword2 & 0x7fffffff;

	if (dword2 & 0x80000000)
		return n >= 3 && n < 64 ? 1ULL << (n - 3) : 0;
	return ((unsigned long long)dword2 + 1) / 8;
}
#endif
