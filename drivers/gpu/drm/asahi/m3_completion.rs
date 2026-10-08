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
        use crate::m3_runtime::Service;
        // Snapshots after a notification before sleeping for the next one: the completion
        // stamps can trail the firmware's event message.
        const TRAILING_POLLS: u32 = 4;
        let (mut trailing, mut waited_at) = (0, u64::MAX);
        loop {
            let (outcome, events) = {
                let mut guard = owner.shared.lock();
                let Some(runtime) = Option::as_mut(&mut *guard) else { return; };
                let outcome=runtime.service_job();
                let events=runtime.events();
                // Preserve the root-only CPU-overlap diagnostic: disabled waits retain
                // the runtime lock so VM/timestamp preparation cannot overlap the GPU.
                if let Service::Waiting(messages)=outcome {
                    if !events.cpu_overlap() {events.wait_past(messages);continue;}
                }
                (outcome,events)
            };
            match outcome {
                Service::Idle => break,
                Service::Progress => { trailing = 0; waited_at = u64::MAX; }
                Service::Waiting(messages) => {
                    if messages != waited_at { waited_at = messages; trailing = 0; }
                    if trailing < TRAILING_POLLS {
                        trailing += 1;
                        kernel::time::delay::fsleep(kernel::time::Delta::from_micros(5));
                    } else {
                        // Until the next firmware event, or a bounded 100 us resnapshot.
                        events.wait_past(messages);
                        trailing = 0;
                    }
                }
            }
        }
    }
}
pub(crate) fn queue(dev: &driver::AsahiDevice) {
    if let Some(work) = dev.completion.as_ref() {
        let _ = workqueue::system_highpri().enqueue(work.clone());
    }
}
