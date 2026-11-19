#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile Bluetooth recovery admission with the production PCI and chip tables."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import subprocess


def sha(s):
    return hashlib.sha256(s.encode()).hexdigest()


def take(source, pattern):
    match = re.search(pattern, source, re.M | re.S)
    assert match, pattern
    return match.group()


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t u32;
struct bcm4377_data;
struct firmware { int unused; };
static int bcm4377_send_ptb(struct bcm4377_data *b,const struct firmware *f) { (void)b;(void)f;return 0; }
static int bcm4378_send_ptb(struct bcm4377_data *b,const struct firmware *f) { (void)b;(void)f;return 0; }
static int bcm4378_send_calibration(struct bcm4377_data *b) { (void)b;return 0; }
static int bcm4387_send_calibration(struct bcm4377_data *b) { (void)b;return 0; }
static int bcm4388_send_calibration(struct bcm4377_data *b) { (void)b;return 0; }
struct pci_device_id { unsigned int vendor,device,subvendor,subdevice,class,class_mask;unsigned long driver_data; };
struct bcm4377_data { const struct bcm4377_hw *hw; };
struct hci_dev { void (*reset)(struct hci_dev *); };
static void bcm4377_hci_timeout(struct hci_dev *hdev) { (void)hdev; }
static const char *board;
static bool of_machine_is_compatible(const char *compat) { assert(!strcmp(compat,"apple,j493"));return !strcmp(board,compat); }
'''

SUFFIX = r'''
static bool probe_registration(struct bcm4377_data *bcm4377,const struct pci_device_id *id,struct hci_dev *hdev) {
@SELECTION@
@REGISTRATION@
    return hdev->reset==bcm4377_hci_timeout;
}
int main(void) {
    unsigned cases=0;
    const char *boards[]={"apple,j493","apple,j273","apple,j293","apple,j313","apple,j274","apple,j375","apple,j473","apple,j474","apple,j414s","apple,j416c","apple,j613","apple,j615","apple,j514s","apple,j700"};
    assert(bcm4377_hw_variants[BCM4378].id==0x4378);
    assert(bcm4377_hw_variants[BCM4378].id!=4378);
    for(unsigned chip=0;chip<4;chip++) {
        const struct pci_device_id *id=&bcm4377_devid_table[chip];
        assert(id->vendor==PCI_VENDOR_ID_BROADCOM);
        assert(id->driver_data==chip);
        for(unsigned b=0;b<sizeof(boards)/sizeof(boards[0]);b++) {
            board=boards[b];struct bcm4377_data data={};struct hci_dev hdev={};
            bool expected=id->device==BCM4378_DEVICE_ID && !strcmp(board,"apple,j493");
            assert(probe_registration(&data,id,&hdev)==expected);
            assert(data.hw==&bcm4377_hw_variants[id->driver_data]);
            assert(bcm4377_resume_recovery_supported(&data)==expected);
            if(expected)assert(hdev.reset==bcm4377_hci_timeout);else assert(!hdev.reset);
            cases++;
        }
    }
    board="apple,j493";
    struct bcm4377_hw wrong=bcm4377_hw_variants[BCM4378];wrong.id=4378;
    struct bcm4377_data decimal={.hw=&wrong};assert(!bcm4377_resume_recovery_supported(&decimal));cases++;
    printf("%u table-backed identity and callback cases PASS\n",cases);return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--old-ref', required=True)
    ap.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.out.mkdir(parents=True, exist_ok=True)
    current = (root / 'drivers/bluetooth/hci_bcm4377.c').read_text()
    old = subprocess.check_output(['git', 'show', args.old_ref + ':drivers/bluetooth/hci_bcm4377.c'], cwd=root, text=True)
    patterns = {
        'enum': r'^enum bcm4377_chip \{.*?^\};',
        'hw_struct': r'^struct bcm4377_hw \{.*?^\};',
        'variants': r'^static const struct bcm4377_hw bcm4377_hw_variants\[\] = \{.*?^\};',
        'pci_macro_table': r'^#define BCM4377_DEVID_ENTRY\(id\).*?^static const struct pci_device_id bcm4377_devid_table\[\] = \{.*?^\};',
        'selection': r'^\tbcm4377->hw = &bcm4377_hw_variants\[id->driver_data\];',
        'registration': r'^\tif \(bcm4377_resume_recovery_supported\(bcm4377\)\)\n\t\thdev->reset = bcm4377_hci_timeout;',
        'gate': r'^static bool bcm4377_resume_recovery_supported\([^;]*?\)\n\{.*?^\}',
    }
    pieces = {name: take(current, pattern) for name, pattern in patterns.items()}
    old_pieces = {name: take(old, pattern) for name, pattern in patterns.items()}
    # The old gate is compiled against the current tables. Later commits added
    # hardware fields and variant settings, so the struct and table may differ,
    # but the old gate must still see the same chip IDs, PCI table and probe code.
    for name in patterns:
        if name not in ('gate', 'hw_struct', 'variants'):
            assert pieces[name] == old_pieces[name], name
    for src in (pieces, old_pieces):
        assert re.search(r'^\tunsigned int id;$', src['hw_struct'], re.M), src['hw_struct']
    variant_ids = r'^\t\[(BCM\d+)\] = \{\n\t\t\.id = (0x[0-9a-f]+),'
    current_ids = re.findall(variant_ids, pieces['variants'], re.M)
    assert len(current_ids) == 4, current_ids
    assert current_ids == re.findall(variant_ids, old_pieces['variants'], re.M), 'variant ids'
    defs = '\n'.join(re.findall(r'^#define BCM\d+_DEVICE_ID [^\n]+', current, re.M))
    assert defs == '\n'.join(re.findall(r'^#define BCM\d+_DEVICE_ID [^\n]+', old, re.M))
    ids = (root / 'include/linux/pci_ids.h').read_text()
    pci_defs = '\n'.join(re.findall(r'^#define (?:PCI_VENDOR_ID_BROADCOM|PCI_CLASS_NETWORK_OTHER)\s+[^\n]+', ids, re.M))
    any_id = take((root / 'include/linux/device-id/pci.h').read_text(), r'^#define PCI_ANY_ID[^\n]+')
    # Register offsets the variant table names by macro, as defined in the driver.
    table_macros = sorted(set(re.findall(r'\b[A-Z][A-Z0-9_]+\b', pieces['variants'])))
    hw_defs = '\n'.join(m.group() for name in table_macros for m in [re.search(r'^#define ' + name + r'\b[^\n]*', current, re.M)] if m)
    assert 'BAR2_CONTEXT_ADDR_HI' in hw_defs, hw_defs
    source_prefix = PREFIX + defs + '\n' + hw_defs + '\n' + pci_defs + '\n' + any_id + '\n' + '\n'.join(pieces[n] for n in ('enum', 'hw_struct', 'variants', 'pci_macro_table'))
    suffix = SUFFIX.replace('@SELECTION@', pieces['selection']).replace('@REGISTRATION@', pieces['registration'])
    gates = {'positive': pieces['gate'], 'published-original': old_pieces['gate']}
    assert pieces['gate'].count('0x4378') == 1
    gates['decimal-id'] = pieces['gate'].replace('0x4378', '4378')
    gates['other-chip'] = pieces['gate'].replace('0x4378', '0x4387')
    gates['board-bypass'] = pieces['gate'].replace('of_machine_is_compatible("apple,j493")', 'true')
    results = {}
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    for name, gate in gates.items():
        source = args.out / f'{name}.c'
        source.write_text(source_prefix + gate + suffix)
        binary = source.with_suffix('')
        subprocess.run([args.cc, '-std=gnu11', '-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(source), '-o', str(binary)], check=True)
        run = subprocess.run([str(binary)], capture_output=True, text=True, preexec_fn=no_core)
        (args.out / f'{name}.log').write_text(run.stdout + run.stderr)
        assert run.returncode == (0 if name == 'positive' else -6), (name, run.returncode)
        results[name] = {'exit': run.returncode, 'source_sha256': sha(source.read_text()), 'stdout': run.stdout.strip()}
    receipt = {'source_sha256': sha(current), 'old_source_sha256': sha(old), 'old_ref': args.old_ref, 'actual_inputs': {name: sha(value) for name, value in pieces.items()}, 'device_ids_sha256': sha(defs), 'table_macros_sha256': sha(hw_defs), 'variant_ids': current_ids, 'pci_defs_sha256': sha(pci_defs + any_id), 'results': results, 'scope': 'Actual variant/PCI tables, hardware struct, probe selection statement, reset registration block and admission function. PCI enumeration, OF board matching and unrelated probe operations are controlled; no full probe or hardware execution.'}
    (args.out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))

if __name__ == '__main__':
    main()
