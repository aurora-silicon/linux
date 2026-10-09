#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check firmware-log ownership, wire sizes and consumption bounds."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DIRECTORY = 'drivers/gpu/drm/asahi/'


def function(source, marker):
    start = source.index(marker)
    begin = source.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PREFIX = r'''
#![allow(dead_code, unused_variables)]
mod m3_init_layout; mod m3_init_storage;
use m3_init_layout as init; use m3_init_storage as storage;
IMPORT_PAYLOAD
type Result<T=()> = core::result::Result<T,i32>;
const EIO:i32=5;
mod driver { pub struct AsahiDevice; }
static SEEN: std::sync::Mutex<Vec<(usize,Vec<u8>)>> = std::sync::Mutex::new(Vec::new());
macro_rules! dev_info {
 ($d:expr,"M3 fwlog payload {} {:02x?}\n",$channel:expr,$payload:expr) => {
  SEEN.lock().unwrap().push(($channel,$payload.to_vec()));
 };
 ($($args:tt)*) => {};
}
macro_rules! dev_err { ($($args:tt)*) => {}; }
#[derive(Debug,Copy,Clone,Default)] #[repr(C,packed(1))] struct U64(u64);
#[derive(Debug,Copy,Clone)] struct Pad<const N:usize>([u8;N]);
impl<const N:usize> Default for Pad<N> {fn default()->Self{Self([0;N])}}
#[derive(Debug,Copy,Clone)] struct Array<const N:usize,T>([T;N]);
impl<const N:usize,T:Default+Copy> Default for Array<N,T>{fn default()->Self{Self([T::default();N])}}
WIRE_STRUCTS
struct Buffer(Vec<u8>);
impl Buffer {
 fn read(&self,offset:usize,out:&mut[u8])->Result {
  let data=self.0.get(offset..offset.checked_add(out.len()).ok_or(EIO)?).ok_or(EIO)?;
  out.copy_from_slice(data); Ok(())
 }
 fn read_u32(&self,offset:usize)->Result<u32>{let mut b=[0;4];self.read(offset,&mut b)?;Ok(u32::from_le_bytes(b))}
 fn read_u64(&self,offset:usize)->Result<u64>{let mut b=[0;8];self.read(offset,&mut b)?;Ok(u64::from_le_bytes(b))}
 fn u32(&mut self,offset:usize,value:u32)->Result{
  self.0.get_mut(offset..offset+4).ok_or(EIO)?.copy_from_slice(&value.to_le_bytes());Ok(())
 }
}
struct Config {objects:Vec<Buffer>,completed_events:u64}
impl Config { DRAIN }
fn fresh()->Config{Config{objects:(0..storage::COUNT).map(|i|Buffer(vec![0;storage::allocation(i).unwrap().size])).collect(),completed_events:0}}
fn main(){
 let entry=core::mem::size_of::<RawFwLogMsg>();let payload=core::mem::size_of::<RawFwLogPayloadMsg>();
 assert_eq!((entry,payload),(56,216));
 let owner=storage::allocation(PAYLOAD_SLOT).unwrap();
 assert!(owner.size>=6*256*payload,"full firmware-log payload allocation");
 let ring=storage::allocation(storage::FIRMWARE_LOG*2+1).unwrap();
 assert_eq!(ring.size,6*256*entry,"ring entry size differs from payload size");
 for i in 0..storage::COUNT {
  if i==PAYLOAD_SLOT {continue;}
  let a=storage::allocation(i).unwrap();
  let start=a.address & !0x3fff;let end=(a.address+a.size as u64+0x3fff)&!0x3fff;
  let pstart=owner.address & !0x3fff;let pend=(owner.address+owner.size as u64+0x3fff)&!0x3fff;
  assert!(pend<=start || end<=pstart,"payload mapping aliases owner {i}");
 }
 let region=|i|{let a=storage::allocation(i).unwrap();init::Region::new(a.address,a.size).unwrap()};
 let channel=init::Channel{state:region(0),ring:region(1)};
 let mut channels=[channel;17];for i in 0..17{channels[i]=init::Channel{state:region(i*2),ring:region(i*2+1)};}
 let mut runtime=init::RuntimePointers{hardware:region(storage::HARDWARE_DATA),unknown_pair:region(storage::UNKNOWN_PAIR),
  PAYLOAD_FIELD:region(PAYLOAD_SLOT),unknown_c0:region(storage::UNKNOWN_C0),unknown_c1:region(storage::UNKNOWN_C1),unknown_c3:region(storage::UNKNOWN_C3),channels};
 let mut encoded=[0xa5;init::RUNTIME_SIZE];runtime.encode(&mut encoded).unwrap();
 assert_eq!(u64::from_le_bytes(encoded[0x1f8..0x200].try_into().unwrap()),owner.address);
 runtime.PAYLOAD_FIELD=init::Region::new(owner.address,16).unwrap();
 let before=encoded;assert!(runtime.encode(&mut encoded).is_err());assert_eq!(encoded,before);
 for channel in 0..6 {for slot in 0..256 {
  let mut c=fresh();let state=storage::FIRMWARE_LOG*2;let ring=state+1;
  c.objects[state].u32(channel*0x30,slot as u32).unwrap();c.objects[state].u32(channel*0x30+0x20,((slot+1)%256) as u32).unwrap();
  let at=(channel*256+slot)*entry;c.objects[ring].u32(at,2).unwrap();
  c.objects[ring].0[at+8..at+16].copy_from_slice(&(slot as u64).to_le_bytes());
  let off=(channel*256+slot)*payload;let mut value=[0x5a;216];value[..4].copy_from_slice(&3u32.to_le_bytes());
  c.objects[PAYLOAD_SLOT].0[off..off+payload].copy_from_slice(&value);
  SEEN.lock().unwrap().clear();c.drain(&driver::AsahiDevice).unwrap();
  assert_eq!(*SEEN.lock().unwrap(),vec![(channel,value.to_vec())]);
  assert_eq!(c.objects[state].read_u32(channel*0x30).unwrap(),((slot+1)%256)as u32);
 }}
 for index in [256u64,u64::MAX] {
  let mut c=fresh();let state=storage::FIRMWARE_LOG*2;c.objects[state].u32(0x20,1).unwrap();
  c.objects[state+1].u32(0,2).unwrap();c.objects[state+1].0[8..16].copy_from_slice(&index.to_le_bytes());
  SEEN.lock().unwrap().clear();assert!(c.drain(&driver::AsahiDevice).is_err());
  assert_eq!(c.objects[state].read_u32(0).unwrap(),0);assert!(SEEN.lock().unwrap().is_empty());
 }
 println!("PASS typed ring/payload ABI, non-overlapping owners, runtime pointer bounds and all 1536 slot reads including wraparound");
}
'''


