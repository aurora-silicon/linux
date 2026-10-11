// SPDX-License-Identifier: GPL-2.0-only
#ifndef T6050_PCIE_DISABLE_H
#define T6050_PCIE_DISABLE_H
#include "t6050_pcie_sequence.h"

int t6050_pcie_disable_port0(const struct t6050_pcie_io *io,
			     int (*force_available)(void *, bool),
			     unsigned int timeout_us);
#endif
