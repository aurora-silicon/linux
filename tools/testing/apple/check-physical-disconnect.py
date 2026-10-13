#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Execute the driver functions with mocked hardware and work scheduling.

Run from the kernel tree; --source may select a parent tree for negative control.
This tests logical notifications and retained ownership, not hardware completion.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--cc', default='clang')
a = p.parse_args()
r = a.source or Path(__file__).resolve().parents[3]
a.output.mkdir(parents=True, exist_ok=True)

def function(file, name):
    s = (r / file).read_text()
    m = re.search(r'^[\w* \t]+\b' + name + r'\([^;{}]*\)\s*\{', s, re.M)
    assert m, name
    pos = m.end()
    depth = 1
    while depth:
        depth += (s[pos] == '{') - (s[pos] == '}')
        pos += 1
    return s[m.start():pos]

pm = (r / 'drivers/usb/typec/tipd/cd321x-pm.h').read_text()
pm = pm.replace('#include <linux/types.h>', '')
core = 'drivers/usb/typec/tipd/core.c'
dcp = 'drivers/gpu/drm/apple/dcp.c'
funcs = '\n'.join(function(f, n) for f, n in (
    ('drivers/gpu/drm/apple/iomfb.c', 'iomfb_pause_presentation'),
    (dcp, 'dcp_external_report_disconnected'),
    (dcp, 'dcp_external_publish_generation'),
    (dcp, 'dcp_external_stop_locked'),
    (dcp, 'dcp_external_disconnect'),
    (dcp, 'dcp_external_stop_work'),
    (dcp, 'dcp_external_hotplug'),
    (dcp, 'dcp_external_link_locked'),
    (dcp, 'dcp_dptx_connect_oob'),
    (core, 'cd321x_retry_revalidation'),
    (core, 'cd321x_update_work')))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64;
#define guard(x) fake_guard
#define fake_guard(x) ((void)(x))
#define scoped_guard(x,y) for (int once = ((void)(y), 1); once; once = 0)
#define lockdep_assert_held(x) ((void)(x))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x) = (v))
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define to_delayed_work(w) container_of(w,struct delayed_work,work)
#define msecs_to_jiffies(x) (x)
#define IS_REACHABLE(x) 1
#define CONFIG_DRM 1
#define IS_ERR(x) false
#define PTR_ERR(x) (-EINVAL)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define CD321X_DEBOUNCE_DELAY_MS 500
#define TPS_STATUS_PLUG_PRESENT (1U << 0)
#define TPS_STATUS_PLUG_UPSIDE_DOWN (1U << 1)
#define TPS_DATA_STATUS_USB2_CONNECTION (1U << 0)
#define TPS_DATA_STATUS_USB3_CONNECTION (1U << 1)
#define TPS_DATA_STATUS_TBT_CONNECTION (1U << 2)
#define TPS_DATA_STATUS_DP_CONNECTION (1U << 3)
#define TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK (3U << 4)
#define TPS_DATA_STATUS_USB_DATA_ROLE (1U << 6)
#define CD321X_DATA_STATUS_USB4_CONNECTION (1U << 7)
#define CD321X_DATA_STATUS_HPD_LEVEL (1U << 8)
#define TPS_POWER_STATUS_PWROPMODE(x) (x)
#define TPS_STATUS_TO_UPSIDE_DOWN(x) ((x)&TPS_STATUS_PLUG_UPSIDE_DOWN)
#define TPS_STATUS_TO_TYPEC_PORTROLE(x) 0
#define TPS_STATUS_TO_TYPEC_VCONN(x) 0
#define TPS_STATUS_TO_TYPEC_DATAROLE(x) 0
#define TYPEC_STATE_SAFE 0
#define TYPEC_STATE_USB 1
#define TYPEC_THUNDERBOLT_SWITCH_OFF 0
#define TYPEC_ACCESSORY_NONE 0
#define DCP_HOTPLUG_NOTIFY 1
struct work_struct { bool pending; };
struct delayed_work { struct work_struct work; };
static int system_freezable_wq;
static int schedule_work(struct work_struct *w) { w->pending = true; return 1; }
static void cancel_work_sync(struct work_struct *w) { w->pending = false; }
static void cancel_delayed_work_sync(struct delayed_work *w) { w->work.pending = false; }
#define cancel_delayed_work cancel_delayed_work_sync
static void mod_delayed_work(int q, struct delayed_work *w, int t)
{ (void)q; (void)t; w->work.pending = true; }
struct apple_connector { bool connected, edid_invalidation_pending; };
struct apple_dcp {
 int external_lock, hpd_mutex;
 struct { int lock; bool presentation_paused; } iomfb;
 struct { bool connected; void *service; } dptxport[1];
 struct apple_connector *connector;
 struct delayed_work typec_reconnect_wq, placeholder_edid_wq;
 struct work_struct external_stop_work;
 bool external_running, external_stopped, external_power_attempted;
 bool external_cable, typec_cable_connected, typec_crtc_off, pipe_enabled_h17p;
 bool external_physically_absent, external_prepared, placeholder_retried;
 int typec_reconnect_tries;
 bool external_hotplug_pending, external_hotplug_connected, quiescing;
 u64 external_generation, external_rx_generation, external_crtc_generation;
 u64 external_stop_generation, typec_generation;
 int external_error, mode_state; void *avep;
};
struct platform_device { struct apple_dcp *data; };
static struct apple_dcp *platform_get_drvdata(struct platform_device *p) { return p->data; }
static int raw_error, raw_links;
static int dcp_dptx_connect_raw(struct apple_dcp *d, u32 port, bool *waiting)
{ assert(port == 0); *waiting = false; raw_links++;
 if (!raw_error) d->dptxport[0].connected = true;
 return raw_error; }
