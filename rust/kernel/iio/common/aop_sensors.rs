// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Apple AOP sensors common code
//!
//! Copyright (C) The Asahi Linux Contributors

use core::marker::{PhantomData, PhantomPinned};
use core::ptr;

use kernel::{
    bindings,
    device,
    prelude::*,
    soc::apple::aop::FakehidListener,
    sync::{
        aref::ARef,
        atomic::{
            Acquire,
            Atomic,
            Relaxed,
            Release, //
        },
        Arc, //
    },
    types::ForeignOwnable,
    ThisModule, //
};

/// Micro-units per unit of a channel value.
pub const MICRO: u64 = 1_000_000;

/// The largest reading that can be stored: both the integer part and the micro part have to fit
/// the `i32` values IIO reports. Larger readings saturate.
pub const MAX_MICRO: u64 = i32::MAX as u64 * MICRO + (MICRO - 1);

/// Decodes one fake-HID report of a sensor into the value the IIO channel exposes.
///
/// It is called from the AOP driver's receive worker while the IIO device may be read from
/// another thread, hence `Send + Sync`.
pub trait MessageProcessor: Send + Sync {
    /// Returns the reading carried by `message` in micro-units of the channel (millionths of a
    /// lux, of a degree, ...), or `None` when the message carries no reading.
    fn process(&self, message: &[u8]) -> Option<u64>;
}

/// The state shared between a sensor's report listener and its IIO device: the latest reading.
pub struct AopSensorData<T: MessageProcessor> {
    dev: ARef<device::Device>,
    ty: u32,
    /// The latest reading in micro-units, at most [`MAX_MICRO`].
    value: Atomic<u64>,
    /// Whether a reading has arrived at all; until then reads return `ENODATA`.
    valid: Atomic<bool>,
    msg_proc: T,
}

impl<T: MessageProcessor> AopSensorData<T> {
    /// Creates the state for one channel of type `ty` whose reports `msg_proc` decodes.
    pub fn new(dev: ARef<device::Device>, ty: u32, msg_proc: T) -> Result<Arc<AopSensorData<T>>> {
        Ok(Arc::new(
            AopSensorData {
                dev,
                ty,
                value: Atomic::new(0),
                valid: Atomic::new(false),
                msg_proc,
            },
            GFP_KERNEL,
        )?)
    }
}

impl<T: MessageProcessor> FakehidListener for AopSensorData<T> {
    fn process_fakehid_report(&self, data: &[u8]) -> Result<()> {
        // A report without a reading leaves the last reading in place.
        if let Some(value) = self.msg_proc.process(data) {
            self.value.store(value.min(MAX_MICRO), Relaxed);
            // Publishes the reading above to readers that see `valid`.
            self.valid.store(true, Release);
        }
        Ok(())
    }
}

/// The `read_raw` callback: processed values are reported with their micro part, raw values as
/// integers.
unsafe extern "C" fn aop_read_raw<T: MessageProcessor + 'static>(
    dev: *mut bindings::iio_dev,
    chan: *const bindings::iio_chan_spec,
    val: *mut i32,
    val2: *mut i32,
    mask: isize,
) -> i32 {
    // SAFETY: The IIO core calls this with the device and channel that `IIORegistration::new()`
    // registered; `priv_` is the `Arc` it stored there.
    let data = unsafe { Arc::<AopSensorData<T>>::borrow((*dev).priv_.cast()) };
    // SAFETY: `chan` is one of the registered channel specs.
    let ty = unsafe { (*chan).type_ };
    if data.ty != ty {
        return EINVAL.to_errno();
    }
    // A sensor that has not reported yet, for instance because it lacks its
    // calibration, has no reading rather than a reading of zero.
    if !data.valid.load(Acquire) {
        return ENODATA.to_errno();
    }
    let micro = data.value.load(Relaxed);
    // Both parts fit an `i32` because stored readings never exceed `MAX_MICRO`.
    let (int, frac) = ((micro / MICRO) as i32, (micro % MICRO) as i32);
    // SAFETY: `val` and `val2` point to the caller's result variables.
    unsafe {
        *val = int;
        *val2 = frac;
    }
    if mask == bindings::BINDINGS_IIO_CHAN_INFO_PROCESSED as isize {
        bindings::IIO_VAL_INT_PLUS_MICRO as i32
    } else if mask == bindings::BINDINGS_IIO_CHAN_INFO_RAW as isize {
        bindings::IIO_VAL_INT as i32
    } else {
        EINVAL.to_errno()
    }
}

struct IIOSpec {
    spec: [bindings::iio_chan_spec; 1],
    vtable: bindings::iio_info,
    _p: PhantomPinned,
}

/// TODO: add documentation
pub struct IIORegistration<T: MessageProcessor + 'static> {
    dev: *mut bindings::iio_dev,
    spec: Pin<KBox<IIOSpec>>,
    registered: bool,
    _p: PhantomData<AopSensorData<T>>,
}

impl<T: MessageProcessor + 'static> IIORegistration<T> {
    /// TODO: add documentation
    pub fn new(
        data: Arc<AopSensorData<T>>,
        name: &'static CStr,
        ty: u32,
        info_mask: usize,
        module: &ThisModule,
    ) -> Result<Self> {
        let spec = KBox::pin(
            IIOSpec {
                spec: [bindings::iio_chan_spec {
                    type_: ty,
                    __bindgen_anon_1: bindings::iio_chan_spec__bindgen_ty_1 {
                        scan_type: bindings::iio_scan_type {
                            sign: b'u' as _,
                            realbits: 32,
                            storagebits: 32,
                            ..Default::default()
                        },
                    },
                    info_mask_separate: info_mask,
                    ..Default::default()
                }],
                vtable: bindings::iio_info {
                    read_raw: Some(aop_read_raw::<T>),
                    ..Default::default()
                },
                _p: PhantomPinned,
            },
            GFP_KERNEL,
        )?;
        let mut this = IIORegistration {
            dev: ptr::null_mut(),
            spec,
            registered: false,
            _p: PhantomData,
        };
        this.dev = unsafe { bindings::iio_device_alloc(data.dev.as_raw(), 0) };
        if this.dev.is_null() {
            return Err(ENOMEM);
        }
        unsafe {
            (*this.dev).priv_ = data.clone().into_foreign().cast();
            (*this.dev).name = name.as_ptr() as _;
            // spec is now pinned
            (*this.dev).channels = this.spec.spec.as_ptr();
            (*this.dev).num_channels = this.spec.spec.len() as i32;
            (*this.dev).info = &this.spec.vtable;
        }
        let ret = unsafe { bindings::__iio_device_register(this.dev, module.as_ptr()) };
        if ret < 0 {
            dev_err!(data.dev, "Unable to register iio sensor");
            return Err(Error::from_errno(ret));
        }
        this.registered = true;
        Ok(this)
    }
}

impl<T: MessageProcessor + 'static> Drop for IIORegistration<T> {
    fn drop(&mut self) {
        if self.dev != ptr::null_mut() {
            unsafe {
                if self.registered {
                    bindings::iio_device_unregister(self.dev);
                }
                Arc::<AopSensorData<T>>::from_foreign((*self.dev).priv_.cast());
                bindings::iio_device_free(self.dev);
            }
        }
    }
}

unsafe impl<T: MessageProcessor> Send for IIORegistration<T> {}
unsafe impl<T: MessageProcessor> Sync for IIORegistration<T> {}
