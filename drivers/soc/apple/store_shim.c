/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Dj */
/*
 * Backing-store file access. In C because the O_* flags are macro-derived and
 * absent from the Rust bindings.
 *
 * These files are the driver's own. Opens run under kernel credentials and
 * resolve from the root of userspace init, so access never depends on which
 * task triggered the operation: a keyctl(2) call from an unprivileged process
 * reaches unseal in that process's context, and without this the driver could
 * not read its own root-only store.
 */

#include <linux/cred.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/slab.h>
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

	/*
	 * Resolve from the root of userspace init, which follows its
	 * switch_root. Kernel threads, init_task included, start in nullfs.
	 */
	get_fs_root(userspace_init_fs, &root);

	old = override_creds(kern);
	f = file_open_root(&root, path, flags, mode);
	put_cred(revert_creds(old));
	path_put(&root);

	return f;
}

/* Opens the store, creating if absent. Mode 0600: the file is the driver's own. */
void *sep_store_open(const char *path)
{
	return open_as_kernel(path, O_RDWR | O_CREAT | O_LARGEFILE, 0600);
}

/*
 * As sep_store_open(), but reports whether this call created the file, so that
 * only a new file is ever initialised. Returns an error pointer on failure.
 */
void *sep_store_open_host(const char *path, bool *created)
{
	struct file *f;

	*created = false;
	f = open_as_kernel(path, O_RDWR | O_CREAT | O_EXCL | O_LARGEFILE, 0600);
	if (PTR_ERR_OR_ZERO(f) == -EEXIST)
		return open_as_kernel(path, O_RDWR | O_LARGEFILE, 0);
	if (!IS_ERR(f))
		*created = true;
	return f;
}

/*
 * Opens truncated to empty, for callers that rewrite the whole file each time,
 * so a shorter record cannot leave stale trailing bytes from a larger one.
 */
void *sep_store_open_trunc(const char *path)
{
	return open_as_kernel(path, O_RDWR | O_CREAT | O_TRUNC | O_LARGEFILE, 0600);
}

/* Admission check only: never create or truncate an existing record. */
void *sep_store_open_writable_existing(const char *path)
{
	struct file *f = open_as_kernel(path, O_WRONLY | O_LARGEFILE, 0);

	return IS_ERR(f) ? NULL : f;
}

static int store_write_all(struct file *file, const void *buf, size_t len,
			   loff_t *pos)
{
	while (len) {
		ssize_t n = kernel_write(file, buf, len, pos);

		if (n < 0)
			return n;
		if (!n)
			return -EIO;
		buf = (const char *)buf + n;
		len -= n;
	}
	return 0;
}

/* Remove only the temporary dentry we opened, never a name reused by another task. */
static int store_remove_temp(struct file *dir, struct dentry *temp_dentry)
{
	struct dentry *dentry;
	int err;

	err = mnt_want_write(dir->f_path.mnt);
	if (err)
		return err;
	dentry = start_removing_dentry(dir->f_path.dentry, temp_dentry);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
	} else {
		err = security_path_unlink(&dir->f_path, dentry);
		if (!err)
			err = vfs_unlink(file_mnt_idmap(dir), file_inode(dir), dentry, NULL);
		end_removing(dentry);
	}
	mnt_drop_write(dir->f_path.mnt);
	return err;
}

/*
 * The ".<name>.new" names are reserved for this driver. The lock serializes
 * the driver's replacements, so among its writers only an interrupted
 * replacement leaves one behind.
 */
static DEFINE_MUTEX(store_replace_lock);

/*
 * Remove a regular file at the reserved temporary name. An entry that is
 * already gone counts as removed. Any non-regular entry is left in place and
 * the replacement fails with -EEXIST.
 */
static int store_remove_stale(struct file *dir, struct qstr *name)
{
	struct dentry *dentry;
	int err;

	err = mnt_want_write(dir->f_path.mnt);
	if (err)
		return err;
	dentry = start_removing(file_mnt_idmap(dir), dir->f_path.dentry, name);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry) == -ENOENT ? 0 : PTR_ERR(dentry);
	} else {
		if (!d_is_reg(dentry))
			err = -EEXIST;
		else
			err = security_path_unlink(&dir->f_path, dentry);
		if (!err)
			err = vfs_unlink(file_mnt_idmap(dir), file_inode(dir), dentry, NULL);
		end_removing(dentry);
	}
	mnt_drop_write(dir->f_path.mnt);
	return err;
}

/*
 * Static parent directory capabilities required by atomic replacement:
 * directory fsync for rename durability and rename itself.
 */
static int store_dir_capable(struct file *dir)
{
	if (!dir->f_op || !dir->f_op->fsync)
		return -EINVAL;
	if (!file_inode(dir)->i_op || !file_inode(dir)->i_op->rename)
		return -EPERM;
	return 0;
}

