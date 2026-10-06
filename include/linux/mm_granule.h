/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MM_GRANULE_H
#define _LINUX_MM_GRANULE_H

#include <linux/mm_types.h>
#include <linux/errno.h>
#include <asm/page.h>

/*
 * User virtual mapping granule. Never use these for physical PFNs, struct
 * page/folio sizes, page-cache indices, or physical memory accounting.
 * The granule is immutable once an mm has mappings or is visible to tasks.
 */
#ifndef arch_mm_page_shift
static inline unsigned int arch_mm_page_shift(const struct mm_struct *mm)
{
	return PAGE_SHIFT;
}
#endif

static inline unsigned int mm_page_shift(const struct mm_struct *mm)
{
	return arch_mm_page_shift(mm);
}

static inline unsigned long mm_page_size(const struct mm_struct *mm)
{
	return 1UL << mm_page_shift(mm);
}

/* A byte-range reference must fit both the user leaf and a native page. */
static inline unsigned long mm_user_fragment_size(const struct mm_struct *mm)
{
	return mm_page_shift(mm) < PAGE_SHIFT ? mm_page_size(mm) : PAGE_SIZE;
}

/* Native physical pages covered by one base user PTE, including fragments. */
static inline unsigned long mm_pte_native_pages(const struct mm_struct *mm)
{
	unsigned long pages = mm_page_size(mm) >> PAGE_SHIFT;

	return pages ?: 1;
}

static inline unsigned long mm_page_mask(const struct mm_struct *mm)
{
	return ~(mm_page_size(mm) - 1);
}

/*
 * ASLR tunables count native-page bits. Retain their byte window when a
 * process uses another granule, including the extra low user-page bits.
 * native_mask must be a contiguous low-bit mask whose byte span fits ulong.
 */
static inline unsigned long mm_aslr_mask(const struct mm_struct *mm,
		unsigned long native_mask)
{
	unsigned int shift = mm_page_shift(mm);

	if (shift < PAGE_SHIFT) {
		unsigned int extra = PAGE_SHIFT - shift;

		return (native_mask << extra) | ((1UL << extra) - 1);
	}
	return native_mask >> (shift - PAGE_SHIFT);
}

/* Virtual-range accounting. Physical pages and cache indices stay native. */
static inline unsigned long vma_user_pages(const struct vm_area_struct *vma)
{
	return (vma->vm_end - vma->vm_start) >> mm_page_shift(vma->vm_mm);
}

static inline unsigned long mm_page_align(const struct mm_struct *mm, unsigned long bytes)
{
	return (bytes + mm_page_size(mm) - 1) & mm_page_mask(mm);
}

/*
 * RSS and virtual counters use the target mm's user granule. Consumers
 * comparing different mms or physical memory must convert explicitly.
 */
static inline unsigned long mm_pages_to_bytes(const struct mm_struct *mm,
		unsigned long pages)
{
	return pages << mm_page_shift(mm);
}

static inline unsigned long mm_pages_to_kb(const struct mm_struct *mm,
		unsigned long pages)
{
	return pages << (mm_page_shift(mm) - 10);
}

/* Round up only when reducing precision; do not hide a partial user page. */
static inline unsigned long mm_pages_to_shift(const struct mm_struct *mm,
		unsigned long pages, unsigned int shift)
{
	unsigned int source = mm_page_shift(mm);

	if (source >= shift)
		return pages << (source - shift);
	return (pages >> (shift - source)) +
		!!(pages & ((1UL << (shift - source)) - 1));
}

static inline unsigned long mm_pages_to_native(const struct mm_struct *mm,
		unsigned long pages)
{
	return mm_pages_to_shift(mm, pages, PAGE_SHIFT);
}

/* A page-count proc ABI uses the reader's granule, including remote reads. */
static inline unsigned long mm_pages_for_reader(const struct mm_struct *mm,
		const struct mm_struct *reader, unsigned long pages)
{
	return mm_pages_to_shift(mm, pages,
				reader ? mm_page_shift(reader) : PAGE_SHIFT);
}

/*
 * Time integrals need one unit across exec. Locked VM also uses this unit:
 * it must represent both logical mappings and native physical charges exactly,
 * including a single native-page pin in a larger-granule userspace mm.
 */
