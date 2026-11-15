// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The internal microphones' lid jack, shared by the AOP audio cards.
//!
//! Closing the lid of an Apple silicon MacBook disconnects its internal
//! microphones in hardware.  They then read back as silence, or on some
//! paths as a constant offset, rather than going away.  [`MicLid`] follows
//! the lid switches through the input core and reports an "Internal Mic"
//! jack on the card: unplugged while a lid is closed, so userspace stops
//! offering the microphones.  A machine without a lid switch keeps the jack
//! plugged.
//!
//! Copyright (C) 2026 The Aurora Silicon Contributors

use core::{mem, ptr};

use kernel::{
    bindings,
    device,
    prelude::*,
    sync::{
        atomic::{
            Atomic,
            AtomicFlag,
            Relaxed, //
        },
        Arc,
        ArcBorrow, //
    },
    types::{ForeignOwnable, Opaque},
    workqueue::{
        self,
        impl_has_work,
        new_work,
        Work,
        WorkItem, //
    }, //
};

/// The lid switches seen through the input core.  The internal microphones
/// are disconnected while any of them reads closed.
///
/// Each switch's flag (in its [`LidHandle`]) and the count change together:
/// every writer of one switch runs under that input device's event lock (the
/// event callback, and the resynchronizations in `lid_resync`), or after the
/// device stopped delivering events to it (`lid_disconnect`).  The count is
/// the only state shared between switches, and it is a single atomic word.
struct LidState {
    /// Lid switches currently closed.
    closed: Atomic<u32>,
}

impl LidState {
    const fn new() -> Self {
        LidState {
            closed: Atomic::new(0),
        }
    }

    /// Records that the switch whose state is `switch` now reads `closed`.
    /// Returns whether that changed anything; a repeated state does not.
    fn set(&self, switch: &AtomicFlag, closed: bool) -> bool {
        if switch.xchg(closed, Relaxed) == closed {
            return false;
        }
        if closed {
            self.closed.add(1, Relaxed);
        } else {
            let _ = self.closed.fetch_sub(1, Relaxed);
        }
        true
    }

    fn mic_cut(&self) -> bool {
        self.closed.load(Relaxed) != 0
    }
}

/// The "Internal Mic" jack status for the lid state.
fn mic_jack_status(mic_cut: bool) -> i32 {
    if mic_cut {
        0
    } else {
        bindings::snd_jack_types_SND_JACK_MICROPHONE as i32
    }
}

/// The input handler that follows the lid switches.  Only its address is
/// shared: the input core owns its contents once it is registered.
struct LidHandler(Opaque<bindings::input_handler>);

// SAFETY: the handler is filled in before registration and only the input
// core, which serializes its own access, touches it after that.
unsafe impl Send for LidHandler {}
// SAFETY: as for `Send`.
unsafe impl Sync for LidHandler {}

/// One lid switch the handler is connected to.  `handle` comes first so the
/// input core's handle pointer is also a pointer to this.
#[repr(C)]
struct LidHandle {
    handle: Opaque<bindings::input_handle>,
    /// The switch reads closed, as recorded in [`LidState`].
    closed: AtomicFlag,
    /// Readings are recorded: the handle is open.
    open: AtomicFlag,
}

impl LidHandle {
    /// Records a reading of this switch, once the handle is open.
    ///
    /// `input_register_handle()` calls `lid_start()` before `lid_connect()`
    /// knows whether opening the handle succeeds, and a handle that fails to
    /// open is freed without `lid_disconnect()`: a reading recorded then
    /// could never be taken back.  `lid_connect()` reads the switch again
    /// once the handle is open.
    fn record(&self, lid: &LidState, closed: bool) -> bool {
        self.open.load(Relaxed) && lid.set(&self.closed, closed)
    }

    /// Records the switch as read under its device's event lock.  With
    /// `activate`, readings are enabled first: `lid_connect()` enables and
    /// reads the switch in one step after opening the handle, so an event
    /// delivered between the open and this reading, which `record()` ignored,
    /// is caught up with.
    fn sync(&self, lid: &LidState, closed: bool, activate: bool) -> bool {
        if activate {
            self.open.store(true, Relaxed);
        }
        self.record(lid, closed)
    }

