#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check render-job stamp addresses against the production command encoder."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DIRECTORY = 'drivers/gpu/drm/asahi/'
MODULES = ('g16_render', 'g16_compute', 'g16_render_command', 'g16_render_state')

CONTROL = r'''
mod g16_render; mod g16_compute; mod g16_render_command; mod g16_render_state;
use g16_render::{Stage, BuildError};
struct Notification { stamp: u64, fw_stamp: u64 }
fn main() {
 let user = 0xffff_fc20_0078_0000u64;
 let fw = 0xffff_fc20_007c_0000u64;
 let notifications = [Notification { stamp: user, fw_stamp: fw }]; let i = 0;
 for stage in [Stage::Tiling, Stage::Fragment] {
  let mut command = [0u8; 0xca0]; let mut sequence = [0u8; 0x400];
  let wire = g16_render_command::layout(stage);
  let args = g16_render_command::Args {
   command: 0xffff_fc20_0010_0000, sku: 0xffff_fc20_0014_0000,
   queue: 0xffff_fc20_0018_0000, stats: 0xffff_fc20_001c_0000,
   pb_slot: 0xffff_fc20_0020_0000, manager: 0xffff_fc20_0024_0000,
   pb_aux: 0xffff_fc20_0028_0000, notifier: 0xffff_fc20_0030_0000,
   STAMP_ARGUMENTS
   uma: 0xffff_fc20_0040_0000, uma_aux: 0x100000000,
   timestamp_storage: 0xffff_fc20_0050_0000,
   context: 1, generation: 1, counter: 1, uuid: 0x16000001,
   stamp: 0x100, event_slot: 0, fragment_slot: 1,
   fragment_stamp: 0x100, pb_id: 1, pass_id: 0,
  };
  assert_eq!(g16_render_command::encode(&mut command, &mut sequence, stage, args), Ok(()));
  let meta = if stage == Stage::Tiling { 0x878 } else { 0xbc0 };
  let get = |bytes: &[u8], offset| u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap());
  assert_eq!(get(&command, meta), user, "JobMeta shared/user stamp");
  assert_eq!(get(&command, meta + 8), fw, "JobMeta internal stamp");
  let finalize_pointer = if stage == Stage::Tiling { 0x48 } else { 0x0c };
  assert_eq!(get(&sequence, wire.finalize + finalize_pointer), fw, "Finalize internal stamp");
  let mut dependency = [0u8; g16_render_command::dependency::SIZE];
  assert_eq!(g16_render_command::render_dependency(&mut dependency, fw, 0, 0x100, 0x100), Ok(()));
  assert_eq!(get(&dependency, g16_render_command::dependency::STAMP), fw);
  assert_eq!(get(&dependency, g16_render_command::dependency::STAMP_COPY), fw);
  let mut bad = args; bad.INTERNAL_FIELD = 0;
  assert_eq!(g16_render_command::encode(&mut command, &mut sequence, stage, bad), Err(BuildError::Address));
 }
 println!("PASS render-job stamp binding, tiling/fragment JobMeta, Finalize, dependencies and invalid address");
}
'''


def check(ref=None):
    def source(name):
        if ref:
            return subprocess.check_output(['git', 'show', ref + ':' + DIRECTORY + name],
                                           cwd=ROOT, text=True)
        return (ROOT / DIRECTORY / name).read_text()

    job = source('g16_render_job.rs')
    bindings = {}
    for aliases in (('user_stamp', 'shared_stamp'), ('fw_stamp', 'stamp_address')):
        matches = [(field, expression) for field in aliases for expression in
                   re.findall(r'\b' + field + r'\s*:\s*(notifications\[i\]\.(?:fw_stamp|stamp))\b', job)]
        if len(matches) != 1:
            raise ValueError('expected one render-job notification binding for ' + str(aliases))
        bindings[matches[0][0]] = matches[0][1]
    internal = 'fw_stamp' if 'fw_stamp' in bindings else 'stamp_address'
    control = CONTROL.replace('STAMP_ARGUMENTS', ', '.join(k + ': ' + v for k, v in bindings.items()) + ',')
    control = control.replace('INTERNAL_FIELD', internal)
    with tempfile.TemporaryDirectory(prefix='g16-stamps-') as directory:
        out = Path(directory)
        for module in MODULES:
            (out / (module + '.rs')).write_text(source(module + '.rs'))
        (out / 'main.rs').write_text(control)
        subprocess.run(['rustc', '--edition=2021', '-Awarnings', str(out / 'main.rs'),
                        '-o', str(out / 'control')], check=True)
        return subprocess.run([str(out / 'control')], capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--old-ref', help='also require the original stamp wiring to fail')
    args = parser.parse_args()
    if not shutil.which('rustc'):
        parser.error('rustc is required')
    current = check()
    print(current.stdout, end='')
    if current.returncode:
        raise SystemExit(current.stderr)
    if args.old_ref:
        old = check(args.old_ref)
        if not old.returncode or 'JobMeta shared/user stamp' not in old.stderr:
            raise SystemExit('original stamp wiring did not fail the shared/user stamp control')
        print('PASS original stamp wiring rejects shared/user stamp control')


if __name__ == '__main__':
    main()
