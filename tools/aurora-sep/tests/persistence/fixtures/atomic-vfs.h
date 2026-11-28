/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Offline VFS boundary fixture. Production replacement functions are appended unchanged. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define GFP_KERNEL 0
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)(-4095))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define PTR_ERR_OR_ZERO(p) (IS_ERR(p) ? PTR_ERR(p) : 0)
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define QSTR_INIT(n, l) { .name = (n), .len = (l) }
#define CAP ((1U << 20) + 32)
#define MAY_EXEC 0x1
#define MAY_WRITE 0x2
#define S_APPEND 0x1
#define S_DEAD 0x2
#define IS_APPEND(inode) dir_flag(inode, S_APPEND)
#define IS_DEADDIR(inode) dir_flag(inode, S_DEAD)
#define umode_t unsigned int
#ifndef S_IFREG
#define S_IFREG 0100000
#endif
#define S_IALLUGO 07777
#define ATTR_MODE (1 << 0)
#define ATTR_CTIME (1 << 6)
struct iattr {
	unsigned int ia_valid;
	umode_t ia_mode;
};
struct super_block {
	int unused;
};
struct file_operations {
	int (*fsync)(void);
};
struct inode_operations {
	int (*rename)(void);
	int (*unlink)(void);
};
struct inode {
	int type;
	umode_t i_mode;
	unsigned int i_flags;
	const struct inode_operations *i_op;
};
struct dentry {
	struct inode inode;
	struct inode *parent;
	struct super_block *d_sb;
	bool linked;
	bool mountpoint;
	int may_delete_err;
};
struct path {
	struct dentry *dentry;
	void *mnt;
};
struct file {
	struct path f_path;
	const struct file_operations *f_op;
	unsigned char bytes[CAP];
	size_t len;
	int refs;
};
struct cred {
	int unused;
};
struct qstr {
	const char *name;
	size_t len;
};
struct renamedata {
	void *mnt_idmap;
	struct dentry *old_parent, *old_dentry, *new_parent, *new_dentry;
	void *delegated_inode;
	unsigned int flags;
};
enum {
	SLOT_USER = 0,
	SLOT_MASTER = 1,
	SLOT_OWNER = 2,
	SLOT_KEYBAG = 3,
	SLOT_COUNT = 4
};
struct catacomb_slot {
	const char *name;
	const char *temp;
	struct file file;
	struct dentry dentry;
	struct dentry stale_dentry;
	bool exists;
	bool stale_present;
	int type;
	bool mountpoint;
	int may_delete_err;
	int write_open_err;
	int stale_type;
	bool stale_mountpoint;
	int stale_may_delete_err;
	int stale_security_err;
	int stale_unlink_err;
};
static struct {
	void *fs;
} init_task;
static struct cred credential;
static struct file directory, temporary_file;
static struct catacomb_slot slots[SLOT_COUNT];
static struct catacomb_slot *active_slot;
static struct dentry root_dentry;
static struct super_block superblock_token;
static int mount_token, idmap_token;
static int stages[36], mounts, locks, credentials, roots, allocations;
static int stub_op(void)
{
	return 0;
}
static const struct file_operations dir_f_op_ok = { .fsync = stub_op };
static const struct file_operations dir_f_op_none = {};
static const struct inode_operations dir_i_op_ok = {
	.rename = stub_op,
	.unlink = stub_op,
};
static const struct inode_operations dir_i_op_no_rename = {
	.unlink = stub_op,
};
static const struct inode_operations dir_i_op_no_unlink = {
	.rename = stub_op,
};
static const struct inode_operations dir_i_op_none = {};
static int attempts, unlinks, renames, temp_syncs, dir_syncs, temp_present,
	collision_count, stale_unlinks, replace_locked;
static int temp_dentry_refs, dir_dentry_refs, temp_inode_locks,
	dir_shared_locks, chmod_calls, read_calls;
static bool temp_closing, stale_vanish;
static umode_t caller_umask;
/* The reserved name store_replace() last tried to create exclusively. */
static char temp_name[64];
static int cleanup_warning, commit_warning, write_error, zero_write,
	write_calls;
