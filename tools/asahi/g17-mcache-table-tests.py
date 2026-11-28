#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""MCache tables of G17 kick entries: compute kicks name their write ranges,
render kicks name none.

Compiles the production MCache range and table encoders and the kick entry's
MCache word (fw/kick.rs), the compute descriptor's table placement and writer
(fw/compute.rs) and the compute queue's choice of ranges (queue/compute.rs)
against stand-ins, with the variants' MCache flags read from fw/variant.rs,
and checks what each firmware's compute kick names. The render side is
checked in the source: the render kick builder names no table for either
stage, and nothing in the render queue or the fragment descriptor writes one.

  --mutation NAME   break the production source first; the run must fail
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import sys
import tempfile

G17 = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17'
FILES = {
    'kick': 'fw/kick.rs',
    'compute': 'fw/compute.rs',
    'variant': 'fw/variant.rs',
    'fwmod': 'fw/mod.rs',
    'queue_compute': 'queue/compute.rs',
    'queue_render': 'queue/render.rs',
    'render_memory': 'queue/render/memory.rs',
    'fw_render': 'fw/render.rs',
}

# name -> (file, old, new): each breaks one property the run checks.
MUTATIONS = {
    # A render kick names a table again.
    'render-table': ('queue_render', '            mcache: None,\n',
                     '            mcache: if stage == 1 { table } else { None },\n'),
    # The fragment descriptor writer gains a table.
    'fragment-writer': ('fw_render', 'use super::kick::{KickTimestamp,',
                        'use super::kick::{McacheTable, KickTimestamp,'),
    # Compute kicks stop naming their writes on the A18 Pro.
    'compute-dropped': ('queue_compute', '            attachments.as_slice()\n', '            &[][..]\n'),
    # M5 command sets name compute writes.
    'compute-on-m5': ('queue_compute', 'if self.graph.mcache && !self.graph.m5 {', 'if self.graph.mcache {'),
    # Wrong range class.
    'class': ('kick', '    Attachment = 1,\n', '    Attachment = 2,\n'),
    # A range's end line rounds down and misses its partial last line.
    'line-end': ('kick', '.div_ceil(Self::LINE_SIZE);', '/ Self::LINE_SIZE;'),
    # A compute table outgrows the register array's MCache space.
    'inline-bound': ('compute', 'if count > MCACHE_INLINE_RANGES {', 'if count > MCACHE_INLINE_RANGES + 1 {'),
    # The kick entry names a table without its presence bit.
    'kick-bit': ('kick', '                header |= Self::MCACHE_PRESENT;\n', ''),
}


def replace(text, old, new):
    assert text.count(old) == 1, f'{old!r}: {text.count(old)} matches, expected 1'
    return text.replace(old, new)


def item(text, signature):
    """The item starting at `signature`, through its matching closing brace."""
    assert text.count(signature) == 1, f'{signature!r}: {text.count(signature)} matches'
    start = text.index(signature)
    depth, end = 1, text.index('{', start) + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def line(text, prefix):
    found = [l for l in text.splitlines() if l.strip().startswith(prefix)]
    assert len(found) == 1, f'{prefix!r}: {len(found)} lines'
    return found[0].strip()


def statement(text, start):
    """The `let` statement at `start`, through the `};` closing its block."""
    assert text.count(start) == 1, f'{start!r}: {text.count(start)} matches'
    begin = text.index(start)
    return text[begin:text.index('};', begin) + 2]


def flag(variant, name, field):
    body = item(variant, f'pub(crate) const {name}: Variant = Variant {{')
    value = re.search(rf'^\s*{field}: (true|false),', body, re.M)
    assert value, f'{name}.{field} not set explicitly'
    return value.group(1)


def code_lines(text):
    """(line number, line) of every line outside `//` comments."""
    for number, text_line in enumerate(text.splitlines(), 1):
        stripped = text_line.split('//', 1)[0]
        if stripped.strip():
            yield number, stripped


def check_render(src):
    """Render kicks name no MCache table. Returns failures as (file, line, text)."""
    failures = []
    render = src['queue_render']
    publish = item(render, '    pub(crate) fn publish(')
    offset = render[:render.index(publish)].count('\n')
    builder = item(publish, 'let args = |stage: usize, barriers, arrays| KickArgs {')
    boffset = offset + publish[:publish.index(builder)].count('\n')
    fields = [(boffset + n, l) for n, l in code_lines(builder) if re.match(r'\s*mcache\s*:', l)]
    if len(fields) != 1 or fields[0][1].strip() != 'mcache: None,':
        failures.append(('queue/render.rs', fields[0][0] if fields else boffset + 1,
                         'render kicks must set `mcache: None` for both stages'))
    for key in ('queue_render', 'render_memory', 'fw_render'):
        for number, text_line in code_lines(src[key]):
            if key == 'queue_render' and number == (fields[0][0] if fields else -1):
                continue
            if re.search(r'\bMcache(Table|Range|Class)\b|\bset_mcache\b|\bmcache\b', text_line):
                failures.append((FILES[key], number, f'render code builds an MCache table: {text_line.strip()}'))
    return failures


