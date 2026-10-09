/* SPDX-License-Identifier: GPL-2.0 */
#ifndef APPLE_QSPI_POLICY_H
#define APPLE_QSPI_POLICY_H

#define APPLE_QSPI_NVRAM_START 0x700000ULL
#define APPLE_QSPI_NVRAM_END   0x800000ULL
#define APPLE_QSPI_PAGE_SIZE   256U
#define APPLE_QSPI_ERASE_SIZE  4096U

/* Subtraction avoids overflow; addr is not truncated to wire width first. */
static inline int apple_qspi_range_allowed(unsigned long long addr, unsigned int len)
{
	return len && addr >= APPLE_QSPI_NVRAM_START &&
		addr < APPLE_QSPI_NVRAM_END && len <= APPLE_QSPI_NVRAM_END - addr;
}

static inline int apple_qspi_mutation_allowed(unsigned int opcode,
		unsigned int addr_bytes, unsigned long long addr,
		unsigned int len, int data_out)
{
	switch (opcode) {
	case 0x06: /* Virtual WREN, never sent independently. */
	case 0x04: /* Virtual WRDI. */
		return !addr_bytes && !len && !data_out;
	case 0x02: /* PP: prevent page wrap, even if SPI-NOR gets geometry wrong. */
		return addr_bytes == 3 && data_out && len <= APPLE_QSPI_PAGE_SIZE &&
			apple_qspi_range_allowed(addr, len) &&
			len <= APPLE_QSPI_PAGE_SIZE - (addr & (APPLE_QSPI_PAGE_SIZE - 1));
	case 0x20: /* SE: hard-code the entire affected 4 KiB range. */
		return addr_bytes == 3 && !len && !data_out &&
			!(addr & (APPLE_QSPI_ERASE_SIZE - 1)) &&
			apple_qspi_range_allowed(addr, APPLE_QSPI_ERASE_SIZE);
	default:
		return 0;
	}
}

/* Protection and activity bits must permit a write; JEDEC ID is checked separately. */
static inline int apple_qspi_status_allows_write(unsigned int sr1,
		unsigned int sr2, unsigned int sr3)
{
	/* BUSY/WEL, BP[2:0]; CMP/SUS; WPS. SRP, TB, SEC, QE, LB stay unchanged. */
	return !(sr1 & 0x1f) && !(sr2 & 0xc0) && !(sr3 & 0x04);
}
#endif
