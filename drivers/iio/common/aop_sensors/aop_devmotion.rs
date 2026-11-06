// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP device-motion fusion driver
//!
//! The `devmotion6` service reports the AOP fusion engine's orientation as a
//! quaternion. The service has several versioned report layouts; the current
//! layouts all carry four little-endian IEEE-754 singles in X, Y, Z, W order.
//! IIO exposes them in its standard W, X, Y, Z order as signed Q30 values.

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
/// Selects the firmware's built-in calibration. AppleSPUHIDDriver writes this
/// even though devmotion6 has no service-specific calibration packets.
const PROP_CALIBRATION_STATE: u32 = 61;
const CALIBRATION_DEFAULT: u32 = 1;
/// The status a calibration-state write is answered with when the service
/// already holds a state from an earlier start in the same AOP session (a
/// rebind, a resume, or a warm reboot); the stream can be started as is.
const STATUS_UNSUPPORTED: u32 = 0xe000_02c7;
const INTERVAL_US: u32 = 20_000;
const Q_FRAC_BITS: i32 = 30;

/// The latest quaternion, in the IIO-defined W, X, Y, Z order.
#[derive(Clone, Copy, Default)]
struct Sample {
    quat: [i32; 4],
    valid: bool,
}

/// One buffer scan. A quaternion is aligned like `IIO_DECLARE_QUATERNION()`.
#[repr(C, align(16))]
struct Scan {
    quat: [i32; 4],
    timestamp: i64,
}

