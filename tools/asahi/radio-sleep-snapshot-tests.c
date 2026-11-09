// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct device {
	int unused;
};
struct hw_info {
	u32 port_msimap, port_msiaddr, port_msiaddr_hi;
};
struct apple_pcie {
	struct device *dev;
	struct hw_info *hw;
	u16 managed_sids;
	unsigned int nvecs;
	unsigned long *bitmap;
};
struct apple_pcie_port {
	unsigned long sid_map[1];
	unsigned int sid_map_sz;
};
struct apple_radio_sleep {
	struct apple_pcie *pcie;
	struct apple_pcie_port *port;
	int fn[2];
	u32 rid[16], msi[32], msicfg, msibase, msiaddr, msiaddr_hi, intmask,
		appclk;
	unsigned long vectors[1], sids[1];
};
static u32 regs[0x4000 / 4];
static bool irqoff;
static unsigned int logs, reads;
static bool irqs_disabled(void)
{
	return irqoff;
}
static void dev_err(struct device *d, const char *fmt, ...)
{
	(void)d;
	(void)fmt;
	logs++;
}
static u32 apple_pcie_port_readl(struct apple_pcie_port *p, u32 off)
{
	(void)p;
	assert(off < sizeof(regs) && !(off & 3));
	reads++;
	return regs[off / 4];
}
static u32 port_rid2sid_offset(struct apple_pcie_port *p, unsigned int i)
{
	(void)p;
	assert(i < 16);
	return 0x3000 + 4 * i;
}
static bool bitmap_equal(const unsigned long *a, const unsigned long *b,
			 unsigned int n)
{
	assert(n <= 32);
	return ((*a ^ *b) & ((1UL << n) - 1)) == 0;
}
static void bitmap_copy(unsigned long *a, const unsigned long *b,
			unsigned int n)
{
	assert(n <= 32);
	*a = *b;
}
/* ACTUAL_PRODUCTION */
int main(void)
{
	unsigned long vectors = 7;
	struct hw_info hw = { .port_msimap = 0x3800,
			      .port_msiaddr = 0x168,
			      .port_msiaddr_hi = 0x16c };
	struct apple_pcie host = {
		.bitmap = &vectors, .hw = &hw, .managed_sids = 6, .nvecs = 32
	};
	struct apple_pcie_port port = { .sid_map_sz = 16, .sid_map = { 6 } };
	struct apple_radio_sleep s = { .pcie = &host, .port = &port };

	regs[PORT_STATUS / 4] = PORT_STATUS_READY;
	/* An operating radio link may assert both UP and BUSY. */
	regs[PORT_LINKSTS / 4] = 0x9b00063d;
	regs[0x3004 / 4] = 0x80010100;
	regs[0x3008 / 4] = 0x80020101;
	regs[PORT_MSICFG / 4] = PORT_MSICFG_EN;
	regs[hw.port_msiaddr / 4] = 0xfffff000;
	for (unsigned int i = 0; i < 32; i++)
		regs[hw.port_msimap / 4 + i] = 0x80000000 | i;
	assert(apple_radio_snapshot(&s, false) == 0);
	assert(apple_radio_snapshot(&s, true) == 0);
	assert(logs == 0);
	unsigned int corrupt[] = {
		0x3004,	      0x3008,	       0x3010,
		0x3800,	      0x387c,	       PORT_MSICFG,
		PORT_MSIBASE, hw.port_msiaddr, hw.port_msiaddr_hi
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(corrupt); i++) {
		regs[corrupt[i] / 4] ^= 0x10;
		irqoff = true;
		assert(apple_radio_snapshot(&s, true) == -EUCLEAN);
		assert(logs == 0); /* No console use in early verification. */
		regs[corrupt[i] / 4] ^= 0x10;
	}
	host.bitmap[0] ^= 1;
	assert(apple_radio_snapshot(&s, true) == -EUCLEAN);
	host.bitmap[0] ^= 1;
	port.sid_map[0] ^= 1;
	assert(apple_radio_snapshot(&s, true) == -EUCLEAN);
	port.sid_map[0] ^= 1;
	irqoff = false;
	regs[PORT_STATUS / 4] = 0;
	reads = 0;
	assert(apple_radio_snapshot(&s, false) == -EUCLEAN && reads == 1);
	regs[PORT_STATUS / 4] = PORT_STATUS_READY;
	regs[PORT_LINKSTS / 4] &= ~PORT_LINKSTS_UP;
	reads = 0;
	assert(apple_radio_snapshot(&s, false) == -EUCLEAN && reads == 2);
	regs[PORT_LINKSTS / 4] |= PORT_LINKSTS_UP;
	regs[0x3004 / 4] ^= 1;
	assert(apple_radio_snapshot(&s, false) == -EUCLEAN);
	regs[0x3004 / 4] ^= 1;
	regs[PORT_MSICFG / 4] = 0;
	assert(apple_radio_snapshot(&s, false) == -EUCLEAN);
	puts("PASS: live-link status and RID/MSI snapshot validation");
}
