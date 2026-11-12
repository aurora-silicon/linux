#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile baseline and opt-in PMP DTs, checking the supplier/consumer graph."""
import argparse
import os
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def get(dtb, node, prop, kind='s', default=None):
    result = subprocess.run(['fdtget', '-t', kind, str(dtb), node, prop],
                            capture_output=True, text=True)
    if result.returncode:
        if default is not None:
            return default
        raise AssertionError(f'{dtb.name} {node} {prop}: {result.stderr}')
    return result.stdout.split()


def check(tree, output, dtc, board, soc, pmp_addr, report_addr, trial, avd_vmax=False):
    profile = "pmp-avd-vmax" if avd_vmax else "pmp" if trial else "baseline"
    dtb = output / f'{board}-{profile}.dtb'
    command = shlex.split(os.environ.get('CPP', 'cc -E')) + [
        '-nostdinc', '-undef', '-D__DTS__', '-I',
        str(tree / 'scripts/dtc/include-prefixes'), '-x', 'assembler-with-cpp']
    if trial:
        command.append('-DAPPLE_USE_PMP')
    board_dir = tree / 'arch/arm64/boot/dts/apple'
    if avd_vmax:
        assert soc == 't8112' and trial
        # A runtime loader requires a sole physical core and an unclaimed
        # bit 10 before installing its core/perf graph.
        wrapper = ('#include "t8112-j413.dts"\n'
                   '&avd { power-domains = <&ps_avd_sys>; };\n'
                   '&avd { /delete-property/ power-domain-names; };\n'
                   '&pmp_report_avd_sys { status = "disabled"; };\n')
        (output / f'{board}-{profile}.dts').write_text(wrapper)
        command += ['-I', str(board_dir), '-']
        source = subprocess.check_output(command, input=wrapper.encode())
    else:
        command.append(str(board_dir / f'{board}.dts'))
        source = subprocess.check_output(command)
    subprocess.run([dtc, '-q', '-@', '-I', 'dts', '-O', 'dtb', '-o', str(dtb)],
                   input=source, check=True)
    pmp = f'/soc/pmp@{pmp_addr:x}'
    report = f'/soc/pmp_report@{report_addr:x}'
    assert get(dtb, pmp, 'status') == ['okay' if trial else 'disabled']
    assert get(dtb, '/aliases', 'pmp', default=[]) == ([pmp] if trial else [])
    assert 'apple,t6000-pmp-v2' in get(dtb, pmp, 'compatible')
    assert get(dtb, pmp, 'reg-names') == ['pmp', 'asc']
    regs = [int(v, 16) for v in get(dtb, pmp, 'reg', 'x')]
    assert (regs[0] << 32 | regs[1]) == pmp_addr
    assert regs[3] == (0x80000 if soc == 't8112' else 0x100000)
    assert regs[7] == 0x4000
    for prop in ('mboxes', 'iommus', 'power-domains'):
        assert get(dtb, pmp, prop, 'x')
    assert get(dtb, pmp, 'mbox-names') == ['mbox']
    # These must come from this boot's ADT, including in an opted-in DTB.
    for prop in ('apple,board-id', 'apple,dram-vendor-id', 'apple,dram-capacity',
                 'apple,tunable-soc-device', 'apple,tunable-ptd-range'):
        assert get(dtb, pmp, prop, 'x', default=[]) == []
    if soc == 't6000':
        assert len(get(dtb, pmp, 'apple,pio-ranges', 'x')) == 7 * 4
    else:
        assert get(dtb, pmp, 'apple,pio-ranges', 'x', default=[]) == []
        report_phandle = get(dtb, report, 'phandle', 'x')[0]
        assert get(dtb, pmp, 'apple,pmp-report', 'x', default=[]) == (
            [report_phandle] if trial else [])
    # All existing macro-selected consumers must use report domains together.
    # A status-only change would miss these edges.
    consumers = [('dcp', 'pmp_report_disp0'), ('avd', 'pmp_report_avd_sys')]
    if soc == 't8112' and not avd_vmax:
        consumers += [('m2_jpeg0', 'pmp_report_jpg'),
                      ('m2_jpeg1', 'pmp_report_jpg')]
    for device, entry in consumers:
        device_path = get(dtb, '/__symbols__', device)[0]
        entry_path = get(dtb, '/__symbols__', entry)[0]
        power = get(dtb, device_path, 'power-domains', 'x')
        expected = get(dtb, entry_path, 'phandle', 'x')[0]
        uses_report = trial and not (avd_vmax and device == 'avd')
        if (soc == 't8112' and device in ('avd', 'm2_jpeg0', 'm2_jpeg1') and
                trial and not avd_vmax):
            if device == 'avd':
                core = get(dtb, device_path, 'resets', 'x')
                assert power == [core[0], expected]
            else:
                assert len(power) == 2 and power[1] == expected
            assert get(dtb, device_path, 'power-domain-names') == ['core', 'perf']
        else:
            assert (int(power[0], 16) == int(expected, 16)) == uses_report
            if soc == 't8112' and device == 'avd':
                assert power == get(dtb, device_path, 'resets', 'x')
                assert get(dtb, device_path, 'power-domain-names', default=[]) == []
        if avd_vmax and device == 'avd':
            assert get(dtb, entry_path, 'status') == ['disabled']
    if soc == 't8112':
        gpu = get(dtb, '/__symbols__', 'gpu')[0]
        gfx = get(dtb, '/__symbols__', 'pmp_report_gfx')[0]
        power = get(dtb, gpu, 'power-domains', 'x')[0]
        expected = get(dtb, gfx, 'phandle', 'x')[0]
        assert (int(power, 16) == int(expected, 16)) == trial
        mcc = get(dtb, '/__symbols__', 'm2_mcc')[0]
        pmp_phandle = get(dtb, pmp, 'phandle', 'x')[0]
        assert get(dtb, mcc, 'apple,pmp', 'x', default=[]) == (
            [pmp_phandle] if trial else [])
    if soc == 't8112':
        # Media performance requests come from the bootloader's ADT copy, and
        # only in the opt-in profile: the baseline DTB must not carry them.
        for entry in ('pmp_report_avd_sys', 'pmp_report_jpg'):
            path = get(dtb, '/__symbols__', entry)[0]
            for prop in ('apple,dvfs', 'apple,dvfs-request-class',
                         'apple,dvfs-request-levels'):
                assert get(dtb, path, prop, 'x', default=[]) == []
            adt = subprocess.run(['fdtget', str(dtb), path, 'apple,adt-dvfs'],
                                 capture_output=True).returncode == 0
            assert adt == trial
        for cluster in ('cpufreq_e', 'cpufreq_p'):
            path = get(dtb, '/__symbols__', cluster)[0]
            vote = get(dtb, path, 'apple,pmp-memory-vote', 'x', default=[])
            assert bool(vote) == trial
    assert get(dtb, report, 'apple,dcs-state-count', 'u') == ['5' if soc == 't8112' else '4']
    print(f'{dtb.name}: supplier resources, calibration provenance and report-domain graph PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--dtc', required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    # The T6000 boards build with APPLE_USE_PMP and keep the PMP node as it is.
    for board, soc, pmp, report in (
            ('t8112-j413', 't8112', 0x23b500000, 0x23b3c0000),):
        for trial in (False, True):
            check(args.tree.resolve(), args.output, args.dtc, board, soc, pmp, report, trial)
        check(args.tree.resolve(), args.output, args.dtc, board, soc, pmp, report, True, True)


if __name__ == '__main__':
    main()
