// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounded isolation for a compute watchdog blocked behind another VM's older kick.
//! Publication order is evidence of a possible blocker, not proof of a firmware
//! dependency. Renewals are bounded; the oldest unfinished queue is never excused.

pub(crate) const MAX_RENEWALS: u8 = 8;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Head {
    pub(crate) vm: usize,
    pub(crate) published: u64,
    pub(crate) finished: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Decision {
    Own,
    Defer,
    Spare,
}

pub(crate) fn classify(own: Head, renewals: u8, others: impl Iterator<Item = Head>) -> Decision {
    if own.finished || !others.into_iter().any(|other| {
        other.vm != own.vm && !other.finished && other.published < own.published
    }) {
        Decision::Own
    } else if renewals < MAX_RENEWALS {
        Decision::Defer
    } else {
        Decision::Spare
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn head(vm: usize, published: u64, finished: bool) -> Head {
        Head { vm, published, finished }
    }
    #[test]
    fn earlier_other_vm_gets_bounded_grace() {
        let own = head(5, 101, false);
        for count in 0..MAX_RENEWALS {
            assert_eq!(classify(own, count, [head(7, 100, false)].into_iter()), Decision::Defer);
        }
        assert_eq!(classify(own, MAX_RENEWALS, [head(7, 100, false)].into_iter()), Decision::Spare);
        assert_eq!(classify(own, 2, [].into_iter()), Decision::Own);
    }
    #[test]
    fn oldest_unfinished_queue_is_never_excused() {
        assert_eq!(classify(head(7, 100, false), 0, [head(5, 101, false)].into_iter()), Decision::Own);
    }
    #[test]
    fn same_vm_finished_and_later_work_do_not_block() {
        let own = head(5, 101, false);
        assert_eq!(classify(own, 0, [head(5, 100, false), head(7, 100, true), head(7, 101, false), head(7, 102, false)].into_iter()), Decision::Own);
        assert_eq!(classify(head(5, 101, true), 0, [head(7, 100, false)].into_iter()), Decision::Own);
    }
}
