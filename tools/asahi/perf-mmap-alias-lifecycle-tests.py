#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Duplicate-alias and failed-alias lifecycle regression tests for the perf
mmap locked-memory accounting (normal ring and AUX area).

The tool compiles the accounting code straight out of each tree under test
(kernel/events/core.c + internal.h: perf_mmap, perf_mmap_rb, perf_mmap_aux,
perf_mmap_close, perf_mmap_account, perf_mmap_unaccount, perf_mmap_calc_limits
and the internal.h inline helpers) into a host harness and drives full
lifecycles through it:

  - duplicate aliases of the normal ring: charge per alias, uncharge once at
    final close (user-reported leak), in both close orders -- aliases first
    and the original mapping first while every alias is still live,
  - duplicate aliases of the AUX area: charge per alias, uncharge once at the
    last AUX close (user-reported leak), in both close orders -- AUX first
    and the normal ring VMA first while the AUX area stays live,
  - failed aliases: AUX first-map failures (-EPERM limit check, rb_alloc_aux
    failure, early -EINVAL), and map_range() failures through the real
    perf_mmap() dispatcher error path, both for a first mapping (minimal
    perf_mmap_close()) and for an alias (full perf_mmap_close()),
  - redirected events (perf_event_set_output): a second event with
    mmap_count == 0 attached to another event's rb.  Its first mmap can
    only be the AUX mmap (a first ring mmap of a redirected event is
    rejected -EBUSY in perf_mmap_rb).  On the unmodified tree,
    perf_mmap_aux() increments the zero count, which WARNs and
    SATURATES (REFCOUNT_ADD_UAF); it does not produce 1.  The scenarios
    assert the desired behaviour (no saturation, R->mmap_count == 1,
    full unwind on map_range failure: rb->mmap_count back to the
    owner's count, the redirect detached, the AUX allocation freed, and
    the shared user counter and both mms individually restored).
    Note the dispatcher's "first mmap" minimal perf_mmap_close() branch
    (mmap_count == 1) is therefore NOT reachable for a redirected
    event's first AUX mmap on unmodified code -- the count reads
    REFCOUNT_SATURATED, not 1 -- and the saturation itself is the
    observed defect: later decrements cannot release that count, and
    the failed mmap leaves R attached to the rb. Setting the count alone
    would newly expose the minimal-close branch to redirected AUX mmaps,
    whose unwinding does not fit it.
  - focused redirected-AUX cases, each stating the exact expected
    redirect mmap_count, rb counters, event list, AUX allocation
    lifetime and charge at every step:
      * the owner's AUX area is ALREADY mapped and the redirected event
        maps it too (perf_mmap_aux()'s rb_has_aux() alias branch),
        succeeding and failing at map_range(),
      * the owner's ring VMA closes FIRST while the redirected AUX
        mapping is still live,
      * the redirected event maps the AUX area a SECOND time
        (mmap_count 1 -> 2),
      * the redirected event maps the NORMAL ring after its first AUX
        map (perf_mmap_rb()'s alias branch).  The current tree admits
        this, and both events then share ONE user_page.  The source
        permits updates from either event to overwrite the other's
        metadata; the harness does not test the contents of that page.
        This change does not alter that pre-existing behavior. The ring
        alias's charge follows the
        tree under test (measured like the AUX alias charge) and is
        recorded as known_ring_alias_charge.
  - real limit splitting through the tree's own perf_mmap_calc_limits()
    (partial spill and full spill into pinned_vm),
  - ring geometries: full data ring, metadata-only ring, and (user4k trees)
    a sub-native-page data ring on a 4K-page mm.

This is a source-level test: the extracted C is the tree's real code, but
VMA/file/user/mutex plumbing, map_range(), rb_alloc()/rb_alloc_aux() page
backing and ring_buffer_attach() are signature-faithful host stubs, it is
single-threaded, and nothing runs in a kernel.  It is not evidence about
hardware or a booted kernel; it is evidence about the source's charge/
uncharge/refcount behaviour.

Assertions encode the desired invariant (every counter and refcount returns
to its initial value when a lifecycle completes; perf_mlock and locked_vm
are checked individually, so a transfer between them cannot mask a leak).
A scenario that ends with
a nonzero counter therefore FAILS and prints the exact leaked page count --
leaks are reported, not hidden.  The tool exits nonzero if any invariant
broke: a tree that charges every alias fails the duplicate-alias scenarios;
a tree whose aliases share the first map's single charge passes the tested
single-user, single-threaded lifecycles, subject to the cases below.

On an older tree that charges per alias, a scenario that maps the same area
twice also picks up the known per-alias charge (#947/#948). Such a scenario
records that residual in known_alias_charge / known_ring_alias_charge, and
invariants_broken() prints it as KNOWN instead of failing on it, so the known
defect stays visible without masking the redirect-specific assertions.
Every other number in those scenarios is still checked exactly, and the
duplicate-alias scenarios themselves still FAIL on the known defect.

RESULT lines distinguish the two clean outcomes: status=PASS means every
invariant held with no residual at all; status=PASS-KNOWN-RESIDUAL means
the scenario's own assertions held but it ended on the measured per-alias
residual.  A FAIL is always a broken invariant.

Each tree under test is reported with its exact provenance before the
run: worktree path, git HEAD commit and the dirty state of kernel/events
and tools/asahi, so a result line is never attributed to an unidentified
source.
"""

from pathlib import Path
import argparse
import re
import subprocess
import sys
import tempfile
from string import Template

HERE = Path(__file__).resolve().parent

DEFAULT_TREES = [base for base in HERE.parents
                 if (base / 'kernel/events/core.c').is_file()]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tree', type=Path, action='append', dest='trees',
                    default=None,
                    help='kernel tree holding kernel/events '
                         '(repeatable; default: the kernel tree this test '
                         'lives in)')
parser.add_argument('--cc', default='cc', help='C compiler (default: cc)')
args = parser.parse_args()
if args.trees is None:
    if not DEFAULT_TREES:
        parser.error('--tree is required: no kernel/events/core.c found '
                     'above %s' % HERE)
    args.trees = DEFAULT_TREES


def definition(text, name):
    """Return the C text of a function definition, not of its prototype."""
    for match in re.finditer(r'\b' + re.escape(name) + r'\s*\(', text):
        index = match.end() - 1
        depth = 0
        while index < len(text):
            if text[index] == '(':
                depth += 1
            elif text[index] == ')':
                depth -= 1
                if depth == 0:
                    break
            index += 1
        brace = text.find('{', index)
        semi = text.find(';', index)
        if brace == -1 or (semi != -1 and semi < brace):
            continue
        if '*/' in text[match.start():brace]:
            continue	# match sat inside a comment; find the real definition
        depth = 0
        for position in range(brace, len(text)):
            if text[position] == '{':
                depth += 1
            elif text[position] == '}':
                depth -= 1
                if depth == 0:
                    start = text.rfind('\n', 0, match.start()) + 1
                    return text[start:position + 1]
    return None


def rb_alloc_profile(ring):
    """Detect the rb_alloc() shape: user4k data_size form or plain nr_pages."""
    func = definition(ring, 'rb_alloc')
    if func is None:
        raise SystemExit('no rb_alloc() definition in ring_buffer.c')
    head = func.split(')')[0]
    user4k = 'data_size' in head
    match = re.search(r'int nr_pages\s*=\s*([^;]+);', func)
    nr_pages_expr = match.group(1).strip() if match else 'nr_pages'
    match = re.search(r'rb->data_page_shift\s*=\s*([^;]+);', func)
    return user4k, nr_pages_expr, match.group(1).strip() if match else None


def rb_init_lines(ring):
    """The mmap_count/mmap_user initialisation the tree does at rb_alloc."""
    lines = []
    if re.search(r'rb->mmap_user\s*=\s*get_current_user\(\);', ring):
        lines.append('\trb->mmap_user = current_user();')
    if re.search(r'refcount_set\(&rb->mmap_count,\s*1\);', ring):
        lines.append('\trefcount_set(&rb->mmap_count, 1);')
    return '\n'.join(lines)


HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define RB_USER4K $RB_USER4K

typedef long atomic_long_t;
typedef long long atomic64_t;
typedef unsigned long long u64;
typedef unsigned long pgoff_t;

#define PAGE_SHIFT 14
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ilog2(n) (31 - __builtin_clz((unsigned int)(n)))
#define READ_ONCE(x) (x)
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)
#define VM_SHARED 0x1
#define VM_WRITE 0x2
#define VM_DONTCOPY 0x0
#define VM_DONTEXPAND 0x0
#define VM_DONTDUMP 0x0
#define CAP_IPC_LOCK 14
#define RLIMIT_MEMLOCK 0
#define PERF_EVENT_STATE_REVOKED (-1)

/* Warning model.  warn_count counts every WARN event; warn_lines counts
 * the lines the KERNEL would print.  WARN_ON() prints every time, while
 * WARN_ONCE() prints only the first time per call site -- and every
 * refcount saturation type is its own WARN_ONCE site in lib/refcount.c.
 * Each scenario models a fresh boot, so the "already printed" state is
 * reset with the rest of the harness state. */
#define WARN_SITES 64
static int warn_count;		/* every WARN event */
static int warn_lines;		/* lines the kernel would print */
static int warn_printed;
static int warn_site_line[WARN_SITES];
static int warn_site_n;

static bool warn_site_seen(int line)
{
	int i;

	for (i = 0; i < warn_site_n; i++)
		if (warn_site_line[i] == line)
			return true;
	return false;
}

static void warn_line(const char *msg)
{
	warn_lines++;
	if (warn_printed++ < 40)
		printf("    WARN %s\n", msg);
}

static void warn_hit(int condition, const char *tag, int line)
{
	char msg[64];

	if (!condition)
		return;
	warn_count++;
	snprintf(msg, sizeof(msg), "%s line %d", tag, line);
	warn_line(msg);
}

static void warn_hit_once(int condition, const char *tag, int line)
{
	char msg[64];

	if (!condition)
		return;
	warn_count++;
	if (warn_site_seen(line))
		return;
	if (warn_site_n < WARN_SITES)
		warn_site_line[warn_site_n++] = line;
	snprintf(msg, sizeof(msg), "%s line %d", tag, line);
	warn_line(msg);
}
#define WARN_ON(c) warn_hit(!!(c), "WARN_ON", __LINE__)
#define WARN_ON_ONCE(c) warn_hit_once(!!(c), "WARN_ON_ONCE", __LINE__)

/* --- environment knobs the real code reads ---------------------------- */
static long sysctl_perf_event_mlock = 256UL << 20;	/* KiB, kernel units */
static unsigned long h_rlimit_memlock = 1UL << 40;	/* bytes */
static int h_paranoid = 1;
static int h_capable = 0;
static int h_cpus = 1;
static int rb_alloc_aux_fail = 0;
static int map_range_fail = 0;

static int num_online_cpus(void) { return h_cpus; }
static unsigned long rlimit(int which) { (void)which; return h_rlimit_memlock; }
static int perf_is_paranoid(void) { return h_paranoid; }
static int capable(int cap) { (void)cap; return h_capable; }

/* --- shims ------------------------------------------------------------ */
struct user_struct {
	atomic_long_t perf_mlock;
	atomic_long_t locked_vm;
};
static struct user_struct test_user;
static struct user_struct *current_user(void) { return &test_user; }
#define get_current_user() current_user()

struct mm_struct {
	atomic64_t pinned_vm;
	unsigned int page_shift;
};
static unsigned int mm_page_shift(struct mm_struct *mm) { return mm->page_shift; }
static unsigned long mm_page_size(struct mm_struct *mm) { return 1UL << mm->page_shift; }

struct file;
struct perf_event;
struct perf_buffer;
struct vm_operations_struct;

struct vm_area_struct {
	unsigned long vm_start, vm_end;
	unsigned long vm_pgoff;
	unsigned long vm_flags;
	unsigned int vm_subpage_offset;
	struct mm_struct *vm_mm;
	struct file *vm_file;
	const struct vm_operations_struct *vm_ops;
};
static inline unsigned int vma_subpage_offset(const struct vm_area_struct *vma)
{
	return vma->vm_subpage_offset;
}
#define vm_flags_set(v, f) ((void)(f))

struct file { void *private_data; };

struct perf_event_mmap_page {
	u64 aux_offset;
	u64 aux_size;
};

struct list_head { struct list_head *next, *prev; };
#define INIT_LIST_HEAD(l) do { (l)->next = (l)->prev = (l); } while (0)
static void list_add_rcu(struct list_head *new_, struct list_head *head)
{
	new_->next = head->next;
	new_->prev = head;
	head->next->prev = new_;
	head->next = new_;
}
static void list_del_rcu(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	entry->prev = entry->next = entry;
}
#define container_of_(p, t, m) ((t *)((char *)(p) - __builtin_offsetof(t, m)))
#define list_for_each_entry_rcu(pos, head, m) \
	for (pos = container_of_((head)->next, typeof(*pos), m); \
	     &pos->m != (head); \
	     pos = container_of_(pos->m.next, typeof(*pos), m))
static void rcu_read_lock(void) { }
static void rcu_read_unlock(void) { }

/* refcount_t with the kernel's real saturating semantics
 * (include/linux/refcount.h, lib/refcount.c): the counter clamps at
 * REFCOUNT_SATURATED and never moves once there.
 *
 * Saturation warns through WARN_ONCE() in lib/refcount.c, so each
 * saturation TYPE prints one line per boot however often it fires:
 * warn_count counts the events, warn_lines the lines a real kernel
 * would print. */
#define REFCOUNT_SATURATED (INT_MIN / 2)

enum refcount_saturation_type {
	REFCOUNT_ADD_NOT_ZERO_OVF,	/* "saturated; leaking memory" */
	REFCOUNT_ADD_OVF,		/* "saturated; leaking memory" */
	REFCOUNT_ADD_UAF,		/* "addition on 0; use-after-free" */
	REFCOUNT_SUB_UAF,		/* "underflow; use-after-free" */
	REFCOUNT_DEC_LEAK,		/* "decrement hit 0; leaking memory" */
};

static const char *const refcount_sat_text[] = {
	"saturated; leaking memory",
	"saturated; leaking memory",
	"addition on 0; use-after-free",
	"underflow; use-after-free",
	"decrement hit 0; leaking memory",
};

typedef struct { int refs; } refcount_t;
static void refcount_set(refcount_t *r, int v) { r->refs = v; }
static int refcount_read(refcount_t *r) { return r->refs; }

/* lib/refcount.c: refcount_warn_saturate() -- clamp and WARN_ONCE. */
static bool refcount_sat_warned[5];

static void refcount_warn_saturate(refcount_t *r, int type)
{
	r->refs = REFCOUNT_SATURATED;
	warn_count++;
	if (!refcount_sat_warned[type]) {
		refcount_sat_warned[type] = true;
		warn_line(refcount_sat_text[type]);
	}
}

/* __refcount_add_not_zero(): no-op at 0, saturates on negative/overflow */
static bool refcount_inc_not_zero(refcount_t *r)
{
	int old = r->refs;

	if (!old)
		return false;
	if (old < 0 || old + 1 < 0) {
		refcount_warn_saturate(r, REFCOUNT_ADD_NOT_ZERO_OVF);
		return true;	/* the kernel still returns old (nonzero) */
	}
	r->refs++;
	return true;
}
/* __refcount_add(): refcount_inc() from 0 warns (REFCOUNT_ADD_UAF) and
 * saturates -- it does NOT yield 1 */
static void refcount_inc(refcount_t *r)
{
	int old = r->refs++;

	if (!old)
		refcount_warn_saturate(r, REFCOUNT_ADD_UAF);
	else if (old < 0 || old + 1 < 0)
		refcount_warn_saturate(r, REFCOUNT_ADD_OVF);
}
/* __refcount_sub_and_test(): success only on reaching exactly 0; warns
 * (REFCOUNT_SUB_UAF) and fails to move when saturated or on underflow */
static bool refcount_dec_and_test(refcount_t *r)
{
	int old = r->refs--;

	if (old - 1 == 0)
		return true;
	if (old <= 0 || old - 1 < 0)
		refcount_warn_saturate(r, REFCOUNT_SUB_UAF);
	return false;
}
/* __refcount_dec(): warns (REFCOUNT_DEC_LEAK) at old <= 1 and saturates */
static void refcount_dec(refcount_t *r)
{
	int old = r->refs--;

	if (old <= 1)
		refcount_warn_saturate(r, REFCOUNT_DEC_LEAK);
}

/* lib/refcount.c: refcount_dec_not_one() -- decrement unless the count
 * is 1, in which case it returns false and leaves the count alone.  At
 * REFCOUNT_SATURATED it returns true WITHOUT warning and WITHOUT moving
 * the counter: a saturated count is not decremented and not reported. */
static bool refcount_dec_not_one(refcount_t *r)
{
	unsigned int val = (unsigned int)r->refs;

	if (val == (unsigned int)REFCOUNT_SATURATED)
		return true;		/* saturated: silent, no decrement */
	if (val == 1)
		return false;		/* the caller takes it to zero */
	if (val == 0) {
		/* underflow: WARN_ONCE, count left alone */
		warn_hit_once(1, "refcount_t: underflow; use-after-free",
			      __LINE__);
		return true;
	}
	r->refs = (int)(val - 1);
	return true;
}

struct mutex { int locked; };
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked = 1; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked = 0; }
static void mutex_init(struct mutex *m) { m->locked = 0; }
static void h_mutex_unlock_cb(struct mutex **m)
{
	if (*m)
		mutex_unlock(*m);	/* scoped_guard step already nulled it */
}
/* lib/refcount.c: refcount_dec_and_mutex_lock() -- the dec_not_one()
 * fast path runs FIRST: a count above 1 is decremented there (without the
 * mutex) and the function reports failure, a saturated count is left
 * alone and reports failure SILENTLY, and only a count of exactly 1
 * reaches the mutex-and-drop-to-zero path. */
static bool refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *m)
{
	if (refcount_dec_not_one(r))
		return false;

	mutex_lock(m);
	if (!refcount_dec_and_test(r)) {
		mutex_unlock(m);
		return false;
	}
	return true;
}
#define H_CAT_(a, b) a##b
#define H_CAT(a, b) H_CAT_(a, b)
#define scoped_guard(scope, lock) \
	for (struct mutex *H_CAT(h_sgl_, __LINE__) \
	     __attribute__((cleanup(h_mutex_unlock_cb))) = \
	     (mutex_lock(lock), (struct mutex *)(lock)), \
	     *H_CAT(h_sgq_, __LINE__) = H_CAT(h_sgl_, __LINE__); \
	     H_CAT(h_sgq_, __LINE__) != NULL; \
	     H_CAT(h_sgq_, __LINE__) = NULL)
/* The tree writes guard(mutex)(&lock): the expansion must end with the
 * guard function so the trailing parenthesised group becomes its call. */
static struct mutex *h_mutex_guard_fn(struct mutex *m)
{
	mutex_lock(m);
	return m;
}
#define guard(scope) \
	struct mutex *H_CAT(h_g_, __LINE__) \
	    __attribute__((cleanup(h_mutex_unlock_cb))) = h_mutex_guard_fn

#define RING_BUFFER_WRITABLE 0x01
#define atomic_long_sub(v, p) (*(p) -= (v))
#define atomic_long_add(v, p) (*(p) += (v))
#define atomic_long_set(p, v) (*(p) = (v))
#define atomic_long_read(p) (*(p))
#define atomic64_sub(v, p) (*(p) -= (v))
#define atomic64_add(v, p) (*(p) += (v))
#define atomic64_read(p) (*(p))
#define atomic_long_inc_not_zero(p) \
	({ long *h_p_ = (long *)(p); bool h_r_ = false; \
	   if (*h_p_ > 0) { (*h_p_)++; h_r_ = true; } \
	   h_r_; })
#define likely(x) (x)
#define unlikely(x) (x)

static bool is_power_of_2(unsigned long n)
{
	return n != 0 && (n & (n - 1)) == 0;
}

typedef void (*mapped_f)(struct perf_event *, struct mm_struct *);
enum perf_event_mapped_state { event_unmapped, event_mapped };
static inline mapped_f get_mapped(struct perf_event *event, int which)
{
	(void)event; (void)which;
	return NULL;
}

struct vm_operations_struct { int unused_; };
static const struct vm_operations_struct perf_mmap_vmops = { 0 };

/* --- tree shapes ------------------------------------------------------- */
struct perf_buffer {
	refcount_t refcount;
	refcount_t mmap_count;
	refcount_t aux_mmap_count;
	refcount_t aux_refcount;
	unsigned long mmap_locked;
	unsigned long aux_mmap_locked;
	struct user_struct *mmap_user;
	struct list_head event_list;
	struct mutex aux_mutex;
	int nr_pages;
	int aux_nr_pages;
	long aux_watermark;
	struct perf_event_mmap_page *user_page;
#if RB_USER4K
	unsigned int data_page_shift;
	unsigned int mmap_page_shift;
	u64 aux_offset;
#else
	unsigned long aux_pgoff;
	int page_order;
#endif
};

struct perf_event_context { void *parent_ctx; };
struct perf_event_attr {
	long watermark;
	long wakeup_watermark;
	long aux_watermark;
	int inherit;
};
struct perf_event {
	int cpu;
	int state;
	struct perf_event_attr attr;
	struct perf_event_context *ctx;
	/* atomic_long_t as in the tree: the rb->event_list walk in
	 * perf_mmap_close() takes a ref on every event, so the stub must
	 * hold list-sized counts */
	atomic_long_t refcount;
	refcount_t mmap_count;
	struct mutex mmap_mutex;
	struct perf_buffer *rb;
	struct list_head rb_entry;
};

/* --- stub declarations (definitions below the extracted helpers) ------- */
$rb_alloc_decl
$rb_alloc_aux_decl
static void rb_free_aux(struct perf_buffer *rb);
static void rb_free(struct perf_buffer *rb);
static struct perf_buffer *ring_buffer_get(struct perf_event *event);
static void ring_buffer_put(struct perf_buffer *rb);
static void ring_buffer_attach(struct perf_event *event, struct perf_buffer *rb);
static void perf_event_update_time(struct perf_event *event);
static void perf_event_init_userpage(struct perf_event *event);
static void perf_event_update_userpage(struct perf_event *event);
static void perf_pmu_output_stop(struct perf_event *event);
static int security_perf_event_read(struct perf_event *event);
static void put_event(struct perf_event *event);
static int map_range(struct perf_buffer *rb, struct vm_area_struct *vma);

/* --- accounting code taken from the tree under test -------------------- */
$helpers

/* --- harness stub definitions ------------------------------------------ */
static int rb_allocs, rb_frees, aux_allocs, aux_frees, map_range_calls;

#if RB_USER4K
struct perf_buffer *rb_alloc(unsigned long data_size, unsigned int mmap_page_shift,
			     long watermark, int cpu, int flags)
{
	struct perf_buffer *rb;
	int nr_pages = $rb_nr_pages;

	(void)watermark; (void)cpu; (void)flags;
	rb = calloc(1, sizeof(*rb) + nr_pages * sizeof(void *));
	if (!rb)
		return NULL;
	rb->nr_pages = nr_pages;
	rb->mmap_page_shift = mmap_page_shift;
$rb_data_shift
	rb->user_page = calloc(1, sizeof(*rb->user_page));
	if (!rb->user_page) {
		free(rb);
		return NULL;
	}
$rb_init
	INIT_LIST_HEAD(&rb->event_list);
	mutex_init(&rb->aux_mutex);
	refcount_set(&rb->refcount, 1);
	rb_allocs++;
	return rb;
}
#else
struct perf_buffer *rb_alloc(int nr_pages, long watermark, int cpu, int flags)
{
	struct perf_buffer *rb;

	(void)watermark; (void)cpu; (void)flags;
	rb = calloc(1, sizeof(*rb) + nr_pages * sizeof(void *));
	if (!rb)
		return NULL;
	rb->nr_pages = nr_pages;
	rb->page_order = 0;
	rb->user_page = calloc(1, sizeof(*rb->user_page));
	if (!rb->user_page) {
		free(rb);
		return NULL;
	}
$rb_init
	INIT_LIST_HEAD(&rb->event_list);
	mutex_init(&rb->aux_mutex);
	refcount_set(&rb->refcount, 1);
	rb_allocs++;
	return rb;
}
#endif

$rb_alloc_aux_def

static void rb_free_aux(struct perf_buffer *rb)
{
	assert(rb->aux_nr_pages);
	rb->aux_nr_pages = 0;
	refcount_set(&rb->aux_refcount, 0);
	aux_frees++;
}

static void rb_free(struct perf_buffer *rb)
{
	rb_frees++;
	free(rb->user_page);
	free(rb);
}

/* Mirrors the tree's ring_buffer_attach(): the rb_alloc() initial refcount
 * IS the first event's attach reference, attach() adds no reference, and
 * detach puts the old rb (which frees it when it hits zero). */
static void ring_buffer_attach(struct perf_event *event, struct perf_buffer *rb)
{
	struct perf_buffer *old_rb = NULL;

	if (rb && rb == event->rb)
		return;
	if (event->rb) {
		old_rb = event->rb;
		list_del_rcu(&event->rb_entry);
	}
	event->rb = rb;
	if (rb)
		list_add_rcu(&event->rb_entry, &rb->event_list);
	if (old_rb)
		ring_buffer_put(old_rb);
}

static struct perf_buffer *ring_buffer_get(struct perf_event *event)
{
	struct perf_buffer *rb = event->rb;

	if (rb)
		refcount_inc(&rb->refcount);
	return rb;
}

static void ring_buffer_put(struct perf_buffer *rb)
{
	if (refcount_dec_and_test(&rb->refcount))
		rb_free(rb);
}

/* Number of events currently attached to the rb (the tree's
 * rb->event_list membership: the owner's mmap plus every set_output
 * redirect still attached). */
static int event_list_len(struct perf_buffer *rb)
{
	struct perf_event *pos;
	int n = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(pos, &rb->event_list, rb_entry)
		n++;
	rcu_read_unlock();
	return n;
}

/* perf_event_set_output(R, A) attach side, signature-faithful to the
 * tree: ring_buffer_get(output_event) takes one rb->refcount for the
 * redirect and ring_buffer_attach() puts R on rb->event_list.  The
 * set_output() validation is syscall plumbing outside the extracted
 * accounting code and is satisfied by construction here; the tree
 * requires: R != A, same cpu (same task when cpu == -1), same clockid,
 * same write_backward, same PMU when both events generate AUX,
 * output_event not REVOKED, and R->mmap_count == 0 ("Can't redirect
 * output if we've got an active mmap()").  That last precondition is
 * what makes R's next mmap() its FIRST mmap of the shared rb. */
static void set_output(struct perf_event *from, struct perf_event *to)
{
	struct perf_buffer *rb = ring_buffer_get(to);

	assert(refcount_read(&from->mmap_count) == 0);
	ring_buffer_attach(from, rb);
}

static void perf_event_update_time(struct perf_event *event) { (void)event; }
static void perf_event_init_userpage(struct perf_event *event) { (void)event; }
static void perf_event_update_userpage(struct perf_event *event) { (void)event; }
static void perf_pmu_output_stop(struct perf_event *event) { (void)event; }
static int security_perf_event_read(struct perf_event *event) { (void)event; return 0; }
static void put_event(struct perf_event *event) { (void)event; }

static int map_range(struct perf_buffer *rb, struct vm_area_struct *vma)
{
	(void)rb; (void)vma;
	map_range_calls++;
	if (map_range_fail)
		return -ENOMEM;
	return 0;
}

/* --- lifecycle driver --------------------------------------------------- */
static struct perf_event ev;
static struct perf_event_context ctx;
static struct mm_struct mm;
static struct file file;
/* the set_output-redirected event: a second event sharing the owner's
 * rb, with its own mm (its AUX vma charges/uncharges that mm) */
static struct perf_event ev_redirect;
static struct perf_event_context ctx_redirect;
static struct mm_struct mm_redirect;
static struct file file_redirect;

static void reset_all(unsigned int mm_shift)
{
	memset(&ev, 0, sizeof(ev));
	memset(&ctx, 0, sizeof(ctx));
	memset(&mm, 0, sizeof(mm));
	memset(&file, 0, sizeof(file));
	ev.cpu = 0;
	ev.ctx = &ctx;
	ev.state = 0;
	mutex_init(&ev.mmap_mutex);
	atomic_long_set(&ev.refcount, 2);
	mm.page_shift = mm_shift;
	file.private_data = &ev;
	memset(&ev_redirect, 0, sizeof(ev_redirect));
	memset(&ctx_redirect, 0, sizeof(ctx_redirect));
	memset(&mm_redirect, 0, sizeof(mm_redirect));
	memset(&file_redirect, 0, sizeof(file_redirect));
	ev_redirect.cpu = 0;
	ev_redirect.ctx = &ctx_redirect;
	ev_redirect.state = 0;
	mutex_init(&ev_redirect.mmap_mutex);
	atomic_long_set(&ev_redirect.refcount, 2);
	mm_redirect.page_shift = mm_shift;
	file_redirect.private_data = &ev_redirect;
	test_user.perf_mlock = 0;
	test_user.locked_vm = 0;
	mm.pinned_vm = 0;
	mm_redirect.pinned_vm = 0;
	rb_allocs = rb_frees = aux_allocs = aux_frees = 0;
	map_range_calls = 0;
	known_alias_charge = 0;
	known_ring_alias_charge = 0;
	warn_count = 0;
	warn_lines = 0;
	warn_printed = 0;
	warn_site_n = 0;
	/* WARN_ONCE() is once per boot, and every scenario models its own
	 * boot, so the "already printed" state resets with the rest. */
	memset(refcount_sat_warned, 0, sizeof(refcount_sat_warned));
	rb_alloc_aux_fail = 0;
	map_range_fail = 0;
	sysctl_perf_event_mlock = 256UL << 20;
	h_rlimit_memlock = 1UL << 40;
	h_paranoid = 1;
	h_capable = 0;
	h_cpus = 1;
}

/* Point user_lock_limit at `pages` pages for one CPU (kernel units: KiB). */
static void set_user_lock_limit(unsigned long pages)
{
	sysctl_perf_event_mlock = (long)pages << (PAGE_SHIFT - 10);
}

static struct vm_area_struct make_vma_bytes_mm(unsigned long bytes,
					       unsigned long pgoff,
					       struct mm_struct *vmm,
					       struct file *vfile)
{
	struct vm_area_struct v;

	memset(&v, 0, sizeof(v));
	v.vm_start = 0x100000000UL;
	v.vm_end = v.vm_start + bytes;
	v.vm_pgoff = pgoff;
	v.vm_flags = VM_SHARED | VM_WRITE;
	v.vm_mm = vmm;
	v.vm_file = vfile;
	return v;
}

static struct vm_area_struct make_vma_bytes(unsigned long bytes, unsigned long pgoff)
{
	return make_vma_bytes_mm(bytes, pgoff, &mm, &file);
}

static void dump(const char *tag)
{
	struct perf_buffer *rb = ev.rb;

	printf("    %-26s perf_mlock=%ld locked_vm=%ld pinned=%ld "
	       "ev.mmap_count=%d rb->mmap_count=%d rb=%d "
	       "alloc=%d/%d aux=%d/%d warns=%d\n", tag,
	       test_user.perf_mlock, test_user.locked_vm, (long)mm.pinned_vm,
	       ev.mmap_count.refs, rb ? rb->mmap_count.refs : -1, rb != NULL,
	       rb_allocs, rb_frees, aux_allocs, aux_frees, warn_count);
}

static long user_perf_mlock(void) { return test_user.perf_mlock; }
static long user_locked_vm(void) { return test_user.locked_vm; }

/* The redirect-side state the tree's own accounting must leave after a
 * step: the redirected event's mmap_count, the shared rb's mapping and
 * AUX mapping counts, the rb's reference count and event list, the AUX
 * allocation's lifetime and the user-side charge. */
struct redirect_state {
	int redirect_mc;
	int rb_mmap;
	int rb_aux_mmap;
	int rb_refcount;
	int list;
	int aux_allocs;
	int aux_frees;
	long charge;
};

static void dump_redirect(const char *tag, struct perf_buffer *rb)
{
	printf("    %-26s perf_mlock=%ld locked_vm=%ld pinned=%ld "
	       "redirect.mc=%d rb->mmap_count=%d rb->aux_mmap_count=%d "
	       "rb->refcount=%d list=%d aux=%d/%d warns=%d/%d\n", tag,
	       test_user.perf_mlock, test_user.locked_vm, (long)mm.pinned_vm,
	       ev_redirect.mmap_count.refs,
	       rb ? rb->mmap_count.refs : -1,
	       rb ? rb->aux_mmap_count.refs : -1,
	       rb ? rb->refcount.refs : -1,
	       rb ? event_list_len(rb) : -1,
	       aux_allocs, aux_frees, warn_count, warn_lines);
}

/* The rb is freed by the last close, so this variant reports only state
 * that outlives it. */
static void dump_redirect_final(const char *tag)
{
	printf("    %-26s perf_mlock=%ld locked_vm=%ld pinned=%ld "
	       "redirect.mc=%d redirect.rb=%d rb=%d/%d aux=%d/%d warns=%d/%d\n",
	       tag, test_user.perf_mlock, test_user.locked_vm,
	       (long)mm.pinned_vm, ev_redirect.mmap_count.refs,
	       ev_redirect.rb != NULL, rb_allocs, rb_frees, aux_allocs,
	       aux_frees, warn_count, warn_lines);
}

/* Only call this while the rb is alive (the last close frees it). */
static int redirect_state_broken(const char *what, struct perf_buffer *rb,
				 const struct redirect_state *want)
{
	int bad = 0;

	if (ev_redirect.mmap_count.refs != want->redirect_mc) {
		printf("    OBSERVED %s: redirect mmap_count %d, expected %d\n",
		       what, ev_redirect.mmap_count.refs, want->redirect_mc);
		bad = 1;
	}
	if (rb->mmap_count.refs != want->rb_mmap) {
		printf("    OBSERVED %s: rb->mmap_count %d, expected %d\n",
		       what, rb->mmap_count.refs, want->rb_mmap);
		bad = 1;
	}
	if (rb->aux_mmap_count.refs != want->rb_aux_mmap) {
		printf("    OBSERVED %s: rb->aux_mmap_count %d, expected %d\n",
		       what, rb->aux_mmap_count.refs, want->rb_aux_mmap);
		bad = 1;
	}
	if (rb->refcount.refs != want->rb_refcount) {
		printf("    OBSERVED %s: rb->refcount %d, expected %d\n",
		       what, rb->refcount.refs, want->rb_refcount);
		bad = 1;
	}
	if (event_list_len(rb) != want->list) {
		printf("    OBSERVED %s: rb event list %d, expected %d\n",
		       what, event_list_len(rb), want->list);
		bad = 1;
	}
	if (aux_allocs != want->aux_allocs || aux_frees != want->aux_frees) {
		printf("    OBSERVED %s: AUX allocations %d/%d, expected %d/%d\n",
		       what, aux_allocs, aux_frees,
		       want->aux_allocs, want->aux_frees);
		bad = 1;
	}
	if (user_$charge_counter() != want->charge) {
		printf("    OBSERVED %s: $charge_counter %ld, expected %ld\n",
		       what, user_$charge_counter(), want->charge);
		bad = 1;
	}
	return bad;
}

/* The tree charges exactly one user counter ($charge_counter) and must
 * leave the other ($spare_counter) untouched: assert both individually,
 * so a transfer between them cannot pose as unchanged or balanced. */
static int user_charge_broken(const char *what, long want)
{
	int bad = 0;

	if (user_$charge_counter() != want) {
		printf("    OBSERVED %s: $charge_counter %ld, expected %ld\n",
		       what, user_$charge_counter(), want);
		bad = 1;
	}
	if (user_$spare_counter() != 0) {
		printf("    OBSERVED %s: $spare_counter nonzero (%ld), expected 0\n",
		       what, user_$spare_counter());
		bad = 1;
	}
	return bad;
}

static int invariants_broken(const char *what)
{
	int bad = 0;
	struct perf_buffer *rb = ev.rb;
	long known = known_alias_charge + known_ring_alias_charge;

	/* each user counter individually: a +leak on one and a -leak on
	 * the other would hide behind a summed total */
	if (user_$charge_counter() != known) {
		printf("    OBSERVED %s: $charge_counter leaked %ld pages\n",
		       what, user_$charge_counter());
		bad = 1;
	} else if (known) {
		printf("    KNOWN residual: %ld pages "
		       "(aux alias %ld [#947/#948], ring alias %ld)\n",
		       known, known_alias_charge, known_ring_alias_charge);
	}
	if (user_$spare_counter()) {
		printf("    OBSERVED %s: $spare_counter leaked %ld pages\n",
		       what, user_$spare_counter());
		bad = 1;
	}
	if (mm.pinned_vm) {
		printf("    OBSERVED %s: pinned_vm leaked %ld pages\n",
		       what, (long)mm.pinned_vm);
		bad = 1;
	}
	if (rb_allocs != rb_frees || aux_allocs != aux_frees) {
		printf("    OBSERVED %s: allocation imbalance rb %d/%d aux %d/%d\n",
		       what, rb_allocs, rb_frees, aux_allocs, aux_frees);
		bad = 1;
	}
	if (ev.mmap_count.refs != 0 || rb != NULL || ev.rb != NULL) {
		printf("    OBSERVED %s: stale refs ev.mmap_count=%d rb=%d\n",
		       what, ev.mmap_count.refs, ev.rb != NULL);
		bad = 1;
	}
	if (warn_count) {
		printf("    OBSERVED %s: %d WARN_ON fires\n", what, warn_count);
		bad = 1;
	}
	return bad;
}

/* Assert the user counters and pinned_vm individually at an intermediate
 * step, so a charge/uncharge mismatch on either counter is reported on
 * its own instead of only via the final zero check. */
static int check_split(const char *what, long want_user, long want_pinned)
{
	int bad = user_charge_broken(what, want_user);

	if (mm.pinned_vm != want_pinned) {
		printf("    OBSERVED %s: pinned_vm %ld, expected %ld\n",
		       what, (long)mm.pinned_vm, want_pinned);
		bad |= 1;
	}
	return bad;
}

/* --- scenarios ----------------------------------------------------------
 * A ring geometry is (vma_pages, mm_shift): vma_pages = data pages + 1
 * metadata page; vma_pages == 1 is a metadata-only ring; the user4k
 * sub-native geometry uses a 4K-page mm so the data chunk (4K) is smaller
 * than the native (16K) page.  charge = pages the tree charges for the
 * first ring mmap.  split = pages of the first charge spilled into
 * pinned_vm via the tree's own perf_mmap_calc_limits(): 0 none, >0 partial,
 * -1 full spill.
 */
static long ring_charge(unsigned long vma_pages, unsigned long mm_shift)
{
#if RB_USER4K
	return DIV_ROUND_UP((long)(vma_pages - 1) << mm_shift, PAGE_SIZE) + 1;
#else
	return vma_pages;
#endif
}

static void setup_limits(long split)
{
	if (split == 0)
		return;			/* default generous limit, no split */
	if (split < 0) {
		set_user_lock_limit(0);	/* full spill */
		return;
	}
	set_user_lock_limit(charge_expect - split);
}

static int sc_rb_baseline(unsigned long vma_pages, unsigned long mm_shift, long split)
{
	struct vm_area_struct v;

	reset_all(mm_shift);
	charge_expect = ring_charge(vma_pages, mm_shift);
	setup_limits(split);

	v = make_vma_bytes(vma_pages << mm_shift, 0);
	if (perf_mmap(&file, &v)) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	dump("after first mmap");
	perf_mmap_close(&v);
	dump("after close");
	return invariants_broken("baseline ring lifecycle");
}

/* One duplicate-ring-alias lifecycle.  Close order: aliases first when
 * orig_first is 0; the original mapping first, while every alias is
 * still live, when orig_first is 1. */
static int rb_dup_cycle(unsigned long vma_pages, unsigned long mm_shift,
			long split, int orig_first)
{
	struct vm_area_struct v[4];	/* 0 = ring, 1..3 = aliases */
	int i, bad = 0;
	long user_first, pinned_first;

	reset_all(mm_shift);
	charge_expect = ring_charge(vma_pages, mm_shift);
	setup_limits(split);
	user_first = split < 0 ? 0 : charge_expect - (split > 0 ? split : 0);
	pinned_first = split < 0 ? charge_expect : (split > 0 ? split : 0);

	v[0] = make_vma_bytes(vma_pages << mm_shift, 0);
	if (perf_mmap(&file, &v[0])) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	dump("after first mmap");
	bad |= check_split("after first mmap", user_first, pinned_first);
	for (i = 1; i < 4; i++) {
		v[i] = make_vma_bytes(vma_pages << mm_shift, 0);
		if (perf_mmap(&file, &v[i])) {
			printf("    UNEXPECTED alias mmap %d failed\n", i);
			return 1;
		}
		dump("after alias mmap");
		/* an alias shares the already-locked pages: no new charge */
		bad |= check_split("after alias mmap", user_first, pinned_first);
	}
	if (orig_first) {
		/* alternate order: the original mapping closes while all
		 * three aliases are still live; the charge must stay put
		 * until the last alias closes */
		perf_mmap_close(&v[0]);
		dump("after original close");
		bad |= check_split("after original close", user_first,
				   pinned_first);
		for (i = 1; i < 4; i++) {
			perf_mmap_close(&v[i]);
			dump(i == 3 ? "after final close" : "after alias close");
			/* the last close completes the lifecycle: its zero
			 * check is invariants_broken() below */
			if (i < 3)
				bad |= check_split("after alias close",
						   user_first, pinned_first);
		}
	} else {
		for (i = 1; i < 4; i++) {
			perf_mmap_close(&v[i]);
			dump("after alias close");
			bad |= check_split("after alias close", user_first,
					   pinned_first);
		}
		perf_mmap_close(&v[0]);
		dump("after final close");
	}
	bad |= invariants_broken(orig_first
				 ? "duplicate ring aliases (3), original closed first"
				 : "duplicate ring aliases (3)");
	printf("    expected first charge: %ld user + %ld pinned; aliases add no charge\n",
	       user_first, pinned_first);
	return bad;
}

static int sc_rb_duplicate(unsigned long vma_pages, unsigned long mm_shift, long split)
{
	/* both close orders must balance */
	return rb_dup_cycle(vma_pages, mm_shift, split, 0) |
	       rb_dup_cycle(vma_pages, mm_shift, split, 1);
}

static int sc_rb_alias_map_range_fail(long split)
{
	struct vm_area_struct v[2];
	int bad = 0;
	long user_first, pinned_first;

	reset_all(PAGE_SHIFT);
	charge_expect = ring_charge(17, PAGE_SHIFT);
	setup_limits(split);
	user_first = split < 0 ? 0 : charge_expect - (split > 0 ? split : 0);
	pinned_first = split < 0 ? charge_expect : (split > 0 ? split : 0);

	v[0] = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &v[0])) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	dump("after first mmap");

	map_range_fail = 1;
	v[1] = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (!perf_mmap(&file, &v[1])) {
		printf("    UNEXPECTED failing alias mmap succeeded\n");
		map_range_fail = 0;
		return 1;
	}
	map_range_fail = 0;
	dump("after FAILED alias mmap (map_range)");
	/* the failed alias charged nothing: counters still hold only the
	 * first map's split */
	bad |= check_split("after FAILED alias mmap (map_range)",
			   user_first, pinned_first);

	if (ev.mmap_count.refs != 1 || ev.rb == NULL ||
	    ev.rb->mmap_count.refs != 1) {
		printf("    UNEXPECTED refcount unwind after failed alias\n");
		bad = 1;
	}
	perf_mmap_close(&v[0]);
	dump("after final close");
	return invariants_broken("failed ring alias via map_range") || bad;
}

static int sc_rb_first_map_range_fail(long split)
{
	struct vm_area_struct v;
	int bad;

	reset_all(PAGE_SHIFT);
	charge_expect = ring_charge(17, PAGE_SHIFT);
	setup_limits(split);

	map_range_fail = 1;
	v = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (!perf_mmap(&file, &v)) {
		printf("    UNEXPECTED failing first mmap succeeded\n");
		map_range_fail = 0;
		return 1;
	}
	map_range_fail = 0;
	dump("after FAILED first mmap (map_range)");
	bad = invariants_broken("failed first ring mmap via map_range");
	return bad;
}

static unsigned long aux_request(struct perf_buffer *rb, unsigned long aux_pages)
{
	unsigned long base;

	base = (perf_data_size(rb)
#if RB_USER4K
		+ perf_metadata_size(rb)
#else
		+ PAGE_SIZE
#endif
		) >> PAGE_SHIFT;
	/* Userspace announces the AUX layout through the shared user page
	 * before the first AUX mmap (same values the tree checks for). */
	rb->user_page->aux_offset = (u64)base << PAGE_SHIFT;
	rb->user_page->aux_size = (u64)aux_pages << PAGE_SHIFT;
	return base;
}

/* Measure how many pages the tree charges AGAIN for an alias mapping of
 * the same AUX area: map the AUX area once, then map it again, and report
 * the difference in the user counter.  A tree with the known #947/#948
 * per-alias charge returns 8 (the AUX size); a tree that charges aliases
 * only once returns 0.  Measuring keeps a scenario's expected charge
 * bound to the tree under test instead of to an assumption. */
static long measure_alias_charge(void)
{
	struct vm_area_struct vring, vaux1, vaux2;
	long before, after;

	reset_all(PAGE_SHIFT);
	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring))
		return -1;
	aux_pgoff_target = aux_request(ev.rb, 8);
	vaux1 = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (perf_mmap(&file, &vaux1))
		return -1;
	before = user_$charge_counter();
	vaux2 = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (perf_mmap(&file, &vaux2))
		return -1;
	after = user_$charge_counter();
	return after - before;
}

/* Same measurement for a RING alias: map the ring twice through the
 * same event and report the difference.  A tree that charges every
 * ring alias returns the ring charge; a tree where aliases share the
 * first map's single charge returns 0. */
static long measure_ring_alias_charge(void)
{
	struct vm_area_struct v1, v2;
	long before, after;

	reset_all(PAGE_SHIFT);
	v1 = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &v1))
		return -1;
	before = user_$charge_counter();
	v2 = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &v2))
		return -1;
	after = user_$charge_counter();
	return after - before;
}

static int sc_aux_baseline(void)
{
	struct vm_area_struct vring, vaux;
	int bad;

	reset_all(PAGE_SHIFT);
	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	aux_pgoff_target = aux_request(ev.rb, 8);
	vaux = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (perf_mmap(&file, &vaux)) {
		printf("    UNEXPECTED aux mmap failed\n");
		return 1;
	}
	dump("after aux mmap");
	perf_mmap_close(&vaux);
	dump("after aux close");
	perf_mmap_close(&vring);
	dump("after ring close");
	bad = invariants_broken("baseline aux lifecycle");
	return bad;
}

/* One duplicate-AUX-alias lifecycle.  Close order: AUX aliases then the
 * normal ring VMA when ring_first is 0; the normal ring VMA first,
 * while the AUX area stays live, when ring_first is 1. */
static int aux_dup_cycle(long split, int ring_first)
{
	struct vm_area_struct vring, vaux[4];
	int i, bad = 0;
	long ring_chg = ring_charge(17, PAGE_SHIFT);
	long aux_user, aux_pinned;	/* the AUX charge's own split */

	reset_all(PAGE_SHIFT);

	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	if (split < 0)
		set_user_lock_limit(0);
	else if (split > 0)
		/* the ring already sits in the user counter: the AUX charge
		 * of 8 pages must split 5 user / `split` pinned against a
		 * limit of current-user + (8 - split) */
		set_user_lock_limit(ring_chg + 8 - split);
	aux_user = split < 0 ? 0 : 8 - split;
	aux_pinned = split < 0 ? 8 : split;

	aux_pgoff_target = aux_request(ev.rb, 8);
	vaux[0] = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (perf_mmap(&file, &vaux[0])) {
		printf("    UNEXPECTED aux mmap failed\n");
		return 1;
	}
	dump("after first aux mmap");
	bad |= check_split("after first aux mmap", ring_chg + aux_user,
			   aux_pinned);
	for (i = 1; i < 4; i++) {
		vaux[i] = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
		if (perf_mmap(&file, &vaux[i])) {
			printf("    UNEXPECTED aux alias mmap %d failed\n", i);
			return 1;
		}
		dump("after aux alias mmap");
		/* an alias shares the already-locked pages: no new charge */
		bad |= check_split("after aux alias mmap", ring_chg + aux_user,
				   aux_pinned);
	}
	if (ring_first) {
		/* alternate order: the normal ring VMA closes while the
		 * AUX area stays live; the AUX mappings still hold
		 * rb->mmap_count, so neither charge may uncharge yet */
		perf_mmap_close(&vring);
		dump("after ring close (aux live)");
		bad |= check_split("after ring close (aux live)",
				   ring_chg + aux_user, aux_pinned);
	}
	for (i = 1; i < 4; i++) {
		perf_mmap_close(&vaux[i]);
		dump("after aux alias close");
		bad |= check_split("after aux alias close", ring_chg + aux_user,
				   aux_pinned);
	}
	perf_mmap_close(&vaux[0]);
	if (ring_first) {
		/* the last AUX close empties rb->mmap_count: the ring and
		 * AUX charges uncharge together here */
		dump("after last aux close (all uncharge)");
	} else {
		dump("after last aux close (aux uncharge)");
		perf_mmap_close(&vring);
		dump("after ring close");
	}
	bad |= invariants_broken(ring_first
				 ? "duplicate aux aliases (3), ring closed first"
				 : "duplicate aux aliases (3)");
	return bad;
}

static int sc_aux_duplicate(long split)
{
	/* both close orders must balance */
	return aux_dup_cycle(split, 0) | aux_dup_cycle(split, 1);
}

static int sc_aux_limit_fail(void)
{
	struct vm_area_struct vring, vaux;
	int bad = 0;

	reset_all(PAGE_SHIFT);
	charge_expect = ring_charge(17, PAGE_SHIFT);
	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	aux_pgoff_target = aux_request(ev.rb, 8);

	/* Force the tree's own calc_limits to reject: pinned_vm pressure. */
	h_rlimit_memlock = 4UL << 20;		/* 256 native pages */
	mm.pinned_vm = 300;
	vaux = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (!perf_mmap(&file, &vaux)) {
		printf("    UNEXPECTED over-limit aux mmap succeeded\n");
		return 1;
	}
	dump("after REJECTED aux mmap (-EPERM)");
	bad |= user_charge_broken("after REJECTED aux mmap (-EPERM)",
				  charge_expect);
	if (ev.rb->mmap_count.refs != 1 || ev.rb->aux_mmap_count.refs != 0 ||
	    ev.mmap_count.refs != 1 || aux_allocs != 0) {
		printf("    UNEXPECTED unwound state after -EPERM aux "
		       "(rbmc=%d auxmc=%d evmc=%d auxa=%d)\n",
		       ev.rb->mmap_count.refs, ev.rb->aux_mmap_count.refs,
		       ev.mmap_count.refs, aux_allocs);
		bad = 1;
	}
	mm.pinned_vm = 0;		/* the -EPERM trigger preload, not a leak */
	perf_mmap_close(&vring);
	dump("after ring close");
	return invariants_broken("aux limit-rejected first map") || bad;
}

static int sc_aux_alloc_fail(void)
{
	struct vm_area_struct vring, vaux;
	int bad = 0;

	reset_all(PAGE_SHIFT);
	charge_expect = ring_charge(17, PAGE_SHIFT);
	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	aux_pgoff_target = aux_request(ev.rb, 8);

	rb_alloc_aux_fail = 1;
	vaux = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (!perf_mmap(&file, &vaux)) {
		printf("    UNEXPECTED failing aux mmap succeeded\n");
		rb_alloc_aux_fail = 0;
		return 1;
	}
	rb_alloc_aux_fail = 0;
	dump("after FAILED aux mmap (rb_alloc_aux)");
	bad |= user_charge_broken("after FAILED aux mmap (rb_alloc_aux)",
				  charge_expect);
	if (ev.rb->mmap_count.refs != 1 || ev.rb->aux_mmap_count.refs != 0 ||
	    ev.mmap_count.refs != 1 || aux_allocs != 0) {
		printf("    UNEXPECTED unwound state after rb_alloc_aux failure "
		       "(rbmc=%d auxmc=%d evmc=%d auxa=%d)\n",
		       ev.rb->mmap_count.refs, ev.rb->aux_mmap_count.refs,
		       ev.mmap_count.refs, aux_allocs);
		bad = 1;
	}
	perf_mmap_close(&vring);
	dump("after ring close");
	return invariants_broken("aux rb_alloc_aux failure") || bad;
}

static int sc_aux_alias_size_reject(void)
{
	struct vm_area_struct vring, vaux, vbad;
	struct perf_buffer *rb;
	int bad = 0;
	long before_mlock, before_locked;

	reset_all(PAGE_SHIFT);
	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	rb = ev.rb;
	aux_pgoff_target = aux_request(rb, 8);
	vaux = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (perf_mmap(&file, &vaux)) {
		printf("    UNEXPECTED aux mmap failed\n");
		return 1;
	}
	before_mlock = user_perf_mlock();
	before_locked = user_locked_vm();

	/* A wrong-size alias must be rejected before any refcount or charge. */
	vbad = make_vma_bytes(4 << PAGE_SHIFT, aux_pgoff_target);
	if (!perf_mmap(&file, &vbad)) {
		printf("    UNEXPECTED wrong-size aux alias succeeded\n");
		return 1;
	}
	dump("after REJECTED wrong-size aux alias");
	if (user_perf_mlock() != before_mlock ||
	    user_locked_vm() != before_locked ||
	    rb->mmap_count.refs != 2 ||
	    rb->aux_mmap_count.refs != 1 || ev.mmap_count.refs != 2) {
		printf("    UNEXPECTED state after rejected alias\n");
		bad = 1;
	}
	perf_mmap_close(&vaux);
	perf_mmap_close(&vring);
	dump("after closes");
	return invariants_broken("wrong-size aux alias") || bad;
}

static int sc_aux_alias_map_range_fail(long split)
{
	struct vm_area_struct vring, vaux[2];
	struct perf_buffer *rb;
	int bad = 0;

	reset_all(PAGE_SHIFT);
	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	rb = ev.rb;
	if (split < 0)
		set_user_lock_limit(0);
	else if (split > 0)
		/* the ring already sits in the user counter (see
		 * sc_aux_duplicate): limit = current-user + (8 - split) */
		set_user_lock_limit(ring_charge(17, PAGE_SHIFT) + 8 - split);
	aux_pgoff_target = aux_request(rb, 8);
	vaux[0] = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (perf_mmap(&file, &vaux[0])) {
		printf("    UNEXPECTED aux mmap failed\n");
		return 1;
	}
	dump("after first aux mmap");

	map_range_fail = 1;
	vaux[1] = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (!perf_mmap(&file, &vaux[1])) {
		printf("    UNEXPECTED failing aux alias mmap succeeded\n");
		map_range_fail = 0;
		return 1;
	}
	map_range_fail = 0;
	dump("after FAILED aux alias mmap (map_range)");
	/* the failed alias charged nothing: counters still hold only the
	 * ring's charge plus the first AUX map's split */
	bad |= check_split("after FAILED aux alias mmap (map_range)",
			   ring_charge(17, PAGE_SHIFT) +
			   (split < 0 ? 0 : 8 - split),
			   split < 0 ? 8 : split);
	if (ev.mmap_count.refs != 2 || rb->mmap_count.refs != 2 ||
	    rb->aux_mmap_count.refs != 1) {
		printf("    UNEXPECTED refcount unwind after failed aux alias\n");
		bad = 1;
	}
	perf_mmap_close(&vaux[0]);
	dump("after last aux close");
	perf_mmap_close(&vring);
	dump("after ring close");
	return invariants_broken("failed aux alias via map_range") || bad;
}

static int sc_aux_first_map_range_fail(void)
{
	struct vm_area_struct vring, vaux;
	int bad = 0;

	reset_all(PAGE_SHIFT);
	charge_expect = ring_charge(17, PAGE_SHIFT);
	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	aux_pgoff_target = aux_request(ev.rb, 8);

	map_range_fail = 1;
	vaux = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (!perf_mmap(&file, &vaux)) {
		printf("    UNEXPECTED failing first aux mmap succeeded\n");
		map_range_fail = 0;
		return 1;
	}
	map_range_fail = 0;
	dump("after FAILED first aux mmap (map_range)");
	/* perf_mmap_close() must have unwound the whole AUX allocation:
	 * the ring keeps its own charge, the AUX charge and allocation
	 * are gone again, each counter checked individually */
	bad |= user_charge_broken("after FAILED first aux mmap (map_range)",
				  charge_expect);
	if (mm.pinned_vm != 0 ||
	    ev.rb->mmap_count.refs != 1 || ev.rb->aux_mmap_count.refs != 0 ||
	    ev.mmap_count.refs != 1 || aux_allocs != aux_frees) {
		printf("    UNEXPECTED unwound state after failed first aux "
		       "(pinned=%ld rbmc=%d auxmc=%d evmc=%d "
		       "auxa=%d auxf=%d)\n",
		       (long)mm.pinned_vm,
		       ev.rb->mmap_count.refs, ev.rb->aux_mmap_count.refs,
		       ev.mmap_count.refs, aux_allocs, aux_frees);
		bad = 1;
	}
	perf_mmap_close(&vring);
	dump("after ring close");
	return invariants_broken("failed first aux mmap via map_range") || bad;
}

/* Positive control: a redirected event whose first mmap is a SUCCESSFUL
 * AUX mmap.  Proves the harness reaches perf_mmap_aux() through the
 * set_output redirect (R->rb was never created by R) and that the
 * successful lifecycle balances.  Run without limit split: per-mm
 * pinned_vm uncharge follows the closing vma's mm, which for cross-mm
 * aliases is the tree's existing behaviour outside this scenario's
 * scope (user-side charges are what must balance here). */
static int sc_aux_redirect_lifecycle(void)
{
	struct vm_area_struct vring, vaux;
	struct perf_buffer *rb;
	int bad = 0;

	reset_all(PAGE_SHIFT);
	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	rb = ev.rb;
	aux_pgoff_target = aux_request(rb, 8);

	set_output(&ev_redirect, &ev);
	if (ev_redirect.rb != rb || rb->refcount.refs != 2 ||
	    event_list_len(rb) != 2 || ev_redirect.mmap_count.refs != 0) {
		printf("    UNEXPECTED redirect state (rb=%d rc=%d list=%d mc=%d)\n",
		       ev_redirect.rb == rb, rb->refcount.refs,
		       event_list_len(rb), ev_redirect.mmap_count.refs);
		return 1;
	}

	vaux = make_vma_bytes_mm(8 << PAGE_SHIFT, aux_pgoff_target,
				 &mm_redirect, &file_redirect);
	if (perf_mmap(&file_redirect, &vaux)) {
		printf("    UNEXPECTED redirected aux mmap failed\n");
		return 1;
	}
	dump("after redirected aux mmap");
	if (ev_redirect.mmap_count.refs != 1 || rb->mmap_count.refs != 2 ||
	    rb->aux_mmap_count.refs != 1 || rb->aux_refcount.refs != 1) {
		printf("    UNEXPECTED redirected aux mmap state\n");
		bad = 1;
	}
	bad |= check_split("after redirected aux mmap",
			   ring_charge(17, PAGE_SHIFT) + 8, 0);
	if (mm_redirect.pinned_vm != 0) {
		printf("    OBSERVED redirect mm pinned_vm %ld, expected 0\n",
		       (long)mm_redirect.pinned_vm);
		bad = 1;
	}

	perf_mmap_close(&vaux);
	dump("after redirected aux close");
	if (ev_redirect.rb != NULL || ev_redirect.mmap_count.refs != 0 ||
	    rb->mmap_count.refs != 1 || rb->refcount.refs != 1 ||
	    rb->aux_mmap_count.refs != 0 || aux_allocs != aux_frees ||
	    event_list_len(rb) != 1) {
		printf("    UNEXPECTED redirect unwind after aux close\n");
		bad = 1;
	}

	perf_mmap_close(&vring);
	dump("after ring close");
	bad |= invariants_broken("redirected aux lifecycle");
	if (mm_redirect.pinned_vm != 0) {
		printf("    OBSERVED redirect mm pinned_vm %ld, expected 0\n",
		       (long)mm_redirect.pinned_vm);
		bad = 1;
	}
	return bad;
}

/* The redirected-event first-map failure: R (set_output, mmap_count 0)
 * AUX-mmaps the shared rb and map_range() fails in the perf_mmap()
 * dispatcher.  perf_mmap_aux() succeeded, ending in
 * refcount_inc(&event->mmap_count) FROM ZERO -- real refcount_t
 * semantics WARN (ADD_UAF) and leave the count SATURATED, so the
 * dispatcher's minimal-close branch (mmap_count == 1) is not taken and
 * the full perf_mmap_close() runs against a saturated per-event count:
 * its refcount_dec_and_mutex_lock(&event->mmap_count) warns again,
 * never reaches 0, and R is left ATTACHED to the rb.  Desired state
 * after the failure (what the assertions encode): no saturation at
 * all, R fully detached with mmap_count 0, rb->mmap_count/refcount
 * back to the owner's alone, the AUX allocation freed (aux_refcount 0,
 * pages gone), user counters hold only the ring's charge, each mm's
 * pinned_vm checked separately. */
static int sc_aux_redirect_first_map_range_fail(long split)
{
	struct vm_area_struct vring, vaux;
	struct perf_buffer *rb;
	int bad = 0;
	long ring_chg = ring_charge(17, PAGE_SHIFT);
	long ring_user, ring_pinned;

	reset_all(PAGE_SHIFT);
	if (split < 0)
		set_user_lock_limit(0);
	else if (split > 0)
		/* the AUX charge of 8 pages splits (8 - split) user /
		 * split pinned against this limit; the owner's ring fits
		 * the user counter whole */
		set_user_lock_limit(ring_chg + 8 - split);
	ring_user = split < 0 ? 0 : ring_chg;
	ring_pinned = split < 0 ? ring_chg : 0;

	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED first mmap failed\n");
		return 1;
	}
	rb = ev.rb;
	aux_pgoff_target = aux_request(rb, 8);
	set_output(&ev_redirect, &ev);
	dump("after redirect");

	map_range_fail = 1;
	vaux = make_vma_bytes_mm(8 << PAGE_SHIFT, aux_pgoff_target,
				 &mm_redirect, &file_redirect);
	if (!perf_mmap(&file_redirect, &vaux)) {
		printf("    UNEXPECTED failing redirected aux mmap succeeded\n");
		map_range_fail = 0;
		return 1;
	}
	map_range_fail = 0;
	dump("after FAILED redirected aux mmap (map_range)");

	/* R detached, owner's refcounts restored */
	if (ev_redirect.rb != NULL || ev_redirect.mmap_count.refs != 0) {
		printf("    OBSERVED redirect event still attached after "
		       "failure (rb=%d mc=%d)\n",
		       ev_redirect.rb != NULL, ev_redirect.mmap_count.refs);
		bad = 1;
	}
	if (rb->mmap_count.refs != 1 || rb->refcount.refs != 1 ||
	    event_list_len(rb) != 1) {
		printf("    OBSERVED rb not restored to owner-only "
		       "(mmap_count=%d refcount=%d event_list=%d)\n",
		       rb->mmap_count.refs, rb->refcount.refs,
		       event_list_len(rb));
		bad = 1;
	}
	/* the AUX allocation must be gone */
	if (rb->aux_mmap_count.refs != 0 || rb->aux_refcount.refs != 0 ||
	    rb->aux_nr_pages != 0 || aux_allocs != aux_frees) {
		printf("    OBSERVED AUX allocation leaked after failure "
		       "(auxmc=%d auxrc=%d pages=%d allocs=%d frees=%d)\n",
		       rb->aux_mmap_count.refs, rb->aux_refcount.refs,
		       rb->aux_nr_pages, aux_allocs, aux_frees);
		bad = 1;
	}
	/* user counters hold only the ring's charge; the AUX charge is
	 * fully undone, on both users and both mms individually */
	bad |= check_split("after FAILED redirected aux mmap (map_range)",
			   ring_user, ring_pinned);
	if (mm_redirect.pinned_vm != 0) {
		printf("    OBSERVED redirect mm pinned_vm %ld, expected 0\n",
		       (long)mm_redirect.pinned_vm);
		bad = 1;
	}

	/* the owner then closes normally: the rb must free with nothing
	 * left over */
	perf_mmap_close(&vring);
	dump("after ring close");
	bad |= invariants_broken("failed redirected first aux mmap via "
				 "map_range");
	if (mm_redirect.pinned_vm != 0) {
		printf("    OBSERVED redirect mm pinned_vm %ld, expected 0\n",
		       (long)mm_redirect.pinned_vm);
		bad = 1;
	}
	return bad;
}

/* --- focused redirected-AUX scenarios ----------------------------------
 * The redirect fix is about the redirected event's OWN mmap_count: it
 * must read 1 after the redirected event's first AUX mmap (not a
 * saturated count) and 0 after the close, and the redirect must detach
 * from the shared rb on every path.  Each scenario below spells out the
 * exact expected numbers for the redirect's count, the shared rb's
 * counters, the AUX allocation's lifetime and the user-side charge.
 *
 * The known #947/#948 per-alias charge is NOT hidden: a scenario whose
 * AUX area is mapped twice is charged twice by the tree and uncharged
 * once, so it states that residual as known_alias_charge and
 * invariants_broken() prints it as the KNOWN residual.  Every other
 * number still has to be exact, and the pre-existing alias scenarios
 * still fail on the known defect.
 */

static int sc_aux_redirect_owner_aux_mapped(void)
{
	struct vm_area_struct vring, vaux_owner, vaux_r;
	struct perf_buffer *rb;
	struct redirect_state want;
	int bad = 0;

	reset_all(PAGE_SHIFT);
	/* the owner's AUX area is mapped first, so the redirect's AUX map
	 * takes perf_mmap_aux()'s alias branch: charged again by a tree
	 * with the known #947/#948 per-alias charge, uncharged once */
	known_alias_charge = alias_charge;

	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED owner ring mmap failed\n");
		return 1;
	}
	rb = ev.rb;
	aux_pgoff_target = aux_request(rb, 8);

	/* the owner maps the AUX area first */
	vaux_owner = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (perf_mmap(&file, &vaux_owner)) {
		printf("    UNEXPECTED owner aux mmap failed\n");
		return 1;
	}
	dump_redirect("after owner aux mmap", rb);

	set_output(&ev_redirect, &ev);

	/* now the redirect maps the same AUX area (rb_has_aux() alias) */
	vaux_r = make_vma_bytes_mm(8 << PAGE_SHIFT, aux_pgoff_target,
				     &mm_redirect, &file_redirect);
	if (perf_mmap(&file_redirect, &vaux_r)) {
		printf("    UNEXPECTED redirected aux mmap failed\n");
		return 1;
	}
	dump_redirect("after redirected aux mmap", rb);

	want = (struct redirect_state){
		.redirect_mc = 1,
		.rb_mmap = 3,
		.rb_aux_mmap = 2,
		.rb_refcount = 2,
		.list = 2,
		.aux_allocs = 1,
		.aux_frees = 0,
		.charge = 25 + known_alias_charge,
	};
	bad |= redirect_state_broken("after redirected aux mmap", rb, &want);

	/* the redirect closes its AUX mapping first */
	perf_mmap_close(&vaux_r);
	dump_redirect("after redirected aux close", rb);
	want = (struct redirect_state){
		.redirect_mc = 0,
		.rb_mmap = 2,
		.rb_aux_mmap = 1,
		.rb_refcount = 1,
		.list = 1,
		.aux_allocs = 1,
		.aux_frees = 0,
		.charge = 25 + known_alias_charge,
	};
	bad |= redirect_state_broken("after redirected aux close", rb, &want);

	/* the owner closes the AUX area: the AUX allocation is freed */
	perf_mmap_close(&vaux_owner);
	dump_redirect("after owner aux close", rb);
	want = (struct redirect_state){
		.redirect_mc = 0,
		.rb_mmap = 1,
		.rb_aux_mmap = 0,
		.rb_refcount = 1,
		.list = 1,
		.aux_allocs = 1,
		.aux_frees = 1,
		.charge = 17 + known_alias_charge,
	};
	bad |= redirect_state_broken("after owner aux close", rb, &want);

	/* the owner closes the ring: the rb is freed */
	perf_mmap_close(&vring);
	dump_redirect_final("after owner ring close");
	bad |= invariants_broken("redirected aux over an already mapped aux area");
	if (ev_redirect.rb != NULL || ev_redirect.mmap_count.refs != 0) {
		printf("    OBSERVED redirect not detached at the end "
		       "(rb=%d mc=%d)\n", ev_redirect.rb != NULL,
		       ev_redirect.mmap_count.refs);
		bad = 1;
	}
	if (mm_redirect.pinned_vm != 0) {
		printf("    OBSERVED redirect mm pinned_vm %ld, expected 0\n",
		       (long)mm_redirect.pinned_vm);
		bad = 1;
	}
	return bad;
}

/* The same shape, with the redirect's AUX map failing at map_range():
 * the dispatcher must unwind it completely -- the redirect detached,
 * the rb back to the owner's counts and the AUX allocation left to the
 * owner.  On unmodified code the redirect's mmap_count reads saturated
 * after the AUX map, so the minimal-close branch is not taken and the
 * full perf_mmap_close() runs against a saturated count: it warns
 * again, never reaches 0 and the redirect stays attached. */
static int sc_aux_redirect_owner_aux_mapped_fail(void)
{
	struct vm_area_struct vring, vaux_owner, vaux_r;
	struct perf_buffer *rb;
	struct redirect_state want;
	int bad = 0;

	reset_all(PAGE_SHIFT);
	known_alias_charge = alias_charge;

	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED owner ring mmap failed\n");
		return 1;
	}
	rb = ev.rb;
	aux_pgoff_target = aux_request(rb, 8);

	vaux_owner = make_vma_bytes(8 << PAGE_SHIFT, aux_pgoff_target);
	if (perf_mmap(&file, &vaux_owner)) {
		printf("    UNEXPECTED owner aux mmap failed\n");
		return 1;
	}
	dump_redirect("after owner aux mmap", rb);

	set_output(&ev_redirect, &ev);

	map_range_fail = 1;
	vaux_r = make_vma_bytes_mm(8 << PAGE_SHIFT, aux_pgoff_target,
				     &mm_redirect, &file_redirect);
	if (!perf_mmap(&file_redirect, &vaux_r)) {
		printf("    UNEXPECTED failing redirected aux mmap succeeded\n");
		map_range_fail = 0;
		return 1;
	}
	map_range_fail = 0;
	dump_redirect("after FAILED redirected aux mmap", rb);

	want = (struct redirect_state){
		.redirect_mc = 0,
		.rb_mmap = 2,
		.rb_aux_mmap = 1,
		.rb_refcount = 1,
		.list = 1,
		.aux_allocs = 1,
		.aux_frees = 0,
		.charge = 25 + known_alias_charge,
	};
	bad |= redirect_state_broken("after FAILED redirected aux mmap", rb,
				      &want);

	/* the owner closes the AUX area: the AUX allocation is freed */
	perf_mmap_close(&vaux_owner);
	dump_redirect("after owner aux close", rb);
	want = (struct redirect_state){
		.redirect_mc = 0,
		.rb_mmap = 1,
		.rb_aux_mmap = 0,
		.rb_refcount = 1,
		.list = 1,
		.aux_allocs = 1,
		.aux_frees = 1,
		.charge = 17 + known_alias_charge,
	};
	bad |= redirect_state_broken("after owner aux close", rb, &want);

	perf_mmap_close(&vring);
	dump_redirect_final("after owner ring close");
	bad |= invariants_broken("failed redirected aux over an already "
				 "mapped aux area");
	if (ev_redirect.rb != NULL || ev_redirect.mmap_count.refs != 0) {
		printf("    OBSERVED redirect not detached after the failure "
		       "(rb=%d mc=%d)\n", ev_redirect.rb != NULL,
		       ev_redirect.mmap_count.refs);
		bad = 1;
	}
	if (mm_redirect.pinned_vm != 0) {
		printf("    OBSERVED redirect mm pinned_vm %ld, expected 0\n",
		       (long)mm_redirect.pinned_vm);
		bad = 1;
	}
	return bad;
}

/* The owner's ring VMA closes FIRST while the redirect's AUX mapping
 * is still live: the rb survives on the redirect's mapping, the AUX
 * allocation is freed by the redirect's close and the ring's charge is
 * undone only when the rb's mapping count reaches 0. */
static int sc_aux_redirect_owner_ring_closes_first(void)
{
	struct vm_area_struct vring, vaux_r;
	struct perf_buffer *rb;
	struct redirect_state want;
	int bad = 0;

	reset_all(PAGE_SHIFT);

	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED owner ring mmap failed\n");
		return 1;
	}
	rb = ev.rb;
	aux_pgoff_target = aux_request(rb, 8);

	set_output(&ev_redirect, &ev);

	/* the redirect maps the AUX area (the rb's first AUX mapping) */
	vaux_r = make_vma_bytes_mm(8 << PAGE_SHIFT, aux_pgoff_target,
				     &mm_redirect, &file_redirect);
	if (perf_mmap(&file_redirect, &vaux_r)) {
		printf("    UNEXPECTED redirected aux mmap failed\n");
		return 1;
	}
	dump_redirect("after redirected aux mmap", rb);
	want = (struct redirect_state){
		.redirect_mc = 1,
		.rb_mmap = 2,
		.rb_aux_mmap = 1,
		.rb_refcount = 2,
		.list = 2,
		.aux_allocs = 1,
		.aux_frees = 0,
		.charge = 25,
	};
	bad |= redirect_state_broken("after redirected aux mmap", rb, &want);

	/* the owner closes its ring while the redirect's AUX stays live */
	perf_mmap_close(&vring);
	dump_redirect("after owner ring close (aux live)", rb);
	want = (struct redirect_state){
		.redirect_mc = 1,
		.rb_mmap = 1,
		.rb_aux_mmap = 1,
		.rb_refcount = 1,
		.list = 1,
		.aux_allocs = 1,
		.aux_frees = 0,
		.charge = 25,
	};
	bad |= redirect_state_broken("after owner ring close (aux live)", rb,
				      &want);
	if (ev.rb != NULL || ev.mmap_count.refs != 0) {
		printf("    OBSERVED owner not detached after its ring close "
		       "(rb=%d mc=%d)\n", ev.rb != NULL, ev.mmap_count.refs);
		bad = 1;
	}

	/* the redirect closes the AUX area: the AUX allocation and the
	 * ring's charge are undone here, the rb is freed */
	perf_mmap_close(&vaux_r);
	dump_redirect_final("after redirected aux close");
	bad |= invariants_broken("owner ring closed before the redirected aux");
	if (ev_redirect.rb != NULL || ev_redirect.mmap_count.refs != 0) {
		printf("    OBSERVED redirect not detached at the end "
		       "(rb=%d mc=%d)\n", ev_redirect.rb != NULL,
		       ev_redirect.mmap_count.refs);
		bad = 1;
	}
	if (mm_redirect.pinned_vm != 0) {
		printf("    OBSERVED redirect mm pinned_vm %ld, expected 0\n",
		       (long)mm_redirect.pinned_vm);
		bad = 1;
	}
	return bad;
}

/* The redirect maps the AUX area a second time: the AUX area is shared
 * with its own first mapping (rb_has_aux() alias branch) and the
 * redirect's mmap_count must go 1 -> 2, not saturate. */
static int sc_aux_redirect_second_aux_map(void)
{
	struct vm_area_struct vring, vaux_r1, vaux_r2;
	struct perf_buffer *rb;
	struct redirect_state want;
	int bad = 0;

	reset_all(PAGE_SHIFT);
	/* the second mapping of the same AUX area is charged again by a
	 * tree with the known #947/#948 per-alias charge, uncharged once */
	known_alias_charge = alias_charge;

	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED owner ring mmap failed\n");
		return 1;
	}
	rb = ev.rb;
	aux_pgoff_target = aux_request(rb, 8);

	set_output(&ev_redirect, &ev);

	vaux_r1 = make_vma_bytes_mm(8 << PAGE_SHIFT, aux_pgoff_target,
				     &mm_redirect, &file_redirect);
	if (perf_mmap(&file_redirect, &vaux_r1)) {
		printf("    UNEXPECTED first redirected aux mmap failed\n");
		return 1;
	}
	dump_redirect("after first redirected aux mmap", rb);

	vaux_r2 = make_vma_bytes_mm(8 << PAGE_SHIFT, aux_pgoff_target,
				     &mm_redirect, &file_redirect);
	if (perf_mmap(&file_redirect, &vaux_r2)) {
		printf("    UNEXPECTED second redirected aux mmap failed\n");
		return 1;
	}
	dump_redirect("after second redirected aux mmap", rb);
	want = (struct redirect_state){
		.redirect_mc = 2,
		.rb_mmap = 3,
		.rb_aux_mmap = 2,
		.rb_refcount = 2,
		.list = 2,
		.aux_allocs = 1,
		.aux_frees = 0,
		.charge = 25 + known_alias_charge,
	};
	bad |= redirect_state_broken("after second redirected aux mmap", rb,
				      &want);

	/* close the second mapping first, then the first */
	perf_mmap_close(&vaux_r2);
	dump_redirect("after second redirected aux close", rb);
	want = (struct redirect_state){
		.redirect_mc = 1,
		.rb_mmap = 2,
		.rb_aux_mmap = 1,
		.rb_refcount = 2,
		.list = 2,
		.aux_allocs = 1,
		.aux_frees = 0,
		.charge = 25 + known_alias_charge,
	};
	bad |= redirect_state_broken("after second redirected aux close", rb,
				      &want);

	perf_mmap_close(&vaux_r1);
	dump_redirect("after first redirected aux close", rb);
	want = (struct redirect_state){
		.redirect_mc = 0,
		.rb_mmap = 1,
		.rb_aux_mmap = 0,
		.rb_refcount = 1,
		.list = 1,
		.aux_allocs = 1,
		.aux_frees = 1,
		.charge = 17 + known_alias_charge,
	};
	bad |= redirect_state_broken("after first redirected aux close", rb,
				      &want);

	perf_mmap_close(&vring);
	dump_redirect_final("after owner ring close");
	bad |= invariants_broken("redirected event mapping the aux area twice");
	if (ev_redirect.rb != NULL || ev_redirect.mmap_count.refs != 0) {
		printf("    OBSERVED redirect not detached at the end "
		       "(rb=%d mc=%d)\n", ev_redirect.rb != NULL,
		       ev_redirect.mmap_count.refs);
		bad = 1;
	}
	if (mm_redirect.pinned_vm != 0) {
		printf("    OBSERVED redirect mm pinned_vm %ld, expected 0\n",
		       (long)mm_redirect.pinned_vm);
		bad = 1;
	}
	return bad;
}

/* The redirect maps the NORMAL ring (offset 0, the owner's geometry)
 * after its first AUX map: perf_mmap_rb()'s alias branch, reachable
 * once the redirect holds mmap_count 1.  The current tree admits this,
 * and both events then share ONE user_page: a userpage update from
 * either event overwrites the other's metadata.  That shared-metadata
 * exposure is a pre-existing limitation of the rb-alias design,
 * documented and asserted here as observed -- not changed by the
 * redirect fix.  The ring alias's charge follows the tree under test
 * (measured like the AUX alias charge) and is recorded in
 * known_ring_alias_charge, so the scenario states the tree's own
 * per-alias ring policy instead of assuming one. */
static int sc_aux_redirect_then_ring_map(void)
{
	struct vm_area_struct vring, vaux_r, vring_r;
	struct perf_buffer *rb;
	struct redirect_state want;
	int bad = 0;

	reset_all(PAGE_SHIFT);
	known_ring_alias_charge = ring_alias_charge;

	vring = make_vma_bytes(17 << PAGE_SHIFT, 0);
	if (perf_mmap(&file, &vring)) {
		printf("    UNEXPECTED owner ring mmap failed\n");
		return 1;
	}
	rb = ev.rb;
	aux_pgoff_target = aux_request(rb, 8);

	set_output(&ev_redirect, &ev);

	/* the redirect's first mmap: the AUX area (allocates it,
	 * mmap_count 0 -> 1) */
	vaux_r = make_vma_bytes_mm(8 << PAGE_SHIFT, aux_pgoff_target,
				   &mm_redirect, &file_redirect);
	if (perf_mmap(&file_redirect, &vaux_r)) {
		printf("    UNEXPECTED redirected aux mmap failed\n");
		return 1;
	}
	dump_redirect("after redirected aux mmap", rb);
	want = (struct redirect_state){
		.redirect_mc = 1,
		.rb_mmap = 2,
		.rb_aux_mmap = 1,
		.rb_refcount = 2,
		.list = 2,
		.aux_allocs = 1,
		.aux_frees = 0,
		.charge = 25,
	};
	bad |= redirect_state_broken("after redirected aux mmap", rb, &want);

	/* now the redirect maps the ring itself (rb alias branch) */
	vring_r = make_vma_bytes_mm(17 << PAGE_SHIFT, 0,
				    &mm_redirect, &file_redirect);
	if (perf_mmap(&file_redirect, &vring_r)) {
		printf("    UNEXPECTED redirected ring mmap failed\n");
		return 1;
	}
	dump_redirect("after redirected ring mmap", rb);
	want = (struct redirect_state){
		.redirect_mc = 2,
		.rb_mmap = 3,
		.rb_aux_mmap = 1,
		.rb_refcount = 2,
		.list = 2,
		.aux_allocs = 1,
		.aux_frees = 0,
		.charge = 25 + known_ring_alias_charge,
	};
	bad |= redirect_state_broken("after redirected ring mmap", rb, &want);

	/* close in reverse order: ring alias, AUX, owner ring */
	perf_mmap_close(&vring_r);
	dump_redirect("after redirected ring close", rb);
	want = (struct redirect_state){
		.redirect_mc = 1,
		.rb_mmap = 2,
		.rb_aux_mmap = 1,
		.rb_refcount = 2,
		.list = 2,
		.aux_allocs = 1,
		.aux_frees = 0,
		.charge = 25 + known_ring_alias_charge,
	};
	bad |= redirect_state_broken("after redirected ring close", rb, &want);

	perf_mmap_close(&vaux_r);
	dump_redirect("after redirected aux close", rb);
	want = (struct redirect_state){
		.redirect_mc = 0,
		.rb_mmap = 1,
		.rb_aux_mmap = 0,
		.rb_refcount = 1,
		.list = 1,
		.aux_allocs = 1,
		.aux_frees = 1,
		.charge = 17 + known_ring_alias_charge,
	};
	bad |= redirect_state_broken("after redirected aux close", rb, &want);

	perf_mmap_close(&vring);
	dump_redirect_final("after owner ring close");
	bad |= invariants_broken("redirected event mapping the ring after "
				 "its aux");
	if (ev_redirect.rb != NULL || ev_redirect.mmap_count.refs != 0) {
		printf("    OBSERVED redirect not detached at the end "
		       "(rb=%d mc=%d)\n", ev_redirect.rb != NULL,
		       ev_redirect.mmap_count.refs);
		bad = 1;
	}
	if (mm_redirect.pinned_vm != 0) {
		printf("    OBSERVED redirect mm pinned_vm %ld, expected 0\n",
		       (long)mm_redirect.pinned_vm);
		bad = 1;
	}
	return bad;
}

/* One RESULT line per scenario: FAIL on any broken invariant, otherwise
 * PASS-KNOWN-RESIDUAL when the scenario ended on the recorded per-alias
 * residual, plain PASS only when nothing was left over. */
static int result(const char *scenario, const char *geo, const char *limits,
		  int bad)
{
	const char *status = bad ? "FAIL"
		: (known_alias_charge || known_ring_alias_charge)
		  ? "PASS-KNOWN-RESIDUAL" : "PASS";

	printf("RESULT scenario=%s%s%s%s%s status=%s\n",
	       scenario,
	       geo ? " geometry=" : "", geo ? geo : "",
	       limits ? " limits=" : "", limits ? limits : "",
	       status);
	return bad;
}

int main(void)
{
	int failures = 0;
	unsigned long geos[][2] = {	/* vma_pages, mm_shift */
		{ 17, PAGE_SHIFT },	/* 16 data pages + metadata */
		{ 1, PAGE_SHIFT },	/* metadata-only */
		{ 2, 12 },		/* user4k: 1 data chunk of 4K on a 4K mm */
	};
	long splits[] = { 0, 3, -1 };	/* none, partial, full spill */
	const char *geo_names[] = { "ring16+meta", "metadata-only", "sub4k-1chunk+meta" };
	const char *lim_names[] = { "nosplit", "partialsplit3", "fullspill" };
	int g, s;

	/* how much does THIS tree charge again for an alias mapping of the
	 * same AUX area?  Measured, so the alias scenarios' expected charge
	 * follows the tree under test. */
	alias_charge = measure_alias_charge();
	printf("-- measured per-alias AUX charge: %ld pages%s\n",
	       alias_charge,
	       alias_charge ? " (known #947/#948 per-alias charge)" : "");
	if (alias_charge < 0) {
		/* a failed probe mmap makes the measurement invalid: the
		 * alias scenarios' expected charges would be built on
		 * nothing.  Hard fail instead of running them. */
		printf("RESULT scenario=alias-charge-probe status=FAIL "
		       "(measurement mmap failed, run aborted)\n");
		printf("SUMMARY failures=1\n");
		return 1;
	}
	printf("RESULT scenario=alias-charge-probe status=PASS\n");

	/* the tree's own per-alias RING policy, measured the same way */
	ring_alias_charge = measure_ring_alias_charge();
	printf("-- measured per-alias ring charge: %ld pages%s\n",
	       ring_alias_charge,
	       ring_alias_charge ? " (charged per ring alias)" : "");
	if (ring_alias_charge < 0) {
		printf("RESULT scenario=ring-alias-charge-probe status=FAIL "
		       "(measurement mmap failed, run aborted)\n");
		printf("SUMMARY failures=1\n");
		return 1;
	}
	printf("RESULT scenario=ring-alias-charge-probe status=PASS\n");

	for (g = 0; g < 3; g++) {
		if (g == 2 && !RB_USER4K)
			continue;
		printf("-- scenario rb-baseline geometry=%s\n", geo_names[g]);
		failures += result("rb-baseline", geo_names[g], NULL,
				   sc_rb_baseline(geos[g][0], geos[g][1], 0));
	}
	for (s = 0; s < 3; s++) {
		for (g = 0; g < 3; g++) {
			long split = splits[s];
			const char *lim = lim_names[s];
			char limname[32];
			long ringchg = ring_charge(geos[g][0], geos[g][1]);

			if (g == 2 && !RB_USER4K)
				continue;
			if (split > 0 && split >= ringchg) {
				/* this charge cannot give up `split` pages
				 * to pinned_vm; use the largest valid
				 * partial spill instead of letting the
				 * limit underflow into "no limit" */
				split = ringchg - 1;
				if (split <= 0) {
					split = -1;	/* cannot split at all */
					lim = lim_names[2];
				} else {
					snprintf(limname, sizeof(limname),
						 "partialsplit%ld", split);
					lim = limname;
				}
			}
			printf("-- scenario rb-duplicate geometry=%s limits=%s\n",
			       geo_names[g], lim);
			failures += result("rb-duplicate", geo_names[g], lim,
					   sc_rb_duplicate(geos[g][0],
							   geos[g][1], split));
		}
	}
	for (s = 0; s < 3; s += 2) {	/* nosplit and fullspill */
		printf("-- scenario rb-alias-map-range-fail limits=%s\n", lim_names[s]);
		failures += result("rb-alias-map-range-fail", NULL, lim_names[s],
				   sc_rb_alias_map_range_fail(splits[s]));
		printf("-- scenario rb-first-map-range-fail limits=%s\n", lim_names[s]);
		failures += result("rb-first-map-range-fail", NULL, lim_names[s],
				   sc_rb_first_map_range_fail(splits[s]));
	}
	printf("-- scenario aux-baseline\n");
	failures += result("aux-baseline", NULL, NULL, sc_aux_baseline());
	for (s = 0; s < 3; s++) {
		printf("-- scenario aux-duplicate limits=%s\n", lim_names[s]);
		failures += result("aux-duplicate", NULL, lim_names[s],
				   sc_aux_duplicate(splits[s]));
	}
	printf("-- scenario aux-limit-reject\n");
	failures += result("aux-limit-reject", NULL, NULL, sc_aux_limit_fail());
	printf("-- scenario aux-alloc-fail\n");
	failures += result("aux-alloc-fail", NULL, NULL, sc_aux_alloc_fail());
	printf("-- scenario aux-alias-size-reject\n");
	failures += result("aux-alias-size-reject", NULL, NULL,
			   sc_aux_alias_size_reject());
	for (s = 0; s < 3; s += 2) {
		printf("-- scenario aux-alias-map-range-fail limits=%s\n", lim_names[s]);
		failures += result("aux-alias-map-range-fail", NULL, lim_names[s],
				   sc_aux_alias_map_range_fail(splits[s]));
	}
	printf("-- scenario aux-first-map-range-fail\n");
	failures += result("aux-first-map-range-fail", NULL, NULL,
			   sc_aux_first_map_range_fail());
	printf("-- scenario aux-redirect-lifecycle\n");
	failures += result("aux-redirect-lifecycle", NULL, NULL,
			   sc_aux_redirect_lifecycle());
	for (s = 0; s < 3; s++) {
		printf("-- scenario aux-redirect-first-map-range-fail limits=%s\n",
		       lim_names[s]);
		failures += result("aux-redirect-first-map-range-fail", NULL,
				   lim_names[s],
				   sc_aux_redirect_first_map_range_fail(splits[s]));
	}
	printf("-- scenario aux-redirect-owner-aux-mapped\n");
	failures += result("aux-redirect-owner-aux-mapped", NULL, NULL,
			   sc_aux_redirect_owner_aux_mapped());
	printf("-- scenario aux-redirect-owner-aux-mapped-fail\n");
	failures += result("aux-redirect-owner-aux-mapped-fail", NULL, NULL,
			   sc_aux_redirect_owner_aux_mapped_fail());
	printf("-- scenario aux-redirect-owner-ring-closes-first\n");
	failures += result("aux-redirect-owner-ring-closes-first", NULL, NULL,
			   sc_aux_redirect_owner_ring_closes_first());
	printf("-- scenario aux-redirect-second-aux-map\n");
	failures += result("aux-redirect-second-aux-map", NULL, NULL,
			   sc_aux_redirect_second_aux_map());
	printf("-- scenario aux-redirect-then-ring-map\n");
	failures += result("aux-redirect-then-ring-map", NULL, NULL,
			   sc_aux_redirect_then_ring_map());

	printf("SUMMARY failures=%d\n", failures);
	return failures ? 1 : 0;
}
'''

HARNESS_GLOBALS = '''
static unsigned long aux_pgoff_target;
static long charge_expect;
/* Pages the tree charges AGAIN for an alias mapping of the same AUX area:
 * the per-alias charge defect fixed outside this change (#947/#948).  It
 * is MEASURED from the tree under test (measure_alias_charge()), not
 * assumed, so a scenario's expected charge states the tree's own policy.
 * A scenario that exercises an alias mapping sets known_alias_charge to
 * it: the residual is reported, not hidden, and every other invariant
 * is still exact.  known_ring_alias_charge is the same shape for a ring
 * mapping taken as an alias (charged per mapping by this tree,
 * uncharged once). */
static long known_alias_charge;
static long known_ring_alias_charge;
static long alias_charge;
static long ring_alias_charge;
'''


def provenance(tree):
    """Exact provenance of the tree under test: path, git HEAD, dirty state."""
    def git(*a):
        return subprocess.run(['git', '-C', str(tree), *a],
                              capture_output=True, text=True).stdout.strip()
    head = git('rev-parse', 'HEAD')
    if not head:
        import hashlib
        core = Path(tree) / 'kernel/events/core.c'
        digest = hashlib.sha256(core.read_bytes()).hexdigest()[:16] \
            if core.is_file() else 'no kernel/events/core.c'
        return '%s (not a git worktree; core.c sha256[:16]=%s)' % (tree, digest)
    dirty = git('status', '--porcelain', '--',
                'kernel/events', 'tools/asahi').splitlines()
    note = ('dirty: %s' % ', '.join(dirty)) if dirty else 'kernel/events clean'
    return '%s @ %s (%s)' % (tree, head, note)


def source_notes(core):
    """Probe the source shape so the tree under test is reported honestly."""
    notes = []
    rb = definition(core, 'perf_mmap_rb')
    close = definition(core, 'perf_mmap_close')
    if rb:
        marker = rb.find('refcount_inc_not_zero(&event->rb->mmap_count)')
        tail = rb[marker:marker + 400] if marker != -1 else ''
        if 'perf_mmap_account' in tail:
            notes.append('ring alias path charges perf_mmap_account() per alias')
        else:
            notes.append('ring alias path adds no charge '
                         '(aliases share the first map\'s charge)')
    if close:
        count = close.count('perf_mmap_unaccount(')
        notes.append('close has %d perf_mmap_unaccount call site(s) '
                     '(ring uncharge happens only when rb->mmap_count '
                     'reaches 0)' % count)
    return notes


def build(tree, out):
    core_path = tree / 'kernel/events/core.c'
    internal_path = tree / 'kernel/events/internal.h'
    ring_path = tree / 'kernel/events/ring_buffer.c'
    core = core_path.read_text()
    internal = internal_path.read_text()
    ring = ring_path.read_text()

    user4k, nr_pages_expr, data_shift_expr = rb_alloc_profile(ring)
    init = rb_init_lines(ring)

    helpers = []
    if not user4k:
        body = definition(internal, 'page_order')
        if body is None:
            raise SystemExit('missing page_order() in internal.h')
        helpers.append(body)
    for name in ('rb_has_aux', 'perf_data_size', 'data_page_nr', 'perf_aux_size'):
        body = definition(internal, name)
        if body is None:
            raise SystemExit('missing inline %s in internal.h' % name)
        helpers.append(body)
    if user4k:
        body = definition(internal, 'perf_metadata_size')
        if body:
            helpers.append(body)
    extracted = []
    for name in ('perf_mmap_calc_limits', 'perf_mmap_account',
                 'perf_mmap_unaccount', 'perf_mmap_rb', 'perf_mmap_aux',
                 'perf_mmap_close', 'perf_mmap'):
        body = definition(core, name)
        if body is None:
            raise SystemExit('missing %s in core.c' % name)
        extracted.append(body)
    # perf_mmap() and perf_mmap_aux() call perf_mmap_offset() in the trees
    # that have it, so extract it whenever they need it -- and before them,
    # since the extracted code calls it.
    if any('perf_mmap_offset' in body for body in extracted):
        body = definition(core, 'perf_mmap_offset')
        if body is None:
            raise SystemExit('tree lacks perf_mmap_offset()')
        helpers.append(body)
    helpers.extend(extracted)

    aux_decl = re.search(r'extern int rb_alloc_aux\(([^;]+)\);', internal)
    if aux_decl is None:
        raise SystemExit('missing rb_alloc_aux prototype in internal.h')
    params = aux_decl.group(1).strip().rstrip(')')
    names = [p.strip().split()[-1].lstrip('*') for p in params.split(',')]
    aux_def = ('static int rb_alloc_aux(%s)\n'
               '{\n'
               '\tif (rb_alloc_aux_fail)\n'
               '\t\treturn -ENOMEM;\t/* the real allocator frees its own partial work */\n'
               '\trb->aux_nr_pages = nr_pages;\n'
               '\trb->aux_watermark = watermark;\n'
               '\trefcount_set(&rb->aux_refcount, 1);\n'
               '#if RB_USER4K\n'
               '\trb->aux_offset = %s;\n'
               '#else\n'
               '\trb->aux_pgoff = %s;\n'
               '#endif\n'
               '\taux_allocs++;\n'
               '\treturn 0;\n'
               '}' % (params, names[2], names[2]))

    unaccount = definition(core, 'perf_mmap_unaccount')
    if unaccount is None:
        raise SystemExit('missing perf_mmap_unaccount in core.c')
    counter = 'perf_mlock' if 'user->perf_mlock' in unaccount else 'locked_vm'

    if user4k:
        rb_alloc_decl = ('struct perf_buffer *rb_alloc(unsigned long data_size, '
                         'unsigned int mmap_page_shift, long watermark, int cpu, '
                         'int flags);')
    else:
        rb_alloc_decl = ('struct perf_buffer *rb_alloc(int nr_pages, '
                         'long watermark, int cpu, int flags);')

    template = Template(HARNESS_GLOBALS + HARNESS)
    source = template.substitute({
        'RB_USER4K': '1' if user4k else '0',
        'rb_alloc_decl': rb_alloc_decl,
        'rb_alloc_aux_decl': 'static int rb_alloc_aux(%s);' % params,
        'rb_alloc_aux_def': aux_def,
        'helpers': '\n\n'.join(helpers),
        'rb_nr_pages': nr_pages_expr,
        'rb_data_shift': ('\trb->data_page_shift = %s;' % data_shift_expr)
                         if data_shift_expr else '\t/* none */',
        'rb_init': init or '\t/* tree leaves mmap_count at 0 */',
        'charge_counter': counter,
        'spare_counter': 'locked_vm' if counter == 'perf_mlock'
                         else 'perf_mlock',
    })
    path = out / (tree.name + '.c')
    path.write_text(source)
    binary = out / (tree.name + '.bin')
    subprocess.run([args.cc, '-std=gnu11', '-Wall', '-Werror',
                    '-Wno-unused-function', str(path), '-o', str(binary)],
                   check=True)
    return binary, user4k, counter, source_notes(core)


overall = 0
with tempfile.TemporaryDirectory(prefix='perf-alias-lifecycle-') as directory:
    for tree in args.trees:
        print('=' * 72)
        print('TREE %s' % tree)
        try:
            binary, user4k, counter, notes = build(tree, Path(directory))
        except SystemExit as error:
            print('FAIL: %s' % error)
            overall = 2
            continue
        except subprocess.CalledProcessError:
            print('FAIL: harness build failed for %s' % tree)
            src = Path(directory) / (tree.name + '.c')
            if src.exists():
                print('harness source kept at %s' % src)
            overall = 2
            continue
        print('provenance: %s' % provenance(tree))
        print('profile: %s, user counter: %s' %
              ('user4k (data_size rb_alloc, per-mm page shift)' if user4k
               else 'plain (nr_pages rb_alloc, PAGE_SHIFT)', counter))
        for note in notes:
            print('source: %s' % note)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        if result.stderr.strip():
            sys.stdout.write(result.stderr)
        if result.returncode < 0:
            # a crashing binary (signal) loses the tempdir; keep the
            # extracted source for a backtrace against the exact code
            keep = Path.cwd() / binary.with_suffix('.c').name
            try:
                keep.write_text(binary.with_suffix('.c').read_text())
                print('harness source kept at %s' % keep)
            except OSError as error:
                print('could not keep harness source: %s' % error)
        if result.returncode != 0 and overall == 0:
            overall = 1
print('=' * 72)
print('OVERALL: %s' % ('tested invariants failed' if overall == 1
                       else 'build/source error' if overall == 2
                       else 'tested lifecycles balanced'))
raise SystemExit(overall)
