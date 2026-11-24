#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check CIO PHY transaction admission, power ordering and unwind against fake MMIO."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    path = ROOT / 'drivers/phy/apple/atc.c'
    source = path.read_text()
    state = re.search(r'^struct atcphy_cio_sleep \{.*?\n\};', source, re.M | re.S).group()
    names = ['ATCPHY_POWER_CTRL', 'ATCPHY_POWER_STAT', 'ATCPHY_POWER_SLEEP_SMALL',
             'ATCPHY_POWER_SLEEP_BIG', 'ATCPHY_POWER_CLAMP_EN', 'ATCPHY_POWER_APB_RESET_N',
             'ATCPHY_POWER_PHY_RESET_N', 'ATCPHY_MISC', 'ATCPHY_MISC_RESET_N',
             'ATCPHY_MISC_LANE_SWAP', 'ACIOPHY_LANE_MODE_T8122', 'ACIOPHY_CROSSBAR_T8122']
    defines = '\n'.join(re.search(r'^#define ' + n + r'\s+[^\n]+', source, re.M).group()
                        for n in names)
    start = source.index('static struct apple_atcphy *atcphy_from_cio_phy(')
    end = source.index('static int atcphy_suspend(', start)
    methods = source[start:end]
    fixture = Path(__file__).with_suffix('.c').read_text().replace('/* ACTUAL_DEFINES */', defines)
    fixture = fixture.replace('/* ACTUAL_STATE */', state)
    mutations = {
        'skip-mode-admission': ('!atcphy_mode_is_cio(atcphy->mode)', 'false'),
        'drop-off-qualification': ('s->off = true;', 's->off = false;'),
        'skip-small-poll': ('!(value & ATCPHY_POWER_SLEEP_SMALL)', 'true'),
        'forget-crossbar': ('writel(s->crossbar, atcphy->regs.core + ACIOPHY_CROSSBAR_T8122);', ''),
        'forget-lane-mode': ('writel(s->lane_mode, atcphy->regs.core + ACIOPHY_LANE_MODE_T8122);', ''),
        'open-failed-restore': ('if (!s->prepared || (s->changed && !s->restored))', 'if (!s->prepared)'),
        'forget-clock-owners': ('atcphy->tunnel_users = s->clock_users;', 'atcphy->tunnel_users = 0;'),
        'forget-pd-restore': ('WRITE_ONCE(atcphy->pd_was_off, false);', ''),
        'forget-stale-ack': ('atcphy->tunnel_ack_stale = true;', 'atcphy->tunnel_ack_stale = false;'),
        'drop-owners-after-failed-restart': ('atcphy->tunnel_users = s->clock_users;\n\t\t\tfor (unsigned int i = 0; i < 2; i++) {\n\t\t\t\tatcphy->tunnel_dpin_rate[i]',
                                             'for (unsigned int i = 0; i < 2; i++) {\n\t\t\t\tatcphy->tunnel_dpin_rate[i]'),
        'no-clock-restart-after-failed-stop': ('if (s->clock_users && atcphy_cio_restart_clocks(atcphy, s))',
                                               'if (false)'),
    }
    variants = {'real': methods}
    for name, (old, new) in mutations.items():
        assert old in methods, name
        variants[name] = methods.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='atc-cio-sleep-') as tmp:
        out = args.output or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        results = {}
        for name, body in variants.items():
            unit, binary = out / (name + '.c'), out / name
            unit.write_text(fixture.replace('/* ACTUAL_FUNCTIONS */', body))
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-Wno-unused-but-set-variable', '-g',
                            '-fsanitize=address,undefined', str(unit), '-o', str(binary)], check=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            results[name] = {'exit': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
            assert (run.returncode == 0) == (name == 'real'), (name, results[name])
            if name == 'real': print(run.stdout, end='')
        (out / 'receipt.json').write_text(json.dumps({
            'sha256': hashlib.sha256(source.encode()).hexdigest(), 'results': results}, indent=2) + '\n')
        print(f'PASS {len(mutations)} negative controls')


if __name__ == '__main__':
    main()
