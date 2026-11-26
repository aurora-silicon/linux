#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Replay a dock's display going and coming back with a present in flight.

The swap watchdog of a pipeline on firmware with the IOMFB queue (H17P, 27) is
compiled from the production bodies (dcp_swap_watchdog_arm(),
dcp_swap_watchdog_keep(), dcp_swap_watchdog_complete(), dcp_swap_watchdog(),
iomfb_present_begun_h17p(), iomfb_present_lost(),
iomfb_retire_dropped_present_h17p() and dcp_present_retire_h17p()) and from
the CRTC's power-off (dcp_poweroff())
against host mocks. The pipeline is a dcpext of the Air (J713) on firmware
27.2, whose node declares a USB-C connector, so it is a Type-C output with or
without a route:

- a present the firmware accepted for a display that went, at the detach or
  at the CRTC's power-off after it, is retired by the watchdog instead of
  holding the queue until its timeout declares the processor crashed;
- the firmware's acceptance runs under the queue's lock, before or after the
  power-off looks: either way the present that will not complete is watched;
- a present begun for the display that went is that display's whenever the
  firmware accepts it, also after the detach and the power-off have looked
  and a replacement is plugged in: retired without a retrain, the
  replacement left alone;
- so is a present the host started for it, whenever the firmware answers
  its SwapStart: its attachment is read as the host starts it and carried
  to the reply (iomfb_present_begun_h17p()), also when the answer comes after
  the detach and a replug;
- a detach between the present's retirement and its retrain is seen before
  anything is retrained: the watchdog decides under hpd_mutex, where the
  detach counts itself;
- retiring that present only completes its flip: it neither invalidates the
  mode nor queues a hotplug, so a display plugged in before the watchdog
  expires is not retrained for the old one's present;
- a present that completes after a replug stops the watchdog;
- the power-off of a live output (the internal panel, a fixed HDMI output,
  a Type-C display still attached) stops the watchdog as before, and a
  present of a live display that does not complete is retrained as before.

Each negative control puts one piece back as it was, or drops it; the replay
must then fail.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DCP = (ROOT / 'drivers/gpu/drm/apple/dcp.c').read_text()
IOMFB = (ROOT / 'drivers/gpu/drm/apple/iomfb.c').read_text()
H17P = (ROOT / 'drivers/gpu/drm/apple/iomfb_h17p.h').read_text()
FABRIC = (ROOT / 'drivers/gpu/drm/apple/dcp-fabric.c').read_text()

SOURCES = {'dcp': DCP, 'iomfb': IOMFB, 'h17p': H17P, 'fabric': FABRIC}
FUNCTIONS = [
    ('fabric', 'dcp_is_typec_output'), ('h17p', 'dcp_present_retire_h17p'),
    ('iomfb', 'iomfb_uses_queue'), ('iomfb', 'iomfb_present_begun_h17p'),
    ('dcp', 'dcp_swap_watchdog_arm'), ('dcp', 'dcp_swap_watchdog_keep'),
    ('dcp', 'dcp_swap_watchdog_complete'),
    ('iomfb', 'iomfb_present_lost'), ('iomfb', 'iomfb_retire_dropped_present_h17p'),
    ('dcp', 'dcp_swap_watchdog'), ('dcp', 'dcp_poweroff'),
]

