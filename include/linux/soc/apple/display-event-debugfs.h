/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_APPLE_DISPLAY_EVENT_DEBUGFS_H
#define _LINUX_APPLE_DISPLAY_EVENT_DEBUGFS_H
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/soc/apple/display-event-log.h>

static int apple_display_events_show(struct seq_file *s, void *unused)
{
	const struct apple_display_event_snapshot *p = s->private;
	u32 i, j;

	seq_printf(s, "apple-display-events version=1 name=%s capacity=512 clock=mono_fast\n", p->name);
	seq_printf(s, "snapshot ns=%llu/%llu seq=%llu/%llu count=%u copied=%u gaps=%u complete=%u\n",
		p->start_ns, p->end_ns, p->seq_start, p->seq_end,
		p->record_count, p->copied, p->gaps, p->complete);
	seq_printf(s, "before committed=%llu attempts=%llu busy=%llu sequence=%llu overwritten=%llu\n",
		p->start.committed, p->start.attempts, p->start.busy_drops,
		p->start.sequence_drops, p->start.overwritten);
	seq_printf(s, "after committed=%llu attempts=%llu busy=%llu sequence=%llu overwritten=%llu\n",
		p->end.committed, p->end.attempts, p->end.busy_drops,
		p->end.sequence_drops, p->end.overwritten);
	for (i = 0; i < p->record_count; i++) {
		const struct apple_display_event_record *r = &p->records[i];

		if (!p->valid[i]) {
			seq_printf(s, "gap index=%u\n", i);
			continue;
		}
		seq_printf(s, "event seq=%llu ns=%llu type=%u flags=%#x result=%d data=",
			r->sequence, r->timestamp_ns, r->type, r->flags, r->result);
		for (j = 0; j < 9; j++)
			seq_printf(s, "%s%016llx", j ? "," : "", r->data[j]);
		seq_putc(s, '\n');
	}
	return 0;
}

static int apple_display_events_open(struct inode *inode, struct file *file)
{
	struct apple_display_event_snapshot *snapshot;
	struct dentry *dentry = file->f_path.dentry;
	int ret;

	/* No owner pointer is read until debugfs removal protection is held. */
	snapshot = kvzalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;
	ret = debugfs_file_get(dentry);
	if (ret)
		goto free;
	ret = apple_display_event_snapshot_copy(inode->i_private, snapshot);
	debugfs_file_put(dentry);
	if (ret)
		goto free;
	/* Only standalone bytes remain: formatting may outlive the device. */
	ret = single_open(file, apple_display_events_show, snapshot);
	if (ret)
		goto free;
	return 0;
free:
	kvfree(snapshot);
	return ret;
}

static int apple_display_events_release(struct inode *inode, struct file *file)
{
	struct seq_file *s = file->private_data;

	kvfree(s->private);
	return single_release(inode, file);
}

static const struct file_operations apple_display_events_fops = {
	.owner = THIS_MODULE,
	.open = apple_display_events_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = apple_display_events_release,
};
#endif
