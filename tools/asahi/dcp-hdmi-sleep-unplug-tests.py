#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the real HDMI sleep keep-link and the unplugged-in-sleep release.

A DCP whose firmware runs through a sleep keeps an attached HDMI display's
link (dcp_platform_suspend()), so the display lights again on wake. Its HPD
IRQ is masked through the sleep, so a display unplugged during it makes no
edge anyone sees: dcp_hdmi_unplugged_in_sleep() releases it on resume, before
the IRQ comes back. A T6030 native output still releases the link at suspend,
because its resume sets the PHY and crossbar up again.

The fixture compiles the HDMI block of dcp_platform_suspend() and the
fabric's dcp_hdmi_release_unplugged_locked(), dcp_hdmi_suspend() and
dcp_hdmi_unplugged_in_sleep() against mocks. It checks which outputs are
released, that negative GPIO errors are not treated as unplugged, that
disable_irq() runs outside dcp_typec_fabric_lock, and that the fabric lock
covers both suspend and resume releases. --mutation names a change to the
production code that the matrix must kill.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DCP = 'drivers/gpu/drm/apple/dcp.c'
FABRIC = 'drivers/gpu/drm/apple/dcp-fabric.c'
RESUME_CALL = '\tif (dcp->hdmi_hpd_irq)\n\t\tdcp_hdmi_unplugged_in_sleep(dcp);\n'

# (target, old, new) where target is 'dcp', 'release', 'suspend', or 'resume'
MUTATIONS = {
    # Keep a T6030 native output's link, which its resume reinit tears down.
    'keep-native': ('suspend',
                    '\tif (dcp->external_native) {\n',
                    '\tif (false && dcp->external_native) {\n'),
    # Release on a failed GPIO read, which cannot establish absence.
    'gpio-error': ('release',
                   'gpiod_get_value_cansleep(dcp->hdmi_hpd) != 0)',
                   'gpiod_get_value_cansleep(dcp->hdmi_hpd) > 0)'),
    # Release the HDMI connector while a Type-C display owns the pipeline.
    'ignore-borrowed': ('release',
                        '\tif (dcp->active_typec_route)\n\t\treturn;\n',
                        ''),
    # Release a borrowed T6030 native pipeline at suspend.
    'suspend-ignore-borrowed': ('suspend',
                                '\tif (dcp->active_typec_route)\n\t\treturn;\n',
                                ''),
    # Leave the hybrid fabric thinking HDMI is still there.
    'no-fabric': ('release',
                  '\tdcp_fabric_hdmi_unplugged_locked(dcp);\n',
                  ''),
    # Leave the hybrid fabric stale when an unplugged T6030 native HDMI suspends.
    'no-native-unplug-fabric': ('suspend',
                                '\t\tif (unplugged)\n\t\t\tdcp_fabric_hdmi_unplugged_locked(dcp);\n',
                                '\t\t(void)unplugged;\n'),
    # Treat a GPIO error as unplugged on a T6030 native output at suspend.
    'native-unplug-gpio-error': ('suspend',
                                 'gpiod_get_value_cansleep(dcp->hdmi_hpd) == 0;',
                                 'gpiod_get_value_cansleep(dcp->hdmi_hpd) <= 0;'),
    # Drop the fabric lock on resume.
    'no-fabric-lock': ('resume',
                       '\tguard(mutex)(&dcp_typec_fabric_lock);\n',
                       ''),
    # Drop the fabric lock on suspend.
    'no-suspend-fabric-lock': ('suspend',
                               '\tguard(mutex)(&dcp_typec_fabric_lock);\n',
                               ''),
    # Never look for an unplug after the sleep.
    'no-resume-check': ('dcp', RESUME_CALL, ''),
    # Never release an unplugged or native HDMI link at suspend.
    'no-suspend-check': ('dcp', '\t\tdcp_hdmi_suspend(dcp);\n', ''),
    # Leave a stale DPTX link connected when HPD is low without a DRM connector.
    'no-stale-dptx-release': ('release',
                              '\tif (!connected) {\n\t\tdcp_dptx_disconnect(dcp, 0);\n\t\treturn;\n\t}\n',
                              '\tif (!connected)\n\t\treturn;\n'),
}

def block(text, start):
    opening = text.index('{', start)
    depth = 0
    for end in range(opening, len(text)):
        depth += text[end] == '{'
        depth -= text[end] == '}'
        if not depth:
            return text[start:end + 1]
    raise AssertionError('unbalanced production block')