static bool dcp_is_typec_output(struct apple_dcp *d) { (void)d; return true; }
static int dcp_dptx_connect(struct apple_dcp *d, u32 port) { (void)d; (void)port; return 0; }
#define DPTX_RECONNECT_DELAY 500
static int settle_error, power_error, hpd_error, release_error;
static int settles, powers, releases, detaches, notices;
static bool dcp_is_external_h17p(struct apple_dcp *d) { (void)d; return true; }
static unsigned dcp_mode_hotplug(int *mode, bool connected, bool *sink)
{ (void)mode; if (sink) *sink = connected; return 1; }
static void dcp_handle_hotplug_actions(struct apple_dcp *d, unsigned act)
{ (void)d; notices += !!act; }
static int iomfb_queue_settle(struct apple_dcp *d, int t)
{ (void)d; (void)t; settles++; return settle_error; }
static int iomfb_external_power_h17p(struct apple_dcp *d, bool on)
{ (void)d; assert(!on); powers++; return power_error; }
static bool dptx_service_available(void *p) { (void)p; return true; }
static int dptxport_set_hpd(void *p, bool on)
{ (void)p; assert(!on); return hpd_error; }
static int dptxport_release_display(void *p)
{ (void)p; releases++; return release_error; }
static void av_service_disconnect(struct apple_dcp *d) { (void)d; }
static void dpavservep_detach(struct apple_dcp *d) { (void)d; detaches++; }
static void dcp_firmware_metadata_connection(struct apple_dcp *d, u64 g, bool c)
{ (void)d; (void)g; (void)c; }
static int dcp_external_disconnect(struct apple_dcp *, bool);
static struct apple_dcp display;
static void drm_connector_oob_hotplug_event(void *node, int status)
{ assert(node); if (!status) dcp_external_disconnect(&display, true); }
#define connector_status_disconnected 0
#define connector_status_connected 1
'''
shim = r'''
enum usb_role { USB_ROLE_NONE, USB_ROLE_HOST, USB_ROLE_DEVICE };
enum typec_pwr_opmode { TYPEC_PWR_MODE_USB, TYPEC_PWR_MODE_PD };
enum typec_orientation { TYPEC_ORIENTATION_NONE, TYPEC_ORIENTATION_NORMAL, TYPEC_ORIENTATION_REVERSE };
struct usb_pd_identity { int id; };
struct typec_mux_state { void *alt, *data; int mode; };
struct typec_thunderbolt_switch_data { int state; };
struct typec_partner_desc { bool usb_pd; int accessory; struct usb_pd_identity *identity; };
struct cd321x_status { u32 status, data_status, status_changed, data_status_changed, pwr_status;
 struct usb_pd_identity partner_identity; };
struct tps6598x { void *partner, *dev, *port, *psy; int lock; u32 data_status; enum usb_role *role_sw; };
struct cd321x { struct tps6598x tps; struct cd321x_status update_status;
 struct cd321x_pm_state pm; struct delayed_work update_work, resume_work;
 struct typec_mux_state state; bool state_valid, display_route_active, cur_partner_is_pd;
 struct usb_pd_identity cur_partner_identity; void *connector_fwnode, *tbt_switch; };
