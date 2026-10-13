#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fault-inject production Apple DWC3 teardown and ATC recovery; retain artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument('--out', type=Path, required=True)
p.add_argument('--case', choices=('all', 'power', 'reset', 'legacy'), default='all')
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
files = {n: (args.source / f).read_text() for n, f in {
    'atc': 'drivers/phy/apple/atc.c',
    'phy': 'drivers/phy/phy-core.c',
    'core': 'drivers/usb/dwc3/core.c',
    'apple': 'drivers/usb/dwc3/dwc3-apple.c',
    'state': 'drivers/usb/dwc3/dwc3-apple-state.h',
}.items()}

def block(s, start):
    a = s.index('{', start)
    depth, b = 1, a + 1
    while depth:
        depth += (s[b] == '{') - (s[b] == '}')
        b += 1
    return s[start:b]

def fn(n, name):
    s = files[n]
    m = re.search(r'^(?:static [^\n]*\b|(?:int|void) )' + name + r'\(', s, re.M)
    assert m, (n, name)
    return block(s, m.start())

decls = []
for name in ('atcphy_generation', 'atcphy_lane_mode', 'atcphy_pipehandler_state', 'atcphy_mode', 'atcphy_dp_link_rate'):
    decls.append(block(files['atc'], files['atc'].index('enum ' + name + ' {')) + ';')
for name in ('atcphy_mode_configuration', 'atcphy_hw', 'atcphy_dp_link_rate_configuration'):
    decls.append(block(files['atc'], files['atc'].index('struct ' + name + ' {')) + ';')
decls.append(block(files['state'], files['state'].index('enum dwc3_apple_state {')) + ';')
start = files['atc'].index('static const struct {\n\tconst struct atcphy_mode_configuration normal;')
end = files['atc'].index('\n};', start) + 3
decls.append(files['atc'][start:end])
start = files['atc'].index('static const struct atcphy_dp_link_rate_configuration dp_lr_config[]')
end = files['atc'].index('\n};', start) + 3
decls.append(files['atc'][start:end])

names = {
    'atc': ('atcphy_pipehandler_lock', 'atcphy_pipehandler_unlock',
            'atcphy_pipehandler_check', 'atcphy_pipehandler_set_mux',
            'atcphy_clear_nonselected_phy_reset',
            'atcphy_configure_pipehandler_usb3_host_t8103',
            'atcphy_configure_pipehandler_usb3_host_t8122',
            'atcphy_configure_pipehandler_usb3', 'atcphy_configure_pipehandler_usb4',
            'atcphy_enable_dummy_phy', 'atcphy_configure_pipehandler_dummy',
            'atcphy_configure_pipehandler', 'atcphy_park_pipehandler',
            'atcphy_get_mode_config', 'atcphy_dp_configure',
            'j700_field_update', 'j700_aux_disable', 'atcphy_disable_dp_aux',
            'atcphy_power_off', 'atcphy_power_on', 'atcphy_configure', 'atcphy_usb2_init', 'atcphy_usb3_power_off',
            'atcphy_usb3_set_mode', 'atcphy_usb2_power_off',
            '_atcphy_dwc3_reset_assert', 'atcphy_dwc3_reset_assert', 'atcphy_mux_set'),
    'phy': ('phy_init', 'phy_exit', 'phy_power_on', 'phy_power_off'),
    'core': ('dwc3_phy_power_on', 'dwc3_phy_power_off', 'dwc3_phy_exit', 'dwc3_core_exit'),
    'apple': ('dwc3_apple_core_stop', 'dwc3_apple_core_init', 'dwc3_apple_core_start',
              'dwc3_apple_init', 'dwc3_apple_exit', 'dwc3_usb_role_switch_set'),
}
names['core'] = ('dwc3_phy_power_on', 'dwc3_phy_power_off', 'dwc3_phy_exit',
                 'dwc3_core_exit_after_phy_power_off', 'dwc3_core_exit',
                 'dwc3_core_remove_common', 'dwc3_core_remove', 'dwc3_core_remove_no_shutdown')
names['atc'] += ('atcphy_dwc3_reset_deassert',)
names = {n: tuple(name for name in group if re.search(r'^(?:static [^\n]*\b|(?:int|void) )' + name + r'\(', files[n], re.M))
         for n, group in names.items()}
functions = '\n\n'.join(fn(n, name) for n, group in names.items() for name in group)
definitions = []
lines = iter(files['atc'].splitlines())
for line in lines:
    if not line.startswith('#define '):
        continue
    definition = [line]
    while definition[-1].endswith('\\'):
        definition.append(next(lines))
    definitions.append('\n'.join(definition))
definitions = '\n'.join(definitions)
here = Path(__file__).resolve().parent
fallback = '' if 'dwc3_core_remove_no_shutdown' in names['core'] else '#define dwc3_core_remove_no_shutdown dwc3_core_remove\n'
unit = ((here / 'apple-dwc3-teardown/prefix.c').read_text() + '\n' + definitions + '\n' +
        '\n'.join(decls) + '\n' + (here / 'apple-dwc3-teardown/mocks.c').read_text() + '\n' +
        functions + '\n' + fallback + (here / 'apple-dwc3-teardown/cases.c').read_text())
(args.out / 'extracted-production.c').write_text(functions)
(args.out / 'host.c').write_text(unit)
metadata = {'functions': names, 'extracted_sha256': hashlib.sha256(functions.encode()).hexdigest(),
            'sources': {n: hashlib.sha256(s.encode()).hexdigest() for n, s in files.items()}}
(args.out / 'sources.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(json.dumps(metadata), flush=True)
subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                str(args.out / 'host.c'), '-o', str(args.out / 'host')], check=True)
subprocess.run([str(args.out / 'host'), args.case], check=True)
