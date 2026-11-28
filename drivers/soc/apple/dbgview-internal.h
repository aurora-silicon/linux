/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _DBGVIEW_INTERNAL_H
#define _DBGVIEW_INTERNAL_H

#include <linux/ioport.h>
#include <linux/seq_file.h>

struct dbgview {
	struct device *dev;
	void __iomem *base;
	resource_size_t size;
	struct dentry *dir;
};

#if IS_ENABLED(CONFIG_APPLE_DBGVIEW_KUNIT_TEST)
ssize_t dbgview_regs_read(struct file *file, char __user *ubuf,
			  size_t count, loff_t *ppos);
int dbgview_snapshot(struct seq_file *s, unsigned int bytes);
extern const struct file_operations dbgview_regs_fops;
const struct file_operations *dbgview_test_snapshot_fops(void);
#endif

#endif
