// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Schedules CPU capacity updates after completed firmware event passes. The
//! worker owns frequency requests independently of the device publication lock.

use crate::g17::{Shared, FEED_WORK};
use kernel::{
    impl_has_work,
    sync::Arc,
    workqueue::{self, WorkItem},
};

impl_has_work! {
    impl HasWork<Shared, FEED_WORK> for Shared { self.feed_work }
}

impl WorkItem<FEED_WORK> for Shared {
    type Pointer = Arc<Shared>;

    fn run(this: Arc<Shared>) {
        this.feed.run();
    }
}

impl Shared {
    /// The completion callbacks have finished outside the device mutex before
    /// the event worker hands the detector's request to the actuator.
    pub(in crate::g17) fn queue_feed(self: &Arc<Self>) {
        if self.feed.take_pending() {
            let _ = workqueue::system_unbound().enqueue::<Arc<Self>, FEED_WORK>(self.clone());
        }
    }
}