    /// The switch goes away: it no longer disconnects the microphones.
    fn forget(&self, lid: &LidState) -> bool {
        self.open.store(false, Relaxed);
        lid.set(&self.closed, false)
    }
}

/// The "Internal Mic" jack of one card and the lid switches it follows.
#[pin_data]
pub(crate) struct MicLid {
    /// The lid switches.
    lid: LidState,
    handler: LidHandler,
    /// The card's jack (null outside the card's lifetime), reported from
    /// `work` because the input core delivers switch events in atomic
    /// context.
    jack: Atomic<*mut bindings::snd_jack>,
    registered: AtomicFlag,
    #[pin]
    work: Work<MicLid>,
}

// SAFETY: the jack pointer is only dereferenced by the report work, which
// `detach` flushes before the card that owns the jack goes away; the rest
// is atomics and the input core's own handler.
unsafe impl Send for MicLid {}
// SAFETY: as for `Send`.
unsafe impl Sync for MicLid {}

impl_has_work! {
    impl HasWork<Self> for MicLid { self.work }
}

/// The jack report after a lid change: it takes mutexes.
impl WorkItem for MicLid {
    type Pointer = Arc<Self>;

    fn run(this: Arc<Self>) {
        this.report();
    }
}

impl MicLid {
    pub(crate) fn new() -> Result<Arc<MicLid>> {
        Arc::pin_init(
            pin_init!(MicLid {
                lid: LidState::new(),
                handler: LidHandler(Opaque::zeroed()),
                jack: Atomic::new(ptr::null_mut()),
                registered: AtomicFlag::new(false),
                work <- new_work!("MicLid::work"),
            }),
            GFP_KERNEL,
        )
    }

    /// Gives `card` (created, not registered yet) its "Internal Mic" jack and
    /// starts following the lid switches: the input core connects the
    /// handler to every device with one, now and as they come.  The jack
    /// reads the state before the card is registered.  On failure the caller
    /// still calls [`MicLid::detach`].
    pub(crate) fn attach(
        self: &Arc<Self>,
        dev: &device::Device,
        card: *mut bindings::snd_card,
    ) -> Result {
        let mut jack = ptr::null_mut();
        // SAFETY: the caller passes a card created by `snd_card_new` and not
        // registered yet; the jack becomes one of its devices and is freed
        // with it.
        let ret = unsafe {
            bindings::snd_jack_new(
                card,
                c"Internal Mic".as_char_ptr(),
                bindings::snd_jack_types_SND_JACK_MICROPHONE as i32,
                &mut jack,
                true,
                // No input device of its own: the lid it follows already
                // is one, and the control is what userspace reads.
                true,
            )
        };
        if ret < 0 {
            dev_err!(dev, "unable to create the microphone jack\n");
            return Err(Error::from_errno(ret));
        }
        self.jack.store(jack, Relaxed);

        let handler = self.handler.0.get();
        // SAFETY: the handler lives in `self`, which the `Arc` leaked into
        // it keeps allocated until `detach` unregisters it, and it is not
        // registered yet.
        unsafe {
            (*handler).private = self.clone().into_foreign().cast();
            (*handler).event = Some(lid_event);
            (*handler).connect = Some(lid_connect);
            (*handler).disconnect = Some(lid_disconnect);
            (*handler).start = Some(lid_start);
            (*handler).passive_observer = true;
            (*handler).name = c"aop-audio-lid".as_char_ptr();
            (*handler).id_table = LID_IDS.as_ptr();
        }
        // SAFETY: the handler is filled in above.
        let ret = unsafe { bindings::input_register_handler(handler) };
        if ret < 0 {
            // SAFETY: the `Arc` leaked above; the handler never registered.
            drop(unsafe { Arc::<MicLid>::from_foreign((*handler).private.cast()) });
            dev_err!(dev, "unable to follow the lid switch: {}\n", ret);
            return Err(Error::from_errno(ret));
        }
        self.registered.store(true, Relaxed);
        // The state as the card is registered, from the only reporter.
        self.as_arc_borrow().queue_report();
        self.flush();
        Ok(())
    }

