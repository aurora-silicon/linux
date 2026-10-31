/* SPDX-License-Identifier: GPL-2.0 */
#ifndef APPLE_QSPI_POLICY_H
#define APPLE_QSPI_POLICY_H

#define APPLE_QSPI_PAGE_SIZE   256U
#define APPLE_QSPI_ERASE_SIZE  4096U

/*
 * [start, end) is the board's nvram partition. Subtraction avoids overflow;
 * addr is not truncated to wire width first.
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
#endif
