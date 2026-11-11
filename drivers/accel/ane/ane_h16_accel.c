// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * H16 and H17 accel node (ABI version 3). It exists only after the staged
 * opt-in completed its program stage. Programs, sections and scratch live in
 * private kernel mappings; userspace passes the original HWX, the host-built
 * sections and plain input/output buffers, never DVAs, firmware IDs or
 * commands. A process may map its program's surfaces (inputs and outputs
 * only) and run requests on them in place, without copies.
 */
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/sizes.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_accel.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/ane_accel.h>

#include "ane_h16_accel.h"

struct ane_h16_accel {
	struct drm_device drm;
	struct ane_h16_session *session;
	/* Serializes admission, transport traffic and completion dispatch. */
	struct mutex lock;
};

struct ane_h16_accel_file {
	/* Pins the program lifetime across LOAD and an EXECUTE's copy phases. */
	struct mutex lock;
	struct ane_h16_program *program;
};

static struct ane_h16_accel *to_h16_accel(struct drm_device *drm)
{
	return container_of(drm, struct ane_h16_accel, drm);
}

static int ane_h16_get_caps_ioctl(struct drm_device *drm, void *data,
				  struct drm_file *file)
{
	struct ane_h16_accel *a = to_h16_accel(drm);
	struct drm_ane_get_caps *args = data;

	if (args->flags || args->pad)
		return -EINVAL;
	*args = (struct drm_ane_get_caps) {
		.size = sizeof(*args),
		.abi_version = DRM_ANE_ABI_V3,
		.chip_family = a->session->transport->soc->chip_family,
		.section_size = sizeof(struct drm_ane_h16_section),
		.exec_io_size = sizeof(struct drm_ane_h16_buffer),
	};
	return 0;
}

static int ane_h16_sections_copy(const struct drm_ane_h16_load *args,
				 void *aux[ANE_H16_AUX_COUNT],
				 size_t aux_size[ANE_H16_AUX_COUNT])
{
	struct drm_ane_h16_section sections[DRM_ANE_H16_SECTIONS];
	unsigned int i;

	BUILD_BUG_ON(DRM_ANE_H16_SECTIONS != ANE_H16_AUX_COUNT);
	BUILD_BUG_ON(DRM_ANE_H16_SECTION_MIN != ANE_H16_AUX_MIN);
	BUILD_BUG_ON(DRM_ANE_H16_SECTION_MAX != ANE_H16_AUX_MAX);
	if (args->section_count != DRM_ANE_H16_SECTIONS)
		return -EINVAL;
	if (copy_from_user(sections, u64_to_user_ptr(args->sections_ptr), sizeof(sections)))
		return -EFAULT;
	for (i = 0; i < DRM_ANE_H16_SECTIONS; i++) {
		const struct drm_ane_h16_section *s = &sections[i];
		unsigned int kind = s->kind - DRM_ANE_H16_SECTION_GENERIC;
		void *bytes;

		if (s->kind < DRM_ANE_H16_SECTION_GENERIC ||
		    s->kind > DRM_ANE_H16_SECTION_TEXT_PROPERTIES || aux[kind] ||
		    s->size < ANE_H16_AUX_MIN || s->size > ANE_H16_AUX_MAX ||
		    !IS_ALIGNED(s->size, 4))
			return -EINVAL;
		bytes = memdup_user(u64_to_user_ptr(s->data_ptr), s->size);
		if (IS_ERR(bytes))
			return PTR_ERR(bytes);
		aux[kind] = bytes;
		aux_size[kind] = s->size;
	}
	return 0;
}

