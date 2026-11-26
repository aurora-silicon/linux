#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Replay the borrowed DCP DART's memory-region walk with disabled placeholders.

A J713 tree carries disabled dcpextN text, data and heap placeholders for the
27.2 handoff. On 26.6.2 the boot loader appends its real regions after them,
so the borrowed path must skip a disabled entry and still refuse every other
malformed one.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def definition(source, name):
    start = source.index(name)
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--keep-placeholders', action='store_true',
                    help='mutant: walk disabled entries as the old code did')
args = parser.parse_args()

source = (ROOT / 'drivers/iommu/apple-dart-dcp.h').read_text()
region = definition(source, 'static int apple_dart_dcp_region(')
leaf = definition(source, 'static int apple_dart_dcp_leaf_regions(')
validate = definition(source, 'static int apple_dart_dcp_validate(')
# The SID23 validation walks the same entries through the same helper.
assert 'apple_dart_dcp_region(dev->of_node, i, &node)' in validate
assert 'if (!node)\n\t\t\tcontinue;' in validate
assert 'of_parse_phandle(dev->of_node, "memory-region"' not in validate
if args.keep_placeholders:
    old = 'if (!of_device_is_available(*node)) {'
    assert region.count(old) == 1
    region = region.replace(old, 'if (0) {', 1)

unit = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
struct resource { u64 start, end; };
struct device_node { const char *name; bool available, no_map, reserved; u64 start, size; };
struct reserved_mem { u64 base, size; };
struct dcp_instance { const char *dcp; };
struct apple_dart { bool dispext; const struct dcp_instance *dcp_instance; };
struct apple_dart_dcp {
    struct apple_dart *dart;
    unsigned int sid;
    struct resource leaf_regions[64];
    unsigned int nr_leaf_regions;
};
static struct device_node consumer = { "dcpext0", true };
static struct device_node *regions[6];
static int nr_regions, refs;
static struct reserved_mem rmem;
static struct device_node *of_find_node_by_path(const char *p)
{ assert(!strcmp(p, "dcpext0")); refs++; return &consumer; }
static int of_property_count_u32_elems(struct device_node *n, const char *p)
{ assert(n == &consumer && !strcmp(p, "memory-region")); return nr_regions; }
static struct device_node *of_parse_phandle(struct device_node *n, const char *p, int i)
{
    assert(n == &consumer && !strcmp(p, "memory-region") && i >= 0 && i < nr_regions);
    if (regions[i])
        refs++;
    return regions[i];
}
static bool of_device_is_available(struct device_node *n) { return n->available; }
static void of_node_put(struct device_node *n) { if (n) { assert(refs > 0); refs--; } }
static bool of_property_read_bool(struct device_node *n, const char *p)
{ assert(!strcmp(p, "no-map")); return n->no_map; }
static int of_address_to_resource(struct device_node *n, int i, struct resource *r)
{
    (void)i;
    r->start = n->start;
    r->end = n->start + n->size - 1;
    return 0;
}
static u64 resource_size(struct resource *r) { return r->end - r->start + 1; }
static struct reserved_mem *of_reserved_mem_lookup(struct device_node *n)
{
    if (!n->reserved)
        return NULL;
    rmem.base = n->start;
    rmem.size = n->size;
    return &rmem;
}
''' + region + '\n' + leaf + r'''
static const struct dcp_instance ext0 = { "dcpext0" };
static struct apple_dart dart = { false, &ext0 };
static int run(struct apple_dart_dcp *d, unsigned int sid, bool dispext)
{
    int ret;

    dart.dispext = dispext;
    d->dart = &dart;
    d->sid = sid;
    d->nr_leaf_regions = 0;
    ret = apple_dart_dcp_leaf_regions(d);
    assert(refs == 0);
    return ret;
}
int main(void)
{
    /* As in t8132-j713.dts: disabled, nothing reserved, reg <0 0 0 0x4000>. */
    struct device_node text = { "text", false, false, false, 0, 0x4000 };
    struct device_node data = text, heap = text;
    struct device_node fw0 = { "fw0", true, true, true, 0x880000000ULL, 0x8000 };
    struct device_node fw1 = { "fw1", true, true, true, 0x890000000ULL, 0x4000 };
    struct apple_dart_dcp d = { 0 };
    struct device_node *node;

    /* The 26.6.2 loader appends its regions after the placeholders. */
    regions[0] = &text; regions[1] = &data; regions[2] = &heap;
    regions[3] = &fw0; regions[4] = &fw1; nr_regions = 5;
    assert(run(&d, 23, false) == 0 && d.nr_leaf_regions == 2);
    assert(d.leaf_regions[0].start == 0x880000000ULL && d.leaf_regions[0].end == 0x880007fffULL);
    assert(d.leaf_regions[1].start == 0x890000000ULL && d.leaf_regions[1].end == 0x890003fffULL);
    /* The external display DART's SID16 serves the same dcpext's memory. */
    assert(run(&d, 16, true) == 0 && d.nr_leaf_regions == 2);
    /* Real regions first and placeholders after: the same result. */
    regions[0] = &fw0; regions[1] = &fw1; regions[2] = &text; regions[3] = &data; regions[4] = &heap;
    assert(run(&d, 23, false) == 0 && d.nr_leaf_regions == 2);

    /* The helper: a disabled entry is NULL with its reference dropped. */
    assert(!apple_dart_dcp_region(&consumer, 2, &node) && !node && refs == 0);
    assert(!apple_dart_dcp_region(&consumer, 0, &node) && node == &fw0 && refs == 1);
    of_node_put(node);

    /* Placeholders alone reserve nothing: refused. */
    regions[0] = &text; regions[1] = &data; regions[2] = &heap; nr_regions = 3;
    assert(run(&d, 23, false) == -EINVAL && d.nr_leaf_regions == 0);
    /* An enabled entry without its reservation is refused as before. */
    regions[3] = &fw0; regions[4] = &text; nr_regions = 5; text.available = true;
    assert(run(&d, 23, false) == -EINVAL);
    text.available = false;
    /* So are an enabled entry that is not no-map and a dangling phandle. */
    fw1.no_map = false; regions[4] = &fw1;
    assert(run(&d, 23, false) == -EINVAL);
    fw1.no_map = true; regions[4] = NULL;
    assert(run(&d, 23, false) == -EINVAL);
    regions[4] = &fw1;
    assert(run(&d, 23, false) == 0 && d.nr_leaf_regions == 2);
    /* An empty list is refused before any entry is read. */
    nr_regions = 0;
    assert(run(&d, 23, false) == -EINVAL);
    puts("PASS borrowed DCP DART skips disabled memory-region placeholders and refuses every other bad entry");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'placeholder.c'
    binary = Path(tmp) / 'placeholder'
    c.write_text(unit)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-sign-compare', '-Wno-missing-field-initializers', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', str(c), '-o', str(binary)],
                   check=True)
    result = subprocess.run([str(binary)])
    raise SystemExit(0 if result.returncode == 0 else 1)
