#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Replay a firmware-27.2 T8132 dcpext's deferred tunnel connect and its release.

The production bodies of the Type-C reconnect work, the DPTX connect, its
session and admission checks, the wait for an earlier release's late answers,
the Type-C detach and the display release are compiled against host mocks of
the DPTX calls, the firmware's link configuration, the Thunderbolt binding and
the AFK transport. The replay drives a dock's tunnel on the Air (J713) on
firmware 27.2:

- the tunnel's provider revokes its binding before the work starts, between
  two DPTX calls, right after the connect's own admission check, and during
  the link wait: no DPTX call goes out for a revoked binding, as each is
  queued under the provider's admission (dcp_dptx_send_gated()), HPD is
  never raised for it, the connect does not report success for it, and the
  detach does not wait out the link wait;
- the CRTC is turned off (DPMS) before the work starts, between two calls and
  during the link wait: the connect never raises HPD on a CRTC turned off;
- a T8132 dcpext connects once per tunnel: a failed connect is not tried
  again blindly;
- a release the firmware does not answer for a display whose tunnel is gone
  gives the route back, and the DPTX port is held until the firmware answers:
  a connect meanwhile does not use it, and does not wait for it either. The
  answer frees the port and connects the display attached meanwhile, whenever
  it comes (dcp_t8132_port_work()); an answer that refuses the release has it
  sent again; a release never answered keeps the port held. The link reports
  the firmware makes for the old release meanwhile are not acted on. A
  release not sent (-ENOSPC), refused, or timed out on a live link, or on a
  link without a tunnel (a fixed HDMI output), fails as before;
- a connect that fails once its display is requested releases it as a detach
  does (dcp_dptx_release_locked()): a revocation after the request with a
  release that times out holds the port, and a release refused is owed to
  the next release or connect. Only the firmware's answer frees a held port:
  no connect clears it;
- the DPTX service's publication, or the tunnel's repeated request, connects
  a display attached while its connect could not run;
- the M4 backend (26.6.2) still waits for its unanswered compact calls,
  until a revocation or the CRTC's power-off, which wakes the wait.

Each mutation puts one piece back as it was, or drops it; the replay must then
fail.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DCP = (ROOT / 'drivers/gpu/drm/apple/dcp.c').read_text()
FABRIC = (ROOT / 'drivers/gpu/drm/apple/dcp-fabric.c').read_text()
SESSION = (ROOT / 'drivers/gpu/drm/apple/dcp-fabric-session.h').read_text()
SOURCES = {'dcp': DCP, 'fabric': FABRIC, 'session': SESSION}
FUNCTIONS = [
    ('session', 'dcp_fabric_session_valid'),
    ('fabric', 'dcp_t8132_ext_m4_node'), ('fabric', 'dcp_is_t8132_ext'),
    ('fabric', 'dcp_t8132_ext_hw'), ('fabric', 'dcp_t8132_late_release'),
    ('fabric', 'dcp_t8132_link_report_late'),
    ('dcp', 'dcp_session_locked'), ('dcp', 'dcp_session_valid_locked'),
    ('dcp', 'dcp_connect_session_valid_locked'), ('dcp', 'dcp_connect_session_valid'),
    ('dcp', 'dcp_typec_sessionless'), ('dcp', 'dcp_queue_typec_reconnect_locked'),
    ('dcp', 'dcp_queue_typec_reconnect'), ('dcp', 'dcp_t8132_wait_teardown_answers'),
    ('dcp', 'dcp_t8132_fw27'), ('dcp', 'dcp_t8132_port_held_locked'),
    ('dcp', 'dcp_t8132_hold_locked'), ('dcp', 'dcp_t8132_refusals'),
    ('dcp', 'dcp_tb_tunnel_withdrawn'), ('dcp', 'dcp_connect_admitted_locked'),
    ('dcp', 'dcp_connect_admitted'), ('dcp', 'dcp_dptx_gate_open'),
    ('dcp', 'dcp_dptx_gate_arm'), ('dcp', 'dcp_dptx_gate_disarm'),
    ('dcp', 'dcp_dptx_gate_close'), ('dcp', 'dcp_dptx_send_gated'),
    ('dcp', 'dcp_watches_tunnel_withdrawal'), ('dcp', 'dcp_dptx_wait_linkcfg'),
    ('dcp', 'dcp_dptx_connect_raw'), ('dcp', 'dcp_typec_reconnect_work'),
    ('dcp', 'dcp_typec_attach_locked'), ('dcp', 'dcp_typec_attach_deferred'),
    ('dcp', 'dcp_t8132_tunnel_gone'), ('dcp', 'dcp_dptx_release_locked'),
    ('dcp', 'dcp_dptx_disconnect'), ('dcp', 'dcp_dptx_disconnect_oob'),
    ('dcp', 'dcp_typec_crtc_turned_off'), ('dcp', 'dcp_typec_resume_locked'),
    ('dcp', 'dcp_t8132_release_answered_locked'), ('dcp', 'dcp_t8132_port_work'),
    ('dcp', 'dcp_typec_attach_repeated'),
]