static int ane_h16_load_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct ane_h16_accel *a = to_h16_accel(drm);
	struct ane_h16_accel_file *f = file->driver_priv;
	struct drm_ane_h16_load *args = data;
	struct ane_h16_hwx_snapshot snapshot = {};
	void *aux[ANE_H16_AUX_COUNT] = {};
	size_t aux_size[ANE_H16_AUX_COUNT] = {};
	struct ane_h16_program *program = NULL;
	unsigned int i;
	int ret;

	if (args->flags || args->pad)
		return -EINVAL;
	ret = ane_h16_sections_copy(args, aux, aux_size);
	if (!ret)
		ret = ane_h16_hwx_snapshot_from_user(u64_to_user_ptr(args->hwx_ptr),
						     args->hwx_size, &snapshot);
	if (ret)
		goto out;
	mutex_lock(&f->lock);
	ret = ane_h16_session_enter(a->session, file->filp->f_flags & O_NONBLOCK);
	if (ret) {
		mutex_unlock(&f->lock);
		goto out;
	}
	/* Lost across system sleep: release it, the caller is reloading. */
	if (f->program && ane_h16_program_lost(a->session, f->program) &&
	    !ane_h16_session_close(a->session, f->program)) {
		ane_h16_session_put(a->session, f->program);
		f->program = NULL;
	}
	if (f->program) {
		ret = -EBUSY;
	} else {
		ret = ane_h16_session_load(a->session, &snapshot,
					   (const void *const *)aux, aux_size, &program);
		if (!ret)
			f->program = program;
	}
	if (!ret) {
		const struct ane_h16_hwx *hwx = &program->hwx;

		args->surface_count = program->resource_count;
		for (i = 0; i < DRM_ANE_H16_SURFACES; i++) {
			args->surface_size[i] = i < program->resource_count ?
				program->resources[i].buffer.size : 0;
			args->surface_direction[i] = i < program->resource_count ?
				hwx->surfaces[i].direction : 0;
		}
	}
	mutex_unlock(&a->lock);
	mutex_unlock(&f->lock);
out:
	/* A consumed snapshot was moved into the program and reads as empty. */
	if (snapshot.bytes)
		ane_h16_hwx_snapshot_destroy(&snapshot);
	for (i = 0; i < ANE_H16_AUX_COUNT; i++)
		kfree(aux[i]);
	return ret;
}

static int ane_h16_exec_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct ane_h16_accel *a = to_h16_accel(drm);
	struct ane_h16_accel_file *f = file->driver_priv;
	struct drm_ane_h16_exec *args = data;
	struct drm_ane_h16_buffer buffers[ANE_H16_SURFACES];
	struct ane_h16_program *p;
	u64 start, locked, copied, done;
	unsigned int i;
	int ret = 0;

	const bool in_place = args->flags & DRM_ANE_H16_EXEC_IN_PLACE;

	if ((args->flags & ~DRM_ANE_H16_EXEC_IN_PLACE) || !args->count ||
	    args->count > ANE_H16_SURFACES || (in_place && args->buffers_ptr))
		return -EINVAL;
	if (!in_place && copy_from_user(buffers, u64_to_user_ptr(args->buffers_ptr),
					array_size(args->count, sizeof(*buffers))))
		return -EFAULT;
	start = ktime_get_ns();
	mutex_lock(&f->lock);
	ret = ane_h16_session_enter(a->session, file->filp->f_flags & O_NONBLOCK);
	if (ret) {
		mutex_unlock(&f->lock);
		return ret;
	}
	p = f->program;
	if (!p || p->closed || args->count != p->resource_count) {
		ret = -EINVAL;
		goto unlock;
	}
	for (i = 0; i < args->count && !in_place; i++) {
		if (buffers[i].size != p->resources[i].buffer.size) {
			ret = -EINVAL;
			goto unlock;
		}
	}
	ret = ane_h16_session_acquire(a->session, p);
	if (ret)
		goto unlock;
	locked = ktime_get_ns();
	/* This program's shared surfaces are ours through the final copy-out. */
	for (i = 0; i < args->count && !in_place; i++) {
		void *cpu = p->surfaces[i]->cpu;

		if (p->resources[i].permissions == 1) {
			if (copy_from_user(cpu, u64_to_user_ptr(buffers[i].ptr), buffers[i].size)) {
				ret = -EFAULT;
				goto release;
			}
		} else {
			memset(cpu, 0xff, buffers[i].size);
		}
	}
	ane_h16_session_sync(a->session, p, true);
	copied = ktime_get_ns();
	ret = ane_h16_session_run(a->session, p);
	if (ret)
		goto release;
	done = ktime_get_ns();
	ane_h16_session_sync(a->session, p, false);
	for (i = 0; i < args->count && !in_place; i++)
		if (p->resources[i].permissions == 2 &&
		    copy_to_user(u64_to_user_ptr(buffers[i].ptr), p->surfaces[i]->cpu,
				 buffers[i].size))
			ret = -EFAULT;
	if (!ret) {
		u64 end = ktime_get_ns();
		const u64 *ns = a->session->timing.ns;

		ane_h16_session_account(a->session, locked - start, copied - locked, end - done,
					end - start);
		drm_dbg_driver(drm,
			       "EXECUTE us: lock %llu in %llu prep %llu open %llu ack %llu report %llu done %llu close %llu fin %llu out %llu fw %llu all %llu polls %llu\n",
			       div_u64(locked - start, 1000),
			       div_u64(copied - locked, 1000),
			       div_u64(ns[ANE_H16_T_PREPARE], 1000),
			       div_u64(ns[ANE_H16_T_GATE_OPEN], 1000),
			       div_u64(ns[ANE_H16_T_SUBMIT_ACK], 1000),
			       div_u64(ns[ANE_H16_T_ACK_REPORT], 1000),
			       div_u64(ns[ANE_H16_T_REPORT_DONE], 1000),
			       div_u64(ns[ANE_H16_T_GATE_CLOSE], 1000),
			       div_u64(ns[ANE_H16_T_FINISH], 1000),
			       div_u64(end - done, 1000),
			       div_u64(ns[ANE_H16_T_FIRMWARE], 1000),
			       div_u64(end - start, 1000), a->session->timing.polls);
	}