def fixture(src):
    kick, compute, fwmod = src['kick'], src['compute'], src['fwmod']
    entry_new = item(kick, '    pub(crate) fn new(args: &KickArgs<\'_>) -> Result<Self> {')
    kick_block = statement(entry_new, 'let mcache = match args.mcache {').replace('Self::MCACHE_PRESENT', 'MCACHE_PRESENT')
    choice = statement(src['queue_compute'], 'let mcache_writes = if')
    variant = src['variant']
    return r'''
#![allow(dead_code)]
type Result<T = ()> = core::result::Result<T, i32>;
const EINVAL: i32 = -22;
''' + '\n'.join([
        line(kick, 'pub(crate) const CONTEXT_MAX:'),
        line(kick, 'const PACKED_VA_SHIFT:'),
        line(kick, 'const PACKED_VA_LIMIT:'),
        line(kick, 'pub(crate) const MCACHE_INLINE_RANGES:'),
        item(kick, 'fn packed_va(va: u64) -> Result<u64> {'),
        item(fwmod, 'pub(crate) fn offset_va(va: u64, offset: u64) -> Result<u64> {'),
        '#[derive(Copy, Clone, Debug, PartialEq, Eq)]',
        item(kick, 'pub(crate) struct WriteRange {'),
        '#[derive(Copy, Clone, Debug, PartialEq, Eq)]\n#[repr(u32)]',
        item(kick, 'pub(crate) enum McacheClass {'),
        '#[repr(C, packed)]\n#[derive(Copy, Clone, Debug)]',
        item(kick, 'pub(crate) struct McacheRange {'),
        item(kick, 'impl McacheRange {'),
        '#[derive(Copy, Clone, Debug, PartialEq, Eq)]',
        item(kick, 'pub(crate) struct McacheTable {'),
        item(kick, 'impl McacheTable {'),
        line(kick, 'const MCACHE_PRESENT:'),
    ]) + r'''
/// The register array's layout around its MCache space (fw/kick.rs asserts
/// the offset).
#[repr(C)]
struct RegisterArray { entries: [[u8; 0xc]; 128], mcache: [McacheRange; MCACHE_INLINE_RANGES], tail: [u8; 0x20] }
const _: () = assert!(core::mem::offset_of!(RegisterArray, mcache) == ''' + re.search(
        r'offset_of!\(RegisterArray, mcache\) == (0x[0-9a-f]+)\);', kick).group(1) + r''');
struct ComputeDescriptor { registers: RegisterArray }
impl ComputeDescriptor {
    /// Stand-in for the descriptor's real register-array offset.
    const REGISTERS_OFFSET: u64 = 0x40;
''' + item(compute, '    pub(crate) fn mcache_table(') + '\n' + item(compute, '    pub(crate) fn write_mcache(') + r'''
}
struct KickArgs { mcache: Option<McacheTable> }
/// Header bits and MCache word of a kick entry, as `KickEntry::new` builds them.
fn kick_words(args: &KickArgs) -> (u64, u64) {
    let mut header = 0u64;
    ''' + kick_block + r'''
    (header, mcache)
}
struct Graph { mcache: bool, m5: bool }
struct Attachments(Vec<WriteRange>);
impl Attachments { fn as_slice(&self) -> &[WriteRange] { &self.0 } }
struct Queue { graph: Graph }
impl Queue {
    /// The compute queue's choice of ranges to name.
    fn writes(&self, attachments: &Attachments) -> Vec<WriteRange> {
        ''' + choice + r'''
        mcache_writes.to_vec()
    }
}
fn zeroed() -> ComputeDescriptor { unsafe { core::mem::zeroed() } }
/// What a compute kick of `writes` names: the kick words and the table's ranges.
fn compute_kick(queue: &Queue, writes: &Attachments, descriptor_va: u64, context: u8) -> (u64, u64, Vec<(u64, u64)>) {
    let named = queue.writes(writes);
    let table = ComputeDescriptor::mcache_table(descriptor_va, named.len()).unwrap();
    let mut descriptor = zeroed();
    descriptor.write_mcache(&named, context).unwrap();
    let (header, word) = kick_words(&KickArgs { mcache: table });
    let ranges = descriptor.registers.mcache[..named.len()].iter().map(|r| ({ r.start }, { r.end })).collect();
    (header, word, ranges)
}
fn main() {
    let line = 0x80u64;
    let present = MCACHE_PRESENT;
    assert_eq!(present, 1 << 58, "kick-entry bit 58 marks an MCache table");
    // One range: 128-byte lines, the end rounded up, the UAT context and the attachment class.
    let r = McacheRange::new(&WriteRange { va: 0x12_3456_7840, size: 0x1001 }, McacheClass::Attachment, 0x2a).unwrap();
    let (start, end) = ({ r.start }, { r.end });
    let mask = (1u64 << 41) - 1;
    assert_eq!(start & mask, 0x12_3456_7840 / line, "first line");
    assert_eq!((start >> 41) & 0x3f, 0x2a, "UAT context");
    assert_eq!(start >> 47, 0x118, "start-word constant");
    assert_eq!(end & mask, (0x12_3456_7840 + 0x1001 + line - 1) / line, "end line covers the partial last line");
    assert_eq!(end >> 41, 0b101, "end-word bit 41 and class 1 (attachment) in bit 43");
    assert_eq!(McacheRange::new(&WriteRange { va: 0x1000, size: 0 }, McacheClass::Attachment, 1).err(), Some(EINVAL));
    assert_eq!(McacheRange::new(&WriteRange { va: 0x1000, size: 0x80 }, McacheClass::Attachment, 0x40).err(), Some(EINVAL));
    // The table word: address >> 5 in bits 43:6, count - 1 in bits 59:54.
    let t = McacheTable::new(0x45_6780, 3).unwrap();
    assert_eq!(kick_words(&KickArgs { mcache: Some(t) }), (present, (0x45_6780u64 >> 5) << 6 | 2 << 54));
    assert_eq!(kick_words(&KickArgs { mcache: None }), (0, 0), "no table: bit 58 clear, word 0");
    assert!(McacheTable::new(0x45_6780, 0).is_err() && McacheTable::new(0x45_6790, 1).is_err());

    let writes = Attachments(vec![
        WriteRange { va: 0x10_0000, size: 0x4_0000 },
        WriteRange { va: 0x20_0040, size: 0x100 },
        WriteRange { va: 0x30_0000, size: 0x1_0000 },
    ]);
    let descriptor_va = 0x8000_0000u64;
    let registers_mcache = descriptor_va + ComputeDescriptor::REGISTERS_OFFSET + core::mem::offset_of!(RegisterArray, mcache) as u64;
    // A18 Pro: a compute kick names every write range, in the main array's MCache space.
    let g17p = Queue { graph: Graph { mcache: ''' + flag(variant, 'V26A428', 'mcache') + r''', m5: false } };
    let (header, word, ranges) = compute_kick(&g17p, &writes, descriptor_va, 7);
    assert_eq!(header, present, "A18 Pro compute kicks name their write ranges");
    assert_eq!(word, (registers_mcache >> 5) << 6 | 2 << 54, "table at the register array's MCache space, 3 ranges");
    for (range, (start, end)) in writes.0.iter().zip(&ranges) {
        let expected = McacheRange::new(range, McacheClass::Attachment, 7).unwrap();
        assert_eq!((*start, *end), ({ expected.start }, { expected.end }));
    }
    // M4 Pro (V25G83) firmware takes no table; M5 command sets send none.
    let m4pro = Queue { graph: Graph { mcache: ''' + flag(variant, 'V25G83', 'mcache') + r''', m5: false } };
    assert_eq!(compute_kick(&m4pro, &writes, descriptor_va, 7).0, 0, "V25G83 compute kicks name no table");
    let m5 = Queue { graph: Graph { mcache: true, m5: true } };
    assert_eq!(compute_kick(&m5, &writes, descriptor_va, 7).0, 0, "M5 compute kicks name no table");
    // The register array holds 16 ranges; more is refused.
    assert!(ComputeDescriptor::mcache_table(descriptor_va, MCACHE_INLINE_RANGES).unwrap().is_some());
    assert_eq!(ComputeDescriptor::mcache_table(descriptor_va, MCACHE_INLINE_RANGES + 1).err(), Some(EINVAL),
               "a compute table holds at most the inline ranges");
    println!("PASS compute MCache tables: range and table encodings, bit 58, A18 Pro names its write ranges, V25G83 and M5 none, 16-range bound");
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()
    src = {key: (G17 / path).read_text() for key, path in FILES.items()}
    if args.mutation:
        key, old, new = MUTATIONS[args.mutation]
        src[key] = replace(src[key], old, new)

    failures = check_render(src)
    for path, number, message in failures:
        print(f'FAIL line {number}: {path}: {message}')
    if not failures:
        print('PASS render kicks name no MCache table; no fragment table writer')

    with tempfile.TemporaryDirectory(prefix='g17-mcache-') as tmp:
        path = Path(tmp) / 'mcache.rs'
        path.write_text(fixture(src))
        binary = path.with_suffix('')
        subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', '-Awarnings', str(path), '-o', str(binary)],
                       check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
    return 1 if failures or result.returncode else 0


if __name__ == '__main__':
    sys.exit(main())