/*
 * O_CREAT applies the calling task's umask, so a restrictive one can leave the
 * temporary without the driver's own access. Set @mode explicitly then.
 */
static int store_ensure_mode(struct file *dir, struct file *temp, umode_t mode)
{
	struct inode *inode = file_inode(temp);
	struct iattr attr;
	int err;

	if ((inode->i_mode & S_IALLUGO) == (mode & S_IALLUGO))
		return 0;
	err = mnt_want_write(dir->f_path.mnt);
	if (err)
		return err;
	inode_lock(inode);
	if ((inode->i_mode & S_IALLUGO) != (mode & S_IALLUGO)) {
		attr = (struct iattr){
			.ia_valid = ATTR_MODE | ATTR_CTIME,
			.ia_mode = (inode->i_mode & ~S_IALLUGO) | (mode & S_IALLUGO),
		};
		err = notify_change(file_mnt_idmap(dir), temp->f_path.dentry,
				    &attr, NULL);
	}
	inode_unlock(inode);
	mnt_drop_write(dir->f_path.mnt);
	return err;
}

/*
 * Durable full-file replacement, without truncating the previous generation.
 * The caller owns both byte slices until return. Only the final pathname is
 * replaced; symlinks and non-regular destinations are refused under rename's
 * directory lock. A successful rename is the visibility commit point: a later
 * directory fsync failure returns an error but cannot safely undo that rename.
 *
 * The temporary name is fixed per destination (".<name>.new") so that policy
 * can label it by name at creation, as it labels the destination: rename keeps
 * the temporary inode's label.
 */
static int store_replace(const char *path, const void *head, size_t head_len,
			 const void *body, size_t body_len)
{
	const char *base = strrchr(path, '/');
	const struct cred *old;
	struct cred *kern;
	struct file *dir, *temp = NULL;
	struct dentry *temp_dentry;
	struct path root;
	struct renamedata rd = {};
	struct qstr name, stale;
	char *parent;
	char temporary[64];
	loff_t pos = 0;
	bool renamed = false;
	int err, cleanup, attempt;

	if (!base || base == path || !base[1])
		return -EINVAL;
	if (snprintf(temporary, sizeof(temporary), ".%s.new", base + 1) >=
	    (int)sizeof(temporary))
		return -ENAMETOOLONG;
	parent = kstrndup(path, base - path, GFP_KERNEL);
	if (!parent)
		return -ENOMEM;
	kern = prepare_kernel_cred(&init_task);
	if (!kern) {
		kfree(parent);
		return -ENOMEM;
	}
	/* Same root as open_as_kernel(): userspace init's, not nullfs. */
	get_fs_root(userspace_init_fs, &root);
	old = override_creds(kern);
	dir = file_open_root(&root, parent, O_RDONLY | O_DIRECTORY | O_LARGEFILE, 0);
	path_put(&root);
	kfree(parent);
	if (IS_ERR(dir)) {
		err = PTR_ERR(dir);
		goto out_creds;
	}
	err = store_dir_capable(dir);
	if (err)
		goto out_dir;

	mutex_lock(&store_replace_lock);
	/*
	 * Exclusive creation never adopts or truncates a previous temporary file:
	 * a leftover is unlinked and the name created again.
	 */
	for (attempt = 0; attempt < 2; attempt++) {
		temp = file_open_root(&dir->f_path, temporary,
			O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_LARGEFILE, 0600);
		if (PTR_ERR_OR_ZERO(temp) != -EEXIST || attempt)
			break;
		stale = (struct qstr)QSTR_INIT(temporary, strlen(temporary));
		err = store_remove_stale(dir, &stale);
		if (err) {
			temp = ERR_PTR(err);
			break;
		}
	}
	if (IS_ERR(temp)) {
		err = PTR_ERR(temp);
		temp = NULL;
		goto out_unlock;
	}
	temp_dentry = dget(temp->f_path.dentry);
	err = store_ensure_mode(dir, temp, 0600);
	if (!err)
		err = store_write_all(temp, head, head_len, &pos);
	if (!err)
		err = store_write_all(temp, body, body_len, &pos);
	if (!err)
		err = vfs_fsync(temp, 0);
	if (err)
		goto out_temp;
	/* Close before rename, so that a flush error leaves the old generation. */
	err = filp_close(temp, NULL);
	temp = NULL;
	if (err)
		goto out_temp;

	/* Do not hold a mount write claim across kernel_write's own freeze lock. */
	err = mnt_want_write(dir->f_path.mnt);
	if (err)
		goto out_temp;
	rd.mnt_idmap = file_mnt_idmap(dir);
	rd.old_parent = dir->f_path.dentry;
	rd.new_parent = dir->f_path.dentry;
	name = (struct qstr)QSTR_INIT(base + 1, strlen(base + 1));
	err = start_renaming_dentry(&rd, 0, temp_dentry, &name);
	if (!err) {
		if (d_is_positive(rd.new_dentry) && !d_is_reg(rd.new_dentry))
			err = -EINVAL;
		else
			err = security_path_rename(&dir->f_path, rd.old_dentry,
						   &dir->f_path, rd.new_dentry, 0);
		if (!err)
			err = vfs_rename(&rd);
		end_renaming(&rd);
	}
	mnt_drop_write(dir->f_path.mnt);
	if (!err) {
		renamed = true;
		err = vfs_fsync(dir, 0);
		if (err)
			pr_err("apple-sep: replacement visible, parent sync failed (%d)\n",
			       err);
	}
out_temp:
	if (temp)
		filp_close(temp, NULL);
	if (!renamed) {
		cleanup = store_remove_temp(dir, temp_dentry);
		if (cleanup)
			pr_warn("apple-sep: temporary store cleanup failed (%d)\n", cleanup);
	}
	dput(temp_dentry);
out_unlock:
	mutex_unlock(&store_replace_lock);
out_dir:
	fput(dir);
out_creds:
	put_cred(revert_creds(old));
	return err;
}