# name -> (source, old, new)
MUTATIONS = {
    # The power-off cancels the watchdog of a present that will not complete.
    'poweroff-cancels': ('dcp', '\tif (display_gone)\n\t\tiomfb_present_lost(dcp, true);\n\telse\n'
                         '\t\tcancel_delayed_work(&dcp->swap_watchdog_wq);',
                         '\tcancel_delayed_work(&dcp->swap_watchdog_wq);'),
    # Every power-off with a present in flight keeps the watchdog (the earlier rule).
    'poweroff-keeps-live': ('dcp', '\tif (display_gone)\n\t\tiomfb_present_lost(dcp, true);',
                            '\tif (true)\n\t\tiomfb_present_lost(dcp, true);'),
    # The check outside the queue's lock: acceptance can slip between it and the cancel.
    'unlocked-check': ('iomfb', '\tguard(mutex)(&dcp->iomfb.lock);\n\tif (state->pending && state->accepted) {',
                       '\tif (state->pending && state->accepted) {'),
    # The retired present of a display that went retrains what is plugged in now.
    'lost-retrains': ('dcp', '\t\tdev_info(dcp->dev, "swap of a display that went retired\\n");\n\t\tdcp_drm_crtc_vblank(dcp->crtc);\n\t\treturn;\n',
                      '\t\tdev_info(dcp->dev, "swap of a display that went retired\\n");\n'),
    # A present is the current display's once accepted (the loss judged at acceptance).
    'identity-at-acceptance': ('dcp', '\tif (dcp_is_typec_output(dcp) || dcp_desktop_swap_watched(dcp)) {\n',
                               '\tif (dcp_is_typec_output(dcp) || dcp_desktop_swap_watched(dcp)) {\n'
                               '\t\tdcp->present_state_h17p.attachment = READ_ONCE(dcp->typec_unplugs);\n'),
    # Nothing is ever taken as lost: the replacement is retrained.
    'never-lost': ('dcp', '\tif (attachment != dcp->typec_unplugs) {', '\tif (false) {'),
    # Everything taken as lost: a stuck present of a live display is not retrained.
    'always-lost': ('dcp', '\tif (attachment != dcp->typec_unplugs) {', '\tif (true) {'),
    # The attachment read at the SwapStart's answer, not when the host started it.
    'attachment-at-reply': ('iomfb', '\tdcp->present_state_h17p.attachment = attachment;',
                            '\tdcp->present_state_h17p.attachment = READ_ONCE(dcp->typec_unplugs);'),
    # The loss decided without hpd_mutex, where a detach counts itself.
    'retrain-unlocked': ('dcp', '\tguard(mutex)(&dcp->hpd_mutex);\n\tif (attachment != dcp->typec_unplugs) {',
                         '\tif (attachment != dcp->typec_unplugs) {'),
    # Nothing kept: the present holds the queue until its timeout.
    'no-keep': ('iomfb', '\tif (state->pending && state->accepted) {\n\t\tdcp_swap_watchdog_keep(dcp);\n\t\treturn true;\n\t}',
                '\tif (false) {\n\t\tdcp_swap_watchdog_keep(dcp);\n\t\treturn true;\n\t}'),
}


