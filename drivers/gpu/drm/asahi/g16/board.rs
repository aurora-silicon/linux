// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4 board admission for the qualified firmware graph.
//!
//! M4 uses ten (J713, J604) or eleven (J773g) firmware rows selected from sixteen source
//! states, including off, and sixteen voltage lanes. The source values belong to this
//! machine's preserved original ADT.

pub(crate) mod data;

use super::adt::GpuAdt;
use super::fw::initdata::Performance;
use kernel::{device, prelude::*};

/// Admit the qualified table layout while preserving every machine-specific source value.
/// Differently shaped tables, inconsistent voltage pairs and layouts with a row count other
/// than the one qualified for this board fail before publication.
pub(crate) fn qualify(dev: &device::Device, adt: &GpuAdt) -> Result<Performance> {
    let rows = super::boot::glue::perf_rows();
    let (performance, layout) = data::select_named(&adt.performance, Some(rows)).map_err(|error| {
        dev_err!(
            dev,
            "M4 original ADT performance layout rejected for {} firmware rows: {:?}\n",
            rows,
            error
        );
        EINVAL
    })?;
    dev_info!(
        dev,
        "M4 ADT DVFS: {} source states -> {} firmware rows ({} layout), top {} MHz\n",
        adt.performance.count,
        performance.count,
        layout,
        performance.frequency[performance.count - 1]
    );
    for i in 0..performance.count {
        dev_info!(
            dev,
            "M4 ADT DVFS S{}: main={} MHz auxiliary={} MHz core={} mV SRAM={} mV\n",
            i,
            performance.frequency[i],
            performance.auxiliary[i],
            performance.voltage[i],
            performance.sram[i]
        );
    }
    Ok(performance)
}
