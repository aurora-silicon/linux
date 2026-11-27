#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test: G17 GET_PARAMS reports DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL exactly where the
kernel fans compute out itself.

Compiles the production board selection of drivers/gpu/drm/asahi/g17/pairs.rs
(`select_board()`, `compute_channels_per_owner()`), `multi_channel()` of
g17/fw/m5/compute.rs, `Board::compute_channels()` and every board's
`compute_channels` value of g17/fw/m5/initdata.rs, `compute_channel_features()`
of g17/mod.rs and the feature constant of file.rs, the latter against every
DRM_ASAHI_FEATURE_* bit that include/uapi/drm/asahi_drm.h defines. It checks
that the bit is set exactly when a logical compute queue may hold more than one
physical compute channel: on the M5 Pro (J714), and neither on the M5 Max,
the M5 Ultra and the M5 (J716C, J775D, J813), whose boards run one firmware
compute queue per command queue, nor on GPUs that never select an M5 board
(G17P), so userspace keeps its own sibling queues there; that the decision reads
the board's channel count alone, never its encoding forms; that GET_PARAMS
reports it from the probed board's channel count on every G17; and that the bit
is distinct from every other feature bit.

Each mutation drops one part of the rule and must fail an assertion of the
compiled harness.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASAHI = ROOT / 'drivers/gpu/drm/asahi'
G17 = ASAHI / 'g17'
UAPI = ROOT / 'include/uapi/drm/asahi_drm.h'


