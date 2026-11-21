// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(dead_code)]
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/fw/initdata.rs"]
mod initdata;

fn input(variant: u64) -> initdata::Input {
    let start = 0xfffffc2000000000u64;
    let shift = variant * 0x100000000;
    let mut input = initdata::Input {
        // J713 frequencies with synthetic voltage bins.
        performance: initdata::Performance {
            count: 10,
            frequency: [0,338,618,796,928,1056,1170,1278,1338,1470,0],
            auxiliary: [0,338,618,796,836,952,1053,1152,1204,1326,0],
            voltage: [100,600,650,700,750,800,850,900,950,1000,0],
            sram: [800,800,800,800,800,800,850,900,950,1000,0],
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
        let mut expected =
            std::fs::read(directory.join(format!("initdata-{variant}.bin"))).unwrap();
        // Substitute synthetic voltages in the immutable C golden before comparison.
        let main: usize = initdata::OBJECT_SIZE[..initdata::id::MAIN].iter().sum();
        let perf = input(variant as u64).performance;
        for i in 0..=ceiling as usize { for lane in 0..16 {
            for (offset, value) in [(0x1008, perf.voltage[i]), (0x1408, perf.sram[i])] {
                let at = main + offset + (i * 16 + lane) * 4;
                expected[at..at+4].copy_from_slice(&value.to_le_bytes());
            }
        }}
        if let Some(offset) = got.iter().zip(&expected).position(|(a, b)| a != b) {
            panic!("initdata variant {variant} differs at {offset:#x}: C={:#x}, Rust={:#x}", expected[offset], got[offset]);
        }
        assert_eq!(got.len(), expected.len());
        bytes_checked += got.len();
    }
    println!("3 initdata fixtures / {bytes_checked} bytes match C with synthetic voltages");
}
