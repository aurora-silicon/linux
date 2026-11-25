// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounded, allocation-free reader of the boot loader's original Apple device tree.
//!
//! This module only decodes source data. Firmware-table selection belongs to the board
//! owner, so a familiar record count cannot silently select another generation's layout.

pub(crate) const MAX_STATES: usize = 32;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Error {
    Malformed,
    Missing,
    Duplicate,
    Identity,
    Shape,
    Range,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Source {
    pub(crate) count: usize,
    pub(crate) frequency_mhz: [u32; MAX_STATES],
    pub(crate) core_mv: [u32; MAX_STATES],
    pub(crate) sram_frequency_mhz: [u32; MAX_STATES],
    pub(crate) sram_mv: [u32; MAX_STATES],
    /// The sgx node's `gpu-pwr-sample-period-aic-clks`: exact GPU power sample period in ticks
    /// of the system counter, where the tree has it.
    pub(crate) power_sample_clks: Option<u32>,
    /// The sgx node's `gpu-power-sample-period`: coarse integer millisecond GPU power sample
    /// period, where the tree has it.
    pub(crate) power_sample_period_ms: Option<u32>,
    /// `/arm-io/pmgr/clpc` `cpu-avg-limiter-input-tc`: average-power filter time constant in
    /// milliseconds, where the tree has it.
    pub(crate) avg_power_filter_tc_ms: Option<u32>,
    /// `/arm-io/pmgr/clpc` `cpu-lowpeak-limiter-input-tc`: low-peak power limiter filter time
    /// constant in milliseconds, where the tree has it.
    pub(crate) lowpeak_limiter_tc_ms: Option<u32>,
    /// `/arm-io/pmgr/clpc` `cpu-lowpeak-limiter-ki`: low-peak power limiter integral gain in
    /// Q16.16 fixed point, where the tree has it.
    pub(crate) lowpeak_limiter_ki_q16: Option<u32>,
    /// `/arm-io/pmgr/clpc` `cpu-lowpeak-limiter-kp`: low-peak power limiter proportional gain
    /// in Q16.16 fixed point, where the tree has it.
    pub(crate) lowpeak_limiter_kp_q16: Option<u32>,
    /// `/arm-io/gfx-asc/iop-gfx-nub` `coredump-copy-buffer`: size in bytes of the host crash
    /// copy buffer, where the tree has it.
    pub(crate) coredump_copy_size: Option<u32>,
}

fn u32_at(data: &[u8], at: usize) -> Result<u32, Error> {
    let end = at.checked_add(4).ok_or(Error::Malformed)?;
    Ok(u32::from_le_bytes(
        data.get(at..end)
            .ok_or(Error::Malformed)?
            .try_into()
            .map_err(|_| Error::Malformed)?,
    ))
}

fn string_is(data: &[u8], expected: &[u8]) -> bool {
    data.strip_suffix(&[0]) == Some(expected)
}

struct Tree<'a> {
    data: &'a [u8],
}

impl<'a> Tree<'a> {
    fn header(&self, at: usize) -> Result<(u32, u32), Error> {
        let props = u32_at(self.data, at)?;
        let children = u32_at(self.data, at.checked_add(4).ok_or(Error::Malformed)?)?;
        if props > 4096 || children > 1024 {
            return Err(Error::Malformed);
        }
        Ok((props, children))
    }

    /// Bounds-check every property, including alignment padding, before its child traversal.
    fn property_end(&self, at: usize) -> Result<usize, Error> {
        let value = at.checked_add(36).ok_or(Error::Malformed)?;
        let length = (u32_at(self.data, at.checked_add(32).ok_or(Error::Malformed)?)? & 0x7fff_ffff)
            as usize;
        let padded = length.checked_add(3).ok_or(Error::Malformed)? & !3;
        let end = value.checked_add(padded).ok_or(Error::Malformed)?;
        self.data.get(at..end).ok_or(Error::Malformed)?;
        Ok(end)
    }

    fn children_start(&self, node: usize) -> Result<usize, Error> {
        let (props, _) = self.header(node)?;
        let mut at = node.checked_add(8).ok_or(Error::Malformed)?;
        for _ in 0..props {
            at = self.property_end(at)?;
        }
        Ok(at)
    }

    fn node_end(&self, node: usize, depth: usize, nodes: &mut usize) -> Result<usize, Error> {
        if depth > 32 || *nodes >= 16_384 {
            return Err(Error::Malformed);
        }
        *nodes += 1;
        let (_, children) = self.header(node)?;
        let mut at = self.children_start(node)?;
        for _ in 0..children {
            at = self.node_end(at, depth + 1, nodes)?;
        }
        Ok(at)
    }

