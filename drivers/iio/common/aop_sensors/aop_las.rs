// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP lid angle sensor driver
//!
//! Copyright (C) The Asahi Linux Contributors

use kernel::{
    bindings, c_str,
    device::Core,
    iio::common::aop_sensors::{AopSensorData, IIORegistration, MessageProcessor, MICRO},
    module_platform_driver, of, platform,
    prelude::*,
    soc::apple::aop::{EPICService, FakehidRegistration, AOP},
};

struct MsgProc;

impl MessageProcessor for MsgProc {
    fn process(&self, message: &[u8]) -> Option<u64> {
        message.get(1).map(|angle| u64::from(*angle) * MICRO)
    }
}

struct IIOAopLasDriver {
    /// Declared first: the subscription ends before the IIO device goes.
    listener: FakehidRegistration,
    _iio: IIORegistration<MsgProc>,
}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    (),
    [(of::DeviceId::new(c_str!("apple,aop-las")), ())]
);

impl platform::Driver for IIOAopLasDriver {
    type IdInfo = ();

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(pdev: &platform::Device<Core>, _info: Option<&()>) -> impl PinInit<Self, Error> {
        let dev = pdev.as_ref();
        // SAFETY: This driver binds only to the service devices the AOP core
        // registers, and `pdev` is being probed.
        let adata = unsafe { <dyn AOP>::from_child(dev) }?;
        // SAFETY: `dev` is a valid device; the AOP core attached its service
        // record as the platform data of every service device it registers.
        let service = unsafe { (*dev.as_raw()).platform_data.cast::<EPICService>() };
        if service.is_null() {
            return Err(ENODEV);
        }
        // SAFETY: The record lives as long as the device it is attached to.
        let service = unsafe { *service };

        let ty = bindings::BINDINGS_IIO_ANGL;
        let data = AopSensorData::new(dev.into(), ty, MsgProc)?;
        let listener = FakehidRegistration::new(adata, service, data.clone())?;
        let info_mask = 1 << bindings::BINDINGS_IIO_CHAN_INFO_RAW;
        let iio =
            IIORegistration::<MsgProc>::new(data, c"aop-sensors-las", ty, info_mask, &THIS_MODULE)?;
        Ok(IIOAopLasDriver {
            listener,
            _iio: iio,
        })
    }

    fn unbind(_dev: &platform::Device<Core>, this: Pin<&Self>) {
        this.listener.unregister();
    }
}

module_platform_driver! {
    type: IIOAopLasDriver,
    name: "iio_aop_las",
    description: "AOP lid angle sensor driver",
    license: "Dual MIT/GPL",
    alias: ["platform:iio_aop_las"],
}
