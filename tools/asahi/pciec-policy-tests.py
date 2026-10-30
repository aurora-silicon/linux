#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise PCIe-C cold-init admission and ACIO admission using production C."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = re.search(r'^(?:static\s+)?bool\s+' + name + r'\(', source, re.M).start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
struct device_node {
    const char *compat[3];
    bool kernel_init, tunables;
    int status, refs;
    struct device_node *dart, *child;
};
struct of_device_id { const char *compatible; const void *data; };
struct of_phandle_args { struct device_node *np; };
static int maps;
static bool of_device_is_compatible(struct device_node *np, const char *compat)
{
    for (int i = 0; np && i < 3 && np->compat[i]; i++)
        if (!strcmp(np->compat[i], compat))
            return true;
    return false;
}
static const struct of_device_id *of_match_node(const struct of_device_id *ids,
                                                struct device_node *np)
{
    for (int i = 0; np && i < 3 && np->compat[i]; i++)
        for (const struct of_device_id *id = ids; id->compatible; id++)
            if (!strcmp(np->compat[i], id->compatible))
                return id;
    return NULL;
}
static bool of_property_read_bool(struct device_node *np, const char *name)
{
    assert(!strcmp(name, "apple,pciec-kernel-init"));
    return np->kernel_init;
}
static bool of_property_present(struct device_node *np, const char *name)
{
    assert(!strcmp(name, "apple,tunable"));
    return np->tunables;
}
static int of_property_read_u32(struct device_node *np, const char *name, u32 *v)
{
    assert(!strcmp(name, "apple,pciec-preinit-status"));
    if (np->status < 0)
        return -1;
    *v = np->status;
    return 0;
}
static int of_map_iommu_id(struct device_node *np, int rid, struct of_phandle_args *a)
{
    assert(rid == 0);
    maps++;
    if (!np->dart)
        return -1;
    a->np = np->dart;
    a->np->refs++;
    return 0;
}
static void of_node_put(struct device_node *np) { assert(np->refs > 0); np->refs--; }
#define pr_warn_once(...) ((void)0)
#define for_each_available_child_of_node_scoped(parent, child) \
    for (struct device_node *child = (parent)->child; child; child = NULL)
/* PRODUCTION */
static void expect(struct device_node *host, bool cold, bool admitted)
{
    struct device_node parent = { .child = host };
    maps = 0;
    assert(apple_pcie_tunnel_needs_cold_init(host) == cold);
    assert(apple_cio_pcie_tunnel_is_preinitialized(&parent) == admitted);
    assert(!host->dart || host->dart->refs == 0);
    if (!host->kernel_init || host->status == 1)
        assert(maps == 0);
}
int main(void)
{
    struct device_node dart = { .tunables = true };
    const char *intr2axi[] = { "apple,t8112-pciec", "apple,t6000-pciec" };
    for (unsigned int i = 0; i < 2; i++) {
        /* M2 must win over its T8103 fallback, without a board allowlist. */
        struct device_node host = { .compat = { intr2axi[i], "apple,t8103-pciec" },
                                   .status = -1, .dart = &dart };
        const struct of_device_id *id = of_match_node(apple_pcie_of_match, &host);
        assert(id && ((const struct hw_info *)id->data)->tunnel_intr2axi);
        expect(&host, false, false);  /* No implicit cold reset. */
        host.kernel_init = true;
        expect(&host, true, true);
        dart.tunables = false;
        expect(&host, false, false); /* Do not let the fallback bypass DART admission. */
        host.status = 1;
        expect(&host, false, true);  /* Preserve a firmware handoff. */
        host.status = 0;
        expect(&host, false, false);
        dart.tunables = true;
        expect(&host, true, true);  /* Failed/missing handoff can be initialized. */
        host.dart = NULL;
        expect(&host, false, false);
    }
    struct device_node m1 = { .compat = { "apple,t8103-pciec" }, .status = -1 };
    expect(&m1, false, false);
    m1.kernel_init = true;
    expect(&m1, true, true);        /* Original M1 does not need T8110 DART tunables. */
    m1.status = 1;
    expect(&m1, false, true);
    struct device_node regular = { .compat = { "apple,pcie" }, .kernel_init = true,
                                  .status = -1 };
    expect(&regular, false, false);
    regular.compat[0] = "apple,unknown-pciec";
    expect(&regular, false, false);
    puts("PASS: M1, M1 Pro, M2, firmware handoff, missing DART and DT opt-in");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    pci = (ROOT / 'drivers/pci/controller/pcie-apple.c').read_text()
    acio = (ROOT / 'drivers/thunderbolt/apple.c').read_text()
    defines = '\n'.join(re.findall(r'^#define .*$', pci, re.M))
    info = pci[pci.index('struct hw_info {'):pci.index('static const struct of_device_id apple_pcie_of_match[];')]
    matches = re.search(r'static const struct of_device_id apple_pcie_of_match\[\] = \{.*?\n\};', pci, re.S)[0]
    production = '\n'.join((defines, info, matches,
                            function(pci, 'apple_pcie_tunnel_needs_cold_init'),
                            function(acio, 'apple_cio_pcie_tunnel_is_preinitialized')))
    code = FIXTURE.replace('/* PRODUCTION */', production)
    with tempfile.TemporaryDirectory(prefix='pciec-policy-') as tmp:
        out = args.out or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        cases = {'production': code,
                 'm2-fallback': code.replace('{ .compatible = "apple,t8112-pciec",\t.data = &t8112_pciec_hw },', ''),
                 'dart-bypass': code.replace('tunables = of_property_present(iommu_spec.np, "apple,tunable");', 'tunables = true;')}
        for name, body in cases.items():
            source = out / (name + '.c')
            source.write_text(body)
            binary = out / name
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function', str(source), '-o', str(binary)], check=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            (out / (name + '.log')).write_text(run.stdout + run.stderr)
            if name == 'production':
                assert run.returncode == 0, run.stderr
                print(run.stdout, end='')
            else:
                assert run.returncode < 0 and 'Assertion' in run.stderr, (name, run.returncode, run.stderr)
                print('PASS negative control:', name)


if __name__ == '__main__':
    main()