static size_t write_at, short_limit;
static bool publish_at_rename;
static char events[1024];
static void event(char c)
{
	size_t n = strlen(events);

	assert(n + 1 < sizeof(events));
	events[n] = c;
	events[n + 1] = 0;
}
enum {
	ALLOC = 1,
	CRED,
	PARENT_OPEN,
	TEMP_OPEN,
	MOUNT,
	RENAME_LOCK,
	SECURITY_RENAME,
	RENAME,
	TEMP_SYNC,
	DIR_SYNC,
	CLEANUP_LOCK,
	SECURITY_UNLINK,
	UNLINK,
	TARGET_TYPE,
	STALE_SOURCE,
	READ_OPEN,
	STALE_REMOVE,
	STALE_SECURITY,
	STALE_UNLINK,
	STALE_TYPE,
	DEST_LOOKUP,
	DEST_MOUNT,
	DEST_TYPE,
	DIR_PERM,
	WRITE_OPEN,
	DIR_APPEND,
	DIR_DEAD,
	DEST_MAY_DELETE,
	HOST_OPEN,
	CREATOR_MAP,
	DIR_NO_FSYNC,
	DIR_NO_RENAME,
	DIR_NO_UNLINK,
	TEMP_CLOSE,
	TEMP_CHMOD
};
static void refresh_dir_ops(void)
{
	directory.f_op = stages[DIR_NO_FSYNC] ? &dir_f_op_none : &dir_f_op_ok;
	if (stages[DIR_NO_RENAME] && stages[DIR_NO_UNLINK])
		directory.f_path.dentry->inode.i_op = &dir_i_op_none;
	else if (stages[DIR_NO_RENAME])
		directory.f_path.dentry->inode.i_op = &dir_i_op_no_rename;
	else if (stages[DIR_NO_UNLINK])
		directory.f_path.dentry->inode.i_op = &dir_i_op_no_unlink;
	else
		directory.f_path.dentry->inode.i_op = &dir_i_op_ok;
}
static const char *const slot_names[SLOT_COUNT] = {
	"apple-sep-catacomb-user.bin",
	"apple-sep-catacomb-master.bin",
	"apple-sep-catacomb-owner.bin",
	"aurora-sep-keybag.bin",
};
static const char *const slot_temps[SLOT_COUNT] = {
	".apple-sep-catacomb-user.bin.new",
	".apple-sep-catacomb-master.bin.new",
	".apple-sep-catacomb-owner.bin.new",
	".aurora-sep-keybag.bin.new",
};
static struct catacomb_slot *slot_for_base(const char *base)
{
	for (int i = 0; i < SLOT_COUNT; i++) {
		if (!strcmp(base, slots[i].name))
			return &slots[i];
	}
	return NULL;
}
static struct catacomb_slot *slot_for_temp(const char *temp)
{
	for (int i = 0; i < SLOT_COUNT; i++) {
		if (!strcmp(temp, slots[i].temp))
			return &slots[i];
	}
	return NULL;
}
static struct catacomb_slot *slot_for_path(const char *path)
{
	const char *base = strrchr(path, '/');

	assert(base && base != path && base[1]);
	struct catacomb_slot *slot = slot_for_base(base + 1);

