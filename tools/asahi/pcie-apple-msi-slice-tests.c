// SPDX-License-Identifier: GPL-2.0-only
/* Host test of apple_pciec_msi_slice(): the T6030 PCIe-C MSI slice shapes. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "../../drivers/pci/controller/pcie-apple-t8122.h"

int main(void)
{
	unsigned int irq, n;

	/* Legacy boot DT: no base, the global window. Corrected to the bridge's slice. */
	irq = 1342; n = 512;
	assert(apple_pciec_msi_slice(false, 0, 32, 1342, &irq, &n) == 1);
	assert(irq == 1374 && n == 32);
	irq = 1342; n = 512;
	assert(apple_pciec_msi_slice(false, 0, 96, 1342, &irq, &n) == 1 && irq == 1438);
	/* Per-slice description: accepted as is. */
	irq = 1374; n = 32;
	assert(apple_pciec_msi_slice(true, 32, 32, 1342, &irq, &n) == 0 && irq == 1374 && n == 32);
	/* Global window with a base (42d2e45a's shape): refused. */
	irq = 1342; n = 512;
	assert(apple_pciec_msi_slice(true, 32, 32, 1342, &irq, &n) < 0);
	/* A base that is not this aperture's, or a misplaced line: refused. */
	irq = 1406; n = 32;
	assert(apple_pciec_msi_slice(true, 64, 32, 1342, &irq, &n) < 0);
	irq = 1375; n = 32;
	assert(apple_pciec_msi_slice(true, 32, 32, 1342, &irq, &n) < 0);
	/* No base outside T6030's legacy shape: refused. */
	irq = 2103; n = 32;
	assert(apple_pciec_msi_slice(false, 0, 32, 2071, &irq, &n) < 0);
	/* T6040's per-slice shape still accepted. */
	irq = 2103; n = 32;
	assert(apple_pciec_msi_slice(true, 32, 32, 2071, &irq, &n) == 0);
	puts("pcie-apple-msi-slice-tests: OK");
	return 0;
}