# name -> (source, old, new)
MUTATIONS = {
    # The connect checks the session alone, not the provider's admission.
    'no-admission': ('dcp', 'return !session || !session->cookie || !dcp_tb_tunnel_withdrawn(dcp, 0);',
                     'return true;'),
    # The calls go out after a check, not under the provider's admission.
    'no-gate': ('dcp', '\tif (ep != dcp->dptxep || READ_ONCE(dcp->dptx_gate_task) != current)\n\t\treturn enqueue(arg);',
                '\treturn enqueue(arg);'),
    # The connect reports success for a binding revoked during the link wait.
    'no-recheck-after-wait': ('dcp', '\tif (!dcp_connect_admitted(dcp, session, cable, recovery))\n\t\treturn -ESTALE;\n\tif (dcp->external) {',
                              '\tif (!dcp_connect_session_valid(dcp, session, cable, recovery))\n\t\treturn -ESTALE;\n\tif (dcp->external) {'),
    # The detach leaves the link wait to run out.
    'detach-no-link-wake': ('dcp', '\t\tif (dcp->dptx_tunnel)\n\t\t\tcomplete(&dcp->dptxport[0].linkcfg_completion);\n',
                            ''),
    # The deferred connect may wake a CRTC turned off.
    'deferred-wakes-off-crtc': ('dcp', '\t */\n\tdcp->typec_reconnect_recovery = true;\n\tdcp_queue_typec_reconnect_locked(dcp, 0);',
                                '\t */\n\tdcp_queue_typec_reconnect_locked(dcp, 0);'),
    # Blind retries of a failed firmware-27 connect.
    'blind-retries': ('dcp', '(dcp_uses_t6020_tunnel_flow(dcp) || dcp_t8132_ext_hw(dcp) ?',
                      '(dcp_uses_t6020_tunnel_flow(dcp) || dcp_is_t8132_ext(dcp) ?'),
    # Any release error on firmware 27 taken as done, as before.
    'any-error-released': ('dcp', '} else if (ret == -ETIMEDOUT && dcp_t8132_tunnel_gone(dcp, port)) {',
                           '} else if (ret && dcp_t8132_ext_hw(dcp)) {'),
    # A live link's timed-out release taken as done.
    'live-link-released': ('dcp', '\treturn dcp_t8132_ext_hw(dcp) && !dcp_is_t8132_ext(dcp) && dcp->dptx_tunnel &&\n'
                           '\t       dcp_tb_tunnel_withdrawn(dcp, port);',
                           '\treturn dcp_t8132_ext_hw(dcp) && !dcp_is_t8132_ext(dcp);'),
    # The next connect reuses the port before the firmware answered.
    'no-fence': ('dcp', '\t} else if (dcp_t8132_port_held_locked(dcp)) {', '\t} else if (false) {'),
    # The M4 backend's wait is not ended by a revoked session.
    'fence-ignores-revocation': ('dcp', '\t\t\t   READ_ONCE(dcp->typec_generation) != generation ||\n', ''),
    # The hold is given up without the firmware's answer.
    'fence-gives-up': ('dcp', '\t\tif (!dcp_t8132_release_answered_locked(dcp))\n\t\t\treturn;\n', ''),
    # A connect clears the hold, as before (outside hpd_mutex, unanswered).
    'connect-clears-hold': ('dcp', '\tif (t8132 && dcp->dptxep && afk_compact_orphans_pending(dcp->dptxep)) {\n'
                            '\t\tret = dcp_t8132_wait_teardown_answers(',
                            '\tWRITE_ONCE(dcp->t8132_release_late, false);\n'
                            '\tif (t8132 && dcp->dptxep && afk_compact_orphans_pending(dcp->dptxep)) {\n'
                            '\t\tret = dcp_t8132_wait_teardown_answers('),
    # A failed connect releases its display ignoring the result, as before.
    'connect-release-ignored': ('dcp', '\tdcp_dptx_gate_disarm(dcp);\n\terr = dcp_dptx_release_locked(dcp, port);\n',
                                '\tdcp_dptx_gate_disarm(dcp);\n\tdptxport_release_display(dcp->dptxport[port].service);\n'
                                '\tdcp->dptxport[port].connected = false;\n\terr = 0;\n'),
    # A release not sent or refused is dropped instead of owed.
    'owed-dropped': ('dcp', '\t    (dcp->dptxport[port].connected || dcp->dptxport[port].release_owed)) {',
                     '\t    dcp->dptxport[port].connected) {'),
    # An answer that refused the release taken as the release done.
    'refusal-taken-as-release': ('dcp', '\tif (afk_late_refusals(ep) == dcp->t8132_release_refusals)\n\t\treturn true;',
                                 '\treturn true;'),
    # The answer frees the port, but nothing connects the display attached meanwhile.
    'no-resume': ('dcp', '\t\tdev_info(dcp->dev, "DPTX port 0: the earlier release is answered: the port is free\\n");\n\t}\n\tdcp_typec_resume_locked(dcp);',
                  '\t\tdev_info(dcp->dev, "DPTX port 0: the earlier release is answered: the port is free\\n");\n\t}'),
    # The resume wakes a CRTC turned off.
    'resume-wakes-off-crtc': ('dcp', '\t    !dcp->typec_cable_connected || READ_ONCE(dcp->typec_crtc_off) ||',
                              '\t    !dcp->typec_cable_connected ||'),
    # Neither the service's publication nor a repeated request resumes a display.
    'repeat-no-resume': ('dcp', '\tif (dcp_t8132_fw27(dcp))\n\t\tschedule_work(&dcp->t8132_port_work);', '\t;'),
    # DPMS does not wake the M4 backend's wait.
    'dpms-no-wake': ('dcp', '\tif (dcp->dptxep)\n\t\tafk_late_replies_wake(dcp->dptxep);\n}\n\n/*\n * Firmware 27: a T8132 dcpext\'s display attached',
                     '}\n\n/*\n * Firmware 27: a T8132 dcpext\'s display attached'),
    # The late link reports of the old release reach the new connect.
    'late-reports-acted-on': ('fabric', '\treturn dcp_t8132_ext_hw(dcp) && dcp_t8132_late_release(dcp, what);',
                              '\treturn false;'),
}


def function(text, name):
    match = re.search(r'^(?:VISIBLE_IF_KUNIT\s+)?(?:static\s+)?(?:inline\s+)?'
                      r'(?:bool|int|void|unsigned long|u64|struct dcp_fabric_session)\s+' + re.escape(name) +
                      r'\([^;{]*?\)\n\{', text, re.M | re.S)
    assert match, name
    pos = text.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return re.sub(r'^VISIBLE_IF_KUNIT\s+', '', text[match.start():pos]) + '\n'


def define(text, name):
    match = re.search(r'^#define ' + re.escape(name) + r'\b.*\n', text, re.M)
    assert match, name
    return match.group(0)


PRE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define smp_store_release(p, v) (*(p) = (v))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
static unsigned int infos, warns, errs;
#define dev_info(dev, ...) ((void)(dev), infos++)
#define dev_warn(dev, ...) ((void)(dev), warns++)
#define dev_err(dev, ...) ((void)(dev), errs++)
#define dev_dbg(dev, ...) ((void)(dev))
#define jiffies_to_msecs(j) ((unsigned int)(j))
static unsigned long jiffies;
static unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
static void usleep_range(unsigned long a, unsigned long b) { (void)a; (void)b; }
struct task_struct { int unused; };
static struct task_struct the_task;
#define current (&the_task)

