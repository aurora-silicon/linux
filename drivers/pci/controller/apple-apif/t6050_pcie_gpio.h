// SPDX-License-Identifier: GPL-2.0-only
#ifndef T6050_PCIE_GPIO_H
#define T6050_PCIE_GPIO_H
#include <linux/types.h>

struct t6050_gpio_pad_io {
	void *context;
	int (*read)(void *, u32 *);
	int (*write)(void *, u32);
};
int t6050_pcie_gpio_perst(const struct t6050_gpio_pad_io *, bool asserted);
#endif
