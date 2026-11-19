// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Qualified selections of the original ADT's sixteen performance states for 25G83.
//!
//! The firmware takes ten rows, each a main and an auxiliary frequency at one voltage. Every
//! qualified layout maps the sixteen ADT states onto those rows by index; the values are always
//! this machine's own, never frequencies or voltages copied from a reference machine.

pub(crate) const SOURCE_STATES: usize = 16;

/// One qualified mapping of the ADT states onto the firmware rows.
pub(crate) struct Layout {
    pub(crate) name: &'static str,
    pub(crate) main: [usize; PERF_STATES],
    pub(crate) auxiliary: [usize; PERF_STATES],
}

/// Tried in order; a layout matches only when each row's two states share frequency, SRAM
/// frequency and both voltages, so the shapes below cannot be mistaken for each other.
pub(crate) const LAYOUTS: [Layout; 2] = [
    // J713: four common low states followed by six equal-voltage pairs.
    Layout {
        name: "J713",
        main: [0, 1, 2, 3, 5, 7, 9, 11, 13, 15],
        auxiliary: [0, 1, 2, 3, 4, 6, 8, 10, 12, 14],
    },
    // J604 (MacBook Pro 14-inch, M4): five low states, five equal-voltage pairs (5/6 to
    // 13/14) and a single top state 15. Eleven distinct rows do not fit in ten, so the fifth
    // low state (4) is left out; the four lowest states, every pair and the top state keep
    // their own ADT values.
    Layout {
        name: "J604",
        main: [0, 1, 2, 3, 6, 8, 10, 12, 14, 15],
        auxiliary: [0, 1, 2, 3, 5, 7, 9, 11, 13, 15],
    },
];

use super::super::fw::initdata::{Performance, PERF_STATES};

/// Allocation-free selection, also exercised directly by the host-side source-data tests.
pub(crate) fn select(
    source: &super::super::adt::perf::Source,
) -> core::result::Result<Performance, super::super::adt::perf::Error> {
    select_named(source).map(|(performance, _)| performance)
}

/// As [`select`], with the name of the layout that matched.
pub(crate) fn select_named(
    source: &super::super::adt::perf::Source,
) -> core::result::Result<(Performance, &'static str), super::super::adt::perf::Error> {
    use super::super::adt::perf::Error;
    if source.count != SOURCE_STATES {
        return Err(Error::Shape);
    }
    for layout in LAYOUTS.iter() {
        match select_layout(source, layout) {
            Err(Error::Shape) => continue,
            Err(error) => return Err(error),
            Ok(performance) => return Ok((performance, layout.name)),
        }
    }
    Err(Error::Shape)
}

fn select_layout(
    source: &super::super::adt::perf::Source,
    layout: &Layout,
) -> core::result::Result<Performance, super::super::adt::perf::Error> {
    use super::super::adt::perf::Error;
    let mut performance = Performance {
        frequency: [0; PERF_STATES],
        auxiliary: [0; PERF_STATES],
        voltage: [0; PERF_STATES],
        sram: [0; PERF_STATES],
    };
    for i in 0..PERF_STATES {
        let (main, aux) = (layout.main[i], layout.auxiliary[i]);
        if source.frequency_mhz[main] != source.sram_frequency_mhz[main]
            || source.frequency_mhz[aux] != source.sram_frequency_mhz[aux]
            || source.core_mv[main] != source.core_mv[aux]
            || source.sram_mv[main] != source.sram_mv[aux]
        {
            return Err(Error::Shape);
        }
        performance.frequency[i] = source.frequency_mhz[main];
        performance.auxiliary[i] = source.frequency_mhz[aux];
        performance.voltage[i] = source.core_mv[main];
        performance.sram[i] = source.sram_mv[main];
    }
    if !performance.valid() {
        return Err(Error::Range);
    }
    Ok(performance)
}
