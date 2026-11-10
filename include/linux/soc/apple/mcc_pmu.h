/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_MCC_PMU_H
#define _LINUX_SOC_APPLE_MCC_PMU_H

#include <linux/auxiliary_bus.h>
#include <linux/io.h>

#define APPLE_MCC_PMU_MAX_REGS	5

enum apple_mcc_pmu_type {
	APPLE_MCC_PMU_T6000,
	APPLE_MCC_PMU_T8112,
};

struct apple_mcc_pmu_map {
	void __iomem *base;
	resource_size_t size;
};

/* Shared by the read-only MCC mapper and its state-owning PMU child. */
struct apple_mcc_pmu_aux {
	struct auxiliary_device auxdev;
	enum apple_mcc_pmu_type type;
	unsigned int nr_maps;
	struct apple_mcc_pmu_map maps[APPLE_MCC_PMU_MAX_REGS];
};

#endif
