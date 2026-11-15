// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(dead_code,non_camel_case_types,non_snake_case,non_upper_case_globals,unused_imports)]
extern crate self as kernel;
pub mod prelude { pub type Result<T=()> = core::result::Result<T,i32>; pub const E2BIG:i32=7; }
mod uapi {include!("public-uapi.rs");}
mod hw {
    pub mod engine {pub const COMPUTE_SEED:u32=0x100;}
    pub mod client {pub const PB_HEAP_SIZE:u64=0x10000000;}
}
mod fw {
    #[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/fw/graph.rs"]
    pub mod graph;
    #[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/fw/work.rs"]
    pub mod work;
}
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/encode.rs"]
mod encode;
fn main() {
    let args:Vec<_>=std::env::args().collect();let directory=std::path::Path::new(&args[1]);
    let dimensions=[[64,64,1,1,32,32,4],[1919,1079,3,2,16,32,4],[2560,1664,1,4,16,16,8],[16384,1,2,1,32,16,16]];
    let counts=[[0,1],[1,2],[16,16],[2,0]];let mut checked=0;
    for v in 0usize..4 {
        let f=0xfffffc2000000000u64+v as u64*0x100000000;
        let g=0x1000000000+v as u64*0x100000000;
        let mut u=uapi::drm_asahi_cmd_render::default();
        let [width,height,layers,samples,uw,uh,sample]=dimensions[v];
        u.width_px=width as u16;u.height_px=height as u16;u.layers=layers as u16;
        u.samples=samples as u8;u.utile_width_px=uw as u8;u.utile_height_px=uh as u8;u.sample_size_B=sample as u8;
        u.flags=uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_RSRC_SPEC_HI;
        if v&1!=0 {u.flags|=uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_PROCESS_EMPTY_TILES;}
        if v&2!=0 {u.flags|=1<<18;}
        u.vdm_ctrl_stream_base=g+0xa00000;
        u.isp_scissor_base=g+0xb00000+v as u64*4;u.isp_dbias_base=g+0xc00000+v as u64*4;u.isp_oclqry_base=g+0xd00000+v as u64*16;
        let resources=[0x1234000010u64+v as u64,0x5678000020+v as u64,0x9876000030+v as u64,0x5432000040+v as u64];
        u.bg.rsrc_spec=resources[0] as u32;u.bg_rsrc_spec_hi=(resources[0]>>32) as u32;u.bg.usc=0x10000;
        u.eot.rsrc_spec=resources[1] as u32;u.bg_eot_rsrc_spec_hi=(resources[1]>>32) as u32;u.eot.usc=0x20000;
        u.partial_bg.rsrc_spec=resources[2] as u32;u.bg_partial_rsrc_spec_hi=(resources[2]>>32) as u32;u.partial_bg.usc=0x30000;
        u.partial_eot.rsrc_spec=resources[3] as u32;u.bg_eot_partial_rsrc_spec_hi=(resources[3]>>32) as u32;u.partial_eot.usc=0x40000;
        u.ppp_multisamplectl=0x8000000000000000+v as u64;u.ppp_ctrl=0x201;
        u.isp_merge_upper_x=0x3f800000+v as u32;u.isp_merge_upper_y=0x40000000+v as u32;
        u.isp_bgobjdepth=0x3e800000+v as u32;u.isp_bgobjvals=0x100+v as u32;
        u.zls_ctrl=if v!=0 {0x1234567800000000+v as u64}else{0};u.depth.stride=0x123400+v as u32;u.stencil.stride=0x567800+v as u32;
        u.isp_zls_pixels=0x1000+v as u32;u.depth.base=g+0xe00000;u.stencil.base=g+0xf00000;
        let r=encode::RenderRefs {
            notifier:f+0x800000,qinfo:[f+0xf00000,f+0x1000000],
            user_stamp:[f+0x800620,f+0x800630],fw_stamp:[f+0x4000000,f+0x4004000],
            uma_mgr:[f+0x300000,f+0x300000],uma_aux:[f+0x300100,f+0x300200],uma_gpu:g+0x6000000,pb_mgr:f+0x3000000,
            scene_list:g+0x500000,parameter_id:[0,3,31,15][v],generation:[1,2,254,127][v],
            activity:[f+0x900004,f+0xa00008],counter:[2+v as u64*2,1+v as u64*2],
            uma_submitted:2+v as u64*2,uuid:0x123400+v as u32,usc_base:1<<40,
            ..Default::default()
        };
        let p=encode::PassRefs {
            ta_work:f+0xb00000,ta_work_gva:g+0x7000000,fs_work:f+0xc00000,fs_work_gva:g+0x8000000,
            ta_seq:f+0xd00000,fs_seq:f+0xe00000,scene:f+0x100000,tpc_gva:g+0x200000,tpc_fw:f+0x3200000,
            tilemap_gva:g+0x100000,heap_gva:g+0x300000,scene_user_gva:g+0x900000,scratch_fw:f+0x200000,
            pre_gva:[g+0x600000,g+0x700000,g+0x800000],aux_gva:g+0xb00000,..Default::default()
        };
        let geo=encode::Geometry::new(&u,p.heap_gva);let ctx=[1,2,63,7][v];let target=0x100+v as u32*0x100;
        let mut ta=[0xa5;fw::work::ta::WORK_SIZE];let mut fs=[0xa5;fw::work::fs::WORK_SIZE];
        let mut tq=[0xa5;fw::work::ta::SEQ_SIZE];let mut fq=[0xa5;fw::work::fs::SEQ_SIZE];
        encode::encode_ta(&r,&p,&u,&[0;4],ctx,target,&geo,None,&mut ta,&mut tq);
        encode::encode_fs(&r,&p,&u,&[0;4],ctx,1<<40,1,target,&geo,&mut fs,&mut fq);
        // Manager metadata can live in a firmware-only allocation while TA/FS
        // auxiliary records use unrelated shared backing. Only the two explicit
        // wire pointers may change; neither encoder may derive them from M+100.
        let mut separate=r;
        separate.uma_aux=[f+0x6500100,f+0x7800200];
        let mut ta_separate=[0xa5;fw::work::ta::WORK_SIZE];
        let mut fs_separate=[0xa5;fw::work::fs::WORK_SIZE];
        let mut tq_separate=[0xa5;fw::work::ta::SEQ_SIZE];
        let mut fq_separate=[0xa5;fw::work::fs::SEQ_SIZE];
        encode::encode_ta(&separate,&p,&u,&[0;4],ctx,target,&geo,None,&mut ta_separate,&mut tq_separate);
        encode::encode_fs(&separate,&p,&u,&[0;4],ctx,1<<40,1,target,&geo,&mut fs_separate,&mut fq_separate);
        assert_eq!(&ta_separate[0x90f..0x917],&separate.uma_aux[0].to_le_bytes());
        assert_eq!(&fs_separate[0xc57..0xc5f],&separate.uma_aux[1].to_le_bytes());
        for i in 0..ta.len(){if !(0x90f..0x917).contains(&i){assert_eq!(ta[i],ta_separate[i],"TA unrelated byte {i:#x}");}}
        for i in 0..fs.len(){if !(0xc57..0xc5f).contains(&i){assert_eq!(fs[i],fs_separate[i],"FS unrelated byte {i:#x}");}}
        assert_eq!(tq,tq_separate);assert_eq!(fq,fq_separate);
        for engine in 0..2 {
            let att:Vec<_>=(0..counts[v][engine]).map(|i|uapi::drm_asahi_attachment {pointer:g+0x50000000+i as u64*0x10000+v as u64*13,size:(i as u64+1)*137,..Default::default()}).collect();
            encode::render_attachments(if engine==0{&mut tq[..]}else{&mut fq[..]},engine,&att);
        }
        let got:Vec<_>=ta.into_iter().chain(fs).chain(tq).chain(fq).collect();
        let expected=std::fs::read(directory.join(format!("render-{v}.bin"))).unwrap();
        if let Some(off)=got.iter().zip(&expected).position(|(a,b)|a!=b) {
            panic!("render{v} differs at{off:#x}: C={:#x},Rust={:#x}",expected[off],got[off]);
        }
        assert_eq!(got.len(),expected.len());checked+=got.len();
        let mut ts=[0u64;4];let mut copies=[0u64;8];
        for i in 0..4 {if v==2||(v==1&&(i==0||i==3)){ts[i]=0xfffffc2ffc000000+0x100+v as u64*0x100+i as u64*8;}}
        for i in 0..8 {if v==3||(v==2&&(i==0||i==3||i==7))||(v==1&&i==0){copies[i]=0xfffffc2ffc000000+0x1000+v as u64*0x100+i as u64*8;}}
        let mut fq=vec![0xa5;fw::work::fs::SEQ_CAPACITY];
        encode::encode_ta(&r,&p,&u,&ts,ctx,target,&geo,None,&mut ta,&mut tq);
        encode::encode_fs(&r,&p,&u,&ts,ctx,1<<40,1,target,&geo,&mut fs,&mut fq);
        for engine in 0..2 {
            let att:Vec<_>=(0..counts[v][engine]).map(|i|uapi::drm_asahi_attachment {pointer:g+0x50000000+i as u64*0x10000+v as u64*13,size:(i as u64+1)*137,..Default::default()}).collect();
            encode::render_attachments(if engine==0{&mut tq[..]}else{&mut fq[..]},engine,&att);
        }
        let mut scene=vec![0xa5;fw::work::scene::SIZE];
        encode::fill_scene(&r,&p,&mut scene);
        encode::scene_render(&r,&p,&ts,ctx,target,v&1!=0,&mut scene);
        for wrapped in [0x7fffff00,0x80000000,0xffffff00,0,0x100] {
            let mut ta=[0xa5;fw::work::ta::WORK_SIZE];let mut fs=[0xa5;fw::work::fs::WORK_SIZE];
            let mut tq=[0xa5;fw::work::ta::SEQ_SIZE];let mut fq=[0xa5;fw::work::fs::SEQ_SIZE];
            encode::encode_ta(&r,&p,&u,&[0;4],ctx,wrapped,&geo,None,&mut ta,&mut tq);
            encode::encode_fs(&r,&p,&u,&[0;4],ctx,1<<40,1,wrapped,&geo,&mut fs,&mut fq);
            assert_eq!(&ta[0x888..0x88c],&wrapped.to_le_bytes());
            assert_eq!(&fs[0xbd0..0xbd4],&wrapped.to_le_bytes());
            let mut scene=vec![0xa5;fw::work::scene::SIZE];
            encode::fill_scene(&r,&p,&mut scene);
            encode::scene_render(&r,&p,&[0;4],ctx,wrapped,false,&mut scene);
        }
        let length=encode::append_fs_timestamp_copies(&mut fs,&mut fq,&p,&copies,&mut scene).unwrap();
        let got:Vec<_>=ta.into_iter().chain(fs).chain(tq).chain(fq[..length].iter().copied()).chain(scene).collect();
        let expected=std::fs::read(directory.join(format!("render-timestamps-{v}.bin"))).unwrap();
        if let Some(off)=got.iter().zip(&expected).position(|(a,b)|a!=b){panic!("renderTS{v} differs at{off:#x}: C={:#x},Rust={:#x}",expected[off],got[off]);}
        assert_eq!(got.len(),expected.len());checked+=got.len();
    }
    for (ordinal,expected) in [(0,0),(1,0x100),(0x7fffff,0x7fffff00),(0x800000,0x80000000),(0xffffff,0xffffff00),(0x1000000,0),(0x1000001,0x100)] {
        assert_eq!(encode::stamp_seed(0x7a000000,ordinal),expected);
        assert_eq!(encode::stamp_seed(0xc1000000,ordinal),expected);
    }
    for (sequence_length,scene_length,copies) in [(fw::work::fs::SEQ_SIZE-1,0x4000,[0;8]),(fw::work::fs::SEQ_SIZE+0x4c-1,0x4000,[0xfffffc2ffc000000;8]),(fw::work::fs::SEQ_CAPACITY,0x77f,[0xfffffc2ffc000000;8])] {
        let mut w=[0xa5;fw::work::fs::WORK_SIZE];let mut q=vec![0x5a;sequence_length];let mut scene=vec![0x37;scene_length];
        assert!(encode::append_fs_timestamp_copies(&mut w,&mut q,&Default::default(),&copies,&mut scene).is_none());
        assert!(w.iter().all(|&v|v==0xa5));assert!(q.iter().all(|&v|v==0x5a));assert!(scene.iter().all(|&v|v==0x37));
    }
    println!("8 TA/FS work+sequence/scene/timestamp fixtures / {checked} bytes match qualified C exactly;14stamp wrap checks,4isolated UMA auxiliary-pointer cases and3short-storage rejection cases pass");
}