/* Locks: held counts, checked where production asserts them. */
struct mutex { int held; };
/* What another thread waiting for a mutex does once it is released. */
static struct mutex *waited_lock;
static void (*waiter)(void);
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = 1; }
static void mutex_unlock(struct mutex *m)
{
	assert(m->held);
	m->held = 0;
	if (m == waited_lock && waiter) {
		void (*w)(void) = waiter;

		waiter = NULL;
		w();
	}
}
typedef struct mutex *mutex_ref;
static void mutex_ref_release(mutex_ref *m) { mutex_unlock(*m); }
static mutex_ref mutex_ref_take(struct mutex *m) { mutex_lock(m); return m; }
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define guard(kind) mutex_ref __attribute__((cleanup(mutex_ref_release))) CAT(guard_, __COUNTER__) = mutex_ref_take
#define scoped_guard(kind, lock) \
	for (bool CAT(once_, __LINE__) = true; CAT(once_, __LINE__);) \
		for (mutex_ref CAT(sg_, __LINE__) __attribute__((cleanup(mutex_ref_release))) = \
		     scoped_take_##kind(lock); CAT(once_, __LINE__); CAT(once_, __LINE__) = false)
#define scoped_take_mutex(lock) mutex_ref_take(lock)
static struct mutex spin_dummy;
#define scoped_take_spinlock_irqsave(lock) ((void)(lock), mutex_ref_take(&spin_dummy))
#define lockdep_assert_held(l) assert((l)->held)

struct completion { int done; };
static void reinit_completion(struct completion *c) { c->done = 0; }
static void complete(struct completion *c) { c->done++; }
/* Events at a wait, as another thread would make them meanwhile. */
static void (*at_link_wait)(void);
static void (*at_fence_wait)(void);
static unsigned int link_waits;
static unsigned long link_wait_spent;
static unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout)
{
	link_waits++;
	if (at_link_wait) {
		void (*hook)(void) = at_link_wait;

		at_link_wait = NULL;
		hook();
	}
	if (c->done) {
		c->done--;
		return timeout;
	}
	jiffies += timeout;
	link_wait_spent += timeout;
	return 0;
}
static unsigned long fence_wait_spent;
/* Wakes of the late-answer waiters (afk_late_replies_wake(), an answer). */
static unsigned int late_wakes;
/* A wait sees a change only if it is woken for it, or at its end. */
#define wait_event_timeout(wq, cond, timeout) ({ \
	unsigned long __t = (timeout); \
	unsigned int __w = late_wakes; \
	(void)(wq); \
	(cond) ? 1 : ({ \
		if (at_fence_wait) { void (*h)(void) = at_fence_wait; at_fence_wait = NULL; h(); } \
		(late_wakes != __w && (cond)) ? 1 : \
		(jiffies += __t, fence_wait_spent += __t, (cond) ? 1 : 0); }); })

struct work_struct { int queued; };
static void schedule_work(struct work_struct *w) { w->queued++; }
struct delayed_work { struct work_struct work; bool pending; unsigned int queued; };
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
static void *system_freezable_wq;
static bool delayed_work_pending(struct delayed_work *w) { return w->pending; }
static bool mod_delayed_work(void *wq, struct delayed_work *w, unsigned long delay)
{
	(void)wq; (void)delay; w->pending = true; w->queued++; return false;
}
static bool cancel_delayed_work(struct delayed_work *w) { bool was = w->pending; w->pending = false; return was; }
static bool cancel_delayed_work_sync(struct delayed_work *w) { return cancel_delayed_work(w); }

/* Display */
enum { DCP_FIRMWARE_V_26_6 = 26, DCP_FIRMWARE_V_27_0 = 27, DCP_FIRMWARE_H17P = 17,
       DCP_FIRMWARE_V_26_6_DESKTOP = 260, DCP_FIRMWARE_V_12_3 = 12, DCP_FIRMWARE_V_13_5 = 13 };
enum { DRM_MODE_CONNECTOR_DisplayPort = 10, DRM_MODE_CONNECTOR_USB = 32 };
struct device_node { const char *compatible; bool v27_2; };
struct device { struct device_node *of_node; };
struct platform_device { struct device dev; };
struct apple_dcp;
struct apple_dcp_afkep { int compact_orphan_wq; bool compact; struct apple_dcp *dcp; };
struct apple_epic_service { struct apple_dcp_afkep *ep; };
struct dcp_fabric_session { u64 generation; u64 cookie; };
struct dptx_port {
	struct apple_epic_service *service;
	bool connected, release_owed, closed, tile_hint;
	void *atcphy;
	struct completion linkcfg_completion;
};
struct apple_connector { int unused; };
struct apple_dcp {
	struct device *dev;
	int fw_compat;
	struct mutex hpd_mutex, tb_lock, dptx_gate_lock;
	struct task_struct *dptx_gate_task;
	u64 dptx_gate_cookie;
	void *phy, *avep, *relink_lock, *relink;
	struct { bool ext_pipe_off; } *desktop;
	struct apple_connector *connector;
	bool external, external_native, dptx_tunnel, typec_cable_connected, typec_crtc_off,
	     typec_follow_retiring, typec_work_enabled, crashed, t8132_release_late,
	     external_link_ready, external_error, placeholder_retried, main_display, hdmi_hpd;
	int connector_type, external_retries;
	struct delayed_work external_retry_wq;
	bool sleep_v27_torn;
	u64 typec_generation, tb_generation, typec_unplugs;
	u32 dptx_die, dptx_phy, dptx_dfp_port;
	unsigned int typec_reconnect_tries;
	bool typec_reconnect_recovery;
	struct dcp_fabric_session typec_reconnect_session;
	struct delayed_work typec_reconnect_wq, placeholder_edid_wq;
	u64 t8132_release_refusals;
	struct work_struct t8132_port_work;
	struct completion typec_iomfb_hpd_ready;
	void *tb_dpin_ctx;
	struct { void *binding; bool active; } split;
	struct apple_dcp_afkep *dptxep;
	struct dptx_port dptxport[2];
};
static bool of_device_is_compatible(const struct device_node *n, const char *c)
{
	return n && !strcmp(n->compatible, c);
}
static bool apple_t8132_dcpext_v27_2(const struct device_node *n)
{
	return of_device_is_compatible(n, "apple,t8132-dcpext") && n->v27_2;
}
static bool of_machine_is_compatible(const char *c) { (void)c; return false; }
static struct platform_device *to_platform_device(struct device *d)
{
	return container_of(d, struct platform_device, dev);
}
static void *platform_get_drvdata(struct platform_device *p);
static int dcp_dptx_release_locked(struct apple_dcp *dcp, u32 port);

