// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP temperature sensor driver
//!
//! The `als-temp` service on current laptops is a board-temperature sensor.
//! Its fake-HID report carries a signed Q16.16 Celsius value and an AOP
//! timestamp. Calibration data, when supplied by the bootloader, is sent
//! unchanged before reports are enabled.

use core::{
    ffi::{c_int, c_void},
    marker::PhantomPinned,
    ptr,
};

use kernel::{
    bindings, c_str,
    device::{self, Core},
    module_platform_driver, new_spinlock, of, platform,
    prelude::*,
    soc::apple::aop::{EPICService, FakehidListener, FakehidRegistration, AOP},
    sync::{
        aref::ARef,
        atomic::{Atomic, Relaxed},
        Arc, SpinLock,
    },
    types::{ForeignOwnable, Opaque},
};

const SUBTYPE_SET_PROPERTY: u16 = 4;
const PROP_INTERVAL: u32 = 0;
const PROP_CALIBRATION_STATE: u32 = 61;
const INTERVAL_US: u32 = 1_000_000;
const STATUS_UNSUPPORTED: u32 = 0xe000_02c7;
const REPORT_LEN: usize = 14;
const TEMP_OFFSET: usize = 2;

const CALIBRATION: [(u32, &CStr); 4] = [
    (11, c_str!("apple,temp-offset-calibration")),
    (208, c_str!("apple,absolute-temp-offset-cal")),
    (208, c_str!("apple,absolute-temp-offset-fcm-cal")),
    (208, c_str!("apple,absolute-temp-offset-bcm-cal")),
];

