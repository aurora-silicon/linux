#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Replay a two-display dock's shared-rate DP re-link over its display's Activate.

Under TB_HOST_DP_SHARED_RATE both DP IN streams of an ATC PHY run at one link
rate. A dock that reports its displays one at a time gets the first tunnel at
HBR3, and the second display has the first re-linked at HBR2: a teardown and a
new tunnel, whose DP IN request replaces the one the display was handed. Made
while the display's firmware is waking the DP IN (Activate), the re-link has
the Activate refused (-ESTALE) and the display stays dark.

The connection manager's pairing, re-link and DPRX completion code (tb.c), the
DPRX work (tunnel.c) and the Apple glue's DP IN request, binding and display
hooks (apple.c, apple-dpin-state.c, apple-dpin-request.h) are compiled from
the tree against host mocks: an ordered domain queue, the freezable queue, a
clock, tunnels whose paths hold their HopIDs until the last reference goes,
and a display side that admits, fails or completes its attempts where each
case puts them. The real DPRX work and callback bring the held pair up. The
cases replay a cold plug and a replug, a display refused a pipeline after the
hold, a pipeline retry in flight when the second display comes and one retired
before it starts, a completion while hotplug is frozen or the domain stops, and
the re-link of a lone tunnel to a higher rate held the same way. On every host
policy that retries for a display pipeline (M1, M3, M4, T6050), a retry stalled
past the DPRX deadline keeps its tunnel.

A tunnel whose DPRX read times out while its re-link waits is kept, and the
re-link is made by pairing and the re-link work from the domain queue, as any
other. Those cases time it out with the second display still there or gone,
after the hold was deferred for system sleep (then nothing is set up again
until the PM gate reopens), with its pairing pass queued for thaw, with an
inline pairing pass (tb_complete()) in the window before the DPRX work drops
the tunnel, and after a failed setup put the first display on the second DP
IN (the displays keep their DP IN adapters). The lone tunnel's re-link up
times out at HBR2 likewise: re-linked at HBR3 on the same pair, then counted
for the debounce, or at HBR2 at once if HBR3 cannot be set up, or at HBR2
once more if it does not come up; nothing is set up while the PM gate is
closed, and a router that replaces the dock before thaw is paired afresh.
The second display coming back while that tunnel, re-linked at HBR3, reads
its DPRX has the re-link down held; if that read times out, the re-link
down is made, on the same pair first and once the PM gate admits it,
rather than the HBR2 fallback of a re-link up that did not come up (the
displays keep their DP IN adapters); if the second display goes again
before it is made, the fallback is, on the same pair.

