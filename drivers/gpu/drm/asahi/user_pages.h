/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ASAHI_USER_PAGES_H
#define _ASAHI_USER_PAGES_H

#include <linux/types.h>

struct asahi_user_fragments;
struct cred;
struct page;
struct user_struct;

/* Called from gem.rs. */
bool asahi_user_mm_is_subpage(void);
long asahi_pin_user_fragments(unsigned long start, unsigned long nr_pages,
			      unsigned int gup_flags, struct page **pages,
			      struct asahi_user_fragments **out);
void asahi_unpin_user_fragments(struct asahi_user_fragments *frags, bool dirty);
int asahi_user_account(unsigned long nr_pages, struct user_struct **out);
void asahi_user_unaccount(struct user_struct *user, unsigned long nr_pages);

#if IS_BUILTIN(CONFIG_KUNIT) && !defined(MODULE)
long asahi_test_user_locked_vm_add(long delta);
const struct cred *asahi_test_drop_ipc_lock(void);
void asahi_test_restore_creds(const struct cred *old);
unsigned long asahi_test_set_memlock(unsigned long bytes);
#endif

#endif