    /// Stops following the lid and lets the last report finish.  Call it
    /// before the card, and the jack with it, goes away.  Idempotent.
    pub(crate) fn detach(&self) {
        if self.registered.xchg(false, Relaxed) {
            let handler = self.handler.0.get();
            // SAFETY: registered in `attach`; unregistering disconnects
            // every handle, after which no callback runs.
            unsafe { bindings::input_unregister_handler(handler) };
            // SAFETY: the `Arc` leaked into the handler, unused from here on.
            drop(unsafe { Arc::<MicLid>::from_foreign((*handler).private.cast()) });
        }
        // No lid change can queue a report any more.
        self.flush();
        self.jack.store(ptr::null_mut(), Relaxed);
    }

    /// A lid switch now reads `closed`: queue the report on a change.
    fn set(self: ArcBorrow<'_, Self>, switch: &LidHandle, closed: bool) {
        if switch.record(&self.lid, closed) {
            self.queue_report();
        }
    }

    /// A lid switch went away.
    fn forget(self: ArcBorrow<'_, Self>, switch: &LidHandle) {
        if switch.forget(&self.lid) {
            self.queue_report();
        }
    }

    /// The work is the only reporter, so reports reach ALSA in the order of
    /// the states they read: a queued or running report that read an older
    /// state is followed by another run.
    fn queue_report(self: ArcBorrow<'_, Self>) {
        // Err = already queued: the pending report reads the new state.
        let _ = workqueue::system_dfl().enqueue(Arc::from(self));
    }

    fn report(&self) {
        let jack = self.jack.load(Relaxed);
        if !jack.is_null() {
            // SAFETY: the jack belongs to the card, which goes away only
            // after `detach` cleared `jack` and flushed this work.
            unsafe { bindings::snd_jack_report(jack, mic_jack_status(self.lid.mic_cut())) };
        }
    }

    fn flush(&self) {
        // SAFETY: the work item lives in `self`; flushing waits for a queued
        // report to finish.
        unsafe { bindings::flush_work(Work::raw_get(&self.work)) };
    }
}

// ---- the lid switch handler: an input handler on every device with
// SW_LID, observing the events of the lid switch userspace already
// listens to ----

/// Devices with a lid switch, and the terminating entry.
static LID_IDS: [bindings::input_device_id; 2] = {
    // SAFETY: the ID is plain integers, for which zero is valid.
    let mut lid: bindings::input_device_id = unsafe { mem::zeroed() };
    lid.flags = (bindings::INPUT_DEVICE_ID_MATCH_EVBIT | bindings::INPUT_DEVICE_ID_MATCH_SWBIT)
        as bindings::kernel_ulong_t;
    lid.evbit[0] = 1 << bindings::EV_SW;
    lid.swbit[0] = 1 << bindings::SW_LID;
    // SAFETY: as above.
    [lid, unsafe { mem::zeroed() }]
};

/// The driver data behind an input handle of the lid handler.
///
/// # Safety
///
/// `handle` must be a handle `lid_connect` registered and the handler must
/// still be registered, so the `Arc` leaked into its private data is live.
unsafe fn lid_data<'a>(handle: *mut bindings::input_handle) -> ArcBorrow<'a, MicLid> {
    // SAFETY: by the caller's contract.
    unsafe { Arc::<MicLid>::borrow((*(*handle).handler).private.cast()) }
}

/// Reads the switch from the device's state and records it, under the
/// device's event lock: events are delivered under that lock after the input
/// core updated the state, so no event can be overtaken by an older reading.
/// `activate` enables readings first (see [`LidHandle::sync`]).
///
/// # Safety
///
/// `handle` must be a handle `lid_connect` registered, with the handler
/// still registered.
unsafe fn lid_resync(handle: *mut bindings::input_handle, activate: bool) {
    // SAFETY: the handle and its device are live while it is registered.
    let dev = unsafe { (*handle).dev };
    // SAFETY: `dev` is live; its event lock is a spinlock taken with
    // interrupts saved, as the input core takes it.
    let lock = unsafe { &raw mut (*dev).event_lock };
    // SAFETY: as above.
    let flags = unsafe { bindings::spin_lock_irqsave(lock) };
    // SAFETY: the switch bitmap is written under the event lock, held here.
    let closed = unsafe { (*dev).sw[0] } & (1 << bindings::SW_LID) != 0;
    // SAFETY: the handle is the first field of a `LidHandle`, live while it
    // is registered, and the handler is registered.
    let (lid, data) = unsafe { (&*handle.cast::<LidHandle>(), lid_data(handle)) };
    if lid.sync(&data.lid, closed, activate) {
        data.queue_report();
    }
    // SAFETY: taken above with these flags.
    unsafe { bindings::spin_unlock_irqrestore(lock, flags) };
}

