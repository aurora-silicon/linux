#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the production always-on predicate against legacy handoff nodes.

Failure modes: overmatching a label, offset or SoC; ignoring an explicit
property; treating a missing/mismatched label as the PPT throttle controller.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'drivers/pmdomain/apple/pmgr-pwrstate.c').read_text()
start = source.index('static bool apple_pmgr_ps_always_on(')
end = source.index('\n}\n', start) + 3
constant = re.search(r'^#define APPLE_PMGR_T8122_PMS_C1PPT\s+.*$', source, re.M).group()
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
struct device_node { const char *compatible; bool always_on; };
static bool of_property_read_bool(struct device_node *n, const char *p)
{ assert(!strcmp(p, "apple,always-on")); return n->always_on; }
static unsigned int compatible_checks;
static bool of_device_is_compatible(struct device_node *n, const char *c)
{ compatible_checks++; return !strcmp(n->compatible, c); }
''' + constant + '\n' + source[start:end] + r'''
int main(void)
{
    struct device_node n = { "apple,t8122-pmgr-pwrstate", false };
    assert(apple_pmgr_ps_always_on(&n, APPLE_PMGR_T8122_PMS_C1PPT, "pms_c1ppt"));
    assert(!apple_pmgr_ps_always_on(&n, APPLE_PMGR_T8122_PMS_C1PPT + 4, "pms_c1ppt"));
    assert(!apple_pmgr_ps_always_on(&n, APPLE_PMGR_T8122_PMS_C1PPT, "pms_c0ppt"));
    assert(!apple_pmgr_ps_always_on(&n, APPLE_PMGR_T8122_PMS_C1PPT, "pms_c1ppt-extra"));
    const char *others[] = { "apple,pmgr-pwrstate", "apple,t8103-pmgr-pwrstate",
                            "apple,t8112-pmgr-pwrstate", "apple,t6030-pmgr-pwrstate",
                            "apple,t8132-pmgr-pwrstate" };
    for (unsigned int i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        n.compatible = others[i];
        assert(!apple_pmgr_ps_always_on(&n, APPLE_PMGR_T8122_PMS_C1PPT, "pms_c1ppt"));
    }
    n.compatible = "apple,t8122-pmgr-pwrstate";
    n.always_on = true;
    compatible_checks = 0;
    assert(apple_pmgr_ps_always_on(&n, APPLE_PMGR_T8122_PMS_C1PPT, "pms_c1ppt"));
    /* An explicit property remains authoritative on any domain/SoC. */
    n.compatible = others[0];
    assert(apple_pmgr_ps_always_on(&n, 0, "other"));
    assert(compatible_checks == 0); /* Explicit property bypasses fallback. */
    return 0;
}
'''
with tempfile.TemporaryDirectory() as work:
    work = Path(work)
    (work / 'test.c').write_text(harness)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: T8122 PPT exact match, look-alikes, other SoCs, explicit property')
