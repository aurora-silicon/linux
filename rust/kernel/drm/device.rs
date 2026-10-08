// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DRM device.
//!
//! C header: [`include/drm/drm_device.h`](srctree/include/drm/drm_device.h)

use crate::{
    alloc::allocator::Kmalloc,
    bindings, device,
    drm::{
        self,
        driver::AllocImpl, //
    },
    error::from_err_ptr,
    prelude::*,
    sync::aref::{
        ARef,
        AlwaysRefCounted, //
    },
    types::Opaque,
    workqueue::{
        HasDelayedWork,
        HasWork,
        Work,
        WorkItem, //
    },
};
use core::{
    alloc::Layout,
    cell::UnsafeCell,
    mem::{self, MaybeUninit},
    ops::Deref,
    ptr::{
        self,
        NonNull, //
    },
    sync::atomic::{AtomicBool, Ordering},
};

#[cfg(CONFIG_DRM_LEGACY)]
macro_rules! drm_legacy_fields {
    ( $($field:ident: $val:expr),* $(,)? ) => {
        bindings::drm_driver {
            $( $field: $val ),*,
            firstopen: None,
            preclose: None,
            dma_ioctl: None,
            dma_quiescent: None,
            context_dtor: None,
            irq_handler: None,
            irq_preinstall: None,
            irq_postinstall: None,
            irq_uninstall: None,
            get_vblank_counter: None,
            enable_vblank: None,
            disable_vblank: None,
            dev_priv_size: 0,
        }
    }
}

#[cfg(not(CONFIG_DRM_LEGACY))]
macro_rules! drm_legacy_fields {
    ( $($field:ident: $val:expr),* $(,)? ) => {
        bindings::drm_driver {
            $( $field: $val ),*
        }
    }
}

/// A typed DRM device with a specific `drm::Driver` implementation.
///
/// The device is always reference-counted.
///
/// # Invariants
///
/// `self.dev` is a valid instance of a `struct device`.
/// When `data_initialized` is true, `data` owns initialized, pinned driver data.
/// Otherwise, driver data must not be accessed.
#[repr(C)]
pub struct Device<T: drm::Driver> {
    dev: Opaque<bindings::drm_device>,
    data: UnsafeCell<MaybeUninit<T::Data>>,
    data_initialized: AtomicBool,
}

impl<T: drm::Driver> Device<T> {
    const VTABLE: bindings::drm_driver = drm_legacy_fields! {
        load: None,
        open: Some(drm::File::<T::File>::open_callback),
        postclose: Some(drm::File::<T::File>::postclose_callback),
        unload: None,
        release: Some(Self::release),
        master_set: None,
        master_drop: None,
        debugfs_init: None,
        gem_create_object: T::Object::ALLOC_OPS.gem_create_object,
        prime_handle_to_fd: T::Object::ALLOC_OPS.prime_handle_to_fd,
        prime_fd_to_handle: T::Object::ALLOC_OPS.prime_fd_to_handle,
        gem_prime_import: T::Object::ALLOC_OPS.gem_prime_import,
        gem_prime_import_sg_table: T::Object::ALLOC_OPS.gem_prime_import_sg_table,
        dumb_create: T::Object::ALLOC_OPS.dumb_create,
        dumb_map_offset: T::Object::ALLOC_OPS.dumb_map_offset,
        show_fdinfo: None,
        fbdev_probe: None,

        major: T::INFO.major,
        minor: T::INFO.minor,
        patchlevel: T::INFO.patchlevel,
        name: crate::str::as_char_ptr_in_const_context(T::INFO.name).cast_mut(),
        desc: crate::str::as_char_ptr_in_const_context(T::INFO.desc).cast_mut(),

        driver_features: T::FEATURES,
        ioctls: T::IOCTLS.as_ptr(),
        num_ioctls: T::IOCTLS.len() as i32,
        fops: &Self::GEM_FOPS,
    };

    const ALLOC_VTABLE: bindings::drm_driver = bindings::drm_driver {
        release: None,
        ..Self::VTABLE
    };

    const GEM_FOPS: bindings::file_operations = drm::gem::create_fops(T::MODULE);

    /// Create a new `drm::Device` for a `drm::Driver`.
    pub fn new(dev: &device::Device, data: impl PinInit<T::Data, Error>) -> Result<ARef<Self>> {
        // SAFETY: The device remains private until its data is initialized below.
        let drm = unsafe { Self::new_uninit(dev)? };
        // SAFETY: This is the only initializer, and no references to the data exist yet.
        unsafe { drm.init_data(data)? };
        Ok(drm)
    }

