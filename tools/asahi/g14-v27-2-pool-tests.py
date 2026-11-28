#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production CommandPool encoder across a failed publication and retry.

GPU arrays use host storage and cache maintenance is a no-op. Publication failure
is injected after encoding, before mark_published(), as when in-flight allocation
fails. This tests command bytes and buffer rotation, not firmware retirement.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'drivers/gpu/drm/asahi'


def block(source, marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


SHIMS = r'''
#![allow(dead_code)]
type Result<T = ()> = std::result::Result<T, i32>;
const EINVAL: i32 = -22;
const ENOMEM: i32 = -12;
mod pgtable {
    pub fn clean_poc(_: *const u8, _: usize) {}
    pub fn poc_barrier() {}
}
mod fw { pub mod types {
    pub struct GpuArray<T> { pub bytes: Vec<T>, pub va: u64 }
    impl<T> GpuArray<T> {
        pub fn gpu_va(&self) -> std::num::NonZeroU64 {
            std::num::NonZeroU64::new(self.va).unwrap()
        }
        pub fn as_slice(&self) -> &[T] { &self.bytes }
        pub fn as_mut_slice(&mut self) -> &mut [T] { &mut self.bytes }
    }
} }
'''

TESTS = r'''
fn pool() -> CommandPool {
    let array = |size, va| fw::types::GpuArray { bytes: vec![0; size], va };
    CommandPool {
        commands: array(2 * POOL_SLOTS * COMMAND_STRIDE, 0x100000),
        published: 0,
        streams: array(POOL_SLOTS * STREAM_STRIDE, 0x200000),
        tracking: array(POOL_SLOTS * 0x100, 0x300000),
        cursors: array(POOL_SLOTS * 0x40, 0x400000),
        stamps: array(POOL_SLOTS * 0x80, 0x500000),
        preempt1: 0,
    }
}

fn encode(pool: &mut CommandPool, slot: usize, byte: u8, result: Result) -> Result<u64> {
    pool.encode_with(slot, 7, 11, 0x100, |cmd, stream, common| {
        assert_eq!(common.context, 7);
        assert_eq!(common.sequence, 11);
        assert_eq!(common.stamp_value, 0x100);
        cmd.fill(byte);
        stream.fill(byte);
        result
    })
}

fn command(pool: &CommandPool, va: u64) -> &[u8] {
    let at = (va - pool.commands.gpu_va().get()) as usize;
    &pool.commands.as_slice()[at..at + COMMAND_STRIDE]
}

// Model a fallible in-flight reservation after the production encoder succeeds.
fn submit(pool: &mut CommandPool, slot: usize, byte: u8, reservation: Result) -> Result<u64> {
    let va = encode(pool, slot, byte, Ok(()))?;
    reservation?;
    pool.mark_published(slot);
    Ok(va)
}

#[test]
fn failed_publication_retries_the_spare_and_preserves_the_last_command() {
    let mut pool = pool();
    // Visit every slot, including the highest mask bit, and both buffer parities.
    for slot in 0..POOL_SLOTS {
        let mut last = submit(&mut pool, slot, 0xa5, Ok(())).unwrap();
        for _ in 0..4 {
            let before = pool.published;
            let saved = pool.commands.as_slice().to_vec();
            let spare = encode(&mut pool, slot, 0x5a, Ok(())).unwrap();
            assert_ne!(spare, last);
            // Repeated failures must not rotate back to the published command.
            for _ in 0..3 {
                assert_eq!(submit(&mut pool, slot, 0x33, Err(ENOMEM)), Err(ENOMEM));
                let retry = encode(&mut pool, slot, 0x5a, Ok(())).unwrap();
                assert_eq!(retry, spare, "failed publish must reuse the spare");
                assert!(command(&pool, last).iter().all(|&b| b == 0xa5));
                assert_eq!(pool.published, before, "encoding must not publish");
            }
            // Only the spare's bytes may change, including across other slots.
            let at = (spare - pool.commands.gpu_va().get()) as usize;
            assert_eq!(&pool.commands.as_slice()[..at], &saved[..at]);
            assert_eq!(&pool.commands.as_slice()[at + COMMAND_STRIDE..],
                       &saved[at + COMMAND_STRIDE..]);
            let published = submit(&mut pool, slot, 0xa5, Ok(())).unwrap();
            assert_eq!(published, spare);
            assert_eq!(pool.published, before ^ (1 << slot));
            assert_eq!(encode(&mut pool, slot, 0x5a, Ok(())), Ok(last));
            assert!(command(&pool, published).iter().all(|&b| b == 0xa5));
            last = published;
        }
    }
}

#[test]
fn encoder_error_also_leaves_the_published_command_alone() {
    let mut pool = pool();
    for slot in 0..POOL_SLOTS {
        let last = submit(&mut pool, slot, 0xa5, Ok(())).unwrap();
        let before = pool.published;
        let spare = encode(&mut pool, slot, 0x5a, Ok(())).unwrap();
        assert_eq!(encode(&mut pool, slot, 0x33, Err(EINVAL)), Err(EINVAL));
        assert_eq!(encode(&mut pool, slot, 0x5a, Ok(())), Ok(spare));
        assert!(command(&pool, last).iter().all(|&b| b == 0xa5));
        assert_eq!(pool.published, before);
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mutation', choices=['flip-on-encode', 'no-publish-flip'])
    args = parser.parse_args()
    source = (DRIVER / 'gpu/v27_2.rs').read_text()
    abi = (DRIVER / 'g14_v27_2_abi.rs').read_text()
    render = (DRIVER / 'g14_v27_2_render.rs').read_text()
    encoder = block(source, '    pub(crate) fn encode_with(')
    publish = block(source, '    pub(crate) fn mark_published(')
    flip = 'self.published ^= 1 << i;'
    assert publish.count(flip) == 1
    if args.mutation == 'flip-on-encode':
        selection = 'let c = g14_v27_2_abi::pool_command(i, g14_v27_2_abi::spare_buffer(self.published, i));'
        assert encoder.count(selection) == 1
        encoder = encoder.replace(selection, selection + '\n' + flip)
        publish = publish.replace(flip, '')
    elif args.mutation == 'no-publish-flip':
        publish = publish.replace(flip, '')
    constants = '\n'.join(re.search(r'^(?:pub\(crate\) )?const ' + name + r':.*;',
                                    source, re.M).group()
                          for name in ['POOL_SLOTS', 'COMMAND_STRIDE', 'STREAM_STRIDE'])
    helpers = '\n'.join(block(abi, 'pub(crate) fn ' + name + '(')
                        for name in ['pool_command', 'spare_buffer'])
    program = (SHIMS + constants + '\nmod g14_v27_2_abi {\n' + helpers + '\n}\n'
               + 'mod g14_v27_2_render {\n' + block(render, 'pub(crate) struct Common {')
               + '\n}\n' + block(source, 'pub(crate) struct CommandPool {')
               + '\nimpl CommandPool {\n' + encoder + '\n' + publish + '\n}\n' + TESTS)
    args.out.mkdir(parents=True, exist_ok=True)
    fixture = args.out / 'pool.rs'
    fixture.write_text(program)
    binary = args.out / 'pool-tests'
    subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', '--test',
                    str(fixture), '-o', str(binary)], check=True)
    raise SystemExit(subprocess.run([str(binary), '--nocapture']).returncode)


if __name__ == '__main__':
    main()
