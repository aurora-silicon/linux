// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Sparse board-image templates: 16-byte lines over a zero image.
//!
//! Core-only (no `kernel` imports): this file is compiled by the host test harness as well.
//! The hash check of an expanded template is the kernel side's ([`super::expand`]).

/// One template line: 16 bytes at `offset`.
pub(crate) struct Line {
    pub(crate) offset: u32,
    pub(crate) data: [u8; 16],
}

/// A sparse template.
pub(crate) struct Template {
    pub(crate) size: usize,
    pub(crate) lines: &'static [Line],
    pub(crate) sha256: [u8; 32],
}

/// Fills the first `template.size` bytes of `out` with the template: zero, then its lines. `Err`
/// when `out` is too short or a line starts past the end.
pub(crate) fn fill(template: &Template, out: &mut [u8]) -> Result<(), ()> {
    let out = out.get_mut(..template.size).ok_or(())?;
    out.fill(0);
    for line in template.lines {
        let offset = line.offset as usize;
        if offset >= template.size {
            return Err(());
        }
        let n = 16.min(template.size - offset);
        out[offset..offset + n].copy_from_slice(&line.data[..n]);
    }
    Ok(())
}
