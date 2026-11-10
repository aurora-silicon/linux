#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check actual DART probe gates against the built J713 device tree.

Compile the production admission expressions with the DT's real compatible
strings and resource addresses. MMIO and OF are substituted; firmware/DMA
execution still requires a hardware boot.
"""
from pathlib import Path
import argparse
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[6]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('dtb', type=Path, help='built J713 device tree')
parser.add_argument('--kernel-root', type=Path, default=ROOT,
                    help='kernel source whose production probe is checked')
args = parser.parse_args()
DTB, ROOT = args.dtb, args.kernel_root.resolve()
source = (ROOT / 'drivers/iommu/apple-dart.c').read_text()
t8132 = (ROOT / 'include/linux/soc/apple/t8132.h').read_text()
# Every board helper, in header order: the gates call them by trait.
air = ''
for start in [m.start() for m in re.finditer(r'^static inline bool apple_t8132_\w+\(', t8132, re.M)]:
    air += t8132[start:t8132.index('\n}\n', start) + 3]


def property(node, name, kind='s'):
    return subprocess.check_output(['fdtget', '-t', kind, str(DTB), node, name], text=True).split()


def assigned(name):
    start = source.index('\n\t' + name + ' =', source.index('static int apple_dart_probe('))
    return source[start:source.index(';', start)].split('=', 1)[1].strip()


def condition_before(marker):
    end = source.index(marker, source.index('static int apple_dart_probe('))
    start = source.rfind('\n\tif (', 0, end) + len('\n\tif (')
    depth, cursor = 1, start
    while depth:
        depth += (source[cursor] == '(') - (source[cursor] == ')')
        cursor += 1
    return source[start:cursor - 1]


radio = assigned('j713_radio')
mtp = condition_before('dart->retained_mtp = true;')
irq = condition_before('dart->irq = platform_get_irq_optional(pdev, 0);')
radio_node = '/soc/iommu@490000000'
mtp_node = '/soc/iommu@394800000'
radio_has_irq = subprocess.run(['fdtget', str(DTB), radio_node, 'interrupts'],
                               capture_output=True).returncode == 0
assert property(mtp_node, 'compatible') == ['apple,t8110-dart']
assert {'apple,j713', 'apple,t8132'} <= set(property('/', 'compatible'))


def address(node):
    cells = [int(x, 16) for x in property(node, 'reg', 'x')]
    return (cells[0] << 32) | cells[1]


assert address(mtp_node) == 0x394800000
assert address(radio_node) == 0x490000000
radio_compat = ', '.join(json.dumps(s) for s in property(radio_node, 'compatible'))
mtp_compat = ', '.join(json.dumps(s) for s in property(mtp_node, 'compatible'))
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define DART_T8110 1
struct resource { uint64_t start; };
struct device_node { const char **compatible; unsigned int count; };
struct device { struct device_node *of_node; };
struct hw { unsigned int type; };
struct dart { struct hw *hw; bool locked, supports_bypass, preserve_display; };
static bool board, soc, bypassed;
static unsigned int checks;
static bool of_machine_is_compatible(const char *s) {
    if (!strcmp(s,"apple,j713")) return board;
    if (!strcmp(s,"apple,j715") || !strcmp(s,"apple,j604") || !strcmp(s,"apple,j623") ||
        !strcmp(s,"apple,j624") || !strcmp(s,"apple,j773g")) return false;
    assert(!strcmp(s,"apple,t8132")); return soc;
}
/* No /chosen: the qualified-firmware term is off and the Air gates decide. */
static struct device_node *of_chosen;
static int of_property_read_string(const struct device_node *n,const char *p,const char **out) {
    (void)n;(void)p;(void)out;return -22;
}
AIR_HELPER
static bool of_device_is_compatible(const struct device_node *n,const char *s) {
    for(unsigned int i=0;i<n->count;i++) if(!strcmp(n->compatible[i],s))return true;
    return false;
}
static bool apple_dart_streams_bypassed(struct dart *dart) { (void)dart;return bypassed; }
static bool radio_gate(struct device *dev,struct resource *res,bool tunneled) {
    return RADIO_EXPRESSION;
}
static bool mtp_gate(struct dart *dart,struct resource *res,bool tunneled) {
    return MTP_EXPRESSION;
}
static bool optional_irq(struct dart *dart,bool j713_radio) { (void)dart;return IRQ_EXPRESSION; }
#define CHECK(x) do { assert(x);checks++; } while(0)
int main(void) {
    const char *radio_compat[]={RADIO_COMPAT};
    const char *mtp_compat[]={MTP_COMPAT};
    struct device_node rn={radio_compat,sizeof(radio_compat)/sizeof(*radio_compat)};
    struct device_node mn={mtp_compat,sizeof(mtp_compat)/sizeof(*mtp_compat)};
    struct device dev={&rn};
    struct resource mr={MTP_ADDRESS},rr={RADIO_ADDRESS};
    struct hw hw={DART_T8110};
    struct dart dart={.hw=&hw,.supports_bypass=true};
    for(unsigned int b=0;b<2;b++)for(unsigned int s=0;s<2;s++) {
        board=b;soc=s;bypassed=true;
        CHECK(mtp_gate(&dart,&mr,false)==(b&&s));
        CHECK(radio_gate(&dev,&rr,false)==(b&&s));
        CHECK(!mtp_gate(&dart,&mr,true));
        CHECK(!radio_gate(&dev,&rr,true));
    }
    board=soc=true;
    CHECK(mtp_gate(&dart,&mr,false));
    dev.of_node=&mn;CHECK(!radio_gate(&dev,&rr,false));dev.of_node=&rn;
    CHECK(!radio_gate(&dev,&mr,false));
    CHECK(!mtp_gate(&dart,&rr,false));
    CHECK(!radio_gate(&dev,NULL,false));
    struct resource foreign={0x314800000ULL};CHECK(!mtp_gate(&dart,&foreign,false));
    dart.locked=true;CHECK(!mtp_gate(&dart,&mr,false));dart.locked=false;
    dart.supports_bypass=false;CHECK(!mtp_gate(&dart,&mr,false));dart.supports_bypass=true;
    bypassed=false;CHECK(!mtp_gate(&dart,&mr,false));bypassed=true;
    hw.type=0;CHECK(!mtp_gate(&dart,&mr,false));hw.type=DART_T8110;
    for(unsigned int display=0;display<2;display++)for(unsigned int radio=0;radio<2;radio++) {
        dart.preserve_display=display;
        CHECK(optional_irq(&dart,radio)==IRQ_EXPECTED);
    }
    printf("%u production DART admission checks passed against real J713 DT properties\n",checks);
}
'''
for name, value in {
    'AIR_HELPER': air,
    'RADIO_EXPRESSION': radio, 'MTP_EXPRESSION': mtp, 'IRQ_EXPRESSION': irq,
    'IRQ_EXPECTED': '(display||radio)' if 'preserve_display' in irq else 'radio',
    'RADIO_COMPAT': radio_compat, 'MTP_COMPAT': mtp_compat,
    'RADIO_ADDRESS': hex(address(radio_node))+'ULL',
    'MTP_ADDRESS': hex(address(mtp_node))+'ULL',
}.items():
    code = code.replace(name, value)
with tempfile.TemporaryDirectory() as directory:
    src, binary = Path(directory)/'contract.c', Path(directory)/'contract'
    src.write_text(code)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Radio DT has an IRQ.' if radio_has_irq else 'Legacy radio DT lacks an IRQ; its fallback admission is checked.')
print('MTP uses the t8110 compatible; both devices reach their intended probe paths.')
