// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Anonymous authorization files. Protocol and lifetime policy remain in Rust. */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "shim.h"

struct sep_context_file {
	void *ctx;
	void (*release)(void *ctx);
	long (*ioctl)(void *ctx, unsigned int cmd, unsigned long arg);
};

static int sep_context_release(struct inode *inode, struct file *file)
{
	struct sep_context_file *context = file->private_data;

	context->release(context->ctx);
	kfree(context);
	return 0;
}

static long sep_context_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct sep_context_file *context = file->private_data;

	return context->ioctl(context->ctx, cmd, arg);
}

static const struct file_operations sep_context_fops = {
	.owner = THIS_MODULE,
	.release = sep_context_release,
	.unlocked_ioctl = sep_context_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

int sep_context_file_new(void *ctx, void (*release)(void *),
			 long (*ioctl)(void *, unsigned int, unsigned long),
			 void **out, int *out_fd)
{
	struct sep_context_file *context;
	struct file *file;
	int fd;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	context = kzalloc(sizeof(*context), GFP_KERNEL_ACCOUNT);
	if (!context) {
		put_unused_fd(fd);
		return -ENOMEM;
	}
	context->ctx = ctx;
	context->release = release;
	context->ioctl = ioctl;
	file = anon_inode_getfile("apple-sep-context", &sep_context_fops,
				  context, O_RDWR | O_CLOEXEC);
	if (IS_ERR(file)) {
		kfree(context);
		put_unused_fd(fd);
		return PTR_ERR(file);
	}
	*out = file;
	*out_fd = fd;
	return 0;
}

void sep_context_file_finish(void *ptr, int fd, bool install)
{
	struct file *file = ptr;

	if (install) {
		fd_install(fd, file);
	} else {
		put_unused_fd(fd);
		fput(file);
	}
}

void *sep_context_file_get(int fd, void **ctx)
{
	struct file *file = fget(fd);
	struct sep_context_file *context;

	if (!file)
		return NULL;
	if (file->f_op != &sep_context_fops) {
		fput(file);
		return NULL;
	}
	context = file->private_data;
	*ctx = context->ctx;
	return file;
}

void sep_context_file_put(void *file)
{
	fput(file);
}
