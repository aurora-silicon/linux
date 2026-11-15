// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(dead_code,non_camel_case_types)]
mod fw {
    #[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/fw/graph.rs"]
    pub mod graph;
    pub mod work {
        #[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/fw/work/compute.rs"]
        pub mod compute;
    }
}
#[allow(unused_imports)]
use fw::{graph::manager, work::compute};
mod uapi {
    #[derive(Default)]
    pub struct drm_asahi_cmd_compute {pub cdm_ctrl_stream_base:u64,pub cdm_ctrl_stream_end:u64}
    pub struct drm_asahi_attachment {pub pointer:u64,pub size:u64}
}
fn put8(b:&mut[u8],off:usize,v:u8){b[off]=v;}
fn put16(b:&mut[u8],off:usize,v:u16){b[off..off+2].copy_from_slice(&v.to_le_bytes());}
fn put32(b:&mut[u8],off:usize,v:u32){b[off..off+4].copy_from_slice(&v.to_le_bytes());}
fn put64(b:&mut[u8],off:usize,v:u64){b[off..off+8].copy_from_slice(&v.to_le_bytes());}
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/encode/compute.rs"]
mod encode;

fn refs(variant:u64)->encode::ComputeRefs {
    let f=0xfffffc2000000000u64+variant*0x100000000;
    let g=0x1000000000+variant*0x100000000;
    encode::ComputeRefs {
        notifier:f+0x600400,qinfo:f+0x200000,user_stamp:f+0x600648,fw_stamp:f+0x600640,
        work:f+0x100000,work_gva:g+0x100000,seq:f+0x100900,preempt_gva:g+0x200000,
        its:if variant&2!=0{f+0x500040}else{f+0x1008c0},stats_fw:f+0x300000,
        auxiliary_fw:f+0x400000,auxiliary_gva:g+0x400000,context_store_gva:g+0x500000,
        counter_fw:f+0x500000,pool_state_gva:g+0x600000,page_list_gva:g+0x700000,
        usc_base:if variant&1!=0{1<<40}else{0},record_fw:if variant&1!=0{f+0x700000}else{0},
        binding_generation:1,uma_slot:1,command_id:1,compute_index:0,work_818:0,target_stamp:0x100,submitted:1,
    }
}

fn same(got:&[u8],path:std::path::PathBuf)->usize {
    let expected=std::fs::read(&path).unwrap();
    if let Some(off)=got.iter().zip(&expected).position(|(a,b)|a!=b) {
        panic!("{} differs at{off:#x}: C={:#x}, Rust={:#x}",path.display(),expected[off],got[off]);
    }
    assert_eq!(got.len(),expected.len());got.len()
}