/* The DPTX calls, in order, and the firmware behind them. */
static char rpcs[64][24];
static unsigned int nrpcs;
static int release_ret, set_hpd_low_ret, set_hpd_high_ret;
static bool hpd_high;
static void (*at_rpc)(const char *what);
/* Whether the firmware configures the link once HPD is raised. */
static bool link_comes;
static struct apple_dcp *the_dcp;
static void rpc(const char *what)
{
	assert(nrpcs < 64);
	snprintf(rpcs[nrpcs++], sizeof(rpcs[0]), "%s", what);
	if (at_rpc)
		at_rpc(what);
}
static bool rpc_seen(const char *what)
{
	for (unsigned int i = 0; i < nrpcs; i++)
		if (!strcmp(rpcs[i], what))
			return true;
	return false;
}
/*
 * A DPTX call is queued through the V2 transport (afk_send_command_common()),
 * which hands its queueing to the gate (dcp_dptx_send_gated()); the reply
 * follows the queueing.
 */
int dcp_dptx_send_gated(struct apple_dcp_afkep *ep, int (*enqueue)(void *arg), void *arg);
static struct apple_dcp_afkep dptxep;
static int queue_rpc(void *what) { rpc(what); return 0; }
static int send(const char *what)
{
	unsigned int before = nrpcs;
	int ret = dcp_dptx_send_gated(&dptxep, queue_rpc, (void *)what);

	/* Refused by the gate: nothing went out. */
	assert(!ret || nrpcs == before);
	return ret;
}
static int dptxport_validate_connection(struct apple_epic_service *s, u8 a, u8 b, u8 c)
{ (void)s; (void)a; (void)b; (void)c; return send("validate"); }
static int dptxport_connect(struct apple_epic_service *s, u8 a, u8 b, u8 c, bool d)
{ (void)s; (void)a; (void)b; (void)c; (void)d; return send("connect"); }
static int dptxport_request_display(struct apple_epic_service *s) { (void)s; return send("request"); }
static int dptxport_set_hpd(struct apple_epic_service *s, bool on)
{
	int ret;

	(void)s;
	ret = send(on ? "hpd1" : "hpd0");
	if (ret)
		return ret;
	if (on && set_hpd_high_ret)
		return set_hpd_high_ret;
	if (!on && set_hpd_low_ret)
		return set_hpd_low_ret;
	hpd_high = on;
	if (on && link_comes)
		complete(&the_dcp->dptxport[0].linkcfg_completion);
	return 0;
}
static int dptxport_set_hpd_timeout(struct apple_epic_service *s, bool on, unsigned int ms)
{ (void)ms; return dptxport_set_hpd(s, on); }
static int dptxport_release_display(struct apple_epic_service *s)
{
	int ret;

	(void)s;
	ret = send("release");
	/* A release always goes out: it is never gated. */
	assert(!ret);
	return release_ret;
}
static bool dptx_service_available(struct dptx_port *p) { return p->service; }

/* The provider's binding, and its admission. */
static bool binding_live = true;
/* The provider revokes the binding right after the n-th check of it, under its own lock. */
static unsigned int revoke_after_checks;
static bool apple_dp_tunnel_binding_live(void *binding)
{
	bool live = binding_live;

	(void)binding;
	if (revoke_after_checks && !--revoke_after_checks)
		binding_live = false;
	return live;
}
static unsigned int gated_calls;
static int apple_dp_tunnel_binding_call(void *binding, int (*call)(void *arg), void *arg)
{
	(void)binding;
	if (!binding_live)
		return -ESTALE;
	gated_calls++;
	return call(arg);
}
#define symbol_get(sym) (&sym)
#define symbol_put(sym) ((void)0)

/* AFK: late answers of calls that timed out, and the refusals among them. */
static bool late_pending, compact_orphans;
static unsigned int orphans_dropped;
static u64 late_refusals;
static bool afk_late_replies_pending(struct apple_dcp_afkep *ep) { (void)ep; return late_pending || compact_orphans; }
static u64 afk_late_refusals(struct apple_dcp_afkep *ep) { (void)ep; return late_refusals; }
static bool afk_compact_orphans_pending(struct apple_dcp_afkep *ep) { (void)ep; return compact_orphans; }
static unsigned int afk_compact_orphans_drop(struct apple_dcp_afkep *ep)
{ (void)ep; compact_orphans = false; return ++orphans_dropped; }
static void afk_late_replies_wake(struct apple_dcp_afkep *ep) { (void)ep; late_wakes++; }

/* The rest of the pipeline, as a firmware-27 dcpext of the DRM device has it. */
static bool dcp_is_typec_output(struct apple_dcp *d) { (void)d; return true; }
static bool dcp_is_usb4_output(struct apple_dcp *d) { return d->dptx_tunnel; }
static bool dcp_ext_dptx_607(struct apple_dcp *d) { (void)d; return false; }
static bool dcp_uses_t6020_tunnel_flow(struct apple_dcp *d) { (void)d; return false; }
static bool iomfb_v14_7_external_ready(struct apple_dcp *d) { (void)d; return true; }
static int iomfb_v14_7_external_open(struct apple_dcp *d) { (void)d; return 0; }
static bool dcpext_scanout_terminal(struct apple_dcp *d) { (void)d; return false; }
static void dcpext_scanout_link_restored(struct apple_dcp *d) { (void)d; }
static void dcpext_scanout_invalidate(struct apple_dcp *d) { (void)d; }
static int dcp_typec_phy_on(struct apple_dcp *d) { (void)d; return 0; }
static bool dcp_is_ext26(struct apple_dcp *d) { (void)d; return false; }
static int dcp_direct_crossbar_link(struct apple_dcp *d, bool up) { (void)d; (void)up; return 0; }
static bool dcp_is_retained_external(struct apple_dcp *d) { (void)d; return false; }
static bool dcp_hdmi_port_attached(struct apple_dcp *d) { (void)d; return false; }
static void av_service_connect(struct apple_dcp *d) { (void)d; }
static void av_service_disconnect(struct apple_dcp *d) { (void)d; }
static void dcp_relink_link_up(void *r, u64 g, unsigned long j) { (void)r; (void)g; (void)j; }
static void dcp_relink_attach(void *r, u64 g) { (void)r; (void)g; }
static void dcp_firmware_metadata_connection(struct apple_dcp *d, u64 g, bool c) { (void)d; (void)g; (void)c; }
static bool dcp_j714s_ext(struct device *d) { (void)d; return false; }
static void dcp_announce_kick(struct apple_dcp *d) { (void)d; }
static int dcp_dptx_connect(struct apple_dcp *d, u32 port) { (void)d; (void)port; abort(); }
static int dcp_dptx_connect_tile(struct apple_dcp *d) { (void)d; return 0; }
static void dcp_t8132_dptx_teardown_log(struct apple_dcp *d, u32 port, const char *what, int ret)
{ (void)d; (void)port; (void)what; if (ret) warns++; }
static void dcp_t8132_hpd_low(struct apple_dcp *d, u32 port) { (void)d; (void)port; abort(); }
static void iomfb_m4_external_link_gone(struct apple_dcp *d) { (void)d; }
static void iomfb_m4_ext_relink_lost(struct apple_dcp *d) { (void)d; }
static int dcp_peer_unplug_v27(struct apple_dcp *d) { (void)d; return 0; }
static void disconnected_hpd_event(struct apple_connector *c) { (void)c; }
/* The detach keeps the watchdog of a present taken for the display that goes. */
static unsigned int presents_lost;
static bool iomfb_present_lost(struct apple_dcp *d, bool poweroff)
{
	(void)d; assert(!poweroff); presents_lost++; return false;
}
static int dcp_desktop_dptx_disconnect_oob(struct apple_dcp *d, u32 port) { (void)d; (void)port; abort(); }
static int dcp_external_disconnect(struct apple_dcp *d, bool oob) { (void)d; (void)oob; abort(); }
static void dcp_peer_release_26_6(struct apple_dcp *d) { (void)d; abort(); }
#define atomic_set(p, v) (*(p) = (v))
'''

CASES = r'''
static struct platform_device pdev;
static struct apple_dcp *dcp = &((struct { struct apple_dcp d; }){ 0 }).d;
static void *platform_get_drvdata(struct platform_device *p) { (void)p; return the_dcp; }
static struct device_node j713_27 = { "apple,t8132-dcpext", true };
static struct device_node j713_26 = { "apple,t8132-dcpext", false };
static struct apple_epic_service service = { &dptxep };
static int binding;

