#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Test real preserved-ADT GPU binning, rejection and the actual firmware table encoder."""
from common import HERE, kernel_root, output_dir, rustc, sha
import hashlib
import json
import struct
import subprocess

root = kernel_root()
out = output_dir()
fixture = HERE / 'fixtures/m4-perf-source.json'
if sha(fixture) != '9ddbafa50e3f517baafa7982c07d021609ae0a3c13828c1a2f05da50dc4f77e3':
    raise RuntimeError('Independently acquired original ADT performance fixture changed')
metadata = json.loads(fixture.read_text())
assert metadata['provenance']['source_sha256'] == '19c11cbd5e4bedc74670b6e188a8deb0f84207014728baed59344420bbfd225f'
properties = {name: bytes.fromhex(value) for name, value in metadata['properties_hex'].items()}

def node(props, children=()):
    image = struct.pack('<II', len(props), len(children))
    for name, value in props.items():
        image += name.encode().ljust(32, b'\0') + struct.pack('<I', len(value))
        image += value + b'\0' * (-len(value) % 4)
    return image + b''.join(children)

# Minimal synthetic envelope around exact measured properties, with no unrelated machine data.
image = node({'name': b'device-tree\0'}, [node({'name': b'arm-io\0'}, [node({'name': b'sgx\0', **properties})])])
blob = out / 'measured-perf.adt'
blob.write_bytes(image)
backend = root / 'drivers/gpu/drm/asahi/g16'
source = r'''
#![allow(dead_code)]
mod backend {
    pub mod adt { #[path = "@PARSER@"] pub mod perf; }
    pub mod fw { #[path = "@ENCODER@"] pub mod initdata; }
    pub mod board { #[path = "@SELECT@"] pub mod data; }
}
use backend::{adt::perf, board::data, fw::initdata as wire};
const MEASURED: &[u8] = include_bytes!("@FIXTURE@");
fn put32(out: &mut [u8], at: usize, value: u32) { out[at..at+4].copy_from_slice(&value.to_le_bytes()); }
fn property_offset(data: &[u8], name: &[u8]) -> usize {
    data.windows(32).position(|value| value.starts_with(name) && value[name.len()..].iter().all(|&x| x==0)).unwrap()
}
fn set_cell(data: &mut [u8], name: &[u8], value: u32) {
    let at = property_offset(data, name) + 36; put32(data, at, value);
}
fn source() -> perf::Source { perf::parse(MEASURED).unwrap() }
fn tables() -> wire::Performance { data::select(&source()).unwrap() }
fn input(performance: wire::Performance) -> wire::Input {
    const HOST: u64 = wire::HOST_START;
    wire::Input {
        performance,
        va: core::array::from_fn(|i| HOST + 0x100000 + i as u64 * 0x100000),
        channel_state: core::array::from_fn(|i| HOST + 0x4000000 + i as u64 * 0x10000),
        channel_ring: core::array::from_fn(|i| HOST + 0x4004000 + i as u64 * 0x10000),
        loader_va: core::array::from_fn(|i| if wire::LOADERS[i].length==0 {0} else {HOST+0x5000000+i as u64*0x100000+(wire::LOADERS[i].pa&0x3fff)}),
        scratch: HOST+0xa000000, lower_uma: wire::BOOTSTRAP_START,
        lower_parameter: wire::BOOTSTRAP_START+0x8000, ktrace_mask:0, progress_recovery:1,
    }
}
fn encode(performance: wire::Performance) -> Result<[Vec<u8>; wire::OBJECT_COUNT], ()> {
    let mut buffers = wire::OBJECT_SIZE.map(|size| vec![0xa5; size]);
    wire::encode(&mut buffers.each_mut().map(|b| b.as_mut_slice()), &input(performance), 9)?;
    Ok(buffers)
}
fn get32(bytes: &[u8], at: usize) -> u32 { u32::from_le_bytes(bytes[at..at+4].try_into().unwrap()) }
#[test] fn measured_original_adt_produces_qualified_tables() {
    let s=source(); assert_eq!(s.count,16);
    // Keep the original frequency layout but use synthetic voltage bins.
    let mut s=s;
    s.core_mv[..16].copy_from_slice(&[100,600,650,700,750,750,800,800,850,850,900,900,950,950,1000,1000]);
    s.sram_mv[..16].copy_from_slice(&[800,800,800,800,800,800,800,800,850,850,900,900,950,950,1000,1000]);
    let p=data::select(&s).unwrap(); assert_eq!(p.count,10);
    assert_eq!(p.frequency,[0,338,618,796,928,1056,1170,1278,1338,1470,0]);
    assert_eq!(p.auxiliary,[0,338,618,796,836,952,1053,1152,1204,1326,0]);
    assert_eq!(p.voltage,[100,600,650,700,750,800,850,900,950,1000,0]);
    assert_eq!(p.sram,[800,800,800,800,800,800,850,900,950,1000,0]);
    assert_eq!(get32(&encode(p).unwrap()[wire::id::MAIN],0xfc0),10);
}
// J773g frequency layout with synthetic voltage bins, both with eleven groups.
fn j773g(core: [u32; 16], sram: [u32; 16]) -> perf::Source {
    let mhz=[0,338,618,796,928,952,1056,1053,1170,1152,1278,1204,1338,1326,1470,1578];
    let mut s=source(); s.count=16;
    s.frequency_mhz=[0;perf::MAX_STATES]; s.sram_frequency_mhz=[0;perf::MAX_STATES];
    s.core_mv=[0;perf::MAX_STATES]; s.sram_mv=[0;perf::MAX_STATES];
    s.frequency_mhz[..16].copy_from_slice(&mhz); s.sram_frequency_mhz[..16].copy_from_slice(&mhz);
    s.core_mv[..16].copy_from_slice(&core); s.sram_mv[..16].copy_from_slice(&sram);
    s
}
#[test] fn j773g_synthetic_bins_produce_eleven_rows_and_encode_all_of_them() {
    for (core,sram,voltage,row_sram) in [
        ([100,600,620,660,700,750,750,800,800,850,850,900,900,950,950,1000],
         [740,740,740,740,740,750,750,800,800,850,850,900,900,950,950,1000],
         [100,600,620,660,700,750,800,850,900,950,1000],
         [740,740,740,740,740,750,800,850,900,950,1000]),
        ([100,590,630,650,690,730,730,790,790,840,840,890,890,940,940,990],
         [760,760,760,760,760,760,760,790,790,840,840,890,890,940,940,990],
         [100,590,630,650,690,730,790,840,890,940,990],
         [760,760,760,760,760,760,790,840,890,940,990]),
    ] {
        let (p,name)=data::select_named(&j773g(core,sram),Some(11)).unwrap();
        assert_eq!(name,"J773g"); assert_eq!(p.count,11);
        assert_eq!(p.frequency,[0,338,618,796,928,1056,1170,1278,1338,1470,1578]);
        assert_eq!(p.auxiliary,[0,338,618,796,928,952,1053,1152,1204,1326,1578]);
        assert_eq!(p.voltage,voltage); assert_eq!(p.sram,row_sram);
        let mut buffers=wire::OBJECT_SIZE.map(|size|vec![0xa5;size]);
        wire::encode(&mut buffers.each_mut().map(|b|b.as_mut_slice()),&input(p.clone()),10).unwrap();
        let main=&buffers[wire::id::MAIN];
        assert_eq!(get32(main,0xfc0),11); assert_eq!(get32(main,0xfc4),10);
        for i in 0..11 {
            assert_eq!(get32(main,0xfc8+i*4),p.frequency[i]);
            assert_eq!(get32(main,0x1808+i*4),p.auxiliary[i]);
            assert_eq!(get32(main,0x1008+i*16*4),p.voltage[i]);
            assert_eq!(get32(main,0x1408+(i*16+15)*4),p.sram[i]);
        }
    }
}
#[test] fn the_row_count_bounds_the_encoded_ceiling() {
    let j713=tables(); let j773g=data::select_named(&j773g(
        [100,600,620,660,700,750,750,800,800,850,850,900,900,950,950,1000],
        [740,740,740,740,740,750,750,800,800,850,850,900,900,950,950,1000]),Some(11)).unwrap().0;
    for (p,good,bad) in [(j713,[1,9],[0,10]),(j773g,[1,10],[0,11])] {
        for ceiling in good {
            let mut buffers=wire::OBJECT_SIZE.map(|size|vec![0xa5;size]);
            assert!(wire::encode(&mut buffers.each_mut().map(|b|b.as_mut_slice()),&input(p.clone()),ceiling).is_ok());
            assert_eq!(get32(&buffers[wire::id::MAIN],0xfc0),p.count as u32);
        }
        for ceiling in bad {
            let mut buffers=wire::OBJECT_SIZE.map(|size|vec![0xa5;size]);
            assert!(wire::encode(&mut buffers.each_mut().map(|b|b.as_mut_slice()),&input(p.clone()),ceiling).is_err());
            assert!(buffers.iter().all(|buffer|buffer.iter().all(|&byte|byte==0xa5)));
        }
    }
    // Rows past the count must stay zero, and the count must match the rows.
    let mut p=tables(); p.frequency[10]=1500; assert!(!p.valid());
    let mut p=tables(); p.count=11; assert!(!p.valid());
}
#[test] fn different_real_bin_values_reach_all_firmware_lanes() {
    let mut raw=MEASURED.to_vec();
    for name in [b"perf-states".as_slice(),b"perf-states-sram".as_slice()] {
        let at=property_offset(&raw,name)+36;
        for i in 0..16 {
            let hz=get32(&raw,at+i*8);let mv=get32(&raw,at+i*8+4);
            put32(&mut raw,at+i*8,if i==0 {0}else{hz+1_000_000});
            put32(&mut raw,at+i*8+4,mv+5);
        }
    }
    let p=data::select(&perf::parse(&raw).unwrap()).unwrap();
    let encoded=encode(p.clone()).unwrap();let main=&encoded[wire::id::MAIN];
    for i in 0..10 {
        assert_eq!(get32(main,0xfc8+i*4),p.frequency[i]);
        assert_eq!(get32(main,0x1808+i*4),p.auxiliary[i]);
        for lane in 0..16 {
            assert_eq!(get32(main,0x1008+(i*16+lane)*4),p.voltage[i]);
            assert_eq!(get32(main,0x1408+(i*16+lane)*4),p.sram[i]);
        }
    }
    assert_eq!(p.frequency[9],1471);assert_eq!(p.voltage[9],975);
}
#[test] fn truncations_and_overflowing_tree_headers_reject_without_panic() {
    for end in 0..MEASURED.len() {assert!(perf::parse(&MEASURED[..end]).is_err());}
    for at in [0,4,40,property_offset(MEASURED,b"perf-states")+32,property_offset(MEASURED,b"compatible")+32] {
        let mut bad=MEASURED.to_vec();put32(&mut bad,at,u32::MAX);
        assert!(perf::parse(&bad).is_err());
    }
    assert!(perf::parse(&vec![0;0x20_0001]).is_err());
    let mut deep=Vec::new();
    for _ in 0..34 {deep.extend_from_slice(&0u32.to_le_bytes());deep.extend_from_slice(&1u32.to_le_bytes());}
    deep.extend_from_slice(&[0;8]);assert!(perf::parse(&deep).is_err());
    let mut padded=MEASURED.to_vec();padded.extend_from_slice(&[0;4096]);assert_eq!(perf::parse(&padded),Ok(source()));
}
#[test] fn missing_wrong_identity_and_duplicate_required_properties_reject() {
    for name in [b"compatible".as_slice(),b"perf-state-count",b"perf-states",b"perf-states-sram"] {
        let mut bad=MEASURED.to_vec();let at=property_offset(&bad,name);bad[at]=b'X';
        assert!(perf::parse(&bad).is_err());
    }
    let mut bad=MEASURED.to_vec();let at=property_offset(&bad,b"compatible")+36;bad[at]=b'X';
    assert_eq!(perf::parse(&bad),Err(perf::Error::Identity));
    let mut bad=MEASURED.to_vec();let at=property_offset(&bad,b"perf-states-sram");
    bad[at..at+32].fill(0);bad[at..at+11].copy_from_slice(b"perf-states");
    assert_eq!(perf::parse(&bad),Err(perf::Error::Duplicate));
}
#[test] fn metadata_shape_frequency_units_off_state_and_voltage_ranges_reject() {
    for (name,value) in [(b"perf-state-count".as_slice(),0),(b"perf-state-count",33),(b"perf-state-count",15),(b"perf-state-table-count",2),(b"gpu-num-perf-states",14)] {
        let mut bad=MEASURED.to_vec();set_cell(&mut bad,name,value);assert!(perf::parse(&bad).is_err());
    }
    let at=property_offset(MEASURED,b"perf-states")+36;
    for (offset,value) in [(0,1),(8,0),(8,338_000_001),(8,3_001_000_000),(12,99),(12,1101)] {
        let mut bad=MEASURED.to_vec();put32(&mut bad,at+offset,value);assert!(perf::parse(&bad).is_err());
    }
}
#[test] fn unsupported_shape_pairs_and_ordering_reject_before_encoding() {
    let mut s=source();s.count=15;assert!(data::select(&s).is_err());
    let mut s=source();s.core_mv[4]+=1;assert!(data::select(&s).is_err());
    let mut s=source();s.sram_mv[4]+=1;assert!(data::select(&s).is_err());
    let mut s=source();s.sram_frequency_mhz[4]+=1;assert!(data::select(&s).is_err());
    let mut s=source();s.frequency_mhz[4]=s.frequency_mhz[5]+1;s.sram_frequency_mhz[4]=s.frequency_mhz[4];assert!(data::select(&s).is_err());
    let mut s=source();s.frequency_mhz[7]=s.frequency_mhz[5];s.sram_frequency_mhz[7]=s.frequency_mhz[7];assert!(data::select(&s).is_err());
    let mut s=source();s.core_mv[14]=600;s.core_mv[15]=600;assert!(data::select(&s).is_err());
    // Three states at one voltage, and split pairs, match no layout.
    let mut s=source();s.core_mv[6]=s.core_mv[5];s.sram_mv[6]=s.sram_mv[5];assert!(data::select(&s).is_err());
    let mut s=source();s.core_mv[4]-=1;s.core_mv[6]-=1;assert!(data::select(&s).is_err());
}
#[test] fn j604_shape_selects_its_own_layout_and_the_j713_keeps_its_own() {
    assert_eq!(data::select_named(&source(),None).unwrap().1,"J713");
    // J604 shape: five low states, equal-voltage pairs 5/6 to 13/14, a single top state 15.
    // Rows 0-3 and 5-15 are the J604's selected values; state 4 (left out) is synthetic.
    let mhz=[0,338,618,796,928,952,1056,1053,1170,1152,1278,1204,1338,1326,1470,1578];
    let mv=[125,625,640,680,725,780,780,825,825,865,865,895,895,970,970,1045];
    let mut s=source();
    for i in 0..16 {
        s.frequency_mhz[i]=mhz[i];s.sram_frequency_mhz[i]=mhz[i];
        s.core_mv[i]=mv[i];s.sram_mv[i]=mv[i].max(780);
    }
    let (p,name)=data::select_named(&s,None).unwrap();
    assert_eq!(name,"J604"); assert_eq!(p.count,10);
    assert_eq!(p.frequency,[0,338,618,796,1056,1170,1278,1338,1470,1578,0]);
    assert_eq!(p.auxiliary,[0,338,618,796,952,1053,1152,1204,1326,1578,0]);
    assert_eq!(p.voltage,[125,625,640,680,780,825,865,895,970,1045,0]);
    assert_eq!(p.sram,[780,780,780,780,780,825,865,895,970,1045,0]);
    assert!(encode(p.clone()).is_ok());
    // The board's row count picks between the two layouts of this shape.
    assert_eq!(data::select_named(&s,Some(10)).unwrap(),(p,"J604"));
    let (p,name)=data::select_named(&s,Some(11)).unwrap();
    assert_eq!(name,"J773g"); assert_eq!(p.count,11);
    assert_eq!(p.frequency,[0,338,618,796,928,1056,1170,1278,1338,1470,1578]);
    assert_eq!(p.auxiliary,[0,338,618,796,928,952,1053,1152,1204,1326,1578]);
    // The J713 shape has no eleven-row layout, and no board has another row count.
    assert_eq!(data::select_named(&source(),Some(10)).unwrap().1,"J713");
    assert_eq!(data::select_named(&source(),Some(11)),Err(perf::Error::Shape));
    for rows in [0,9,12] { assert_eq!(data::select_named(&s,Some(rows)),Err(perf::Error::Shape)); }
    // A J604 shape with one unequal pair matches no layout.
    let mut bad=s;bad.core_mv[9]+=1;bad.sram_mv[9]+=1;
    assert_eq!(data::select(&bad),Err(perf::Error::Shape));
    assert_eq!(data::select_named(&bad,Some(11)),Err(perf::Error::Shape));
}
#[test] fn invalid_encoder_input_is_refused_without_mutating_output() {
    for variant in 0..5 {
        let mut p=tables(); match variant {0=>p.frequency[1]=0,1=>p.auxiliary[5]=p.frequency[5]+1,2=>p.voltage[9]=1101,3=>p.sram[9]=100,_=>p.frequency[0]=1};
        let mut buffers=wire::OBJECT_SIZE.map(|size|vec![0xa5;size]);
        assert!(wire::encode(&mut buffers.each_mut().map(|b|b.as_mut_slice()),&input(p),9).is_err());
        assert!(buffers.iter().all(|buffer|buffer.iter().all(|&byte|byte==0xa5)));
    }
}
'''
for marker, value in {'PARSER': backend / 'adt/perf.rs', 'ENCODER': backend / 'fw/initdata.rs',
                      'SELECT': backend / 'board/data.rs', 'FIXTURE': blob}.items():
    source = source.replace('@' + marker + '@', str(value))
path = out / 'perf.rs'
path.write_text(source)
binary = out / 'perf-tests'
subprocess.run(rustc() + ['--edition=2021', '--test', '-C', 'overflow-checks=yes', str(path), '-o', str(binary)], check=True)
run = subprocess.run([str(binary), '--test-threads=1'], text=True, capture_output=True)
print(run.stdout, end='')
print(run.stderr, end='')
run.check_returncode()
report = {'passed': True, 'test_groups': 10, 'hardware_validated': False,
          'fixture_sha256': sha(fixture), 'original_adt_sha256': metadata['provenance']['source_sha256'],
          'production_sha256': {str(p.relative_to(root)): sha(p) for p in
              [backend / 'adt.rs', backend / 'adt/perf.rs', backend / 'board.rs', backend / 'board/data.rs', backend / 'initdata.rs', backend / 'fw/initdata.rs']}}
(out / 'perf-results.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