release:
	ane_h16_session_release(a->session, p);
unlock:
	mutex_unlock(&a->lock);
	mutex_unlock(&f->lock);
	return ret;
}

static void ane_h16_vm_open(struct vm_area_struct *vma)
{
	ane_h16_export_get(vma->vm_private_data);
}

static void ane_h16_vm_close(struct vm_area_struct *vma)
{
	ane_h16_export_put(vma->vm_private_data);
}

static const struct vm_operations_struct ane_h16_vm_ops = {
	.open = ane_h16_vm_open,
	.close = ane_h16_vm_close,
};

/*
 * Map a prefix of surface i of the file's program at DRM_ANE_H16_MMAP_OFFSET(i).
 * Inputs may be written, outputs only read. The pages are the surface's own,
 * held by the mapping (struct ane_h16_export) until it is gone.
 */
static int ane_h16_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *file = filp->private_data;
	struct ane_h16_accel *a = to_h16_accel(file->minor->dev);
	struct ane_h16_accel_file *f = file->driver_priv;
	const u64 offset = (u64)vma->vm_pgoff << PAGE_SHIFT;
	const unsigned long pages = vma_pages(vma);
	struct ane_h16_export *e = NULL;
	struct ane_h16_program *p;
	unsigned long n, i;
	struct page **list;
	unsigned int index;
	bool input = false;
	int ret;

	/*
	 * Surfaces are 16 KiB blocks of order-0 pages only with 16 KiB pages;
	 * a smaller page would map the tail pages of a higher-order block.
	 */
	if (PAGE_SIZE != SZ_16K)
		return -EOPNOTSUPP;
	if (offset & GENMASK_ULL(31, 0) || offset >> 32 < 1 ||
	    offset >> 32 > ANE_H16_SURFACES || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	index = (offset >> 32) - 1;
	mutex_lock(&f->lock);
	ret = ane_h16_session_enter(a->session, filp->f_flags & O_NONBLOCK);
	if (ret) {
		mutex_unlock(&f->lock);
		return ret;
	}
	p = f->program;
	ret = -EINVAL;
	if (p && !p->closed && index < p->resource_count &&
	    pages <= DIV_ROUND_UP(p->resources[index].buffer.size, PAGE_SIZE) &&
	    pages <= p->surfaces[index]->size >> PAGE_SHIFT &&
	    !((vma->vm_flags & VM_WRITE) && p->resources[index].permissions != 1)) {
		input = p->resources[index].permissions == 1;
		ret = ane_h16_session_export(a->session, p, index, &e);
	}
	mutex_unlock(&a->lock);
	mutex_unlock(&f->lock);
	if (ret)
		return ret;

	list = kvmalloc_array(pages, sizeof(*list), GFP_KERNEL);
	if (!list) {
		ane_h16_export_put(e);
		return -ENOMEM;
	}
	for (i = 0; i < pages; i++)
		list[i] = vmalloc_to_page(e->surface.cpu + i * PAGE_SIZE);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	vm_flags_clear(vma, VM_MAYEXEC);
	if (!input)
		vm_flags_clear(vma, VM_MAYWRITE);
	vma->vm_private_data = e;
	vma->vm_ops = &ane_h16_vm_ops;
	n = pages;
	ret = vm_insert_pages(vma, vma->vm_start, list, &n);
	kvfree(list);
	if (ret) {
		/* The failed mmap is unmapped without our close: drop the reference here. */
		vma->vm_ops = NULL;
		vma->vm_private_data = NULL;
		ane_h16_export_put(e);
	}
	return ret;
}

