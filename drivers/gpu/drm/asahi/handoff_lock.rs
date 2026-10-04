// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounded Dekker acquisition. A timeout withdraws AP interest; it never
//! grants ownership of firmware-shared memory.

use core::sync::atomic::{AtomicU8, AtomicU32, Ordering};

pub(crate) fn acquire(
    ap: &AtomicU8,
    fw: &AtomicU8,
    turn: &AtomicU32,
    mut expired: impl FnMut() -> bool,
    mut relax: impl FnMut(),
) -> bool {
    ap.store(1, Ordering::SeqCst);
    while fw.load(Ordering::SeqCst) != 0 {
        if expired() {
            ap.store(0, Ordering::SeqCst);
            return false;
        }
        if turn.load(Ordering::SeqCst) != 0 {
            ap.store(0, Ordering::SeqCst);
            while turn.load(Ordering::SeqCst) != 0 {
                if expired() { return false; }
                relax();
            }
            ap.store(1, Ordering::SeqCst);
        }
        relax();
    }
    true
}

pub(crate) fn release(ap: &AtomicU8, turn: &AtomicU32) {
    turn.store(1, Ordering::SeqCst);
    ap.store(0, Ordering::SeqCst);
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::cell::Cell;

    #[test]
    fn uncontended_acquisition_and_release() {
        let ap = AtomicU8::new(0);
        let fw = AtomicU8::new(0);
        let turn = AtomicU32::new(0);
        assert!(acquire(&ap, &fw, &turn, || false, || panic!("must not wait")));
        assert_eq!(ap.load(Ordering::Relaxed), 1);
        release(&ap, &turn);
        assert_eq!(ap.load(Ordering::Relaxed), 0);
        assert_eq!(turn.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn both_stuck_peer_paths_withdraw_without_ownership() {
        for priority in [0, 1] {
            let ap = AtomicU8::new(0);
            let fw = AtomicU8::new(1);
            let turn = AtomicU32::new(priority);
            let polls = Cell::new(0);
            assert!(!acquire(&ap, &fw, &turn,
                || { polls.set(polls.get() + 1); polls.get() == 4 }, || {}));
            assert_eq!(ap.load(Ordering::Relaxed), 0);
            assert_eq!(fw.load(Ordering::Relaxed), 1);
            assert_eq!(turn.load(Ordering::Relaxed), priority);
        }
    }

    #[test]
    fn yields_to_peer_then_acquires() {
        let ap = AtomicU8::new(0);
        let fw = AtomicU8::new(1);
        let turn = AtomicU32::new(1);
        assert!(acquire(&ap, &fw, &turn, || false, || {
            if turn.load(Ordering::Relaxed) != 0 {
                assert_eq!(ap.load(Ordering::Relaxed), 0);
                fw.store(0, Ordering::SeqCst);
                turn.store(0, Ordering::SeqCst);
            }
        }));
        assert_eq!(ap.load(Ordering::Relaxed), 1);
    }
}