/// Called after the handle is registered, and again when a client that
/// grabbed the device releases it: events delivered only to the grabbing
/// client in between never reached `lid_event`.
///
/// # Safety
///
/// Called by the input core with a handle `lid_connect` registered.
unsafe extern "C" fn lid_start(handle: *mut bindings::input_handle) {
    // SAFETY: by the caller's contract; the input core calls this without
    // the event lock.
    unsafe { lid_resync(handle, false) };
}

/// # Safety
///
/// Called by the input core, in atomic context with the device's event lock
/// held, with a handle `lid_connect` registered.
unsafe extern "C" fn lid_event(
    handle: *mut bindings::input_handle,
    type_: u32,
    code: u32,
    value: i32,
) {
    if type_ != bindings::EV_SW || code != bindings::SW_LID {
        return;
    }
    // SAFETY: the handle is the first field of a `LidHandle`, live until
    // `lid_disconnect`, and the handler is registered.
    let (lid, data) = unsafe { (&*handle.cast::<LidHandle>(), lid_data(handle)) };
    data.set(lid, value != 0);
}

/// # Safety
///
/// Called by the input core with the registered lid handler and a device
/// that matched `LID_IDS`.
unsafe extern "C" fn lid_connect(
    handler: *mut bindings::input_handler,
    dev: *mut bindings::input_dev,
    _id: *const bindings::input_device_id,
) -> i32 {
    let lid = match KBox::new(
        LidHandle {
            handle: Opaque::zeroed(),
            closed: AtomicFlag::new(false),
            open: AtomicFlag::new(false),
        },
        GFP_KERNEL,
    ) {
        Ok(lid) => KBox::into_raw(lid),
        Err(_) => return ENOMEM.to_errno(),
    };
    // SAFETY: `lid` was allocated above; `dev` and `handler` are live for
    // the handle's lifetime, which ends in `lid_disconnect`.
    let handle = unsafe { (*lid).handle.get() };
    // SAFETY: as above; the handle is not registered yet.
    unsafe {
        (*handle).dev = dev;
        (*handle).handler = handler;
        (*handle).name = c"aop-audio-lid".as_char_ptr();
    }
    // SAFETY: the handle is filled in and stays allocated until it is
    // unregistered.
    let ret = unsafe { bindings::input_register_handle(handle) };
    if ret < 0 {
        // SAFETY: allocated above and never registered.
        drop(unsafe { KBox::from_raw(lid) });
        return ret;
    }
    // SAFETY: the handle is registered.
    let ret = unsafe { bindings::input_open_device(handle) };
    if ret < 0 {
        // Nothing was recorded for the handle (`LidHandle::record`).
        // SAFETY: registered above, not open; nothing else refers to it.
        unsafe {
            bindings::input_unregister_handle(handle);
            drop(KBox::from_raw(lid));
        }
        return ret;
    }
    // Events reach the handle from here on: enable readings and read the
    // switch as it is now, in one step under the event lock.
    // SAFETY: the handle is registered and open, and so is the handler.
    unsafe { lid_resync(handle, true) };
    0
}

/// # Safety
///
/// Called by the input core with a handle `lid_connect` registered, while
/// the handler is still registered.
unsafe extern "C" fn lid_disconnect(handle: *mut bindings::input_handle) {
    // SAFETY: the handle is registered and open; after this no event
    // reaches it.
    unsafe {
        bindings::input_close_device(handle);
        bindings::input_unregister_handle(handle);
    }
    // SAFETY: the handle is the first field of the `LidHandle` allocated by
    // `lid_connect`, and the handler is still registered.
    let (lid, data) = unsafe { (KBox::from_raw(handle.cast::<LidHandle>()), lid_data(handle)) };
    data.forget(&lid);
}

/// The lid bookkeeping behind the microphone jack.
#[kunit_tests(snd_soc_apple_aop_lid)]
mod lid_tests {
    use super::*;

    fn handle() -> LidHandle {
        LidHandle {
            handle: Opaque::zeroed(),
            closed: AtomicFlag::new(false),
            open: AtomicFlag::new(false),
        }
    }