def item(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def ordered_ok(source, *parts):
    at = -1
    for part in parts:
        idx = source.find(part, at + 1)
        if idx < 0:
            return False
        at = idx
    return True


REPORTED = '                | compute_channel_features(pairs::compute_channels_per_owner());\n'

# name -> [(file, old, new)]: each must apply exactly once.
MUTATIONS = {
    'always-multi': [('mod', '    if channels > 1 {\n        crate::file::DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL',
                      '    if channels > 0 {\n        crate::file::DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL')],
    'never-multi': [('mod', '    if channels > 1 {\n        crate::file::DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL',
                     '    if false {\n        crate::file::DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL')],
    'not-reported': [('mod', '                | crate::file::DRM_ASAHI_FEATURE_COMPUTE_TIMEOUT\n' + REPORTED,
                      '                | crate::file::DRM_ASAHI_FEATURE_COMPUTE_TIMEOUT;\n')],
    'per-board-gate': [('mod', REPORTED,
                        '                | compute_channel_features(pairs::COMPUTE_CHANNELS_MAX as u8);\n')],
    'forms-gate': [('fw', '    board.compute_channels() > 1\n',
                    '    board.compute_channels() > 1 || !board.g17d_forms()\n')],
    'max-channels': [('pairs', '    COMPUTE_CHANNELS.store(board.compute_channels(), Relaxed);',
                      '    COMPUTE_CHANNELS.store(COMPUTE_CHANNELS_MAX as u8, Relaxed);')],
    'multi-channel-max': [('initdata', '    host_only_render_kicks: true,\n    compute_channels: 1,\n',
                           '    host_only_render_kicks: true,\n    compute_channels: 3,\n')],
    'colliding-bit': [('uapi', '#define DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL ((1ULL) << 42)',
                       '#define DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL ((1ULL) << 39)')],
    'unsynced-binding': [('file', '    uapi::DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL as u64;',
                          '    uapi::DRM_ASAHI_FEATURE_ASYNC_SUBMIT as u64;')],
}


def build(mutation=None):
    files = {
        'pairs': (G17 / 'pairs.rs').read_text(),
        'mod': (G17 / 'mod.rs').read_text(),
        'fw': (G17 / 'fw/m5/compute.rs').read_text(),
        'file': (ASAHI / 'file.rs').read_text(),
        'uapi': UAPI.read_text(),
        'initdata': (G17 / 'fw/m5/initdata.rs').read_text(),
    }
    for name, old, new in MUTATIONS.get(mutation, []):
        assert files[name].count(old) == 1, (mutation, old)
        files[name] = files[name].replace(old, new)
    pairs, mod, fw, file, uapi, initdata = (files[k] for k in ('pairs', 'mod', 'fw', 'file', 'uapi', 'initdata'))

    # Every board's own channel count, following `..BASE` for a board that inherits it.
    boards = {}
    for name, body in re.findall(r'^pub\(crate\) const (\w+): Board = Board \{\n(.*?)^\};', initdata, re.M | re.S):
        own = re.search(r'^    compute_channels: (\d+),$', body, re.M)
        base = re.search(r'^    \.\.(\w+)$', body, re.M)
        assert own or base, name
        boards[name] = int(own.group(1)) if own else boards[base.group(1)]
    assert set(boards) == {'J714', 'J716C', 'J775D', 'J813', 'J813_26B123'}, boards
    board_table = ',\n'.join(f'    ("{name}", {n})' for name, n in boards.items())
    channels_fn = item(initdata, 'pub(crate) const fn compute_channels(').replace('pub(crate) ', '')

    bits = re.findall(r'^\s*(DRM_ASAHI_FEATURE_\w+)\s*=\s*\(1U?L{0,2}\)\s*<<\s*(\d+),', uapi, re.M)
    bits += re.findall(r'^#define\s+(DRM_ASAHI_FEATURE_\w+)\s+\(\(1ULL\)\s*<<\s*(\d+)\)', uapi, re.M)
    names = [name for name, _ in bits]
    uapi_mod = '\n'.join(f'    pub const {name}: u64 = 1 << {bit};' for name, bit in bits)
    table = ',\n'.join(f'    ("{name}", 1u64 << {bit})' for name, bit in bits)

    pairs_code = '\n'.join([
        re.search(r'pub\(crate\) const COMPUTE_CHANNELS_MAX: usize = \d+;', pairs).group(0),
        re.search(r'const MULTI_PAIRS: u8 = [^;]+;', pairs).group(0),
        re.search(r'static PAIRS: AtomicU8 = AtomicU8::new\(\d+\);', pairs).group(0),
        re.search(r'static COMPUTE_CHANNELS: AtomicU8 = AtomicU8::new\(\d+\);', pairs).group(0),
        item(pairs, 'pub(crate) fn select_board('),
        item(pairs, 'pub(crate) fn per_owner('),
        item(pairs, 'pub(crate) fn compute_channels_per_owner('),
    ]).replace('pub(crate) ', 'pub ').replace('crate::g17::fw::m5::initdata::Board', 'crate::Board') \
        .replace('crate::g17::fw::m5::compute::multi_channel', 'crate::multi_channel') \
        .replace('RENDER_PAIRS_MAX as u8', '2u8')
    features = item(mod, 'fn compute_channel_features(')
    multi = item(fw, 'pub(crate) fn multi_channel(').replace('pub(crate) ', '')
    file_const = re.search(r'pub\(crate\) const DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL: u64 =\s*[^;]+;', file).group(0)
    params = item(mod, 'fn get_params(&self, params: &mut uapi::drm_asahi_params_global)')
    structure = {
        'GET_PARAMS reports the bit from the probed channel count on every G17': ordered_ok(
            params, 'params.features |=', '| crate::file::DRM_ASAHI_FEATURE_COMPUTE_TIMEOUT', REPORTED.strip(),
            'let variant = self.shared.variant;'),
        'the UAPI header defines the bit': 'DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL' in names,
    }
    checks = '\n'.join(f'    assert!({str(ok).lower()}, "structure: {name}");' for name, ok in structure.items())
    return (TEMPLATE.replace('@@UAPI@@', uapi_mod).replace('@@TABLE@@', table).replace('@@PAIRS@@', pairs_code)
            .replace('@@FEATURES@@', features).replace('@@MULTI@@', multi).replace('@@FILE@@', file_const)
            .replace('@@STRUCTURE@@', checks).replace('@@BOARDS@@', board_table)
            .replace('@@CHANNELS@@', channels_fn))


TEMPLATE = r'''
#![allow(dead_code)]
use std::sync::atomic::{AtomicU8, Ordering::Relaxed};

mod uapi {
@@UAPI@@
}
const FEATURES: &[(&str, u64)] = &[
@@TABLE@@
];

mod file {
    use crate::uapi;
    @@FILE@@
}

pub struct Board { g17d: bool, clusters: u32, compute_channels: u8 }
impl Board {
    fn g17d_forms(&self) -> bool { self.g17d }
    @@CHANNELS@@
}
@@MULTI@@

/// The production boards and their `compute_channels`.
const BOARDS: &[(&str, u8)] = &[
@@BOARDS@@
];

mod pairs {
    use std::sync::atomic::{AtomicU8, Ordering::Relaxed};
    @@PAIRS@@
}

@@FEATURES@@

/// What GET_PARAMS reports, as get_params() computes it.
fn reported() -> u64 { compute_channel_features(pairs::compute_channels_per_owner()) }

fn main() {
@@STRUCTURE@@

    // The bit is the header's and no other feature has it.
    let bit = file::DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL;
    assert_eq!(bit, uapi::DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL, "file.rs reports the header's bit");
    assert_eq!(bit.count_ones(), 1);
    for (name, value) in FEATURES {
        if *name != "DRM_ASAHI_FEATURE_COMPUTE_MULTI_CHANNEL" {
            assert_ne!(*value, bit, "{} shares the multi-channel bit", name);
        }
    }

    // A GPU that never selects an M5 board (G17P) has one channel per owner: no bit.
    assert_eq!(pairs::compute_channels_per_owner(), 1);
    assert_eq!(reported() & bit, 0, "single-channel G17P keeps its own sibling queues");
    // The production boards: only the M5 Pro holds several channels per owner, so only it
    // reports the bit; the M5 Max, M5 Ultra and M5 run one firmware compute queue per queue.
    for &(name, channels) in BOARDS {
        let multi = name == "J714";
        assert!(channels >= 1 && usize::from(channels) <= pairs::COMPUTE_CHANNELS_MAX, "{}", name);
        for (g17d, clusters) in [(false, 1), (false, 2), (true, 4), (true, 8)] {
            let board = Board { g17d, clusters, compute_channels: channels };
            assert_eq!(multi_channel(&board), multi, "{}: multi_channel()", name);
            pairs::select_board(&board);
            assert_eq!(pairs::compute_channels_per_owner(), channels, "{}: channels per owner", name);
            assert_eq!(reported() & bit, if multi { bit } else { 0 }, "{}: reported", name);
        }
    }
    assert_eq!(BOARDS.iter().find(|b| b.0 == "J714").unwrap().1 as usize, pairs::COMPUTE_CHANNELS_MAX,
               "the M5 Pro holds the most channels");
    // Set exactly when an owner may hold more than one channel, and nothing else.
    for channels in 0..=u8::MAX {
        assert_eq!(compute_channel_features(channels), if channels > 1 { bit } else { 0 });
    }

    println!("PASS g17-compute-multi-channel-feature: reported exactly where a queue holds several compute channels");
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = ap.parse_args()
    source = build(args.mutation)
    with tempfile.TemporaryDirectory(prefix='g17-multi-channel-') as tmp:
        path = Path(tmp) / 'feature.rs'
        binary = path.with_suffix('')
        path.write_text(source)
        built = subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(path),
                                '-o', str(binary)], capture_output=True, text=True)
        if built.returncode:
            sys.stderr.write(built.stderr)
            return built.returncode
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
