// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J616s retained-profile jack control probe.
//!
//! This child reads the qualified EPIC-v2 service without attaching a device,
//! uploading coefficients, powering a path, allocating DMA or registering a
//! PCM. Its provider and DMA lifecycle must be qualified before streaming.

#[path = "j616s_jack_profile.rs"]
mod profile;

use kernel::{
    bindings, c_str,
    device::Core,
    module_platform_driver, of, platform,
    prelude::*,
    soc::apple::aop::{EPICService, ServiceABI, ServicePlatformData, AOP},
    str::CString,
    sync::Arc,
};
use profile::Route;

struct J616sJack {
    // Owned independently of provider drvdata; no teardown request is needed
    // because this child never changes firmware or publishes a DMA address.
    _provider: Arc<dyn AOP>,
    _service: EPICService,
}

fn get_property(
    provider: &dyn AOP,
    service: &EPICService,
    route: Route,
    property: u32,
    bytes: usize,
) -> Result<KVec<u8>> {
    let request = profile::get_request(route, property);
    // Capture one extra byte so transport truncation cannot hide a long reply.
    let (retcode, reply) = provider.epic_call_ret(service, 0x20, &request, bytes + 5)?;
    if retcode != 0 || profile::property_data(&reply, bytes).is_none() {
        return Err(EIO);
    }
    Ok(reply)
}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <J616sJack as platform::Driver>::IdInfo,
    [(of::DeviceId::new(c_str!("apple,j616s-aop-jack")), ())]
);

impl platform::Driver for J616sJack {
    type IdInfo = ();
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(pdev: &platform::Device<Core>, _info: Option<&()>) -> impl PinInit<Self, Error> {
        // SAFETY: The compatible string is static and NUL-terminated.
        if unsafe { bindings::of_machine_is_compatible(c_str!("apple,j616s").as_char_ptr()) } == 0 {
            return Err(ENODEV);
        }
        let node = pdev.as_ref().fwnode().ok_or(ENODEV)?;
        // SAFETY: The node reference stays alive for the call. Manual bind
        // cannot bypass the loader's explicit inactive child status.
        if !unsafe { bindings::fwnode_device_is_available(node.as_raw()) } {
            return Err(ENODEV);
        }
        let uuid = node
            .property_read::<CString>(c_str!("apple,firmware-uuid"))
            .required_by(pdev.as_ref())?;
        let name = node
            .property_read::<CString>(c_str!("apple,firmware-name"))
            .required_by(pdev.as_ref())?;
        // SAFETY: Only the AOP provider creates this platform child. ABI and
        // admission are checked before any firmware request is issued.
        let provider =
            unsafe { <dyn AOP>::audio_from_child(pdev.as_ref(), ServiceABI::J616sJack) }?;
        // SAFETY: The provider owns the copied platform data during probe.
        let payload = unsafe {
            (*pdev.as_ref().as_raw())
                .platform_data
                .cast::<ServicePlatformData>()
                .as_ref()
        }
        .ok_or(ENODEV)?;
        let service = payload.service;
        if !profile::identity_matches(
            uuid.to_bytes(),
            name.to_bytes(),
            provider.protocol_version()?,
            service.endpoint,
        ) {
            return Err(ENODEV);
        }
        for route in [Route::Cout, Route::Cin] {
            let state = get_property(provider.as_ref(), &service, route, 200, 4)?;
            if !profile::idle_reply(&state) {
                return Err(EBUSY);
            }
            let mca = get_property(provider.as_ref(), &service, route, 701, 109)?;
            if !profile::profile_reply(route, &mca) {
                return Err(ENODEV);
            }
        }
        // Recheck both shared-clock users after all profile reads. This is
        // admission for control inspection, not concurrent PCM qualification.
        for route in [Route::Cout, Route::Cin] {
            let state = get_property(provider.as_ref(), &service, route, 200, 4)?;
            if !profile::idle_reply(&state) {
                return Err(EBUSY);
            }
            dev_info!(
                pdev.as_ref(),
                "{:?}: retained profile, nominal 48000 Hz, {} channels, ADMAC channel {}",
                route.device(),
                route.channels(),
                route.dma_channel()
            );
        }
        Ok(Self {
            _provider: provider,
            _service: service,
        })
    }
}

module_platform_driver! {
    type: J616sJack,
    name: "snd_soc_j616s_aop_jack",
    description: "J616s retained-profile AOP jack control probe",
    license: "Dual MIT/GPL",
}