    /// Allocate a DRM device whose driver data will be initialized separately.
    ///
    /// # Safety
    ///
    /// The caller must not access driver data, publish the device to userspace, or
    /// schedule work that accesses driver data before calling [`Self::init_data`].
    pub unsafe fn new_uninit(dev: &device::Device) -> Result<ARef<Self>> {
        let layout = Kmalloc::aligned_layout(Layout::new::<Self>());
        // Allocation failure must not inspect fields that we have not initialized yet.
        let raw_drm: *mut Self = unsafe {
            bindings::__drm_dev_alloc(
                dev.as_raw(),
                const { &Self::ALLOC_VTABLE },
                layout.size(),
                mem::offset_of!(Self, dev),
            )
        }
        .cast();
        let raw_drm = NonNull::new(from_err_ptr(raw_drm)?).ok_or(ENOMEM)?;
        // SAFETY: Allocation succeeded, and these fields have no previous value.
        unsafe {
            ptr::addr_of_mut!((*raw_drm.as_ptr()).data)
                .write(UnsafeCell::new(MaybeUninit::uninit()));
            ptr::addr_of_mut!((*raw_drm.as_ptr()).data_initialized).write(AtomicBool::new(false));
        }
        // SAFETY: The allocation is private and its release state is now initialized.
        let drm_dev = unsafe { Self::into_drm_device(raw_drm) };
        unsafe { (*drm_dev).driver = const { &Self::VTABLE } };
        // SAFETY: We own the initial DRM reference; uninitialized data is behind UnsafeCell.
        Ok(unsafe { ARef::from_raw(raw_drm) })
    }

    /// Initialize the pinned driver data of a device allocated by [`Self::new_uninit`].
    ///
    /// # Safety
    ///
    /// The caller must be the only initializer, the data must be uninitialized, and
    /// no driver-data users may run until this method returns successfully.
    pub unsafe fn init_data(&self, data: impl PinInit<T::Data, Error>) -> Result {
        let raw_data = self.data.get().cast::<T::Data>();
        // SAFETY: The caller guarantees exclusive initialization of this pinned slot.
        unsafe { data.__pinned_init(raw_data) }?;
        // Publish destructor ownership only after the real initializer succeeded.
        self.data_initialized.store(true, Ordering::Release);
        Ok(())
    }

    pub(crate) fn as_raw(&self) -> *mut bindings::drm_device {
        self.dev.get()
    }

    /// # Safety
    ///
    /// `ptr` must be a valid pointer to a `struct device` embedded in `Self`.
    unsafe fn from_drm_device(ptr: *const bindings::drm_device) -> *mut Self {
        // SAFETY: By the safety requirements of this function `ptr` is a valid pointer to a
        // `struct drm_device` embedded in `Self`.
        unsafe { crate::container_of!(Opaque::cast_from(ptr), Self, dev) }.cast_mut()
    }

    /// # Safety
    ///
    /// `ptr` must be a valid pointer to `Self`.
    unsafe fn into_drm_device(ptr: NonNull<Self>) -> *mut bindings::drm_device {
        // SAFETY: By the safety requirements of this function, `ptr` is a valid pointer to `Self`.
        unsafe { &raw mut (*ptr.as_ptr()).dev }.cast()
    }

    /// Not intended to be called externally, except via declare_drm_ioctls!()
    ///
    /// # Safety
    ///
    /// Callers must ensure that `ptr` is valid, non-null, and has a non-zero reference count,
    /// i.e. it must be ensured that the reference count of the C `struct drm_device` `ptr` points
    /// to can't drop to zero, for the duration of this function call and the entire duration when
    /// the returned reference exists.
    ///
    /// Additionally, callers must ensure that the `struct device`, `ptr` is pointing to, is
    /// embedded in `Self`.
    #[doc(hidden)]
    pub unsafe fn from_raw<'a>(ptr: *const bindings::drm_device) -> &'a Self {
        // SAFETY: By the safety requirements of this function `ptr` is a valid pointer to a
        // `struct drm_device` embedded in `Self`.
        let ptr = unsafe { Self::from_drm_device(ptr) };

        // SAFETY: `ptr` is valid by the safety requirements of this function.
        unsafe { &*ptr.cast() }
    }