fn le_u32(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

fn location0_packet(prop: &[u8]) -> Option<&[u8]> {
    if prop.len() < 4 || prop[3] != 1 {
        return None;
    }
    let mut at = 4;
    for _ in 0..prop[2] {
        let location = le_u32(prop, at)?;
        let len = usize::try_from(le_u32(prop, at + 4)?).ok()?;
        let start = at.checked_add(8)?;
        let packet = prop.get(start..start.checked_add(len)?)?;
        if location == 0 {
            return Some(packet);
        }
        at = start + len;
    }
    None
}

fn decode_millicelsius(report: &[u8]) -> Option<i32> {
    if report.len() != REPORT_LEN {
        return None;
    }
    let raw = le_u32(report, TEMP_OFFSET)? as i32;
    Some(((i64::from(raw) * 1000) / 65_536) as i32)
}

struct Control {
    dev: ARef<device::Device>,
    aop: Arc<dyn AOP>,
    service: EPICService,
    packets: KVec<(u32, KVec<u8>)>,
}

impl Control {
    fn new(dev: &device::Device, aop: Arc<dyn AOP>, service: EPICService) -> Result<Arc<Self>> {
        let node = dev.fwnode().ok_or(ENODEV)?;
        let mut packets = KVec::new();
        for (prop, name) in CALIBRATION {
            if !node.property_present(name) {
                continue;
            }
            let len = node.property_count_elem::<u8>(name)?;
            let data = node
                .property_read_array_vec::<u8>(name, len)?
                .required_by(dev)?;
            let Some(packet) = location0_packet(&data) else {
                dev_err!(dev, "{:?} holds no calibration for location 0\n", name);
                return Err(EINVAL);
            };
            let mut owned = KVec::new();
            owned.extend_from_slice(packet, GFP_KERNEL)?;
            packets.push((prop, owned), GFP_KERNEL)?;
        }
        if packets.is_empty() {
            dev_dbg!(
                dev,
                "no temperature calibration from the bootloader; using sensor defaults\n"
            );
        }
        Ok(Arc::new(
            Self {
                dev: dev.into(),
                aop,
                service,
                packets,
            },
            GFP_KERNEL,
        )?)
    }

    fn write(&self, prop: u32, value: &[u8]) -> Result<u32> {
        let mut msg = KVec::with_capacity(8 + value.len(), GFP_KERNEL)?;
        msg.extend_from_slice(&0u32.to_le_bytes(), GFP_KERNEL)?;
        msg.extend_from_slice(&prop.to_le_bytes(), GFP_KERNEL)?;
        msg.extend_from_slice(value, GFP_KERNEL)?;
        self.aop
            .epic_call(&self.service, SUBTYPE_SET_PROPERTY, &msg)
    }

    fn start(&self) -> Result {
        for (prop, packet) in &self.packets {
            let status = self.write(*prop, packet)?;
            if status != 0 && status != STATUS_UNSUPPORTED {
                dev_err!(
                    self.dev,
                    "calibration property {} refused: status {:#x}\n",
                    prop,
                    status
                );
                return Err(EIO);
            }
        }
        let calibration_state = if self.packets.is_empty() { 1u32 } else { 2u32 };
        let status = self.write(PROP_CALIBRATION_STATE, &calibration_state.to_le_bytes())?;
        if status != 0 {
            dev_err!(
                self.dev,
                "calibration state refused: status {:#x}\n",
                status
            );
            return Err(EIO);
        }
        let status = self.write(PROP_INTERVAL, &INTERVAL_US.to_le_bytes())?;
        if status != 0 {
            dev_err!(self.dev, "interval refused: status {:#x}\n", status);
            return Err(EIO);
        }
        Ok(())
    }

    fn stop(&self) -> Result {
        let status = self.write(PROP_INTERVAL, &0u32.to_le_bytes())?;
        if status != 0 {
            dev_err!(self.dev, "stop refused: status {:#x}\n", status);
            return Err(EIO);
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Default)]
struct Sample {
    millicelsius: i32,
    valid: bool,
}

#[repr(C, align(8))]
struct Scan {
    millicelsius: i32,
    pad: u32,
    timestamp: i64,
}

#[pin_data]
struct State {
    iio: IioPtr,
    #[pin]
    sample: SpinLock<Sample>,
}

struct IioPtr(*mut bindings::iio_dev);

// SAFETY: The IIO device may be used from any thread.
unsafe impl Send for IioPtr {}
// SAFETY: As above.
unsafe impl Sync for IioPtr {}

impl FakehidListener for State {
    fn process_fakehid_report(&self, data: &[u8]) -> Result<()> {
        let millicelsius = decode_millicelsius(data).ok_or(EMSGSIZE)?;
        *self.sample.lock() = Sample {
            millicelsius,
            valid: true,
        };
        // SAFETY: The listener is removed before the IIO device is freed.
        let timestamp = unsafe { bindings::iio_get_time_ns(self.iio.0) };
        let scan = Scan {
            millicelsius,
            pad: 0,
            timestamp,
        };
        // SAFETY: The scan layout matches the only available scan mask.
        unsafe { bindings::iio_push_to_buffers(self.iio.0, ptr::from_ref(&scan).cast()) };
        Ok(())
    }
}

static SCAN_MASKS: [usize; 2] = [1, 0];

/// # Safety
///
/// The IIO core supplies the registered device and output pointers.
unsafe extern "C" fn read_raw(
    indio_dev: *mut bindings::iio_dev,
    _chan: *const bindings::iio_chan_spec,
    val: *mut i32,
    val2: *mut i32,
    mask: isize,
) -> i32 {
    // SAFETY: `priv_` is the `Arc<State>` installed during allocation.
    let state = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) };
    if mask == bindings::BINDINGS_IIO_CHAN_INFO_SAMP_FREQ as isize {
        // SAFETY: The pointers are the caller's result variables.
        unsafe {
            *val = 1;
            *val2 = 0;
        }
        return bindings::IIO_VAL_INT as i32;
    }
    if mask != bindings::BINDINGS_IIO_CHAN_INFO_PROCESSED as isize {
        return EINVAL.to_errno();
    }
    let sample = *state.sample.lock();
    if !sample.valid {
        return ENODATA.to_errno();
    }
    // SAFETY: The pointers are the caller's result variables. IIO temperature
    // processed values are in millidegrees Celsius.
    unsafe {
        *val = sample.millicelsius;
        *val2 = 0;
    }
    bindings::IIO_VAL_INT as i32
}

fn temp_channel() -> bindings::iio_chan_spec {
    bindings::iio_chan_spec {
        type_: bindings::BINDINGS_IIO_TEMP,
        scan_index: 0,
        __bindgen_anon_1: bindings::iio_chan_spec__bindgen_ty_1 {
            scan_type: bindings::iio_scan_type {
                __bindgen_anon_1: bindings::iio_scan_type__bindgen_ty_1 { sign: b's' },
                realbits: 32,
                storagebits: 32,
                ..Default::default()
            },
        },
        info_mask_separate: 1usize << bindings::BINDINGS_IIO_CHAN_INFO_PROCESSED,
        info_mask_shared_by_type: 1usize << bindings::BINDINGS_IIO_CHAN_INFO_SAMP_FREQ,
        ..Default::default()
    }
}

