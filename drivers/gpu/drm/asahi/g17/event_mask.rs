// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Test-only G16 KSM kick event-mask arms.
//!
//! Arm A preserves the production masks byte for byte. The other arms add KSM
//! event 253 independently to compute, tiling, fragment, the render
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

/// KSM event 253 in the fourth word of the 256-bit mask.
const KSM_EVENT_253: u64 = 1 << 61;

/// Test arm selected at module load.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum Arm {
    /// Existing Linux masks, unchanged.
    A,
    /// Add event 253 to compute only.
    C,
    /// Add event 253 to tiling only.
    T,
    /// Add event 253 to fragment only.
    F,
    /// Add event 253 to both render stages.
    R,
    /// Add event 253 to all three engines.
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

fn mask(value: u8, engine: Engine) -> [u64; 4] {
    let mut mask = match engine {
        Engine::Compute => COMPUTE_KICK_EVENT_MASK,
        Engine::Tiling | Engine::Fragment => RENDER_KICK_EVENT_MASK,
    };
    if Arm::from_param(value).covers(engine) {
        mask[3] |= KSM_EVENT_253;
    }
    mask
}

/// Returns the mask for one kick under the selected test arm.
pub(crate) fn selected(engine: Engine) -> [u64; 4] {
    mask(module_parameters::g16_ksm_event_mask_arm.value(), engine)
}

#[kunit_tests(asahi_g17_ksm_event_mask)]
mod tests {
    use super::*;

    const COMPUTE_EVENT_253: [u64; 4] = [0, 0, 4, KSM_EVENT_253];
    const RENDER_EVENT_253: [u64; 4] = [0, 0, 0, KSM_EVENT_253];

    #[test]
    fn arm_a_and_invalid_values_preserve_the_default() {
        for value in [0, 6, u8::MAX] {
            assert_eq!(mask(value, Engine::Compute), COMPUTE_KICK_EVENT_MASK);
            assert_eq!(mask(value, Engine::Tiling), RENDER_KICK_EVENT_MASK);
            assert_eq!(mask(value, Engine::Fragment), RENDER_KICK_EVENT_MASK);
        }
    }

    #[test]
    fn single_engine_arms_are_isolated() {
        assert_eq!(mask(1, Engine::Compute), COMPUTE_EVENT_253);
        assert_eq!(mask(1, Engine::Tiling), RENDER_KICK_EVENT_MASK);
        assert_eq!(mask(1, Engine::Fragment), RENDER_KICK_EVENT_MASK);

        assert_eq!(mask(2, Engine::Compute), COMPUTE_KICK_EVENT_MASK);
        assert_eq!(mask(2, Engine::Tiling), RENDER_EVENT_253);
        assert_eq!(mask(2, Engine::Fragment), RENDER_KICK_EVENT_MASK);

        assert_eq!(mask(3, Engine::Compute), COMPUTE_KICK_EVENT_MASK);
        assert_eq!(mask(3, Engine::Tiling), RENDER_KICK_EVENT_MASK);
        assert_eq!(mask(3, Engine::Fragment), RENDER_EVENT_253);
    }

    #[test]
    fn paired_and_all_engine_arms_cover_only_their_named_engines() {
        assert_eq!(mask(4, Engine::Compute), COMPUTE_KICK_EVENT_MASK);
        assert_eq!(mask(4, Engine::Tiling), RENDER_EVENT_253);
        assert_eq!(mask(4, Engine::Fragment), RENDER_EVENT_253);

        assert_eq!(mask(5, Engine::Compute), COMPUTE_EVENT_253);
        assert_eq!(mask(5, Engine::Tiling), RENDER_EVENT_253);
        assert_eq!(mask(5, Engine::Fragment), RENDER_EVENT_253);
    }
}
