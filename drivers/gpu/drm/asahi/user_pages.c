// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Long-term pins of user memory as whole native pages, for GEM_USERPTR, and
 * their charge to the user against RLIMIT_MEMLOCK.
 *
 * The GPU maps memory in native pages. A process whose user page size is
 * smaller (CONFIG_MM_SUBPAGE) maps each of its pages to a fragment of a
 * native page, and only the fragment API can pin those. A native page can be
 * given to the GPU only where the process maps all of it, its fragments in
 * order at consecutive addresses: then the GPU sees what the CPU sees, and no
 * memory the process does not map.
 */

#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/mm.h>
#include <linux/mm_subpage.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/user.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>

#include "user_pages.h"

/*
 * Charges @nr_pages to the current user's locked_vm, the count RLIMIT_MEMLOCK
 * bounds for io_uring's registered buffers and iommufd's pinned pages as well
 * (__io_account_mem(), incr_user_locked_vm()). The count belongs to the user,
 * not to an mm: every process of the user shares it, and pages a process pinned
 * stay counted after it exits, until they are released. As both of those, a
 * CAP_IPC_LOCK caller is not charged at all.
 *
 * On success *@out is the charged user, with a reference, for
 * asahi_user_unaccount(), or NULL when nothing was charged. -ENOMEM when the
 * pages would take the user past RLIMIT_MEMLOCK.
 */
int asahi_user_account(unsigned long nr_pages, struct user_struct **out)
{
	struct user_struct *user = current_user();
	unsigned long limit, cur, new;

	*out = NULL;
	if (ns_capable_noaudit(&init_user_ns, CAP_IPC_LOCK))
		return 0;

	limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;
	cur = atomic_long_read(&user->locked_vm);
	do {
		if (check_add_overflow(cur, nr_pages, &new) || new > limit)
			return -ENOMEM;
	} while (!atomic_long_try_cmpxchg(&user->locked_vm, &cur, new));
	*out = get_uid(user);
	return 0;
}

void asahi_user_unaccount(struct user_struct *user, unsigned long nr_pages)
{
	if (!user)
		return;
	if (!WARN_ON(atomic_long_read(&user->locked_vm) < nr_pages))
		atomic_long_sub(nr_pages, &user->locked_vm);
	free_uid(user);
}

#if IS_BUILTIN(CONFIG_KUNIT) && !defined(MODULE)
/* For gem.rs's KUnit tests: adds @delta to the current user's charged pages. */
long asahi_test_user_locked_vm_add(long delta)
{
	return atomic_long_add_return(delta, &current_user()->locked_vm);
}

/* For gem.rs's KUnit tests: the thread's credentials without CAP_IPC_LOCK. */
const struct cred *asahi_test_drop_ipc_lock(void)
{
	struct cred *new = prepare_creds();

	if (!new)
		return NULL;
	cap_lower(new->cap_effective, CAP_IPC_LOCK);
	return override_creds(new);
}

void asahi_test_restore_creds(const struct cred *old)
{
	put_cred(revert_creds(old));
}

/* For gem.rs's KUnit tests: sets the thread's RLIMIT_MEMLOCK, returns the old one. */
unsigned long asahi_test_set_memlock(unsigned long bytes)
{
	struct rlimit *rlim = &current->signal->rlim[RLIMIT_MEMLOCK];
	unsigned long old = rlim->rlim_cur;

	WRITE_ONCE(rlim->rlim_cur, bytes);
	return old;
}
#endif

#ifdef CONFIG_MM_SUBPAGE
/* Pinned fragments of a range; released only through asahi_unpin_user_fragments(). */
struct asahi_user_fragments {
	unsigned long count;
	struct user_page_fragment records[];
};

bool asahi_user_mm_is_subpage(void)
{
	return current->mm && mm_page_size(current->mm) < PAGE_SIZE;
}

void asahi_unpin_user_fragments(struct asahi_user_fragments *frags, bool dirty)
{
	if (!frags)
		return;
	release_user_fragments(frags->records, frags->count, dirty);
	kvfree(frags);
}

/* The native page that @rec and its @n - 1 successors map in order, or NULL. */
static struct page *whole_native_page(const struct user_page_fragment *rec,
				      unsigned long n, unsigned long granule)
{
	unsigned long i;

	if (!IS_ALIGNED(rec->offset, PAGE_SIZE))
		return NULL;
	for (i = 0; i < n; i++)
		if (rec[i].folio != rec->folio || rec[i].length != granule ||
		    rec[i].offset != rec->offset + i * granule)
			return NULL;
	return folio_page(rec->folio, rec->offset >> PAGE_SHIFT);
}

/*
 * Pins @nr_pages native pages' worth of the current process's memory at
 * @start (native-page aligned) and stores the native pages in @pages. All or
 * nothing: -EFAULT if a page cannot be pinned or a native page is not mapped
 * whole and in order; -ENOMEM if the records cannot be allocated.
 */
long asahi_pin_user_fragments(unsigned long start, unsigned long nr_pages,
			      unsigned int gup_flags, struct page **pages,
			      struct asahi_user_fragments **out)
{
	struct mm_struct *mm = current->mm;
	unsigned long granule = mm_page_size(mm);
	unsigned long per_page = PAGE_SIZE / granule;
	unsigned long length = nr_pages << PAGE_SHIFT, done = 0, i;
	struct asahi_user_fragments *frags;
	long ret = 0;

	if (nr_pages > (ULONG_MAX >> PAGE_SHIFT) / per_page)
		return -ENOMEM;
	frags = kvzalloc(struct_size(frags, records, nr_pages * per_page),
			 GFP_KERNEL_ACCOUNT);
	if (!frags)
		return -ENOMEM;
	while (done < length) {
		long n = pin_user_fragments_remote(mm, start + done, length - done,
						   gup_flags, frags->records + frags->count,
						   nr_pages * per_page - frags->count);

		if (n <= 0) {
			ret = n ?: -EFAULT;
			goto fail;
		}
		for (i = frags->count; i < frags->count + n; i++)
			done += frags->records[i].length;
		frags->count += n;
	}
	for (i = 0; i < nr_pages; i++) {
		pages[i] = whole_native_page(&frags->records[i * per_page], per_page, granule);
		if (!pages[i]) {
			ret = -EFAULT;
			goto fail;
		}
	}
	*out = frags;
	return 0;

fail:
	asahi_unpin_user_fragments(frags, false);
	return ret;
}
#else
bool asahi_user_mm_is_subpage(void)
{
	return false;
}

void asahi_unpin_user_fragments(struct asahi_user_fragments *frags, bool dirty)
{
}

long asahi_pin_user_fragments(unsigned long start, unsigned long nr_pages,
			      unsigned int gup_flags, struct page **pages,
			      struct asahi_user_fragments **out)
{
	return -EOPNOTSUPP;
}
#endif