    extern "C" fn release(ptr: *mut bindings::drm_device) {
        // SAFETY: `ptr` is a valid pointer to a `struct drm_device` and embedded in `Self`.
        let this = unsafe { Self::from_drm_device(ptr) };

        // SAFETY: new_uninit() initialized this flag before enabling this callback.
        // A failed initializer unwinds its own partial fields and never sets the flag.
        if unsafe { (*this).data_initialized.load(Ordering::Acquire) } {
            // SAFETY: Successful initialization published ownership of pinned driver data.
            // DRM still owns the embedded C device and its final managed cleanup/kfree.
            unsafe { ptr::drop_in_place((*this).data.get().cast::<T::Data>()) };
        }
    }
}

impl<T: drm::Driver> Deref for Device<T> {
    type Target = T::Data;

    fn deref(&self) -> &Self::Target {
        debug_assert!(self.data_initialized.load(Ordering::Acquire));
        // SAFETY: Safe constructors initialize data; new_uninit() requires callers
        // to prevent data access until init_data() succeeds.
        unsafe { &*self.data.get().cast::<T::Data>() }
    }
}

// SAFETY: DRM device objects are always reference counted and the get/put functions
// satisfy the requirements.
unsafe impl<T: drm::Driver> AlwaysRefCounted for Device<T> {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::drm_dev_get(self.as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: `obj` is a valid pointer to `Self`.
        let drm_dev = unsafe { Self::into_drm_device(obj) };

        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::drm_dev_put(drm_dev) };
    }
}

impl<T: drm::Driver> AsRef<device::Device> for Device<T> {
    fn as_ref(&self) -> &device::Device {
        // SAFETY: `bindings::drm_device::dev` is valid as long as the DRM device itself is valid,
        // which is guaranteed by the type invariant.
        unsafe { device::Device::from_raw((*self.as_raw()).dev) }
    }
}

// SAFETY: A `drm::Device` can be released from any thread.
unsafe impl<T: drm::Driver> Send for Device<T> {}

// SAFETY: A `drm::Device` can be shared among threads because all immutable methods are protected
// by the synchronization in `struct drm_device`.
unsafe impl<T: drm::Driver> Sync for Device<T> {}

impl<T, const ID: u64> WorkItem<ID> for Device<T>
where
    T: drm::Driver,
    T::Data: WorkItem<ID, Pointer = ARef<Device<T>>>,
    T::Data: HasWork<Device<T>, ID>,
{
    type Pointer = ARef<Device<T>>;

    fn run(ptr: ARef<Device<T>>) {
        T::Data::run(ptr);
    }
}

// SAFETY:
//
// - `raw_get_work` and `work_container_of` return valid pointers by relying on
// `T::Data::raw_get_work` and `container_of`. In particular, `T::Data` is
// stored inline in `drm::Device`, so the `container_of` call is valid.
//
// - The two methods are true inverses of each other: given `ptr: *mut
// Device<T>`, `raw_get_work` will return a `*mut Work<Device<T>, ID>` through
// `T::Data::raw_get_work` and given a `ptr: *mut Work<Device<T>, ID>`,
// `work_container_of` will return a `*mut Device<T>` through `container_of`.
unsafe impl<T, const ID: u64> HasWork<Device<T>, ID> for Device<T>
where
    T: drm::Driver,
    T::Data: HasWork<Device<T>, ID>,
{
    unsafe fn raw_get_work(ptr: *mut Self) -> *mut Work<Device<T>, ID> {
        // SAFETY: The caller promises that `ptr` points to a valid `Device<T>`.
        let data_ptr = unsafe { (&raw mut (*ptr).data).cast::<T::Data>() };

        // SAFETY: `data_ptr` is a valid pointer to `T::Data`.
        unsafe { T::Data::raw_get_work(data_ptr) }
    }

    unsafe fn work_container_of(ptr: *mut Work<Device<T>, ID>) -> *mut Self {
        // SAFETY: The caller promises that `ptr` points at a `Work` field in
        // `T::Data`.
        let data_ptr = unsafe { T::Data::work_container_of(ptr) }
            .cast::<UnsafeCell<MaybeUninit<T::Data>>>();

        // SAFETY: `T::Data` is stored as the `data` field in `Device<T>`.
        unsafe { crate::container_of!(data_ptr, Self, data) }
    }
}

// SAFETY: Our `HasWork<T, ID>` implementation returns a `work_struct` that is
// stored in the `work` field of a `delayed_work` with the same access rules as
// the `work_struct` owing to the bound on `T::Data: HasDelayedWork<Device<T>,
// ID>`, which requires that `T::Data::raw_get_work` return a `work_struct` that
// is inside a `delayed_work`.
unsafe impl<T, const ID: u64> HasDelayedWork<Device<T>, ID> for Device<T>
where
    T: drm::Driver,
    T::Data: HasDelayedWork<Device<T>, ID>,
{
}