#ifdef CONFIG_MM_SUBPAGE
#define MM_ACCOUNT_SHIFT 12
#else
#define MM_ACCOUNT_SHIFT PAGE_SHIFT
#endif

static inline unsigned long mm_pages_to_account(const struct mm_struct *mm,
		unsigned long pages)
{
	return pages << (mm_page_shift(mm) - MM_ACCOUNT_SHIFT);
}

static inline unsigned long mm_locked_vm_bytes(const struct mm_struct *mm)
{
	return READ_ONCE(mm->locked_vm) << MM_ACCOUNT_SHIFT;
}

/* Physical-page limits must include any partial native page of locked VM. */
static inline unsigned long mm_locked_vm_native_pages(const struct mm_struct *mm)
{
	unsigned long units = READ_ONCE(mm->locked_vm);
	unsigned int shift = PAGE_SHIFT - MM_ACCOUNT_SHIFT;

	return (units >> shift) + !!(units & ((1UL << shift) - 1));
}

/* File-cache indices remain native-page units even for a 4K VMA. */
struct vm_page_offset {
	pgoff_t index;
	unsigned int offset;
};

static inline struct vm_page_offset vm_page_offset_add(struct vm_page_offset pos,
						       unsigned long bytes)
{
	unsigned long low = pos.offset + (bytes & ~PAGE_MASK);

	pos.index += (bytes >> PAGE_SHIFT) + (low >> PAGE_SHIFT);
	pos.offset = low & ~PAGE_MASK;
	return pos;
}

static inline struct vm_page_offset vm_page_offset_sub(struct vm_page_offset pos,
						       unsigned long bytes)
{
	unsigned long low = bytes & ~PAGE_MASK;

	pos.index -= (bytes >> PAGE_SHIFT) + (low > pos.offset);
	pos.offset = (pos.offset - low) & ~PAGE_MASK;
	return pos;
}

static inline bool vm_page_offset_equal(struct vm_page_offset a, struct vm_page_offset b)
{
	return a.index == b.index && a.offset == b.offset;
}

static inline unsigned int vma_subpage_offset(const struct vm_area_struct *vma)
{
#ifdef CONFIG_MM_SUBPAGE
	return vma->vm_subpage_offset;
#else
	return 0;
#endif
}

/* address must be >= vm_start; vm_end computes the next contiguous offset. */
static inline struct vm_page_offset vma_page_offset_at(const struct vm_area_struct *vma,
						      unsigned long address)
{
	unsigned long delta = address - vma->vm_start;
	unsigned long low = (delta & ~PAGE_MASK) + vma_subpage_offset(vma);

	return (struct vm_page_offset) {
		.index = vma->vm_pgoff + (delta >> PAGE_SHIFT) + (low >> PAGE_SHIFT),
		.offset = low & ~PAGE_MASK,
	};
}

/* The start byte of a faulting granule must precede EOF. */
static inline bool vma_file_offset_valid(const struct vm_area_struct *vma,
		unsigned long address, loff_t size)
{
	struct vm_page_offset pos = vma_page_offset_at(vma, address);
	pgoff_t end = size >> PAGE_SHIFT;

	return pos.index < end ||
		(pos.index == end && pos.offset < (size & ~PAGE_MASK));
}

static inline void vma_set_page_offset(struct vm_area_struct *vma, struct vm_page_offset pos)
{
	vma->vm_pgoff = pos.index;
#ifdef CONFIG_MM_SUBPAGE
	vma->vm_subpage_offset = pos.offset;
#endif
}

/* Inverse lookup of a single byte; an offset outside the VMA is not clamped. */
static inline unsigned long vma_address_at_offset(const struct vm_area_struct *vma,
						 struct vm_page_offset pos)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long index, delta, last;
	unsigned int offset = vma_subpage_offset(vma);

	if (!len || pos.index < vma->vm_pgoff || pos.offset >= PAGE_SIZE)
		return -EFAULT;
	index = pos.index - vma->vm_pgoff;
	last = ((len - 1) >> PAGE_SHIFT) +
		((((len - 1) & ~PAGE_MASK) + offset) >> PAGE_SHIFT);
	if (index > last)
		return -EFAULT;
	delta = (index << PAGE_SHIFT) + pos.offset;
	if (delta < offset || delta - offset >= len)
		return -EFAULT;
	return vma->vm_start + delta - offset;
}

#endif