fn main(){
    let args:Vec<_>=std::env::args().collect();let directory=std::path::Path::new(&args[1]);
    let mut checked=0;
    for variant in 0u64..4 {
        let g=0x1000000000+variant*0x100000000;
        let r=refs(variant);
        let c=uapi::drm_asahi_cmd_compute {cdm_ctrl_stream_base:g+0x300000+variant*4,cdm_ctrl_stream_end:g+0x300200+variant*8};
        let attachments:Vec<_>=(0..[0,1,16,2][variant as usize]).map(|i|uapi::drm_asahi_attachment {pointer:g+0x50000000+i*0x10000+variant*13,size:(i+1)*137}).collect();
        let mut work=[0xa5;compute::WORK_SIZE];let mut sequence=[0xa5;compute::SEQ_SIZE];
        encode::encode_compute(&r,&c,&attachments,&[0,0],1,1,1,&mut work,&mut sequence).unwrap();
        checked+=same(&work,directory.join(format!("compute-{variant}.bin")));
        assert_eq!(&sequence[..],&work[0x900..0xc00]);
        let mut state=vec![0xa5;compute::POOL_STATE_SIZE];let mut list=vec![0xa5;compute::PAGE_LIST_SIZE];
        encode::init_compute_pool(g+0x2000000,&mut state,&mut list).unwrap();state.extend(list);
        checked+=same(&state,directory.join(format!("compute-pool-{variant}.bin")));
    }
    // Native stamps wrap through zero; test encoder output at both top-bit transitions
    // and zero itself, independently of the historical first-command golden fixtures.
    for target in [0x7ffffe00,0x7fffff00,0x80000000,0xfffffe00,0xffffff00,0,0x100] {
        let r=encode::ComputeRefs {target_stamp:target,..refs(0)};
        let c=uapi::drm_asahi_cmd_compute {cdm_ctrl_stream_base:0x1000300000,cdm_ctrl_stream_end:0x1000300200};
        let mut w=[0xa5;compute::WORK_SIZE];let mut q=[0x5a;compute::SEQ_SIZE];
        encode::encode_compute(&r,&c,&[],&[0;2],1,1,1,&mut w,&mut q).unwrap();
        assert_eq!(&w[0x810..0x814],&target.to_le_bytes());
    }
    // Native command-ID, compute-index and work+818 are distinct typed fields.
    // Exercise nonzero high words so a mistaken packed u64 StartCompute store fails.
    for (id,index,opaque,target) in [(1u64,0u32,0u32,0x100u32),
        (0x1122334455667788,0x00ffffff,0x99aabbcc,0),
        (u64::MAX,u32::MAX,0x12345678,0xffffff00),
        (0x100000001,0,0xfedcba98,0x100)] {
        let r=encode::ComputeRefs {command_id:id,compute_index:index,work_818:opaque,target_stamp:target,..refs(0)};
        let c=uapi::drm_asahi_cmd_compute {cdm_ctrl_stream_base:0x1000300000,cdm_ctrl_stream_end:0x1000300200};
        let mut w=[0xa5;compute::WORK_SIZE];let mut q=[0x5a;compute::SEQ_SIZE];
        encode::encode_compute(&r,&c,&[],&[0;2],1,1,1,&mut w,&mut q).unwrap();
        assert_eq!(&w[0x820..0x828],&id.to_le_bytes());
        assert_eq!(&w[0x818..0x81c],&opaque.to_le_bytes());
        assert_eq!(&q[0x38..0x3c],&index.to_le_bytes());
        assert_eq!(&q[0x3c..0x40],&opaque.to_le_bytes());
        assert_eq!(&w[0x810..0x814],&target.to_le_bytes());
    }
    let mut cases=0;
    let r=refs(0);let c=uapi::drm_asahi_cmd_compute {cdm_ctrl_stream_base:0x1000300000,cdm_ctrl_stream_end:0x1000300200};
    let invalid:Vec<(encode::ComputeRefs,u32)>=vec![
        (r,0),(r,64),
        (encode::ComputeRefs {binding_generation:0,..r},1),
        (encode::ComputeRefs {uma_slot:0,..r},1),(encode::ComputeRefs {uma_slot:33,..r},1),
        (encode::ComputeRefs {target_stamp:0x101,..r},1),
        (encode::ComputeRefs {submitted:0,..r},1),
        (encode::ComputeRefs {work:0xfffffc1ffffffff8,..r},1),
        (encode::ComputeRefs {work:0xfffffc2ffbfff000,..r},1),
        (encode::ComputeRefs {seq:0xfffffc2ffbffff00,..r},1),
        (encode::ComputeRefs {work_gva:r.work_gva+1,..r},1),
        (encode::ComputeRefs {usc_base:1<<40|8,..r},1),
        (encode::ComputeRefs {record_fw:0xfffffc2ffbffff00,..r},1),
    ];
    for (r,ctx) in invalid {
        let mut w=[0xa5;compute::WORK_SIZE];let mut q=[0x5a;compute::SEQ_SIZE];
        assert!(encode::encode_compute(&r,&c,&[],&[0;2],ctx,1,1,&mut w,&mut q).is_err());
        assert!(w.iter().all(|&b|b==0xa5));assert!(q.iter().all(|&b|b==0x5a));cases+=1;
    }
    for (base,end) in [(0x4000,0x4000),(0x8000,0x4000),(0x4001,0x5000),(0x4000,0x5001),(0x4000,0x80004004)] {
        let c=uapi::drm_asahi_cmd_compute {cdm_ctrl_stream_base:base,cdm_ctrl_stream_end:end};
        let mut w=[0xa5;compute::WORK_SIZE];let mut q=[0x5a;compute::SEQ_SIZE];
        assert!(encode::encode_compute(&r,&c,&[],&[0;2],1,1,1,&mut w,&mut q).is_err());
        assert!(w.iter().all(|&b|b==0xa5));assert!(q.iter().all(|&b|b==0x5a));cases+=1;
    }
    for attachments in [vec![uapi::drm_asahi_attachment {pointer:0x8000,size:0}],vec![uapi::drm_asahi_attachment {pointer:u64::MAX-127,size:256}],(0..17).map(|_|uapi::drm_asahi_attachment {pointer:0x8000,size:128}).collect()] {
        let mut w=[0xa5;compute::WORK_SIZE];let mut q=[0x5a;compute::SEQ_SIZE];
        assert!(encode::encode_compute(&r,&c,&attachments,&[0;2],1,1,1,&mut w,&mut q).is_err());
        assert!(w.iter().all(|&b|b==0xa5));assert!(q.iter().all(|&b|b==0x5a));cases+=1;
    }
    for ts in [[0x8000,0],[0xfffffc2ffc000001,0],[0,0xfffffc3000000000]] {
        let mut w=[0xa5;compute::WORK_SIZE];let mut q=[0x5a;compute::SEQ_SIZE];
        assert!(encode::encode_compute(&r,&c,&[],&ts,1,1,1,&mut w,&mut q).is_err());
        assert!(w.iter().all(|&b|b==0xa5));assert!(q.iter().all(|&b|b==0x5a));cases+=1;
    }
    for previous in [1u64,0xfffffffe,u64::MAX-1] {
        let r=encode::ComputeRefs {submitted:previous+1,uma_slot:2,..r};
        let mut old=core::array::from_fn(|i|(i*73+29)as u8);
        put64(&mut old,0,r.work);put32(&mut old,8,1);put64(&mut old,0x14,r.pool_state_gva);
        put64(&mut old,0x30,r.page_list_gva);put64(&mut old,0x4c,r.counter_fw);put64(&mut old,0x54,previous);
        let mut expected=old;put32(&mut expected,8,r.uma_slot);
        let mut w=[0xa5;compute::WORK_SIZE];
        assert_eq!(encode::preserve_compute_descriptor(&r,&old,previous,&mut w).unwrap(),previous+1);
        assert_eq!(&w[0xe00..0xea0],&expected);assert!(w[..0xe00].iter().all(|&v|v==0xa5));assert!(w[0xea0..].iter().all(|&v|v==0xa5));cases+=1;
        for (off,bad) in [(0,r.work+8),(0x14,r.pool_state_gva+0x4000),(0x30,r.page_list_gva+0x4000),(0x4c,r.counter_fw+8),(0x54,previous-1)] {
            let mut damaged=old;put64(&mut damaged,off,bad);let mut w=[0xa5;compute::WORK_SIZE];
            assert!(encode::preserve_compute_descriptor(&r,&damaged,previous,&mut w).is_err());assert!(w.iter().all(|&v|v==0xa5));cases+=1;
        }
    }
    let old=[0u8;compute::DESCRIPTOR_SIZE];
    for previous in [0,u64::MAX] {let mut w=[0xa5;compute::WORK_SIZE];assert!(encode::preserve_compute_descriptor(&r,&old,previous,&mut w).is_err());assert!(w.iter().all(|&v|v==0xa5));cases+=1;}
    println!("4 compute work/sequence and allocator fixtures / {checked} bytes match qualified C exactly; {cases} rejection/reuse cases preserve private output and firmware-mutated descriptors");
}
