// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The once-per-boot module pin taken before the firmware is first touched.
//!
//! A started firmware session that is never freed (every backend's: G15, G16, G18, the M5
//! RAM-boot path, and G17 on M5) keeps RTKit and mailbox callbacks into this module, so the
//! module must stay loaded until reboot from the first firmware-visible write on.
//! `__module_get` alone does not ensure that: a deferred or asynchronous probe can run while
//! an ordinary rmmod is already past its reference check, and the module is then unloaded
//! under a running firmware. `try_module_get` fails once removal has begun, so the pin is
//! taken with it, before the session is marked retained and before anything
//! firmware-visible happens.

use core::sync::atomic::{AtomicBool, Ordering};

#[cfg(not(test))]
extern "C" {
    fn asahi_module_try_get() -> bool;
    fn asahi_module_put();
}

/// The module is being removed: nothing was pinned or marked, and nothing may be started.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Removing;

/// Pins the module once for the session `retained` tracks. `get` is
/// `try_module_get(THIS_MODULE)`, `put` is `module_put(THIS_MODULE)`.
///
/// Already retained: nothing more is taken. Otherwise the reference comes first; `retained` is
/// set only once it is held. A caller that loses the race to set it drops only its own
/// reference, so exactly one reference stays and it is never put.
pub(crate) fn retain(
    retained: &AtomicBool,
    get: impl FnOnce() -> bool,
    put: impl FnOnce(),
) -> Result<(), Removing> {
    if retained.load(Ordering::Acquire) {
        return Ok(());
    }
    if !get() {
        return Err(Removing);
    }
    if retained.swap(true, Ordering::AcqRel) {
        put();
    }
    Ok(())
}

/// Pins this module until reboot, once for the session `retained` tracks
/// (`try_module_get(THIS_MODULE)`, see [`retain`]). ENODEV, with nothing pinned or marked, once
/// the module's removal has begun.
#[cfg(not(test))]
pub(crate) fn retain_module(retained: &AtomicBool) -> kernel::error::Result {
    retain(
        retained,
        // SAFETY: Takes a reference to this module unless its removal has begun.
        || unsafe { asahi_module_try_get() },
        // SAFETY: Drops only the reference this call took; the winner's is never put.
        || unsafe { asahi_module_put() },
    )
    .map_err(|_| kernel::error::code::ENODEV)
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::sync::atomic::AtomicI32;
    use std::sync::Arc;

    #[test]
    fn removal_fails_closed_with_nothing_pinned_or_marked() {
        let retained = AtomicBool::new(false);
        let refs = AtomicI32::new(1); // base reference only: rmmod has released it
        let put = || panic!("nothing to put");
        assert_eq!(retain(&retained, || false, put), Err(Removing));
        assert!(!retained.load(Ordering::Acquire), "a refused pin must not mark the session");
        assert_eq!(refs.load(Ordering::Acquire), 1);
        // A later live start can still pin: the refusal left no state behind.
        let get = || {
            refs.fetch_add(1, Ordering::AcqRel);
            true
        };
        assert_eq!(retain(&retained, get, || panic!("winner keeps its reference")), Ok(()));
        assert!(retained.load(Ordering::Acquire));
        assert_eq!(refs.load(Ordering::Acquire), 2);
    }

    #[test]
    fn first_pin_is_kept_and_later_calls_take_nothing() {
        let retained = AtomicBool::new(false);
        let refs = AtomicI32::new(1);
        let get = || {
            refs.fetch_add(1, Ordering::AcqRel);
            true
        };
        assert_eq!(retain(&retained, get, || panic!("winner keeps its reference")), Ok(()));
        // The post-start call in Gpu::retain and any later one: no new reference, no put.
        for _ in 0..3 {
            assert_eq!(
                retain(&retained, || panic!("already retained"), || panic!("nothing to put")),
                Ok(())
            );
        }
        assert_eq!(refs.load(Ordering::Acquire), 2);
    }

    #[test]
    fn a_lost_race_drops_only_its_own_reference() {
        let retained = AtomicBool::new(false);
        let refs = AtomicI32::new(2); // base + the reference the winner already holds
        let puts = AtomicI32::new(0);
        // The winner marks the session between this caller's check and its swap.
        let get = || {
            refs.fetch_add(1, Ordering::AcqRel);
            retained.store(true, Ordering::Release);
            true
        };
        let put = || {
            refs.fetch_sub(1, Ordering::AcqRel);
            puts.fetch_add(1, Ordering::AcqRel);
        };
        assert_eq!(retain(&retained, get, put), Ok(()));
        assert_eq!(puts.load(Ordering::Acquire), 1);
        assert_eq!(refs.load(Ordering::Acquire), 2);
    }

    #[test]
    fn concurrent_pins_leave_exactly_one_reference() {
        for _ in 0..500 {
            let retained = Arc::new(AtomicBool::new(false));
            let refs = Arc::new(AtomicI32::new(1));
            let start = Arc::new(std::sync::Barrier::new(8));
            let threads: Vec<_> = (0..8)
                .map(|_| {
                    let (retained, refs, start) = (retained.clone(), refs.clone(), start.clone());
                    std::thread::spawn(move || {
                        let (a, b) = (refs.clone(), refs.clone());
                        start.wait();
                        retain(
                            &retained,
                            move || {
                                a.fetch_add(1, Ordering::AcqRel);
                                true
                            },
                            move || {
                                b.fetch_sub(1, Ordering::AcqRel);
                            },
                        )
                    })
                })
                .collect();
            for t in threads {
                assert_eq!(t.join().unwrap(), Ok(()));
            }
            assert!(retained.load(Ordering::Acquire));
            assert_eq!(refs.load(Ordering::Acquire), 2, "exactly one kept reference");
        }
    }
}
