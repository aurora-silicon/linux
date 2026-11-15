// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(dead_code)]
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/fw/initdata.rs"]
mod wire;
const HOST: u64 = wire::HOST_START;
fn emit<T>(out:&mut Vec<u8>, value:Result<T,()>)->Option<T> { out.push(u8::from(value.is_err()));value.ok() }
fn input()->wire::Input {
    wire::Input {
        // Independently pinned qualified C/J713 bin for this immutable byte fixture.
        performance: wire::Performance {
            frequency: [0,338,618,796,928,1056,1170,1278,1338,1470],
            auxiliary: [0,338,618,796,836,952,1053,1152,1204,1326],
            voltage: [125,610,645,680,725,780,825,865,895,970],
            sram: [780,780,780,780,780,780,825,865,895,970],
        },
        va: core::array::from_fn(|i|HOST+0x100000+i as u64*0x100000),
        channel_state:core::array::from_fn(|i|HOST+0x4000000+i as u64*0x10000),
        channel_ring:core::array::from_fn(|i|HOST+0x4004000+i as u64*0x10000),
        loader_va:core::array::from_fn(|i|if wire::LOADERS[i].length!=0 {HOST+0x5000000+i as u64*0x100000+(wire::LOADERS[i].pa&0x3fff)}else{0}),
        scratch:HOST+0xa000000,lower_uma:wire::BOOTSTRAP_START+0x200000,
        lower_parameter:wire::BOOTSTRAP_START+0x240000,ktrace_mask:0,progress_recovery:1,
    }
}
fn init_result(buffers:&mut[Vec<u8>;wire::OBJECT_COUNT], input:&wire::Input, ceiling:u32)->Result<(),()> {
    let mut slices=buffers.each_mut().map(|buffer|buffer.as_mut_slice());
    wire::encode(&mut slices,input,ceiling)
}
fn main() {
    let mut out=Vec::new();
    for id in 0..wire::OBJECT_COUNT {for v in 0..6 {
        let mut va=input().va;
        va[id]=match v {0=>HOST-8,1=>wire::HOST_END,2=>va[id]+1,
            3=>va[(id+1)%wire::OBJECT_COUNT],4=>wire::HOST_END-8,
            _=>wire::HOST_END-wire::OBJECT_SIZE[id]as u64};
        emit(&mut out,wire::encode_root(&mut[0u8;0xc8],&va));
    }}
    let s=HOST+0x100000;let r=HOST+0x200000;
    let states=[s,s+1,r,r+0x1000,HOST-8,wire::HOST_END,s,s,s];
    let rings=[r,r,r,r,r,r,HOST-8,wire::HOST_END,r+1];
    for control in [false,true] {for i in 0..states.len() {
        let mut record=[0u8;0x20];
        if emit(&mut out,wire::encode_channel(&mut record,states[i],rings[i],control)).is_none(){record.fill(0);}
        out.extend_from_slice(&record);
    }}
    for slot in 0..=wire::LOADER_COUNT {for bank in 0..3 {
        let mut record=[0u8;16];
        if let Some((pa,size,offset))=emit(&mut out,wire::loader_span(slot,bank)) {
            record[..8].copy_from_slice(&pa.to_le_bytes());record[8..12].copy_from_slice(&size.to_le_bytes());record[12..].copy_from_slice(&offset.to_le_bytes());
        }
        out.extend_from_slice(&record);
    }}
    // A logical 192KiB record maps two96KiB banks32MiB apart.
    assert_eq!(wire::loader_span(3,0),Ok((0x2201c4000,0x18000,0)));
    assert_eq!(wire::loader_span(3,1),Ok((0x2221c4000,0x18000,0x18000)));
    for slot in 0..=wire::LOADER_COUNT {for v in 0..5 {
        let mut va=if slot<wire::LOADER_COUNT&&wire::LOADERS[slot].length!=0 {HOST+0x1000000+slot as u64*0x100000+(wire::LOADERS[slot].pa&0x3fff)}else{0};
        va=match v {1=>va+1,2=>HOST-1,3=>wire::HOST_END,4=>0,_=>va};
        let mut record=[0u8;0x28];
        if emit(&mut out,wire::loader_encode(slot,va,&mut record)).is_none(){record.fill(0);}
        out.extend_from_slice(&record);
    }}
    let mut buffers=wire::OBJECT_SIZE.map(|size|vec![0u8;size]);
    for ceiling in 0..11 {emit(&mut out,init_result(&mut buffers,&input(),ceiling));}
    // An absent C object has the same refusal as an absent Rust output slice.
    for id in 0..wire::OBJECT_COUNT {
        buffers[id].clear();emit(&mut out,init_result(&mut buffers,&input(),9));
        buffers[id].resize(wire::OBJECT_SIZE[id],0);
    }
    for v in 0..8 {
        let mut bad=input();
        match v {0=>bad.scratch=wire::HOST_END,1=>bad.scratch+=1,
            2=>bad.lower_uma=wire::BOOTSTRAP_START-0x4000,3=>bad.lower_parameter+=1,
            4=>bad.lower_parameter=bad.lower_uma,5=>bad.lower_parameter=bad.lower_uma+0x4000,
            6=>bad.channel_state[11]=bad.channel_ring[11],_=>bad.loader_va[41]+=1}
        emit(&mut out,init_result(&mut buffers,&bad,9));
    }
    let reference=std::fs::read(std::env::args().nth(1).unwrap()).unwrap();
    assert_eq!(out.len(),reference.len());
    for (offset,(a,b)) in out.iter().zip(&reference).enumerate(){assert_eq!(a,b,"difference at byte{offset:#x}");}
    // Rust additionally checks slice lengths before writing any object.
    assert!(wire::encode_root(&mut[0u8;0xc7],&input().va).is_err());
    assert!(wire::encode_channel(&mut[0u8;0x1f],s,r,false).is_err());
    assert!(wire::loader_encode(0,HOST+0x100000,&mut[0u8;0x27]).is_err());
    println!("598 C/Rust bound, overlap, ceiling and loader cases match ({} output bytes);3 Rust short-buffer refusals;slot3 two-bank geometry exact",out.len());
}
