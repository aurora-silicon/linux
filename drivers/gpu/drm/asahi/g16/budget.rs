// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright (C) 2026 Eryk Wieliczko

//! Bound prepared render epochs, including jobs blocked on unsignalled input fences.
//! Wait for deferred unmap to return credits rather than surfacing transient EBUSY
//! as a lost Vulkan device. The ten-second wait is an administrative bound.

use core::sync::atomic::{AtomicU64, Ordering};
use kernel::{
    new_condvar, new_mutex,
    prelude::*,
    sync::{Arc, CondVar, CondVarTimeoutResult, Mutex},
};

static WAITS: AtomicU64 = AtomicU64::new(0);
pub(crate) fn waits() -> u64 {
    WAITS.load(Ordering::Relaxed)
}

pub(crate) const LIMIT: u32 = super::hw::sizes::QUEUE_CAP;
#[pin_data]
pub(crate) struct Budget {
    #[pin]
    used: Mutex<u32>,
    #[pin]
    available: CondVar,
}
impl Budget {
    pub(crate) fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                used <- new_mutex!(0),
                available <- new_condvar!(),
            }),
            GFP_KERNEL,
        )
    }
    fn charge(used: &mut u32, count: u32) -> Result {
        *used = used
            .checked_add(count)
            .filter(|&next| next <= LIMIT)
            .ok_or(EBUSY)?;
        Ok(())
    }
    pub(crate) fn reserve(budget: &Arc<Self>, count: u32) -> Result<Reservation> {
        if count > LIMIT {
            return Err(E2BIG);
        }
        let mut used = budget.used.lock();
        let mut remaining = kernel::time::msecs_to_jiffies(10_000);
        loop {
            if Self::charge(&mut used, count).is_ok() {
                break;
            }
            WAITS.fetch_add(1, Ordering::Relaxed);
            match budget
                .available
                .wait_interruptible_timeout(&mut used, remaining)
            {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { .. } => return Err(ERESTARTSYS),
                CondVarTimeoutResult::Timeout => {
                    // Completion may race the timeout while the mutex is reacquired.
                    Self::charge(&mut used, count)?;
                    break;
                }
            }
        }
        Ok(Reservation {
            budget: budget.clone(),
            remaining: count,
        })
    }
    #[cfg(test)]
    fn try_reserve(budget: &Arc<Self>, count: u32) -> Result<Reservation> {
        Self::charge(&mut budget.used.lock(), count)?;
        Ok(Reservation {
            budget: budget.clone(),
            remaining: count,
        })
    }
    fn release(&self, count: u32) {
        *self.used.lock() -= count;
        self.available.notify_all();
    }
}
pub(crate) struct Reservation {
    budget: Arc<Budget>,
    remaining: u32,
}
impl Reservation {
    pub(crate) fn take(&mut self) -> Result<Permit> {
        if self.remaining == 0 {
            return Err(EINVAL);
        }
        self.remaining -= 1;
        Ok(Permit(self.budget.clone()))
    }
}
impl Drop for Reservation {
    fn drop(&mut self) {
        self.budget.release(self.remaining);
    }
}
pub(crate) struct Permit(Arc<Budget>);
impl Drop for Permit {
    fn drop(&mut self) {
        self.0.release(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn full_budget_refuses_without_mutation_and_rollback_returns_unused_credits() {
        let budget = Budget::new().unwrap();
        let mut reservation = Budget::try_reserve(&budget, LIMIT).unwrap();
        assert!(matches!(Budget::try_reserve(&budget, 1), Err(EBUSY)));
        let permit = reservation.take().unwrap();
        drop(reservation);
        assert_eq!(*budget.used.lock(), 1);
        assert!(Budget::try_reserve(&budget, LIMIT - 1).is_ok());
        drop(permit);
        assert_eq!(*budget.used.lock(), 0);
    }
    #[test]
    fn oversized_and_overflowed_batches_never_wrap_the_budget() {
        let budget = Budget::new().unwrap();
        assert!(matches!(
            Budget::try_reserve(&budget, LIMIT + 1),
            Err(EBUSY)
        ));
        assert!(matches!(Budget::try_reserve(&budget, u32::MAX), Err(EBUSY)));
        let mut reservation = Budget::try_reserve(&budget, 1).unwrap();
        let permit = reservation.take().unwrap();
        assert!(matches!(reservation.take(), Err(EINVAL)));
        assert!(matches!(Budget::try_reserve(&budget, u32::MAX), Err(EBUSY)));
        drop((reservation, permit));
        assert_eq!(*budget.used.lock(), 0);
    }
}