int sep_store_replace(const char *path, const void *head, size_t head_len,
		      const void *body, size_t body_len)
{
	return store_replace(path, head, head_len, body, body_len);
}

/*
 * Check whether an entry at the reserved temporary name can be unlinked by
 * store_remove_stale() without modifying it. Runs the path unlink hook;
 * vfs_unlink() still enforces security_inode_unlink() and filesystem checks.
 */
static int store_check_reserved(struct file *dir, struct qstr *name)
{
	struct dentry *dentry;
	int err;

	dentry = start_removing(file_mnt_idmap(dir), dir->f_path.dentry, name);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry) == -ENOENT ? 0 : PTR_ERR(dentry);

	if (!d_is_reg(dentry))
		err = -EEXIST;
	else if (d_mountpoint(dentry))
		err = -EBUSY;
	else if (!file_inode(dir)->i_op || !file_inode(dir)->i_op->unlink)
		err = -EPERM;
	else
		err = may_delete_dentry(file_mnt_idmap(dir), file_inode(dir),
					dentry, false);
	if (!err)
		err = security_path_unlink(&dir->f_path, dentry);
	end_removing(dentry);
	return err;
}

/*
 * Whether store_replace() can publish at @path, for callers about to make a
 * change the enclave cannot undo. Resolves from userspace init's root under
 * kernel credentials, like store_replace(). Checks the parent mount, creator
 * ID mapping for the temporary inode, parent directory, any existing
 * destination (required when @require_existing is set) via may_delete_dentry(),
 * and whether any entry at the reserved temporary name passes the preflight
 * unlink checks.
 *
 * Mountpoints are rejected with d_mountpoint(), which catches mounts reached
 * via any alias in this namespace and conservatively also refuses a destination
 * bind-mounted only in another mount namespace. Binding the parent directory
 * itself is allowed when the file inside it is replaceable. Later races, I/O
 * errors, or create/unlink/rename filesystem and LSM denials remain possible.
 */
static int store_check_publish(const char *path, bool require_existing)
{
	const char *base = strrchr(path, '/');
	const struct cred *old;
	struct cred *kern;
	struct file *dir;
	struct dentry *dentry;
	struct path root;
	struct qstr name, stale;
	char *parent;
	char temporary[64];
	int err;

	if (!base || base == path || !base[1])
		return -EINVAL;
	if (snprintf(temporary, sizeof(temporary), ".%s.new", base + 1) >=
	    (int)sizeof(temporary))
		return -ENAMETOOLONG;
	parent = kstrndup(path, base - path, GFP_KERNEL);
	if (!parent)
		return -ENOMEM;
	kern = prepare_kernel_cred(&init_task);
	if (!kern) {
		kfree(parent);
		return -ENOMEM;
	}
	get_fs_root(userspace_init_fs, &root);
	old = override_creds(kern);
	dir = file_open_root(&root, parent, O_RDONLY | O_DIRECTORY | O_LARGEFILE, 0);
	path_put(&root);
	kfree(parent);
	if (IS_ERR(dir)) {
		err = PTR_ERR(dir);
		goto out_creds;
	}
	err = store_dir_capable(dir);
	if (err)
		goto out_dir;

	mutex_lock(&store_replace_lock);
	err = mnt_want_write(dir->f_path.mnt);
	if (err)
		goto out_unlock;
	if (!fsuidgid_has_mapping(dir->f_path.dentry->d_sb,
				  file_mnt_idmap(dir))) {
		err = -EOVERFLOW;
		goto out_mnt;
	}

	name = (struct qstr)QSTR_INIT(base + 1, strlen(base + 1));
	dentry = start_removing(file_mnt_idmap(dir), dir->f_path.dentry, &name);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		if (err == -ENOENT && !require_existing) {
			inode_lock_shared(file_inode(dir));
			err = inode_permission(file_mnt_idmap(dir),
					       file_inode(dir),
					       MAY_WRITE | MAY_EXEC);
			if (!err && IS_APPEND(file_inode(dir)))
				err = -EPERM;
			if (!err && IS_DEADDIR(file_inode(dir)))
				err = -ENOENT;
			inode_unlock_shared(file_inode(dir));
		}
	} else {
		if (!d_is_reg(dentry))
			err = -EINVAL;
		else if (d_mountpoint(dentry))
			err = -EBUSY;
		else
			err = may_delete_dentry(file_mnt_idmap(dir),
						file_inode(dir), dentry, false);
		end_removing(dentry);
	}
	if (!err) {
		stale = (struct qstr)QSTR_INIT(temporary, strlen(temporary));
		err = store_check_reserved(dir, &stale);
	}