static int tbt_error, role_error, safe_error, mode_error;
static int tbts, roles, safes;
static enum usb_role usb_role_switch_get_role(enum usb_role *r) { return *r; }
static int usb_role_switch_set_role(enum usb_role *r, enum usb_role n)
{ roles++; if (role_error) return role_error; *r = n; return 0; }
static int typec_thunderbolt_switch_set(void *p, void *s)
{ (void)p; (void)s; tbts++; return tbt_error; }
static int typec_set_mode(void *p, int s)
{ (void)p; assert(s == TYPEC_STATE_SAFE); safes++; return safe_error; }
static int cd321x_typec_update_mode(struct tps6598x *t, struct cd321x_status *s, bool *busy)
{ (void)t; (void)s; *busy = mode_error == -EBUSY; return mode_error; }
static void typec_unregister_partner(void *p) { (void)p; }
static void *typec_register_partner(void *p, struct typec_partner_desc *d)
{ (void)p; (void)d; return (void *)1; }
static void typec_partner_set_identity(void *p) { (void)p; }
#define typec_set_pwr_opmode(...) ((void)0)
#define typec_set_pwr_role(...) ((void)0)
#define typec_set_vconn_role(...) ((void)0)
#define typec_set_orientation(...) ((void)0)
#define typec_set_data_role(...) ((void)0)
#define power_supply_changed(...) ((void)0)
'''
tests = r'''
static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
static struct apple_connector sink;
static enum usb_role role;
static struct cd321x port;
static void init(void)
{
 memset(&display,0,sizeof(display)); memset(&sink,0,sizeof(sink)); memset(&port,0,sizeof(port));
 sink.connected = true; display.connector = &sink; display.external_prepared = true;
 raw_error = raw_links = 0;
 display.external_running = display.external_power_attempted = true;
 display.external_cable = display.typec_cable_connected = true;
 display.dptxport[0].connected = display.pipe_enabled_h17p = true;
 display.external_generation = display.external_rx_generation = display.external_crtc_generation = 7;
 display.typec_generation = 4;
 port.tps.partner = (void *)1; port.tps.role_sw = &role; role = USB_ROLE_HOST;
 port.pm.phase = CD321X_PM_RUNNING; port.state_valid = true; port.state.mode = 2;
 port.display_route_active = true; port.connector_fwnode = (void *)1;
 settle_error = power_error = hpd_error = release_error = 0;
 tbt_error = role_error = safe_error = mode_error = 0;
 settles = powers = releases = detaches = notices = tbts = roles = safes = 0;
}
static void callback(bool connected)
{
 if (!dcp_external_hotplug(&display,connected))
  dcp_mode_hotplug(&display.mode_state,connected,&sink.connected);
}
int main(void)
{
 /* Unplug at each fallible transition with failing DCP power teardown. */
 for (int which = 0; which < 3; which++) {
  init(); power_error = -ETIMEDOUT;
  if (which == 0) tbt_error = -ETIMEDOUT;
  if (which == 1) role_error = -EIO;
  if (which == 2) safe_error = -ETIMEDOUT;
  cd321x_update_work(&port.update_work.work);
  CHECK(!sink.connected); CHECK(!display.external_cable);
  CHECK(display.external_error == -ETIMEDOUT);
  CHECK(display.dptxport[0].connected && display.pipe_enabled_h17p);
  CHECK(!display.external_stopped && !display.typec_crtc_off);
  CHECK(port.display_route_active && !port.state_valid);
  CHECK(port.pm.phase == CD321X_PM_REVALIDATE && port.pm.attempts_left == CD321X_RESUME_ATTEMPTS);
  CHECK(notices > 0 && releases == 0);
  callback(true); CHECK(!sink.connected);
  printf("case transition-%d complete\n",which);
 }
 /* A rejected/nonowner port cannot disconnect the shared owner's sink. */
 for (int which = 0; which < 3; which++) {
  init(); port.display_route_active = false;
  if (which == 0) tbt_error = -EIO;
  if (which == 1) role_error = -EIO;
  if (which == 2) safe_error = -EIO;
  cd321x_update_work(&port.update_work.work);
  CHECK(sink.connected && display.external_cable && !notices);
 }
 init(); port.display_route_active = false; role = USB_ROLE_NONE;
 port.update_status.status = TPS_STATUS_PLUG_PRESENT;
 port.update_status.data_status = TPS_DATA_STATUS_DP_CONNECTION | CD321X_DATA_STATUS_HPD_LEVEL;
 mode_error = -EBUSY; cd321x_update_work(&port.update_work.work);
 CHECK(sink.connected && !notices && !port.display_route_active);
 /* Two ports share the connector: busy-port unplug leaves its owner live. */
 init();
 struct cd321x second = port;
 enum usb_role second_role = USB_ROLE_NONE;
 second.tps.role_sw = &second_role; second.display_route_active = false;
 second.update_status.status = TPS_STATUS_PLUG_PRESENT;
 second.update_status.data_status = TPS_DATA_STATUS_DP_CONNECTION | CD321X_DATA_STATUS_HPD_LEVEL;
 mode_error = -EBUSY; cd321x_update_work(&second.update_work.work);
 CHECK(!second.display_route_active && sink.connected && !notices);
 second.update_status.status = second.update_status.data_status = 0;
 tbt_error = -EIO; cd321x_update_work(&second.update_work.work);
 CHECK(port.display_route_active && sink.connected && !notices);
 power_error = -EIO; cd321x_update_work(&port.update_work.work);
 CHECK(!sink.connected && port.display_route_active);
 CHECK(display.dptxport[0].connected && display.pipe_enabled_h17p);
 /* Revalidation is bounded even though the failed hardware remains owned. */
 init(); tbt_error = -EIO;
 cd321x_update_work(&port.update_work.work);
 for (int i = 0; i < CD321X_RESUME_ATTEMPTS; i++) {
  CHECK(cd321x_pm_begin_read(&port.pm)); cd321x_pm_snapshot_ready(&port.pm);
  cd321x_update_work(&port.update_work.work);
 }
 CHECK(port.pm.phase == CD321X_PM_STALE && !port.pm.attempts_left);
 CHECK(!sink.connected && port.display_route_active);
 /* Stop failure stages preserve all state whose shutdown was not acked. */
 for (int which = 0; which < 4; which++) {
  init();
  if (which == 0) settle_error = -EIO;
  if (which == 1) power_error = -EIO;
  if (which == 2) hpd_error = -EIO;
  if (which == 3) release_error = -EIO;
  CHECK(dcp_external_disconnect(&display,true) == -EIO);
  CHECK(!sink.connected && !display.typec_cable_connected);
  CHECK(display.external_generation == 8 && display.external_rx_generation == 8);
  CHECK(display.external_error == -EIO && display.dptxport[0].connected);
  CHECK(!display.external_stopped && !display.typec_crtc_off);
  CHECK(display.pipe_enabled_h17p == (which < 2));
  callback(true); CHECK(!sink.connected);
 }
 /* An existing error returns before pause: late RX still cannot resurrect. */
 init(); display.external_error = -EIO;
 CHECK(dcp_external_disconnect(&display,true) == -EIO);
 CHECK(!display.iomfb.presentation_paused);
 callback(true); CHECK(!sink.connected);
 CHECK(display.dptxport[0].connected && display.pipe_enabled_h17p);
 /* A queued callback from the old sink cannot be replayed after unplug. */
 init(); display.external_error = -EIO;
 display.iomfb.presentation_paused = true; callback(true);
 CHECK(display.external_hotplug_pending);
 CHECK(dcp_external_disconnect(&display,true) == -EIO);
 iomfb_pause_presentation(&display,false); CHECK(!sink.connected);
 /* An old stop generation cannot touch a later session. */
 init(); display.external_stop_generation = 6;
 dcp_external_stop_work(&display.external_stop_work);
 CHECK(sink.connected && settles == 0 && powers == 0 && releases == 0);
 CHECK(dcp_external_stop_locked(&display,6) == 0 && sink.connected);
 /* Same-cable CRTC off remains distinct from physical absence. */
 init(); CHECK(dcp_external_disconnect(&display,false) == 0);
 CHECK(display.external_cable && display.typec_cable_connected);
 CHECK(display.external_generation == 7);
 /* Genuine discovery and the paused DPMS generation gate still work. */
 init(); sink.connected = false; callback(true); CHECK(sink.connected);
 init(); sink.connected = false; display.iomfb.presentation_paused = true;
 display.external_power_attempted = false; callback(true); CHECK(sink.connected);
 init(); sink.connected = false; display.iomfb.presentation_paused = true;
 display.external_stopped = true; display.external_rx_generation = 8;
 callback(true); CHECK(sink.connected);
 init(); sink.connected = false; display.iomfb.presentation_paused = true;
 display.external_stopped = true; callback(true); CHECK(!sink.connected);
 init(); sink.connected = false; display.iomfb.presentation_paused = true;
 display.external_error = -EIO; callback(true); CHECK(!sink.connected);
 /* Fresh firmware discovery precedes OOB: no second RX is required. */
 init(); sink.connected = false;
 display.external_cable = display.typec_cable_connected = false;
 display.external_generation = display.external_rx_generation = 37;
 display.external_power_attempted = false; display.external_stopped = true;
 display.dptxport[0].connected = false;
 callback(true); CHECK(sink.connected); CHECK(!display.external_physically_absent);
 struct platform_device device = { .data = &display };
 CHECK(dcp_dptx_connect_oob(&device,0) == 0);
 CHECK(raw_links == 1 && sink.connected);
 CHECK(display.external_generation == 38);
 /* Initially paused discovery keeps the original pending-replay behavior. */
 init(); sink.connected = false; display.external_cable = false;
 display.external_power_attempted = false; display.iomfb.presentation_paused = true;
 callback(true); CHECK(display.external_hotplug_pending); CHECK(!sink.connected);
 CHECK(dcp_dptx_connect_oob(&device,0) == 0 && raw_links == 1);
 iomfb_pause_presentation(&display,false); CHECK(sink.connected);
 /* Physical absence consumes late RX and clears an already queued replay. */
 init(); display.iomfb.presentation_paused = true; callback(true);
 CHECK(display.external_hotplug_pending);
 CHECK(dcp_external_disconnect(&display,true) == 0);
 CHECK(display.external_physically_absent && !display.external_hotplug_pending);
 callback(true); CHECK(!sink.connected && !display.external_hotplug_pending);
 /* A real next OOB connect removes the tombstone and permits discovery. */
 CHECK(dcp_dptx_connect_oob(&device,0) == 0);
 CHECK(!display.external_physically_absent && display.external_cable);
 CHECK(display.external_generation == 9 && raw_links == 1);
 callback(true); CHECK(sink.connected);
 /* Same-cable DPMS does not create a physical-absence tombstone. */
 init(); CHECK(dcp_external_disconnect(&display,false) == 0);
 CHECK(!display.external_physically_absent && display.external_cable);
 sink.connected = false; callback(true); CHECK(!sink.connected);
 CHECK(display.external_hotplug_pending);
 iomfb_pause_presentation(&display,false); CHECK(sink.connected);
 /* A failed hardware stop remains owned, even after a real OOB reconnect. */
 init(); display.external_error = -EIO;
 CHECK(dcp_external_disconnect(&display,true) == -EIO);
 CHECK(display.external_physically_absent); callback(true); CHECK(!sink.connected);
 CHECK(dcp_dptx_connect_oob(&device,0) == -EIO);
 CHECK(!display.external_physically_absent && display.external_error == -EIO);
 CHECK(raw_links == 0 && display.dptxport[0].connected && display.pipe_enabled_h17p);
 /* Successful physical teardown may release ownership normally. */
 init(); CHECK(dcp_external_disconnect(&display,true) == 0);
 CHECK(!sink.connected && !display.dptxport[0].connected);
 CHECK(display.external_stopped && display.typec_crtc_off && !display.pipe_enabled_h17p);
 printf("%d assertions, %d failures\n", checks, failures); return !!failures;
}
'''
source = prefix + pm + shim + funcs + tests
c = a.output / 'physical-disconnect.c'
c.write_text(source)
exe = a.output / 'physical-disconnect'
subprocess.run([a.cc, '-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-variable',
                '-Wno-unused-function', '-Wno-unused-but-set-variable', '-fsanitize=address,undefined', '-g',
                str(c), '-o', str(exe)], check=True)
result = subprocess.run([str(exe)], capture_output=True, text=True)
(a.output / 'result.log').write_text(result.stdout + result.stderr)
(a.output / 'result.json').write_text(json.dumps({
    'source': str(r), 'extracted_functions_sha256': hashlib.sha256(funcs.encode()).hexdigest(),
    'exit_code': result.returncode, 'output': result.stdout + result.stderr,
    'limitations': ['Kernel locks/work queues and hardware RPCs are mocked; no hardware qualification.']
}, indent=2) + '\n')
print(result.stdout + result.stderr, end='')
raise SystemExit(result.returncode)
