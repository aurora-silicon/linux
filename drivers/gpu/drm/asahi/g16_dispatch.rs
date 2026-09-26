// SPDX-License-Identifier: GPL-2.0-only OR MIT


pub(crate) const VALUE: u32 = 0x44a72000;
pub(crate) const SENTINEL: u32 = 0xdeadbeef;

fn mov32(register: u64, value: u32) -> [u8; 8] {
    let v = value as u64;
    (0x2800c | (register << 4) | ((v & 0x7f) << 8)
        | (((v >> 7) & 0xf) << 33) | (((v >> 11) & 3) << 42)
        | (((v >> 13) & 0xfff) << 48) | (((v >> 25) & 0x7f) << 25)).to_le_bytes()
}

pub(crate) fn shader(output: u64) -> Result<[u8; 42], ()> {
    if output == 0 || output >= 1 << 42 || output & 3 != 0 { return Err(()); }
    let mut code = [0; 42];
    code[..8].copy_from_slice(&mov32(0, output as u32));
    code[8..16].copy_from_slice(&mov32(1, (output >> 32) as u32));
    code[16..24].copy_from_slice(&mov32(2, VALUE));
    code[24..].copy_from_slice(&[0xe7, 0, 0x54, 4, 0, 0, 0, 0,
        0x11, 1, 0, 0x90, 8, 0, 0x0e, 0, 0, 0]);
    Ok(code)
}

pub(crate) const ENTRY: usize = 0x3c0;
const PUBLISH: [u8; 10] = [3, 0, 7, 0, 2, 0, 0, 0, 0x60, 0];

pub(crate) fn image(output: u64) -> Result<[u8; 0x400], ()> {
    let body = shader(output)?;
    let mut out = [0; 0x400];
    out[..4].copy_from_slice(&0x340u32.to_le_bytes());
    for half in out[0x40..0x340].chunks_exact_mut(2) { half[0] = 6; }
    for base in [0x100, 0x200] {
        for index in 0..10 {
            let offset = base + index * 16;
            out[offset..offset + 10].fill(0);
            out[offset] = 0x0f;
            out[offset + 2] = 0x54;
            out[offset + 3] = ((10 - index) * 16) as u8;
        }
        for offset in [base + 0xa0, base + 0xb0] {
            // Default SETPROFILECTL followed by the helper return.
            out[offset..offset + 8].copy_from_slice(&[0xf7, 3, 0xaa, 0, 0x8f, 2, 0x54, 1]);
        }
    }
    // Header + constant program + aligned main body.
    out[0x340..0x344].copy_from_slice(&0xc0u32.to_le_bytes());
    out[0x380..0x38a].copy_from_slice(&PUBLISH);
    out[0x38a] = 0x0e;
    out[ENTRY..ENTRY + body.len()].copy_from_slice(&body);
    Ok(out)
}

pub(crate) fn esl(container: u64) -> Result<[u8; 90], ()> {
    if container == 0 || container >= (1 << 42) - 0x400 || container & 0x3fff != 0 {
        return Err(());
    }
    let shader = container + ENTRY as u64;
    let mut out = [0; 90];
    out[..8].copy_from_slice(&0x412a0077u64.to_le_bytes());
    out[8..16].copy_from_slice(&((shader << 17) | 0x2a0177).to_le_bytes());
    out[16..28].copy_from_slice(&[0, 0, 0xf7, 0, 0x2a, 0, 0, 0, 0, 0, 0, 0]);
    out[28..36].copy_from_slice(&mov32(1, 0));
    out[36..44].copy_from_slice(&[0x14, 0x81, 0x11, 6, 0, 0, 0, 0]);
    out[44..52].copy_from_slice(&mov32(0, (container + 0x100) as u32));
    out[52..62].copy_from_slice(&[0x9f, 0x11, 0x54, 0, 2, 0, 8, 0xa8, 0x10, 5]);
    out[62..70].copy_from_slice(&mov32(1, (container >> 32) as u32));
    out[70..76].copy_from_slice(&[0x0f, 0x12, 0x54, 0, 0x4c, 0]);
    out[76..86].copy_from_slice(&PUBLISH);
    out[86] = 0x0e;
    Ok(out)
}

pub(crate) fn cdm(esl: u64) -> Result<[u8; 48], ()> {
    if esl == 0 || esl >= 1 << 42 || esl & 63 != 0 { return Err(()); }
    let words = [1u32 << 19, ((esl >> 16) & 0xffc00000) as u32,
        (esl >> 6) as u32, 0x40000001, 1, 1, 1, 1, 1, 1, 0x60000160, 0x40000000];
    let mut out = [0; 48];
    for (word, bytes) in words.iter().zip(out.chunks_exact_mut(4)) {
        bytes.copy_from_slice(&word.to_le_bytes());
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn independently_decoded_m3_store_fixture() {
        assert_eq!(shader(0x10000090000).unwrap(), [
            0x0c,0x80,0x02,0,0,0,0x48,0, 0x1c,0x80,0x02,0,4,0,0,0,
            0x2c,0x80,0x02,0x44,0,0,0x39,5, 0xe7,0,0x54,4,0,0,0,0,
            0x11,1,0,0x90,8,0,0x0e,0,0,0]);
        assert!(shader(0).is_err());
        assert!(shader(1 << 42).is_err());
    }
    #[test]
    fn completed_m4_container_and_loader() {
        let encoded = esl(0x10000000000).unwrap();
        // Completed iadd: LDSHDR points at +3c0, not +380. The low address
        // bits overlap the old decoder's supposed additive 0xaa bias.
        assert_eq!(&encoded[8..28], &[0x77,1,0xaa,7,0,0,0,2,
            0,0,0xf7,0,0x2a,0,0,0,0,0,0,0]);
        let container = image(0x10000090000).unwrap();
        assert_eq!(&container[0x340..0x344], &[0xc0,0,0,0]);
        assert_eq!(&container[0x380..0x38a], &encoded[76..86]);
        assert_eq!(&container[0x1a0..0x1a8], &[0xf7,3,0xaa,0,0x8f,2,0x54,1]);
        assert_eq!(&container[ENTRY..ENTRY + 42], &shader(0x10000090000).unwrap());
        assert!(esl(65).is_err());
        assert!(cdm(63).is_err());
    }
}
