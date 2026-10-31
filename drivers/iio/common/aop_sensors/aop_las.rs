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
        // Report 1 contains a little-endian, 9-bit angle in degrees.
        // Other report IDs carry sensor properties, not angle samples.
        if *message.first()? != 1 {
            return None;
        }
        let angle = u16::from_le_bytes([*message.get(1)?, *message.get(2)?]) & 0x1ff;
        if angle > 360 {
            return None;
        }
        Some(u64::from(angle) * MICRO)
    }

    fn scale(&self) -> Option<(i32, i32)> {
        // IIO_ANGL uses radians: one raw count is pi / 180 radians.
        Some((0, 17_453_293))
    }
}

struct IIOAopLasDriver {
    /// Declared first: the subscription ends before the IIO device goes.
    listener: FakehidRegistration,
    _iio: IIORegistration<MsgProc>,
}

kernel::of_device_table!(
    OF_TABLE,
    (),
    [(of::DeviceId::new(c_str!("apple,aop-las")), ())]
);

impl platform::Driver for IIOAopLasDriver {
    type IdInfo = ();
    type Data<'bound> = IIOAopLasDriver;

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        _info: Option<&'bound ()>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
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
        let info_mask = (1 << bindings::BINDINGS_IIO_CHAN_INFO_RAW)
            | (1 << bindings::BINDINGS_IIO_CHAN_INFO_SCALE);
        let iio = IIORegistration::<MsgProc>::new(
            data,
            c"aop-sensors-las",
            ty,
            info_mask,
            kernel::module::this_module::<crate::LocalModule>(),
        )?;
        Ok(IIOAopLasDriver {
            listener,
            _iio: iio,
        })
    }

    fn unbind(_dev: &platform::Device<Core<'_>>, this: Pin<&Self>) {
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