def function(text, name):
    match = re.search(r'\n((?:static )?[^\n;]*\b' + name + r'\([^;{}]*\)\n)\{', text)
    assert match, name
    return block(text, match.start(1))

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define READ_ONCE(x) (x)
#define dev_info(...) ((void)0)
struct device { int unused; };
struct gpio_desc { int level; };
struct apple_connector { bool connected; };
struct dptx_port { bool connected; };
struct apple_dcp {
	struct device *dev;
	void *active_typec_route;
	struct apple_connector *connector;
	struct dptx_port dptxport[1];
	struct gpio_desc *hdmi_hpd;
	int hdmi_hpd_irq;
	bool external_native;
};
static unsigned hpd_events, disconnects, fabric_unplugs, irq_off;
static int dcp_typec_fabric_lock;
static int gpiod_get_value_cansleep(struct gpio_desc *g) { return g->level; }
static void disable_irq(int irq)
{
	assert(!dcp_typec_fabric_lock);
	irq_off++;
}
static void disconnected_hpd_event(struct apple_connector *c)
{
	assert(dcp_typec_fabric_lock);
	if (c)
		c->connected = false;
	hpd_events++;
}
static int dcp_dptx_disconnect(struct apple_dcp *d, int port)
{
	assert(dcp_typec_fabric_lock);
	d->dptxport[port].connected = false;
	disconnects++;
	return 0;
}
static int *hold_lock(int *lock)
{
	assert(!*lock);
	*lock = 1;
	return lock;
}
static void release_lock(int **lock) { **lock = 0; }
#define guard(kind) int *held_##kind __attribute__((cleanup(release_lock))) = hold_lock
#define lockdep_assert_held(lock) assert(*(lock))
static void dcp_hdmi_disconnect(struct apple_dcp *dcp)
{
	assert(dcp_typec_fabric_lock);
	disconnected_hpd_event(dcp->connector);
	dcp_dptx_disconnect(dcp, 0);
}
static void dcp_fabric_hdmi_unplugged_locked(struct apple_dcp *d)
{
	assert(dcp_typec_fabric_lock);
	fabric_unplugs++;
}
static void reset(void) { hpd_events = disconnects = fabric_unplugs = irq_off = 0; }
'''

CHECKS = r'''
static void suspend_hdmi(struct apple_dcp *dcp) { SUSPEND }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d (%s: %s)\n", __LINE__, case_name, #c); failures++; } } while (0)

