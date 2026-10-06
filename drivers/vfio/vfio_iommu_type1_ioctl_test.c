// SPDX-License-Identifier: GPL-2.0-only
/* Included after the KUnit harness; real EL0 arguments, recorded domains only. */
#include <linux/compat.h>
#include <linux/miscdevice.h>
#include <linux/workqueue.h>
#include "vfio_iommu_type1_test.h"

struct vfio_ioctl_fixture {
	struct vfio_fragment_fixture pins;
	struct mutex request_lock;
};

struct vfio_ioctl_rw {
	struct work_struct work;
	struct vfio_iommu *iommu;
	dma_addr_t iova;
	void *data;
	size_t size;
	bool write;
	int ret;
};

static void vfio_ioctl_rw_work(struct work_struct *work)
{
	struct vfio_ioctl_rw *rw = container_of(work, typeof(*rw), work);

	rw->ret = vfio_iommu_type1_dma_rw(rw->iommu, rw->iova, rw->data,
					 rw->size, rw->write);
}

static int vfio_ioctl_test_open(struct inode *inode, struct file *file)
{
	struct vfio_ioctl_fixture *ctx;
	struct vfio_fragment_fixture *f;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	f = &ctx->pins;
	f->iommu = vfio_iommu_type1_open(VFIO_TYPE1v2_IOMMU);
	if (IS_ERR(f->iommu)) {
		int ret = PTR_ERR(f->iommu);

		kfree(ctx);
		return ret;
	}
	mutex_init(&ctx->request_lock);
	for (unsigned int i = 0; i < ARRAY_SIZE(f->domains); i++) {
		struct vfio_test_domain *mock = &f->domains[i];

		xa_init(&mock->translations);
		mock->core.ops = &vfio_test_domain_ops;
		mock->core.type = IOMMU_DOMAIN_UNMANAGED;
		mock->core.pgsize_bitmap = SZ_4K | PAGE_SIZE;
		mock->type1.domain = &mock->core;
		INIT_LIST_HEAD(&mock->type1.next);
		INIT_LIST_HEAD(&mock->type1.group_list);
	}
	f->group.iommu_group = (struct iommu_group *)ctx;
	list_add(&f->group.next, &f->iommu->emulated_iommu_groups);
	f->iommu->num_non_pinned_groups = 1;
	f->device.ops = &vfio_test_device_ops;
	vfio_iommu_type1_register_device(f->iommu, &f->device);
	file->private_data = ctx;
	return 0;
}

static int vfio_ioctl_test_release(struct inode *inode, struct file *file)
{
	struct vfio_ioctl_fixture *ctx = file->private_data;
	struct vfio_fragment_fixture *f = &ctx->pins;

	while (f->external_refs) {
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA, 1);
		f->external_refs--;
	}
	vfio_iommu_type1_unregister_device(f->iommu, &f->device);
	vfio_iommu_unmap_unpin_all(f->iommu);
	for (unsigned int i = 0; i < ARRAY_SIZE(f->domains); i++) {
		list_del_init(&f->domains[i].type1.next);
		xa_destroy(&f->domains[i].translations);
	}
	list_del(&f->group.next);
	vfio_iommu_type1_release(f->iommu);
	kfree(ctx);
	return 0;
}

static int vfio_ioctl_test_domain(struct vfio_fragment_fixture *f,
				  struct vfio_type1_test_cmd *cmd, bool add)
{
	struct vfio_test_domain *mock;
	struct rb_node *node;
	int ret = 0;