    fn property(&self, node: usize, name: &[u8]) -> Result<&'a [u8], Error> {
        let (props, _) = self.header(node)?;
        let mut at = node.checked_add(8).ok_or(Error::Malformed)?;
        let mut found = None;
        for _ in 0..props {
            let end = self.property_end(at)?;
            let field = self.data.get(at..at + 32).ok_or(Error::Malformed)?;
            let n = field.iter().position(|&c| c == 0).unwrap_or(field.len());
            if &field[..n] == name {
                if found.is_some() {
                    return Err(Error::Duplicate);
                }
                let length = (u32_at(self.data, at + 32)? & 0x7fff_ffff) as usize;
                found = Some(&self.data[at + 36..at + 36 + length]);
            }
            at = end;
        }
        found.ok_or(Error::Missing)
    }

    fn child(&self, node: usize, name: &[u8]) -> Result<usize, Error> {
        let (_, children) = self.header(node)?;
        let mut at = self.children_start(node)?;
        let mut found = None;
        for _ in 0..children {
            if string_is(self.property(at, b"name")?, name) {
                if found.replace(at).is_some() {
                    return Err(Error::Duplicate);
                }
            }
            at = self.node_end(at, 0, &mut 0)?;
        }
        found.ok_or(Error::Missing)
    }

    fn cell(&self, node: usize, name: &[u8]) -> Result<u32, Error> {
        let data = self.property(node, name)?;
        if data.len() != 4 {
            return Err(Error::Shape);
        }
        u32_at(data, 0)
    }

    fn opt_nonzero_cell(&self, node: usize, name: &[u8]) -> Result<Option<u32>, Error> {
        match self.cell(node, name) {
            Ok(0) => Err(Error::Range),
            Ok(value) => Ok(Some(value)),
            Err(Error::Missing) => Ok(None),
            Err(error) => Err(error),
        }
    }
}

fn decode_record(data: &[u8], state: usize) -> Result<(u32, u32), Error> {
    let hz = u32_at(data, state * 8)?;
    let mv = u32_at(data, state * 8 + 4)?;
    if hz % 1_000_000 != 0
        || hz > 3_000_000_000
        || ((state == 0) != (hz == 0))
        || !(100..=1100).contains(&mv)
    {
        return Err(Error::Range);
    }
    Ok((hz / 1_000_000, mv))
}

/// Copying and physical reservation admission are the caller's responsibility. No reference to
/// `data` escapes. Padding after the root is permitted because phram exports whole pages.
pub(crate) fn parse(data: &[u8], sgx_compatible: &[u8]) -> Result<Source, Error> {
    if data.is_empty() || data.len() > 0x20_0000 {
        return Err(Error::Malformed);
    }
    let tree = Tree { data };
    tree.node_end(0, 0, &mut 0)?;
    let armio = tree.child(0, b"arm-io")?;
    let sgx = tree.child(armio, b"sgx")?;
    let compatible = tree.property(sgx, b"compatible")?;
    if compatible.last() != Some(&0)
        || !compatible
            .split(|&c| c == 0)
            .any(|value| value == sgx_compatible)
    {
        return Err(Error::Identity);
    }
    let count = tree.cell(sgx, b"perf-state-count")? as usize;
    if !(2..=MAX_STATES).contains(&count)
        || tree.cell(sgx, b"perf-state-table-count")? != 1
        || tree.cell(sgx, b"gpu-num-perf-states")? != count as u32 - 1
    {
        return Err(Error::Shape);
    }
    let core = tree.property(sgx, b"perf-states")?;
    let sram = tree.property(sgx, b"perf-states-sram")?;
    if core.len() != count * 8 || sram.len() != count * 8 {
        return Err(Error::Shape);
    }
    let power_sample_clks = tree.opt_nonzero_cell(sgx, b"gpu-pwr-sample-period-aic-clks")?;
    let power_sample_period_ms = tree.opt_nonzero_cell(sgx, b"gpu-power-sample-period")?;
    let clpc = match tree.child(armio, b"pmgr") {
        Ok(pmgr) => match tree.child(pmgr, b"clpc") {
            Ok(clpc) => Some(clpc),
            Err(Error::Missing) => None,
            Err(error) => return Err(error),
        },
        Err(Error::Missing) => None,
        Err(error) => return Err(error),
    };
    let (
        avg_power_filter_tc_ms,
        lowpeak_limiter_tc_ms,
        lowpeak_limiter_ki_q16,
        lowpeak_limiter_kp_q16,
    ) = if let Some(clpc) = clpc {
        (
            tree.opt_nonzero_cell(clpc, b"cpu-avg-limiter-input-tc")?,
            tree.opt_nonzero_cell(clpc, b"cpu-lowpeak-limiter-input-tc")?,
            tree.opt_nonzero_cell(clpc, b"cpu-lowpeak-limiter-ki")?,
            tree.opt_nonzero_cell(clpc, b"cpu-lowpeak-limiter-kp")?,
        )
    } else {
        (None, None, None, None)
    };
    let coredump_copy_size = match tree.child(armio, b"gfx-asc") {
        Ok(asc) => match tree.child(asc, b"iop-gfx-nub") {
            Ok(nub) => match tree.opt_nonzero_cell(nub, b"coredump-copy-buffer")? {
                Some(bytes) if bytes & 0xfff != 0 || bytes > 0x10_0000 => {
                    return Err(Error::Range);
                }
                other => other,
            },
            Err(Error::Missing) => None,
            Err(error) => return Err(error),
        },
        Err(Error::Missing) => None,
        Err(error) => return Err(error),
    };
    let mut source = Source {
        count,
        frequency_mhz: [0; MAX_STATES],
        core_mv: [0; MAX_STATES],
        sram_frequency_mhz: [0; MAX_STATES],
        sram_mv: [0; MAX_STATES],
        power_sample_clks,
        power_sample_period_ms,
        avg_power_filter_tc_ms,
        lowpeak_limiter_tc_ms,
        lowpeak_limiter_ki_q16,
        lowpeak_limiter_kp_q16,
        coredump_copy_size,
    };
    for i in 0..count {
        (source.frequency_mhz[i], source.core_mv[i]) = decode_record(core, i)?;
        (source.sram_frequency_mhz[i], source.sram_mv[i]) = decode_record(sram, i)?;
    }
    Ok(source)
}
