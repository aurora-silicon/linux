// SPDX-License-Identifier: GPL-2.0-only OR MIT


use kernel::prelude::*;

pub(crate) const FREQUENCIES_MHZ: [u32; 10] =
    [0, 338, 618, 796, 928, 1056, 1170, 1278, 1338, 1470];
pub(crate) const AUX_FREQUENCIES_MHZ: [u32; 10] =
    [0, 338, 618, 796, 836, 952, 1053, 1152, 1204, 1326];
pub(crate) const VOLTAGES_MV: [u32; 10] =
    [125, 610, 645, 680, 725, 780, 825, 865, 895, 970];
pub(crate) const SRAM_VOLTAGES_MV: [u32; 10] =
    [780, 780, 780, 780, 780, 780, 825, 865, 895, 970];

pub(crate) fn requested_state() -> Result<u32> {
    let state = *crate::module_parameters::g16_pstate.value();
    if !(1..FREQUENCIES_MHZ.len() as u32).contains(&state) { return Err(EINVAL); }
    Ok(state)
}

pub(crate) fn frequency_khz() -> Result<u32> {
    Ok(FREQUENCIES_MHZ[requested_state()? as usize] * 1000)
}
