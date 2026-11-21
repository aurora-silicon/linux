#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Replay a READY worker waiting behind a native26 layout-changing commit."""
from pathlib import Path
import subprocess
import tempfile
import sys

root = Path(__file__).resolve().parents[2]
s = (root / 'drivers/gpu/drm/apple/iomfb.c').read_text()
a = s.index('static int dcp_retrain_active_crtc(')
b = s.index('\nvoid dcp_handle_hotplug_actions', a)
body = s[a:b]
if '--negative-control' in sys.argv:
    a = body.index('\t} else if (dcp_is_ext26(dcp) && crtc && crtc->state &&')
    b = body.index('\t} else if (crtc && crtc->state && crtc->state->active &&', a)
    body = body[:a] + body[b:]

pre = r'''
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#define READ_ONCE(x) (x)
struct mode { int id; };
struct drm_device { int unused; };
struct drm_modeset_acquire_ctx { int unused; };
struct drm_crtc_state { bool active; struct mode mode; };
struct drm_crtc { struct drm_crtc_state *state; };
struct apple_connector { struct { struct drm_device *dev; struct { struct drm_crtc *crtc; } *state; } base; };
struct apple_dcp { bool native, admitted, external_stopped; struct { bool valid; } mode_state; };
struct drm_mode_set { struct drm_crtc *crtc; };
static int resets, toggles, releases, transition;
static bool lost;
static struct apple_dcp *current;
static void acquire(void) {
 /* Model the commit that held the modeset locks before the queued worker. */
 if (transition) {
  current->admitted = false;
  current->mode_state.valid = false;
  /* Reconnect has already started; the old patch's guard no longer holds. */
  current->external_stopped = false;
 }
}
#define DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, flags, ret) do { acquire();
#define DRM_MODESET_LOCK_ALL_END(dev, ctx, ret) } while (0)
static bool dcp_is_ext26(struct apple_dcp *d) { return d->native; }
static bool dcp_has_mode(struct apple_dcp *d, const struct mode *m) { return d->admitted && m->id == 1; }
static bool dcp_crtc_lost_route(struct drm_crtc *c, struct apple_connector *p) { return lost; }
static bool dcp_crtc_can_retrain(struct drm_crtc *c, struct apple_connector *p) { return true; }
static int drm_atomic_helper_set_config(struct drm_mode_set *s, void *ctx) { releases++; return 0; }
static int dcp_set_crtc_active(struct drm_crtc *c, void *ctx, bool on) { toggles++; c->state->active = on; return 0; }
static int drm_atomic_helper_reset_crtc(struct drm_crtc *c, void *ctx) { resets++; return current->admitted ? 0 : -22; }
'''
main = r'''
int main(void) {
 (void)dcp_is_ext26; (void)dcp_has_mode;
 struct drm_crtc_state cs = {.active=true,.mode.id=1};
 struct drm_crtc crtc = {.state=&cs};
 typeof(((struct apple_connector *)0)->base.state[0]) state = {.crtc=&crtc};
 struct apple_connector c = {.base.state=&state};
 struct apple_dcp d = {.native=true,.admitted=true}; current=&d;
 transition=1;
 assert(dcp_retrain_active_crtc(&c,&d,false)==0);
 assert(!resets && !toggles && !releases);
 transition=0; d.admitted=true; /* fresh catalogue READY must still retrain */
 assert(!dcp_retrain_active_crtc(&c,&d,false) && resets==1);
 d.mode_state.valid=true; /* prior commit already completed the work */
 assert(!dcp_retrain_active_crtc(&c,&d,false) && resets==1);
 d.native=false; d.mode_state.valid=false;
 assert(!dcp_retrain_active_crtc(&c,&d,false) && resets==2);
 assert(!dcp_retrain_active_crtc(&c,&d,true) && toggles==2);
 d.native=true; d.admitted=false; lost=true;
 assert(!dcp_retrain_active_crtc(&c,&d,false) && releases==1);
}
'''
with tempfile.TemporaryDirectory() as t:
    p = Path(t) / 'retrain.c'
    p.write_text(pre + body + main)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror',
                    '-Wno-unused-variable', '-fsanitize=address,undefined',
                    str(p), '-o', str(p.with_suffix(''))], check=True)
    result = subprocess.run([str(p.with_suffix(''))])
    if result.returncode:
        sys.exit(1)
print('PASS native26 retrain: lock-wait layout restart, fresh READY, completed commit, classic reset/power cycle')
