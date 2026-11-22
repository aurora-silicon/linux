#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute the production desktop 26.x external pipe re-power of dcp_flush().

A display whose link came back under a CRTC that userspace kept active gets
its pipe powered and its mode set from the next flush, but only once the
display lists that mode again. Kernel services are controlled host stubs.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


def function(source, name):
    match = re.search(r'^(?:static )?(?:inline )?(?:void|bool|int) ' + re.escape(name) +
                      r'\([^;]*?\)\n\{', source, re.M)
    if not match:
        raise RuntimeError('missing production function: ' + name)
    begin = source.index('{', match.start())
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define READ_ONCE(x) (x)
#define DRM_MODE_FMT "%d"
#define DRM_MODE_ARG(m) ((m)->hdisplay)
#define dev_dbg(dev, ...) ((void)(dev))
enum { DCP_FIRMWARE_V_26_6, DCP_FIRMWARE_V_26_6_DESKTOP, DCP_FIRMWARE_V_27_0 };
struct drm_display_mode { int hdisplay; };
struct drm_crtc_state { bool active; struct drm_display_mode mode; };
struct drm_crtc { int unused; };
struct drm_atomic_commit { struct drm_crtc_state *crtc_state; };
struct platform_device { int unused; };
struct apple_connector { bool connected; };
struct dcp_desktop_state { bool ext_pipe_off; };
struct apple_dcp {
	int fw_compat;
	bool main_display, listed;
	struct { bool valid; } mode_state;
	struct dcp_desktop_state *desktop;
	struct apple_connector *connector;
	void *dev;
};
struct apple_crtc { struct drm_crtc base; struct platform_device *dcp; };
static struct apple_crtc test_crtc;
static struct apple_dcp *test_dcp;
static int powerons, modesets;
static struct apple_crtc *to_apple_crtc(struct drm_crtc *crtc)
{
	assert(crtc == &test_crtc.base);
	return &test_crtc;
}
static struct drm_crtc_state *drm_atomic_get_new_crtc_state(struct drm_atomic_commit *c,
							     struct drm_crtc *crtc)
{
	(void)crtc;
	return c->crtc_state;
}
/* The display's mode list: lookup_mode() of the CRTC's mode. */
static bool dcp_mode_settable(struct apple_dcp *dcp, const struct drm_display_mode *mode)
{
	(void)mode;
	return dcp->listed;
}
static void dcp_poweron(struct platform_device *pdev)
{
	assert(pdev == test_crtc.dcp);
	powerons++;
}
/* A mode the display does not list fails here with "no match". */
static int dcp_crtc_atomic_modeset(struct drm_crtc *crtc, struct drm_atomic_commit *c)
{
	(void)crtc;
	(void)c;
	assert(test_dcp->listed);
	modesets++;
	return 0;
}
'''

TESTS = r'''
static struct dcp_desktop_state desktop;
static struct apple_connector connector;
static struct drm_crtc_state crtc_state;
static struct drm_atomic_commit commit = { &crtc_state };
static struct platform_device pdev;
static struct apple_dcp dcp;

static void fresh(void)
{
	desktop = (struct dcp_desktop_state){ .ext_pipe_off = true };
	connector = (struct apple_connector){ .connected = true };
	crtc_state = (struct drm_crtc_state){ .active = true, .mode = { .hdisplay = 3840 } };
	dcp = (struct apple_dcp){ .fw_compat = DCP_FIRMWARE_V_26_6_DESKTOP, .listed = true,
				  .desktop = &desktop, .connector = &connector };
	test_crtc.dcp = &pdev;
	test_dcp = &dcp;
	powerons = modesets = 0;
}

int main(void)
{
	/* Link back, mode listed: the pipe is powered and the mode set. */
	fresh();
	dcp_desktop_ext_repower(&dcp, &test_crtc.base, &commit);
	assert(powerons == 1 && modesets == 1);

	/* Link back before the display lists the CRTC's mode: nothing yet. */
	fresh();
	dcp.listed = false;
	dcp_desktop_ext_repower(&dcp, &test_crtc.base, &commit);
	assert(powerons == 0 && modesets == 0);
	/* The next flush once it is listed sets it. */
	dcp.listed = true;
	dcp_desktop_ext_repower(&dcp, &test_crtc.base, &commit);
	assert(powerons == 1 && modesets == 1);

	/* Nothing for a mode already set, a panel, a gone display or an inactive CRTC. */
	fresh();
	dcp.mode_state.valid = true;
	dcp_desktop_ext_repower(&dcp, &test_crtc.base, &commit);
	fresh();
	dcp.main_display = true;
	dcp_desktop_ext_repower(&dcp, &test_crtc.base, &commit);
	fresh();
	connector.connected = false;
	dcp_desktop_ext_repower(&dcp, &test_crtc.base, &commit);
	fresh();
	crtc_state.active = false;
	dcp_desktop_ext_repower(&dcp, &test_crtc.base, &commit);
	fresh();
	desktop.ext_pipe_off = false;
	dcp_desktop_ext_repower(&dcp, &test_crtc.base, &commit);
	fresh();
	dcp.fw_compat = DCP_FIRMWARE_V_27_0;
	dcp_desktop_ext_repower(&dcp, &test_crtc.base, &commit);
	assert(powerons == 0 && modesets == 0);
	puts("desktop 26.x external pipe re-power PASS");
	return 0;
}
'''

GUARD = re.compile(r'\tif \(!dcp_mode_settable\(dcp, &cs->mode\)\) \{.*?\n\t\}\n', re.S)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--negative-control', action='store_true',
                        help='drop the listed-mode check; the harness must fail')
    args = parser.parse_args()
    source = (args.tree / 'drivers/gpu/drm/apple/iomfb.c').read_text()
    repower = function(source, 'dcp_desktop_ext_repower')
    if args.negative_control:
        repower, count = GUARD.subn('', repower)
        assert count == 1, 'missing production listed-mode check'
    with tempfile.TemporaryDirectory(prefix='dcp-ext-repower-') as directory:
        path = Path(directory) / 'repower.c'
        path.write_text(PRELUDE + repower + TESTS)
        binary = path.with_suffix('')
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        subprocess.run(compiler + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                   str(path),
                                   '-o', str(binary)], check=True)
        result = subprocess.run([binary], capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        sys.exit(0 if result.returncode == 0 else 1)


if __name__ == '__main__':
    main()