    fn opened() -> LidHandle {
        let h = handle();
        h.open.store(true, Relaxed);
        h
    }

    #[test]
    fn a_closed_lid_unplugs_the_microphone_jack() {
        let lid = LidState::new();
        let switch = opened();
        assert!(!lid.mic_cut());
        assert_ne!(mic_jack_status(false), 0);

        assert!(switch.record(&lid, true));
        assert!(lid.mic_cut());
        assert_eq!(mic_jack_status(lid.mic_cut()), 0);

        assert!(switch.record(&lid, false));
        assert!(!lid.mic_cut());
    }

    #[test]
    fn a_repeated_switch_state_changes_nothing() {
        let lid = LidState::new();
        let switch = opened();
        // the reading at connect, then the same state again from an event
        assert!(!switch.record(&lid, false));
        assert!(switch.record(&lid, true));
        assert!(!switch.record(&lid, true));
        assert!(lid.mic_cut());
        assert!(switch.record(&lid, false));
        assert!(!switch.record(&lid, false));
        assert!(!lid.mic_cut());
    }

    #[test]
    fn any_closed_lid_switch_cuts_the_microphones() {
        let lid = LidState::new();
        let (a, b) = (opened(), opened());
        a.record(&lid, true);
        b.record(&lid, true);
        a.record(&lid, false);
        assert!(lid.mic_cut());
        b.record(&lid, false);
        assert!(!lid.mic_cut());
    }

    #[test]
    fn a_lid_switch_that_goes_away_closed_restores_the_microphones() {
        let lid = LidState::new();
        let switch = opened();
        switch.record(&lid, true);
        assert!(switch.forget(&lid));
        assert!(!lid.mic_cut());
        assert_eq!(lid.closed.load(Relaxed), 0);
        // nothing reaches a forgotten handle any more
        assert!(!switch.record(&lid, true));
        assert!(!lid.mic_cut());
    }

    /// Re-review finding 2: `lid_start()` runs from `input_register_handle()`
    /// before the open; a handle whose open then fails is freed without
    /// `lid_disconnect()`, so its closed reading must never have counted.  A
    /// later handle for the same device that opens counts normally.
    #[test]
    fn a_failed_open_leaves_no_closed_switch_behind() {
        let lid = LidState::new();
        let failed = handle();
        // lid_start() during input_register_handle(), switch closed
        assert!(!failed.record(&lid, true));
        // input_open_device() fails: unregistered and freed, no disconnect
        drop(failed);
        assert!(!lid.mic_cut());
        assert_eq!(lid.closed.load(Relaxed), 0);

        let attached = handle();
        // lid_start() during registration, before the open
        assert!(!attached.sync(&lid, true, false));
        // opened: lid_connect() enables and reads the switch in one step
        assert!(attached.sync(&lid, true, true));
        assert!(lid.mic_cut());
        assert!(attached.forget(&lid));
        assert!(!lid.mic_cut());
    }

    /// Review of v3, minor 3: an initially closed switch must count once the
    /// handle is open.  `lid_connect()` enables readings and reads the switch
    /// in the same step, in that order; reading first would drop the reading
    /// and leave the jack plugged until the next lid event.
    #[test]
    fn a_connect_enables_readings_before_reading_the_switch() {
        let lid = LidState::new();
        let switch = handle();
        assert!(switch.sync(&lid, true, true));
        assert!(lid.mic_cut());
        // the start() from a grab release reads without enabling anything
        let other = handle();
        assert!(!other.sync(&lid, true, false));
        assert_eq!(lid.closed.load(Relaxed), 1);
    }

    /// Events delivered only to a client that grabbed the device never reach
    /// the handler; the resynchronization on release reads the device's
    /// switch and repairs the state, in both directions.
    #[test]
    fn a_resync_after_missed_events_repairs_the_state() {
        let lid = LidState::new();
        let switch = opened();
        switch.record(&lid, true);
        // grabbed: the lid opens unseen; on release the bitmap reads open
        assert!(switch.record(&lid, false));
        assert!(!lid.mic_cut());
        // grabbed: the lid closes unseen; on release the bitmap reads closed
        assert!(switch.record(&lid, true));
        assert!(lid.mic_cut());
    }
}
