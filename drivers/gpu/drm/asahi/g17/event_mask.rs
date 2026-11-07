// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Test-only G16 KSM kick event-mask arms.
//!
//! Arm A preserves the production masks byte for byte. The other arms add one
//! operator-selected event independently to compute, tiling, fragment, the render
//! pair, or every engine. They exist only for controlled A/B boots and must not
//! become the default without a separate correctness and latency result.

use crate::g17::fw::{compute::COMPUTE_KICK_EVENT_MASK, render::RENDER_KICK_EVENT_MASK};
use crate::module_parameters;
use kernel::prelude::kunit_tests;

/// Engine whose kick entry carries the mask.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Engine {
    Compute,
    Tiling,
    Fragment,
}

/// Test arm selected at module load.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum Arm {
    /// Existing Linux masks, unchanged.
    A,
    /// Add the selected event to compute only.
    C,
    /// Add the selected event to tiling only.
    T,
    /// Add the selected event to fragment only.
    F,
    /// Add the selected event to both render stages.
    R,
    /// Add the selected event to all three engines.
    M,
}

impl Arm {
    fn from_param(value: u8) -> Self {
        match value {
            1 => Self::C,
            2 => Self::T,
            3 => Self::F,
            4 => Self::R,
            5 => Self::M,
            _ => Self::A,
        }
    }

    fn covers(self, engine: Engine) -> bool {
        matches!(
            (self, engine),
            (Self::C, Engine::Compute)
                | (Self::T, Engine::Tiling)
                | (Self::F, Engine::Fragment)
                | (Self::R, Engine::Tiling | Engine::Fragment)
                | (Self::M, _)
        )
    }
}

fn mask(value: u8, bit: u16, engine: Engine) -> [u64; 4] {
    let mut mask = match engine {
        Engine::Compute => COMPUTE_KICK_EVENT_MASK,
        Engine::Tiling | Engine::Fragment => RENDER_KICK_EVENT_MASK,
    };
    if Arm::from_param(value).covers(engine) && bit < 256 {
        mask[usize::from(bit / 64)] |= 1 << (bit % 64);
    }
    mask
}

/// Returns the mask for one kick under the selected test arm.
pub(crate) fn selected(engine: Engine) -> [u64; 4] {
    mask(
        module_parameters::g16_ksm_event_mask_arm.value(),
        module_parameters::g16_ksm_event_mask_bit.value(),
        engine,
    )
}

#[kunit_tests(asahi_g17_ksm_event_mask)]
mod tests {
    use super::*;

    const EVENT: u64 = 1 << 17;
    const COMPUTE_EVENT: [u64; 4] = [EVENT, 0, 4, 0];
    const RENDER_EVENT: [u64; 4] = [EVENT, 0, 0, 0];

    #[test]
    fn arm_a_and_invalid_values_preserve_the_default() {
        for value in [0, 6, u8::MAX] {
            assert_eq!(mask(value, 17, Engine::Compute), COMPUTE_KICK_EVENT_MASK);
            assert_eq!(mask(value, 17, Engine::Tiling), RENDER_KICK_EVENT_MASK);
            assert_eq!(mask(value, 17, Engine::Fragment), RENDER_KICK_EVENT_MASK);
        }
        assert_eq!(mask(5, 256, Engine::Compute), COMPUTE_KICK_EVENT_MASK);
        assert_eq!(mask(5, u16::MAX, Engine::Tiling), RENDER_KICK_EVENT_MASK);
    }

    #[test]
    fn single_engine_arms_are_isolated() {
        assert_eq!(mask(1, 17, Engine::Compute), COMPUTE_EVENT);
        assert_eq!(mask(1, 17, Engine::Tiling), RENDER_KICK_EVENT_MASK);
        assert_eq!(mask(1, 17, Engine::Fragment), RENDER_KICK_EVENT_MASK);

        assert_eq!(mask(2, 17, Engine::Compute), COMPUTE_KICK_EVENT_MASK);
        assert_eq!(mask(2, 17, Engine::Tiling), RENDER_EVENT);
        assert_eq!(mask(2, 17, Engine::Fragment), RENDER_KICK_EVENT_MASK);

        assert_eq!(mask(3, 17, Engine::Compute), COMPUTE_KICK_EVENT_MASK);
        assert_eq!(mask(3, 17, Engine::Tiling), RENDER_KICK_EVENT_MASK);
        assert_eq!(mask(3, 17, Engine::Fragment), RENDER_EVENT);
    }

    #[test]
    fn paired_and_all_engine_arms_cover_only_their_named_engines() {
        assert_eq!(mask(4, 17, Engine::Compute), COMPUTE_KICK_EVENT_MASK);
        assert_eq!(mask(4, 17, Engine::Tiling), RENDER_EVENT);
        assert_eq!(mask(4, 17, Engine::Fragment), RENDER_EVENT);

        assert_eq!(mask(5, 17, Engine::Compute), COMPUTE_EVENT);
        assert_eq!(mask(5, 17, Engine::Tiling), RENDER_EVENT);
        assert_eq!(mask(5, 17, Engine::Fragment), RENDER_EVENT);
    }
}
