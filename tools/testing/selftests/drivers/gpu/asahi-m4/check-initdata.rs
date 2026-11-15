// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(dead_code)]
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/fw/initdata.rs"]
mod initdata;

fn input(variant: u64) -> initdata::Input {
    let start = 0xfffffc2000000000u64;
    let shift = variant * 0x100000000;
    let mut input = initdata::Input {
        // Independently pinned qualified C/J713 bin for this immutable byte fixture.
        performance: initdata::Performance {
            frequency: [0,338,618,796,928,1056,1170,1278,1338,1470],
            auxiliary: [0,338,618,796,836,952,1053,1152,1204,1326],
            voltage: [125,610,645,680,725,780,825,865,895,970],
            sram: [780,780,780,780,780,780,825,865,895,970],
        },
        va: core::array::from_fn(|i| start + shift + 0x100000 + i as u64 * 0x100000),
        channel_state: core::array::from_fn(|i| start + shift + 0x4000000 + i as u64 * 0x10000),
        channel_ring: core::array::from_fn(|i| start + shift + 0x4004000 + i as u64 * 0x10000),
        loader_va: [0; initdata::LOADER_COUNT],
        scratch: start + shift + 0xa000000,
        lower_uma: 0x7000200000,
        lower_parameter: 0x7000240000,
        ktrace_mask: [0, 0x12345678, 0xfedcba98][variant as usize],
        progress_recovery: u32::from(variant != 1),
    };
    for (i, loader) in initdata::LOADERS.iter().enumerate() {
        if loader.length != 0 {
            input.loader_va[i] = start + shift + 0x5000000 + i as u64 * 0x100000 + (loader.pa & 0x3fff);
        }
    }
    input
}

fn main() {
    let args: Vec<_> = std::env::args().collect();
    let directory = std::path::Path::new(&args[1]);
    let mut bytes_checked = 0;
    for (variant, ceiling) in [1, 9, 4].into_iter().enumerate() {
        let mut buffers = initdata::OBJECT_SIZE.map(|size| vec![0xa5u8; size]);
        let mut slices: [&mut [u8]; initdata::OBJECT_COUNT] = buffers.each_mut().map(|buffer| buffer.as_mut_slice());
        initdata::encode(&mut slices, &input(variant as u64), ceiling).expect("valid C fixture rejected");
        let got: Vec<_> = buffers.into_iter().flatten().collect();
        let expected = std::fs::read(directory.join(format!("initdata-{variant}.bin"))).unwrap();
        if let Some(offset) = got.iter().zip(&expected).position(|(a, b)| a != b) {
            panic!("initdata variant {variant} differs at {offset:#x}: C={:#x}, Rust={:#x}", expected[offset], got[offset]);
        }
        assert_eq!(got.len(), expected.len());
        bytes_checked += got.len();
    }
    println!("3 initdata fixtures / {bytes_checked} output bytes match qualified C exactly");
}
