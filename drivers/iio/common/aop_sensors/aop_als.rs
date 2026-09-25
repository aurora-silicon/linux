// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP ambient light sensor driver
//!
//! Copyright (C) The Asahi Linux Contributors

use kernel::{
    bindings, c_str,
    device::Core,
    firmware::Firmware,
    iio::common::aop_sensors::{AopSensorData, IIORegistration, MessageProcessor, MICRO},
    module_platform_driver, of, platform,
    prelude::*,
    soc::apple::aop::{EPICService, FakehidRegistration, AOP},
};

const EPIC_SUBTYPE_GET_AOP_PROPERTY: u16 = 0xa;
const EPIC_SUBTYPE_SET_ALS_PROPERTY: u16 = 0x4;
const LUX_OFFSET_CT720: usize = 0x1d;
const LUX_OFFSET_VD6286: usize = 0x28;

fn get_lux_offset(aop: &dyn AOP, dev: &platform::Device, svc: &EPICService) -> Result<usize> {
    let name = get_aop_property(aop, svc, 0xf, 16)?.1;
    match name.as_slice() {
        b"Redbird\0" => Ok(LUX_OFFSET_VD6286),
        b"FireFish2\0" => Ok(LUX_OFFSET_CT720),
        _ => {
            dev_warn!(
                dev.as_ref(),
                "Unknown sensor type {:?}",
                core::str::from_utf8(&name)
            );
            Err(EIO)
        }
    }
}

fn enable_als(aop: &dyn AOP, dev: &platform::Device, svc: &EPICService) -> Result<()> {
    let fw = Firmware::request(c_str!("apple/aop-als-cal.bin"), dev.as_ref())?;
    set_als_property(aop, svc, 0xb, fw.data())?;
    set_als_property(aop, svc, 0, &200000u32.to_le_bytes())?;

    Ok(())
}

fn get_aop_property(
    aop: &dyn AOP,
    svc: &EPICService,
    tag: u32,
    data_len: usize,
) -> Result<(u32, KVec<u8>)> {
    let mut buf = KVec::new();
    buf.resize(8, 0, GFP_KERNEL)?;
    buf[4..8].copy_from_slice(&tag.to_le_bytes());
    aop.epic_call_ret(svc, EPIC_SUBTYPE_GET_AOP_PROPERTY, &buf, data_len)
}

fn set_als_property(aop: &dyn AOP, svc: &EPICService, tag: u32, data: &[u8]) -> Result<u32> {
    let mut buf = KVec::new();
    buf.resize(data.len() + 8, 0, GFP_KERNEL)?;
    buf[8..].copy_from_slice(data);
    buf[4..8].copy_from_slice(&tag.to_le_bytes());
    aop.epic_call(svc, EPIC_SUBTYPE_SET_ALS_PROPERTY, &buf)
}

/// Converts the bits of an IEEE 754 single to microlux, rounding down.
/// Negative values clamp to zero; NaN and the infinities carry no reading.
/// Values beyond `MAX_MICRO` saturate when they are stored.
fn f32_to_micro(f: u32) -> Option<u64> {
    let exp = ((f >> 23) & 0xff) as i32;
    if exp == 0xff {
        return None;
    }
    if f & 0x8000_0000 != 0 || exp == 0 {
        return Some(0);
    }
    // The value is mant * 2^shift; scaled to microlux it is below 2^44
    // before the shift, so a left shift of up to 19 cannot overflow.
    let mant = u64::from(f & 0x7f_ffff | 0x80_0000) * MICRO;
    let shift = exp - 127 - 23;
    Some(if shift >= 20 {
        u64::MAX
    } else if shift >= 0 {
        mant << shift
    } else if shift <= -64 {
        0
    } else {
        mant >> -shift
    })
}

struct MsgProc(usize);

impl MessageProcessor for MsgProc {
    fn process(&self, message: &[u8]) -> Option<u64> {
        let raw = message.get(self.0..self.0 + 4)?;
        f32_to_micro(u32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]]))
    }
}

struct IIOAopAlsDriver {
    /// Declared first: the subscription ends before the IIO device goes.
    listener: FakehidRegistration,
    _iio: IIORegistration<MsgProc>,
}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    (),
    [(of::DeviceId::new(c_str!("apple,aop-als")), ())]
);

impl platform::Driver for IIOAopAlsDriver {
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
        let ty = bindings::BINDINGS_IIO_LIGHT;
        let offset = get_lux_offset(adata.as_ref(), pdev, &service)?;
        let data = AopSensorData::new(dev.into(), ty, MsgProc(offset))?;
        let listener = FakehidRegistration::new(adata.clone(), service, data.clone())?;
        enable_als(adata.as_ref(), pdev, &service)?;
        let info_mask = 1 << bindings::BINDINGS_IIO_CHAN_INFO_PROCESSED;
        let iio =
            IIORegistration::<MsgProc>::new(data, c"aop-sensors-als", ty, info_mask, &THIS_MODULE)?;
        Ok(IIOAopAlsDriver {
            listener,
            _iio: iio,
        })
    }

    fn unbind(_dev: &platform::Device<Core>, this: Pin<&Self>) {
        this.listener.unregister();
    }
}

module_platform_driver! {
    type: IIOAopAlsDriver,
    name: "iio_aop_als",
    description: "AOP ambient light sensor driver",
    license: "Dual MIT/GPL",
    alias: ["platform:iio_aop_als"],
    firmware: ["apple/aop-als-cal.bin"],
}