fn timestamp_channel() -> bindings::iio_chan_spec {
    bindings::iio_chan_spec {
        type_: bindings::BINDINGS_IIO_TIMESTAMP,
        channel: -1,
        scan_index: 1,
        __bindgen_anon_1: bindings::iio_chan_spec__bindgen_ty_1 {
            scan_type: bindings::iio_scan_type {
                __bindgen_anon_1: bindings::iio_scan_type__bindgen_ty_1 { sign: b's' },
                realbits: 64,
                storagebits: 64,
                ..Default::default()
            },
        },
        ..Default::default()
    }
}

struct Spec {
    channels: [bindings::iio_chan_spec; 2],
    info: bindings::iio_info,
    _p: PhantomPinned,
}

struct TempIio {
    dev: *mut bindings::iio_dev,
    _spec: Pin<KBox<Spec>>,
    registered: bool,
}

impl TempIio {
    fn new(pdev: &platform::Device<Core<'_>>) -> Result<(Self, Arc<State>)> {
        let dev = pdev.as_ref();
        let spec = KBox::pin(
            Spec {
                channels: [temp_channel(), timestamp_channel()],
                info: bindings::iio_info {
                    read_raw: Some(read_raw),
                    ..Default::default()
                },
                _p: PhantomPinned,
            },
            GFP_KERNEL,
        )?;
        // SAFETY: `dev` is valid.
        let indio_dev = unsafe { bindings::iio_device_alloc(dev.as_raw(), 0) };
        if indio_dev.is_null() {
            return Err(ENOMEM);
        }
        let state = match Arc::pin_init(
            pin_init!(State {
                iio: IioPtr(indio_dev),
                sample <- new_spinlock!(Sample::default()),
            }),
            GFP_KERNEL,
        ) {
            Ok(state) => state,
            Err(e) => {
                // SAFETY: Allocated above and not registered.
                unsafe { bindings::iio_device_free(indio_dev) };
                return Err(e);
            }
        };
        // SAFETY: The IIO device is unregistered and `spec` is pinned.
        unsafe {
            (*indio_dev).priv_ = state.clone().into_foreign().cast();
            (*indio_dev).name = c"aop-sensors-als-temp".as_char_ptr();
            (*indio_dev).channels = spec.channels.as_ptr();
            (*indio_dev).num_channels = spec.channels.len() as i32;
            (*indio_dev).info = &spec.info;
            (*indio_dev).modes = bindings::INDIO_DIRECT_MODE as i32;
            (*indio_dev).available_scan_masks = SCAN_MASKS.as_ptr();
        }
        // SAFETY: Both devices are valid and the buffer is device-managed.
        let ret = unsafe {
            bindings::devm_iio_kfifo_buffer_setup_ext(
                dev.as_raw(),
                indio_dev,
                ptr::null(),
                ptr::null_mut(),
            )
        };
        if ret < 0 {
            // SAFETY: The device is not registered; the private pointer is
            // the foreign `Arc` installed above.
            unsafe {
                drop(Arc::<State>::from_foreign((*indio_dev).priv_.cast()));
                bindings::iio_device_free(indio_dev);
            }
            return Err(Error::from_errno(ret));
        }
        Ok((
            Self {
                dev: indio_dev,
                _spec: spec,
                registered: false,
            },
            state,
        ))
    }