def function(text, name):
    match = re.search(r'^(?:VISIBLE_IF_KUNIT\s+)?(?:static\s+)?(?:inline\s+)?'
                      r'(?:bool|int|void)\s*\n?' + re.escape(name) +
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
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
static unsigned int infos, warns;
#define dev_info(dev, ...) ((void)(dev), infos++)
#define dev_warn(dev, ...) ((void)(dev), warns++)
#define dev_err(dev, ...) ((void)(dev), warns++)
#define dev_dbg(dev, ...) ((void)(dev))
static unsigned long jiffies;
static unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define xchg(p, v) ({ __typeof__(*(p)) __o = *(p); *(p) = (v); __o; })
#define IOMFB_DROPPED_MAX 4

/* Locks, with what another thread holding or waiting for one does. */
struct mutex { int held; void (*before)(void); void (*after)(void); };
static void mutex_lock(struct mutex *m)
{
	if (m->before) { void (*f)(void) = m->before; m->before = NULL; f(); }
	assert(!m->held);
	m->held = 1;
}
static void mutex_unlock(struct mutex *m)
{
	assert(m->held);
	m->held = 0;
	if (m->after) { void (*f)(void) = m->after; m->after = NULL; f(); }
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
		     mutex_ref_take(lock); CAT(once_, __LINE__); CAT(once_, __LINE__) = false)
#define lockdep_assert_held(l) assert((l)->held)

struct work_struct { int unused; };
struct delayed_work { struct work_struct work; bool pending; unsigned long due; unsigned int arms; };
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
static void *system_dfl_wq, *system_unbound_wq;
static bool mod_delayed_work(void *wq, struct delayed_work *w, unsigned long delay)
{
	bool was = w->pending;

	(void)wq; w->pending = true; w->due = jiffies + delay; w->arms++; return was;
}
static bool delayed_work_pending(struct delayed_work *w) { return w->pending; }
static bool cancel_delayed_work(struct delayed_work *w) { bool was = w->pending; w->pending = false; return was; }
static bool cancel_delayed_work_sync(struct delayed_work *w) { return cancel_delayed_work(w); }
static bool queue_work(void *wq, struct work_struct *w) { (void)wq; (void)w; abort(); }

enum { DCP_FIRMWARE_V_26_6 = 26, DCP_FIRMWARE_V_26_6_DESKTOP = 260, DCP_FIRMWARE_H17P = 17,
       DCP_FIRMWARE_V_27_0 = 27 };
enum { DCP_IOMFB_METHODS_DEFAULT, DCP_IOMFB_METHODS_H17G };
enum { DRM_MODE_CONNECTOR_USB = 32, DRM_MODE_CONNECTOR_HDMIA = 11, DRM_MODE_CONNECTOR_eDP = 14 };
struct platform_device { int unused; };
struct apple_epic_service { int unused; };
struct dptx_port { bool connected, enabled; struct apple_epic_service *service; };
struct apple_connector { const char *sink; bool connected; };
struct apple_crtc { int unused; };
struct dcp_present_state_h17p {
	bool pending, accepted;
	u32 swap_id;
	unsigned int nr_retired;
	u32 retired[IOMFB_DROPPED_MAX];
	u64 attachment;
};
struct iomfb_scanout_h17p { int link; };
struct apple_dcp {
	void *dev;
	int fw_compat, fixed_connector_type;
	struct { int iomfb_method_profile; } hw;
	void *active_typec_route, *avep, *hdmi_hpd;
	struct mutex external_lock;
	bool ext26_pm_preparing, external_stopped, external_stop_deferred;
	struct work_struct ext26_blank_work;
	u64 external_crtc_generation;
	struct mutex hpd_mutex;
	struct { struct mutex lock; struct iomfb_scanout_h17p *next_scanout; int dropped; } iomfb;
	struct dcp_present_state_h17p present_state_h17p;
	struct apple_connector *connector;
	struct apple_crtc *crtc;
	struct { bool valid; } mode_state;
	u64 typec_unplugs, crtc_unplugs;
	bool typec_cable_connected, typec_crtc_off, vrr_enabled, external_native;
	unsigned int swap_watchdog_retrains;
	unsigned long swap_watchdog_deadline;
	struct delayed_work swap_watchdog_wq, typec_reconnect_wq, placeholder_edid_wq;
	struct { int done; } typec_iomfb_hpd_ready;
	struct { bool present_pending, vrr_failed, takeover_adopted, mode_live; u32 present_id; } *desktop;
	struct { bool active; } split;
	struct dptx_port dptxport[2];
};
static struct apple_dcp *the_dcp;
static void *platform_get_drvdata(struct platform_device *p) { (void)p; return the_dcp; }
static bool dcp_desktop_swap_watched(struct apple_dcp *d) { (void)d; return false; }

/* What the watchdog's recovery does, and what the queue sees. */
static unsigned int vblanks, hotplugs, invalidates, failed, advanced, unarmed, poweroffs, hpd_lows;
static struct apple_connector *hotplugged;
static void dcp_drm_crtc_vblank(struct apple_crtc *c) { (void)c; vblanks++; }
static void dcp_queue_hotplug(struct apple_connector *c) { hotplugs++; hotplugged = c; }
static void dcp_mode_invalidate(void *m) { (void)m; invalidates++; }
static void dcp_takeover_adopt_proven(struct apple_dcp *d) { (void)d; }
static void dcp_unarm_retained_framebuffers(struct apple_dcp *d, u32 id) { (void)d; (void)id; unarmed++; }
static void dcp_backlight_desktop_finish(struct apple_dcp *d, u32 id, bool a, bool b)
{ (void)d; (void)id; (void)a; (void)b; }
static void iomfb_present_failed_h17p(struct apple_dcp *d) { (void)d; failed++; }
static bool iomfb_present_brightness_only_h17p(struct apple_dcp *d) { (void)d; return false; }
static void iomfb_queue_advance(struct apple_dcp *d) { (void)d; advanced++; }
#define list_add_tail(a, b) ((void)(a), (void)(b))

/* The CRTC's power-off, outside the watchdog. */
static bool panel;
static bool iomfb_is_m4(struct apple_dcp *d) { (void)d; return false; }
static bool dcp_is_ext26(struct apple_dcp *d) { (void)d; return false; }
static bool dcp_is_retained_external(struct apple_dcp *d) { (void)d; return false; }
static bool dcp_uses_soft_dpms(struct apple_dcp *d) { (void)d; return panel; }
static int dcp_backlight_dpms(struct apple_dcp *d, bool on) { (void)d; (void)on; return 0; }
static void dcp_dpms_power_off(struct apple_dcp *d) { (void)d; poweroffs++; }
static bool dcp_h17g_panel(struct apple_dcp *d) { (void)d; return false; }
static void iomfb_blank_h17p(struct apple_dcp *d) { (void)d; abort(); }
static int iomfb_m4_blank(struct apple_dcp *d) { (void)d; abort(); }
static void _dcp_poweroff(struct apple_dcp *d) { (void)d; poweroffs++; }
static void av_service_disconnect(struct apple_dcp *d) { (void)d; }
static void reinit_completion(void *c) { (void)c; }
static bool dptx_service_available(struct dptx_port *p) { return p->service; }
static int dptxport_set_hpd(struct apple_epic_service *s, bool on) { (void)s; if (!on) hpd_lows++; return 0; }
static int dcp_dptx_disconnect(struct apple_dcp *d, u32 port) { (void)d; (void)port; return 0; }
static void dcp_external_retry(struct apple_dcp *d, const char *why, int e, unsigned int ms)
{ (void)d; (void)why; (void)e; (void)ms; abort(); }
static int dcp_external_stop_locked(struct apple_dcp *d, u64 g) { (void)d; (void)g; abort(); }
static void dcp_external_crtc_released(struct apple_dcp *d) { (void)d; abort(); }
static bool hdmi_present;
static int gpiod_get_value_cansleep(void *g) { (void)g; return hdmi_present; }
static void disconnected_hpd_event(struct apple_connector *c) { (void)c; }
static void dcp_typec_crtc_turned_off(struct apple_dcp *d)
{
	scoped_guard(mutex, &d->hpd_mutex) {
		if (d->typec_cable_connected)
			d->typec_crtc_off = true;
	}
}
'''

CASES = r'''
static struct platform_device pdev;
static struct apple_connector dock = { "dock monitor", true }, replacement = { "replacement", true };
static struct apple_crtc crtc;
static struct apple_epic_service service;

/* A pipeline on firmware 27: a J713 dcpext (USB-C connector), the panel, or a fixed HDMI output */
static struct apple_dcp *pipe(int connector_type)
{
	static struct apple_dcp d;

	memset(&d, 0, sizeof(d));
	d.fw_compat = DCP_FIRMWARE_V_27_0;
	d.fixed_connector_type = connector_type;
	d.connector = &dock;
	d.crtc = &crtc;
	d.mode_state.valid = true;
	d.dptxport[0].service = &service;
	the_dcp = &d;
	jiffies = 1000;
	vblanks = hotplugs = invalidates = failed = advanced = unarmed = poweroffs = hpd_lows = 0;
	infos = warns = 0;
	hotplugged = NULL;
	panel = connector_type == DRM_MODE_CONNECTOR_eDP;
	return &d;
}

/* The host starts a present: its SwapStart carries the attachment (do_swap()). */
static u64 started;
static void start(void)
{
	started = READ_ONCE(the_dcp->typec_unplugs);
}
/* The firmware answers the SwapStart: the present begins (dcp_present_begin()). */
static void answered(void)
{
	struct apple_dcp *d = the_dcp;
	static u32 id = 300;

	mutex_lock(&d->iomfb.lock);
	assert(!d->present_state_h17p.pending);
	d->present_state_h17p.pending = true;
	d->present_state_h17p.accepted = false;
	d->present_state_h17p.swap_id = ++id;
	iomfb_present_begun_h17p(d, started);
	mutex_unlock(&d->iomfb.lock);
}
/* ... at once, for the display attached now. */
static void begin(void)
{
	start();
	answered();
}
/* The firmware accepts it (dcp_swapped(), under the queue's lock); one begun now if none is. */
static void accept(void)
{
	struct apple_dcp *d = the_dcp;

	if (!d->present_state_h17p.pending)
		begin();
	mutex_lock(&d->iomfb.lock);
	d->present_state_h17p.accepted = true;
	if (d->fixed_connector_type == DRM_MODE_CONNECTOR_USB)
		dcp_swap_watchdog_arm(d);
	mutex_unlock(&d->iomfb.lock);
}
/* ... and completes it (dcpep_cb_swap_complete()). */
static void completes(void)
{
	struct apple_dcp *d = the_dcp;

	mutex_lock(&d->iomfb.lock);
	d->present_state_h17p.pending = d->present_state_h17p.accepted = false;
	dcp_swap_watchdog_complete(d);
	mutex_unlock(&d->iomfb.lock);
}
/* The watchdog runs when due. */
static void expire(void)
{
	struct apple_dcp *d = the_dcp;

	assert(d->swap_watchdog_wq.pending);
	jiffies = d->swap_watchdog_wq.due;
	d->swap_watchdog_wq.pending = false;
	dcp_swap_watchdog(&d->swap_watchdog_wq.work);
}
/* The dock goes (dcp_dptx_disconnect_oob()): the unplug is counted, a present is kept. */
static void unplug(void)
{
	mutex_lock(&the_dcp->hpd_mutex);
	the_dcp->typec_unplugs++;
	mutex_unlock(&the_dcp->hpd_mutex);
	iomfb_present_lost(the_dcp, false);
}
/* The dock's display is gone and its CRTC turned off: the power-off counts it gone. */
static void gone(void)
{
	mutex_lock(&the_dcp->hpd_mutex);
	the_dcp->typec_unplugs++;
	mutex_unlock(&the_dcp->hpd_mutex);
}
/* ... and a replacement is plugged in on the same pipeline. */
static void unplug_replug(void)
{
	unplug();
	the_dcp->connector = &replacement;
}

static void lost_display(void)
{
	struct apple_dcp *d;

	/* Accepted, then the dock goes and the CRTC is turned off: retired, no retrain. */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	accept();
	d->swap_watchdog_wq.pending = false;	/* stopped by something since */
	unplug();
	assert(d->swap_watchdog_wq.pending && infos == 1);
	dcp_poweroff(&pdev);
	assert(d->swap_watchdog_wq.pending && poweroffs == 1 && !hpd_lows);
	/* A display plugged in before the watchdog expires: not retrained for the old present. */
	d->connector = &replacement;
	expire();
	assert(failed == 1 && advanced == 1 && vblanks == 1);
	assert(!hotplugs && !invalidates && d->mode_state.valid);
	assert(!d->present_state_h17p.pending);
	/* The replacement's own presents are watched and retrained as before. */
	accept();
	expire();
	assert(invalidates == 1 && hotplugs == 1 && hotplugged == &replacement);
	puts("ok lost display: its present retired without a retrain, the replacement's as before");

	/* The power-off alone, after the unplug, keeps the watchdog too. */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	accept();
	gone();
	d->swap_watchdog_wq.pending = false;
	dcp_poweroff(&pdev);
	assert(d->swap_watchdog_wq.pending);
	expire();
	assert(advanced == 1 && !hotplugs && !invalidates);
	puts("ok lost display: kept by the power-off after the unplug");

	/* The firmware completes it after a replug: the watchdog stops. */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	accept();
	unplug();
	d->connector = &replacement;
	completes();
	assert(!d->swap_watchdog_wq.pending && !failed);
	puts("ok lost display: completed after a replug, the watchdog stops");

	/* The firmware aborts it instead: the next display's present is its own. */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	accept();
	unplug();
	mutex_lock(&d->iomfb.lock);
	d->present_state_h17p.pending = d->present_state_h17p.accepted = false;
	mutex_unlock(&d->iomfb.lock);
	d->connector = &replacement;
	accept();
	expire();
	assert(invalidates == 1 && hotplugs == 1 && hotplugged == &replacement);
	puts("ok lost display: aborted, the replacement's next present is watched as its own");
}

/* The firmware's acceptance runs under the queue's lock, as the power-off looks. */
static void acceptance_during_poweroff(void)
{
	struct apple_dcp *d;

	/* Accepted just before the power-off takes the lock: kept. */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	begin();
	gone();
	d->iomfb.lock.before = accept;
	dcp_poweroff(&pdev);
	assert(d->swap_watchdog_wq.pending);
	d->connector = &replacement;
	expire();
	assert(failed == 1 && advanced == 1 && !hotplugs && !invalidates);
	/*
	 * Accepted just after it, with a display plugged in meanwhile: armed at
	 * the acceptance, and still the present of the display that went.
	 */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	begin();
	gone();
	d->iomfb.lock.after = accept;
	dcp_poweroff(&pdev);
	d->connector = &replacement;
	assert(d->swap_watchdog_wq.pending);
	expire();
	assert(failed == 1 && advanced == 1 && vblanks == 1);
	assert(!hotplugs && !invalidates && d->mode_state.valid);
	puts("ok acceptance during the power-off: watched in either order, the replacement left alone");

	/*
	 * Begun before the detach, accepted after the detach and the power-off
	 * have looked: retired as the lost display's, not the replacement's.
	 */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	begin();
	unplug();
	assert(!d->swap_watchdog_wq.pending);
	dcp_poweroff(&pdev);
	d->connector = &replacement;
	accept();
	expire();
	assert(failed == 1 && !hotplugs && !invalidates && d->mode_state.valid);
	/* The replacement's own stuck present is retrained, as before. */
	accept();
	expire();
	assert(invalidates == 1 && hotplugs == 1 && hotplugged == &replacement);
	puts("ok accepted after the detach: the lost display's, the replacement's own retrained");
}

/* The power-off of a live output stops the watchdog, as before. */
static void live_outputs(void)
{
	static const int types[] = { DRM_MODE_CONNECTOR_eDP, DRM_MODE_CONNECTOR_HDMIA,
				     DRM_MODE_CONNECTOR_USB };
	struct apple_dcp *d;

	for (unsigned int i = 0; i < 3; i++) {
		d = pipe(types[i]);
		hdmi_present = true;
		accept();
		d->swap_watchdog_wq.pending = true;
		dcp_poweroff(&pdev);
		assert(!d->swap_watchdog_wq.pending && !infos);
	}
	/* A live Type-C display's present that does not complete is retrained, as before. */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	accept();
	expire();
	assert(failed == 1 && invalidates == 1 && hotplugs == 1 && hotplugged == &dock);
	/* The detach with nothing in flight keeps nothing. */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	unplug();
	assert(!d->swap_watchdog_wq.pending);
	/* Firmware without the IOMFB queue: the power-off stops it, nothing is kept. */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	d->fw_compat = DCP_FIRMWARE_V_26_6;
	d->present_state_h17p.pending = d->present_state_h17p.accepted = true;
	d->typec_unplugs++;
	d->swap_watchdog_wq.pending = true;
	dcp_poweroff(&pdev);
	assert(!d->swap_watchdog_wq.pending);
	puts("ok live outputs: panel, fixed HDMI and attached Type-C DPMS stop the watchdog");
}

/*
 * The host starts a present for the dock's display; the dock goes and a
 * replacement comes before the firmware answers the SwapStart. The present
 * is the display's that went: accepted and stuck, it is retired without a
 * retrain. A detach between its retirement and the retrain is seen too.
 */
static void started_before_detach(void)
{
	struct apple_dcp *d;

	d = pipe(DRM_MODE_CONNECTOR_USB);
	start();
	unplug_replug();
	answered();
	accept();
	expire();
	assert(failed == 1 && advanced == 1 && vblanks == 1);
	assert(!hotplugs && !invalidates && d->mode_state.valid);
	/* The replacement's own presents are retrained as before. */
	accept();
	expire();
	assert(invalidates == 1 && hotplugs == 1 && hotplugged == &replacement);
	puts("ok started before the detach, answered after the replug: retired, the replacement left alone");

	/* The dock goes while the watchdog retires its stuck present. */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	accept();
	d->iomfb.lock.after = unplug_replug;
	expire();
	assert(failed == 1 && !hotplugs && !invalidates && d->mode_state.valid);
	/* ... or while the watchdog waits for hpd_mutex to retrain it. */
	d = pipe(DRM_MODE_CONNECTOR_USB);
	accept();
	d->hpd_mutex.before = unplug_replug;
	expire();
	assert(d->typec_unplugs == 1);
	assert(failed == 1 && !hotplugs && !invalidates && d->mode_state.valid);
	puts("ok a detach between the retirement and the retrain: the replacement left alone");
}

int main(void)
{
	lost_display();
	started_before_detach();
	acceptance_during_poweroff();
	live_outputs();
	puts("PASS a dock display's present in flight across its unplug, power-off and replug");
	return 0;
}
'''


def build(tmp, name, sources):
    unit = PRE + define(sources['dcp'], 'DCP_SWAP_WATCHDOG_MS')
    unit += define(sources['dcp'], 'DCP_SWAP_WATCHDOG_RETRAINS')
    for src, fn in FUNCTIONS:
        unit += function(sources[src], fn)
    unit += CASES
    c = Path(tmp) / (name + '.c')
    binary = Path(tmp) / name
    c.write_text(unit)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(c), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory(prefix='dock-display-replug-') as tmp:
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
