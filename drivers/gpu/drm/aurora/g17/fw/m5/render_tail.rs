// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M5 render descriptor tail encodings.

#[derive(Clone, Copy)]
pub(crate) enum RenderStage { Tiling, Fragment }

/// The common 26A428 interface inserts six bytes before the UMA pointer.
/// Register programs and completion stamps precede the insert and keep their
/// offsets. Move the entire trailing record, including the context token and
/// extra-address list, after encoding the original M5 layout.
pub(crate) fn common26_layout(out: &mut [u8], stage: RenderStage) -> Option<()> {
    let (start, end) = match stage {
        RenderStage::Tiling => (0x92e, 0x990),
        RenderStage::Fragment => (0x21c8, 0x2233),
    };
    out.get(..end + 6)?;
    out.copy_within(start..end, start + 6);
    out[start..start + 6].fill(0);
    Some(())
}

/// A context token packs the six-bit execution identity above its reuse generation.
/// The token in a render record agrees with the record header's context ID.
pub(crate) fn context_token(context: u32, generation: u64) -> Option<u64> {
    if context >= 64 || generation == 0 || generation >= (1u64 << 58) {
        return None;
    }
    Some((u64::from(context) << 58) | generation)
}

/// Merge the shifted usage hint into the TA tail's descriptor value.
///
/// The value is eight bytes. Its first byte belongs to the descriptor; the
/// shifted hint fits only the following seven bytes. The next byte starts the
/// independent resolved usage pointer, so accepting an eighth hint byte would
/// overwrite that pointer.
pub(crate) fn tiling_descriptor_value(descriptor: u64, shifted_hint: u64) -> Option<u64> {
    if shifted_hint > (u64::MAX >> 8) {
        return None;
    }
    Some((descriptor & 0xff) | (shifted_hint << 8))
}

pub(crate) struct RenderTail {

    pub(crate) descriptor_value: u64,

    pub(crate) resolved_value: Option<u64>,

    pub(crate) trailing_value: u64,
}

pub(crate) fn encode(out: &mut [u8], stage: RenderStage, value: &RenderTail) -> Option<()> {
    let base = match stage { RenderStage::Tiling => 0x937, RenderStage::Fragment => 0x21d1 };
    let tail = out.get_mut(base..base + 24)?;
    tail[..8].copy_from_slice(&value.descriptor_value.to_le_bytes());
    if let Some(resolved) = value.resolved_value {
        tail[8..16].copy_from_slice(&resolved.to_le_bytes());
    }
    tail[16..24].copy_from_slice(&value.trailing_value.to_le_bytes());
    Some(())
}

pub(crate) struct ExtraAddresses {
    pub(crate) count: u32,
    pub(crate) policy: u8,
    pub(crate) addresses: [u64; 4],
}

pub(crate) fn encode_extra(out: &mut [u8], stage: RenderStage, value: &ExtraAddresses) -> Option<()> {
    let (base, end) = match stage {
        RenderStage::Tiling => (0x96f, 0x990),
        RenderStage::Fragment => (0x220e, 0x2233),
    };
    let tail = out.get_mut(base..end)?;
    tail[32] &= 0xf2;
    if value.count != 0 || value.addresses.iter().any(|&address| address != 0) {
        tail[32] |= 1 | ((value.policy & 3) << 2);
        for (slot, address) in tail[..32].chunks_exact_mut(8).zip(value.addresses) {
            slot.copy_from_slice(&address.to_le_bytes());
        }
    }
    if let RenderStage::Fragment = stage { tail[33..37].fill(0); }
    Some(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn common26_render_tail_offsets() {
        for (stage, uma, token, extra, length) in [
            (RenderStage::Tiling, 0x934, 0x94d, 0x975, 0x9a0),
            (RenderStage::Fragment, 0x21ce, 0x21e7, 0x2214, 0x2240),
        ] {
            let mut bytes = [0u8; 0x2240];
            let out = &mut bytes[..length];
            let pointer = 0xfffffc2000123400u64;
            let identity = context_token(10, 1).unwrap();
            out[uma - 6..uma + 2].copy_from_slice(&pointer.to_le_bytes());
            out[uma - 7] = 0xa5;
            encode(out, stage, &RenderTail {
                descriptor_value: 0x1234,
                resolved_value: Some(pointer),
                trailing_value: identity,
            }).unwrap();
            encode_extra(out, stage, &ExtraAddresses {
                count: 1, policy: 2, addresses: [pointer; 4],
            }).unwrap();
            common26_layout(out, stage).unwrap();
            assert_eq!(&out[uma..uma + 8], &pointer.to_le_bytes());
            assert_eq!(&out[token..token + 8], &identity.to_le_bytes());
            assert_eq!(&out[extra..extra + 8], &pointer.to_le_bytes());
            assert_eq!(out[extra + 32], 9);
            assert_eq!(&out[uma - 6..uma], &[0; 6]);
            assert_eq!(out[uma - 7], 0xa5);
        }
    }

    #[test]
    fn common26_requires_room_for_extended_tiling_tail() {
        let mut bytes = [0xa5; 0x990];
        assert!(common26_layout(&mut bytes, RenderStage::Tiling).is_none());
        assert!(bytes.iter().all(|&byte| byte == 0xa5));
    }
}
