#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the production atomic admission check for external H17P VRR."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/gpu/drm/apple/dcp.c').read_text()
start = source.index('int dcp_crtc_atomic_check(')
opening = source.index('{', start)
end, depth = opening + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
check = source[start:end]
prefix = r"""
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define READ_ONCE(x) (x)
#define dev_dbg(...) ((void)0)
struct apple_connector { bool connected; };
struct apple_dcp {
    bool crashed;
    struct { int iomfb_method_profile; } hw;
    struct { bool valid; } mode_state;
    struct apple_connector *connector;
    void *dev;
};
struct platform_device { struct apple_dcp *data; };
struct drm_crtc { struct platform_device *dcp; };
struct drm_crtc_state { bool vrr_enabled, modeset; };
struct drm_atomic_state { struct drm_crtc_state crtc; };
#define to_apple_crtc(x) (x)
#define platform_get_drvdata(x) ((x)->data)
static struct drm_crtc_state *drm_atomic_get_new_crtc_state(
    struct drm_atomic_state *s, struct drm_crtc *c)
{ (void)c; return &s->crtc; }
static bool drm_atomic_crtc_needs_modeset(struct drm_crtc_state *s)
{ return s->modeset; }
"""
suffix = r"""
int main(void)
{
    struct apple_connector connector = { .connected = true };
    struct apple_dcp dcp = { .connector = &connector };
    struct platform_device pdev = { .data = &dcp };
    struct drm_crtc crtc = { .dcp = &pdev };
    struct drm_atomic_state state = {0};
    unsigned int cases = 0;
    /* Check initial modesets, plane-only commits and link recovery states. */
    for (unsigned int profile = 0; profile < 3; profile++)
    for (unsigned int valid = 0; valid < 2; valid++)
    for (unsigned int modeset = 0; modeset < 2; modeset++)
    for (unsigned int link = 0; link < 3; link++)
    for (unsigned int vrr = 0; vrr < 2; vrr++) {
        dcp.hw.iomfb_method_profile = profile;
        dcp.mode_state.valid = valid;
        state.crtc.modeset = modeset;
        state.crtc.vrr_enabled = vrr;
        dcp.connector = link == 2 ? NULL : &connector;
        connector.connected = link == 1;
        int expected = profile == DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL && vrr ? -EINVAL : 0;
        int actual = dcp_crtc_atomic_check(&crtc, &state);
        if (actual != expected) {
            fprintf(stderr, "profile=%u valid=%u modeset=%u link=%u vrr=%u: got %d expected %d\n",
                    profile, valid, modeset, link, vrr, actual, expected);
            return 1;
        }
        cases++;
    }
    dcp.crashed = true;
    state.crtc.vrr_enabled = false;
    assert(dcp_crtc_atomic_check(&crtc, &state) == -EINVAL);
    printf("PASS: %u production atomic VRR admission cases and crashed-device rejection\n", cases);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='apple-external-vrr-') as tmp:
    tmp = Path(tmp)
    unit = tmp / 'test.c'
    unit.write_text(prefix + check + suffix)
    command = shlex.split(os.environ.get('CC', 'clang'))
    command += ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(unit), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
