// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Render-target allocation geometry and M5 viewport state.

pub(crate) const PAGE_BYTES: usize = 0x4000;
pub(crate) const AUX_BYTES: usize = 0x8000;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct M5RenderTargetLayout {
    pub(crate) tilemap_bytes: usize,
    pub(crate) heapmeta_offset: usize,
    pub(crate) heapmeta_bytes: usize,
    pub(crate) target_bytes: usize,
    pub(crate) tpc_bytes: usize,
}

fn align(value: usize, alignment: usize) -> Option<usize> {
    Some(value.checked_add(alignment - 1)? & !(alignment - 1))
}

impl M5RenderTargetLayout {
    pub(crate) fn new(
        width: u32, height: u32, layers: u16, utile_width: u8, utile_height: u8,
        clusters: u32,
    ) -> Option<Self> {
        if !(1..=16384).contains(&width) || !(1..=16384).contains(&height)
            || !(1..=2048).contains(&layers) || clusters == 0
            || !matches!((utile_width, utile_height), (32, 32) | (32, 16) | (16, 16)) {
            return None;
        }
        // Same 4x4 macrotile geometry as the register builders. M5 packs its
        // boundaries in ten-bit fields; that does not change allocation size.
        let tiles_x = (width as usize + 31) / 32;
        let tiles_y = (height as usize + 31) / 32;
        let per_mtile = align((tiles_x + 3) / 4, 4)?
            .checked_mul(align((tiles_y + 3) / 4, 4)?)?;
        let utiles = (32 / utile_width as usize) * (32 / utile_height as usize);
        let tilemap_bytes = align(5usize.checked_mul(per_mtile)?.checked_mul(utiles)?, 4)?
            .checked_mul(16)?.checked_mul(layers as usize)?;
        let heapmeta_offset = align(tilemap_bytes, 0x1000)?;
        let heapmeta_bytes = if layers > 1 { 0x300 } else { 0x200 };
        let target_bytes = align(heapmeta_offset.checked_add(heapmeta_bytes)?, PAGE_BYTES)?;
        let tpc_bytes = align(8usize.checked_mul(per_mtile)?.checked_mul(utiles)?
            .checked_mul(16)?.checked_mul(layers as usize)?
            .checked_mul(clusters as usize)?, PAGE_BYTES)?;
        Some(Self { tilemap_bytes, heapmeta_offset, heapmeta_bytes, target_bytes, tpc_bytes })
    }
}

fn half_float_bits(value: u32) -> u32 {
    // The admitted integer dimensions are exactly representable in binary32.
    let highest = 31 - value.leading_zeros();
    ((highest + 126) << 23) | ((value << (23 - highest)) & 0x7fffff)
}

/// Native M5 deflake page: viewport at +0x9c0, command header 0x3000.
/// G17P uses +0x900/0xc00 and must not supply this page. The eight nonzero
/// dwords match the M5 25G76 resource capture; dimensions are caller-owned.
pub(crate) fn initialize_deflake(page: &mut [u8], width: u32, height: u32) -> Option<()> {
    if page.len() != PAGE_BYTES || !(1..=16384).contains(&width)
        || !(1..=16384).contains(&height) { return None; }
    page.fill(0);
    let x = half_float_bits(width);
    let y = half_float_bits(height);
    for (offset, value) in [(0x9c0, 0x3000),
        (0x9c4, 0x80000000 | ((width + 31) / 32 - 1)),
        (0x9c8, (height + 31) / 32 - 1), (0x9d0, x), (0x9d4, x),
        (0x9d8, y), (0x9dc, y | 0x80000000), (0x9e4, 0x3f800000)] {
        page[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
    Some(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reference_target_geometry() {
        let layout = M5RenderTargetLayout::new(256, 256, 1, 32, 32, 1).unwrap();
        assert_eq!(layout.tilemap_bytes, 0x500);
        assert_eq!(layout.heapmeta_offset, 0x1000);
        assert_eq!(layout.heapmeta_bytes, 0x200);
        assert_eq!(layout.target_bytes, PAGE_BYTES);
        assert_eq!(layout.tpc_bytes, PAGE_BYTES);
    }

    #[test]
    fn larger_layered_targets_increase_capacity() {
        let a = M5RenderTargetLayout::new(4096, 2048, 1, 32, 32, 1).unwrap();
        let b = M5RenderTargetLayout::new(4096, 2048, 2, 16, 16, 2).unwrap();
        assert_eq!(b.tilemap_bytes, a.tilemap_bytes * 8);
        assert_eq!(b.tpc_bytes, a.tpc_bytes * 16);
        assert!(b.heapmeta_offset >= b.tilemap_bytes);
        assert!(b.target_bytes >= b.heapmeta_offset + b.heapmeta_bytes);
    }

    #[test]
    fn invalid_dimensions_leave_output_untouched() {
        let mut page = [0xa5; PAGE_BYTES];
        assert_eq!(initialize_deflake(&mut page, 0, 256), None);
        assert!(page.iter().all(|b| *b == 0xa5));
        assert!(M5RenderTargetLayout::new(16385, 1, 1, 32, 32, 1).is_none());
        assert!(M5RenderTargetLayout::new(1, 1, 0, 32, 32, 1).is_none());
        assert!(M5RenderTargetLayout::new(1, 1, 1, 8, 32, 1).is_none());
        assert!(M5RenderTargetLayout::new(1, 1, 1, 32, 32, 0).is_none());
    }

    #[test]
    fn m5_viewport_is_distinct_and_dimension_derived() {
        for (width, height) in [(256, 256), (1920, 1080), (1, 16384)] {
            let mut page = [0xa5; PAGE_BYTES];
            initialize_deflake(&mut page, width, height).unwrap();
            let word = |off| u32::from_le_bytes(page[off..off + 4].try_into().unwrap());
            assert_eq!(word(0x900), 0);
            assert_eq!(word(0x9c0), 0x3000);
            assert_eq!(word(0x9d0), (width as f32 / 2.0).to_bits());
            assert_eq!(word(0x9d8), (height as f32 / 2.0).to_bits());
            assert_eq!(word(0x9dc), (-(height as f32 / 2.0)).to_bits());
        }
    }
}
