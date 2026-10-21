// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The GPU's current performance state, as the firmware reports it.
//!
//! Two places carry it:
//!
//! - The stats ring: 256 records of 0x40 bytes, a u32 tag at +0 and, in a power-state record
//!   (tag 0x0b), the performance-state index at +0x20.
//! - HardwareDataA (hardware record +0x4580): actual (+0x2c), target (+0x30) and current
//!   (+0x38) state indices and the frequency in MHz as an IEEE single (+0x58).
//!
//! An index counts the firmware's DVFS states, 0 (off) to 8; its frequency is the state's
//! table-A operating point ([`super::super::board`]), taken from the device tree.
//!
//! [`write`] prints the `asahi-g15/pstate` file. Its first line is what a sampler reads:
//! `pstate <index> freq_khz <kHz> source <stats|hwdata>`, from the newest power-state record
//! seen, or HardwareDataA's current state when the stats ring has carried none.
//!
//! Core-only (no `kernel` imports), so that it can be tested on the host.

use core::fmt;

/// DVFS states of the firmware tables (= `board::DVFS_STATES`).
pub(crate) const STATES: usize = 9;

/// HardwareDataA words.
pub(crate) const HWDATA_A_ACTUAL: usize = 0x2c;
pub(crate) const HWDATA_A_TARGET: usize = 0x30;
pub(crate) const HWDATA_A_CURRENT: usize = 0x38;
pub(crate) const HWDATA_A_FREQ_MHZ: usize = 0x58;

/// Stats ring records.
pub(crate) const STATS_RECORD: usize = 0x40;
pub(crate) const STATS_SLOTS: u32 = 256;
pub(crate) const STATS_TAG: usize = 0x00;
pub(crate) const TAG_POWER_STATE: u32 = 0x0b;
pub(crate) const POWER_STATE_INDEX: usize = 0x20;

/// Tags counted one by one; higher tags are counted together.
pub(crate) const TAGS: usize = 16;

/// What the stats ring has carried.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct StatsSeen {
    /// Records consumed.
    pub(crate) records: u64,
    /// Records per tag; the last slot counts every tag from `TAGS - 1` up.
    pub(crate) tags: [u64; TAGS],
    /// The index of the newest power-state record.
    pub(crate) last: Option<u32>,
}

impl StatsSeen {
    /// Notes one record: its tag and the word at [`POWER_STATE_INDEX`].
    pub(crate) fn note(&mut self, tag: u32, index: u32) {
        self.records = self.records.wrapping_add(1);
        let slot = (tag as usize).min(TAGS - 1);
        self.tags[slot] = self.tags[slot].wrapping_add(1);
        if tag == TAG_POWER_STATE {
            self.last = Some(index);
        }
    }
}

/// The ring slots from the consumer `read` to the producer `write`, oldest first; `None` when
/// either index is out of the ring.
pub(crate) fn pending(read: u32, write: u32) -> Option<impl Iterator<Item = u32>> {
    if read >= STATS_SLOTS || write >= STATS_SLOTS {
        return None;
    }
    let count = (write + STATS_SLOTS - read) % STATS_SLOTS;
    Some((0..count).map(move |i| (read + i) % STATS_SLOTS))
}

/// The HardwareDataA words.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct HwWords {
    pub(crate) actual: u32,
    pub(crate) target: u32,
    pub(crate) current: u32,
    /// IEEE single bits, MHz.
    pub(crate) freq_mhz: u32,
}

/// The table-A frequency of each firmware state, kHz.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct Table {
    pub(crate) khz: [u32; STATES],
}

impl Table {
    /// From the table-A operating points in MHz.
    pub(crate) fn from_mhz(mhz: [u32; STATES]) -> Self {
        Self {
            khz: mhz.map(|m| m.saturating_mul(1000)),
        }
    }

    /// The frequency of state `index`, kHz; `None` past the table.
    pub(crate) fn khz(&self, index: u32) -> Option<u32> {
        self.khz.get(index as usize).copied()
    }
}

