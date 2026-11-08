/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_INTERNAL_H
#define LINUX_KEXEC_INTERNAL_H

#include <linux/kexec.h>

struct kexec_segment;

struct kimage *do_kimage_alloc_init(void);
int sanity_check_segment_list(struct kimage *image);
void kimage_free_page_list(struct list_head *list);
void kimage_free(struct kimage *image);
int kimage_load_segment(struct kimage *image, int idx);
void kimage_terminate(struct kimage *image);
int kimage_is_destination_range(struct kimage *image,
				unsigned long start, unsigned long end);

/*
 * Whatever is used to serialize accesses to the kexec_crash_image needs to be
 * NMI safe, as __crash_kexec() can happen during nmi_panic(), so here we use a
 * "simple" atomic variable that is acquired with a cmpxchg().
 */
extern atomic_t __kexec_lock;
bool notrace kexec_blocked(void);
const char *notrace kexec_block_reason(void);
#ifdef CONFIG_KEXEC_BLOCK_KUNIT_TEST
bool kexec_test_load_limits_unlimited(void);
#endif
/*
 * A blocker keeps a new kernel from reusing memory that a device still reaches
 * by DMA. A crash kernel reuses none of it: it runs in the crash kernel
 * reservation, which memblock set aside at boot, before any driver could give
 * memory to a device. Only CMA-backed crash memory (crashkernel=,cma) is lent
 * to the page allocator while the system runs, so blockers still stop a crash
 * kernel that has some.
 */
static __always_inline bool notrace kexec_crash_blocked(void)
{
	return kexec_blocked() && crashk_cma_cnt;
}

/* Caller holds the existing NMI-safe kexec lock; no image is dereferenced. */
static __always_inline bool kexec_crash_image_allowed(bool image_loaded)
{
	return image_loaded && !kexec_crash_blocked();
}

#ifdef CONFIG_KEXEC
int do_kexec_load(unsigned long entry, unsigned long nr_segments,
		  struct kexec_segment *segments, unsigned long flags);
#endif
#ifdef CONFIG_KEXEC_FILE
int do_kexec_file_load(int kernel_fd, int initrd_fd, unsigned long cmdline_len,
		       const char __user *cmdline_ptr, unsigned long flags);
#endif

static inline bool kexec_trylock(void)
{
	int old = 0;
	return atomic_try_cmpxchg_acquire(&__kexec_lock, &old, 1);
}
static inline void kexec_unlock(void)
{
	atomic_set_release(&__kexec_lock, 0);
}

#ifdef CONFIG_KEXEC_FILE
#include <linux/purgatory.h>
void kimage_file_post_load_cleanup(struct kimage *image);
extern char kexec_purgatory[];
extern size_t kexec_purgatory_size;
#else /* CONFIG_KEXEC_FILE */
static inline void kimage_file_post_load_cleanup(struct kimage *image) { }
#endif /* CONFIG_KEXEC_FILE */

struct kexec_buf;

#ifdef CONFIG_KEXEC_HANDOVER
int kho_locate_mem_hole(struct kexec_buf *kbuf,
			int (*func)(struct resource *, void *));
int kho_fill_kimage(struct kimage *image);
#else
static inline int kho_locate_mem_hole(struct kexec_buf *kbuf,
				      int (*func)(struct resource *, void *))
{
	return 1;
}

static inline int kho_fill_kimage(struct kimage *image) { return 0; }
#endif /* CONFIG_KEXEC_HANDOVER */
#endif /* LINUX_KEXEC_INTERNAL_H */
