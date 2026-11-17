// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Byte map from the native processRenderSetup slice, not a complete descriptor.
//! Source: the native render passthrough map, version 3.
//! Unknown bytes return None; callers must keep them open in the ledger.
//! Gears uses the common body but its parser envelope needs separate admission.
#![allow(dead_code)]
// (descriptor offset, segment offset, extent, byte mask); mask 0 means zero.
const FIELDS: &[(usize, usize, usize, u8)] = &[
    (0x440, 0x71c, 4, 255),
    (0x4e0, 0x480, 8, 255),
    (0x4e8, 0x478, 8, 255),
    (0x4f0, 0x788, 8, 255),
    (0x4f8, 0x8b0, 24, 255),
    (0x510, 0x488, 8, 255),
    (0x518, 0x398, 128, 255),
    (0x598, 0x7f0, 128, 255),
    (0x618, 0x418, 48, 255),
    (0x648, 0x790, 48, 255),
    (0x678, 0x490, 40, 255),
    (0x6a0, 0x4d8, 8, 255),
    (0x6a8, 0x870, 8, 255),
    (0x6b0, 0x500, 8, 255),
    (0x6b8, 0x528, 8, 255),
    (0x6c0, 0x880, 8, 255),
    (0x6c8, 0x4b8, 8, 255),
    (0x6d0, 0x4e0, 8, 255),
    (0x6d8, 0x890, 8, 255),
    (0x6e0, 0x508, 8, 255),
    (0x6e8, 0x530, 8, 255),
    (0x6f0, 0x898, 8, 255),
    (0x6f8, 0x4c0, 8, 255),
    (0x700, 0x4e8, 8, 255),
    (0x708, 0x510, 8, 255),
    (0x710, 0x538, 8, 255),
    (0x718, 0x4c8, 8, 255),
    (0x720, 0x4f0, 8, 255),
    (0x728, 0x878, 8, 255),
    (0x730, 0x4d0, 8, 255),
    (0x738, 0x4f8, 8, 255),
    (0x740, 0x518, 8, 255),
    (0x748, 0x540, 8, 255),
    (0x750, 0x888, 8, 255),
    (0x758, 0x520, 8, 255),
    (0x760, 0x548, 8, 255),
    (0x768, 0x570, 8, 255),
    (0x770, 0x550, 32, 255),
    (0x790, 0x458, 32, 255),
    (0x7b0, 0x7d0, 16, 255),
    (0x7c0, 0x448, 12, 255),
    (0x7cc, 0x7c0, 12, 255),
    (0x7d8, 0x700, 8, 255),
    (0x7e8, 0x590, 8, 255),
    (0x7f8, 0x598, 4, 255),
    (0x818, 0x717, 1, 1),
    (0x830, 0x578, 8, 255),
    (0x838, 0x8d4, 1, 1),
    (0x8a0, 0x8dc, 1, 1),
    (0x8a4, 0x8d8, 4, 255),
    (0x8a8, 0x8e0, 8, 255),
    (0x8c0, 0x708, 4, 255),
    (0x8c4, 0x70d, 1, 1),
    (0x8c5, 0x8e8, 1, 1),
    (0x8c6, 0x8a0, 5, 1),
    (0x8cd, 0x710, 2, 1),
    (0x8d0, 0x713, 1, 1),
    (0x8d1, 0x716, 1, 1),
    (0x8d2, 0x8a5, 2, 1),
    (0x970, 0x8f8, 12, 255),
    (0x981, 0x8ea, 1, 255),
    (0x982, 0x714, 2, 1),
    (0xac8, 0x8d0, 4, 255),
    (0xaf0, 0x580, 4, 255),
    (0xaf8, 0x588, 8, 255),
    (0xb18, 0x738, 48, 255),
    (0xb58, 0x768, 28, 255),
    (0xfd8, 0x320, 4, 255),
    (0x1058, 0xc8, 48, 255),
    (0x1088, 0x2b8, 40, 255),
    (0x10b0, 0x2e8, 20, 255),
    (0x10c8, 0xf8, 112, 255),
    (0x1138, 0x180, 12, 255),
    (0x1158, 0x2ac, 12, 255),
    (0x1178, 0x30e, 1, 1),
    (0x1179, 0x0, 3, 0),
    (0x1180, 0x168, 8, 255),
    (0x11a1, 0x305, 3, 1),
    (0x11a5, 0x309, 1, 1),
    (0x11a6, 0x8e8, 1, 1),
    (0x11a7, 0x30a, 2, 1),
    (0x11aa, 0x311, 1, 1),
    (0x12d4, 0x30c, 2, 1),
    (0x1410, 0x300, 4, 255),
    (0x1550, 0x170, 4, 255),
    (0x1558, 0x178, 8, 255),
    (0x1590, 0x348, 48, 255),
    (0x15d0, 0x378, 28, 255),
    (0x15f1, 0x313, 1, 1),
    (0x1660, 0x312, 1, 1),
];

