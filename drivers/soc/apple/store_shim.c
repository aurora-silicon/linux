/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Dj */
/*
 * Backing-store file access. In C because the O_* flags are macro-derived and
 * absent from the Rust bindings.
 *
 * These files are the driver's own. Opens run under kernel credentials so
 * access never depends on which task triggered the operation: a keyctl(2) call
 * from an unprivileged process reaches unseal in that process's context, and
 * without this the driver could not read its own root-only store.
 */

#include <linux/cred.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/sched.h>
#include <linux/types.h>

#include "shim.h"

static struct file *open_as_kernel(const char *path, int flags, umode_t mode)
{
	const struct cred *old;
	struct cred *kern;
	struct file *f;
	struct path root;

	kern = prepare_kernel_cred(&init_task);
	if (!kern)
		return ERR_PTR(-ENOMEM);

	task_lock(&init_task);
	get_fs_root(init_task.fs, &root);
	task_unlock(&init_task);

	old = override_creds(kern);
	f = file_open_root(&root, path, flags, mode);
	put_cred(revert_creds(old));
	path_put(&root);

	return f;
}

/* Opens the store, creating if absent. Mode 0600: the file is the driver's own. */
void *sep_store_open(const char *path)
{
	struct file *f = open_as_kernel(path, O_RDWR | O_CREAT | O_LARGEFILE, 0600);

	return IS_ERR(f) ? NULL : f;
}

/*
 * Opens truncated to empty, for callers that rewrite the whole file each time,
 * so a shorter record cannot leave stale trailing bytes from a larger one.
 */
void *sep_store_open_trunc(const char *path)
{
	struct file *f = open_as_kernel(path, O_RDWR | O_CREAT | O_TRUNC | O_LARGEFILE, 0600);

	return IS_ERR(f) ? NULL : f;
}

/*
 * Opens an existing file read-only, NULL if absent. O_RDONLY and no O_CREAT so
 * the seed cannot be created, truncated or written by mistake.
 */
void *sep_store_open_ro(const char *path)
{
	struct file *f = open_as_kernel(path, O_RDONLY | O_LARGEFILE, 0);

	return IS_ERR(f) ? NULL : f;
}

void *sep_store_open_block(const char *path, int writable)
{
	struct file *f = open_as_kernel(path,
					writable ? O_RDWR | O_LARGEFILE : O_RDONLY | O_LARGEFILE,
					0);

	if (IS_ERR(f))
		return NULL;
	if (!S_ISBLK(file_inode(f)->i_mode) ||
	    bdev_read_only(file_bdev(f)) == !!writable) {
		filp_close(f, NULL);
		return NULL;
	}
	return f;
}

void sep_store_close(void *handle)
{
	if (handle)
		filp_close((struct file *)handle, NULL);
}

long long sep_store_size(void *handle)
{
	struct file *f = handle;

	if (S_ISBLK(file_inode(f)->i_mode))
		return bdev_nr_bytes(file_bdev(f));
	return i_size_read(file_inode(f));
}

long sep_store_read(void *handle, long long off, void *buf, size_t len)
{
	struct file *f = handle;
	loff_t pos = off;

	return kernel_read(f, buf, len, &pos);
}

long sep_store_write(void *handle, long long off, const void *buf,
			    size_t len)
{
	struct file *f = handle;
	loff_t pos = off;

	return kernel_write(f, buf, len, &pos);
}

int sep_store_sync(void *handle)
{
	return vfs_fsync((struct file *)handle, 0);
}
