// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Qualify firmware DVFS rows using this machine's preserved original ADT.

pub(crate) mod data;

use super::adt::GpuAdt;
use super::fw::initdata::Performance;
use kernel::{device, prelude::*};

/// Admit the qualified table layout while preserving every machine-specific source value.
/// The firmware DVFS row count is derived directly from the preserved ADT table shape rather
/// than from a board compatible check; differently shaped tables or inconsistent voltage pairs
/// fail before publication.
pub(crate) fn qualify(dev: &device::Device, adt: &GpuAdt) -> Result<Performance> {
    let (performance, layout) = data::select_named(&adt.performance).map_err(|error| {
        dev_err!(dev, "GPU original ADT performance layout rejected: {:?}\n", error);
        EINVAL
    })?;
    dev_info!(
        dev,
        "GPU ADT DVFS: {} source states -> {} firmware rows ({} layout), top {} MHz\n",
        adt.performance.count,
        performance.count,
        layout,
        performance.frequency[performance.count - 1]
    );
    for i in 0..performance.count {
        dev_info!(
            dev,
            "GPU ADT DVFS S{}: main={} MHz auxiliary={} MHz core={} mV SRAM={} mV\n",
            i,
            performance.frequency[i],
            performance.auxiliary[i],
            performance.voltage[i],
            performance.sram[i]
        );
    }
    Ok(performance)
}
