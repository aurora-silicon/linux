/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Per-phase timing of the requests an ANE engine ran, shared by ane.ko
 * (H13 task manager) and ane_t6021.ko (H14 firmware).
 *
 * A driver fills one sample per successful request: first the phases in
 * nanoseconds, then plain counts (poll sleeps, or 1 when the request had to
 * power something up). Each sample folds into the last, sum, minimum and
 * maximum since the last reset. A debugfs file prints the table in
 * microseconds and a write to it resets it. Failed requests are not
 * counted. There is no switch: the cost is a few ktime_get_ns() calls and
 * one short spinlock per request.
 */
#ifndef __ANE_EXEC_TIMING_H__
#define __ANE_EXEC_TIMING_H__

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>

#define ANE_XT_MAX	12

struct ane_xt_sample {
	u64 v[ANE_XT_MAX];
};

struct ane_xt_stats {
	u64 count;
	struct ane_xt_sample last, sum, min, max;
};

struct ane_xt {
	spinlock_t lock;	/* protects st */
	const char *const *names;
	unsigned int n;		/* entries in use */
	unsigned int nns;	/* the first nns entries are nanoseconds */
	struct ane_xt_stats st;
};

static inline void ane_xt_init(struct ane_xt *xt, const char *const *names,
			       unsigned int nns, unsigned int n)
{
	spin_lock_init(&xt->lock);
	xt->names = names;
	xt->n = min_t(unsigned int, n, ANE_XT_MAX);
	xt->nns = min(nns, xt->n);
	memset(&xt->st, 0, sizeof(xt->st));
}

static inline void ane_xt_fold(struct ane_xt_stats *st,
			       const struct ane_xt_sample *x, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		u64 v = x->v[i];

		st->sum.v[i] += v;
		if (!st->count || v < st->min.v[i])
			st->min.v[i] = v;
		if (!st->count || v > st->max.v[i])
			st->max.v[i] = v;
	}
	st->last = *x;
	st->count++;
}

static inline void ane_xt_add(struct ane_xt *xt, const struct ane_xt_sample *x)
{
	unsigned long flags;

	spin_lock_irqsave(&xt->lock, flags);
	ane_xt_fold(&xt->st, x, xt->n);
	spin_unlock_irqrestore(&xt->lock, flags);
}

static inline void ane_xt_snapshot(struct ane_xt *xt, struct ane_xt_stats *out,
				   bool reset)
{
	unsigned long flags;

	spin_lock_irqsave(&xt->lock, flags);
	if (out)
		*out = xt->st;
	if (reset)
		memset(&xt->st, 0, sizeof(xt->st));
	spin_unlock_irqrestore(&xt->lock, flags);
}

/* Nanoseconds as microseconds with one decimal. */
static inline void ane_xt_us(struct seq_file *m, u64 ns)
{
	seq_printf(m, " %9llu.%u", div_u64(ns, 1000),
		   (unsigned int)(div_u64(ns, 100) % 10));
}

static inline int ane_xt_show(struct seq_file *m, void *v)
{
	struct ane_xt *xt = m->private;
	struct ane_xt_stats *st;
	unsigned int i;

	st = kmalloc_obj(*st);
	if (!st)
		return -ENOMEM;
	ane_xt_snapshot(xt, st, false);
	seq_printf(m, "requests %llu\n", st->count);
	if (st->count) {
		seq_printf(m, "%-12s %11s %11s %11s %11s\n", "phase",
			   "last_us", "mean_us", "min_us", "max_us");
		for (i = 0; i < xt->nns; i++) {
			seq_printf(m, "%-12s", xt->names[i]);
			ane_xt_us(m, st->last.v[i]);
			ane_xt_us(m, div64_u64(st->sum.v[i], st->count));
			ane_xt_us(m, st->min.v[i]);
			ane_xt_us(m, st->max.v[i]);
			seq_putc(m, '\n');
		}
		for (; i < xt->n; i++)
			seq_printf(m, "%-12s %11llu %9llu.%u %11llu %11llu\n",
				   xt->names[i], st->last.v[i],
				   div64_u64(st->sum.v[i], st->count),
				   (unsigned int)(div64_u64(st->sum.v[i] * 10,
							    st->count) % 10),
				   st->min.v[i], st->max.v[i]);
	}
	kfree(st);
	return 0;
}

static inline int ane_xt_open(struct inode *inode, struct file *file)
{
	return single_open(file, ane_xt_show, inode->i_private);
}

static inline ssize_t ane_xt_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct ane_xt *xt = ((struct seq_file *)file->private_data)->private;

	ane_xt_snapshot(xt, NULL, true);
	return count;
}

/*
 * <parent>/<name>: the table above, mode 0600. The caller removes the
 * returned dentry before xt goes away.
 */
static inline struct dentry *ane_xt_debugfs(const char *name,
					    struct dentry *parent,
					    struct ane_xt *xt)
{
	static const struct file_operations fops = {
		.owner = THIS_MODULE,
		.open = ane_xt_open,
		.read = seq_read,
		.write = ane_xt_write,
		.llseek = seq_lseek,
		.release = single_release,
	};

	return debugfs_create_file(name, 0600, parent, xt, &fops);
}

#endif /* __ANE_EXEC_TIMING_H__ */