Every tunnel the re-link machinery tears down or sets up goes through one
admission point, tb_dp_relink_enter(), against the PM gate (a mock that
counts its readers and can close the moment the last one leaves, as the PM
notifier does). With the gate closed at each path, nothing is set up until
it reopens, and then the work is done: the re-link down; the re-link up,
with the gate closing after the re-link work's own check; the HBR2 fallback
of a lone tunnel re-linked up, and of one whose re-link down another display
waited for until that display went, hotplug on or frozen until thaw; the
teardown of a kept tunnel; and the pairing pass after a teardown, after a
failed re-link, or after a re-link down that made room for another display
(that display's tunnel is set up only once the gate reopens). When the HBR2
fallback of a tunnel re-linked up cannot set it up again, the pass that
pairs its display again keeps the HBR2 cap until the gate admits it, so it
is set up at HBR2, not at the rate that just failed, as with the gate
open. A negative control takes the gate out of the admission point, and
one per path goes around it.

Each negative control puts one piece back as it was, or takes a fix out; the
replay must then fail (exit 77 from a CHECK).
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TBDIR = ROOT / 'drivers/thunderbolt'
FILES = {'tb': 'tb.c', 'tunnel': 'tunnel.c', 'apple': 'apple.c', 'dpin': 'apple-dpin-state.c',
         'request': 'apple-dpin-request.h'}

FUNCTIONS = [
    ('tunnel', 'dprx_timeout_to_ktime'), ('tunnel', 'tb_tunnel_set_active'),
    ('tunnel', 'tb_dp_host_awaits_display'), ('tunnel', 'tb_dp_dprx_work'),
    ('tunnel', 'tb_dp_tunnel_dprx_pending'), ('tunnel', 'tb_dp_tunnel_release_display'),
    ('tunnel', 'tb_dp_tunnel_retirable'),
    ('tunnel', 'tb_dp_dprx_start'), ('tunnel', 'tb_dp_dprx_stop'), ('tunnel', 'tb_dp_tunnel_cancel_dprx'),
    ('dpin', 'apple_dpin_token_request'), ('dpin', 'apple_dpin_token_admit'),
    ('dpin', 'apple_dpin_token_complete'), ('dpin', 'apple_dpin_token_revoke'),
    ('dpin', 'apple_dpin_token_access'), ('dpin', 'apple_dpin_stale_binding'),
    ('dpin', 'apple_dpin_admission_blocked'), ('dpin', 'apple_dpin_awaits_display'),
    ('dpin', 'apple_dpin_retirable'), ('dpin', 'apple_dpin_request_event'),
    ('dpin', 'apple_dpin_step'),
    ('apple', 'apple_dpin_request'), ('apple', 'apple_dpin_dcp_set_active'),
    ('apple', 'apple_dpin_index_for_port'), ('apple', 'apple_dpin_valid'),
    ('apple', 'apple_nhi_dp_tunnel_changed'), ('apple', 'apple_nhi_dp_tunnel_awaits_display'),
    ('apple', 'apple_nhi_dp_tunnel_retirable'), ('apple', 'apple_nhi_dp_tunnel_retire_waiting'),
    ('tb', 'tb_dp_relink_retry_delay'), ('tb', 'tb_dp_host_rate_for'),
    ('tb', 'tb_dp_relink_down'), ('tb', 'tb_dp_host_rate_after'),
    ('tb', 'tb_dp_relink_up_wanted'), ('tb', 'tb_dp_relink_up'), ('tb', 'tb_dp_relink_backoff'),
    ('tb', 'tb_dp_relink_activation'), ('tb', 'tb_dp_relink_settle'),
    ('tb', 'tb_dp_relink_settle_tunnel'), ('tb', 'tb_dp_host_drop_relink_up'),
    ('tb', 'tb_dp_host_shares_rate'), ('tb', 'tb_tunnel_is_host_dp'), ('tb', 'tb_dp_host_tunnels'),
    ('tb', 'tb_dp_host_free_pairs'), ('tb', 'tb_dp_relink_wanted'),
    ('tb', 'tb_dp_tunnel_inactive'), ('tb', 'tb_dp_relink_kept'), ('tb', 'tb_dp_relink_held_on'),
    ('tb', 'tb_dp_host_clear_relink'), ('tb', 'tb_dp_host_cancel_relink_up'),
    ('tb', 'tb_dp_relink_arm'), ('tb', 'tb_dp_relink_enter'), ('tb', 'tb_dp_relink_exit'),
    ('tb', 'tb_dp_host_relink'), ('tb', 'tb_dp_host_rate_limit'),
    ('tb', 'tb_dp_host_tunnel_gone'), ('tb', 'tb_dp_host_relink_marked'),
    ('tb', 'tb_dp_host_relink_up'), ('tb', 'tb_dp_host_relink_kept'), ('tb', 'tb_dp_relink_replay'),
    ('tb', 'tb_dp_relink_pair_again'),
    ('tb', 'tb_dp_relink_unhold'), ('tb', 'tb_tunnel_dp'), ('tb', 'tb_dp_tunnel_work'), ('tb', 'tb_dp_relink_work'),
    ('tb', 'tb_dp_relink_run'), ('tb', 'tb_queue_dp_tunnels'), ('tb', 'tb_dp_resource_unavailable'),
    ('tb', 'tb_dp_resource_available'), ('tb', 'tb_dp_tunnel_active'), ('tb', 'tb_dp_tunnel_awaits'),
    ('tb', 'tb_freeze_noirq'), ('tb', 'tb_thaw_noirq'),
]

# name -> (source, old, new): each puts one piece back as it was, or takes a fix out.
MUTATIONS = {
    # The hold itself, and the wakeup that brings the held pair up.
    'relink-under-activate': ('tb', 'return action == TB_DP_RELINK_NOW && dprx_pending ? TB_DP_RELINK_HOLD : action;',
                              'return action;'),
    'no-wakeup': ('tb', '\t/* A re-link held for this tunnel\'s display may go ahead now. */\n\ttb_dp_relink_unhold(tb);\n',
                  ''),
    'hold-after-dprx': ('tunnel', 'tunnel->state == TB_TUNNEL_ACTIVATING &&', ''),
    'hold-without-display': ('tunnel', '\tif (tunnel->host_dp_notified &&\n\t    !ops->dp_tunnel_retire_waiting(',
                             '\tif (true || !ops->dp_tunnel_retire_waiting('),
    # 1: a hold outlives the DPRX deadline once the display waits for a pipeline.
    'pipeline-wait-no-reconsider': ('tunnel', '\t\tif (tunnel->await_callback)\n\t\t\ttunnel->await_callback(tunnel, tunnel->callback_data);\n', ''),
    # 2: an admitted pipeline retry loses its binding mid-Activate.
    'retire-in-flight': ('dpin', 'return apple_dpin_awaits_display(s, p) && t->admitted != generation;',
                         'return apple_dpin_awaits_display(s, p);'),
    # An admitted pipeline retry no longer counts as waiting: its DPRX deadline runs out under it.
    'retry-deadline-runs': ('apple', '\treturn apple_dpin_awaits_display(&c->state, c->acio->dp);',
                            '\treturn apple_dpin_awaits_display(&c->state, c->acio->dp) &&\n'
                            '\t       apple_dpin_retirable(&c->state, &c->tokens, c->acio->dp,\n'
                            '\t\t\t\t    c->tokens.requested);'),
    # The DPRX work wakes the held re-link while an admitted retry may still wake the display.
    'wake-in-flight': ('tb', 'if (tb_dp_relink_held_on(tunnel) && tb_dp_tunnel_retirable(tunnel))',
                       'if (tb_dp_relink_held_on(tunnel))'),
    'decide-on-awaits': ('tunnel', '\tif (tunnel->host_dp_notified &&\n\t    !ops->dp_tunnel_retire_waiting(tunnel->tb->nhi, tunnel->src_port,\n\t\t\t\t\t   tunnel->host_dp_generation))\n\t\treturn false;\n\ttunnel->host_dp_notified = false;\n\treturn true;',
                         '\t(void)ops;\n\treturn tb_dp_host_awaits_display(tunnel);'),
    # A DPRX timeout drops the DP IN of the tunnel whose re-link waited (to HBR2 or up), as before the hold.
    'timeout-drops-input': ('tb', '\t} else if (tb_dp_relink_wanted(tunnel) || tunnel->host_relinked_up) {',
                            '\t} else if (tunnel->host_relinked_up) {'),
    # A re-link up's HBR2 fallback goes ahead of the re-link down another display waits for.
    'fallback-before-relink': ('tb', '\t\tif (tb_dp_relink_wanted(tunnel))\n\t\t\tcontinue;\n\t\tif (!tb_dp_relink_enter(tb))',
                               '\t\tif (!tunnel->host_relinked_up && tb_dp_relink_wanted(tunnel))\n\t\t\tcontinue;\n'
                               '\t\tif (!tb_dp_relink_enter(tb))'),
    # 4: a completion while hotplug is frozen drops the pairing request.
    'frozen-unrecorded': ('tb', '\tif (!tcm->stopping)\n\t\ttcm->dp_tunnel_pending = true;\n\ttb_queue_dp_tunnels(tb);\n',
                          '\t(void)tcm;\n\ttb_queue_dp_tunnels(tb);\n'),
    'stop-records': ('tb', '\tif (!tcm->stopping)\n\t\ttcm->dp_tunnel_pending = true;\n\ttb_queue_dp_tunnels(tb);\n',
                     '\ttcm->dp_tunnel_pending = true;\n\ttb_queue_dp_tunnels(tb);\n'),
    # The timeout keeps the tunnel only while its re-link is held, not deferred or queued.
    'timeout-needs-hold': ('tb', '\tif (!tb_dp_host_shares_rate(tb) || !tb_tunnel_is_host_dp(tunnel))\n\t\treturn false;\n\tup = tb_dp_host_tunnels(tb, NULL);',
                           '\tif (!(tcm->dp_relink_held || tcm->dp_relink_up_held) || !tb_tunnel_is_host_dp(tunnel))\n\t\treturn false;\n\tup = tb_dp_host_tunnels(tb, NULL);'),
    # The timeout does not keep a tunnel whose re-link to a higher rate is due.
    'up-timeout-drops-input': ('tb', '!= TB_DP_RELINK_NONE ||\n\t       tb_dp_relink_up(tunnel->host_relink, up,',
                               '!= TB_DP_RELINK_NONE ||\n\t       tb_dp_relink_up(false, up,'),
    # A kept tunnel's DPRX work may still hold it: re-linked by a pass that is not on the domain queue.
    'kept-not-held': ('tb', '\tif (tunnel->host_relink_kept && tb_dp_tunnel_dprx_pending(tunnel))\n\t\treturn TB_DP_RELINK_HOLD;\n', ''),
    'kept-released-inline': ('tb', '\t\tif (release)\n\t\t\ttb_dp_tunnel_cancel_dprx(tunnel);\n\t\telse if (tb_dp_tunnel_dprx_pending(tunnel))\n\t\t\tcontinue;\n',
                             '\t\t(void)release;\n\t\ttb_dp_tunnel_cancel_dprx(tunnel);\n'),
    # A kept tunnel whose re-link is not wanted any more is left listed, dark.
    'kept-never-dropped': ('tb', '\t\tif (tb_dp_relink_wanted(tunnel))\n\t\t\tcontinue;\n\t\tif (!tb_dp_relink_enter(tb))',
                           '\t\tif (true)\n\t\t\tcontinue;\n\t\tif (!tb_dp_relink_enter(tb))'),
    # A kept re-link up whose re-link down is not wanted any more loses its DP IN, without the HBR2 fallback.
    'kept-up-dropped': ('tb', '\t\ttb_dp_tunnel_inactive(tb, tunnel);\n\t\ttb_dp_relink_exit(tb);',
                        '\t\ttb_dp_resource_unavailable(tb, tunnel->src_port, "DPRX negotiation failed");\n'
                        '\t\ttb_dp_relink_exit(tb);'),
    # The HBR2 fallback as before: torn down, and paired afresh at HBR2 (maybe on the other DP IN).
    'fallback-not-same-pair': ('tb', '\t\tif (!tb_dp_host_relink(tb, tunnel, TB_DP_HOST_RATE_SHARED,\n'
                               '\t\t\t\t       "not active at the higher rate"))\n\t\t\ttcm->dp_host_hold_shared = true;',
                               '\t\ttb_deactivate_and_free_tunnel(tunnel);\n\t\ttcm->dp_host_hold_shared = true;\n'
                               '\t\ttb_queue_dp_tunnels(tb);'),
    # The system sleep gate. The choke point itself admits whatever the PM state:
    'gate-bypassed': ('tb', 'if (!tcm->dp_relink_admitted && !tb_dp_relink_admit()) {',
                      'if (!tcm->dp_relink_admitted && !admit_ungated()) {'),
    # ... or one path goes around it: the re-link down,
    'relink-down-ungated': ('tb', 'admitted = action == TB_DP_RELINK_NOW && tb_dp_relink_enter(tb);\n\t\tif',
                            'admitted = action == TB_DP_RELINK_NOW && enter_ungated(tb);\n\t\tif'),
    # the re-link up,
    'relink-up-ungated': ('tb', 'admitted = action == TB_DP_RELINK_NOW && tb_dp_relink_enter(tb);\n\tif',
                          'admitted = action == TB_DP_RELINK_NOW && enter_ungated(tb);\n\tif'),
    # a kept tunnel's HBR2 fallback or teardown,
    'kept-ungated': ('tb', '\t\tif (!tb_dp_relink_enter(tb))\n\t\t\tcontinue;',
                     '\t\tif (!enter_ungated(tb))\n\t\t\tcontinue;'),
    # the pass that sets up what a re-link made room for or tore down,
    'pairing-ungated': ('tb', '\tif (tcm->dp_relink_pairing) {\n\t\tif (!tb_dp_relink_enter(tb))',
                        '\tif (tcm->dp_relink_pairing) {\n\t\tif (!enter_ungated(tb))'),
    # or that pass is not marked as part of the re-link: after a failed setup,
    'retry-pass-unmarked': ('tb', '\ttcm->dp_relink_pairing = true;\n\ttb_queue_dp_tunnels(tb);\n\treturn NULL;',
                            '\ttb_queue_dp_tunnels(tb);\n\treturn NULL;'),
    # or after a kept tunnel's teardown,
    'teardown-pass-unmarked': ('tb', '\t\tif (!tunnel->host_relinked_up)\n\t\t\ttcm->dp_relink_pairing = true;\n', ''),
    # or after a re-link down that made room for another tunnel.
    'relink-down-pass-unmarked': ('tb', '\t\t\t/* Pairing what it made room for is part of it. */\n'
                                  '\t\t\ttcm->dp_relink_pairing = true;\n', ''),
    # The HBR2 cap is used up by a pass the gate then keeps from pairing.
    'cap-dropped-when-deferred': ('tb', '\t\trecovering = tcm->dp_host_hold_shared;\n\t}\n',
                                  '\t\trecovering = tcm->dp_host_hold_shared;\n'
                                  '\t\ttcm->dp_host_hold_shared = false;\n\t}\n'),
}


def function(text, name):
    match = re.search(r'^(?:VISIBLE_IF_KUNIT\s+)?[A-Za-z_][^\n;{}#()]*?\b' + re.escape(name) +
                      r'\((?:[^;{}])*?\)\n\{', text, re.M)
    assert match, 'missing production function ' + name
    pos = text.index('\n{', match.start()) + 2
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return re.sub(r'^VISIBLE_IF_KUNIT\s+', '', text[match.start():pos]) + '\n'


def policy(text, name):
    match = re.search(r'^const struct apple_dpin_policy ' + name + r' = \{.*?^\};\n', text, re.M | re.S)
    assert match, 'missing policy ' + name
    return match.group(0)


FAKE_LINUX = {
    'bits.h': '#define BIT(n) (1UL << (n))\n',
    'types.h': '#include <stdbool.h>\n#include <stdint.h>\ntypedef uint64_t u64;\ntypedef uint32_t u32;\n'
               'typedef uint8_t u8;\ntypedef int64_t s64;\n',
    'errno.h': '#include <asm-generic/errno.h>\n',
}

PRE = r'''
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <linux/types.h>
#include <linux/bits.h>
#include <linux/errno.h>
static char logbuf[1 << 16];
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK line %d: %s\n", __LINE__, #x); \
    if (getenv("TB_REPLAY_LOG")) { fputs(logbuf, stderr); } exit(77); } } while (0)
#define fallthrough __attribute__((fallthrough))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define __iomem

/* ---- the log: every message the code under test prints ---- */
static size_t loglen;
static void logf_(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    loglen += vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, ap);
    va_end(ap);
    CHECK(loglen < sizeof(logbuf) - 256);
}
static unsigned int logged(const char *s)
{
    unsigned int n = 0;

    for (const char *p = logbuf; (p = strstr(p, s)); p++)
        n++;
    return n;
}
#define tb_tunnel_info(t, ...) logf_(__VA_ARGS__)
#define tb_tunnel_dbg(t, ...) logf_(__VA_ARGS__)
#define tb_tunnel_warn(t, ...) logf_(__VA_ARGS__)
#define tb_port_dbg(p, ...) logf_(__VA_ARGS__)
#define tb_port_warn(p, ...) logf_(__VA_ARGS__)
#define tb_port_info(p, ...) logf_(__VA_ARGS__)
#define tb_dbg(t, ...) logf_(__VA_ARGS__)
#define tb_warn(t, ...) logf_(__VA_ARGS__)
#define dev_info(d, ...) logf_(__VA_ARGS__)
#define dev_dbg(d, ...) logf_(__VA_ARGS__)
#define dev_warn(d, ...) logf_(__VA_ARGS__)

/* ---- lists ---- */
struct list_head { struct list_head *next, *prev; };
static void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{ n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n; }
static void list_del(struct list_head *e)
{ e->prev->next = e->next; e->next->prev = e->prev; e->next = e->prev = NULL; }
static void list_del_init(struct list_head *e)
{ if (e->next && e->next != e) { e->prev->next = e->next; e->next->prev = e->prev; } INIT_LIST_HEAD(e); }
#define list_entry(p, t, m) container_of(p, t, m)
#define list_for_each_entry(pos, head, m) \
    for (pos = list_entry((head)->next, typeof(*pos), m); &pos->m != (head); \
         pos = list_entry(pos->m.next, typeof(*pos), m))
#define list_for_each_entry_safe(pos, n, head, m) \
    for (pos = list_entry((head)->next, typeof(*pos), m), n = list_entry(pos->m.next, typeof(*pos), m); \
         &pos->m != (head); pos = n, n = list_entry(n->m.next, typeof(*n), m))

/* ---- locks ---- */
struct mutex { int held; };
static void mutex_lock(struct mutex *m) { CHECK(!m->held); m->held = 1; }
static void mutex_unlock(struct mutex *m) { CHECK(m->held); m->held = 0; }
#define lockdep_assert_held(m) CHECK((m)->held)
static struct mutex *guard_mutex_lock(struct mutex *m) { mutex_lock(m); return m; }
static void guard_mutex_unlock(struct mutex **m) { if (*m) mutex_unlock(*m); }
#define GUARD_CAT2(a, b) a##b
#define GUARD_CAT(a, b) GUARD_CAT2(a, b)
#define guard(type) struct mutex *GUARD_CAT(guard_, __COUNTER__) \
    __attribute__((unused, cleanup(guard_##type##_unlock))) = guard_##type##_lock
#define scoped_guard(type, x) \
    for (struct mutex *scope __attribute__((cleanup(guard_##type##_unlock))) = guard_##type##_lock(x), \
         *scope_done = NULL; !scope_done; scope_done = scope)

/* ---- time: one jiffy is one ms, ktime counts ms ---- */
typedef s64 ktime_t;
#define KTIME_MAX INT64_MAX
static unsigned long jiffies;
#define HZ 1000
static unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
static unsigned int jiffies_to_msecs(unsigned long j) { return j; }
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
static ktime_t ktime_get(void) { return jiffies; }
static ktime_t ktime_add_ms(ktime_t t, s64 ms) { return t + ms; }
static bool ktime_before(ktime_t a, ktime_t b) { return a < b; }

/* ---- work queues: the ordered domain queue, the freezable one, the DP IN one ---- */
struct work_struct { void (*func)(struct work_struct *); bool pending; void *wq; long due; bool timer; };
struct delayed_work { struct work_struct work; };
struct wq { const char *name; struct work_struct *items[64]; unsigned int n; bool frozen; };
static struct wq domain_q = { "domain" }, freezable_q = { "freezable" }, dpin_q = { "dpin" };
static void *system_freezable_wq = &freezable_q;
#define INIT_WORK(w, f) ((w)->func = (f))
#define INIT_DELAYED_WORK(w, f) ((w)->work.func = (f))
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
static struct work_struct *timers[64];
static unsigned int ntimers;
static void wq_add(struct wq *q, struct work_struct *w)
{ CHECK(q->n < 64); w->wq = q; w->timer = false; q->items[q->n++] = w; }
static void wq_remove(struct work_struct *w)
{
    struct wq *q = w->wq;

    if (w->timer) {
        for (unsigned int i = 0; i < ntimers; i++)
            if (timers[i] == w) { timers[i] = timers[--ntimers]; break; }
        return;
    }
    for (unsigned int i = 0; q && i < q->n; i++)
        if (q->items[i] == w) { memmove(&q->items[i], &q->items[i + 1], (q->n - i - 1) * sizeof(w)); q->n--; break; }
}
/* System sleep starts as soon as this work is queued (after the gate check that queued it). */
static bool pm_sleeping;
static struct work_struct *close_on_queue;
static bool queue_work(void *wq, struct work_struct *w)
{
    if (w->pending)
        return false;
    w->pending = true;
    wq_add(wq, w);
    if (w == close_on_queue) {
        close_on_queue = NULL;
        pm_sleeping = true;
    }
    return true;
}
static void arm(void *wq, struct work_struct *w, unsigned long delay)
{
    w->pending = true;
    if (!delay) { wq_add(wq, w); return; }
    w->wq = wq; w->timer = true; w->due = jiffies + delay;
    CHECK(ntimers < 64); timers[ntimers++] = w;
}
static bool queue_delayed_work(void *wq, struct delayed_work *dw, unsigned long delay)
{
    if (dw->work.pending)
        return false;
    arm(wq, &dw->work, delay);
    return true;
}
static bool mod_delayed_work(void *wq, struct delayed_work *dw, unsigned long delay)
{
    bool was = dw->work.pending;

    if (was)
        wq_remove(&dw->work);
    arm(wq, &dw->work, delay);
    return was;
}
static bool cancel_delayed_work(struct delayed_work *dw)
{
    if (!dw->work.pending)
        return false;
    wq_remove(&dw->work);
    dw->work.pending = false;
    return true;
}
static bool cancel_work(struct work_struct *w)
{
    if (!w->pending)
        return false;
    wq_remove(w);
    w->pending = false;
    return true;
}
#define delayed_work_pending(dw) ((dw)->work.pending)

/* ---- Thunderbolt types ---- */
#define TB_HOST_DP_NOTIFY		BIT(0)
#define TB_HOST_DP_HPD_ON_ACTIVATE	BIT(1)
#define TB_HOST_DP_ACTIVE_BEFORE_DPRX	BIT(2)
#define TB_HOST_DP_KEEP_DPRX_TIMEOUT	BIT(3)
#define TB_HOST_DP_ADAPTER_QUIRKS	BIT(4)
#define TB_HOST_DP_INITIAL_BW_GRANT	BIT(5)
#define TB_HOST_DP_BW_HELD_ACK		BIT(6)
#define TB_HOST_DP_SHARED_RATE		BIT(7)
#define TB_DP_HOST_RATE_ALONE	8100
#define TB_DP_HOST_RATE_SHARED	5400
#define TB_DP_RELINK_DELAY_MS	2500
#define TB_DP_RELINK_POLL_MS	500
#define TB_DP_RELINK_RETRIES	5
#define TB_DP_RELINK_DELAY_MAX_MS	40000
#define TB_DP_RELINK_CALM_MS		60000
#define TB_DPRX_WAIT_TIMEOUT		25
#define TB_DPRX_POLL_DELAY		50
#define TB_DPRX_AWAIT_POLL_DELAY	1000
static int dprx_timeout = 12000;
enum tb_dp_relink { TB_DP_RELINK_NONE, TB_DP_RELINK_WAIT, TB_DP_RELINK_NOW, TB_DP_RELINK_HOLD };
enum tb_tunnel_state { TB_TUNNEL_INACTIVE, TB_TUNNEL_ACTIVATING, TB_TUNNEL_ACTIVE };
enum tb_tunnel_type { TB_TUNNEL_PCI, TB_TUNNEL_DP, TB_TUNNEL_DMA, TB_TUNNEL_USB3 };
enum { TB_TUNNEL_ACTIVATED, TB_TUNNEL_DEACTIVATED };
enum { PORT_LANE, PORT_DPIN, PORT_DPOUT };
struct device { int unused; };
struct tb_switch;
struct tb_port { struct tb_switch *sw; u8 port; int kind; bool enabled; int hopids; struct list_head list; };
struct tb_switch { struct tb *tb; u64 route; struct { int max_port_number; } config; struct tb_port ports[16]; struct device dev; };
#define tb_switch_for_each_port(sw, p) \
    for ((p) = &(sw)->ports[1]; (p) <= &(sw)->ports[(sw)->config.max_port_number]; (p)++)
struct tb_nhi;
struct tb_nhi_ops {
    void (*dp_tunnel_changed)(struct tb_nhi *nhi, u8 in_port, u64 generation, bool active);
    bool (*dp_tunnel_awaits_display)(struct tb_nhi *nhi, struct tb_port *in);
    bool (*dp_tunnel_retirable)(struct tb_nhi *nhi, struct tb_port *in, u64 generation);
    bool (*dp_tunnel_retire_waiting)(struct tb_nhi *nhi, struct tb_port *in, u64 generation);
};
struct tb_nhi { const struct tb_nhi_ops *ops; unsigned long host_dp_policy; };
struct tb { struct mutex lock; struct tb_nhi *nhi; void *wq; struct tb_switch *root_switch; struct device dev; };
struct kref { int count; };
struct tb_tunnel {
    struct kref kref;
    struct tb *tb;
    struct tb_port *src_port, *dst_port;
    struct list_head list;
    enum tb_tunnel_type type;
    enum tb_tunnel_state state;
    u32 max_rate, host_rate_limit, host_rate, host_max_rate;
    bool host_relink, host_relinked_up, host_relink_kept, dprx_started, dprx_canceled;
    u64 host_dp_generation;
    bool host_dp_notified;
    ktime_t dprx_timeout;
    struct delayed_work dprx_work;
    void (*callback)(struct tb_tunnel *tunnel, void *data);
    void (*await_callback)(struct tb_tunnel *tunnel, void *data);
    void *callback_data;
    /* model: what the DP IN adapter reports */
    bool dprx_done;
};
struct tb_cm {
    struct list_head tunnel_list, dp_resources;
    bool hotplug_active, stopping;
    struct work_struct dp_tunnel_work;
    bool dp_tunnel_pending;
    struct delayed_work dp_relink_work;
    struct work_struct dp_relink_run;
    bool dp_relink_deferred;
    unsigned int dp_relink_tries;
    unsigned long dp_relink_due;
    unsigned int dp_relink_delay;
    unsigned long dp_relinked_at;
    bool dp_host_hold_shared, dp_relink_held, dp_relink_up_held, dp_relink_pairing;
    unsigned int dp_relink_admitted;
};
static struct tb domain_tb;
static struct tb_cm domain_cm;
static int domain_refs = 1, live_tunnels;
static struct tb_cm *tb_priv(struct tb *tb) { CHECK(tb == &domain_tb); return &domain_cm; }
static struct tb *tcm_to_tb(struct tb_cm *tcm) { CHECK(tcm == &domain_cm); return &domain_tb; }
static struct tb *tb_domain_get(struct tb *tb) { CHECK(domain_refs >= 1); domain_refs++; return tb; }
static void tb_domain_put(struct tb *tb) { CHECK(domain_refs > 1); domain_refs--; }
static void tb_tunnel_get(struct tb_tunnel *t) { CHECK(t->kref.count > 0); t->kref.count++; }
static void tb_tunnel_put(struct tb_tunnel *t)
{
    CHECK(t->kref.count > 0);
    if (--t->kref.count)
        return;
    /* the paths go with the last reference: their HopIDs are free again */
    t->src_port->hopids--;
    t->dst_port->hopids--;
    CHECK(!t->dprx_work.work.pending);
    live_tunnels--;
    free(t);
}
static bool tb_tunnel_is_dp(const struct tb_tunnel *t) { return t->type == TB_TUNNEL_DP; }
static bool tb_tunnel_is_active(const struct tb_tunnel *t) { return t->state == TB_TUNNEL_ACTIVE; }
static void tb_tunnel_event(struct tb *tb, int e, int type, struct tb_port *a, struct tb_port *b) {}
static bool tb_port_is_dpin(const struct tb_port *p) { return p->kind == PORT_DPIN; }
static bool tb_port_is_dpout(const struct tb_port *p) { return p->kind == PORT_DPOUT; }
static bool tb_port_is_enabled(struct tb_port *p) { return p->enabled; }
static u64 tb_route(const struct tb_switch *sw) { return sw->route; }
static bool tb_port_is_apple_host_dpin(const struct tb_port *p) { return tb_port_is_dpin(p) && !tb_route(p->sw); }
static bool tb_host_dp_policy(struct tb_nhi *nhi, unsigned long f) { return nhi->host_dp_policy & f; }
static bool tb_dp_is_usb4(struct tb_switch *sw) { return true; }
static int tb_dp_wait_dprx(struct tb_tunnel *t, int ms) { return t->dprx_done ? 0 : -ETIMEDOUT; }
static void tb_dp_dump_apple(struct tb_tunnel *t) {}
static bool tb_acpi_may_tunnel_dp(void) { return true; }
static int tb_tunnel_consumed_bandwidth(struct tb_tunnel *t, int *up, int *down) { *up = 0; *down = 17280; return 0; }
static void tb_reclaim_usb3_bandwidth(struct tb *tb, struct tb_port *i, struct tb_port *o) {}
static void tb_configure_asym(struct tb *tb, struct tb_port *i, struct tb_port *o, int up, int down) {}
static void tb_recalc_estimated_bandwidth(struct tb *tb) {}
static void tb_increase_tmu_accuracy(struct tb_tunnel *t) {}
static void tb_enter_redrive(struct tb_port *p) {}
static void tb_exit_redrive(struct tb_port *p) {}
/*
 * The PM gate (tb_dp_pm_gate): admission is a read hold, taken only while no
 * system sleep transition is in progress. The PM notifier closes the gate
 * once the last reader is out (@close_on_release models a PREPARE that
 * waited for one).
 */
static int gate_readers;
static bool close_on_release;
static bool tb_dp_relink_admit(void)
{
    if (pm_sleeping)
        return false;
    gate_readers++;
    return true;
}
static void tb_dp_relink_done(void)
{
    CHECK(gate_readers > 0);
    if (!--gate_readers && close_on_release) {
        close_on_release = false;
        pm_sleeping = true;
    }
}
/* A negative control's way past the gate: admitted whatever the PM state. */
static bool admit_ungated(void) { gate_readers++; return true; }
/* tb_dp_relink_enter() skipped at one call site: entered whatever the PM state. */
static bool enter_ungated(struct tb *tb)
{
    if (!domain_cm.dp_relink_admitted)
        gate_readers++;
    domain_cm.dp_relink_admitted++;
    return true;
}
static bool tb_dp_pm_open(void) { return !pm_sleeping; }
#define WARN_ON(x) ({ bool w_ = (x); CHECK(!w_); w_; })
static int pm_runtime_resume_and_get(struct device *d) { return 0; }
static void pm_runtime_mark_last_busy(struct device *d) {}
static void pm_runtime_put_autosuspend(struct device *d) {}
static struct tb_tunnel *tb_find_tunnel(struct tb *tb, int type, struct tb_port *in, struct tb_port *out)
{
    struct tb_tunnel *t;

    list_for_each_entry(t, &domain_cm.tunnel_list, list)
        if ((in && t->src_port == in) || (out && t->dst_port == out))
            return t;
    return NULL;
}
/* tb_find_dp_out(): the first free DP OUT of another router */
static struct tb_port *tb_find_dp_out(struct tb *tb, struct tb_port *in)
{
    struct tb_port *p;

    list_for_each_entry(p, &domain_cm.dp_resources, list)
        if (tb_port_is_dpout(p) && !p->enabled && p->sw != in->sw)
            return p;
    return NULL;
}
static u64 next_generation = 1;
static void tb_dp_tunnel_awaits(struct tb_tunnel *tunnel, void *data);
static void tb_dp_tunnel_active(struct tb_tunnel *tunnel, void *data);
static void tb_queue_dp_tunnels(struct tb *tb);
static void tb_dp_relink_unhold(struct tb *tb);
static void tb_dp_relink_pair_again(struct tb *tb);
static bool tb_dp_relink_wanted(struct tb_tunnel *tunnel);
static bool tb_dp_relink_held_on(struct tb_tunnel *tunnel);
static void tb_dp_resource_unavailable(struct tb *tb, struct tb_port *port, const char *reason);
static unsigned int tb_dp_host_free_pairs(struct tb *tb);
static void tb_dp_relink_replay(struct tb *tb);
static bool tb_dp_relink_enter(struct tb *tb);
static void tb_dp_relink_exit(struct tb *tb);
static struct tb_tunnel *tb_dp_host_relink(struct tb *tb, struct tb_tunnel *tunnel, u32 rate_limit, const char *why);
static int tb_dp_dprx_start(struct tb_tunnel *tunnel);
static void tb_dp_dprx_stop(struct tb_tunnel *tunnel);
static void tb_dp_dprx_work(struct work_struct *work);
static void tb_tunnel_dp(struct tb *tb);
/* A display-side retry run in the window before a teardown announces the tunnel down. */
static void (*teardown_window)(void);
/* Setting up a tunnel fails: once from this DP IN adapter, or at this rate limit every time */
static struct tb_port *fail_in;
static u32 fail_rate;
/*
 * tb_tunnel_one_dp() as far as these cases see it: the tunnel's paths take
 * HopIDs on both adapters (free again only with its last reference), it is
 * activated (its DPRX read started, as tb_tunnel_activate() does) and
 * announced to the host glue (tb_dp_tunnel_notify()).
 */
static struct tb_tunnel *tb_tunnel_one_dp(struct tb *tb, struct tb_port *in, struct tb_port *out, u32 rate_limit)
{
    struct tb_tunnel *t;

    CHECK(tb->lock.held);
    if (in == fail_in || (fail_rate && rate_limit == fail_rate)) {
        if (in == fail_in)
            fail_in = NULL;
        return NULL;
    }
    /* an earlier tunnel's paths still hold the HopIDs: setting up now fails in hardware */
    CHECK(!in->hopids && !out->hopids);
    t = calloc(1, sizeof(*t));
    CHECK(t);
    in->hopids++; out->hopids++; live_tunnels++;
    t->kref.count = 1; t->tb = tb; t->src_port = in; t->dst_port = out; t->type = TB_TUNNEL_DP;
    t->host_rate_limit = rate_limit;
    t->host_max_rate = TB_DP_HOST_RATE_ALONE;
    t->host_rate = rate_limit ? min(rate_limit, (u32)TB_DP_HOST_RATE_ALONE) : TB_DP_HOST_RATE_ALONE;
    t->max_rate = t->host_rate;
    t->callback = tb_dp_tunnel_active; t->callback_data = tb;
    t->await_callback = tb_dp_tunnel_awaits;
    INIT_DELAYED_WORK(&t->dprx_work, tb_dp_dprx_work);
    list_add_tail(&t->list, &domain_cm.tunnel_list);
    in->enabled = out->enabled = true;
    t->state = TB_TUNNEL_ACTIVATING;
    CHECK(tb_dp_dprx_start(t) == -EINPROGRESS);
    t->host_dp_generation = next_generation++;
    t->host_dp_notified = true;
    tb->nhi->ops->dp_tunnel_changed(tb->nhi, in->port, t->host_dp_generation, true);
    return t;
}
/* tb_deactivate_and_free_tunnel(): announced down, DPRX stopped, adapters off, list reference dropped */
static void tb_deactivate_and_free_tunnel(struct tb_tunnel *t)
{
    CHECK(t->tb->lock.held);
    if (teardown_window) {
        void (*window)(void) = teardown_window;

        teardown_window = NULL;
        window();
    }
    if (t->host_dp_notified) {
        t->host_dp_notified = false;
        t->tb->nhi->ops->dp_tunnel_changed(t->tb->nhi, t->src_port->port, t->host_dp_generation, false);
    }
    tb_dp_dprx_stop(t);
    t->state = TB_TUNNEL_INACTIVE;
    t->src_port->enabled = t->dst_port->enabled = false;
    list_del(&t->list);
    tb_tunnel_put(t);
}

/* ---- Apple glue types (the real apple-dpin-state.h and apple-dpin-request.h) ---- */
#include "apple-dpin-state.h"
#include "apple-dpin-request.h"
struct apple_cio;
struct apple_dpin_ctx {
    struct apple_cio *acio;
    struct work_struct work;
    struct delayed_work retry;
    struct mutex lock;
    void __iomem *regs;
    unsigned int idx;
    struct apple_dpin_state state;
    struct apple_dpin_tokens tokens;
    unsigned long settle_until;
    bool dcp_touched, dcp_active;
};
struct apple_cio { struct device *dev; const struct apple_dpin_policy *dp; void *dp_wq; unsigned int dpin_count; struct apple_dpin_ctx dpin[2]; };
struct apple_nhi { struct tb_nhi nhi; struct tb *tb; struct apple_cio *acio; };
struct apple_dpin_binding { struct apple_dpin_ctx *ctx; u64 generation; };
static struct apple_nhi *nhi_to_anhi(struct tb_nhi *nhi) { return container_of(nhi, struct apple_nhi, nhi); }
static void apple_dpin_request_inactive(void *data) {}
static void apple_dpin_request_mask_irqs(void *data) {}
static int apple_dpin_set_active(struct apple_cio *acio, void __iomem *regs, unsigned int idx, bool active) { return 0; }
'''

POST = r'''
/* ---- the domain: host router DP IN 5 and 6 (dpin0, dpin1), dock DP OUT 13 and 14 ---- */
static struct tb_switch host, dock;
static struct apple_cio acio;
static struct apple_nhi anhi;
static struct device dev;
static const struct tb_nhi_ops ops = {
    .dp_tunnel_changed = apple_nhi_dp_tunnel_changed,
    .dp_tunnel_awaits_display = apple_nhi_dp_tunnel_awaits_display,
    .dp_tunnel_retirable = apple_nhi_dp_tunnel_retirable,
    .dp_tunnel_retire_waiting = apple_nhi_dp_tunnel_retire_waiting,
};
#define IN0 (&host.ports[5])
#define IN1 (&host.ports[6])
#define OUT0 (&dock.ports[13])
#define OUT1 (&dock.ports[14])
#define DP0 (&acio.dpin[0])
#define DP1 (&acio.dpin[1])

static void setup(const struct apple_dpin_policy *policy)
{
    /* well past the last re-link up's calm period (dp_relinked_at 0) */
    jiffies = 10 * TB_DP_RELINK_CALM_MS;
    host.tb = dock.tb = &domain_tb;
    host.config.max_port_number = 6;
    dock.config.max_port_number = 14;
    dock.route = 1;
    for (int i = 1; i <= 14; i++) {
        host.ports[i] = (struct tb_port){ .sw = &host, .port = i };
        dock.ports[i] = (struct tb_port){ .sw = &dock, .port = i };
    }
    IN0->kind = IN1->kind = PORT_DPIN;
    OUT0->kind = OUT1->kind = PORT_DPOUT;
    domain_tb.root_switch = &host;
    domain_tb.wq = &domain_q;
    domain_tb.nhi = &anhi.nhi;
    anhi.nhi.ops = &ops;
    anhi.nhi.host_dp_policy = policy->host_policy;
    anhi.tb = &domain_tb;
    anhi.acio = &acio;
    acio.dev = &dev;
    acio.dp = policy;
    acio.dp_wq = &dpin_q;
    acio.dpin_count = 2;
    for (unsigned int i = 0; i < 2; i++) {
        acio.dpin[i].acio = &acio;
        acio.dpin[i].idx = i;
    }
    INIT_LIST_HEAD(&domain_cm.tunnel_list);
    INIT_LIST_HEAD(&domain_cm.dp_resources);
    INIT_WORK(&domain_cm.dp_tunnel_work, tb_dp_tunnel_work);
    INIT_DELAYED_WORK(&domain_cm.dp_relink_work, tb_dp_relink_work);
    INIT_WORK(&domain_cm.dp_relink_run, tb_dp_relink_run);
    domain_cm.dp_relink_delay = TB_DP_RELINK_DELAY_MS;
    domain_cm.hotplug_active = true;
    mutex_lock(&domain_tb.lock);
    list_add_tail(&IN0->list, &domain_cm.dp_resources);
    list_add_tail(&IN1->list, &domain_cm.dp_resources);
    mutex_unlock(&domain_tb.lock);
}

/* ---- the display side: apple_dpin_work_fn() and apple_dpin_up() ---- */
static u64 handed[2];
static bool dpin_work_queued(struct apple_dpin_ctx *c)
{
    for (unsigned int i = 0; i < dpin_q.n; i++)
        if (dpin_q.items[i] == &c->work)
            return true;
    return false;
}
static void dpin_down(struct apple_dpin_ctx *c)
{
    scoped_guard(mutex, &c->lock) {
        if (handed[c->idx])
            apple_dpin_token_revoke(&c->tokens, handed[c->idx]);
        handed[c->idx] = 0;
        c->regs = NULL;
        apple_dpin_step(&c->state, c->acio->dp, APPLE_DPIN_DROPPED, false, 0);
    }
}
/*
 * Run the DP IN work up to an attempt to hand the request to a display: the
 * admission, after which DCP's Activate may come before the call returns.
 * The generation admitted, 0 if none.
 */
static u64 attempt_begin(struct apple_dpin_ctx *c)
{
    unsigned int actions;
    u64 gen;

    if (dpin_work_queued(c))
        cancel_work(&c->work);
    for (;;) {
        scoped_guard(mutex, &c->lock) {
            actions = apple_dpin_step(&c->state, c->acio->dp, APPLE_DPIN_WORK, !!c->regs, 0);
            gen = c->tokens.requested;
        }
        if (actions & APPLE_DPIN_DROP) {
            dpin_down(c);
            if (actions & APPLE_DPIN_AGAIN)
                continue;
        }
        if (!(actions & APPLE_DPIN_ATTACH))
            return 0;
        mutex_lock(&c->lock);
        if (!c->state.alive || c->tokens.requested != gen) {
            mutex_unlock(&c->lock);
            continue;
        }
        c->regs = (void *)1;
        apple_dpin_token_admit(&c->tokens, gen);
        apple_dpin_step(&c->state, c->acio->dp, APPLE_DPIN_ADMITTED, true, 0);
        mutex_unlock(&c->lock);
        return gen;
    }
}
/* The attempt @gen returns @ret: as the tail of apple_dpin_work_fn(). */
static unsigned int attempt_end(struct apple_dpin_ctx *c, u64 gen, int ret)
{
    unsigned int actions = 0;
    bool stale;

    scoped_guard(mutex, &c->lock) {
        stale = !c->state.alive || c->tokens.requested != gen;
        if (!ret) {
            stale |= !apple_dpin_token_complete(&c->tokens, gen);
            if (!stale)
                handed[c->idx] = gen;
        } else {
            apple_dpin_token_revoke(&c->tokens, gen);
        }
        if (!stale)
            actions = apple_dpin_step(&c->state, c->acio->dp, APPLE_DPIN_RESULT, !!c->regs, ret);
    }
    if (stale && !ret)
        scoped_guard(mutex, &c->lock)
            apple_dpin_token_revoke(&c->tokens, gen);
    return actions;
}
/* The DP IN retry timer fires: the work runs again if the step says so. */
static bool retry_fires(struct apple_dpin_ctx *c)
{
    unsigned int actions;

    scoped_guard(mutex, &c->lock)
        actions = apple_dpin_step(&c->state, c->acio->dp, APPLE_DPIN_RETRY, !!c->regs, 0);
    return actions & APPLE_DPIN_QUEUE;
}
/* DCP's Activate on the binding it was handed for @gen. */
static int activate(struct apple_dpin_ctx *c, u64 gen)
{
    struct apple_dpin_binding b = { .ctx = c, .generation = gen };

    return apple_dpin_dcp_set_active(&b, true);
}
/* A display is handed the request: admitted, Activate meets it, the call returns. */
static u64 display_takes(struct apple_dpin_ctx *c)
{
    u64 gen = attempt_begin(c);

    CHECK(gen);
    CHECK(activate(c, gen) == 0);
    CHECK(!(attempt_end(c, gen, 0) & APPLE_DPIN_WARN));
    return gen;
}

/* ---- running the queues ---- */
static void run_one(struct work_struct *w)
{
    w->pending = false;
    CHECK(!domain_tb.lock.held);
    w->func(w);
    CHECK(!domain_tb.lock.held);
    /* no re-link admission outlives the domain lock */
    CHECK(!gate_readers && !domain_cm.dp_relink_admitted);
}
/* Let @ms pass: timers fire, the domain and (unless frozen) freezable queues run. */
static void advance(unsigned int ms)
{
    for (unsigned int t = 0; t <= ms; t++) {
        for (unsigned int i = 0; i < ntimers;) {
            struct work_struct *w = timers[i];

            if (time_after_eq(jiffies, w->due)) {
                timers[i] = timers[--ntimers];
                wq_add(w->wq, w);
            } else {
                i++;
            }
        }
        for (struct wq *q = &domain_q; ; q = &freezable_q) {
            while (q->n && !q->frozen) {
                struct work_struct *w = q->items[0];

                memmove(&q->items[0], &q->items[1], (q->n - 1) * sizeof(w));
                q->n--;
                run_one(w);
            }
            if (q == &freezable_q)
                break;
        }
        if (t < ms)
            jiffies++;
    }
}
static void locked_plug(struct tb_port *p)
{
    mutex_lock(&domain_tb.lock);
    tb_dp_resource_available(&domain_tb, p);
    mutex_unlock(&domain_tb.lock);
}
static void locked_unplug(struct tb_port *p)
{
    unsigned int up;

    mutex_lock(&domain_tb.lock);
    up = tb_dp_host_tunnels(&domain_tb, NULL);
    tb_dp_resource_unavailable(&domain_tb, p, "adapter unplug");
    tb_dp_host_tunnel_gone(&domain_tb, up);
    mutex_unlock(&domain_tb.lock);
}
static struct tb_tunnel *tunnel_on(struct tb_port *in)
{
    struct tb_tunnel *t;

    list_for_each_entry(t, &domain_cm.tunnel_list, list)
        if (t->src_port == in)
            return t;
    return NULL;
}
static bool listed(struct tb_port *p)
{
    struct tb_port *q;

    list_for_each_entry(q, &domain_cm.dp_resources, list)
        if (q == p)
            return true;
    return false;
}
/* Both host DP tunnels up at HBR2, and their displays handed the current requests. */
static void both_at_hbr2(void)
{
    struct tb_tunnel *a = tunnel_on(IN0), *b = tunnel_on(IN1);

    CHECK(a && b && a->host_rate == TB_DP_HOST_RATE_SHARED && b->host_rate == TB_DP_HOST_RATE_SHARED);
    CHECK(DP0->tokens.requested == a->host_dp_generation && DP1->tokens.requested == b->host_dp_generation);
}
static void dprx_done(struct tb_tunnel *t) { t->dprx_done = true; }

/*
 * The first display comes alone: A (DP IN 5 to DP OUT 13) at HBR3. Its
 * display is being handed A's DP IN (admitted, not returned yet) when the
 * second display shows up on DP OUT 14: pairing holds A's re-link. The
 * generation admitted for A's display.
 */
static u64 hold_under_activate(void)
{
    struct tb_tunnel *a;
    u64 gen;

    locked_plug(OUT0);
    a = tunnel_on(IN0);
    CHECK(a && a->host_rate == TB_DP_HOST_RATE_ALONE && a->state == TB_TUNNEL_ACTIVATING);
    gen = attempt_begin(DP0);
    CHECK(gen == a->host_dp_generation);
    advance(10);
    locked_plug(OUT1);
    CHECK(tunnel_on(IN0) == a && !tunnel_on(IN1) && domain_cm.dp_relink_held);
    CHECK(logged("re-linking once its display has read its DPRX") == 1);
    /* DCP's Activate meets the binding it was handed */
    CHECK(activate(DP0, gen) == 0);
    return gen;
}

/* Cold plug, then an unplug and a replug: the held pair comes up through the real DPRX work. */
static void case_cold_plug(void)
{
    struct tb_tunnel *a;
    u64 first, gen;

    for (int round = 0; round < 2; round++) {
        loglen = 0;
        logbuf[0] = 0;
        first = hold_under_activate();
        a = tunnel_on(IN0);
        CHECK(!(attempt_end(DP0, first, 0) & APPLE_DPIN_WARN));
        /* Still reading its DPRX: still held, however often pairing runs */
        advance(500);
        mutex_lock(&domain_tb.lock);
        tb_queue_dp_tunnels(&domain_tb);
        mutex_unlock(&domain_tb.lock);
        advance(500);
        CHECK(tunnel_on(IN0) == a && !tunnel_on(IN1));
        CHECK(logged("re-linking once its display has read its DPRX") == 1);
        /* DPRX done: the DPRX work's callback has pairing re-link A and set up B */
        dprx_done(a);
        advance(100);
        both_at_hbr2();
        CHECK(!domain_cm.dp_relink_held);
        CHECK(logged("re-linking at up to 5400 Mb/s per lane (was 8100)") == 1);
        /* The old binding is refused as replaced, the new ones are taken */
        CHECK(activate(DP0, first) == -ESTALE);
        CHECK(logged("display Activate refused: tunnel") && logged(" replaced (requested "));
        gen = display_takes(DP0);
        CHECK(gen == tunnel_on(IN0)->host_dp_generation);
        display_takes(DP1);
        dprx_done(tunnel_on(IN0));
        dprx_done(tunnel_on(IN1));
        advance(100);
        both_at_hbr2();
        /* Unplug the dock's displays */
        locked_unplug(OUT1);
        locked_unplug(OUT0);
        advance(100);
        CHECK(!tunnel_on(IN0) && !tunnel_on(IN1) && !live_tunnels);
        attempt_begin(DP0);
        attempt_begin(DP1);
        CHECK(!DP0->state.alive && !DP1->state.alive);
        /* nothing re-links the lone tunnel up: none is left */
        advance(3000);
    }
}

/*
 * After the hold, the display side refuses A a pipeline: A waits for one,
 * and no display reads its DPRX. The DPRX work, renewing its deadline, has
 * pairing decide again, which retires A's request and re-links it now.
 */
static void case_pipeline_wait(void)
{
    u64 gen;

    gen = hold_under_activate();
    CHECK(attempt_end(DP0, gen, -EBUSY) & APPLE_DPIN_ARM_RETRY);
    advance(1500);
    both_at_hbr2();
    CHECK(!domain_cm.dp_relink_held);
    /* The pipeline retry of the old request starts nothing */
    retry_fires(DP0);
    CHECK(attempt_begin(DP0) != gen);
    CHECK(!logged("not active"));
}

/*
 * A was refused a pipeline before the second display came, and its retry is
 * admitted (DCP may call Activate on it) when the second display shows up:
 * pairing holds the re-link instead of retiring the request under the call.
 * Then the retry is handed the display, or refused again.
 */
static void case_retry_in_flight(bool succeeds)
{
    struct tb_tunnel *a;
    u64 gen;

    locked_plug(OUT0);
    a = tunnel_on(IN0);
    gen = attempt_begin(DP0);
    CHECK(attempt_end(DP0, gen, -EBUSY) & APPLE_DPIN_ARM_RETRY);
    advance(2000);
    CHECK(a->state == TB_TUNNEL_ACTIVATING && a->dprx_timeout > (ktime_t)jiffies + 9000);
    CHECK(retry_fires(DP0));
    CHECK(attempt_begin(DP0) == gen);
    locked_plug(OUT1);
    CHECK(tunnel_on(IN0) == a && !tunnel_on(IN1) && domain_cm.dp_relink_held);
    CHECK(activate(DP0, gen) == 0);
    /* While the attempt runs, the DPRX work does not have pairing ask again */
    advance(2500);
    CHECK(tunnel_on(IN0) == a && !tunnel_on(IN1));
    CHECK(logged("re-linking once its display has read its DPRX") == 1);
    if (succeeds) {
        CHECK(attempt_end(DP0, gen, 0) & APPLE_DPIN_RECOVERED);
        advance(300);
        CHECK(tunnel_on(IN0) == a);
        dprx_done(a);
    } else {
        CHECK(!(attempt_end(DP0, gen, -EBUSY) & APPLE_DPIN_WARN));
    }
    advance(1500);
    both_at_hbr2();
    CHECK(activate(DP0, gen) == -ESTALE);
}

/*
 * A waits for a pipeline with no attempt running when the second display
 * comes: its request is retired now, atomically, and the pipeline retry that
 * was due, run by the DP IN work while pairing tears A down, finds it gone
 * and starts no attempt.
 */
static u64 raced;
static void retry_in_window(void)
{
    if (retry_fires(DP0))
        raced = attempt_begin(DP0);
}
static void case_retire_before_retry(void)
{
    u64 gen;

    locked_plug(OUT0);
    gen = attempt_begin(DP0);
    CHECK(attempt_end(DP0, gen, -EBUSY) & APPLE_DPIN_ARM_RETRY);
    advance(1200);
    teardown_window = retry_in_window;
    locked_plug(OUT1);
    CHECK(!teardown_window);
    CHECK(raced != gen);
    CHECK(!domain_cm.dp_relink_held);
    CHECK(tunnel_on(IN0)->host_dp_generation != gen);
    CHECK(DP0->tokens.requested != gen);
    /* the retry due for gen: no attempt for it */
    retry_fires(DP0);
    CHECK(attempt_begin(DP0) != gen);
    CHECK(DP0->tokens.admitted != gen && DP0->tokens.inflight != gen);
    advance(100);
    both_at_hbr2();
}

/*
 * A's DPRX read, held for, times out. With the second display still there,
 * A is kept for its re-link, which pairing makes from the domain queue once
 * the DPRX work has let A go: A again on its own pair at HBR2 first, then B
 * on the other. With the second display gone, A's DP IN is dropped as
 * before.
 */
static void case_timeout(bool second_stays)
{
    u64 gen = hold_under_activate();

    CHECK(!(attempt_end(DP0, gen, 0) & APPLE_DPIN_WARN));
    if (!second_stays) {
        locked_unplug(OUT1);
        advance(10);
        CHECK(!domain_cm.dp_relink_held);
    }
    advance(13000);
    CHECK(logged("not active"));
    if (second_stays) {
        both_at_hbr2();
        CHECK(tunnel_on(IN0)->dst_port == OUT0 && tunnel_on(IN1)->dst_port == OUT1);
        CHECK(listed(IN0) && listed(IN1));
        CHECK(logged("not active, kept for its re-link") == 1);
        CHECK(logged("another DP tunnel is wanted: re-linking at up to 5400 Mb/s per lane (was 8100)") == 1);
    } else {
        CHECK(!listed(IN0) && !tunnel_on(IN0));
        CHECK(logged("DP IN resource unavailable: DPRX negotiation failed"));
    }
}

/*
 * A's DPRX read ends while hotplug is stopped for system sleep: the pairing
 * request is kept for thaw, which sets up the pair. Once the domain is
 * stopping, nothing is kept and nothing queued.
 */
static void case_frozen(bool stopping)
{
    struct tb_tunnel *a;
    u64 gen = hold_under_activate();

    a = tunnel_on(IN0);
    CHECK(!(attempt_end(DP0, gen, 0) & APPLE_DPIN_WARN));
    mutex_lock(&domain_tb.lock);
    CHECK(!tb_freeze_noirq(&domain_tb));
    domain_cm.stopping = stopping;
    mutex_unlock(&domain_tb.lock);
    dprx_done(a);
    advance(100);
    CHECK(!domain_cm.dp_relink_held && tunnel_on(IN0) == a && !tunnel_on(IN1));
    CHECK(domain_cm.dp_tunnel_pending == !stopping);
    CHECK(!domain_cm.dp_tunnel_work.pending);
    if (stopping)
        return;
    mutex_lock(&domain_tb.lock);
    CHECK(!tb_thaw_noirq(&domain_tb));
    mutex_unlock(&domain_tb.lock);
    advance(100);
    both_at_hbr2();
}

/*
 * The re-link of a lone tunnel to a higher rate: A and B at HBR2, B goes,
 * A is marked; A's display reads its DPRX again (a resume re-activated A)
 * when the debounce ends. The re-link up is held until that is over, or
 * until the display side refuses A a pipeline, and made then.
 */
static void case_relink_up(bool pipeline_wait)
{
    struct tb_tunnel *a;
    u64 gen;

    locked_plug(OUT0);
    a = tunnel_on(IN0);
    display_takes(DP0);
    locked_plug(OUT1);
    CHECK(domain_cm.dp_relink_held);
    dprx_done(a);
    advance(100);
    both_at_hbr2();
    display_takes(DP0);
    display_takes(DP1);
    dprx_done(tunnel_on(IN0));
    dprx_done(tunnel_on(IN1));
    advance(100);
    locked_unplug(OUT1);
    a = tunnel_on(IN0);
    CHECK(a && a->host_relink && !tunnel_on(IN1));
    advance(100);
    attempt_begin(DP1);
    /* tb_resume_noirq() activates A again: its DPRX read runs, its display reads it */
    mutex_lock(&domain_tb.lock);
    a->state = TB_TUNNEL_ACTIVATING;
    a->dprx_done = false;
    a->dprx_started = false;
    CHECK(tb_dp_dprx_start(a) == -EINPROGRESS);
    mutex_unlock(&domain_tb.lock);
    gen = a->host_dp_generation;
    advance(domain_cm.dp_relink_delay + 100);
    CHECK(tunnel_on(IN0) == a && a->host_rate == TB_DP_HOST_RATE_SHARED && domain_cm.dp_relink_up_held);
    CHECK(activate(DP0, gen) == 0);
    if (pipeline_wait) {
        /* The display side drops A and refuses it a pipeline */
        scoped_guard(mutex, &DP0->lock) {
            apple_dpin_token_revoke(&DP0->tokens, gen);
            DP0->state.handed = false;
            DP0->state.waiting = true;
        }
        handed[0] = 0;
    } else {
        dprx_done(a);
    }
    advance(1500);
    a = tunnel_on(IN0);
    CHECK(a && a->host_rate == TB_DP_HOST_RATE_ALONE && a->host_dp_generation != gen);
    CHECK(!domain_cm.dp_relink_up_held);
    CHECK(logged("alone on the host router: re-linking at up to 8100"));
}

/*
 * Any host whose DP IN policy retries for a display pipeline: A comes up
 * alone and is refused a pipeline; its retry is admitted and then waits,
 * past the DPRX deadline, to reach the display (the display driver's
 * provider call can wait on a lock shared by every display). The DPRX
 * deadline is renewed meanwhile: A stays, not active (T6050 keeps a tunnel
 * at its DPRX timeout and would report it active), and its display's
 * Activate meets the binding; DPRX then completes.
 */
static void case_retry_stalled(void)
{
    struct tb_tunnel *a;
    u64 gen;

    locked_plug(OUT0);
    a = tunnel_on(IN0);
    CHECK(a && a->state == TB_TUNNEL_ACTIVATING);
    gen = attempt_begin(DP0);
    CHECK(attempt_end(DP0, gen, -EBUSY) & APPLE_DPIN_ARM_RETRY);
    advance(2000);
    CHECK(retry_fires(DP0));
    CHECK(attempt_begin(DP0) == gen);
    advance(dprx_timeout + 2000);
    CHECK(listed(IN0) && tunnel_on(IN0) == a);
    CHECK(a->state == TB_TUNNEL_ACTIVATING);
    CHECK(DP0->tokens.requested == gen && DP0->tokens.admitted == gen);
    CHECK(activate(DP0, gen) == 0);
    CHECK(attempt_end(DP0, gen, 0) & APPLE_DPIN_RECOVERED);
    dprx_done(a);
    /* the next pass of the DPRX work, at most TB_DPRX_AWAIT_POLL_DELAY on */
    advance(TB_DPRX_AWAIT_POLL_DELAY + 100);
    CHECK(tunnel_on(IN0) == a && a->state == TB_TUNNEL_ACTIVE);
    CHECK(!logged("not active") && !logged("DPRX timeout"));
}

/*
 * A's re-link is held while its display reads the DPRX, and a pairing pass
 * runs with system sleep starting (the PM gate closed, hotplug still on):
 * the re-link is deferred for the sleep instead of held. A's DPRX read
 * then times out before the gate reopens. A is kept, both DP IN adapters
 * stay listed, and nothing is set up again while the gate is closed; once
 * it reopens, A is re-linked on its own pair at HBR2 and B set up on the
 * other. If B goes meanwhile (@second_goes), A is kept while the gate is
 * closed too; once it reopens, A is torn down and its DP IN dropped, as
 * for any DPRX timeout, and A's display is paired on the other DP IN.
 */
static void case_sleep_gate_timeout(bool second_goes)
{
    u64 gen = hold_under_activate(), a_gen = tunnel_on(IN0)->host_dp_generation;

    CHECK(!(attempt_end(DP0, gen, 0) & APPLE_DPIN_WARN));
    advance(dprx_timeout - 1000);
    CHECK(tunnel_on(IN0) && !tunnel_on(IN1));
    pm_sleeping = true;
    mutex_lock(&domain_tb.lock);
    tb_queue_dp_tunnels(&domain_tb);
    mutex_unlock(&domain_tb.lock);
    advance(10);
    CHECK(!domain_cm.dp_relink_held && domain_cm.dp_relink_deferred);
    advance(1500);
    CHECK(logged("not active, kept for its re-link") == 1);
    CHECK(tunnel_on(IN0) && tunnel_on(IN0)->host_dp_generation == a_gen && !tunnel_on(IN1));
    CHECK(listed(IN0) && listed(IN1));
    CHECK(!logged("re-linking at up to"));
    if (second_goes) {
        locked_unplug(OUT1);
        advance(1000);
        /* nothing torn down or set up while the gate is closed */
        CHECK(tunnel_on(IN0) && tunnel_on(IN0)->host_dp_generation == a_gen);
        CHECK(next_generation == a_gen + 1 && listed(IN0) && !tunnel_on(IN1));
        CHECK(!logged("DP IN resource unavailable: DPRX negotiation failed"));
        pm_sleeping = false;
        advance(1000);
        CHECK(!tunnel_on(IN0) && !listed(IN0));
        CHECK(logged("DP IN resource unavailable: DPRX negotiation failed"));
        CHECK(tunnel_on(IN1) && tunnel_on(IN1)->dst_port == OUT0 && live_tunnels == 1);
        return;
    }
    advance(1000);
    CHECK(tunnel_on(IN0) && tunnel_on(IN0)->host_dp_generation == a_gen && !tunnel_on(IN1));
    pm_sleeping = false;
    advance(1000);
    both_at_hbr2();
    CHECK(tunnel_on(IN0)->dst_port == OUT0 && tunnel_on(IN1)->dst_port == OUT1);
    CHECK(logged("another DP tunnel is wanted: re-linking at up to 5400 Mb/s per lane (was 8100)") == 1);
}

/*
 * A's re-link is held; then another DPRX completion (any tunnel's) decides
 * it again while hotplug is frozen for system sleep: the pairing pass is
 * queued for thaw. A's DPRX read times out before thaw: A is kept, both DP
 * IN adapters stay listed, and thaw re-links A and sets up B at HBR2.
 */
static void case_queued_timeout(void)
{
    u64 gen = hold_under_activate(), a_gen = tunnel_on(IN0)->host_dp_generation;

    CHECK(!(attempt_end(DP0, gen, 0) & APPLE_DPIN_WARN));
    mutex_lock(&domain_tb.lock);
    CHECK(!tb_freeze_noirq(&domain_tb));
    tb_dp_relink_unhold(&domain_tb);
    CHECK(!domain_cm.dp_relink_held && domain_cm.dp_tunnel_pending);
    mutex_unlock(&domain_tb.lock);
    advance(dprx_timeout + 1000);
    CHECK(tunnel_on(IN0) && tunnel_on(IN0)->host_dp_generation == a_gen);
    CHECK(listed(IN0) && listed(IN1));
    mutex_lock(&domain_tb.lock);
    CHECK(!tb_thaw_noirq(&domain_tb));
    mutex_unlock(&domain_tb.lock);
    advance(100);
    both_at_hbr2();
    CHECK(tunnel_on(IN0)->dst_port == OUT0 && tunnel_on(IN1)->dst_port == OUT1);
}

/*
 * The re-link of a lone tunnel to a higher rate, held: A on DP IN 6 (to DP
 * OUT 14) and B on DP IN 5 at HBR2; B goes while A's display reads its
 * DPRX, and at the end of the debounce the re-link up is held for it. The
 * tunnel A, generation of its display handoff.
 */
static struct tb_tunnel *relink_up_held(u64 *gen)
{
    struct tb_tunnel *a;

    locked_plug(OUT0);
    display_takes(DP0);
    locked_plug(OUT1);
    CHECK(domain_cm.dp_relink_held);
    dprx_done(tunnel_on(IN0));
    advance(100);
    both_at_hbr2();
    display_takes(DP0);
    dprx_done(tunnel_on(IN0));
    a = tunnel_on(IN1);
    CHECK(a->dst_port == OUT1);
    *gen = display_takes(DP1);
    advance(100);
    locked_unplug(OUT0);
    CHECK(tunnel_on(IN1) == a && a->host_relink && !tunnel_on(IN0));
    attempt_begin(DP0);
    advance(domain_cm.dp_relink_delay + 100);
    CHECK(tunnel_on(IN1) == a && a->host_rate == TB_DP_HOST_RATE_SHARED && domain_cm.dp_relink_up_held);
    return a;
}

/*
 * A, held for its re-link up, times out at HBR2: it is kept, and the
 * re-link work re-links it on the same pair at HBR3 once the DPRX work has
 * let it go, as the re-link up would have: the display keeps its DP IN
 * adapter, and the re-link counts for the debounce of the next one (a
 * second display coming soon after doubles it). If that tunnel does not
 * come up either (@fails), it is set up at HBR2 again, once, as after any
 * re-link up.
 */
static void case_relink_up_timeout(bool fails)
{
    struct tb_tunnel *t;
    u64 gen;

    relink_up_held(&gen);
    advance(dprx_timeout);
    CHECK(listed(IN0) && listed(IN1) && !tunnel_on(IN0));
    t = tunnel_on(IN1);
    CHECK(t && t->dst_port == OUT1 && t->host_rate == TB_DP_HOST_RATE_ALONE && t->host_relinked_up);
    CHECK(logged("not active, kept for its re-link") == 1);
    CHECK(logged("alone on the host router: re-linking at up to 8100 Mb/s per lane (was 5400)") == 1);
    CHECK(activate(DP1, gen) == -ESTALE);
    gen = display_takes(DP1);
    CHECK(gen == t->host_dp_generation);
    if (!fails) {
        dprx_done(t);
        advance(100);
        CHECK(tunnel_on(IN1) == t && t->state == TB_TUNNEL_ACTIVE);
        /* A second display soon after: the re-link back down doubles the debounce */
        locked_plug(OUT0);
        advance(100);
        CHECK(tunnel_on(IN1) && tunnel_on(IN1)->dst_port == OUT1 && tunnel_on(IN0));
        CHECK(domain_cm.dp_relink_delay == 2 * TB_DP_RELINK_DELAY_MS);
        return;
    }
    advance(dprx_timeout + 100);
    CHECK(logged("not active at the higher rate, setting it up again at up to 5400"));
    CHECK(listed(IN0) && listed(IN1));
    t = tunnel_on(IN0) ? tunnel_on(IN0) : tunnel_on(IN1);
    CHECK(t && t->dst_port == OUT1 && t->host_rate == TB_DP_HOST_RATE_SHARED);
}

/*
 * As above, and setting up A again at HBR3 fails at once: it is set up on
 * the same pair at HBR2 at once, as the re-link up does.
 */
static void case_relink_up_setup_fails(void)
{
    struct tb_tunnel *t;
    u64 gen;

    relink_up_held(&gen);
    fail_rate = TB_DP_HOST_RATE_ALONE;
    advance(dprx_timeout);
    t = tunnel_on(IN1);
    CHECK(t && t->dst_port == OUT1 && t->host_rate == TB_DP_HOST_RATE_SHARED && !tunnel_on(IN0));
    CHECK(logged("DP tunnel set up again at up to 5400 Mb/s per lane") == 1);
    fail_rate = 0;
}

/*
 * As above, and system sleep starts (the PM gate closes, hotplug still
 * on) before A times out: nothing is set up again while the gate is
 * closed; once it reopens, A is re-linked on the same pair at HBR3.
 */
static void case_relink_up_sleep_gate(void)
{
    struct tb_tunnel *t;
    u64 gen, a_gen;

    a_gen = relink_up_held(&gen)->host_dp_generation;
    pm_sleeping = true;
    advance(dprx_timeout + 1000);
    CHECK(logged("not active, kept for its re-link") == 1);
    t = tunnel_on(IN1);
    CHECK(t && t->host_dp_generation == a_gen && !tunnel_on(IN0));
    CHECK(!logged("alone on the host router: re-linking"));
    pm_sleeping = false;
    advance(1000);
    t = tunnel_on(IN1);
    CHECK(t && t->dst_port == OUT1 && t->host_rate == TB_DP_HOST_RATE_ALONE && t->host_dp_generation != a_gen);
}

/*
 * As above, with hotplug frozen for system sleep when A times out. Then the
 * dock goes and a router comes back at the same route with the same DP OUT
 * adapter before thaw: that is a new display, paired as any other (DP IN 5
 * first, at HBR3, not a re-link up), not A's re-link.
 */
static void case_relink_up_replaced(void)
{
    struct tb_tunnel *t;
    u64 gen;

    relink_up_held(&gen);
    mutex_lock(&domain_tb.lock);
    CHECK(!tb_freeze_noirq(&domain_tb));
    mutex_unlock(&domain_tb.lock);
    advance(dprx_timeout + 1000);
    CHECK(logged("not active, kept for its re-link") == 1);
    locked_unplug(OUT1);
    CHECK(!tunnel_on(IN1) && !live_tunnels);
    mutex_lock(&domain_tb.lock);
    list_add_tail(&OUT1->list, &domain_cm.dp_resources);
    CHECK(!tb_thaw_noirq(&domain_tb));
    mutex_unlock(&domain_tb.lock);
    advance(100);
    t = tunnel_on(IN0);
    CHECK(t && t->dst_port == OUT1 && !t->host_relinked_up && !tunnel_on(IN1));
}

/*
 * tb_complete() on a host that keeps its routers awake through system sleep
 * pairs DP resources inline, not from the domain queue: here in the window
 * after A's DPRX callback, before its DPRX work drops A and its HopIDs. A
 * timed out with its re-link wanted (to HBR2 with B there, @up: to HBR3,
 * alone) and is kept: that pass leaves A alone, also when A's display has
 * given A up by then and waits for a pipeline for it, and the domain queue
 * then re-links it on its own pair.
 */
static bool window_waits;
static void complete_pass(struct tb_tunnel *tunnel, void *data)
{
    struct tb_cm *tcm = &domain_cm;

    tb_dp_tunnel_active(tunnel, data);
    if (window_waits) {
        scoped_guard(mutex, &DP0->lock) {
            apple_dpin_token_revoke(&DP0->tokens, handed[0]);
            DP0->state.handed = false;
            DP0->state.waiting = true;
        }
        handed[0] = 0;
    }
    mutex_lock(&domain_tb.lock);
    tcm->dp_tunnel_pending = false;
    tb_tunnel_dp(&domain_tb);
    if (tcm->dp_tunnel_pending)
        tb_queue_dp_tunnels(&domain_tb);
    tb_dp_relink_replay(&domain_tb);
    mutex_unlock(&domain_tb.lock);
}
static void case_complete_in_window(bool up)
{
    struct tb_tunnel *a, *t;
    u64 gen;

    if (up) {
        a = relink_up_held(&gen);
    } else {
        gen = hold_under_activate();
        CHECK(!(attempt_end(DP0, gen, 0) & APPLE_DPIN_WARN));
        a = tunnel_on(IN0);
        window_waits = true;
    }
    a->callback = complete_pass;
    advance(dprx_timeout + 1000);
    CHECK(logged("not active, kept for its re-link") == 1);
    if (up) {
        t = tunnel_on(IN1);
        CHECK(t && t->dst_port == OUT1 && t->host_rate == TB_DP_HOST_RATE_ALONE && !tunnel_on(IN0));
        return;
    }
    both_at_hbr2();
    CHECK(tunnel_on(IN0)->dst_port == OUT0 && tunnel_on(IN1)->dst_port == OUT1);
}

/*
 * A setup from DP IN 5 failed once, so A came up alone on DP IN 6 (to DP OUT
 * 13) at HBR3. The second display comes on DP OUT 14 under A's Activate and
 * A's DPRX read then times out: A is re-linked on its own pair, DP IN 6,
 * first, and B gets DP IN 5, as the re-link makes it; the displays do not
 * swap DP IN adapters.
 */
static void case_timeout_keeps_pair(void)
{
    struct tb_tunnel *a;
    u64 gen;

    fail_in = IN0;
    locked_plug(OUT0);
    a = tunnel_on(IN1);
    CHECK(a && a->dst_port == OUT0 && a->host_rate == TB_DP_HOST_RATE_ALONE && !tunnel_on(IN0));
    gen = attempt_begin(DP1);
    CHECK(gen == a->host_dp_generation);
    advance(10);
    locked_plug(OUT1);
    CHECK(tunnel_on(IN1) == a && !tunnel_on(IN0) && domain_cm.dp_relink_held);
    CHECK(activate(DP1, gen) == 0);
    CHECK(!(attempt_end(DP1, gen, 0) & APPLE_DPIN_WARN));
    advance(dprx_timeout + 1000);
    CHECK(tunnel_on(IN1) && tunnel_on(IN1)->dst_port == OUT0 && tunnel_on(IN1)->host_rate == TB_DP_HOST_RATE_SHARED);
    CHECK(tunnel_on(IN0) && tunnel_on(IN0)->dst_port == OUT1 && tunnel_on(IN0)->host_rate == TB_DP_HOST_RATE_SHARED);
}

/*
 * A and B up at HBR2; A's display (DP OUT 13) goes, and B (DP IN 6 to DP
 * OUT 14), left alone, is re-linked at HBR3 once the debounce is over, and
 * its display handed the new tunnel, whose DPRX read runs. B, and the
 * generation of its display handoff.
 */
static struct tb_tunnel *relinked_up(u64 *gen)
{
    struct tb_tunnel *b;

    locked_plug(OUT0);
    display_takes(DP0);
    locked_plug(OUT1);
    CHECK(domain_cm.dp_relink_held);
    dprx_done(tunnel_on(IN0));
    advance(100);
    both_at_hbr2();
    display_takes(DP0);
    display_takes(DP1);
    dprx_done(tunnel_on(IN0));
    dprx_done(tunnel_on(IN1));
    advance(100);
    locked_unplug(OUT0);
    attempt_begin(DP0);
    advance(domain_cm.dp_relink_delay + 100);
    b = tunnel_on(IN1);
    CHECK(b && b->dst_port == OUT1 && b->host_rate == TB_DP_HOST_RATE_ALONE && b->host_relinked_up);
    CHECK(b->state == TB_TUNNEL_ACTIVATING && !tunnel_on(IN0));
    *gen = display_takes(DP1);
    CHECK(*gen == b->host_dp_generation);
    /* the log from here on */
    loglen = 0;
    logbuf[0] = 0;
    return b;
}

/*
 * As relinked_up(), and DP OUT 13 comes back while B's DPRX read at HBR3
 * runs: the re-link back to HBR2 is held for B's display.
 */
static struct tb_tunnel *replug_under_relink_up(u64 *gen)
{
    struct tb_tunnel *b = relinked_up(gen);

    locked_plug(OUT0);
    CHECK(tunnel_on(IN1) == b && !tunnel_on(IN0) && domain_cm.dp_relink_held);
    CHECK(logged("re-linking once its display has read its DPRX") == 1);
    return b;
}

/*
 * B's DPRX read at HBR3, held for, times out with the second display back:
 * the re-link to HBR2 it waited for is made, not the HBR2 fallback of a
 * re-link up that did not come up. B is kept, then re-linked on its own
 * pair at HBR2 and A set up on the other: the displays keep their DP IN
 * adapters. With the PM gate closed after the replug (@gate), nothing is
 * set up until it reopens.
 */
static void case_replug_under_relink_up(bool gate)
{
    struct tb_tunnel *b;
    u64 gen, b_gen;

    b = replug_under_relink_up(&gen);
    b_gen = b->host_dp_generation;
    pm_sleeping = gate;
    advance(dprx_timeout + 1000);
    if (gate) {
        /* no new tunnel generation while the gate is closed */
        CHECK(next_generation == b_gen + 1);
        CHECK(tunnel_on(IN1) && tunnel_on(IN1)->host_dp_generation == b_gen && !tunnel_on(IN0));
        CHECK(listed(IN0) && listed(IN1));
        CHECK(!logged("re-linking at up to 5400"));
        pm_sleeping = false;
        advance(1000);
    }
    both_at_hbr2();
    CHECK(tunnel_on(IN1)->dst_port == OUT1 && tunnel_on(IN0)->dst_port == OUT0);
    CHECK(logged("not active, kept for its re-link") == 1);
    CHECK(!logged("not active at the higher rate"));
    CHECK(logged("another DP tunnel is wanted: re-linking at up to 5400 Mb/s per lane (was 8100)") == 1);
    CHECK(activate(DP1, gen) == -ESTALE);
}

/*
 * As above, with hotplug frozen for system sleep: B times out and is kept.
 * A's display goes again before thaw, so the re-link to HBR2 is not wanted
 * any more: B is set up again at HBR2 as after any re-link up that does not
 * come up, not torn down with its DP IN dropped.
 */
static void case_replug_under_relink_up_gone(void)
{
    struct tb_tunnel *b, *t;
    u64 gen;

    b = replug_under_relink_up(&gen);
    mutex_lock(&domain_tb.lock);
    CHECK(!tb_freeze_noirq(&domain_tb));
    mutex_unlock(&domain_tb.lock);
    advance(dprx_timeout + 1000);
    CHECK(logged("not active, kept for its re-link") == 1);
    CHECK(tunnel_on(IN1) == b && !tunnel_on(IN0));
    locked_unplug(OUT0);
    CHECK(tunnel_on(IN1) == b);
    mutex_lock(&domain_tb.lock);
    CHECK(!tb_thaw_noirq(&domain_tb));
    mutex_unlock(&domain_tb.lock);
    advance(100);
    CHECK(logged("not active at the higher rate, setting it up again at up to 5400") == 1);
    CHECK(!logged("DP IN resource unavailable: DPRX negotiation failed"));
    CHECK(listed(IN0) && listed(IN1));
    /* on its own pair: B's display keeps its DP IN */
    t = tunnel_on(IN1);
    CHECK(t && t->dst_port == OUT1 && t->host_rate == TB_DP_HOST_RATE_SHARED && live_tunnels == 1);
}

/*
 * The HBR2 fallback of B, re-linked up and not come up, is a re-link: with
 * the PM gate closed, B stays kept and listed, and nothing is set up until
 * the gate reopens; then B is set up at HBR2 on its own pair, and its
 * display brings it up. B's DPRX read times out with the gate closed:
 *  - @replug: after DP OUT 13 came back (its re-link down wanted), and
 *    DP OUT 13 goes again before the gate reopens, hotplug on;
 *  - @frozen: as @replug, the timeout while hotplug is frozen for system
 *    sleep, and thaw before the gate reopens;
 *  - neither: B alone, the HBR2 fallback as on main, but not until the gate
 *    reopens.
 */
static void case_fallback_sleep_gate(bool replug, bool frozen)
{
    struct tb_tunnel *b, *t;
    u64 gen, b_gen;

    b = replug ? replug_under_relink_up(&gen) : relinked_up(&gen);
    b_gen = b->host_dp_generation;
    pm_sleeping = true;
    if (frozen) {
        mutex_lock(&domain_tb.lock);
        CHECK(!tb_freeze_noirq(&domain_tb));
        mutex_unlock(&domain_tb.lock);
    }
    advance(dprx_timeout + 1000);
    CHECK(logged("not active, kept for its re-link") == 1);
    CHECK(tunnel_on(IN1) == b && !tunnel_on(IN0));
    if (replug)
        locked_unplug(OUT0);
    if (frozen) {
        mutex_lock(&domain_tb.lock);
        CHECK(!tb_thaw_noirq(&domain_tb));
        mutex_unlock(&domain_tb.lock);
    }
    advance(2000);
    /* no new tunnel generation while the gate is closed */
    CHECK(next_generation == b_gen + 1);
    CHECK(tunnel_on(IN1) == b && b->host_dp_generation == b_gen && !tunnel_on(IN0));
    CHECK(live_tunnels == 1 && listed(IN0) && listed(IN1));
    CHECK(!logged("not active at the higher rate"));
    pm_sleeping = false;
    advance(1000);
    CHECK(logged("not active at the higher rate, setting it up again at up to 5400") == 1);
    t = tunnel_on(IN1);
    CHECK(t && t != b && t->dst_port == OUT1 && t->host_rate == TB_DP_HOST_RATE_SHARED);
    CHECK(!tunnel_on(IN0) && live_tunnels == 1 && listed(IN0) && listed(IN1));
    /* the sink that failed at HBR3 lights at HBR2 */
    CHECK(activate(DP1, gen) == -ESTALE);
    gen = display_takes(DP1);
    CHECK(gen == t->host_dp_generation);
    dprx_done(t);
    advance(100);
    CHECK(tunnel_on(IN1) == t && t->state == TB_TUNNEL_ACTIVE);
}

/*
 * A and B up at HBR2; B goes and A, left alone, is marked for its re-link
 * up. At the end of the debounce the re-link work finds no system sleep
 * transition and queues its run on the domain queue, and one starts right
 * then: the re-link up waits for it, A untouched, and is made once the
 * gate reopens.
 */
static void case_relink_up_gate_race(void)
{
    struct tb_tunnel *a;
    u64 a_gen, next;

    locked_plug(OUT0);
    display_takes(DP0);
    locked_plug(OUT1);
    dprx_done(tunnel_on(IN0));
    advance(100);
    both_at_hbr2();
    display_takes(DP0);
    display_takes(DP1);
    dprx_done(tunnel_on(IN0));
    dprx_done(tunnel_on(IN1));
    advance(100);
    locked_unplug(OUT1);
    a = tunnel_on(IN0);
    CHECK(a && a->host_relink && a->host_rate == TB_DP_HOST_RATE_SHARED);
    a_gen = a->host_dp_generation;
    next = next_generation;
    close_on_queue = &domain_cm.dp_relink_run;
    advance(domain_cm.dp_relink_delay + 2000);
    CHECK(pm_sleeping && !close_on_queue);
    CHECK(tunnel_on(IN0) == a && a->host_rate == TB_DP_HOST_RATE_SHARED && a->host_relink);
    CHECK(next_generation == next && !logged("alone on the host router: re-linking"));
    pm_sleeping = false;
    advance(1000);
    a = tunnel_on(IN0);
    CHECK(a && a->host_rate == TB_DP_HOST_RATE_ALONE && a->host_dp_generation != a_gen);
    CHECK(logged("alone on the host router: re-linking at up to 8100 Mb/s per lane (was 5400)") == 1);
}

/*
 * A, kept at its DPRX timeout while hotplug was frozen for system sleep, is
 * torn down at thaw once the second display went: its DP IN dropped. The
 * gate closes as soon as that teardown is over (a PREPARE that waited for
 * it): the pass that pairs A's display again is part of it, and sets
 * nothing up until the gate reopens; then A's display gets DP IN 6.
 */
static void case_teardown_pairing_gate(void)
{
    u64 gen = hold_under_activate(), a_gen = tunnel_on(IN0)->host_dp_generation;

    CHECK(!(attempt_end(DP0, gen, 0) & APPLE_DPIN_WARN));
    mutex_lock(&domain_tb.lock);
    CHECK(!tb_freeze_noirq(&domain_tb));
    mutex_unlock(&domain_tb.lock);
    advance(dprx_timeout + 1000);
    CHECK(logged("not active, kept for its re-link") == 1);
    locked_unplug(OUT1);
    CHECK(tunnel_on(IN0) && tunnel_on(IN0)->host_dp_generation == a_gen);
    close_on_release = true;
    mutex_lock(&domain_tb.lock);
    CHECK(!tb_thaw_noirq(&domain_tb));
    mutex_unlock(&domain_tb.lock);
    advance(2000);
    CHECK(pm_sleeping && !close_on_release);
    CHECK(logged("DP IN resource unavailable: DPRX negotiation failed"));
    CHECK(!listed(IN0) && !tunnel_on(IN0) && !live_tunnels);
    CHECK(next_generation == a_gen + 1);
    pm_sleeping = false;
    advance(1000);
    CHECK(tunnel_on(IN1) && tunnel_on(IN1)->dst_port == OUT0 && live_tunnels == 1);
}

/*
 * A (DP IN 5 to DP OUT 13) up at HBR3, its DPRX read done; the second
 * display comes and A's re-link to HBR2 cannot set A up again (DP IN 5
 * fails once). The gate closes as soon as that re-link is over: the pass
 * that pairs the host DP IN adapters again is part of the re-link, and
 * sets nothing up until the gate reopens; then both at HBR2, each display
 * on its DP IN.
 */
static void case_retry_pass_gate(void)
{
    struct tb_tunnel *a;
    u64 a_gen;

    locked_plug(OUT0);
    a = tunnel_on(IN0);
    CHECK(a && a->host_rate == TB_DP_HOST_RATE_ALONE);
    display_takes(DP0);
    dprx_done(a);
    advance(100);
    CHECK(a->state == TB_TUNNEL_ACTIVE);
    a_gen = a->host_dp_generation;
    fail_in = IN0;
    close_on_release = true;
    locked_plug(OUT1);
    CHECK(pm_sleeping && !close_on_release && !fail_in);
    CHECK(logged("DP tunnel not set up again, pairing again") == 1);
    advance(2000);
    CHECK(!tunnel_on(IN0) && !tunnel_on(IN1) && !live_tunnels);
    CHECK(next_generation == a_gen + 1 && listed(IN0) && listed(IN1));
    pm_sleeping = false;
    advance(1000);
    both_at_hbr2();
    CHECK(tunnel_on(IN0)->dst_port == OUT0 && tunnel_on(IN1)->dst_port == OUT1);
}

/*
 * A (DP IN 5 to DP OUT 13) up at HBR3, its DPRX read done; the second
 * display comes and A is re-linked at HBR2 on its own pair. The gate
 * closes as soon as that re-link is over: setting up the second display's
 * tunnel, which the re-link made room for, is part of it, and waits for
 * the gate to reopen; then both at HBR2, each display on its DP IN.
 */
static void case_relink_down_pairing_gate(void)
{
    struct tb_tunnel *a;
    u64 a_gen;

    locked_plug(OUT0);
    a = tunnel_on(IN0);
    CHECK(a && a->host_rate == TB_DP_HOST_RATE_ALONE);
    display_takes(DP0);
    dprx_done(a);
    advance(100);
    CHECK(a->state == TB_TUNNEL_ACTIVE);
    a_gen = a->host_dp_generation;
    close_on_release = true;
    locked_plug(OUT1);
    CHECK(pm_sleeping && !close_on_release);
    CHECK(logged("another DP tunnel is wanted: re-linking at up to 5400 Mb/s per lane (was 8100)") == 1);
    advance(2000);
    /* A's replacement only: no tunnel for the second display while the gate is closed */
    a = tunnel_on(IN0);
    CHECK(a && a->dst_port == OUT0 && a->host_rate == TB_DP_HOST_RATE_SHARED);
    CHECK(a->host_dp_generation == a_gen + 1 && next_generation == a_gen + 2);
    CHECK(!tunnel_on(IN1) && listed(IN1) && live_tunnels == 1);
    pm_sleeping = false;
    advance(1000);
    CHECK(tunnel_on(IN0) == a);
    display_takes(DP0);
    display_takes(DP1);
    both_at_hbr2();
    CHECK(tunnel_on(IN0)->dst_port == OUT0 && tunnel_on(IN1)->dst_port == OUT1);
}

/*
 * B, re-linked up to HBR3 and left alone, does not come up: its DPRX read
 * times out and its HBR2 fallback on the same pair cannot set it up (DP IN
 * 6 fails once). The pass that pairs B's display again is part of that
 * re-link and keeps its HBR2 cap until it runs. With the gate closing as
 * soon as the fallback is over (@gate), that pass sets nothing up until the
 * gate reopens, and then sets B's display up at HBR2, as with the gate
 * open; not at the HBR3 rate that just failed.
 */
static void case_fallback_retry(bool gate)
{
    struct tb_tunnel *b, *t;
    u64 gen, b_gen;

    b = relinked_up(&gen);
    b_gen = b->host_dp_generation;
    fail_in = IN1;
    close_on_release = gate;
    advance(dprx_timeout + 1000);
    CHECK(logged("not active, kept for its re-link") == 1);
    CHECK(logged("not active at the higher rate, setting it up again at up to 5400") == 1);
    CHECK(logged("DP tunnel not set up again, pairing again") == 1);
    CHECK(!fail_in && !close_on_release && pm_sleeping == gate);
    if (gate) {
        advance(2000);
        CHECK(!tunnel_on(IN0) && !tunnel_on(IN1) && !live_tunnels);
        CHECK(next_generation == b_gen + 1 && listed(IN0) && listed(IN1));
        pm_sleeping = false;
        advance(1000);
    }
    t = tunnel_on(IN1) ? tunnel_on(IN1) : tunnel_on(IN0);
    CHECK(t && t->dst_port == OUT1 && live_tunnels == 1);
    CHECK(t->host_rate == TB_DP_HOST_RATE_SHARED && t->host_rate_limit == TB_DP_HOST_RATE_SHARED);
    CHECK(!domain_cm.dp_host_hold_shared && !domain_cm.dp_relink_pairing);
    /* the sink that failed at HBR3 lights at HBR2 */
    gen = display_takes(t->src_port == IN1 ? DP1 : DP0);
    CHECK(gen == t->host_dp_generation);
    dprx_done(t);
    advance(100);
    CHECK(t->state == TB_TUNNEL_ACTIVE);
}

static const struct apple_dpin_policy *policy_for(const char *c)
{
    static const struct { const char *name; const struct apple_dpin_policy *p; } p[] = {
        { "retry-stalled-m1", &apple_dpin_m1 }, { "retry-stalled-m3", &apple_dpin_m3 },
        { "retry-stalled-m4", &apple_dpin_m4 }, { "retry-stalled-m5", &apple_dpin_m5 },
    };

    for (unsigned int i = 0; i < sizeof(p) / sizeof(p[0]); i++)
        if (!strcmp(c, p[i].name))
            return p[i].p;
    return &apple_dpin_m4;
}

int main(int argc, char **argv)
{
    const char *c;

    CHECK(argc == 2);
    c = argv[1];
    setup(policy_for(c));
    if (!strncmp(c, "retry-stalled-", 14))
        case_retry_stalled();
    else if (!strcmp(c, "sleep-gate-timeout"))
        case_sleep_gate_timeout(false);
    else if (!strcmp(c, "sleep-gate-timeout-second-gone"))
        case_sleep_gate_timeout(true);
    else if (!strcmp(c, "relink-up-setup-fails"))
        case_relink_up_setup_fails();
    else if (!strcmp(c, "relink-up-sleep-gate"))
        case_relink_up_sleep_gate();
    else if (!strcmp(c, "relink-up-replaced"))
        case_relink_up_replaced();
    else if (!strcmp(c, "complete-in-window"))
        case_complete_in_window(false);
    else if (!strcmp(c, "relink-up-complete-in-window"))
        case_complete_in_window(true);
    else if (!strcmp(c, "timeout-keeps-pair"))
        case_timeout_keeps_pair();
    else if (!strcmp(c, "replug-under-relink-up"))
        case_replug_under_relink_up(false);
    else if (!strcmp(c, "replug-under-relink-up-sleep-gate"))
        case_replug_under_relink_up(true);
    else if (!strcmp(c, "replug-under-relink-up-gone"))
        case_replug_under_relink_up_gone();
    else if (!strcmp(c, "queued-timeout"))
        case_queued_timeout();
    else if (!strcmp(c, "relink-up-timeout"))
        case_relink_up_timeout(false);
    else if (!strcmp(c, "relink-up-timeout-fallback"))
        case_relink_up_timeout(true);
    else if (!strcmp(c, "cold-plug-replug"))
        case_cold_plug();
    else if (!strcmp(c, "pipeline-wait"))
        case_pipeline_wait();
    else if (!strcmp(c, "retry-in-flight-handed"))
        case_retry_in_flight(true);
    else if (!strcmp(c, "retry-in-flight-refused"))
        case_retry_in_flight(false);
    else if (!strcmp(c, "retire-before-retry"))
        case_retire_before_retry();
    else if (!strcmp(c, "timeout-second-stays"))
        case_timeout(true);
    else if (!strcmp(c, "timeout-second-gone"))
        case_timeout(false);
    else if (!strcmp(c, "frozen-then-thaw"))
        case_frozen(false);
    else if (!strcmp(c, "stopping"))
        case_frozen(true);
    else if (!strcmp(c, "fallback-sleep-gate"))
        case_fallback_sleep_gate(false, false);
    else if (!strcmp(c, "replug-under-relink-up-gone-sleep-gate"))
        case_fallback_sleep_gate(true, false);
    else if (!strcmp(c, "replug-under-relink-up-gone-thaw-gate"))
        case_fallback_sleep_gate(true, true);
    else if (!strcmp(c, "teardown-pairing-gate"))
        case_teardown_pairing_gate();
    else if (!strcmp(c, "retry-pass-gate"))
        case_retry_pass_gate();
    else if (!strcmp(c, "relink-up-gate-race"))
        case_relink_up_gate_race();
    else if (!strcmp(c, "relink-down-pairing-gate"))
        case_relink_down_pairing_gate();
    else if (!strcmp(c, "fallback-retry"))
        case_fallback_retry(false);
    else if (!strcmp(c, "fallback-retry-gate"))
        case_fallback_retry(true);
    else if (!strcmp(c, "relink-up-dprx-done"))
        case_relink_up(false);
    else if (!strcmp(c, "relink-up-pipeline-wait"))
        case_relink_up(true);
    else
        return 78;
    printf("ok %s\n", c);
    return 0;
}
'''

CASES = ['cold-plug-replug', 'pipeline-wait', 'retry-in-flight-handed', 'retry-in-flight-refused',
         'retire-before-retry', 'timeout-second-stays', 'timeout-second-gone', 'frozen-then-thaw',
         'stopping', 'relink-up-dprx-done', 'relink-up-pipeline-wait',
         'retry-stalled-m1', 'retry-stalled-m3', 'retry-stalled-m4', 'retry-stalled-m5',
         'sleep-gate-timeout', 'queued-timeout', 'relink-up-timeout', 'relink-up-timeout-fallback',
         'sleep-gate-timeout-second-gone', 'relink-up-setup-fails', 'relink-up-sleep-gate', 'relink-up-replaced',
         'complete-in-window', 'relink-up-complete-in-window', 'timeout-keeps-pair',
         'replug-under-relink-up', 'replug-under-relink-up-sleep-gate', 'replug-under-relink-up-gone',
         'replug-under-relink-up-gone-sleep-gate', 'replug-under-relink-up-gone-thaw-gate',
         'fallback-sleep-gate', 'teardown-pairing-gate', 'retry-pass-gate', 'relink-up-gate-race',
         'relink-down-pairing-gate', 'fallback-retry', 'fallback-retry-gate']
POLICIES = ['apple_dpin_m1', 'apple_dpin_m3', 'apple_dpin_m4', 'apple_dpin_m5']


def build(tmp, name, sources):
    unit = PRE
    for policy_name in POLICIES:
        unit += policy(sources['dpin'], policy_name)
    for src, fn in FUNCTIONS:
        unit += function(sources[src], fn)
    unit += POST
    c = Path(tmp) / (name + '.c')
    binary = Path(tmp) / name
    c.write_text(unit)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-sign-compare',
                    '-Wno-missing-field-initializers',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I', str(Path(tmp) / 'include'), '-I', str(Path(tmp) / 'overlay'),
                    str(c), '-o', str(binary)], check=True)
    return {case: subprocess.run([str(binary), case], capture_output=True, text=True) for case in CASES}


