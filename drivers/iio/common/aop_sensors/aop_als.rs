// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP ambient light sensor driver
//!
//! Copyright (C) The Asahi Linux Contributors

use kernel::{
    bindings, c_str,
    device::Core,
    firmware::Firmware,
    iio::common::aop_sensors::{
        AopSensorData, FakehidRegistration, IIORegistration, MessageProcessor,
    },
    module_platform_driver, of, platform,
    prelude::*,
    soc::apple::aop::{EPICService, AOP},
    sync::Arc,
    types::ForeignOwnable,
};

const EPIC_SUBTYPE_GET_AOP_PROPERTY: u16 = 0xa;
const EPIC_SUBTYPE_SET_ALS_PROPERTY: u16 = 0x4;

/* ALS properties, in the order the sensor expects them. */
const ALS_PROP_INTERVAL: u32 = 0x00;
const ALS_PROP_CALIBRATION: u32 = 0x0b;
const ALS_PROP_BATCH_INTERVAL: u32 = 0x16;
const ALS_PROP_RAW_INTERVAL: u32 = 0x5c;
const ALS_PROP_VERBOSITY: u32 = 0xe1;

/*
 * The sensor only starts reporting on the zero to non-zero transition of the
 * report interval, and it only accepts that transition once the batch and raw
 * intervals have been written.  Writing the same non-zero interval again does
 * nothing. At initial probe the interval is zero, providing the edge.
 *
 * The batch and raw values are not intervals despite their names; these are
 * the values the sensor is configured with on this generation.  The sensor
 * rejects all three writes with a not-permitted status and arms regardless,
 * so the return codes are logged but not treated as failures.
 */
const ALS_BATCH_INTERVAL: u32 = 1;
const ALS_RAW_INTERVAL: u32 = 0;
const ALS_VERBOSITY: u32 = 6;
/// Reporting interval in microseconds.  The sensor delivers reports at this
/// rate once armed.
const ALS_INTERVAL_US: u32 = 200000;
const LUX_OFFSET_CT720: usize = 0x1d;
const LUX_OFFSET_VD6286: usize = 0x28;
/*
 * The CT817 sends a fixed 32-byte report: two raw channel floats at +0x08 and
 * +0x0c and the lux float at +0x1c.  Note this is one byte below the CT720's
 * 0x1d -- the parts do not share a layout despite being the same family.
 */
const LUX_OFFSET_CT817: usize = 0x1c;

fn get_lux_offset(aop: &dyn AOP, dev: &platform::Device, svc: &EPICService) -> Result<usize> {
    let name = get_aop_property(aop, svc, 0xf, 16)?.1;
    match name.as_slice() {
        b"Redbird\0" => Ok(LUX_OFFSET_VD6286),
        b"FireFish2\0" => Ok(LUX_OFFSET_CT720),
        // J700 (MacBook Neo): the AOP reports the part number itself rather
        // than a codename.  CT817 does NOT share the CT720's layout -- its lux
        // float is at +0x1c of a 32-byte report, one byte below the CT720's
        // 0x1d (see LUX_OFFSET_CT817).
        b"CT817\0" => Ok(LUX_OFFSET_CT817),
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

fn enable_als(
    aop: &dyn AOP,
    dev: &platform::Device,
    svc: &EPICService,
    offset: usize,
) -> Result<()> {
    if offset != LUX_OFFSET_CT817 {
        // Existing sensors receive their mandatory calibration over EPIC.
        let fw = Firmware::request(c_str!("apple/aop-als-cal.bin"), dev.as_ref())?;
        set_als_property(aop, svc, ALS_PROP_CALIBRATION, fw.data())?;
        set_als_property(aop, svc, ALS_PROP_INTERVAL, &ALS_INTERVAL_US.to_le_bytes())?;
        return Ok(());
    }

    // CT817 calibration is supplied separately on the T8140 setup mailbox
    // before EPIC clients start. It is not the legacy ALS property payload.
    // Every one of these writes is answered with a not-permitted status and
    // takes effect anyway, so firmware status is logged rather than failing
    // probe. Allocation and transport errors still abort the sequence.
    // Do not add MODE (0xd7) or 0xe4 here: both wedge the AOP outright, and the
    // endpoint -- along with every other sensor behind it -- stays dead for the
    // rest of the boot.  What actually arms the part is the calibration the AOP
    // driver pushes on the setup port; see soc/apple/aop.rs.
    let set = |tag: u32, val: u32, what: &'static str| -> Result<()> {
        let rc = set_als_property(aop, svc, tag, &val.to_le_bytes())?;
        dev_dbg!(dev.as_ref(), "{} = {:#x}, status {:#x}\n", what, val, rc);
        Ok(())
    };

    set(ALS_PROP_VERBOSITY, ALS_VERBOSITY, "verbosity")?;
    // Order matters: the batch and raw values must be in place before the
    // report interval, and the sensor starts reporting on the zero to
    // non-zero transition of the interval.  It is zero at probe, so writing
    // it once here is the transition.
    set(
        ALS_PROP_BATCH_INTERVAL,
        ALS_BATCH_INTERVAL,
        "batch interval",
    )?;
    set(ALS_PROP_RAW_INTERVAL, ALS_RAW_INTERVAL, "raw interval")?;

    let rc = set_als_property(aop, svc, ALS_PROP_INTERVAL, &ALS_INTERVAL_US.to_le_bytes())?;
    dev_dbg!(
        dev.as_ref(),
        "reporting every {} us, status {:#x}\n",
        ALS_INTERVAL_US,
        rc
    );

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

fn f32_to_u32(f: u32) -> u32 {
    if f & 0x80000000 != 0 {
        return 0;
    }
    let exp = ((f & 0x7f800000) >> 23) as i32 - 127;
    if exp < 0 {
        return 0;
    }
    if exp == 128 && f & 0x7fffff != 0 {
        return 0;
    }
    let mant = f & 0x7fffff | 0x800000;
    if exp <= 23 {
        return mant >> (23 - exp);
    }
    if exp >= 32 {
        return u32::MAX;
    }
    mant << (exp - 23)
}

struct MsgProc(usize);

impl MessageProcessor for MsgProc {
    fn process(&self, message: &[u8]) -> u32 {
        let offset = self.0;
        if offset + 4 > message.len() {
            return 0;
        }
        let raw = u32::from_le_bytes(message[offset..offset + 4].try_into().unwrap());
        f32_to_u32(raw)
    }
}

struct IIOAopAlsDriver {
    // Field order also removes the callback before IIO teardown on rollback.
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
        let ty = bindings::BINDINGS_IIO_LIGHT;
        // Query the part name first: it selects the report layout below.  Do
        // not fetch the HID report descriptor (subtype 0x01) here.  The AOP
        // never answers it on this endpoint, and an unanswered EPIC call
        // wedges the endpoint's only call slot, losing the device entirely.
        let offset = get_lux_offset(adata.as_ref(), pdev, &service)?;
        let data = AopSensorData::new(dev.into(), ty, MsgProc(offset))?;
        let info_mask = 1 << bindings::BINDINGS_IIO_CHAN_INFO_PROCESSED;
        // No callback is installed until all IIO allocation/registration can
        // succeed. Later failures drop listener before this earlier local.
        let iio = IIORegistration::<MsgProc>::new(
            data.clone(),
            c"aop-sensors-als",
            ty,
            info_mask,
            &THIS_MODULE,
        )?;
        let listener = FakehidRegistration::new(adata.clone(), service, data)?;
        enable_als(adata.as_ref(), pdev, &service, offset)?;
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
