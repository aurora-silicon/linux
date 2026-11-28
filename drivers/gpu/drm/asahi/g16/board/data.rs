// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Select firmware DVFS rows from this machine's ADT performance states.
//!
//! Each row pairs a main and an auxiliary frequency at a shared voltage.

/// One qualified mapping of the ADT states onto the first `rows` firmware rows.
pub(crate) struct Layout {
    pub(crate) name: &'static str,
    pub(crate) rows: usize,
    pub(crate) main: [usize; PERF_STATES],
    pub(crate) auxiliary: [usize; PERF_STATES],
}

/// Tried in order; a layout matches only when each row's two states share frequency, SRAM
/// frequency and both voltages. The 10-row (6 equal-voltage pairs) and 11-row (5 equal-voltage
/// pairs plus a single top state) ADT shapes are mutually exclusive, so the row count is
/// derived directly from the ADT table without any board check.
pub(crate) const LAYOUTS: [Layout; 2] = [
    // 10-row ADT shape (e.g. J713/J715): four common low states followed by six equal-voltage
    // pairs (4/5 through 14/15).
    Layout {
        name: "10-row",
        rows: 10,
        main: [0, 1, 2, 3, 5, 7, 9, 11, 13, 15, 0],
        auxiliary: [0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 0],
    },
    // 11-row ADT shape (e.g. J604/J773g): five low states, five equal-voltage pairs (5/6
    // through 13/14) and a single top state 15, with every distinct state kept.
    Layout {
        name: "11-row",
        rows: 11,
        main: [0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 15],
        auxiliary: [0, 1, 2, 3, 4, 5, 7, 9, 11, 13, 15],
    },
];

// Four low states, then five equal-voltage pairs. The equal frequency at source
// states 9 and 10 belongs to two voltage bins and is kept in both rows.
const NINE_ROW_LAYOUT: Layout = Layout {
    name: "9-row",
    rows: 9,
    main: [0, 1, 2, 3, 5, 7, 9, 11, 13, 0, 0],
    auxiliary: [0, 1, 2, 3, 4, 6, 8, 10, 12, 0, 0],
};

use super::super::fw::initdata::{Performance, PERF_STATES};

/// Allocation-free selection, also exercised directly by the host-side source-data tests.
pub(crate) fn select(
    source: &super::super::adt::perf::Source,
) -> core::result::Result<Performance, super::super::adt::perf::Error> {
    select_named(source).map(|(performance, _)| performance)
}

/// As [`select`], with the name of the ADT shape that matched.
pub(crate) fn select_named(
    source: &super::super::adt::perf::Source,
) -> core::result::Result<(Performance, &'static str), super::super::adt::perf::Error> {
    use super::super::adt::perf::Error;

    let layouts = match source.count {
        14 => core::slice::from_ref(&NINE_ROW_LAYOUT),
        16 => LAYOUTS.as_slice(),
        _ => return Err(Error::Shape),
    };
    for layout in layouts {
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
        count: layout.rows,
        frequency: [0; PERF_STATES],
        auxiliary: [0; PERF_STATES],
        voltage: [0; PERF_STATES],
        sram: [0; PERF_STATES],
    };
    for i in 0..layout.rows {
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
