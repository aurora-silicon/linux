#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the workqueue priority policy across its firmware variants."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def sha(data):
    return hashlib.sha256(data.encode()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--ref', help='Read tracked production inputs from this Git object')
    ap.add_argument('--old-ref', help='Require this original policy to fail the same assertions')
    ap.add_argument('--rustc', default=os.environ.get('RUSTC', 'rustc'))
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.out.mkdir(parents=True, exist_ok=True)
    def read(path, ref=None):
        if ref:
            return subprocess.check_output(['git', 'show', f'{ref}:{path}'], cwd=root, text=True)
        return (root / path).read_text()
    def policy(text):
        start = text.index('        let mut prio = *raw::PRIORITY.get(priority as usize).ok_or(EINVAL)?;')
        # The policy ends with the compute priority override; queue allocation follows.
        tail = '            prio.5 = 1;\n        }\n'
        end = text.index(tail, start) + len(tail)
        return text[start:end]
    queue = read('drivers/gpu/drm/asahi/workqueue.rs', args.ref)
    macro = read('rust/macros/versions.rs', args.ref)
    fw = read('drivers/gpu/drm/asahi/fw/workqueue.rs', args.ref)
    table = re.search(r'pub\(crate\) const PRIORITY: \[Priority; 4\] = \[.*?\n    \];', fw, re.S).group()
    macro_file = args.out / 'versions.rs'
    macro_file.write_text('extern crate proc_macro;\nmod implementation {\n' + macro + '\n}\n#[proc_macro_attribute]\npub fn versions(a: proc_macro::TokenStream, b: proc_macro::TokenStream) -> proc_macro::TokenStream { implementation::versions(a,b) }\n')
    subprocess.run([args.rustc, '--edition=2021', '--crate-type=proc-macro', str(macro_file), '-o', str(args.out / 'libversions.so')], check=True)
    prefix = '''#![allow(dead_code, non_upper_case_globals)]
use versions::versions;
use std::sync::atomic::{AtomicBool, Ordering};
static DEBUG: AtomicBool = AtomicBool::new(false);
enum DebugFlags { Debug0 }
fn debug_enabled(_: DebugFlags) -> bool { DEBUG.load(Ordering::Relaxed) }
mod module_parameters {
    pub static compute_preempt: Parameter = Parameter;
    pub static mut PARAM: i32 = -1;
    pub struct Parameter;
    impl Parameter { pub fn value(&self) -> i32 { unsafe { *std::ptr::addr_of!(PARAM) } } }
}
#[derive(Clone, Copy, Debug, PartialEq)] struct U64(u64);
#[derive(Clone, Copy, Debug, PartialEq)] enum PipeType { Vertex, Fragment, Compute }
struct Config { chip_id: u32 }
const EINVAL: i32 = 22;
mod raw {
    use super::U64;
    #[derive(Clone, Copy, Debug, PartialEq)] pub struct Priority(pub u32,pub u32,pub U64,pub u32,pub u32,pub u32);
'''
    middle = '''
}
#[versions(AGX)] struct Policy;
#[versions(AGX)] impl Policy::ver {
    fn select(cfg: &Config, pipe_type: PipeType, priority: u32) -> Result<raw::Priority,i32> {
'''
    suffix = '''
        Ok(prio)
    }
}
fn check(select: fn(&Config, PipeType, u32)->Result<raw::Priority,i32>, g14x: bool) -> usize {
    let mut cases=0;
    for chip in [0x8103,0x6000,0x6001,0x6002,0x8112,0x6020,0x6021,0x6022,0x8122,0x6030,0x6031,0x8140] {
        for param in [-1,0,1,2,-2] {
            unsafe { module_parameters::PARAM=param; }
            for debug in [false,true] {
                DEBUG.store(debug,Ordering::Relaxed);
                let preempt=if param == -1 { (g14x && chip==0x6021)||debug } else { param!=0 };
                for pipe in [PipeType::Vertex,PipeType::Fragment,PipeType::Compute] {
                    for priority in [0,1,2,3,4,u32::MAX] {
                        let actual=select(&Config{chip_id:chip},pipe,priority);
                        if priority>=4 { assert_eq!(actual,Err(EINVAL)); }
                        else {
                            let mut expected=raw::PRIORITY[priority as usize];
                            if pipe==PipeType::Compute && !preempt { expected.0=0;expected.5=1; }
                            assert_eq!(actual,Ok(expected),"chip={chip:x} param={param} debug={debug} pipe={pipe:?} priority={priority}");
                        }
                        cases+=1;
                    }
                }
            }
        }
    }
    cases
}
fn main() {
    let mut cases=0;
    cases+=check(PolicyG13V12_3::select,false);
    cases+=check(PolicyG14V12_4::select,false);
    cases+=check(PolicyG13V13_5::select,false);
    cases+=check(PolicyG14V13_5::select,false);
    cases+=check(PolicyG14XV13_5::select,true);
    cases+=check(PolicyG15V14_8_3::select,false);
    println!("{cases} policy and priority cases PASS");
}
'''
    g15_available = bool(re.search(r'"G15"\s*,\s*"V14_8_3"', macro))
    if not g15_available:
        suffix = suffix.replace('    cases+=check(PolicyG15V14_8_3::select,false);\n', '')
    selected = policy(queue)
    sources = {'positive': selected}
    if args.old_ref:
        sources['original'] = policy(read('drivers/gpu/drm/asahi/workqueue.rs', args.old_ref))
    changes = {
        'all-g14x-auto': ('cfg.chip_id == 0x6021', 'true'),
        'ignore-debug': ('preempt_auto || debug_enabled(DebugFlags::Debug0)', 'preempt_auto'),
        'debug-overrides-zero': ('value => value != 0,', 'value => value != 0 || debug_enabled(DebugFlags::Debug0),'),
        'fragment-priority': ('pipe_type == PipeType::Compute && !preempt', 'pipe_type != PipeType::Vertex && !preempt'),
    }
    for name,(old,new) in changes.items():
        assert selected.count(old)==1, name
        sources[name]=selected.replace(old,new)
    results={}
    for name,body in sources.items():
        source=args.out/f'{name}.rs'
        source.write_text(prefix+table+middle+body+suffix)
        binary=args.out/name
        subprocess.run([args.rustc,'--edition=2021',str(source),'--extern',f'versions={args.out / "libversions.so"}','-o',str(binary)],check=True)
        run=subprocess.run([str(binary)],capture_output=True,text=True)
        (args.out/f'{name}.log').write_text(run.stdout+run.stderr)
        assert (run.returncode==0)==(name=='positive'), (name,run.returncode)
        results[name]={'returncode':run.returncode,'policy_sha256':sha(body),'compiled_source_sha256':sha(source.read_text())}
    receipt={'g15_backend_available':g15_available,'results':results,'policy_sha256':sha(selected),'priority_table_sha256':sha(table),'versions_sha256':sha(macro),'scope':'Actual priority policy, priority table and version macro; controlled parameter/debug inputs. No GPU allocation, firmware scheduling, cancellation or hardware execution.'}
    (args.out/'results.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))

if __name__=='__main__':
    main()
