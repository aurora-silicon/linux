#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the J613 boot gate with modular DRM and refused handoffs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = source.index('static int __init ' + name + '(void)')
    end = source.index('\n}', start) + 2
    return source[start:end]


def check(build=None):
    source = (ROOT / 'drivers/soc/apple/j613-display-gate.c').read_text()
    code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "include/linux/soc/apple/j613-display.h"
#define __init
#define __iomem
#define BIT(x) (1U << (x))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define pr_info(...) ((void)0)
struct device_node { const char *path; unsigned index; };
struct of_changeset { unsigned pending; };
static struct device_node nodes[11];
static struct of_changeset gate;
static unsigned display_clock_hz;
static bool machine, profile_present, requested, firmware_only;
bool apple_t6030_display_gate_enabled(void) { return requested; }
static bool video_firmware_drivers_only(void) { return firmware_only; }
static u32 profile, version[3], witness[5], markers[3], cpu[0x48 / 4];
static const char *uuid;
static unsigned enabled, populated, writes, refs;
static int missing_node, ready_result, pmp_result, apply_result;
static struct device_node *of_find_node_by_path(const char *path) {
 for (unsigned i=0;i<ARRAY_SIZE(nodes);i++) if (!strcmp(path,nodes[i].path)) {
  if ((int)i==missing_node) return NULL;
  refs++; return &nodes[i];
 }
 return NULL;
}
static void of_node_put(struct device_node *node) { if(node){assert(refs);refs--;} }
static bool of_machine_is_compatible(const char *name) { assert(!strcmp(name,"apple,j613"));return machine; }
static bool of_property_present(struct device_node *node,const char *name) {
 assert(node==&nodes[9]&&!strcmp(name,"apple,j613-25g83-profile"));return profile_present;
}
static int of_property_count_u32_elems(struct device_node *node,const char *name) {
 if (!strcmp(name,"apple,firmware-compat")) return 3;
 if (!strcmp(name,"apple,j613-25g83-display-clock-adt")) {assert(node==&nodes[10]);return 5;}
 return 1;
}
static int of_property_read_u32(struct device_node *node,const char *name,u32 *value) {
 if (!strcmp(name,"apple,j613-25g83-profile")) *value=profile;
 else { assert(node->index>=7&&node->index<=9&&!strcmp(name,J613_25G83_HANDOFF));*value=markers[node->index-7]; }
 return 0;
}
static int of_property_read_u32_array(struct device_node *node,const char *name,u32 *values,size_t n) {
 (void)node;memcpy(values,!strcmp(name,"apple,firmware-compat")?version:witness,n*sizeof(u32));return 0;
}
static int of_property_read_string(struct device_node *node,const char *name,const char **value) {
 if(!strcmp(name,"apple,firmware-uuid"))*value=uuid;
 else {assert(node->index<10&&!strcmp(name,"status"));*value=enabled&(1U<<node->index)?"okay":"disabled";}
 return 0;
}
static int j613_power_ready(void) {return ready_result;}
static void *of_iomap(struct device_node *node,unsigned index) {assert(node==&nodes[9]&&!index);return cpu;}
static u32 readl(const void *ptr) {assert(ptr==(const char*)cpu+0x44);return cpu[0x44 / 4];}
static void iounmap(void *ptr) {(void)ptr;}
static void of_changeset_init(struct of_changeset *change) {change->pending=0;}
static int j613_pmp_values(struct device_node *node) {assert(node==&nodes[3]);return pmp_result;}
static int of_changeset_update_prop_string(struct of_changeset *change,struct device_node *node,const char *name,const char *value) {
 assert(!strcmp(name,"status")&&!strcmp(value,"okay"));change->pending|=1U<<node->index;return 0;
}
static int of_changeset_apply(struct of_changeset *change) {
 assert(!populated);writes++;if(apply_result)return apply_result;enabled=change->pending;return 0;
}
static void of_changeset_destroy(struct of_changeset *change) {change->pending=0;}
static void reset(void) {
 const char *paths[]={"/soc/pmp-report@2d03c0000","/soc/iommu@2d0300000","/soc/mailbox@2d0c08000","/soc/pmp@2d0500000",
 "/soc/iommu@28d30c000","/soc/iommu@28d304000","/soc/mailbox@28ec08000","/soc/display-subsystem",
 "/soc/dcp@28ec00000/piodma","/soc/dcp@28ec00000","/chosen"};
 for(unsigned i=0;i<ARRAY_SIZE(nodes);i++)nodes[i]=(struct device_node){paths[i],i};
 machine=profile_present=requested=true;profile=1;version[0]=26;version[1]=6;version[2]=2;
 firmware_only=false;
 witness[0]=1;witness[1]=345;witness[2]=416;witness[3]=712000000;witness[4]=0;
 markers[0]=markers[1]=markers[2]=1;cpu[0x44 / 4]=BIT(4);uuid=J613_25G83_DCP_UUID;
 enabled=populated=writes=refs=display_clock_hz=0;missing_node=-1;ready_result=pmp_result=apply_result=0;
}
GATE_FUNCTIONS
int main(void) {
 reset();assert(!apple_j613_25g83_display_gate());assert(enabled==0x3ff&&writes==1&&display_clock_hz==witness[3]&&!refs);
 /* Platform population can discover the display before its DRM module loads. */
 populated=enabled;assert(populated&(1U<<7));assert(populated&(1U<<9));
 for(unsigned failure=0;failure<14;failure++) {
  reset();switch(failure){
   case 0:machine=false;break;case 1:profile_present=false;break;case 2:profile=2;break;
   case 3:version[0]=14;break;case 4:version[2]=3;break;case 5:uuid="wrong";break;
   case 6:witness[3]=0;break;case 7:markers[1]=0;break;case 8:missing_node=2;break;
   case 9:ready_result=-EBUSY;break;case 10:pmp_result=-EINVAL;break;case 11:apply_result=-ENOMEM;break;
   case 12:requested=false;break;
   case 13:firmware_only=true;break;
  }
  assert(!apple_j613_25g83_display_gate());assert(!enabled&&!display_clock_hz&&!refs);
 }
 puts("PASS J613 coldplug before platform population; legacy/mismatched/partial/busy/failed handoffs retain framebuffer");
}
'''
    code = code.replace('GATE_FUNCTIONS', function(source, 'apple_j613_25g83_coldplug') + '\n' +
                        function(source, 'apple_j613_25g83_display_gate'))
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        (tmp / 'linux').mkdir()
        (tmp / 'linux/types.h').write_text('#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\ntypedef uint32_t u32;\n')
        (tmp / 'linux/string.h').write_text('#include <string.h>\n')
        (tmp / 'linux/kconfig.h').write_text('#define IS_ENABLED(x) 1\n')
        (tmp / 'test.c').write_text(code)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I'+str(tmp), '-I'+str(ROOT),
                        str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
        subprocess.run([str(tmp / 'test')], check=True)
    if build:
        config = (build / '.config').read_text()
        assert 'CONFIG_DRM_APPLE=m\n' in config
        assert 'CONFIG_APPLE_J613_DISPLAY_GATE=y\n' in config
        builtin = subprocess.check_output(['llvm-nm', '-a', str(build / 'drivers/soc/apple/built-in.a')], text=True)
        module = subprocess.check_output(['llvm-nm', '-a', str(build / 'drivers/gpu/drm/apple/appledrm.o')], text=True)
        assert '__initcall__' in builtin and 'apple_j613_25g83_display_gate' in builtin
        assert 'apple_j613_25g83_coldplug' not in module
        assert ' U apple_j613_25g83_clock_hz' in module
        assert '__export_symbol_apple_j613_25g83_clock_hz' in builtin
        print('PASS compiled built-in J613 arch_initcall and exported clock with modular DRM')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path)
    check(parser.parse_args().build_dir)