/* A dcpext of J713 on firmware 27.2 (or the M4 backend), routed to a dock's tunnel */
static void setup(struct device_node *node)
{
	static struct apple_dcp d;

	memset(&d, 0, sizeof(d));
	pdev.dev.of_node = node;
	d.dev = &pdev.dev;
	d.fw_compat = node->v27_2 ? DCP_FIRMWARE_V_27_0 : DCP_FIRMWARE_V_26_6;
	d.phy = &d;
	d.dptx_tunnel = true;
	d.typec_work_enabled = true;
	d.connector_type = DRM_MODE_CONNECTOR_USB;
	d.tb_generation = 7;
	d.tb_dpin_ctx = &binding;
	d.dptxep = &dptxep;
	dptxep.dcp = &d;
	d.dptxport[0].service = &service;
	d.typec_generation = 40;
	the_dcp = dcp = &d;
	nrpcs = 0; warns = errs = infos = 0; link_waits = 0; link_wait_spent = fence_wait_spent = 0;
	release_ret = set_hpd_low_ret = 0; hpd_high = false; link_comes = true; binding_live = true;
	late_pending = compact_orphans = false; orphans_dropped = late_wakes = presents_lost = 0;
	late_refusals = 0; set_hpd_high_ret = 0;
	at_rpc = NULL; at_link_wait = at_fence_wait = NULL; waiter = NULL;
	revoke_after_checks = gated_calls = 0;
}

/* The provider's tunnel goes: it revokes, then the fabric detaches (dcp_tb_release_locked()). */
static void revoke(void) { binding_live = false; }
static void detach(void)
{
	binding_live = false;
	assert(dcp_dptx_disconnect_oob(&pdev, 0) == 0);
}
/* The DPTX calls made before the CRTC was turned off. */
static unsigned int dpms_mark;
static bool hpd_raised_after_dpms(void)
{
	for (unsigned int i = dpms_mark; i < nrpcs; i++)
		if (!strcmp(rpcs[i], "hpd1"))
			return true;
	return false;
}
static void dpms_off(void)
{
	dpms_mark = nrpcs;
	/* dcp_poweroff(): the CRTC is off, then its own release. */
	dcp_typec_crtc_turned_off(dcp);
	cancel_delayed_work(&dcp->typec_reconnect_wq);
	if (dcp->dptxport[0].connected) {
		dptxport_set_hpd(dcp->dptxport[0].service, false);
		dcp_dptx_disconnect(dcp, 0);
	}
}

static void run_work(void)
{
	assert(!dcp->hpd_mutex.held && !dcp->tb_lock.held);
	dcp->typec_reconnect_wq.pending = false;
	dcp_typec_reconnect_work(&dcp->typec_reconnect_wq.work);
	assert(!dcp->hpd_mutex.held && !dcp->tb_lock.held);
}

static const char *revoke_after;
static void revoke_at(const char *what) { if (!strcmp(what, revoke_after)) revoke(); }
/* DPMS during a DPTX call of the connect: it waits for hpd_mutex. */
static void dpms_at(const char *what)
{
	if (strcmp(what, revoke_after))
		return;
	waited_lock = &dcp->hpd_mutex;
	waiter = dpms_off;
}