	assert(slot);
	return slot;
}
static struct catacomb_slot *slot_for_file(struct file *f)
{
	for (int i = 0; i < SLOT_COUNT; i++) {
		if (f == &slots[i].file)
			return &slots[i];
	}
	return NULL;
}
static struct catacomb_slot *slot_for_stale_dentry(struct dentry *d)
{
	for (int i = 0; i < SLOT_COUNT; i++) {
		if (d == &slots[i].stale_dentry)
			return &slots[i];
	}
	return NULL;
}
static struct catacomb_slot *slot_for_dentry(struct dentry *d)
{
	for (int i = 0; i < SLOT_COUNT; i++) {
		if (d == &slots[i].dentry)
			return &slots[i];
	}
	return NULL;
}
void fixture_reset(void)
{
	memset(&directory, 0, sizeof(directory));
	memset(&temporary_file, 0, sizeof(temporary_file));
	memset(slots, 0, sizeof(slots));
	active_slot = NULL;
	temp_name[0] = 0;
	directory.f_path.mnt = &mount_token;
	directory.f_op = &dir_f_op_ok;
	directory.f_path.dentry = calloc(1, sizeof(struct dentry));
	assert(directory.f_path.dentry);
	directory.f_path.dentry->inode.type = 2;
	directory.f_path.dentry->inode.i_op = &dir_i_op_ok;
	directory.f_path.dentry->d_sb = &superblock_token;
	temporary_file.f_path.dentry = calloc(1, sizeof(struct dentry));
	assert(temporary_file.f_path.dentry);
	temporary_file.f_path.dentry->inode.type = 1;
	temporary_file.f_path.mnt = &mount_token;
	for (int i = 0; i < SLOT_COUNT; i++) {
		slots[i].name = slot_names[i];
		slots[i].temp = slot_temps[i];
		slots[i].file.f_path.mnt = &mount_token;
		slots[i].file.f_path.dentry = &slots[i].dentry;
		slots[i].type = 1;
		slots[i].stale_type = 1;
		slots[i].dentry.inode.type = 1;
		slots[i].stale_dentry.inode.type = 1;
	}
	slots[SLOT_KEYBAG].exists = true;
	memset(stages, 0, sizeof(stages));
	mounts = locks = credentials = roots = allocations = 0;
	attempts = unlinks = renames = temp_syncs = dir_syncs = temp_present =
		collision_count = stale_unlinks = replace_locked = 0;
	cleanup_warning = commit_warning = write_error = zero_write =
		write_calls = 0;
	temp_dentry_refs = dir_dentry_refs = temp_inode_locks =
		dir_shared_locks = chmod_calls = read_calls = 0;
	temp_closing = stale_vanish = false;
	caller_umask = 022;
	write_at = short_limit = 0;
	publish_at_rename = false;
	events[0] = 0;
}
void fixture_done(void)
{
	assert(!mounts && !locks && !credentials && !roots && !allocations);
	assert(!replace_locked);
	assert(!temp_dentry_refs && !dir_dentry_refs);
	assert(!temp_inode_locks && !dir_shared_locks && !temp_closing);
	assert(!directory.refs && !temporary_file.refs);
	for (int i = 0; i < SLOT_COUNT; i++)
		assert(!slots[i].file.refs);
	free(directory.f_path.dentry);
	free(temporary_file.f_path.dentry);
}
void fixture_set_slot(int idx, const void *p, size_t n)
{
	assert(idx >= 0 && idx < SLOT_COUNT && n <= CAP);
	memcpy(slots[idx].file.bytes, p, n);
	slots[idx].file.len = n;
	slots[idx].exists = true;
}
void fixture_set(const void *p, size_t n)
{
	fixture_set_slot(SLOT_USER, p, n);
}
void fixture_slot_mountpoint(int idx, int on)
{
	assert(idx >= 0 && idx < SLOT_COUNT);
	slots[idx].mountpoint = on != 0;
}
void fixture_slot_type(int idx, int type)
{
	assert(idx >= 0 && idx < SLOT_COUNT);
	slots[idx].type = type;
}
void fixture_slot_may_delete(int idx, int err)
{
	assert(idx >= 0 && idx < SLOT_COUNT);
	slots[idx].may_delete_err = err;
}
void fixture_slot_write_open(int idx, int err)
{
	assert(idx >= 0 && idx < SLOT_COUNT);
	slots[idx].write_open_err = err;
}
void fixture_slot_stale(int idx, int present, int type, int mountpoint,
			int may_delete_err, int security_err, int unlink_err)
{
	assert(idx >= 0 && idx < SLOT_COUNT);
	slots[idx].stale_present = present != 0;
	slots[idx].stale_type = type ? type : 1;
	slots[idx].stale_mountpoint = mountpoint != 0;
	slots[idx].stale_may_delete_err = may_delete_err;
	slots[idx].stale_security_err = security_err;
	slots[idx].stale_unlink_err = unlink_err;
}
void fixture_race_publication(void)
{
	publish_at_rename = true;
}
/* The calling task's umask, which O_CREAT applies to the temporary. */
void fixture_umask(umode_t mask)
{
	caller_umask = mask & 0777;
}
/* The leftover temporary disappears between the -EEXIST and its removal. */
void fixture_stale_vanish(void)
{
	stale_vanish = true;
}
size_t fixture_get(void *p, size_t n)
{
	if (!slots[SLOT_USER].exists)
		return 0;
	assert(n >= slots[SLOT_USER].file.len);
	memcpy(p, slots[SLOT_USER].file.bytes, slots[SLOT_USER].file.len);
	return slots[SLOT_USER].file.len;
}
void fixture_fault(int stage, int error)
{
	assert(stage > 0 && stage < 36);
	stages[stage] = error;
	if (stage == DIR_APPEND) {
		if (error)
			directory.f_path.dentry->inode.i_flags |= S_APPEND;
		else
			directory.f_path.dentry->inode.i_flags &= ~S_APPEND;
	} else if (stage == DIR_DEAD) {
		if (error)
			directory.f_path.dentry->inode.i_flags |= S_DEAD;
		else
			directory.f_path.dentry->inode.i_flags &= ~S_DEAD;
	} else if (stage == DIR_NO_FSYNC || stage == DIR_NO_RENAME ||
		   stage == DIR_NO_UNLINK) {
		refresh_dir_ops();
	}
}
void fixture_write_fault(size_t at, int error, int zero)
{
	write_at = at;
	write_error = error;
	zero_write = zero;
}
void fixture_short(size_t n)
{
	short_limit = n;
}
void fixture_collisions(int n)
{
	collision_count = n;
}
int fixture_count(int n)
{
	switch (n) {
	case 0:
		return renames;
	case 1:
		return temp_syncs;
	case 2:
		return dir_syncs;
	case 3:
		return temp_present;
	case 4:
		return unlinks;
	case 5:
		return commit_warning;
	case 6:
		return cleanup_warning;
	case 7:
		return attempts;
	case 8:
		return stale_unlinks;
	case 9:
		return read_calls;
	case 10:
		return chmod_calls;
	default:
		abort();
	}
}
const char *fixture_events(void)
{
	return events;
}
static void *kstrndup(const char *p, size_t n, int g)
{
	assert(g == GFP_KERNEL);
	if (stages[ALLOC])
		return NULL;
	char *s = malloc(n + 1);

	assert(s);
	memcpy(s, p, n);
	s[n] = 0;
	allocations++;
	return s;
}
static void kfree(void *p)
{
	free(p);
	allocations--;
	assert(allocations >= 0);
}
static struct cred *prepare_kernel_cred(void *task)
{
	assert(task == &init_task);
	return stages[CRED] ? NULL : &credential;
}
static struct { int token; } userspace_init_fs_token;
#define userspace_init_fs (&userspace_init_fs_token)
static void get_fs_root(void *fs, struct path *p)
{
	/* init_task.fs is the kernel's private nullfs; /var/lib is not there. */
	assert(fs == userspace_init_fs);
	*p = (struct path){ &root_dentry, &mount_token };
	roots++;
}
static void path_put(struct path *p)
{
	assert(p->dentry == &root_dentry);
	roots--;
}
static const struct cred *override_creds(struct cred *p)
{
	assert(p == &credential);
	credentials++;
	return &credential;
}
static struct cred *revert_creds(const struct cred *p)
{
	assert(p == &credential);
	credentials--;
	return &credential;
}
static void put_cred(struct cred *p)
{
	assert(p == &credential);
}
static struct file *file_open_root(const struct path *p, const char *name,
				   int flags, umode_t mode)
{
	assert(credentials == 1);
	if (p->dentry == &root_dentry) {
		assert(!strcmp(name, "/var/lib"));
		assert(flags == (O_RDONLY | O_DIRECTORY | O_LARGEFILE) &&
		       mode == 0);
		if (stages[PARENT_OPEN])
			return ERR_PTR(-stages[PARENT_OPEN]);
		directory.refs++;
		return &directory;
	}
	assert(p == &directory.f_path);
	assert(flags ==
	       (O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_LARGEFILE));
	assert(mode == 0600);
	assert(replace_locked);
	struct catacomb_slot *slot = slot_for_temp(name);

	assert(slot && slot != &slots[SLOT_KEYBAG]);
	assert(strlen(name) < sizeof(temp_name));
	strcpy(temp_name, name);
	active_slot = slot;
	attempts++;
	if (collision_count > 0) {
		collision_count--;
		slot->stale_present = true;
	}
	if (slot->stale_present)
		return ERR_PTR(-EEXIST);
	if (stages[CREATOR_MAP])
		return ERR_PTR(-EOVERFLOW);
	if (stages[TEMP_OPEN])
		return ERR_PTR(-stages[TEMP_OPEN]);
	assert(!temp_present);
	temp_present = 1;
	temporary_file.refs++;
	temporary_file.f_path.dentry->linked = true;
	temporary_file.f_path.dentry->inode.i_mode =
		S_IFREG | (mode & ~caller_umask);
	temporary_file.len = 0;
	event('C');
	return &temporary_file;
}
#define DEFINE_MUTEX(name) int name
static int store_replace_lock;
static void mutex_lock(int *m)
{
	assert(m == &store_replace_lock && !replace_locked);
	replace_locked = 1;
}
static void mutex_unlock(int *m)
{
	assert(m == &store_replace_lock && replace_locked);
	replace_locked = 0;
	temp_name[0] = 0;
	active_slot = NULL;
}
static struct inode *file_inode(struct file *f)
{
	return &f->f_path.dentry->inode;
}
/* Directory flags are read only under the directory's lock. */
static bool dir_flag(struct inode *inode, unsigned int flag)
{
	if (inode == file_inode(&directory))
		assert(locks > 0 || dir_shared_locks > 0);
	return inode->i_flags & flag;
}
static void inode_lock(struct inode *inode)
{
	assert(inode == file_inode(&temporary_file) && temporary_file.refs);
	assert(mounts == 1 && !locks && !temp_inode_locks && replace_locked);
	temp_inode_locks++;
}
static void inode_unlock(struct inode *inode)
{
	assert(inode == file_inode(&temporary_file) && temp_inode_locks == 1);
	temp_inode_locks--;
}
static void inode_lock_shared(struct inode *inode)
{
	assert(inode == file_inode(&directory));
	assert(mounts == 1 && !locks && !dir_shared_locks && replace_locked);
	dir_shared_locks++;
}
static void inode_unlock_shared(struct inode *inode)
{
	assert(inode == file_inode(&directory) && dir_shared_locks == 1);
	dir_shared_locks--;
}
static struct dentry *start_removing(void *idmap, struct dentry *parent,
				     struct qstr *name)
{
	assert(idmap == &idmap_token && parent == directory.f_path.dentry);
	assert(name->len == strlen(name->name));
	assert(mounts && !locks && replace_locked);
	struct catacomb_slot *slot = slot_for_temp(name->name);

