#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the actual DAPF parser's optional-description and rejection contract.

The host dependencies model DT metadata, allocation and mapping counts only.
No filter register or firmware is accessed; this cannot qualify hardware replay.
"""
import argparse
from pathlib import Path
import re
import resource
import subprocess


def function(text):
    match = re.search(r'^static int apple_dart_dapf_parse\(', text, re.M)
    if not match:
        raise RuntimeError('production DAPF parser not found')
    start = text.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define IORESOURCE_MEM 0
#define GFP_KERNEL 0
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
struct device_node { int unused; };
struct device { struct device_node *of_node; };
struct platform_device { struct device dev; };
struct resource { size_t size; };
struct apple_dart { unsigned dapf_count; void *dapf; u64 *dapf_ranges; u32 *dapf_r20; };
static bool has_ranges, has_r20, force_replay, map_failure;
static int count, maps, allocations;
static struct resource region;
static struct resource *platform_get_resource_byname(struct platform_device *p, int type, const char *name)
{ return &region; }
static size_t resource_size(struct resource *r) { return r->size; }
static bool of_property_present(struct device_node *n, const char *name)
{ return !strcmp(name,"apple,dapf-ranges") ? has_ranges : has_r20; }
static bool of_property_read_bool(struct device_node *n, const char *name)
{ assert(!strcmp(name,"apple,restore-on-power-up")); return force_replay; }
static int of_property_count_u64_elems(struct device_node *n, const char *name)
{ assert(!strcmp(name,"apple,dapf-ranges")); return has_ranges ? count : -EINVAL; }
static int dev_err_probe(struct device *d, int error, const char *format, ...) { return error; }
static void *devm_platform_ioremap_resource_byname(struct platform_device *p, const char *name)
{ ++maps; return map_failure ? (void *)(intptr_t)-EIO : &region; }
static void *devm_kcalloc(struct device *d, size_t n, size_t size, int flags)
{ ++allocations; return calloc(n ? n : 1,size); }
static int of_property_read_u64_array(struct device_node *n, const char *name, u64 *values, int nr)
{ for (int i=0;i<nr;++i) values[i]=0x1000u+0x1000u*(unsigned)i; return 0; }
static int of_property_count_u32_elems(struct device_node *n, const char *name)
{ return has_r20 ? count/2 : -EINVAL; }
static int of_property_read_u32_array(struct device_node *n, const char *name, u32 *values, int nr)
{ for (int i=0;i<nr;++i) values[i]=3; return 0; }
'''

CASES = r'''
static void row(const char *name, bool ranges, int nr, bool r20, bool force,
                size_t bytes, bool bad_map, int expected, int expected_maps)
{
    struct device_node node={0};
    struct platform_device p={.dev={.of_node=&node}};
    struct apple_dart dart={0};
    has_ranges=ranges;count=nr;has_r20=r20;force_replay=force;
    region.size=bytes;map_failure=bad_map;maps=allocations=0;
    int result=apple_dart_dapf_parse(&p,&dart);
    if (result!=expected || maps!=expected_maps) fprintf(stderr,"DAPF case: %s\n",name);
    assert(result==expected);
    assert(maps==expected_maps);
    if (!ranges && !expected) {
        assert(dart.dapf_count==0 && !dart.dapf && allocations==0);
    }
    if (!expected && ranges) {
        assert(dart.dapf_count==(unsigned)nr/2 && allocations==2);
        assert(dart.dapf_ranges[0]==0x1000);
        assert(dart.dapf_r20[0]==(r20 ? 3u : 1u));
    }
    free(dart.dapf_ranges);free(dart.dapf_r20);
}
int main(void)
{
    /* The actual M5 cold nodes name a DAPF window without replay properties. */
    row("inherited cold state",false,0,false,false,0x100,false,0,0);
    row("orphan r20",false,0,true,false,0x100,false,-EINVAL,0);
    row("explicit replay without ranges",false,0,false,true,0x100,false,-EINVAL,0);
    row("present empty",true,0,false,false,0x100,false,-EINVAL,0);
    row("present truncated",true,-EOVERFLOW,false,false,0x100,false,-EINVAL,0);
    row("present odd",true,3,false,false,0x100,false,-EINVAL,0);
    row("region too small",true,4,false,false,0x40,false,-EINVAL,0);
    row("map failure",true,2,false,false,0x100,true,-EIO,1);
    row("one normal range",true,2,false,false,0x100,false,0,1);
    row("two normal ranges",true,4,true,false,0x100,false,0,1);
    row("explicit replay with valid data",true,2,false,true,0x100,false,0,1);
    puts("DAPF parser optional/rejection controls passed");
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    ap.add_argument('--mutation', choices=['missing-guard', 'ignore-replay', 'ignore-orphan'])
    a = ap.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source = (a.source/'drivers/iommu/apple-dart.c').read_text()
    body = function(source)
    if a.mutation == 'missing-guard':
        body, count = re.subn(r'\tif \(!of_property_present\(np, "apple,dapf-ranges"\)\) \{\n.*?\n\t\}', '', body, flags=re.S)
        if count != 1:
            raise RuntimeError('missing-guard mutation did not apply exactly once')
    elif a.mutation:
        old = ('of_property_read_bool(np, "apple,restore-on-power-up")' if a.mutation == 'ignore-replay'
               else 'of_property_present(np, "apple,dapf-r20")')
        if body.count(old) != 1:
            raise RuntimeError('mutation did not apply exactly once')
        body = body.replace(old, 'false')
    stride = re.search(r'^#define DART_DAPF_STRIDE\s+[^\n]+', source, re.M)
    if not stride:
        raise RuntimeError('production stride definition not found')
    a.out.mkdir(parents=True, exist_ok=True)
    c, binary = a.out/'control.c', a.out/'control'
    c.write_text(PREFIX+'\n'+stride.group()+'\n'+body+'\n'+CASES)
    # Retain the production int/unsigned count comparison and its warning.
    # Only that diagnostic is nonfatal here; all other -Werror checks remain.
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-error=sign-compare','-Wno-unused-parameter',
                    '-Wno-unused-function',str(c),'-o',str(binary)], check=True)
    raise SystemExit(subprocess.run([str(binary)],check=False).returncode)


if __name__ == '__main__':
    main()