static void connect_cases(void)
{
	/* The deferred connect of a tunnel: validate, connect, request, HPD, link. */
	setup(&j713_27);
	dcp_typec_attach_deferred(dcp);
	assert(dcp->typec_reconnect_wq.pending && dcp->typec_reconnect_recovery);
	run_work();
	assert(nrpcs == 4 && rpc_seen("validate") && rpc_seen("request") && hpd_high);
	assert(dcp->dptxport[0].connected && !warns);
	/* Every call of the tunnel's connect went out under the provider's admission. */
	assert(gated_calls == 4 && !dcp->dptx_gate_task && !dcp->dptx_gate_lock.held);
	puts("ok connect: the deferred connect of a live tunnel lights it");

	/*
	 * Revoked right after a check of the connect said it was admitted,
	 * before the call it checked for: that call is refused by the gate.
	 */
	for (unsigned int i = 1; i <= 4; i++) {
		setup(&j713_27);
		dcp_typec_attach_deferred(dcp);
		revoke_after_checks = i;
		run_work();
		assert(nrpcs == (i == 4 ? 4 : i - 1) && !hpd_high && !rpc_seen("hpd1"));
		assert(!dcp->dptx_gate_task && !dcp->dptx_gate_lock.held);
		/* The display requested is released, which is never gated. */
		if (i == 4)
			assert(!strcmp(rpcs[3], "release"));
	}
	puts("ok connect: revoked right after its admission check: the call is refused");

	/* Revoked during the link wait, its link come: no success reported. */
	setup(&j713_27);
	dcp_typec_attach_deferred(dcp);
	at_link_wait = revoke;
	scoped_guard(mutex, &dcp->hpd_mutex)
		dcp->typec_reconnect_session = dcp_session_locked(dcp);
	assert(dcp_dptx_connect_raw(dcp, 0, NULL, &dcp->typec_reconnect_session, true, true) == -ESTALE);
	puts("ok connect: revoked during the link wait: not reported connected");

	/* Revoked before the work starts: no DPTX call at all. */
	setup(&j713_27);
	dcp_typec_attach_deferred(dcp);
	revoke();
	run_work();
	assert(nrpcs == 0 && !hpd_high && !dcp->dptxport[0].connected);
	assert(!dcp->typec_reconnect_wq.pending);
	puts("ok connect: revoked before the work: no DPTX call");

	/* Revoked between two calls: nothing after it, a display request withdrawn. */
	static const char *const steps[] = { "validate", "connect", "request" };
	for (unsigned int i = 0; i < 3; i++) {
		setup(&j713_27);
		dcp_typec_attach_deferred(dcp);
		revoke_after = steps[i]; at_rpc = revoke_at;
		run_work();
		assert(!rpc_seen("hpd1") && !hpd_high && !dcp->dptxport[0].connected);
		assert(nrpcs == i + 1 + (i == 2));
		if (i == 2)
			assert(!strcmp(rpcs[3], "release"));
	}
	puts("ok connect: revoked between DPTX calls: none after, HPD never raised");

	/* Revoked during the link wait: the detach ends the wait at once. */
	setup(&j713_27);
	dcp_typec_attach_deferred(dcp);
	link_comes = false;
	at_link_wait = detach;
	run_work();
	assert(link_wait_spent == 0);
	assert(!dcp->dptxport[0].connected && rpc_seen("release"));
	assert(!dcp->typec_reconnect_wq.pending && presents_lost == 1);
	puts("ok connect: revoked during the link wait: the detach does not wait it out");

	/* DPMS off before the work starts, between calls, during the wait. */
	setup(&j713_27);
	dcp->typec_cable_connected = true;
	dcp_typec_attach_deferred(dcp);
	dpms_off();
	run_work();
	assert(nrpcs == 0 && !hpd_high);
	/*
	 * During a DPTX call: the power-off waits for hpd_mutex, which the
	 * connect holds from its first call to its HPD; it then lowers that HPD
	 * and releases the display, and the connect raises nothing after.
	 */
	for (unsigned int i = 0; i < 3; i++) {
		setup(&j713_27);
		dcp->typec_cable_connected = true;
		dcp_typec_attach_deferred(dcp);
		revoke_after = steps[i]; at_rpc = dpms_at;
		link_comes = false;
		run_work();
		assert(dpms_mark && !hpd_raised_after_dpms() && !hpd_high);
		assert(!strcmp(rpcs[nrpcs - 1], "release") && !dcp->dptxport[0].connected);
		assert(link_waits == 1 && !dcp->typec_reconnect_wq.pending);
	}
	setup(&j713_27);
	dcp_typec_attach_deferred(dcp);
	link_comes = false;
	at_link_wait = dpms_off;
	run_work();
	assert(!hpd_raised_after_dpms() && !hpd_high);
	assert(!strcmp(rpcs[nrpcs - 1], "release") && !dcp->dptxport[0].connected);
	assert(!dcp->typec_reconnect_wq.pending);
	puts("ok connect: a CRTC turned off is never woken by the deferred connect");

	/* A connect that fails is not tried again blindly. */
	setup(&j713_27);
	dcp_typec_attach_deferred(dcp);
	link_comes = false;
	run_work();
	assert(link_waits == 1 && !dcp->typec_reconnect_wq.pending);
	assert(dcp->typec_reconnect_wq.queued == 1);
	puts("ok connect: a failed firmware-27 connect is not retried blindly");
}

static int release(bool live, bool tunnel, int ret, struct device_node *node)
{
	int r;

	setup(node);
	dcp->typec_work_enabled = true;
	dcp->dptxport[0].connected = true;
	dcp->dptx_tunnel = tunnel;
	binding_live = live;
	release_ret = ret;
	scoped_guard(mutex, &dcp->hpd_mutex)
		r = dcp_dptx_release_locked(dcp, 0);
	assert(rpc_seen("release"));
	assert(!r == !dcp->dptxport[0].connected);
	return r;
}

static void release_cases(void)
{
	/* The tunnel went and the release timed out: route back, port held. */
	assert(release(false, true, -ETIMEDOUT, &j713_27) == 0 && dcp->t8132_release_late);
	/* Not sent, refused, or on a live or fixed link: the error, as before. */
	assert(release(false, true, -ENOSPC, &j713_27) == -ENOSPC && !dcp->t8132_release_late);
	assert(release(false, true, -EIO, &j713_27) == -EIO);
	assert(release(true, true, -ETIMEDOUT, &j713_27) == -ETIMEDOUT && !dcp->t8132_release_late);
	assert(release(true, false, -ETIMEDOUT, &j713_27) == -ETIMEDOUT);
	assert(release(false, true, 0, &j713_27) == 0 && !dcp->t8132_release_late);
	/* The M4 backend, as before: taken as released whatever the firmware says. */
	assert(release(true, true, -EIO, &j713_26) == 0);
	assert(release(true, true, -ETIMEDOUT, &j713_26) == 0 && dcp->t8132_release_late);
	puts("ok release: only a timed-out release of a gone tunnel is taken as done");

	/* An unanswered HPD low of a gone tunnel holds the port and goes on to the release. */
	setup(&j713_27);
	dcp->dptxport[0].connected = true;
	set_hpd_low_ret = -ETIMEDOUT; release_ret = -ETIMEDOUT;
	binding_live = false;
	assert(dcp_dptx_disconnect_oob(&pdev, 0) == 0);
	assert(rpc_seen("release") && dcp->t8132_release_late && !dcp->dptxport[0].connected);
	setup(&j713_27);
	dcp->dptxport[0].connected = true;
	set_hpd_low_ret = -ETIMEDOUT;
	assert(dcp_dptx_disconnect_oob(&pdev, 0) == -ETIMEDOUT && !rpc_seen("release"));
	puts("ok release: an unanswered HPD low only of a gone tunnel goes on");
}

/* The firmware answers the timed-out call: its slot is freed, its waiters woken, the work queued. */
static void answer(void)
{
	late_pending = false;
	late_wakes++;
	dcp->t8132_port_work.queued++;
}
/* ... refusing it. */
static void answer_refused(void)
{
	late_refusals++;
	answer();
}
static void run_port_work(void)
{
	assert(dcp->t8132_port_work.queued);
	dcp->t8132_port_work.queued = 0;
	dcp_t8132_port_work(&dcp->t8132_port_work);
	assert(!dcp->hpd_mutex.held);
}