def check(directory, ref=None, mutant=None):
    def source(name):
        if ref:
            return subprocess.check_output(['git', 'show', ref + ':' + DIRECTORY + name], cwd=ROOT, text=True)
        return (ROOT / DIRECTORY / name).read_text()
    config = source('m3_config.rs')
    layout = source('m3_init_layout.rs')
    storage = source('m3_init_storage.rs')
    if mutant == 'stride':
        config = config.replace('init::fwlog::ENTRY_SIZE,init::fwlog::SLOTS', '0xd8,init::fwlog::SLOTS')
    if mutant == 'small':
        storage = storage.replace('0xfffffc2040900000,fwlog::PAYLOAD_BYTES', '0xfffffc2040900000,16')
    if mutant == 'alias':
        storage = storage.replace('0xfffffc2040900000,fwlog::PAYLOAD_BYTES', '0xfffffc204078bff0,fwlog::PAYLOAD_BYTES')
    if mutant == 'index':
        layout = layout.replace('index >= SLOTS as u64', 'index > u64::MAX')
    for name, content in [('m3_init_layout.rs', layout), ('m3_init_storage.rs', storage)]:
        (directory / name).write_text(content)
    raw = source('fw/channels.rs')
    wire = '\n'.join('#[derive(Debug,Copy,Clone,Default)]\n#[repr(C)]\n' + function(raw, 'pub(crate) struct ' + name + ' {') for name in ['RawFwLogMsg', 'RawFwLogPayloadMsg'])
    current = 'FWLOG_PAYLOAD' in storage
    code = PREFIX.replace('WIRE_STRUCTS', wire).replace('DRAIN', function(config, 'pub(crate) fn drain('))
    code = code.replace('IMPORT_PAYLOAD', 'use storage::FWLOG_PAYLOAD;' if current else '')
    code = code.replace('PAYLOAD_SLOT', 'storage::FWLOG_PAYLOAD' if current else 'storage::UNKNOWN_SMALL')
    code = code.replace('PAYLOAD_FIELD', 'fwlog_payload' if current else 'unknown_small')
    (directory / 'main.rs').write_text(code)
    subprocess.run(['rustc', '--edition=2021', '-Awarnings', str(directory / 'main.rs'), '-o', str(directory / 'control')], check=True)
    return subprocess.run([str(directory / 'control')], capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--old-ref')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='m3-fwlog-') as name:
        directory = Path(name)
        result = check(directory)
        if result.returncode: raise SystemExit(result.stderr)
        print(result.stdout, end='')
        if args.old_ref:
            result = check(directory, ref=args.old_ref)
            if not result.returncode or 'full firmware-log payload allocation' not in result.stderr:
                raise SystemExit('original allocation did not fail the capacity control')
            print('PASS original allocation rejects full payload capacity')
        for mutant in ['stride', 'small', 'alias', 'index']:
            result = check(directory, mutant=mutant)
            if not result.returncode: raise SystemExit('mutant accepted: ' + mutant)
            print('PASS rejected ' + mutant + ' mutant')


if __name__ == '__main__':
    main()