fn le_u32(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

/// Converts a finite IEEE-754 single to signed Q30 without using FP in the
/// kernel. Values outside the representation saturate.
fn f32_to_q30(bits: u32) -> Option<i32> {
    let exp = ((bits >> 23) & 0xff) as i32;
    if exp == 0xff {
        return None;
    }
    if exp == 0 {
        return Some(0);
    }

    let mant = i64::from(bits & 0x7f_ffff | 0x80_0000);
    let shift = exp - 127 - 23 + Q_FRAC_BITS;
    let magnitude = if shift >= 31 {
        i64::MAX
    } else if shift >= 0 {
        mant.checked_shl(shift as u32).unwrap_or(i64::MAX)
    } else if shift <= -63 {
        0
    } else {
        mant >> -shift
    };
    let signed = if bits & 0x8000_0000 != 0 {
        -magnitude
    } else {
        magnitude
    };
    Some(signed.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32)
}

fn decode(report: &[u8]) -> Option<[i32; 4]> {
    let offset = match (report.first().copied()?, report.len()) {
        (2, 108) => 28,
        (3 | 4, 60) => 12,
        // Type 1 is a five-byte state report and has no orientation.
        _ => return None,
    };
    let x = f32_to_q30(le_u32(report, offset)?)?;
    let y = f32_to_q30(le_u32(report, offset + 4)?)?;
    let z = f32_to_q30(le_u32(report, offset + 8)?)?;
    let w = f32_to_q30(le_u32(report, offset + 12)?)?;
    Some([w, x, y, z])
}

struct Control {
    dev: ARef<device::Device>,
    aop: Arc<dyn AOP>,
    service: EPICService,
}

impl Control {
    fn set(&self, prop: u32, value: u32) -> Result {
        self.set_allowing(prop, value, 0)
    }

    fn set_allowing(&self, prop: u32, value: u32, also_ok: u32) -> Result {
        let mut msg = KVec::with_capacity(12, GFP_KERNEL)?;
        msg.extend_from_slice(&0u32.to_le_bytes(), GFP_KERNEL)?;
        msg.extend_from_slice(&prop.to_le_bytes(), GFP_KERNEL)?;
        msg.extend_from_slice(&value.to_le_bytes(), GFP_KERNEL)?;
        let status = self
            .aop
            .epic_call(&self.service, SUBTYPE_SET_PROPERTY, &msg)?;
        if status != 0 && (also_ok == 0 || status != also_ok) {
            dev_err!(
                self.dev,
                "property {} refused: status {:#x}\n",
                prop,
                status
            );
            return Err(EIO);
        }
        Ok(())
    }

    /// Selects the default calibration before the first enable after probe.
    fn initialize(&self) -> Result {
        self.set_allowing(PROP_CALIBRATION_STATE, CALIBRATION_DEFAULT, STATUS_UNSUPPORTED)?;
        self.enable()
    }

    fn enable(&self) -> Result {
        self.set(PROP_INTERVAL, INTERVAL_US)
    }

    fn disable(&self) -> Result {
        self.set(PROP_INTERVAL, 0)
    }
}

#[pin_data]
struct State {
    iio: IioPtr,
    #[pin]
    sample: SpinLock<Sample>,
}

struct IioPtr(*mut bindings::iio_dev);

// SAFETY: The IIO device may be used from any thread. The listener only calls
// IIO helpers that are valid from the AOP receive worker.
unsafe impl Send for IioPtr {}
// SAFETY: As above.
unsafe impl Sync for IioPtr {}

impl FakehidListener for State {
    fn process_fakehid_report(&self, data: &[u8]) -> Result<()> {
        let Some(quat) = decode(data) else {
            // State-only reports are expected and leave the sample unchanged.
            return Ok(());
        };
        *self.sample.lock() = Sample { quat, valid: true };
        // SAFETY: The listener is removed before the IIO device is freed.
        let timestamp = unsafe { bindings::iio_get_time_ns(self.iio.0) };
        let scan = Scan { quat, timestamp };
        // SAFETY: `scan` contains the complete only data channel followed by
        // the timestamp, and the IIO core copies it synchronously.
        unsafe { bindings::iio_push_to_buffers(self.iio.0, ptr::from_ref(&scan).cast()) };
        Ok(())
    }
}

static SCAN_MASKS: [usize; 2] = [1, 0];

/// # Safety
///
/// The IIO core supplies the registered device, channel, and output arrays.
unsafe extern "C" fn read_raw_multi(
    indio_dev: *mut bindings::iio_dev,
    _chan: *const bindings::iio_chan_spec,
    max_len: i32,
    vals: *mut i32,
    val_len: *mut i32,
    mask: isize,
) -> i32 {
    // SAFETY: `priv_` is the `Arc<State>` installed during allocation.
    let state = unsafe { Arc::<State>::borrow((*indio_dev).priv_.cast()) };
    if mask == bindings::BINDINGS_IIO_CHAN_INFO_SCALE as isize {
        if max_len < 2 {
            return EINVAL.to_errno();
        }
        // SAFETY: The caller provided at least two elements.
        unsafe {
            *vals = 1;
            *vals.add(1) = Q_FRAC_BITS;
            *val_len = 2;
        }
        return bindings::IIO_VAL_FRACTIONAL_LOG2 as i32;
    }
    if mask != bindings::BINDINGS_IIO_CHAN_INFO_RAW as isize || max_len < 4 {
        return EINVAL.to_errno();
    }
    let sample = *state.sample.lock();
    if !sample.valid {
        return ENODATA.to_errno();
    }
    // SAFETY: The caller provided at least four elements.
    unsafe {
        ptr::copy_nonoverlapping(sample.quat.as_ptr(), vals, sample.quat.len());
        *val_len = sample.quat.len() as i32;
    }
    bindings::IIO_VAL_INT_MULTIPLE as i32
}

fn quaternion_channel() -> bindings::iio_chan_spec {
    let mut chan = bindings::iio_chan_spec {
        type_: bindings::BINDINGS_IIO_ROT,
        channel2: bindings::BINDINGS_IIO_MOD_QUATERNION as i32,
        scan_index: 0,
        __bindgen_anon_1: bindings::iio_chan_spec__bindgen_ty_1 {
            scan_type: bindings::iio_scan_type {
                __bindgen_anon_1: bindings::iio_scan_type__bindgen_ty_1 { sign: b's' },
                realbits: 32,
                storagebits: 32,
                repeat: 4,
                ..Default::default()
            },
        },
        info_mask_separate: (1usize << bindings::BINDINGS_IIO_CHAN_INFO_RAW)
            | (1usize << bindings::BINDINGS_IIO_CHAN_INFO_SCALE),
        ..Default::default()
    };
    chan.set_modified(1);
    chan
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

struct MotionIio {
    dev: *mut bindings::iio_dev,
    _spec: Pin<KBox<Spec>>,
    registered: bool,
}

impl MotionIio {
    fn new(pdev: &platform::Device<Core<'_>>) -> Result<(Self, Arc<State>)> {
        let dev = pdev.as_ref();
        let spec = KBox::pin(
            Spec {
                channels: [quaternion_channel(), timestamp_channel()],
                info: bindings::iio_info {
                    read_raw_multi: Some(read_raw_multi),
                    ..Default::default()
                },
                _p: PhantomPinned,
            },
            GFP_KERNEL,
        )?;
        // SAFETY: `dev` is a valid platform device.
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
            (*indio_dev).name = c"aop-sensors-devmotion6".as_char_ptr();
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

impl Drop for MotionIio {
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
unsafe impl Send for MotionIio {}
// SAFETY: As above; there are no methods taking `&self`.
unsafe impl Sync for MotionIio {}

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
    // SAFETY: By the callback contract above.
    let hook = unsafe { &*nb.cast::<PmHook>() };
    let ret = match action as u32 {
        bindings::PM_SUSPEND_PREPARE | bindings::PM_HIBERNATION_PREPARE => {
            hook.control.disable()
        }
        bindings::PM_POST_SUSPEND | bindings::PM_POST_HIBERNATION => {
            hook.control.enable()
        }
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

struct IIOAopDevMotionDriver {
    pm: Pin<KBox<PmHook>>,
    listener: FakehidRegistration,
    control: Arc<Control>,
    _iio: MotionIio,
}

kernel::of_device_table!(
    OF_TABLE,
    (),
    [(of::DeviceId::new(c_str!("apple,aop-devmotion6")), ())]
);

impl platform::Driver for IIOAopDevMotionDriver {
    type IdInfo = ();
    type Data<'bound> = IIOAopDevMotionDriver;

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
        let control = Arc::new(
            Control {
                dev: dev.into(),
                aop: aop.clone(),
                service,
            },
            GFP_KERNEL,
        )?;
        let (mut iio, state) = MotionIio::new(pdev)?;
        let listener = FakehidRegistration::new(aop, service, state)?;
        let pm = PmHook::new(control.clone())?;
        control.initialize()?;
        if let Err(e) = iio.register() {
            let _ = control.disable();
            return Err(e);
        }
        Ok(IIOAopDevMotionDriver {
            pm,
            listener,
            control,
            _iio: iio,
        })
    }

    fn unbind(_dev: &platform::Device<Core<'_>>, this: Pin<&Self>) {
        this.pm.unregister();
        let _ = this.control.disable();
        this.listener.unregister();
    }
}

module_platform_driver! {
    type: IIOAopDevMotionDriver,
    name: "iio_aop_devmotion",
    description: "AOP device-motion fusion driver",
    license: "Dual MIT/GPL",
    alias: ["platform:iio_aop_devmotion"],
}
