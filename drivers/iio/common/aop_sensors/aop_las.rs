// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP lid angle sensor driver
//!
//! Copyright (C) The Asahi Linux Contributors

use kernel::{
    bindings, c_str,
    device::Core,
    iio::common::aop_sensors::{
        AopSensorData, FakehidRegistration, IIORegistration, MessageProcessor,
    },
    module_platform_driver, of, platform,
    prelude::*,
    soc::apple::aop::{EPICService, AOP},
    sync::Arc,
    types::ForeignOwnable,
};

struct MsgProc;

impl MessageProcessor for MsgProc {
    fn process(&self, message: &[u8]) -> u32 {
        message.get(1).copied().unwrap_or(0) as u32
    }
}

struct IIOAopLasDriver {
    // Field order also removes the callback before IIO teardown on rollback.
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
        let parent = dev.parent().ok_or(ENODEV)?;
        let parent_data = parent.get_drvdata::<core::ffi::c_void>();
        if parent_data.is_null() {
            return Err(ENODEV);
        }
        // SAFETY: AOP owns this child and synchronously releases its driver
        // before parent driver data can be freed. Driver-core serializes an
        // active child probe against that release; clone the live parent Arc.
        let adata_ptr = unsafe { Pin::<KBox<Arc<dyn AOP>>>::borrow(parent_data) };
        let adata = (&*adata_ptr).clone();
        // SAFETY: the child device is live; AOP supplied its service record.
        let service_ptr = unsafe { (*dev.as_raw()).platform_data as *const EPICService };
        if service_ptr.is_null() {
            return Err(ENODEV);
        }
        // SAFETY: the service record lives with this registered child device.
        let service = unsafe { *service_ptr };

        let ty = bindings::BINDINGS_IIO_ANGL;
        let data = AopSensorData::new(dev.into(), ty, MsgProc)?;
        let info_mask = 1 << bindings::BINDINGS_IIO_CHAN_INFO_RAW;
        // Registration failures occur before any callback can reach MsgProc.
        let iio = IIORegistration::<MsgProc>::new(
            data.clone(),
            c"aop-sensors-las",
            ty,
            info_mask,
            &THIS_MODULE,
        )?;
        let listener = FakehidRegistration::new(adata, service, data)?;
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
