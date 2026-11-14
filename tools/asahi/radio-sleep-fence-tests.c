// SPDX-License-Identifier: GPL-2.0-only
/* Failure-first host fixture for the production checked PCI command fence. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint16_t u16;
#define PCI_COMMAND 4
#define PCI_COMMAND_MASTER 4
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
struct pci_dev {
	u16 command;
	int read_count, write_count, fail_read, fail_write;
	bool wrong_write;
};
struct apple_radio_function {
	struct pci_dev *pdev;
	u16 command;
	bool attempted, held;
};
static int pci_read_config_word(struct pci_dev *d, int reg, u16 *out)
{
	assert(reg == PCI_COMMAND);
	if (++d->read_count == d->fail_read)
		return 1;
	*out = d->command;
	return 0;
}
static int pci_write_config_word(struct pci_dev *d, int reg, u16 value)
{
	assert(reg == PCI_COMMAND);
	d->write_count++;
	d->command = d->wrong_write ? (value ^ 2) : value;
	return d->write_count == d->fail_write;
}
/* ACTUAL_FUNCTIONS */
int main(void)
{
	for (int fault = 0; fault < 6; fault++) {
		struct pci_dev d = { .command = 7 };
		struct apple_radio_function f = { .pdev = &d };

		if (fault == 1)
			d.fail_read = 1;
		if (fault == 2)
			d.command = 0xffff;
		if (fault == 3)
			d.fail_write = 1;
		if (fault == 4)
			d.fail_read = 2;
		if (fault == 5)
			d.wrong_write = true;
		int r = apple_radio_bme_hold(&f);

		assert((r == 0) == (fault == 0));
		if (fault == 1 || fault == 2) {
			assert(!f.attempted);
			assert(d.write_count == 0);
		} else
			assert(f.attempted);
		if (!fault) {
			assert(f.held && d.command == 3);
			assert(!apple_radio_bme_restore(&f));
			assert(d.command == 7 && !f.held);
		}
	}
	{
		struct pci_dev d = { .command = 3 };
		struct apple_radio_function f = { .pdev = &d };

		assert(!apple_radio_bme_hold(&f));
		assert(!apple_radio_bme_restore(&f));
		assert(d.command == 3);
	}
	{
		struct pci_dev d = { .command = 7 };
		struct apple_radio_function f = { .pdev = &d };

		assert(!apple_radio_bme_hold(&f));
		d.command = 1;
		int w = d.write_count;

		assert(apple_radio_bme_restore(&f) == -EUCLEAN);
		assert(d.write_count == w && f.held);
	}
	{
		struct pci_dev d = { .command = 7, .fail_write = 1 };
		struct apple_radio_function f = { .pdev = &d };

		assert(apple_radio_bme_hold(&f));
		assert(f.attempted);
		assert(!apple_radio_bme_restore(&f));
		assert(d.command == 7);
	}
	puts("PASS: PCI bus-master fence and uncertain-write recovery");
}
