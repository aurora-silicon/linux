// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define dev_info(...) ((void)0)
struct device {
	int unused;
};
struct pci_dev {
	int unused;
};
struct hardware {
	u32 bar0_window1, bar0_window2, bar0_core2_window2;
	bool has_bar0_core2_window2, clear_pciecfg_subsystem_ctrl_bit19;
};
struct bcm4377_data {
	struct pci_dev *pdev;
	struct hardware *hw;
};
static struct pci_dev pci;
static struct hardware hw = { 0x18002000, 0x18109000, 0x18106000, true, false };
static struct bcm4377_data data = { &pci, &hw };
static bool owned, physical, ignore_window;
static unsigned int ops, writes, fail;
static u32 cfg[64];
static jmp_buf fatal;
static struct pci_dev *to_pci_dev(struct device *d)
{
	(void)d;
	return &pci;
}
static struct bcm4377_data *pci_get_drvdata(struct pci_dev *d)
{
	(void)d;
	return &data;
}
static bool apple_radio_sleep_owned(struct device *d)
{
	(void)d;
	return owned;
}
static bool pm_resume_via_firmware(void)
{
	return physical;
}
static int pci_read_config_dword(struct pci_dev *d, unsigned int off, u32 *v)
{
	(void)d;
	if (++ops == fail)
		return 0x87;
	*v = cfg[off / 4];
	return 0;
}
static int pci_write_config_dword(struct pci_dev *d, unsigned int off, u32 v)
{
	(void)d;
	writes++;
	if (++ops == fail)
		return 0x87;
	if (!(ignore_window && off == 0x80))
		cfg[off / 4] = v;
	return 0;
}
#define panic(...) longjmp(fatal, 1)
/* ACTUAL */
static void fresh(void)
{
	owned = physical = true;
	ignore_window = false;
	ops = writes = fail = 0;
	memset(cfg, 0, sizeof(cfg));
	cfg[0x88 / 4] = BIT(19) | 0x80;
}
int main(void)
{
	struct device dev = {};

	fresh();

	assert(!bcm4377_resume_noirq(&dev));
	assert(cfg[0x80 / 4] == hw.bar0_window1);
	assert(cfg[0x70 / 4] == hw.bar0_window2);
	assert(cfg[0x74 / 4] == 0x18011000);
	assert(cfg[0x78 / 4] == hw.bar0_core2_window2);
	assert(cfg[0x84 / 4] == 0x19000000);
	assert(cfg[0x88 / 4] == (BIT(19) | BIT(16) | 0x80));
	unsigned int count = ops;

	fresh();

	owned = false;
	assert(!bcm4377_resume_noirq(&dev));
	assert(!ops);
	fresh();
	physical = false;
	assert(!bcm4377_resume_noirq(&dev));
	assert(!ops);
	for (unsigned int i = 1; i <= count; i++) {
		fresh();
		fail = i;
		if (!setjmp(fatal)) {
			bcm4377_resume_noirq(&dev);
			assert(0);
		}
		assert(ops == i);
	}
	fresh();
	ignore_window = true;
	if (!setjmp(fatal)) {
		bcm4377_resume_noirq(&dev);
		assert(0);
	}
	puts("PASS: private PCI windows, retained-path skip and restore failures");
	return 0;
}