/* The dock's tunnel comes back: a new binding, on the same pipeline. */
static void replug(void)
{
	binding_live = true;
	dcp->tb_generation++;
	dcp_typec_attach_deferred(dcp);
}

/* The dock hands over a replacement request: detach, then the new tunnel. */
static void replace(void)
{
	detach();
	replug();
}

/* A gone tunnel's release unanswered, the dock back: its connect does not use the port. */
static void held_replug(void)
{
	release(false, true, -ETIMEDOUT, &j713_27);
	late_pending = true;
	dcp->typec_cable_connected = false;
	replug();
	nrpcs = 0;
	release_ret = 0;
	run_work();
	assert(nrpcs == 0 && !hpd_high && dcp->t8132_release_late && !dcp->typec_reconnect_wq.pending);
	/* It did not wait for the answer either. */
	assert(fence_wait_spent == 0);
}

/* The M4 backend's display goes and comes back (the detach wakes the waiters). */
static void m4_reattach(void)
{
	scoped_guard(mutex, &dcp->hpd_mutex)
		dcp->typec_generation++;
	afk_late_replies_wake(dcp->dptxep);
}

static void fence_cases(void)
{
	/* The replug's connect is held; the answer, whenever it comes, connects it. */
	held_replug();
	/* Meanwhile the old release's link reports are not acted on. */
	assert(dcp_t8132_link_report_late(dcp, "SetActiveLaneCount"));
	answer();
	run_port_work();
	assert(!dcp->t8132_release_late && dcp->typec_reconnect_wq.pending);
	run_work();
	assert(hpd_high && dcp->dptxport[0].connected && nrpcs == 4);
	assert(!dcp_t8132_link_report_late(dcp, "SetActiveLaneCount"));
	puts("ok fence: the replug connects once the old release is answered, however late");

	/* Answered with a refusal: released again, then connected. */
	held_replug();
	answer_refused();
	run_port_work();
	assert(nrpcs == 1 && !strcmp(rpcs[0], "release") && !dcp->t8132_release_late);
	run_work();
	assert(hpd_high && dcp->dptxport[0].connected);
	/* ... refused again: still held, nothing connects. */
	held_replug();
	answer_refused();
	release_ret = -EIO;
	run_port_work();
	assert(nrpcs == 1 && dcp->t8132_release_late && !dcp->typec_reconnect_wq.pending);
	puts("ok fence: an answer that refuses the release has it sent again");

	/* The replug's tunnel goes before the answer: nothing to connect then. */
	held_replug();
	detach();
	answer();
	run_port_work();
	assert(!dcp->t8132_release_late && !dcp->typec_reconnect_wq.pending && !hpd_high);
	puts("ok fence: a display gone before the answer is not connected");

	/* The dock replaces the tunnel before the answer: the replacement connects. */
	held_replug();
	replace();
	answer();
	run_port_work();
	assert(dcp->typec_reconnect_wq.pending);
	assert(dcp->typec_reconnect_session.cookie == dcp->tb_generation);
	run_work();
	assert(hpd_high && dcp->dptxport[0].connected);
	puts("ok fence: a replaced tunnel's replacement connects on the answer");

	/* The CRTC turned off before the answer: not woken, its power-on connects. */
	held_replug();
	dcp->typec_cable_connected = true;
	dpms_off();
	answer();
	run_port_work();
	assert(!dcp->t8132_release_late && !dcp->typec_reconnect_wq.pending && !hpd_high);
	puts("ok fence: a CRTC turned off is not woken by the answer");

	/* Never answered: the port is not used, however often the dock asks. */
	held_replug();
	dcp_typec_attach_repeated(dcp);
	run_port_work();
	run_work();
	assert(nrpcs == 0 && !hpd_high && dcp->t8132_release_late);
	assert(dcp_t8132_link_report_late(dcp, "SetActiveLaneCount"));
	puts("ok fence: a release never answered keeps the port unused");

	/* A connect paused before its checks does not clear a hold a detach just set. */
	release(false, true, -ETIMEDOUT, &j713_27);
	late_pending = true;
	binding_live = true;
	dcp->typec_cable_connected = true;
	scoped_guard(mutex, &dcp->hpd_mutex)
		dcp->typec_reconnect_session = dcp_session_locked(dcp);
	nrpcs = 0;
	assert(dcp_dptx_connect_raw(dcp, 0, NULL, &dcp->typec_reconnect_session, true, false) == -EBUSY);
	assert(dcp->t8132_release_late && nrpcs == 0);
	puts("ok fence: only the answer frees a held port, never a connect");

	/* The M4 backend gives up on its compact orphans, as before. */
	setup(&j713_26);
	compact_orphans = true;
	dcp->typec_cable_connected = true;
	scoped_guard(mutex, &dcp->hpd_mutex)
		dcp->typec_reconnect_session = dcp_session_locked(dcp);
	assert(dcp_dptx_connect_raw(dcp, 0, NULL, &dcp->typec_reconnect_session, true, false) == 0);
	assert(orphans_dropped == 1 && hpd_high);
	/* A new attachment revokes the session and ends its wait at once. */
	setup(&j713_26);
	compact_orphans = true;
	dcp->typec_cable_connected = true;
	scoped_guard(mutex, &dcp->hpd_mutex)
		dcp->typec_reconnect_session = dcp_session_locked(dcp);
	at_fence_wait = m4_reattach;
	assert(dcp_dptx_connect_raw(dcp, 0, NULL, &dcp->typec_reconnect_session, true, false) == -ESTALE);
	assert(fence_wait_spent == 0 && !hpd_high);
	/* So does the CRTC's power-off, for a connect that must not wake it. */
	setup(&j713_26);
	compact_orphans = true;
	dcp->typec_cable_connected = true;
	scoped_guard(mutex, &dcp->hpd_mutex)
		dcp->typec_reconnect_session = dcp_session_locked(dcp);
	at_fence_wait = dpms_off;
	assert(dcp_dptx_connect_raw(dcp, 0, NULL, &dcp->typec_reconnect_session, true, true) == -ESTALE);
	assert(fence_wait_spent == 0 && !hpd_high);
	puts("ok fence: the M4 backend's wait, ended by its answers, a revocation or DPMS");
}