static int ane_h16_open(struct drm_device *drm, struct drm_file *file)
{
	struct ane_h16_accel_file *f = kzalloc_obj(*f);

	if (!f)
		return -ENOMEM;
	mutex_init(&f->lock);
	file->driver_priv = f;
	return 0;
}

/* TERMINATE and UNLOAD on close. Firmware-visible memory stays until reboot. */
static void ane_h16_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct ane_h16_accel *a = to_h16_accel(drm);
	struct ane_h16_accel_file *f = file->driver_priv;
	int ret;

	mutex_lock(&a->lock);
	if (f->program) {
		/* Quarantined before this close: nothing failed here. */
		bool quarantined = a->session->quarantined;

		ret = ane_h16_session_close(a->session, f->program);
		/*
		 * -EINPROGRESS: deferred to the end of system sleep. -EIO on an
		 * already quarantined session: no firmware traffic was tried, and
		 * the program's memory stays mapped with every other mapping
		 * until reboot, since the quarantined firmware may still read it.
		 */
		if (ret == -EIO && quarantined)
			drm_dbg(drm, "program kept until reboot: engine quarantined\n");
		else if (ret && ret != -EINPROGRESS)
			drm_err(drm, "program teardown failed %d; session quarantined\n", ret);
		else if (!ret)
			ane_h16_session_put(a->session, f->program);
	}
	mutex_unlock(&a->lock);
	mutex_destroy(&f->lock);
	kfree(f);
}

static const struct drm_ioctl_desc ane_h16_ioctls[] = {
	DRM_IOCTL_DEF_DRV(ANE_GET_CAPS, ane_h16_get_caps_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ANE_H16_LOAD, ane_h16_load_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ANE_H16_EXEC, ane_h16_exec_ioctl, DRM_RENDER_ALLOW),
};

/*
 * No GEM objects: DRM_ACCEL_FOPS with the driver's own surface .mmap
 * (drm_gem_mmap needs DRIVER_GEM). The DRM core refuses to open a file whose
 * fops lack FOP_UNSIGNED_OFFSET.
 */
static const struct file_operations ane_h16_fops = {
	.owner = THIS_MODULE,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	.open = accel_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = ane_h16_mmap,
};

static const struct drm_driver ane_h16_drm_driver = {
	.driver_features = DRIVER_COMPUTE_ACCEL,
	.open = ane_h16_open,
	.postclose = ane_h16_postclose,
	.ioctls = ane_h16_ioctls,
	.num_ioctls = ARRAY_SIZE(ane_h16_ioctls),
	.fops = &ane_h16_fops,
	.major = DRM_ANE_ABI_V3,
	.minor = 1,
	.name = "ane",
	.desc = "Apple Neural Engine (H16 and later)",
};

int ane_h16_accel_register(struct device *dev, struct ane_h16_session *session)
{
	struct ane_h16_accel *a;
	int ret;

	a = devm_drm_dev_alloc(dev, &ane_h16_drm_driver, struct ane_h16_accel, drm);
	if (IS_ERR(a))
		return PTR_ERR(a);
	a->session = session;
	ret = drmm_mutex_init(&a->drm, &a->lock);
	if (ret)
		return ret;
	/* The session's sleep gate serializes on the node lock. */
	session->lock = &a->lock;
	ret = drm_dev_register(&a->drm, 0);
	if (ret)
		return ret;
	drm_info(&a->drm, "H%u accel node ready (ABI 3, %s)\n",
		 session->transport->soc->chip_family,
		 ane_fw_transport_switch_safe(session->transport) ?
		 "programs switch within one firmware boot" : "one loaded program at a time");
	return 0;
}
