// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Optional T6030 Globals debug-word control; preserve every other byte.

pub(crate) const OFFSET: usize = 0x2c;

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Error { Size, Value }

pub(crate) fn apply(chip:u32, off:bool, globals:&mut[u8])->Result<bool,Error> {
    if chip!=0x6030 || !off {return Ok(false);}
    if globals.len()<0xe00 {return Err(Error::Size);}
    let bytes=&globals[OFFSET..OFFSET+4];
    let value=u32::from_le_bytes([bytes[0],bytes[1],bytes[2],bytes[3]]);
    if value>1 {return Err(Error::Value);}
    globals[OFFSET..OFFSET+4].fill(0);
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_debug_word_changes_in_full_owner() {
        let mut bytes=[0x5a;15868];
        bytes[OFFSET..OFFSET+4].copy_from_slice(&1u32.to_le_bytes());
        let before=bytes;
        assert_eq!(apply(0x6030,true,&mut bytes),Ok(true));
        for i in 0..bytes.len() {
            assert_eq!(bytes[i],if (OFFSET..OFFSET+4).contains(&i) {0} else {before[i]});
        }
        assert_eq!(apply(0x6030,true,&mut bytes),Ok(true));
    }
    #[test]
    fn disabled_and_other_chips_keep_original_bytes() {
        for (chip,off) in [(0x6030,false),(0x8103,true),(0x8112,true),(0x8122,true),(0x6031,true)] {
            let mut bytes=[0x5a;15868];let before=bytes;
            assert_eq!(apply(chip,off,&mut bytes),Ok(false));assert_eq!(bytes,before);
        }
    }
    #[test]
    fn invalid_inputs_fail_before_mutation() {
        let mut bytes=[0x5a;15868];let before=bytes;
        assert_eq!(apply(0x6030,true,&mut bytes),Err(Error::Value));assert_eq!(bytes,before);
        assert_eq!(apply(0x6030,true,&mut bytes[..0xdff]),Err(Error::Size));assert_eq!(bytes,before);
    }
}