/// An IEEE single number of MHz in kHz, rounded to nearest; `None` for a negative number, an
/// infinity, a NaN or a value past `u32::MAX` kHz.
pub(crate) fn f32_mhz_to_khz(bits: u32) -> Option<u32> {
    let exp = (bits >> 23) & 0xff;
    let frac = bits & 0x7f_ffff;
    if bits & 0x7fff_ffff == 0 {
        return Some(0);
    }
    if bits >> 31 != 0 || exp == 0xff {
        return None;
    }
    // value = m * 2^(e - 150); a subnormal has no implicit bit and e = 1.
    let (m, e) = if exp == 0 {
        (frac as u64, 1i32)
    } else {
        ((frac | 0x80_0000) as u64, exp as i32)
    };
    let scaled = m * 1000; // < 2^34
    let shift = e - 150;
    let khz = if shift >= 0 {
        if shift > 31 {
            return None;
        }
        scaled << shift
    } else {
        let s = (-shift) as u32;
        if s >= 64 {
            0
        } else {
            let q = scaled >> s;
            let rem = scaled & ((1u64 << s) - 1);
            let half = 1u64 << (s - 1);
            // Ties to even.
            if rem > half || (rem == half && q & 1 == 1) {
                q + 1
            } else {
                q
            }
        }
    };
    u32::try_from(khz).ok()
}

/// Where the reported state comes from.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Source {
    Stats,
    HwData,
}

impl Source {
    fn name(self) -> &'static str {
        match self {
            Source::Stats => "stats",
            Source::HwData => "hwdata",
        }
    }
}

/// The reported state: the newest power-state record, else HardwareDataA's current state.
pub(crate) fn current(seen: &StatsSeen, hw: &HwWords) -> (u32, Source) {
    match seen.last {
        Some(index) => (index, Source::Stats),
        None => (hw.current, Source::HwData),
    }
}

/// A frequency for the file: kHz, or `unknown`.
struct Khz(Option<u32>);

impl fmt::Display for Khz {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            Some(khz) => write!(f, "{}", khz),
            None => f.write_str("unknown"),
        }
    }
}

/// The `asahi-g15/pstate` file:
///
/// ```text
/// pstate <index> freq_khz <kHz|unknown> source <stats|hwdata>
/// stats: records <n> power_states <n> last <index|none> tags <tag>:<n> ...
/// hwdata: actual <i> target <i> current <i> freq_mhz_f32 <bits> freq_khz <kHz|unknown>
/// table_khz: <kHz> x 9
/// ```
pub(crate) fn write(
    f: &mut impl fmt::Write,
    seen: &StatsSeen,
    hw: &HwWords,
    table: &Table,
) -> fmt::Result {
    let (index, source) = current(seen, hw);
    writeln!(
        f,
        "pstate {} freq_khz {} source {}",
        index,
        Khz(table.khz(index)),
        source.name()
    )?;
    write!(
        f,
        "stats: records {} power_states {} last ",
        seen.records, seen.tags[TAG_POWER_STATE as usize]
    )?;
    match seen.last {
        Some(last) => write!(f, "{}", last)?,
        None => f.write_str("none")?,
    }
    f.write_str(" tags")?;
    for (tag, n) in seen.tags.iter().enumerate() {
        if *n != 0 {
            write!(f, " {}:{}", tag, n)?;
        }
    }
    writeln!(f)?;
    writeln!(
        f,
        "hwdata: actual {} target {} current {} freq_mhz_f32 {:#010x} freq_khz {}",
        hw.actual,
        hw.target,
        hw.current,
        hw.freq_mhz,
        Khz(f32_mhz_to_khz(hw.freq_mhz))
    )?;
    f.write_str("table_khz:")?;
    for khz in table.khz {
        write!(f, " {}", khz)?;
    }
    writeln!(f)
}

#[cfg(test)]
mod tests {
    use super::*;

    // A made-up table, not any machine's.
    fn table() -> Table {
        Table::from_mhz([0, 100, 200, 300, 400, 500, 600, 700, 800])
    }

    #[test]
    fn table_is_khz_and_bounded() {
        let t = table();
        assert_eq!(t.khz(0), Some(0));
        assert_eq!(t.khz(8), Some(800_000));
        assert_eq!(t.khz(9), None);
        assert_eq!(t.khz(u32::MAX), None);
    }

    #[test]
    fn notes_tags_and_the_newest_power_state() {
        let mut s = StatsSeen::default();
        assert_eq!(s.last, None);
        s.note(4, 7);
        assert_eq!(s.last, None);
        s.note(TAG_POWER_STATE, 3);
        s.note(TAG_POWER_STATE, 8);
        s.note(9, 1);
        s.note(200, 0);
        assert_eq!(s.records, 5);
        assert_eq!(s.last, Some(8));
        assert_eq!(s.tags[4], 1);
        assert_eq!(s.tags[TAG_POWER_STATE as usize], 2);
        assert_eq!(s.tags[9], 1);
        assert_eq!(s.tags[TAGS - 1], 1);
    }