	if (cmd->domain >= ARRAY_SIZE(f->domains) || cmd->flags)
		return -EINVAL;
	mock = &f->domains[cmd->domain];
	mutex_lock(&f->iommu->lock);
	if (add) {
		if (!list_empty(&mock->type1.next)) {
			ret = -EEXIST;
			goto out;
		}
		if (cmd->size != SZ_4K && cmd->size != SZ_16K && cmd->size != SZ_64K) {
			ret = -EINVAL;
			goto out;
		}
		mock->core.pgsize_bitmap = cmd->size;
		mock->fail_after = cmd->fail_after;
		ret = vfio_iommu_replay(f->iommu, &mock->type1);
		if (ret)
			goto out;
		list_add_tail(&mock->type1.next, &f->iommu->domain_list);
	} else {
		if (list_empty(&mock->type1.next)) {
			ret = -ENOENT;
			goto out;
		}
		if (list_is_singular(&f->iommu->domain_list)) {
			vfio_iommu_unmap_unpin_reaccount(f->iommu);
		} else {
			for (node = rb_first(&f->iommu->dma_list); node; node = rb_next(node)) {
				struct vfio_dma *dma = rb_entry(node, struct vfio_dma, node);

				iommu_unmap(&mock->core, dma->iova, dma->size);
			}
		}
		list_del_init(&mock->type1.next);
	}
	vfio_update_pgsize_bitmap(f->iommu);
out:
	mutex_unlock(&f->iommu->lock);
	return ret;
}

static int vfio_ioctl_test_rw(struct vfio_fragment_fixture *f,
			      struct vfio_type1_test_cmd *cmd)
{
	struct vfio_ioctl_rw rw = {
		.iommu = f->iommu, .iova = cmd->iova, .size = cmd->size,
		.write = cmd->flags & VFIO_TYPE1_TEST_WRITE,
	};
	void __user *buffer = u64_to_user_ptr(cmd->buffer);
	int ret;

	if (!cmd->size || cmd->size > 256 ||
	    cmd->flags & ~(VFIO_TYPE1_TEST_WRITE | VFIO_TYPE1_TEST_WORKER))
		return -EINVAL;
	rw.data = kzalloc(cmd->size, GFP_KERNEL);
	if (!rw.data)
		return -ENOMEM;
	if (rw.write && copy_from_user(rw.data, buffer, cmd->size)) {
		ret = -EFAULT;
		goto out;
	}
	if (cmd->flags & VFIO_TYPE1_TEST_WORKER) {
		INIT_WORK(&rw.work, vfio_ioctl_rw_work);
		queue_work(system_unbound_wq, &rw.work);
		flush_work(&rw.work);
		ret = rw.ret;
	} else {
		ret = vfio_iommu_type1_dma_rw(f->iommu, rw.iova, rw.data, rw.size, rw.write);
	}
	if (!ret && !rw.write && copy_to_user(buffer, rw.data, cmd->size))
		ret = -EFAULT;
out:
	kfree(rw.data);
	return ret;
}