    fn register(&mut self) -> Result {
        let module = kernel::module::this_module::<crate::LocalModule>();
        // SAFETY: The device is fully initialized and unregistered.
        let ret = unsafe { bindings::__iio_device_register(self.dev, module.as_ptr()) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        self.registered = true;
        Ok(())
    }
}

impl Drop for TempIio {
    fn drop(&mut self) {
        // SAFETY: The device and its private `Arc` were installed by `new()`.
        unsafe {
            if self.registered {
                bindings::iio_device_unregister(self.dev);
            }
            drop(Arc::<State>::from_foreign((*self.dev).priv_.cast()));
            bindings::iio_device_free(self.dev);
        }
    }
}

// SAFETY: The IIO core serializes registration and removal.
unsafe impl Send for TempIio {}
// SAFETY: As above.
unsafe impl Sync for TempIio {}

#[repr(C)]
struct PmHook {
    nb: Opaque<bindings::notifier_block>,
    control: Arc<Control>,
    registered: Atomic<bool>,
}

/// # Safety
///
/// `nb` is the first field of a registered `PmHook`.
unsafe extern "C" fn pm_event(
    nb: *mut bindings::notifier_block,
    action: usize,
    _data: *mut c_void,
) -> c_int {
    // SAFETY: `nb` is the first field of a registered, pinned `PmHook`.
    let hook = unsafe { &*nb.cast::<PmHook>() };
    let ret = match action as u32 {
        bindings::PM_SUSPEND_PREPARE | bindings::PM_HIBERNATION_PREPARE => hook.control.stop(),
        bindings::PM_POST_SUSPEND | bindings::PM_POST_HIBERNATION => hook.control.start(),
        _ => return bindings::NOTIFY_DONE as c_int,
    };
    if let Err(e) = ret {
        dev_warn!(
            hook.control.dev,
            "reports not switched for sleep: {:?}\n",
            e
        );
    }
    bindings::NOTIFY_OK as c_int
}

#[cfg(CONFIG_PM_SLEEP)]
/// # Safety
///
/// `nb` remains pinned until [`pm_unregister`] is called.
unsafe fn pm_register(nb: *mut bindings::notifier_block) -> c_int {
    // SAFETY: The caller keeps the block pinned through unregister.
    unsafe { bindings::register_pm_notifier(nb) }
}

#[cfg(CONFIG_PM_SLEEP)]
/// # Safety
///
/// `nb` was registered by [`pm_register`].
unsafe fn pm_unregister(nb: *mut bindings::notifier_block) {
    // SAFETY: The block was registered by `pm_register`.
    unsafe { bindings::unregister_pm_notifier(nb) };
}

#[cfg(not(CONFIG_PM_SLEEP))]
/// # Safety
///
/// No requirements; this configuration has no PM notifier.
unsafe fn pm_register(_nb: *mut bindings::notifier_block) -> c_int {
    0
}

#[cfg(not(CONFIG_PM_SLEEP))]
/// # Safety
///
/// No requirements; this configuration has no PM notifier.
unsafe fn pm_unregister(_nb: *mut bindings::notifier_block) {}

// SAFETY: The PM core serializes notifier operations.
unsafe impl Send for PmHook {}
// SAFETY: As above.
unsafe impl Sync for PmHook {}

impl PmHook {
    fn new(control: Arc<Control>) -> Result<Pin<KBox<Self>>> {
        let hook = KBox::pin(
            Self {
                nb: Opaque::new(bindings::notifier_block {
                    notifier_call: Some(pm_event),
                    next: ptr::null_mut(),
                    priority: 0,
                }),
                control,
                registered: Atomic::new(false),
            },
            GFP_KERNEL,
        )?;
        // SAFETY: The notifier remains pinned in `hook`.
        let ret = unsafe { pm_register(hook.nb.get()) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        hook.registered.store(true, Relaxed);
        Ok(hook)
    }

    fn unregister(&self) {
        if self.registered.xchg(false, Relaxed) {
            // SAFETY: Registered by `new()` and still pinned.
            unsafe { pm_unregister(self.nb.get()) };
        }
    }
}

impl Drop for PmHook {
    fn drop(&mut self) {
        self.unregister();
    }
}

struct IIOAopTempDriver {
    pm: Pin<KBox<PmHook>>,
    listener: FakehidRegistration,
    control: Arc<Control>,
    _iio: TempIio,
}

kernel::of_device_table!(
    OF_TABLE,
    (),
    [(of::DeviceId::new(c_str!("apple,aop-als-temp")), ())]
);

impl platform::Driver for IIOAopTempDriver {
    type IdInfo = ();
    type Data<'bound> = IIOAopTempDriver;

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        _info: Option<&'bound ()>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        let dev = pdev.as_ref();
        // SAFETY: This binds only to children created by the AOP core.
        let aop = unsafe { <dyn AOP>::from_child(dev) }?;
        // SAFETY: The AOP core attaches an `EPICService` as platform data.
        let service = unsafe { (*dev.as_raw()).platform_data.cast::<EPICService>() };
        if service.is_null() {
            return Err(ENODEV);
        }
        // SAFETY: The record lives as long as this child device.
        let service = unsafe { *service };
        let control = Control::new(dev, aop.clone(), service)?;
        let (mut iio, state) = TempIio::new(pdev)?;
        let listener = FakehidRegistration::new(aop, service, state)?;
        let pm = PmHook::new(control.clone())?;
        control.start()?;
        if let Err(e) = iio.register() {
            let _ = control.stop();
            return Err(e);
        }
        Ok(IIOAopTempDriver {
            pm,
            listener,
            control,
            _iio: iio,
        })
    }

    fn unbind(_dev: &platform::Device<Core<'_>>, this: Pin<&Self>) {
        this.pm.unregister();
        let _ = this.control.stop();
        this.listener.unregister();
    }
}

module_platform_driver! {
    type: IIOAopTempDriver,
    name: "iio_aop_temp",
    description: "AOP temperature sensor driver",
    license: "Dual MIT/GPL",
    alias: ["platform:iio_aop_temp"],
}
