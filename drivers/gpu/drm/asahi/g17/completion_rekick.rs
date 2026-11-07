// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Test-only 25G83 completion-to-dispatch re-kick policy.
//!
//! The production arm does nothing. The other arms let a controlled A/B boot
//! send the ordinary work notification after the host has drained a completion
//! event, when the completed queue still retains published work. This uses no
//! new firmware field or undocumented interrupt register.

use crate::{g17::fw::variant::InitGraph, module_parameters};
use kernel::prelude::kunit_tests;

/// Work pipe whose completion can request a re-kick.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Engine {
    Compute,
    Render,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum Arm {
    /// Existing behavior, unchanged.
    A,
    /// Re-kick compute after a compute completion drain.
    Compute,
    /// Re-kick render after a fragment completion drain.
    Render,
    /// Enable both independent arms.
    Both,
    /// Re-kick compute when a completed QID is named by the front type-4 wait.
    Waiter,
}

impl Arm {
    const fn from_param(value: u8) -> Self {
        match value {
            1 => Self::Compute,
            2 => Self::Render,
            3 => Self::Both,
            4 => Self::Waiter,
            _ => Self::A,
        }
    }

    const fn covers_waiter(self) -> bool {
        matches!(self, Self::Waiter)
    }

    const fn covers(self, engine: Engine) -> bool {
        matches!(
            (self, engine),
            (Self::Compute, Engine::Compute) | (Self::Render, Engine::Render) | (Self::Both, _)
        )
    }
}

const fn decision(
    value: u8,
    graph: InitGraph,
    engine: Engine,
    completion_seen: bool,
    work_queued: bool,
) -> bool {
    matches!(graph, InitGraph::V25G83)
        && Arm::from_param(value).covers(engine)
        && completion_seen
        && work_queued
}

/// Whether the selected boot arm permits this post-drain re-kick. Rejects
/// firmware graphs for which the ordering issue and work notification have
/// not been qualified.
pub(crate) fn selected_for(
    graph: InitGraph,
    engine: Engine,
    completion_seen: bool,
    work_queued: bool,
) -> bool {
    decision(
        module_parameters::g16_completion_rekick_arm.value(),
        graph,
        engine,
        completion_seen,
        work_queued,
    )
}

const fn waiter_decision(
    value: u8,
    graph: InitGraph,
    wait_at_front: bool,
    producer_completed: bool,
) -> bool {
    matches!(graph, InitGraph::V25G83)
        && Arm::from_param(value).covers_waiter()
        && wait_at_front
        && producer_completed
}

/// Whether the selected boot arm permits a work notification for a compute
/// queue whose front item is a type-4 wait naming this completion batch.
pub(crate) fn selected_for_waiter(
    graph: InitGraph,
    wait_at_front: bool,
    producer_completed: bool,
) -> bool {
    waiter_decision(
        module_parameters::g16_completion_rekick_arm.value(),
        graph,
        wait_at_front,
        producer_completed,
    )
}

#[kunit_tests(asahi_g17_completion_rekick)]
mod tests {
    use super::*;

    #[test]
    fn baseline_and_invalid_arms_never_rekick() {
        for value in [0, 5, u8::MAX] {
            assert!(!decision(
                value,
                InitGraph::V25G83,
                Engine::Compute,
                true,
                true
            ));
            assert!(!waiter_decision(value, InitGraph::V25G83, true, true));
            assert!(!decision(
                value,
                InitGraph::V25G83,
                Engine::Render,
                true,
                true
            ));
        }
    }

    #[test]
    fn engine_arms_are_independent() {
        assert!(decision(1, InitGraph::V25G83, Engine::Compute, true, true));
        assert!(!decision(1, InitGraph::V25G83, Engine::Render, true, true));
        assert!(!decision(2, InitGraph::V25G83, Engine::Compute, true, true));
        assert!(decision(2, InitGraph::V25G83, Engine::Render, true, true));
        assert!(decision(3, InitGraph::V25G83, Engine::Compute, true, true));
        assert!(decision(3, InitGraph::V25G83, Engine::Render, true, true));
        assert!(!decision(4, InitGraph::V25G83, Engine::Compute, true, true));
        assert!(!decision(4, InitGraph::V25G83, Engine::Render, true, true));
    }

    #[test]
    fn a_completion_and_queued_work_are_both_required() {
        for engine in [Engine::Compute, Engine::Render] {
            assert!(!decision(3, InitGraph::V25G83, engine, false, true));
            assert!(!decision(3, InitGraph::V25G83, engine, true, false));
            assert!(!decision(3, InitGraph::V25G83, engine, false, false));
        }
    }

    #[test]
    fn other_firmware_graphs_are_unchanged() {
        for engine in [Engine::Compute, Engine::Render] {
            assert!(!decision(3, InitGraph::G17P, engine, true, true));
        }
        assert!(!waiter_decision(4, InitGraph::G17P, true, true));
    }

    #[test]
    fn waiter_arm_requires_front_wait_and_matching_completion() {
        assert!(waiter_decision(4, InitGraph::V25G83, true, true));
        assert!(!waiter_decision(4, InitGraph::V25G83, false, true));
        assert!(!waiter_decision(4, InitGraph::V25G83, true, false));
        assert!(!waiter_decision(4, InitGraph::V25G83, false, false));
        for value in 0..=3 {
            assert!(!waiter_decision(value, InitGraph::V25G83, true, true));
        }
    }
}
