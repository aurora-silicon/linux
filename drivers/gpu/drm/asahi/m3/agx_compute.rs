// SPDX-License-Identifier: GPL-2.0-only OR MIT

/// Complete client CDM range and its USC address window.
#[derive(Clone, Copy)]
pub(crate) struct Control {
    pub base: u64,
    pub end: u64,
    pub usc_base: u64,
    /// The helper program, zero unless the queue's command builder encodes it
    /// (`m3_submit::Queue::samplers_and_helpers`).
    #[cfg_attr(not(CONFIG_DRM_ASAHI_G14_V27_2), allow(dead_code))]
    pub helper: crate::m3::agx_uapi::UapiHelperProgram,
    /// The sampler heap and its sampler count, both zero unless the queue's command builder
    /// encodes them (`m3_submit::Queue::samplers_and_helpers`).
    #[cfg_attr(not(CONFIG_DRM_ASAHI_G14_V27_2), allow(dead_code))]
    pub sampler_heap: u64,
    #[cfg_attr(not(CONFIG_DRM_ASAHI_G14_V27_2), allow(dead_code))]
    pub sampler_count: u32,
}
