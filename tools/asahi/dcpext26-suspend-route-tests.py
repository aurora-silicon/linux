#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Replay retained suspend/resume branch pairing; route/MMIO calls are mocks."""
from pathlib import Path
import re
import resource
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/gpu/drm/apple/dcp.c').read_text()
header = (root / 'drivers/gpu/drm/apple/dcp-internal.h').read_text()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

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
    match = re.search(r'\b' + name + r'\([^;{}]*\)\n\{', text)
    assert match, name
    return block(text, text.rfind('\n', 0, match.start()) + 1)

suspend_at = source.index('static int dcp_platform_suspend(')
suspend = block(source, source.index('if (dcp_is_retained_external(dcp)) {', suspend_at))
resume_at = source.index('static int dcp_platform_resume(')
resume_native = block(source, source.index('if (dcp_is_ext26(dcp)) {', resume_at))
unpark = re.search(r'if \(dcp_is_external_h17p\(dcp\)\)\n\s*dcp_typec_route_unpark\(dcp\);', source[resume_at:])
assert unpark
park = 'if (dcp_is_external_h17p(dcp))\n\t\t\tdcp_typec_route_park(dcp);'
assert suspend.count(park) == 1
if '--native-parks' in sys.argv:
    suspend = suspend.replace(park, 'dcp_typec_route_park(dcp);')
if '--drop-h17p-park' in sys.argv:
    suspend = suspend.replace(park, '(void)dcp;')

pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define APPLE_DCP_COPROC_CPU_CONTROL 0
#define APPLE_DCP_COPROC_CPU_CONTROL_RUN 1
#define DCP_IOMFB_METHODS_H17P_EXTERNAL 1
#define DCP_IOMFB_METHODS_J613_EXT26 2
struct device { int unused; };
struct apple_dcp {
 struct { int iomfb_method_profile; } hw;
 void *coproc_reg;
 bool crashed, ext26_blocked, external_suspended;
 int external_error;
};
static unsigned run=1, parks, unparks, wake_paths, disabled, enabled;
static int disconnect_error;
static unsigned readl(const void *p) { return run; }
static void dcp_typec_route_park(struct apple_dcp *d) { parks++; }
static void dcp_typec_route_unpark(struct apple_dcp *d) { unparks++; }
static void dcp_disable_typec_work(struct apple_dcp *d,bool sync) { disabled++; }
static void dcp_enable_typec_work(struct apple_dcp *d) { enabled++; }
static int dcp_external_disconnect(struct apple_dcp *d,bool wait) { return disconnect_error; }
static void device_set_wakeup_path(struct device *d) { wake_paths++; }
static void reset_counts(void) { parks=unparks=wake_paths=disabled=enabled=0; }
'''
code = pre + function(header, 'dcp_is_ext26') + '\n' + function(source, 'dcp_is_external_h17p') + '\n' + function(source, 'dcp_is_retained_external')
code += '\nstatic int suspend_path(struct device *dev,struct apple_dcp *dcp) {\n' + suspend + '\nreturn 0; }\n'
code += '\nstatic int resume_path(struct device *dev,struct apple_dcp *dcp) {\n' + resume_native + '\n' + unpark.group(0) + '\nreturn 0; }\n'
code += r'''
int main(void) {
 struct device dev={0};
 struct apple_dcp native={.hw.iomfb_method_profile=DCP_IOMFB_METHODS_J613_EXT26,
                         .coproc_reg=(void *)0x1000,.external_suspended=true};
 assert(suspend_path(&dev,&native)==0);
 assert(parks==0 && disabled==0 && wake_paths==1);
 assert(resume_path(&dev,&native)==0 && unparks==0 && !native.external_suspended);
 reset_counts();run=0;
 assert(suspend_path(&dev,&native)==-EHOSTDOWN && parks==0 && wake_paths==0);
 assert(resume_path(&dev,&native)==-EHOSTDOWN && native.ext26_blocked && native.external_error==-EHOSTDOWN);
 run=1;reset_counts();disconnect_error=-EBUSY;
 assert(suspend_path(&dev,&native)==-EBUSY && parks==0 && wake_paths==0 && enabled==0);
 struct apple_dcp h17p={.hw.iomfb_method_profile=DCP_IOMFB_METHODS_H17P_EXTERNAL};
 disconnect_error=0;reset_counts();
 assert(suspend_path(&dev,&h17p)==0 && parks==1 && disabled==1 && wake_paths==1);
 assert(resume_path(&dev,&h17p)==0 && unparks==1);
 reset_counts();disconnect_error=-EBUSY;
 assert(suspend_path(&dev,&h17p)==-EBUSY && parks==0 && wake_paths==0 && disabled==1 && enabled==1);
 struct apple_dcp other={0};disconnect_error=0;reset_counts();
 assert(suspend_path(&dev,&other)==0 && resume_path(&dev,&other)==0 && parks==0 && unparks==0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'suspend.c'
    path.write_text(code)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', str(path), '-o', str(path.with_suffix(''))], check=True)
    result = subprocess.run([str(path.with_suffix(''))])
    if result.returncode:
        sys.exit(1)
print('PASS retained sleep: H17P park/unpark paired; native26 no park, RUN refusal; disconnect rollback preserved')
