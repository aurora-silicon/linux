// SPDX-License-Identifier: GPL-2.0-only


#![cfg_attr(not(test), allow(dead_code))]

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum BuildError {
    Geometry,
    Address,
    Register,
    BufferTooSmall,
    ArithmeticOverflow,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum Utile {
    Pixels16,
    Pixels32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct Geometry {
    pub(crate) layers: u16,
    pub(crate) pixels: u32,
    pub(crate) screen: u32,
    pub(crate) x_blocks: u32,
    pub(crate) y_blocks: u32,
    pub(crate) blocks: u32,
    pub(crate) tiles: u32,
    pub(crate) region_stride: u32,
    pub(crate) tpc_stride: u32,
    pub(crate) isp_blocks: u32,
    pub(crate) utile_config: u64,
    pub(crate) tilemap_bytes: u64,
    pub(crate) tpc_bytes: u64,
}

impl Geometry {
    pub(crate) fn parameter_bytes(&self) -> usize {
        let blocks = ((self.tiles as usize).div_ceil(128) + 7) & !7;
        (blocks * 128 * 1024).max(4 * 1024 * 1024)
    }

    pub(crate) fn set_samples(&mut self, samples: u8) -> Result<(), BuildError> {
        let log2 = match samples { 1 => 0, 2 => 1, 4 => 2,
            _ => return Err(BuildError::Geometry) };
        self.utile_config = (self.utile_config & !3) | log2;
        Ok(())
    }

    pub(crate) fn new(
        width: u32,
        height: u32,
        x: Utile,
        y: Utile,
        accelerator_count: u32,
    ) -> Result<Self, BuildError> {
        Self::new_layered(width, height, x, y, accelerator_count, 1)
    }

    pub(crate) fn new_layered(
        width: u32, height: u32, x: Utile, y: Utile,
        accelerator_count: u32, layers: u16,
    ) -> Result<Self, BuildError> {
        if layers == 0 || layers > 2048 {
            return Err(BuildError::Geometry);
        }
        if width == 0 || height == 0 || width > 16384 || height > 16384 || accelerator_count == 0 {
            return Err(BuildError::Geometry);
        }
        let tx = (width + 31) >> 5;
        let ty = (height + 31) >> 5;
        let bx = (((tx + 3) >> 2) + 3) & !3;
        let by = (((ty + 3) >> 2) + 3) & !3;
        let sx = bx << u32::from(x == Utile::Pixels16);
        let sy = by << u32::from(y == Utile::Pixels16);
        let entries = (sx * 4) * (sy * 4);
        let tpc_bytes = u64::from(accelerator_count)
            .checked_mul(u64::from(entries))
            .and_then(|n| n.checked_mul(8))
            .and_then(|n| n.checked_mul(u64::from(layers)))
            .ok_or(BuildError::ArithmeticOverflow)?;
        Ok(Self {
            layers,
            pixels: (width - 1) | ((height - 1) << 16),
            screen: (tx - 1) | ((ty - 1) << 12),
            x_blocks: (bx << 10) | (bx << 18) | (bx * 3),
            y_blocks: (by << 18) | (by << 10) | (by * 3),
            blocks: bx * by,
            tiles: tx * ty,
            region_stride: (entries * 5) >> 6,
            tpc_stride: entries >> 3,
            isp_blocks: ((sx & 0x3fd) << 16) | (sy & 0x3fd),
            utile_config: (if x == Utile::Pixels32 { 0x2000 } else { 0x1000 })
                | (if y == Utile::Pixels32 { 0x8000 } else { 0x4000 }),
            tilemap_bytes: (u64::from(entries) * 5 * u64::from(layers) + 0xfff) & !0xfff,
            tpc_bytes,
        })
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ParameterBufferDescriptor(pub(crate) [u32; 4]);

impl ParameterBufferDescriptor {
    pub(crate) fn update(
        &mut self,
        page_list: u64,
        pages: u32,
        read: u32,
        write: u32,
        wrapped: bool,
        valid: bool,
    ) -> Result<(), BuildError> {
        if page_list == 0
            || page_list & 127 != 0
            || page_list >= 1 << 43
            || pages == 0
            || pages > 0x3fffff
            || read > 0x3fffff
            || write > 0x3fffff
        {
            return Err(BuildError::Address);
        }
        let bias = if page_list & (1 << 42) != 0 {
            0
        } else {
            0x7000000000
        };
        let high = (((page_list + bias) >> 8) & 0x70000000) | ((page_list >> 11) & 0x80000000);
        self.0 = [
            ((page_list >> 4) as u32 & 0xfffffff8) | (self.0[0] & 6) | u32::from(valid),
            high as u32 | (self.0[1] & 0x0fc00000) | pages,
            (self.0[2] & 0xffc00000) | read,
            (self.0[3] & 0x7fc00000) | write | (u32::from(wrapped) << 31),
        ];
        Ok(())
    }

    pub(crate) fn to_bytes(self) -> [u8; 16] {
        let mut out = [0; 16];
        for (word, dst) in self.0.iter().zip(out.chunks_exact_mut(4)) {
            dst.copy_from_slice(&word.to_le_bytes());
        }
        out
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum Stage {
    Tiling,
    Fragment,
}

impl Stage {
    pub(crate) const fn command_size(self) -> usize {
        match self {
            Self::Tiling => 0x940,
            Self::Fragment => 0xca0,
        }
    }
    const fn register_offset(self) -> usize {
        match self {
            Self::Tiling => 0x40,
            Self::Fragment => 0x80,
        }
    }
    const fn suffix_entries(self) -> usize {
        match self {
            Self::Tiling => 3,
            Self::Fragment => 2,
        }
    }
}

#[derive(Clone, Copy, Default)]
pub(crate) struct Register {
    pub(crate) offset: u32,
    pub(crate) flagged: bool,
    pub(crate) value: u64,
}

pub(crate) fn write_registers(
    command: &mut [u8],
    stage: Stage,
    address: u64,
    registers: &[Register],
) -> Result<(), BuildError> {
    const ENTRY_BYTES: usize = 12;
    const APERTURE_OFFSET: usize = 0x600;
    const HEADER_OFFSET: usize = 0x700;
    let base = stage.register_offset();
    if command.len() < stage.command_size() {
        return Err(BuildError::BufferTooSmall);
    }
    if address == 0 || address & 7 != 0 || address > (1u64 << 42) - stage.command_size() as u64 {
        return Err(BuildError::Address);
    }
    if registers.is_empty()
        || registers.len() > APERTURE_OFFSET / ENTRY_BYTES - stage.suffix_entries()
    {
        return Err(BuildError::BufferTooSmall);
    }
    if registers.iter().any(|r| r.offset & !0x3fff8 != 0) {
        return Err(BuildError::Register);
    }
    for (i, register) in registers.iter().enumerate() {
        let start = base + i * ENTRY_BYTES;
        let word = register.offset | u32::from(register.flagged);
        command[start..start + 4].copy_from_slice(&word.to_le_bytes());
        command[start + 4..start + 12].copy_from_slice(&register.value.to_le_bytes());
    }
    let end = base + registers.len() * ENTRY_BYTES;
    command[end..end + stage.suffix_entries() * ENTRY_BYTES].fill(0);
    let header = base + HEADER_OFFSET;
    command[header..header + 8].copy_from_slice(&(address + base as u64).to_le_bytes());
    command[header + 8..header + 10].copy_from_slice(&(registers.len() as u16).to_le_bytes());
    command[header + 10..header + 12]
        .copy_from_slice(&((registers.len() * ENTRY_BYTES) as u16).to_le_bytes());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parameter_heap_is_recyclable_across_layers() {
        for layers in [1, 32, 2048] {
            let g = Geometry::new_layered(16384, 16384,
                Utile::Pixels32, Utile::Pixels32, 1, layers).unwrap();
            assert_eq!(g.parameter_bytes(), 256 * 1024 * 1024);
            assert_eq!(g.tilemap_bytes, 1280 * 1024 * u64::from(layers));
            assert_eq!(g.tpc_bytes, 2 * 1024 * 1024 * u64::from(layers));
        }
        let four_k = Geometry::new(3840, 2160,
            Utile::Pixels32, Utile::Pixels32, 1).unwrap();
        assert_eq!(four_k.parameter_bytes(), 8 * 1024 * 1024);
        let tiny = Geometry::new(1, 1,
            Utile::Pixels16, Utile::Pixels16, 1).unwrap();
        assert_eq!(tiny.parameter_bytes(), 4 * 1024 * 1024);
    }

    #[test]
    fn render_geometry_matches_parameter_storage() {
        let small = Geometry::new(4, 4, Utile::Pixels32, Utile::Pixels32, 1).unwrap();
        assert_eq!((small.pixels, small.screen, small.tiles), (0x30003, 0, 1));
        assert_eq!((small.x_blocks, small.y_blocks), (0x10100c, 0x10100c));
        assert_eq!((small.tilemap_bytes, small.tpc_bytes), (4096, 2048));
        let panel = Geometry::new(2560, 1664, Utile::Pixels32, Utile::Pixels32, 1).unwrap();
        assert_eq!((panel.tiles, panel.blocks), (4160, 320));
        assert_eq!((panel.region_stride, panel.tpc_stride), (400, 640));
        assert_eq!((panel.tilemap_bytes, panel.tpc_bytes), (28672, 40960));
        assert_eq!(
            Geometry::new(0, 4, Utile::Pixels32, Utile::Pixels32, 1),
            Err(BuildError::Geometry)
        );
    }

    #[test]
    fn layered_geometry_scales_allocations_not_per_layer_strides() {
        for (w,h) in [(8,8),(257,139),(16384,16384)] {
            for (x,y) in [(Utile::Pixels32,Utile::Pixels32),
                          (Utile::Pixels32,Utile::Pixels16),
                          (Utile::Pixels16,Utile::Pixels16)] {
                let one=Geometry::new(w,h,x,y,1).unwrap();
                for n in [1u16,2,4,2048] {
                    let many=Geometry::new_layered(w,h,x,y,1,n).unwrap();
                    assert_eq!(many.tilemap_bytes,
                        (u64::from(one.region_stride)*64*u64::from(n)+0xfff)&!0xfff);
                    assert_eq!(many.tpc_bytes,one.tpc_bytes*u64::from(n));
                    assert_eq!(many.region_stride,one.region_stride);
                    assert_eq!(many.tpc_stride,one.tpc_stride);
                    assert_eq!(many.tiles,one.tiles);
                    assert_eq!(many.layers,n);
                }
            }
        }
        for n in [0,2049] {
            assert_eq!(Geometry::new_layered(8,8,Utile::Pixels32,Utile::Pixels32,1,n),
                       Err(BuildError::Geometry));
        }
    }

    #[test]
    fn parameter_buffer_root_selector_and_reserved_bits() {
        let mut pb = ParameterBufferDescriptor([0; 4]);
        pb.update(0x10000a000, 128, 0, 127, false, true).unwrap();
        assert_eq!(pb.0, [0x10000a01, 0x70000080, 0, 127]);
        pb.update(0x4300000a000, 128, 0, 0, false, false).unwrap();
        assert_eq!(pb.0, [0xa00, 0xb0000080, 0, 0]);
        let saved = pb;
        assert_eq!(
            pb.update(0x10000a001, 128, 0, 0, false, false),
            Err(BuildError::Address)
        );
        assert_eq!(pb, saved);
        let mut pb = ParameterBufferDescriptor([6, 0x0fc00000, 0xffc00000, 0x7fc00000]);
        pb.update(0x10000a000, 0x3fffff, 0x3fffff, 0x3fffff, true, true)
            .unwrap();
        assert_eq!(pb.0, [0x10000a07, 0x7fffffff, 0xffffffff, 0xffffffff]);
        assert_eq!(&pb.to_bytes()[..4], &[7, 10, 0, 16]);
    }

    #[test]
    fn register_layout_preserves_apertures_and_rejects_before_writing() {
        for stage in [Stage::Tiling, Stage::Fragment] {
            let mut command = [0xa5; 0xca0];
            let registers = [Register {
                offset: 0x1738,
                flagged: true,
                value: 1,
            }];
            write_registers(&mut command, stage, 0x3000000000, &registers).unwrap();
            let base = stage.register_offset();
            assert_eq!(
                &command[base..base + 12],
                &[0x39, 0x17, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]
            );
            assert!(command[base + 12..base + 12 * (1 + stage.suffix_entries())]
                .iter()
                .all(|b| *b == 0));
            assert!(command[base + 0x600..base + 0x700]
                .iter()
                .all(|b| *b == 0xa5));
            assert_eq!(
                &command[base + 0x700..base + 0x708],
                &(0x3000000000u64 + base as u64).to_le_bytes()
            );
            let before = command;
            let invalid = [Register {
                offset: 3,
                flagged: false,
                value: 0,
            }];
            assert_eq!(
                write_registers(&mut command, stage, 0x3000000000, &invalid),
                Err(BuildError::Register)
            );
            assert_eq!(command, before);
            assert_eq!(
                write_registers(
                    &mut command[..stage.command_size() - 1],
                    stage,
                    0x3000000000,
                    &registers
                ),
                Err(BuildError::BufferTooSmall)
            );
            assert_eq!(command, before);
        }
    }
}
