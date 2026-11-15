// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(dead_code)]
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/fw/channels.rs"]
mod channels;
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/fw/graph.rs"]
mod graph;
mod fwid {
@FWID_BUILD@
}
mod hw {pub enum GpuVariant {G}}
struct Config {
    chip_id:u32,gpu_variant:hw::GpuVariant,gpu_revision:u32,uat_ias:u32,uat_oas:u32,
    num_clusters:u32,num_cores_per_cluster:u32,
    hw_ids:&'static[backend_hw::HwId],firmware:&'static[backend_hw::FirmwareBuild],
}
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/hw.rs"]
mod backend_hw;
fn main() {
    let args:Vec<_>=std::env::args().collect();let mut got=Vec::new();
    for pipe in 0..12 {for h in [0,1,255] {for e in [0,15,112,113,127] {for new in [false,true] {for timestamp in [0,0x12345678abcdef00,u64::MAX] {
        let record=channels::SubmitRecord {unk_0:timestamp,qinfo:0xfffffc2000100000+pipe*0x4000,kind:(pipe%3)as u32,head:h,event:e,new_queue:u8::from(new)};
        got.extend(record.to_bytes());got.extend((backend_hw::engine::DOORBELL|pipe).to_le_bytes());
    }}}}}
    for v in 0..4 {
        let f=0xfffffc2000000000u64+v*0x100000000;
        let mut state=vec![0;graph::qstate::SIZE];state[graph::qstate::CAP..graph::qstate::CAP+4].copy_from_slice(&channels::RING_SLOTS.to_le_bytes());
        let mut queue=vec![0;graph::qinfo::SIZE];queue[..0xb4].copy_from_slice(&graph::queue_image(f+0x100000,f+0x200000,f+0x400000,f+0x300000,f+0x500000));
        let context=graph::context_image();let mut notifiers=vec![0;0x1000];
        for engine in 0..3 {notifiers[engine*0x200..engine*0x200+0x118].copy_from_slice(&graph::notifier_image(f+0x600600+engine as u64*8,1));}
        got.extend(state);got.extend(queue);got.extend(context);got.extend(notifiers);
    }
    let expected=std::fs::read(&args[1]).unwrap();
    if let Some(off)=got.iter().zip(&expected).position(|(a,b)|a!=b){panic!("channel/graph differs at{off:#x}: C={:#x},Rust={:#x}",expected[off],got[off]);}
    assert_eq!(got.len(),expected.len());
    assert_eq!((channels::devctl::NOP,channels::devctl::IMPORT_GLOBALS,channels::devctl::START),(0x1a,0x34,0x0a));
    assert_eq!(backend_hw::engine::DEVCTL_DOORBELL,0x0084000000000011);
    assert_eq!((backend_hw::ep::FIRMWARE,backend_hw::ep::DOORBELL,backend_hw::ep::INITDATA),(0x20,0x21,0x0081000000000000));
    for op in [channels::devctl::NOP,channels::devctl::IMPORT_GLOBALS,channels::devctl::START] {
        let record=channels::DevCtl::op(op);assert_eq!(record.opcode(),op);let bytes=record.to_bytes();
        assert_eq!(&bytes[..4],&op.to_le_bytes());assert!(bytes[4..].iter().all(|&v|v==0));
    }
    println!("1080 submit records/kicks and4queue/context/context-one notifier images / {} bytes match qualified C; initial device-control opcodes pass",got.len());
}