def main():
    sources = {k: (TBDIR / v).read_text() for k, v in FILES.items()}
    with tempfile.TemporaryDirectory(prefix='tb-dp-relink-hold-') as tmp:
        inc = Path(tmp) / 'include' / 'linux'
        inc.mkdir(parents=True)
        for name, text in FAKE_LINUX.items():
            (inc / name).write_text(text)

        def overlay(srcs):
            ov = Path(tmp) / 'overlay'
            ov.mkdir(exist_ok=True)
            for header in ('apple-dpin-state.h', 'apple-dpin-request.h'):
                key = 'request' if header == 'apple-dpin-request.h' else None
                text = srcs[key] if key else (TBDIR / header).read_text()
                (ov / header).write_text(text)

        overlay(sources)
        runs = build(tmp, 'production', sources)
        failed = {c: r for c, r in runs.items() if r.returncode}
        for case, run in runs.items():
            sys.stdout.write(run.stdout)
        if failed:
            for case, run in failed.items():
                sys.stderr.write('FAIL %s (%d)\n%s%s' % (case, run.returncode, run.stdout, run.stderr))
            raise SystemExit(1)
        for name, (src, old, new) in MUTATIONS.items():
            assert sources[src].count(old) == 1, 'stale mutation: ' + name
            mutated = dict(sources)
            mutated[src] = sources[src].replace(old, new)
            overlay(mutated)
            runs = build(tmp, name, mutated)
            caught = sorted(c for c, r in runs.items() if r.returncode == 77)
            other = sorted(c for c, r in runs.items() if r.returncode not in (0, 77))
            assert caught and not other, 'mutation survived or crashed: %s %s' % (name, other)
            print('PASS negative control %s: fails %s' % (name, ', '.join(caught)))
        print('PASS %d cases: the shared-rate re-link waits for the Activate, and the held pair comes up' %
              len(CASES))


if __name__ == '__main__':
    main()