pub(crate) fn field(command: &[u8], descriptor: usize, width: usize) -> Option<u64> {
    if !matches!(command.len(), 0xac0 | 0xad8) || width == 0 || width > 8 { return None; }
    let mut value = 0u64;
    for byte in 0..width {
        let offset = descriptor.checked_add(byte)?;
        let &(dest, source, count, mask) = FIELDS.iter().find(|&&(dest, _, count, _)|
            offset >= dest && offset - dest < count)?;
        let _ = count;
        let v = if mask == 0 { 0 } else { *command.get(source + offset - dest)? & mask };
        value |= u64::from(v) << (byte * 8);
    }
    Some(value)
}

/// Only the empirically mapped userspace bytes. Unknown prepare-time fields
/// are counted, returned as zero for dry-run output, and MUST block admission.
pub(crate) fn capture_fields(command: &[u8]) -> (crate::g18::fw::render_command::G18RenderCommandInputs, usize) {
    use crate::g18::fw::render_command::*;
    fn read(c: &[u8], offset: usize, bytes: usize, open: &mut usize) -> u64 {
        match field(c, offset, bytes) { Some(v) => v, None => { *open += 1; 0 } }
    }
    let mut open = 0;
    let mut inputs = G18RenderCommandInputs::default();
    inputs.ta.d12d0 = read(command, 0x12d0, 4, &mut open) as u32;
    inputs.ta.d12c0 = read(command, 0x12c0, 4, &mut open) as u32;
    inputs.ta.d12c8 = read(command, 0x12c8, 8, &mut open) as u64;
    inputs.ta.d11b8 = read(command, 0x11b8, 8, &mut open) as u64;
    inputs.ta.d11d8 = read(command, 0x11d8, 8, &mut open) as u64;
    inputs.ta.d1068 = read(command, 0x1068, 8, &mut open) as u64;
    inputs.ta.d1070 = read(command, 0x1070, 8, &mut open) as u64;
    inputs.ta.d10f8 = read(command, 0x10f8, 8, &mut open) as u64;
    inputs.ta.d08a0 = read(command, 0x8a0, 1, &mut open) as u8;
    inputs.ta.d08a4 = read(command, 0x8a4, 4, &mut open) as u32;
    inputs.ta.d08a8 = read(command, 0x8a8, 4, &mut open) as u32;
    inputs.ta.d08ac = read(command, 0x8ac, 4, &mut open) as u32;
    inputs.ta.d08b0 = read(command, 0x8b0, 8, &mut open) as u64;
    inputs.ta.d1418 = core::array::from_fn(|i| read(command, 0x1418 + i * 8, 8, &mut open));
    inputs.ta.d1428 = read(command, 0x1428, 8, &mut open) as u64;
    inputs.ta.d1430 = read(command, 0x1430, 8, &mut open) as u64;
    inputs.ta.d1474 = read(command, 0x1474, 1, &mut open) as u8;
    inputs.ta.d11a4 = read(command, 0x11a4, 1, &mut open) as u8;
    inputs.ta.d11a7 = read(command, 0x11a7, 1, &mut open) as u8;
    inputs.fragment.d158 = read(command, 0x158, 8, &mut open) as u64;
    inputs.fragment.d150 = read(command, 0x150, 4, &mut open) as u32;
    inputs.fragment.d980_is_2 = read(command, 0x980, 4, &mut open) == 2;
    inputs.fragment.d08cf = read(command, 0x8cf, 1, &mut open) as u8;
    inputs.fragment.dbbc = read(command, 0xbbc, 1, &mut open) as u8;
    inputs.fragment.d08d8 = read(command, 0x8d8, 8, &mut open) as u64;
    inputs.fragment.d08f0 = read(command, 0x8f0, 8, &mut open) as u64;
    inputs.fragment.d08f8 = read(command, 0x8f8, 4, &mut open) as u32;
    inputs.fragment.d0900 = read(command, 0x900, 4, &mut open) as u32;
    inputs.fragment.d0908 = read(command, 0x908, 4, &mut open) as u32;
    inputs.fragment.d0910 = core::array::from_fn(|i| read(command, 0x910 + i * 8, 8, &mut open));
    inputs.fragment.d0920 = read(command, 0x920, 8, &mut open) as u64;
    inputs.fragment.d0930 = read(command, 0x930, 4, &mut open) as u32;
    inputs.fragment.d04e0 = read(command, 0x4e0, 8, &mut open) as u64;
    inputs.fragment.d04e8 = read(command, 0x4e8, 8, &mut open) as u64;
    inputs.fragment.d04f0 = read(command, 0x4f0, 8, &mut open) as u64;
    inputs.fragment.d0510 = read(command, 0x510, 8, &mut open) as u64;
    inputs.fragment.d0518 = core::array::from_fn(|i| core::array::from_fn(|j| read(command, 0x518 + i * 16 + j * 8, 8, &mut open)));
    inputs.fragment.d0690 = read(command, 0x690, 8, &mut open) as u64;
    inputs.fragment.d0698 = read(command, 0x698, 8, &mut open) as u64;
    inputs.fragment.d06a0 = core::array::from_fn(|i| read(command, 0x6a0 + i * 8, 8, &mut open));
    inputs.fragment.d06b0 = read(command, 0x6b0, 8, &mut open) as u64;
    inputs.fragment.d06b8 = core::array::from_fn(|i| read(command, 0x6b8 + i * 8, 8, &mut open));
    inputs.fragment.d06d8 = read(command, 0x6d8, 8, &mut open) as u64;
    inputs.fragment.d06f0 = read(command, 0x6f0, 8, &mut open) as u64;
    inputs.fragment.d06f8 = read(command, 0x6f8, 8, &mut open) as u64;
    inputs.fragment.d0708 = read(command, 0x708, 8, &mut open) as u64;
    inputs.fragment.d0728 = read(command, 0x728, 8, &mut open) as u64;
    inputs.fragment.d0730 = read(command, 0x730, 8, &mut open) as u64;
    inputs.fragment.d0750 = read(command, 0x750, 8, &mut open) as u64;
    inputs.fragment.d0758 = read(command, 0x758, 8, &mut open) as u64;
    inputs.fragment.d0770 = core::array::from_fn(|i| read(command, 0x770 + i * 8, 8, &mut open));
    inputs.fragment.d0780 = core::array::from_fn(|i| read(command, 0x780 + i * 8, 8, &mut open));
    inputs.fragment.d0790 = core::array::from_fn(|i| read(command, 0x790 + i * 8, 8, &mut open));
    inputs.fragment.d07b0 = core::array::from_fn(|i| read(command, 0x7b0 + i * 8, 8, &mut open));
    inputs.fragment.d07c0 = read(command, 0x7c0, 8, &mut open) as u64;
    inputs.fragment.d07c8 = read(command, 0x7c8, 4, &mut open) as u32;
    inputs.fragment.d07cc = read(command, 0x7cc, 8, &mut open) as u64;
    inputs.fragment.d07d4 = read(command, 0x7d4, 4, &mut open) as u32;
    inputs.fragment.d07d8 = read(command, 0x7d8, 8, &mut open) as u64;
    inputs.fragment.d07e8 = read(command, 0x7e8, 8, &mut open) as u64;
    inputs.fragment.d07f8 = read(command, 0x7f8, 4, &mut open) as u32;
    inputs.fragment.d04d0 = read(command, 0x4d0, 4, &mut open) as u32;
    inputs.fragment.d08c4 = read(command, 0x8c4, 1, &mut open) as u8;
    inputs.fragment.dc98 = read(command, 0xc98, 4, &mut open) as u32;
    inputs.fragment.dc9c = read(command, 0xc9c, 1, &mut open) as u8;
    inputs.fragment.d08a0 = read(command, 0x8a0, 1, &mut open) as u8;
    inputs.fragment.d08a4 = read(command, 0x8a4, 4, &mut open) as u32;
    inputs.fragment.d08a8 = read(command, 0x8a8, 4, &mut open) as u32;
    inputs.fragment.d08ac = read(command, 0x8ac, 4, &mut open) as u32;
    inputs.fragment.dad0 = core::array::from_fn(|i| read(command, 0xad0 + i * 8, 8, &mut open));
    inputs.fragment.dae0 = read(command, 0xae0, 8, &mut open) as u64;
    inputs.fragment.dae8 = read(command, 0xae8, 8, &mut open) as u64;
    inputs.fragment.d08b0 = core::array::from_fn(|i| read(command, 0x8b0 + i * 8, 8, &mut open));
    inputs.fragment.d08c5_flags = [0x8c5, 0x8c6, 0x8c7, 0x8cb].map(|o| read(command, o, 1, &mut open) as u8);
    inputs.fragment.d08cd = read(command, 0x8cd, 1, &mut open) as u8;
    (inputs, open)
}
