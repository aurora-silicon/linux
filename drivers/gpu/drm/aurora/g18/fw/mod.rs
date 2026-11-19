// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G18 firmware record and register encoders. Nothing here allocates or publishes.

#[allow(dead_code)]
pub(crate) mod compute;
#[allow(dead_code)]
pub(crate) mod compute_memory;
#[allow(dead_code)]
pub(crate) mod compute_params;
#[allow(dead_code)]
pub(crate) mod compute_state;
#[allow(dead_code)]
pub(crate) mod config;
#[allow(dead_code)]
pub(crate) mod graphics_payload;
#[allow(dead_code)]
pub(crate) mod initdata;
#[allow(dead_code)]
pub(crate) mod m5_render;
#[allow(dead_code)]
pub(crate) mod m5_render_target;
#[allow(dead_code)]
pub(crate) mod parameter_buffer;
#[allow(dead_code)]
pub(crate) mod render;
#[allow(dead_code)]
pub(crate) mod render_assembly;
#[allow(dead_code)]
pub(crate) mod render_command;
#[allow(dead_code)]
pub(crate) mod render_params;
#[allow(dead_code)]
pub(crate) mod render_passthrough;
#[allow(dead_code)]
pub(crate) mod scheduler;
#[allow(dead_code)]
pub(crate) mod sksm;
#[allow(dead_code)]
pub(crate) mod store;
#[allow(dead_code)]
pub(crate) mod submission;
#[allow(dead_code)]
pub(crate) mod uma;
#[allow(dead_code)]
pub(crate) mod vm_layout;
#[allow(dead_code)]
pub(crate) mod events;
#[allow(dead_code)]
pub(crate) mod rtkit;