out_mnt:
	mnt_drop_write(dir->f_path.mnt);
out_unlock:
	mutex_unlock(&store_replace_lock);
out_dir:
	fput(dir);
out_creds:
	put_cred(revert_creds(old));
	return err;
}

int sep_store_replaceable(const char *path)
{
	return store_check_publish(path, true);
}

int sep_store_publishable(const char *path)
{
	return store_check_publish(path, false);
}

/*
 * Opens an existing file read-only, returning an error pointer on failure.
 * O_RDONLY and no O_CREAT: the seed cannot be created, truncated or written.
 */
void *sep_store_open_ro(const char *path)
{
	return open_as_kernel(path, O_RDONLY | O_LARGEFILE, 0);
}

void *sep_store_open_block(const char *path, int writable)
{
	struct file *f = open_as_kernel(path,
					writable ? O_RDWR | O_LARGEFILE : O_RDONLY | O_LARGEFILE,
					0);

	if (IS_ERR(f))
		return NULL;
	if (!S_ISBLK(file_inode(f)->i_mode) ||
	    (writable && bdev_read_only(file_bdev(f)))) {
		filp_close(f, NULL);
		return NULL;
	}
	return f;
}

/* The platform driver of the SoC's embedded (ANS) NVMe controller. */
#define SEP_STORE_ANS_DRIVER "nvme-apple"

/*
 * Whether @dev, a disk's device, sits below the embedded NVMe controller. The
 * first platform-bus ancestor must be bound to the ANS driver: an NVMe
 * namespace's parent is its controller, whose parent is the ANS platform
 * device. A disk behind USB, PCIe/Thunderbolt or a loop device reaches a
 * different platform device, or none, and is refused.
 */
static bool sep_store_dev_is_internal(const struct device *dev)
{
	for (dev = dev->parent; dev; dev = dev->parent) {
		const struct device_driver *drv;

		if (!dev_is_platform(dev))
			continue;
		drv = READ_ONCE(dev->driver);
		return drv && !strcmp(drv->name, SEP_STORE_ANS_DRIVER);
	}
	return false;
}

/*
 * As sep_store_open_block(), for the enclave's own store: the path is only a
 * name for the partition, so the device it resolves to must also be on the
 * internal NVMe disk.
 */
void *sep_store_open_internal_block(const char *path, int writable)
{
	struct file *f = sep_store_open_block(path, writable);

	if (f && !sep_store_dev_is_internal(disk_to_dev(file_bdev(f)->bd_disk))) {
		pr_warn_ratelimited("apple-sep: %s is not on the internal NVMe disk; not used\n",
				    path);
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

/*
 * Flushes the directory that holds an open store file. fsync() of the file
 * itself is not guaranteed to make its name in that directory durable, so a
 * file created this boot could be missing after a crash. Opened under kernel
 * credentials, as the file was.
 */
int sep_store_sync_parent(void *handle)
{
	struct file *f = handle;
	struct cred *kern;
	struct file *dir;
	struct path parent;
	int ret;

	kern = prepare_kernel_cred(&init_task);
	if (!kern)
		return -ENOMEM;

	/* The file holds its mount; the parent dentry is pinned until dput(). */
	parent.mnt = f->f_path.mnt;
	parent.dentry = dget_parent(f->f_path.dentry);
	dir = dentry_open(&parent, O_RDONLY | O_DIRECTORY, kern);
	dput(parent.dentry);
	put_cred(kern);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	ret = vfs_fsync(dir, 0);
	fput(dir);
	return ret;
}

/* Like the module's Rust tests, which need KUnit built in. */
#if IS_ENABLED(CONFIG_APPLE_SEP_KUNIT_TEST) && IS_BUILTIN(CONFIG_KUNIT)
#include "store_shim_test.c"
#endif
