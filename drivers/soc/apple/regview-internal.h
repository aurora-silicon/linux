/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _REGVIEW_INTERNAL_H
#define _REGVIEW_INTERNAL_H

#include <linux/mod_devicetable.h>
#include <linux/seq_file.h>

/* One run of 32-bit registers inside one DT reg entry. */
struct apple_regview_range {
	u8 reg;		/* index into the node's reg property */
	u32 offset;	/* byte offset from the start of that entry */
	u32 count;	/* number of 32-bit registers */
	u32 stride;	/* byte distance between registers; 0 means 4 */
	const char *name;
};

struct apple_regview_hw {
	const struct apple_regview_range *ranges;
	unsigned int nr_ranges;
	unsigned int nr_regs;	/* reg entries the node must have */
};

struct apple_regview_map {
	void __iomem *base;
	resource_size_t size;
};

struct apple_regview {
	struct device *dev;
	const struct apple_regview_hw *hw;
	struct apple_regview_map *maps;
	struct dentry *dir;
};

#define MCC_TZ_BASE		0x6a0
#define MCC_TZ_STRIDE		0x10
#define MCC_TZ_REGIONS		4

#define MCC_PLANE_STRIDE	0x40000
#define MCC_PLANE_CACHE_STATUS	0x1c04
#define SPMI_SB_BUS_FAULTS	0x200
#define SPMI_SB_NR_BUS_FAULTS	10
#define SPMI_MON_COUNTERS		1

#if IS_ENABLED(CONFIG_APPLE_REGVIEW_KUNIT_TEST)
u32 apple_regview_stride(const struct apple_regview_range *r);
int apple_regview_check(const struct apple_regview *rv);
int apple_regview_show(struct seq_file *s, void *unused);
extern const struct of_device_id apple_regview_of_match[];
const struct file_operations *apple_regview_test_fops(void);
#endif

#endif