    #[test]
    fn pending_walks_from_read_to_write_with_wrap() {
        let v: Vec<u32> = pending(3, 3).unwrap().collect();
        assert!(v.is_empty());
        let v: Vec<u32> = pending(3, 6).unwrap().collect();
        assert_eq!(v, [3, 4, 5]);
        let v: Vec<u32> = pending(254, 1).unwrap().collect();
        assert_eq!(v, [254, 255, 0]);
        assert!(pending(256, 0).is_none());
        assert!(pending(0, 256).is_none());
    }

    #[test]
    fn f32_mhz_in_khz() {
        let k = |x: f32| f32_mhz_to_khz(x.to_bits());
        assert_eq!(k(0.0), Some(0));
        assert_eq!(f32_mhz_to_khz(0x8000_0000), Some(0)); // -0
        assert_eq!(k(1.0), Some(1000));
        assert_eq!(k(800.0), Some(800_000));
        assert_eq!(k(1234.5), Some(1_234_500));
        assert_eq!(k(0.0015), Some(2)); // 1.5 kHz, nearest of the single
        assert_eq!(k(0.0004), Some(0));
        assert_eq!(k(4_000_000.0), Some(4_000_000_000));
        assert_eq!(k(5_000_000.0), None); // past u32 kHz
        assert_eq!(k(-1.0), None);
        assert_eq!(k(f32::INFINITY), None);
        assert_eq!(k(f32::NAN), None);
        assert_eq!(f32_mhz_to_khz(1), Some(0)); // smallest subnormal
                                                // Exact halves (an odd mantissa x 2^-4 MHz is n + 0.5 kHz) round to even.
        for m in [0x80_0001u32, 0x80_0003, 0x80_0005, 0xff_ffff] {
            let bits = (146 << 23) | (m & 0x7f_ffff);
            let want = (m as f64 * 62.5).round_ties_even() as u32;
            assert_eq!(f32_mhz_to_khz(bits), Some(want), "{m:#x}");
        }
        // Against the host's arithmetic over a sweep.
        for i in 0..20_000u32 {
            let x = i as f32 * 0.37;
            let want = (x as f64 * 1000.0).round_ties_even() as u32;
            assert_eq!(k(x), Some(want), "{x}");
        }
    }

    #[test]
    fn current_prefers_the_stats_ring() {
        let hw = HwWords {
            current: 5,
            ..Default::default()
        };
        let mut s = StatsSeen::default();
        assert_eq!(current(&s, &hw), (5, Source::HwData));
        s.note(TAG_POWER_STATE, 8);
        assert_eq!(current(&s, &hw), (8, Source::Stats));
        s.note(TAG_POWER_STATE, 0);
        assert_eq!(current(&s, &hw), (0, Source::Stats));
    }

    #[test]
    fn file_from_the_stats_ring() {
        let mut s = StatsSeen::default();
        s.note(4, 0);
        s.note(TAG_POWER_STATE, 8);
        let hw = HwWords {
            actual: 8,
            target: 8,
            current: 6,
            freq_mhz: 800.0f32.to_bits(),
        };
        let mut out = String::new();
        write(&mut out, &s, &hw, &table()).unwrap();
        assert_eq!(
            out,
            "pstate 8 freq_khz 800000 source stats\n\
             stats: records 2 power_states 1 last 8 tags 4:1 11:1\n\
             hwdata: actual 8 target 8 current 6 freq_mhz_f32 0x44480000 freq_khz 800000\n\
             table_khz: 0 100000 200000 300000 400000 500000 600000 700000 800000\n"
        );
    }

    #[test]
    fn file_from_hwdata_and_an_index_past_the_table() {
        let hw = HwWords {
            actual: 1,
            target: 2,
            current: 12,
            freq_mhz: f32::NAN.to_bits(),
        };
        let mut out = String::new();
        write(&mut out, &StatsSeen::default(), &hw, &table()).unwrap();
        let mut lines = out.lines();
        assert_eq!(
            lines.next(),
            Some("pstate 12 freq_khz unknown source hwdata")
        );
        assert_eq!(
            lines.next(),
            Some("stats: records 0 power_states 0 last none tags")
        );
        assert_eq!(
            lines.next(),
            Some("hwdata: actual 1 target 2 current 12 freq_mhz_f32 0x7fc00000 freq_khz unknown")
        );
    }
}
