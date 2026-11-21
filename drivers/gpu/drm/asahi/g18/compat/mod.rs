// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Module 1134 items still read by the G18 backend: the `g17p_host_path`
//! WORD_STORES bit (`fw::store`), the `g17p_diag_knobs` FRG +0x222d bit
//! (`fw::render_params`) and the G17P SKSM scratch geometry
//! (`fw::submission`).

pub(crate) mod host_path;
pub(crate) mod timing_buffer;
#[allow(dead_code)]
pub(crate) mod g17p;