	if (slot) {
		if (temp_name[0])
			assert(!strcmp(name->name, temp_name));
		if (stages[STALE_REMOVE])
			return ERR_PTR(-stages[STALE_REMOVE]);
		if (stale_vanish) {
			slot->stale_present = false;
			stale_vanish = false;
		}
		if (!slot->stale_present)
			return ERR_PTR(-ENOENT);
		slot->stale_dentry.linked = true;
		slot->stale_dentry.parent = file_inode(&directory);
		slot->stale_dentry.inode.type =
			stages[STALE_TYPE] ? stages[STALE_TYPE] : slot->stale_type;
		slot->stale_dentry.mountpoint = slot->stale_mountpoint;
		slot->stale_dentry.may_delete_err = slot->stale_may_delete_err;
		locks++;
		return &slot->stale_dentry;
	}
	slot = slot_for_base(name->name);
	assert(slot && !temp_name[0]);
	if (stages[DEST_LOOKUP])
		return ERR_PTR(-stages[DEST_LOOKUP]);
	if (!slot->exists)
		return ERR_PTR(-ENOENT);
	slot->dentry.linked = true;
	slot->dentry.parent = file_inode(&directory);
	slot->dentry.inode.type =
		stages[DEST_TYPE] ? stages[DEST_TYPE] : slot->type;
	slot->dentry.mountpoint =
		stages[DEST_MOUNT] ? true : slot->mountpoint;
	slot->dentry.may_delete_err =
		stages[DEST_MAY_DELETE] ? stages[DEST_MAY_DELETE] :
					  slot->may_delete_err;
	locks++;
	return &slot->dentry;
}
static ssize_t kernel_write(struct file *f, const void *buf, size_t n,
			    loff_t *pos)
{
	assert(f == &temporary_file || slot_for_file(f));
	assert(!mounts && !temp_inode_locks &&
	       (f != &temporary_file || replace_locked));
	assert(*pos >= 0 && (size_t)*pos + n <= CAP);
	write_calls++;
	if ((write_error || zero_write) && (size_t)*pos >= write_at)
		return zero_write ? 0 : -write_error;
	if (short_limit && n > short_limit)
		n = short_limit;
	if ((write_error || zero_write) && (size_t)*pos < write_at &&
	    n > write_at - (size_t)*pos)
		n = write_at - (size_t)*pos;
	memcpy(f->bytes + *pos, buf, n);
	*pos += n;
	if ((size_t)*pos > f->len)
		f->len = *pos;
	event('W');
	return n;
}
static int vfs_fsync(struct file *f, int data)
{
	assert(!data && !locks && !mounts);
	if (f == &directory) {
		if (!f->f_op || !f->f_op->fsync)
			return -EINVAL;
		dir_syncs++;
		event('D');
		return -stages[DIR_SYNC];
	}
	assert(f == &temporary_file || slot_for_file(f));
	assert(f != &temporary_file || replace_locked);
	temp_syncs++;
	event('F');
	return -stages[TEMP_SYNC];
}
static int mnt_want_write(void *m)
{
	assert(m == &mount_token);
	if (stages[MOUNT])
		return -stages[MOUNT];
	assert(!mounts);
	mounts++;
	return 0;
}
static void mnt_drop_write(void *m)
{
	assert(m == &mount_token && mounts == 1 && !locks);
	assert(!temp_inode_locks && !dir_shared_locks);
	mounts--;
}
static void *file_mnt_idmap(struct file *f)
{
	assert(f == &directory);
	return &idmap_token;
}
static int inode_permission(void *idmap, struct inode *inode, int mask)
{
	assert(idmap == &idmap_token && inode == file_inode(&directory));
	assert(mask == (MAY_WRITE | MAY_EXEC) && mounts == 1 && credentials == 1);
	assert(dir_shared_locks == 1);
	return -stages[DIR_PERM];
}
static int may_delete_dentry(void *idmap, struct inode *dir,
			     struct dentry *victim, bool isdir)
{
	assert(idmap == &idmap_token && dir == file_inode(&directory));
	assert(!isdir && locks == 1 && mounts == 1 && credentials == 1);
	assert(victim->parent == dir && victim->linked);
	if (stages[DIR_PERM])
		return -stages[DIR_PERM];
	if (IS_APPEND(dir))
		return -EPERM;
	if (IS_DEADDIR(dir))
		return -ENOENT;
	if (victim->may_delete_err)
		return -victim->may_delete_err;
	if (victim->inode.type == 2)
		return -EISDIR;
	return 0;
}
static bool d_mountpoint(const struct dentry *d)
{
	return d->mountpoint;
}
static bool fsuidgid_has_mapping(struct super_block *sb, void *idmap)
{
	assert(sb == &superblock_token && idmap == &idmap_token);
	assert(credentials == 1 && mounts == 1);
	return !stages[CREATOR_MAP];
}
static int start_renaming_dentry(struct renamedata *rd, int flags,
				 struct dentry *old, struct qstr *name)
{
	assert(credentials == 1 && mounts == 1 && !locks && flags == 0);
	assert(replace_locked && rd->mnt_idmap == &idmap_token);
	assert(rd->old_parent == directory.f_path.dentry &&
	       rd->new_parent == directory.f_path.dentry);
	assert(old == temporary_file.f_path.dentry);
	assert(temp_dentry_refs == 1 && !temporary_file.refs);
	assert(name->len == strlen(name->name));
	struct catacomb_slot *slot = slot_for_base(name->name);

