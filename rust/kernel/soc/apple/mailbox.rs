// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Support for Apple ASC Mailbox.
//!
//! C header: [`include/linux/soc/apple/mailbox.h`](../../../../include/linux/soc/apple/mailbox.h)

use crate::{
    bindings,
    device,
    error::{
        from_err_ptr,
        to_result, //
    },
    prelude::*,
    str::CStrExt,
    types::{
        ForeignOwnable,
        ScopeGuard, //
    }, //
};

use core::marker::PhantomData;

/// 96-bit message. What it means is up to the upper layer
pub type Message = bindings::apple_mbox_msg;

/// Mailbox receive callback
pub trait MailCallback {
    /// Callback context
    type Data: ForeignOwnable + Send + Sync;

    /// The actual callback. Called in an interrupt context.
    fn recv_message(data: <Self::Data as ForeignOwnable>::Borrowed<'_>, msg: Message);
}

/// Wrapper over `struct apple_mbox *`
#[repr(transparent)]
pub struct Mailbox<T: MailCallback> {
    mbox: *mut bindings::apple_mbox,
    _p: PhantomData<T>,
}

extern "C" fn mailbox_rx_callback<T: MailCallback>(
    _mbox: *mut bindings::apple_mbox,
    msg: Message,
    cookie: *mut crate::ffi::c_void,
) {
    // SAFETY: cookie came from a call to `into_foreign`
    T::recv_message(unsafe { T::Data::borrow(cookie.cast()) }, msg);
}

impl<T: MailCallback> Mailbox<T> {
    /// Creates a mailbox for the specified name.
    pub fn new_byname(
        dev: &device::Device,
        mbox_name: &'static CStr,
        data: T::Data,
    ) -> Result<Mailbox<T>> {
        let ptr: *mut crate::ffi::c_void = data.into_foreign().cast();
        let guard = ScopeGuard::new(|| {
            // SAFETY: `ptr` came from a previous call to `into_foreign`.
            unsafe { T::Data::from_foreign(ptr.cast()) };
        });
        // SAFETY: Just calling the c function, all values are valid.
        let mbox = unsafe {
            from_err_ptr(bindings::apple_mbox_get_byname(
                dev.as_raw(),
                mbox_name.as_char_ptr(),
            ))?
        };
        // SAFETY: mbox is a valid pointer
        unsafe {
            (*mbox).cookie = ptr;
            (*mbox).rx = Some(mailbox_rx_callback::<T>);
            if let Err(err) = to_result(bindings::apple_mbox_start(mbox)) {
                (*mbox).rx = None;
                (*mbox).cookie = core::ptr::null_mut();
                return Err(err);
            }
        }
        guard.dismiss();
        Ok(Mailbox {
            mbox,
            _p: PhantomData,
        })
    }
    /// The mailbox provider's device (DMA through its own IOMMU stream).
    pub fn device(&self) -> &device::Device {
        // SAFETY: `mbox` is a valid pointer for the life of `self` and its
        // `dev` is the provider's device, which outlives the client.
        unsafe { device::Device::from_raw((*self.mbox).dev) }
    }

    /// Sends the specified message
    pub fn send(&self, msg: Message, atomic: bool) -> Result<()> {
        // SAFETY: Calling the c function, `mbox` is a valid pointer
        to_result(unsafe { bindings::apple_mbox_send(self.mbox, msg, atomic) })
    }
}

impl<T: MailCallback> Drop for Mailbox<T> {
    fn drop(&mut self) {
        // SAFETY: mbox is a valid pointer
        unsafe { bindings::apple_mbox_stop(self.mbox) };
        // SAFETY: RX is synchronously disabled above, and `cookie` came from
        // `into_foreign`. Clear the provider-visible callback before freeing
        // its context so provider removal can never observe a dangling owner.
        unsafe {
            let cookie = (*self.mbox).cookie;
            (*self.mbox).rx = None;
            (*self.mbox).cookie = core::ptr::null_mut();
            T::Data::from_foreign(cookie.cast());
        }
    }
}

unsafe impl<T> Sync for Mailbox<T>
where
    T: MailCallback,
    T::Data: Sync,
{
}

unsafe impl<T> Send for Mailbox<T>
where
    T: MailCallback,
    T::Data: Send,
{
}