int main(void)
{
	const char *case_name;
	struct apple_connector con;
	struct gpio_desc hpd;
	struct apple_dcp dcp;
	int route;

#define SETUP(name, L, N) do { case_name = name; reset(); con.connected = true; \
	hpd.level = L; dcp = (struct apple_dcp){ .connector = &con, \
	.dptxport = { { .connected = true } }, .hdmi_hpd = &hpd, \
	.hdmi_hpd_irq = 7, .external_native = N }; } while (0)

	/* Attached through the sleep: kept, lit again on wake. */
	SETUP("kept", 1, false);
	suspend_hdmi(&dcp);
	CHECK(irq_off == 1 && con.connected && !disconnects && !fabric_unplugs &&
	      !dcp_typec_fabric_lock);
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(con.connected && !disconnects && !fabric_unplugs && !dcp_typec_fabric_lock);

	/* Unplugged during the sleep: released on resume, fabric told. */
	SETUP("unplugged-in-sleep", 1, false);
	suspend_hdmi(&dcp);
	hpd.level = 0;
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(!con.connected && hpd_events == 1 && disconnects == 1 && fabric_unplugs == 1);

	/* Gone before the sleep: released at suspend and fabric told, nothing on resume. */
	SETUP("gone-before", 0, false);
	suspend_hdmi(&dcp);
	CHECK(!con.connected && hpd_events == 1 && disconnects == 1 && fabric_unplugs == 1);
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(disconnects == 1 && hpd_events == 1 && fabric_unplugs == 1);

	/* Already disconnected and idle: untouched, no spurious fabric unplug hold. */
	SETUP("already-disconnected", 0, false);
	con.connected = false;
	dcp.dptxport[0].connected = false;
	suspend_hdmi(&dcp);
	CHECK(!con.connected && !hpd_events && !disconnects && !fabric_unplugs);
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(!hpd_events && !disconnects && !fabric_unplugs);

	/* Stale DPTX link without a connected DRM connector: port released, no fabric unplug. */
	SETUP("stale-dptx", 0, false);
	con.connected = false;
	suspend_hdmi(&dcp);
	CHECK(!dcp.dptxport[0].connected && !hpd_events && disconnects == 1 && !fabric_unplugs);
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(disconnects == 1 && !hpd_events && !fabric_unplugs);

	/* A failed GPIO read at suspend cannot establish absence. */
	SETUP("suspend-gpio-error", -5, false);
	suspend_hdmi(&dcp);
	CHECK(irq_off == 1 && con.connected && !hpd_events && !disconnects && !fabric_unplugs);
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(con.connected && !hpd_events && !disconnects && !fabric_unplugs);

	/* A failed GPIO read at resume cannot establish absence. */
	SETUP("gpio-error", 1, false);
	suspend_hdmi(&dcp);
	hpd.level = -5;
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(con.connected && !disconnects && !fabric_unplugs);

	/* A Type-C display owns the pipeline: the HDMI side is not touched. */
	SETUP("borrowed", 0, false);
	dcp.active_typec_route = &route;
	suspend_hdmi(&dcp);
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(con.connected && !disconnects && !hpd_events && !fabric_unplugs);

	/* A Type-C display owns a T6030 native pipeline: HDMI suspend leaves it alone. */
	SETUP("borrowed-native", 1, true);
	dcp.active_typec_route = &route;
	suspend_hdmi(&dcp);
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(con.connected && !disconnects && !hpd_events && !fabric_unplugs);

	/* T6030 native output attached: released at suspend for reinit, not marked unplugged. */
	SETUP("native", 1, true);
	suspend_hdmi(&dcp);
	CHECK(!con.connected && disconnects == 1 && !fabric_unplugs);
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(disconnects == 1 && !fabric_unplugs);

	/* T6030 native output unplugged before sleep: released and fabric told at suspend. */
	SETUP("native-unplugged", 0, true);
	suspend_hdmi(&dcp);
	CHECK(!con.connected && disconnects == 1 && fabric_unplugs == 1);
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(disconnects == 1 && fabric_unplugs == 1);

	/* T6030 native output with GPIO error: released for reinit, not marked unplugged. */
	SETUP("native-gpio-error", -5, true);
	suspend_hdmi(&dcp);
	CHECK(!con.connected && disconnects == 1 && !fabric_unplugs);
	dcp_hdmi_unplugged_in_sleep(&dcp);
	CHECK(disconnects == 1 && !fabric_unplugs);

	if (failures)
		return 1;
	puts("PASS HDMI sleep: attached kept, unplugged-in-sleep released on resume, "
	     "gone-before released and fabric notified at suspend, GPIO errors and borrowed "
	     "pipelines untouched, T6030 native released");
	return 0;
}
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()

    source = (ROOT / DCP).read_text()
    fabric = (ROOT / FABRIC).read_text()
    parts = {
        'dcp': source,
        'release': function(fabric, 'dcp_hdmi_release_unplugged_locked'),
        'suspend': function(fabric, 'dcp_hdmi_suspend'),
        'resume': function(fabric, 'dcp_hdmi_unplugged_in_sleep'),
    }
    if args.mutation:
        target, old, new = MUTATIONS[args.mutation]
        assert parts[target].count(old) == 1, args.mutation
        parts[target] = parts[target].replace(old, new)

    resume = function(parts['dcp'], 'dcp_platform_resume')
    # The release runs while the HPD IRQ is still masked: an edge from then
    # on is replayed when dcp_fabric_run_resume() enables it.
    if RESUME_CALL not in resume or \
       resume.index(RESUME_CALL) > resume.index('dcp_fabric_run_resume(&dcp_resume_ops, dcp)'):
        print('FAIL line 0 (resume: no unplugged-in-sleep release before the HPD IRQ comes back)')
        sys.exit(1)

    suspend = function(parts['dcp'], 'dcp_platform_suspend')
    hdmi = block(suspend, suspend.index('\tif (dcp->hdmi_hpd_irq) {\n\t\tdisable_irq'))
    code = (HARNESS + parts['release'] + '\n' + parts['suspend'] + '\n' +
            parts['resume'] + CHECKS.replace('SUSPEND', hdmi))

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'check.c'
        path.write_text(code)
        subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                        '-fsanitize=address,undefined', str(path), '-o', str(path.with_suffix(''))],
                       check=True)
        sys.exit(subprocess.run([str(path.with_suffix(''))]).returncode)

if __name__ == '__main__':
    main()
