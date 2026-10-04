// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Completion work owned only by the M3 runtime.

use kernel::{prelude::*, sync::Arc, workqueue::{self, impl_has_work, new_work, Work, WorkItem}};
use crate::{driver, m3_drm::Shared};

#[pin_data]
pub(crate) struct Completion {
    shared: Shared,
    #[pin]
    work: Work<Completion>,
}
impl Completion {
    pub(crate) fn new(shared: Shared) -> Result<Arc<Self>> {
        Arc::pin_init(try_pin_init!(Self {
            shared,
            work <- new_work!("M3 completion"),
        }), GFP_KERNEL)
    }
}
impl_has_work! { impl HasWork<Completion> for Completion { self.work } }
impl WorkItem for Completion {
    type Pointer = Arc<Self>;
    fn run(owner: Arc<Self>) {
        if let Some(runtime) = Option::as_mut(&mut *owner.shared.lock()) {
            runtime.service_events();
        }
    }
}
pub(crate) fn queue(dev: &driver::AsahiDevice) {
    if let Some(work) = dev.completion.as_ref() {
        let _ = workqueue::system_highpri().enqueue(work.clone());
    }
}
