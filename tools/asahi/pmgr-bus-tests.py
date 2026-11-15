#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise transparent PMGR binding using the production probe and table."""
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]

PREAMBLE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define GFP_KERNEL 0
#define dev_dbg(...) ((void)0)
#define dev_err_probe(dev, err, ...) (err)
struct clk_bulk_data { int unused; };
struct of_dev_auxdata { int unused; };
struct of_device_id { const char *compatible; const void *data; };
struct device_node { const char *compatible[5]; };
struct device_driver { const struct of_device_id *of_match_table; };
struct device {
    struct device_node *of_node;
    struct device_driver *driver;
    bool override;
};
struct platform_device { struct device dev; };
static unsigned int effects;
static const void *dev_get_platdata(const struct device *dev)
{ (void)dev; return NULL; }
static bool device_has_driver_override(const struct device *dev)
{ return dev->override; }
static int of_property_match_string(const struct device_node *np,
                                    const char *prop, const char *value)
{
    (void)prop;
    for (int i = 0; np->compatible[i]; i++)
        if (!strcmp(np->compatible[i], value))
            return i;
    return -EINVAL;
}
/* OF selects the earliest device compatible, independent of table order. */
static const struct of_device_id *of_match_device(
        const struct of_device_id *table, const struct device *dev)
{
    for (int i = 0; dev->of_node->compatible[i]; i++)
        for (const struct of_device_id *m = table; m->compatible; m++)
            if (!strcmp(dev->of_node->compatible[i], m->compatible))
                return m;
    return NULL;
}
static void *devm_kzalloc(struct device *dev, size_t size, int flags)
{ (void)dev; (void)size; (void)flags; effects++; return NULL; }
static int devm_clk_bulk_get_all(struct device *dev, struct clk_bulk_data **clks)
{ (void)dev; (void)clks; effects++; return 0; }
static void dev_set_drvdata(struct device *dev, void *data)
{ (void)dev; (void)data; effects++; }
static void pm_runtime_enable(struct device *dev)
{ (void)dev; effects++; }
static void of_platform_populate(struct device_node *np, const void *match,
                                const void *lookup, struct device *dev)
{ (void)np; (void)match; (void)lookup; (void)dev; effects++; }
'''

MAIN = r'''
static void check(struct device_node *np, int expected, bool override)
{
    struct device_driver driver = { simple_pm_bus_of_match };
    struct platform_device pdev = { .dev = { np, &driver, override } };
    effects = 0;
    assert(simple_pm_bus_probe(&pdev) == expected);
    /* Transparent buses must not allocate, acquire clocks or repopulate. */
    assert(effects == 0);
}
int main(void)
{
    struct device_node t6041 = {
        { "apple,t6041-pmgr", "apple,t8103-pmgr", "syscon", "simple-mfd" }
    };
    struct device_node t8103 = {
        { "apple,t8103-pmgr", "apple,pmgr", "syscon", "simple-mfd" }
    };
    struct device_node bus = { { "simple-mfd" } };
    struct device_node dedicated = { { "vendor,dedicated", "simple-mfd" } };
    struct device_node future = {
        { "apple,future-pmgr", "apple,t8103-pmgr", "syscon", "simple-mfd" }
    };
    check(&t6041, 0, false);
    check(&t8103, 0, false);
    check(&bus, 0, false);
    check(&dedicated, -ENODEV, false);
    check(&future, -ENODEV, false);
    check(&dedicated, 0, true);
    puts("PASS: transparent PMGR binding and specific-driver guard");
    return 0;
}
'''


def main():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source = (ROOT / 'drivers/bus/simple-pm-bus.c').read_text()
    probe = source[source.index('static int simple_pm_bus_probe('):
                   source.index('static void simple_pm_bus_remove(')]
    bus = re.search(r'struct simple_pm_bus \{.*?\n\};', source, re.S)[0]
    table = source[source.index('#define ONLY_BUS'):
                   source.index('MODULE_DEVICE_TABLE(of, simple_pm_bus_of_match)')]
    with tempfile.TemporaryDirectory(prefix='pmgr-bus-') as tmp:
        tmp = Path(tmp)
        for negative in (False, True):
            selected = table
            if negative:
                selected = re.sub(r'^.*"apple,t6041-pmgr".*\n', '', table, flags=re.M)
                assert selected != table, 'T6041 transparent match missing'
            cfile, exe = tmp / 'test.c', tmp / 'test'
            cfile.write_text(PREAMBLE + bus + '\n' + probe + selected + MAIN)
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                           ['-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile),
                            '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            if negative:
                assert result.returncode < 0 and 'Assertion' in result.stderr, result
                print('PASS: removing the T6041 match fails the binding assertion')
            else:
                assert result.returncode == 0, result.stderr
                print(result.stdout, end='')


if __name__ == '__main__':
    main()
