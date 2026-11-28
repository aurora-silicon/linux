#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Touch ID factory-record host tests, against the production readers or one mutant.

tools/asahi/sep-factory/records.rs builds drivers/soc/apple/apfs_read.rs and
sensor_factory.rs unchanged with `rustc --test` and runs them on synthetic
APFS containers. --mutation rewrites exactly one site in a copy of them; the
tests must then fail, which run-host-tests.py counts as a kill. A mutation
whose anchor is gone fails without a kill, so a stale one cannot pass.

Usage: sep-factory-records-tests.py --out DIR [--mutation NAME]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
APFS = 'drivers/soc/apple/apfs_read.rs'
FACTORY = 'drivers/soc/apple/sensor_factory.rs'
TESTS = 'tools/asahi/sep-factory/records.rs'

# name -> (file, exact text, replacement)
MUTATIONS = {
    # Objects are used without their checksum.
    'no-checksum': (APFS, '        if le64(&data, 0)? != checksum(&data) {\n'
                          '            return Err(Refusal::Malformed);\n'
                          '        }\n        Ok(data)', '        Ok(data)'),
    # A superblock is a checkpoint without its mappings.
    'incomplete-checkpoint': (APFS, '                && this\n'
                                    '                    .complete(&candidate, paddr, desc_base, desc_count)\n'
                                    '                    .is_ok()\n', ''),
    # The object map gives the first version, or one from a later transaction.
    'oldest-version': (APFS, 'if key.0 == oid && key.1 <= xid {', 'if key.0 == oid && key.1 <= xid && found.is_none() {'),
    'future-version': (APFS, 'if key.0 == oid && key.1 <= xid {', 'if key.0 == oid {'),
    # Layouts the reader does not handle are read anyway.
    'hashed-nodes': (APFS, 'flags & (NODE_HASHED | NODE_NOHEADER) != 0', 'flags & NODE_NOHEADER != 0'),
    'encrypted-volume': (APFS, 'if le64(&sb, 264)? & FS_UNENCRYPTED == 0 {', 'if false {'),
    'encrypted-extent': (APFS, 'if le64(val, 16)? != 0 {', 'if false {'),
    'compressed': (APFS, 'if bsd & UF_COMPRESSED != 0 {', 'if false {'),
    'hole': (APFS, 'if len_flags >> 56 != 0 || paddr == 0 {', 'if len_flags >> 56 != 0 {'),
    'gap': (APFS, 'if logical != covered || len == 0', 'if len == 0'),
    'no-size-limit': (APFS, 'if size > max as u64 {', 'if false {'),
    # Another Mac's container is used.
    'no-binding': (FACTORY, 'Err(Refusal::Absent) => return Err(Refusal::Foreign),',
                   'Err(Refusal::Absent) => Entry { id: 0, kind: DT_DIR },'),
    # One of several calibrations is used, or one from another sensor.
    'first-calibration': (FACTORY, '            1 => listing.calibration.ok_or(Refusal::Absent)?,\n'
                                   '            _ => return Err(Refusal::Ambiguous),',
                          '            _ => listing.calibration.ok_or(Refusal::Absent)?,'),
    'no-pairing': (FACTORY, '.is_some_and(|root| root.sensor() != found.sensor())',
                   '.is_some_and(|root| root.sensor() != found.sensor() && false)'),
    # The trust bundle is not the one in use.
    'trust-bytes': (FACTORY, 'if *self.read(*id)? != *trust {', 'if false {'),
    'trust-name': (FACTORY, 'if (self.sha256)(&trust)? != digest {', 'if false {'),
    # The standalone image is handed over as stored, or a standalone FSCl is rebuilt.
    'no-reseal': (FACTORY, 'Ok((rebuilt, true))', 'Ok((bytes, rebuilt.is_empty()))'),
    'standalone-fscl': (FACTORY, 'const STANDALONE_CALIBRATION: [&[u8]; 1] = [b"FSC2"];',
                        'const STANDALONE_CALIBRATION: [&[u8]; 2] = [b"FSC2", b"FSCl"];'),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = ap.parse_args()

    tree = args.out / 'tree'
    if tree.exists():
        shutil.rmtree(tree)
    for rel in (APFS, FACTORY, TESTS):
        (tree / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / rel, tree / rel)
    if args.mutation:
        rel, old, new = MUTATIONS[args.mutation]
        text = (tree / rel).read_text()
        if text.count(old) != 1:
            print(f'mutation {args.mutation}: its anchor occurs {text.count(old)} times in {rel}', file=sys.stderr)
            return 2
        (tree / rel).write_text(text.replace(old, new))

    binary = args.out / 'tests'
    build = subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', '--test',
                            str(tree / TESTS), '-o', str(binary)])
    if build.returncode:
        print(f'build failed: exit {build.returncode}', file=sys.stderr)
        return 2
    return subprocess.run([str(binary), '--test-threads=1']).returncode


if __name__ == '__main__':
    sys.exit(main())
