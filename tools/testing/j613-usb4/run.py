#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the production PMGR off witness against a fault-injecting regmap."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / 'drivers/pmdomain/apple/pmgr-pwrstate.c').read_text()
start = source.index('int apple_pmgr_ps_verify_off(')
end = source.index('\nEXPORT_SYMBOL', start)
macros = '\n'.join(re.findall(r'^#define APPLE_PMGR_(?:AUTO_ENABLE|BUSY|PS_ACTUAL|PS_TARGET|PS_LEVELS)\b[^\n]*', source, re.M))
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31-(h))) & (~0U << (l)))
struct generic_pm_domain { void *power_off; };
struct device { void *pm_domain; };
struct apple_pmgr_ps { void *regmap; unsigned int offset; };
static struct apple_pmgr_ps state;
static struct generic_pm_domain domain;
static void apple_pmgr_ps_power_off(void) {}
#define pd_to_genpd(p) ((struct generic_pm_domain *)(p))
#define genpd_to_apple_pmgr_ps(p) (&state)
#define dev_err(...) ((void)0)
static u32 value;
static int error;
static int regmap_read(void *map, unsigned int offset, u32 *out)
{ *out = value; return error; }
'''
cases = r'''
int main(void)
{
    struct device d = {0};
    assert(apple_pmgr_ps_verify_off(&d) == -ENODEV);
    d.pm_domain = &domain;
    assert(apple_pmgr_ps_verify_off(&d) == -EOPNOTSUPP);
    domain.power_off = apple_pmgr_ps_power_off;
    error = -EIO;
    assert(apple_pmgr_ps_verify_off(&d) == -EIO);
    error = 0;
    assert(apple_pmgr_ps_verify_off(&d) == 0);
    for (unsigned int bit = 0; bit < 32; bit++) {
        value = BIT(bit);
        int expected = value & (APPLE_PMGR_PS_LEVELS | APPLE_PMGR_BUSY |
                               APPLE_PMGR_AUTO_ENABLE) ? -EBUSY : 0;
        assert(apple_pmgr_ps_verify_off(&d) == expected);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='j613-usb4-') as tmp:
    t = Path(tmp)
    (t / 'test.c').write_text(prefix + macros + '\n' + source[start:end] + cases)
    subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Werror',
                    str(t / 'test.c'), '-o', str(t / 'test')], check=True)
    subprocess.run([str(t / 'test')], check=True)
print('PASS: production PMGR witness rejects missing/foreign domains, read faults, all live state bits')

# The PCIe-C slices as the J613 DT gives them, checked by the same helper PCIe-C admission uses.
dts = (root / 'arch/arm64/boot/dts/apple/t8122-usb4.dtsi').read_text()
internal = (root / 'arch/arm64/boot/dts/apple/t8122.dtsi').read_text()
bases = [int(x) for x in re.findall(r'apple,msi-vector-base = <(\d+)>', dts)]
irqs = [int(x) for x in re.findall(r'msi-ranges = <&aic AIC_IRQ (\d+) IRQ_TYPE_EDGE_RISING 32>', dts)]
window = int(re.search(r'msi-ranges = <&aic AIC_IRQ (\d+) IRQ_TYPE_EDGE_RISING 32>', internal).group(1))
assert len(bases) == len(irqs) == 2, (bases, irqs)
assert len(set(bases)) == len(bases), f'two PCIe-C bridges claim the same MSI slice: {bases}'
pairs = ', '.join(f'{{{b}, {i}}}' for b, i in zip(bases, irqs))
msi = r'''
#include <assert.h>
#include "pcie-apple-t8122.h"
static const unsigned int slices[][2] = { PAIRS };
int main(void)
{
    for (unsigned int i = 0; i < sizeof(slices) / sizeof(slices[0]); i++)
        assert(apple_t8122_msi_window(slices[i][0], slices[i][1], 32) == WINDOW);
    assert(apple_t8122_msi_window(0, WINDOW, 32) < 0);   /* the internal host's vectors */
    assert(apple_t8122_msi_window(48, WINDOW + 48, 32) < 0);
    assert(apple_t8122_msi_window(32, WINDOW + 32, 64) < 0);
    assert(apple_t8122_msi_window(64, 10, 32) < 0);
    assert(apple_t8122_msi_window(256, WINDOW + 256, 32) < 0);  /* past vector 255 */
    return 0;
}
'''.replace('PAIRS', pairs).replace('WINDOW', str(window))
with tempfile.TemporaryDirectory(prefix='j613-msi-') as tmp:
    t = Path(tmp)
    (t / 'test.c').write_text(msi)
    subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Werror',
                    '-I', str(root / 'drivers/pci/controller'),
                    str(t / 'test.c'), '-o', str(t / 'test')], check=True)
    subprocess.run([str(t / 'test')], check=True)
print('PASS: T8122 PCIe-C MSI slices from the DT, and invalid slices refused')
