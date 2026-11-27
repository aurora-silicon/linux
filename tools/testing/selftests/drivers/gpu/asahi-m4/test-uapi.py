#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile complete kernel/Rust UAPI definitions; optionally compare real Mesa."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from abi import c_layout, differences, rust_layout
from common import FIXTURES, kernel_root, output_dir, rustc, verify_fixtures


def item(text, declaration):
    start = text.index(declaration)
    at = text.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


SOFT_FAULTS_HARNESS = r'''
#![allow(dead_code, non_upper_case_globals)]
mod uapi { __UAPI__ }
mod module_parameters {
    pub struct Param(pub std::cell::Cell<u32>);
    impl Param { pub fn value(&self) -> u32 { self.0.get() } }
    std::thread_local! {
        pub static FAULT_CONTROL: Param = const { Param(std::cell::Cell::new(0xb)) };
    }
    pub struct FaultControl;
    impl FaultControl { pub fn value(&self) -> u32 { FAULT_CONTROL.with(|p| p.value()) } }
    pub static fault_control: FaultControl = FaultControl;
}
pub struct QueueLimits;
trait Backend {
    fn backend_feature_flags(&self) -> u64 { 0 }
    fn supports_vm_status(&self) -> bool { false }
    fn queue_limits(&self) -> Option<QueueLimits> { None }
    __SHARED__
}
struct G16;
impl Backend for G16 {
    fn supports_vm_status(&self) -> bool { true }
    fn queue_limits(&self) -> Option<QueueLimits> { Some(QueueLimits) }
    __OVERRIDE__
}
const SOFT: u64 = uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_SOFT_FAULTS as u64;
#[test]
fn g16_advertises_soft_faults_until_fault_control_opts_out() {
    let set = |v| module_parameters::FAULT_CONTROL.with(|p| p.0.set(v));
    set(0xb);
    assert_eq!(G16.feature_flags() & SOFT, SOFT);
    for other in [0, 1, 0xa, 0xf] {
        set(other);
        assert_eq!(G16.feature_flags() & SOFT, 0, "fault_control {other:#x}");
    }
    set(0xb);
    // The other shared bits are unchanged by the opt-out.
    let rest = G16.feature_flags() & !SOFT;
    set(0);
    assert_eq!(G16.feature_flags(), rest);
}
'''


def soft_faults(kernel, header, out):
    """The production G16 SOFT_FAULTS advertisement: the shared defaults with G16's override."""
    gpu = (kernel / 'drivers/gpu/drm/asahi/gpu.rs').read_text()
    g16 = (kernel / 'drivers/gpu/drm/asahi/g16/mod.rs').read_text()
    # G16 keeps the shared fault_control opt-out: it overrides soft_faults() only.
    assert g16.count('fn soft_faults(&self) -> bool {') == 1
    assert 'fn supports_soft_faults(' not in g16
    text = header.read_text()

    def feature(name):
        shift = re.search(r'\b' + name + r'\s*=\s*\(1UL\)\s*<<\s*(\d+)', text).group(1)
        return f'pub const drm_asahi_feature_{name}: u32 = 1 << {shift};'

    names = ('SOFT_FAULTS', 'VM_STATUS', 'VM_STATUS_MIRROR', 'INDEPENDENT_QUEUES')
    uapi = '\n'.join(feature('DRM_ASAHI_FEATURE_' + n) for n in names)
    shared = '\n'.join(item(gpu, decl) for decl in ('fn supports_soft_faults(&self) -> bool {',
                                                     'fn feature_flags(&self) -> u64 {',
                                                     'fn soft_faults(&self) -> bool {'))
    source = (SOFT_FAULTS_HARNESS.replace('__UAPI__', uapi).replace('__SHARED__', shared)
              .replace('__OVERRIDE__', item(g16, 'fn soft_faults(&self) -> bool {')))
    (out / 'soft-faults.rs').write_text(source)
    for command in [rustc() + ['--edition=2021', '--test', str(out / 'soft-faults.rs'), '-o',
                               str(out / 'soft-faults')],
                    [str(out / 'soft-faults')]]:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
        print(result.stdout, end='')
        if result.returncode:
            raise SystemExit(result.returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa', type=Path, default=os.environ.get('ASAHI_M4_MESA'),
                        help='actual Mesa checkout; no vendored header substitute')
    parser.add_argument('--kernel', type=Path, default=kernel_root())
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    verify_fixtures()
    out = output_dir()
    bindings = FIXTURES / 'public-uapi.rs'
    header = args.kernel.resolve() / 'include/uapi/drm/asahi_drm.h'
    kernel = c_layout(header, bindings, out, 'kernel-layout')
    rust = rust_layout(bindings, out)
    errors = differences(kernel, rust)
    soft_faults(args.kernel.resolve(), header, out)
    mesa = None
    if args.mesa:
        mesa = args.mesa.resolve() / 'include/drm-uapi/asahi_drm.h'
        errors += differences(kernel, c_layout(mesa, bindings, out, 'mesa-layout'))
    report = {'passed': not errors, 'scope': 'Compiled public UAPI structures, field offsets and constants; frozen params prefix plus exact known capacity-tail assertions; no parser or feature-semantic coverage',
              'kernel': str(header), 'bindings': str(bindings), 'comparisons': len(kernel),
              'mesa': str(mesa) if mesa else None,
              'mesa_comparison': 'performed' if mesa else 'not requested; set ASAHI_M4_MESA or --mesa',
              'errors': errors,
              'soft_faults': 'G16 SOFT_FAULTS advertisement checked against fault_control'}
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