static long vfio_ioctl_test_ioctl(struct file *file, unsigned int request,
				  unsigned long arg)
{
	struct vfio_ioctl_fixture *ctx = file->private_data;
	struct vfio_fragment_fixture *f = &ctx->pins;
	struct vfio_type1_test_cmd cmd;
	void __user *user = (void __user *)arg;
	struct page *page;
	int ret = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	/* Request serialization belongs to this fixture, not production type1. */
	mutex_lock(&ctx->request_lock);
	switch (request) {
	case VFIO_CHECK_EXTENSION:
	case VFIO_IOMMU_GET_INFO:
	case VFIO_IOMMU_MAP_DMA:
	case VFIO_IOMMU_UNMAP_DMA:
	case VFIO_IOMMU_DIRTY_PAGES:
		ret = vfio_iommu_type1_ioctl(f->iommu, request, arg);
		goto out;
	default:
		break;
	}
	if (copy_from_user(&cmd, user, sizeof(cmd))) {
		ret = -EFAULT;
		goto out;
	}
	if (cmd.reserved) {
		ret = -EINVAL;
		goto out;
	}
	switch (request) {
	case VFIO_TYPE1_TEST_DOMAIN_ADD:
	case VFIO_TYPE1_TEST_DOMAIN_DEL:
		ret = vfio_ioctl_test_domain(f, &cmd, request == VFIO_TYPE1_TEST_DOMAIN_ADD);
		break;
	case VFIO_TYPE1_TEST_RW:
		ret = vfio_ioctl_test_rw(f, &cmd);
		break;
	case VFIO_TYPE1_TEST_PIN:
	case VFIO_TYPE1_TEST_UNPIN:
		if (ALIGN_DOWN(cmd.iova, PAGE_SIZE) != VFIO_TEST_IOVA || cmd.flags) {
			ret = -EINVAL;
			break;
		}
		if (request == VFIO_TYPE1_TEST_PIN) {
			ret = vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
						      cmd.iova, 1, IOMMU_READ | IOMMU_WRITE, &page);
			if (ret == 1) {
				f->external_refs++;
				ret = 0;
			}
		} else if (f->external_refs) {
			vfio_iommu_type1_unpin_pages(f->iommu, cmd.iova, 1);
			f->external_refs--;
		} else {
			ret = -ENOENT;
		}
		break;
	case VFIO_TYPE1_TEST_EMULATED:
		if (cmd.flags > 1 || f->external_refs) {
			ret = -EBUSY;
			break;
		}
		mutex_lock(&f->iommu->lock);
		if (cmd.flags && list_empty(&f->group.next)) {
			list_add(&f->group.next, &f->iommu->emulated_iommu_groups);
			if (!f->group.pinned_page_dirty_scope)
				f->iommu->num_non_pinned_groups++;
		} else if (!cmd.flags && !list_empty(&f->group.next)) {
			list_del_init(&f->group.next);
			if (!f->group.pinned_page_dirty_scope)
				f->iommu->num_non_pinned_groups--;
		}
		mutex_unlock(&f->iommu->lock);
		break;
	case VFIO_TYPE1_TEST_STATE:
		cmd.locked_bytes = 0;
		cmd.owner_locked_bytes = mm_locked_vm_bytes(current->mm);
		cmd.flags = 0;
		cmd.owner_users = 0;
		mutex_lock(&f->iommu->lock);
		for (struct rb_node *node = rb_first(&f->iommu->dma_list);
		     node; node = rb_next(node)) {
			struct vfio_dma *dma = rb_entry(node, struct vfio_dma, node);

			cmd.locked_bytes += dma->locked_vm * PAGE_SIZE;
			cmd.owner_locked_bytes = mm_locked_vm_bytes(dma->mm);
			cmd.owner_users = atomic_read(&dma->mm->mm_users);
			if (dma->fragments)
				cmd.flags |= VFIO_TYPE1_TEST_STATE_FRAGMENTS;
		}
		cmd.native_page_size = PAGE_SIZE;
		cmd.minimum_page_size = 1UL << __ffs(f->iommu->pgsize_bitmap);
		for (unsigned int i = 0; i < ARRAY_SIZE(f->domains); i++)
			cmd.mapped_bytes[i] = f->domains[i].mapped;
		mutex_unlock(&f->iommu->lock);
		cmd.notifications = f->notifications;
		cmd.external_refs = f->external_refs;
		break;
	default:
		ret = -ENOTTY;
	}
	if (!ret && copy_to_user(user, &cmd, sizeof(cmd)))
		ret = -EFAULT;
out:
	mutex_unlock(&ctx->request_lock);
	return ret;
}

static const struct file_operations vfio_ioctl_test_fops = {
	.owner = THIS_MODULE,
	.open = vfio_ioctl_test_open,
	.release = vfio_ioctl_test_release,
	.unlocked_ioctl = vfio_ioctl_test_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct miscdevice vfio_ioctl_test_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "aurora-vfio-type1-test",
	.fops = &vfio_ioctl_test_fops,
	.mode = 0600,
};

static int __init vfio_ioctl_test_init(void)
{
	return misc_register(&vfio_ioctl_test_device);
}
late_initcall(vfio_ioctl_test_init);
