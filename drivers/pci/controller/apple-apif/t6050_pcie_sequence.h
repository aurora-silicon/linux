// SPDX-License-Identifier: GPL-2.0-only
#ifndef T6050_PCIE_SEQUENCE_H
#define T6050_PCIE_SEQUENCE_H
#include <linux/types.h>

struct t6050_pcie_io {
	void *context;
	int (*read)(void *, unsigned int adt_reg, unsigned int offset, u32 *);
	int (*write)(void *, unsigned int adt_reg, unsigned int offset, u32);
	int (*poll)(void *, unsigned int adt_reg, unsigned int offset, u32 mask,
		    u32 value, unsigned int timeout_us);
	int (*apply_tunables)(void *, const char *property,
			      unsigned int adt_reg);
	int (*apply_port_tunables)(void *, const char *property,
				   unsigned int adt_reg);
	int (*read16)(void *, unsigned int adt_reg, unsigned int offset, u16 *);
	int (*write16)(void *, unsigned int adt_reg, unsigned int offset, u16);
	void (*delay)(void *, unsigned int usec);

	int (*endpoint_reset)(void *, bool asserted);
};

enum {
	T6050_PCIE_ECAM = 0,
	T6050_PCIE_PORT0 = 16,
	T6050_PCIE_PORT0_PHY = 18,
	T6050_PCIE_PORT0_INTR2AXI = 19,
};

int t6050_pcie_reset_port0(const struct t6050_pcie_io *io);
int t6050_pcie_enable_port0(const struct t6050_pcie_io *io,
			    unsigned int timeout_us);
int t6050_pcie_configure_port0(const struct t6050_pcie_io *io,
			       unsigned int max_speed);
int t6050_pcie_train_port0(const struct t6050_pcie_io *io,
			   unsigned int timeout_us);
int t6050_pcie_configure_msi_port0(const struct t6050_pcie_io *io, u64 doorbell,
				   unsigned int count);
#endif