	assert(slot && slot == active_slot);
	if (stages[RENAME_LOCK])
		return -stages[RENAME_LOCK];
	if (stages[STALE_SOURCE])
		return -EINVAL;
	locks++;
	if (publish_at_rename) {
		fixture_set_slot(slot - slots, "competing", 9);
		publish_at_rename = false;
	}
	rd->old_dentry = old;
	rd->new_dentry = &slot->dentry;
	rd->new_dentry->linked = slot->exists;
	rd->new_dentry->inode.type =
		stages[TARGET_TYPE] ? stages[TARGET_TYPE] : slot->type;
	return 0;
}
static bool d_is_positive(struct dentry *d)
{
	return d->linked;
}
static bool d_is_reg(struct dentry *d)
{
	return d->inode.type == 1;
}
static int security_path_rename(const struct path *a, struct dentry *b,
				const struct path *c, struct dentry *d,
				unsigned int flags)
{
	assert(a == &directory.f_path && c == a &&
	       b == temporary_file.f_path.dentry &&
	       active_slot && d == &active_slot->dentry && !flags && locks);
	return -stages[SECURITY_RENAME];
}
static int vfs_rename(struct renamedata *rd)
{
	assert(locks && mounts && credentials && replace_locked && active_slot);
	assert(strchr(events, 'F'));
	assert(rd->delegated_inode == NULL && rd->flags == 0);
	if (!file_inode(&directory)->i_op ||
	    !file_inode(&directory)->i_op->rename)
		return -EPERM;
	if (stages[RENAME])
		return -stages[RENAME];
	active_slot->file.len = temporary_file.len;
	memcpy(active_slot->file.bytes, temporary_file.bytes,
	       active_slot->file.len);
	active_slot->dentry.inode.i_mode =
		temporary_file.f_path.dentry->inode.i_mode;
	active_slot->exists = true;
	temp_present = 0;
	renames++;
	event('R');
	return 0;
}
static void end_renaming(struct renamedata *rd)
{
	assert(rd->old_parent == directory.f_path.dentry && locks == 1);
	locks--;
}
static struct dentry *start_removing_dentry(struct dentry *parent,
					    struct dentry *child)
{
	assert(parent == directory.f_path.dentry &&
	       child == temporary_file.f_path.dentry && mounts && !locks &&
	       replace_locked);
	assert(temp_dentry_refs == 1 && !temporary_file.refs);
	if (stages[CLEANUP_LOCK])
		return ERR_PTR(-stages[CLEANUP_LOCK]);
	if (stages[STALE_SOURCE])
		return ERR_PTR(-EINVAL);
	locks++;
	return child;
}
static int security_path_unlink(const struct path *p, struct dentry *d)
{
	assert(p == &directory.f_path && locks && replace_locked);
	struct catacomb_slot *slot = slot_for_stale_dentry(d);

	if (slot) {
		assert(d->inode.type == 1);
		return -(slot->stale_security_err ? slot->stale_security_err :
						    stages[STALE_SECURITY]);
	}
	assert(d == temporary_file.f_path.dentry);
	return -stages[SECURITY_UNLINK];
}
static int vfs_unlink(void *idmap, struct inode *inode, struct dentry *d,
		      void *delegated)
{
	assert(idmap == &idmap_token && inode == file_inode(&directory) &&
	       !delegated && locks && mounts && replace_locked);
	if (!inode->i_op || !inode->i_op->unlink)
		return -EPERM;
	struct catacomb_slot *slot = slot_for_stale_dentry(d);

	if (slot) {
		assert(slot->stale_present && d->inode.type == 1);
		int err = slot->stale_unlink_err ? slot->stale_unlink_err :
						   stages[STALE_UNLINK];
		if (err)
			return -err;
		slot->stale_present = false;
		stale_unlinks++;
		event('S');
		return 0;
	}
	assert(d == temporary_file.f_path.dentry);
	if (stages[UNLINK])
		return -stages[UNLINK];
	assert(temp_present);
	temp_present = 0;
	unlinks++;
	event('U');
	return 0;
}
static void end_removing(struct dentry *d)
{
	assert((d == temporary_file.f_path.dentry || slot_for_dentry(d) ||
		slot_for_stale_dentry(d)) && locks == 1);
	locks--;
}
static void fput(struct file *f)
{
	/* The temporary is released only through filp_close(). */
	assert(f != &temporary_file || temp_closing);
	assert(f->refs > 0);
	f->refs--;
}
/* The temporary has no ->flush; TEMP_CLOSE models a flush error at close. */
static int filp_close(struct file *f, void *id)
{
	assert(f == &temporary_file && !id && replace_locked);
	assert(!mounts && !locks && !temp_inode_locks && temp_dentry_refs == 1);
	temp_closing = true;
	fput(f);
	temp_closing = false;
	event('L');
	return -stages[TEMP_CLOSE];
}
static int notify_change(void *idmap, struct dentry *d, struct iattr *attr,
			 void *delegated)
{
	assert(idmap == &idmap_token && d == temporary_file.f_path.dentry);
	assert(!delegated && attr->ia_valid == (ATTR_MODE | ATTR_CTIME));
	assert(mounts == 1 && temp_inode_locks == 1 && credentials == 1);
	assert(replace_locked && temporary_file.refs);
	assert((attr->ia_mode & ~S_IALLUGO) == (d->inode.i_mode & ~S_IALLUGO));
	if (stages[TEMP_CHMOD])
		return -stages[TEMP_CHMOD];
	d->inode.i_mode = attr->ia_mode;
	chmod_calls++;
	event('M');
	return 0;
}
static struct dentry *dget(struct dentry *d)
{
	assert(d == temporary_file.f_path.dentry && temporary_file.refs);
	assert(!temp_dentry_refs);
	temp_dentry_refs++;
	return d;
}
/* Leaves for the existing production parent-sync operation used on retry. */
static struct dentry *dget_parent(struct dentry *d)
{
	assert(slot_for_dentry(d));
	dir_dentry_refs++;
	return directory.f_path.dentry;
}
static void dput(struct dentry *d)
{
	if (d == temporary_file.f_path.dentry) {
		assert(temp_dentry_refs == 1);
		temp_dentry_refs--;
		return;
	}
	assert(d == directory.f_path.dentry && dir_dentry_refs > 0);
	dir_dentry_refs--;
}
static struct file *dentry_open(const struct path *p, int flags,
			       const struct cred *cred)
{
	assert(p->mnt == &mount_token && p->dentry == directory.f_path.dentry);
	assert(flags == (O_RDONLY | O_DIRECTORY) && cred == &credential);
	if (stages[PARENT_OPEN])
		return ERR_PTR(-stages[PARENT_OPEN]);
	directory.refs++;
	return &directory;
}
#define pr_err(...) (++commit_warning)
#define pr_warn(...) (++cleanup_warning)
/* Existing C/Rust I/O ABI for testing the actual Catacomb reader and old writer. */
void *sep_store_open_ro(const char *p)
{
	struct catacomb_slot *slot = slot_for_path(p);

	if (stages[READ_OPEN])
		return ERR_PTR(-stages[READ_OPEN]);
	if (!slot->exists)
		return ERR_PTR(-ENOENT);
	slot->file.refs++;
	return &slot->file;
}
void *sep_store_open_trunc(const char *p)
{
	struct catacomb_slot *slot = slot_for_path(p);

	slot->file.len = 0;
	slot->exists = true;
	slot->file.refs++;
	return &slot->file;
}
void *sep_store_open(const char *p)
{
	return sep_store_open_trunc(p);
}
void *sep_store_open_host(const char *p, bool *created)
{
	struct catacomb_slot *slot = slot_for_path(p);

	if (stages[HOST_OPEN]) {
		*created = false;
		return ERR_PTR(-stages[HOST_OPEN]);
	}
	if (slot->exists) {
		*created = false;
		slot->file.refs++;
		return &slot->file;
	}
	*created = true;
	slot->exists = true;
	slot->file.len = 0;
	slot->file.refs++;
	event('H');
	return &slot->file;
}
void *sep_store_open_block(const char *p, int w)
{
	(void)p;
	(void)w;
	abort();
}
void *sep_store_open_writable_existing(const char *p)
{
	struct catacomb_slot *slot = slot_for_path(p);

	if (!slot->exists || slot->write_open_err || stages[WRITE_OPEN])
		return NULL;
	slot->file.refs++;
	return &slot->file;
}
void sep_store_close(void *p)
{
	fput(p);
}
long long sep_store_size(void *p)
{
	return ((struct file *)p)->len;
}
long sep_store_read(void *p, long long off, void *buf, size_t n)
{
	struct file *f = p;

	read_calls++;
	if ((size_t)off >= f->len)
		return 0;
	if (n > f->len - off)
		n = f->len - off;
	memcpy(buf, f->bytes + off, n);
	return n;
}
long sep_store_write(void *p, long long off, const void *buf, size_t n)
{
	loff_t pos = off;

	return kernel_write(p, buf, n, &pos);
}
int sep_store_sync(void *p)
{
	return vfs_fsync(p, 0);
}
int sep_random_bytes(void *p, size_t n)
{
	memset(p, 1, n);
	return 0;
}