/* A connect that fails once its display is requested releases it as a detach does. */
static void failed_connect_cases(void)
{
	/*
	 * Revoked after Request, the release unanswered: the port is held, and
	 * the replacement does not use it until the answer.
	 */
	setup(&j713_27);
	dcp->typec_cable_connected = true;
	dcp_typec_attach_deferred(dcp);
	revoke_after = "request"; at_rpc = revoke_at;
	release_ret = -ETIMEDOUT;
	run_work();
	assert(rpc_seen("release") && dcp->t8132_release_late);
	assert(!dcp->dptxport[0].connected && !dcp->dptxport[0].release_owed);
	at_rpc = NULL;
	late_pending = true;
	replace();
	nrpcs = 0;
	release_ret = 0;
	run_work();
	assert(nrpcs == 0 && !hpd_high);
	answer();
	run_port_work();
	run_work();
	assert(hpd_high && dcp->dptxport[0].connected);
	puts("ok failed connect: revoked after Request, release unanswered: the port is held");

	/* HPD refused on a live link, the release refused too: owed to the detach. */
	setup(&j713_27);
	dcp->typec_cable_connected = true;
	dcp_typec_attach_deferred(dcp);
	set_hpd_high_ret = -EIO; release_ret = -EIO;
	run_work();
	assert(!dcp->dptxport[0].connected && dcp->dptxport[0].release_owed);
	nrpcs = 0; release_ret = 0;
	assert(dcp_dptx_disconnect_oob(&pdev, 0) == 0);
	assert(rpc_seen("release") && !dcp->dptxport[0].release_owed);
	/* ... or to the next connect, which sends it first. */
	setup(&j713_27);
	dcp->typec_cable_connected = true;
	dcp_typec_attach_deferred(dcp);
	set_hpd_high_ret = -EIO; release_ret = -EIO;
	run_work();
	nrpcs = 0; release_ret = 0; set_hpd_high_ret = 0;
	scoped_guard(mutex, &dcp->hpd_mutex)
		dcp->typec_reconnect_session = dcp_session_locked(dcp);
	assert(dcp_dptx_connect_raw(dcp, 0, NULL, &dcp->typec_reconnect_session, true, false) == 0);
	assert(!strcmp(rpcs[0], "release") && !strcmp(rpcs[1], "validate") && hpd_high);
	assert(!dcp->dptxport[0].release_owed);
	/* A release owed and refused again: no connect. */
	setup(&j713_27);
	dcp->typec_cable_connected = true;
	dcp_typec_attach_deferred(dcp);
	set_hpd_high_ret = -EIO; release_ret = -EIO;
	run_work();
	nrpcs = 0; set_hpd_high_ret = 0;
	scoped_guard(mutex, &dcp->hpd_mutex)
		dcp->typec_reconnect_session = dcp_session_locked(dcp);
	assert(dcp_dptx_connect_raw(dcp, 0, NULL, &dcp->typec_reconnect_session, true, false) == -EIO);
	assert(nrpcs == 1 && dcp->dptxport[0].release_owed && !hpd_high);
	puts("ok failed connect: a release refused is owed to the next release or connect");
}

/* The DPTX service's publication, or a repeated request, connects a display left waiting. */
static void resume_cases(void)
{
	struct apple_epic_service *published = dcp->dptxport[0].service;

	setup(&j713_27);
	dcp->typec_cable_connected = true;
	published = dcp->dptxport[0].service;
	dcp->dptxport[0].service = NULL;
	dcp_typec_attach_deferred(dcp);
	run_work();
	assert(nrpcs == 0 && !dcp->typec_reconnect_wq.pending);
	/* dptxport_init(): the service is published. */
	dcp->dptxport[0].service = published;
	dcp_typec_attach_repeated(dcp);
	run_port_work();
	run_work();
	assert(hpd_high && dcp->dptxport[0].connected);
	/* A repeat for a display connected already does nothing more. */
	nrpcs = 0;
	dcp_typec_attach_repeated(dcp);
	run_port_work();
	assert(!dcp->typec_reconnect_wq.pending && nrpcs == 0);
	puts("ok resume: the service's publication connects a display that came first");

	/* The M4 backend has its own: nothing queued. */
	setup(&j713_26);
	dcp_typec_attach_repeated(dcp);
	assert(!dcp->t8132_port_work.queued);
	puts("ok resume: firmware 27 only");
}

int main(void)
{
	connect_cases();
	release_cases();
	fence_cases();
	failed_connect_cases();
	resume_cases();
	puts("PASS firmware-27.2 T8132 dcpext deferred connect, revocation, DPMS, release and its fence");
	return 0;
}
'''


def build(tmp, name, sources):
    unit = PRE + define(sources['dcp'], 'DCP_T8132_ORPHAN_WAIT')
    unit += define(sources['dcp'], 'DPTX_CONNECT_TIMEOUT')
    unit += define(sources['dcp'], 'DPTX_TUNNEL_CONNECT_TIMEOUT')
    unit += define(sources['dcp'], 'DPTX_RECONNECT_DELAY')
    unit += define(sources['dcp'], 'DPTX_RECONNECT_RETRIES')
    unit += define(sources['dcp'], 'DCP_LINKCFG_POLL_MS')
    unit += 'typedef bool apple_dp_tunnel_binding_live_t(void *);\n'
    unit += 'typedef int apple_dp_tunnel_binding_call_t(void *, int (*)(void *), void *);\n'
    for src, fn in FUNCTIONS:
        body = function(sources[src], fn)
        body = body.replace('typeof(apple_dp_tunnel_binding_live) *live;',
                            'apple_dp_tunnel_binding_live_t *live;')
        body = body.replace('typeof(apple_dp_tunnel_binding_call) *call;',
                            'apple_dp_tunnel_binding_call_t *call;')
        unit += body
    unit += CASES
    c = Path(tmp) / (name + '.c')
    binary = Path(tmp) / name
    c.write_text(unit)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
                    '-Wno-unused-but-set-variable', '-Wno-sign-compare',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(c), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory(prefix='t8132-v27-connect-') as tmp:
        for name, (src, old, new) in MUTATIONS.items():
            assert SOURCES[src].count(old) == 1, 'stale mutation: ' + name
            sources = dict(SOURCES)
            sources[src] = SOURCES[src].replace(old, new)
            run = build(tmp, name, sources)
            assert run.returncode and 'Assertion' in run.stderr, 'mutation survived: ' + name
            print('PASS negative control:', name)
        run = build(tmp, 'production', SOURCES)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        raise SystemExit(run.returncode)


if __name__ == '__main__':
    main()
