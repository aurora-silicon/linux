// SPDX-License-Identifier: GPL-2.0-only OR MIT


use kernel::prelude::*;

pub(crate) fn requested_state() -> Result<u32> {
    let board = crate::g16_board::get()?;
    let state = (*crate::module_parameters::g16_pstate.value()).min(board.max_pstate());
    if state == 0 { return Err(EINVAL); }
    Ok(state)
}

pub(crate) fn frequency_khz() -> Result<u32> {
    Ok(crate::g16_board::get()?.frequencies_mhz[requested_state()? as usize] * 1000)
}
