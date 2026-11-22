// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Per-board die temperature limit of the G17-family firmware builds.
//!
//! Every G17 build runs a die temperature limiter in the firmware: above its target the firmware
//! lowers the GPU state on its own, below its release temperature it lets go. Each build's tables
//! carry a default. A board whose enclosure must stay cooler describes its limit on the GPU node
//! with `apple,fast-die0-target-temp` and `apple,fast-die0-release-temp`, in degrees Celsius like
//! the older generations' `apple,fast-die0-release-temp`. Both are optional; a board value only
//! ever lowers a build's default, so a node without them runs every build as before.

use kernel::{c_str, device, prelude::*};

/// Highest accepted target, in 0.01 °C: the 26A428 default, which no board may raise.
const TARGET_MAX_CC: u32 = super::fw::initdata::DIE_LIMIT.target_cc;
/// Lowest accepted target, in 0.01 °C. A lower limit would hold the GPU at its lowest state at
/// ordinary load temperatures; such a value is a mistake, not a policy.
const TARGET_MIN_CC: u32 = 8_500;
/// Lowest accepted release temperature, in 0.01 °C.
const RELEASE_MIN_CC: u32 = 4_000;
/// Smallest gap kept between a lowered target and the release temperature, in 0.01 °C.
const MIN_GAP_CC: u32 = 100;

/// Limiter temperatures as one firmware table holds them, in 0.01 °C.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Limits {
    pub(crate) target_cc: u32,
    pub(crate) release_cc: u32,
}

/// The board's limit from the device tree, validated; `None` where the board sets nothing.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct DieLimit {
    target_cc: Option<u32>,
    release_cc: Option<u32>,
}

/// A device-tree value the driver did not take as written.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Problem {
    /// Target above [`TARGET_MAX_CC`], clamped to it.
    TargetClamped(u32),
    /// Target below [`TARGET_MIN_CC`], ignored.
    TargetIgnored(u32),
    /// Release temperature below [`RELEASE_MIN_CC`] or not below the target, ignored.
    ReleaseIgnored(u32),
}

impl DieLimit {
    /// Reads and validates the GPU node's limit. Values out of range are clamped or ignored
    /// with a warning; an unreadable node leaves every build's default.
    pub(crate) fn read(dev: &device::Device) -> Self {
        let Some(node) = dev.of_node() else {
            return Self::default();
        };
        let read = |name: &'static CStr| match node.get_property::<u32>(name) {
            Ok(value) => Some(value),
            Err(ENOENT) => None,
            Err(error) => {
                dev_warn!(dev, "Ignoring unreadable GPU {:?}: {:?}\n", name, error);
                None
            }
        };
        let target = read(c_str!("apple,fast-die0-target-temp"));
        let release = read(c_str!("apple,fast-die0-release-temp"));
        let limit = Self::from_celsius(target, release, |problem| match problem {
            Problem::TargetClamped(t) => dev_warn!(
                dev,
                "GPU die target {} C above the firmware default, using {} C\n",
                t,
                TARGET_MAX_CC / 100
            ),
            Problem::TargetIgnored(t) => dev_warn!(
                dev,
                "GPU die target {} C below {} C, ignored\n",
                t,
                TARGET_MIN_CC / 100
            ),
            Problem::ReleaseIgnored(r) => {
                dev_warn!(dev, "GPU die release temperature {} C out of range, ignored\n", r)
            }
        });
        if let Some(target_cc) = limit.target_cc {
            dev_info!(dev, "GPU die limiter target {} C from the device tree\n", target_cc / 100);
        }
        limit
    }

    /// Validates device-tree values in degrees Celsius.
    fn from_celsius(
        target: Option<u32>,
        release: Option<u32>,
        mut problem: impl FnMut(Problem),
    ) -> Self {
        let target_cc = target.and_then(|t| match t.saturating_mul(100) {
            cc if cc > TARGET_MAX_CC => {
                problem(Problem::TargetClamped(t));
                Some(TARGET_MAX_CC)
            }
            cc if cc < TARGET_MIN_CC => {
                problem(Problem::TargetIgnored(t));
                None
            }
            cc => Some(cc),
        });
        let ceiling = target_cc.unwrap_or(TARGET_MAX_CC);
        let release_cc = release.and_then(|r| match r.saturating_mul(100) {
            cc if (RELEASE_MIN_CC..ceiling).contains(&cc) => Some(cc),
            _ => {
                problem(Problem::ReleaseIgnored(r));
                None
            }
        });
        Self { target_cc, release_cc }
    }

    /// Whether the board sets any limit.
    pub(crate) fn is_set(&self) -> bool {
        *self != Self::default()
    }

    /// The limiter temperatures for a firmware table whose build default is `default`: the
    /// board's where set, never above the build's, with the release below the target.
    pub(crate) fn apply(&self, default: Limits) -> Limits {
        if !self.is_set() {
            return default;
        }
        let target_cc = self.target_cc.map_or(default.target_cc, |t| t.min(default.target_cc));
        let release_cc = self
            .release_cc
            .map_or(default.release_cc, |r| r.min(default.release_cc))
            .min(target_cc.saturating_sub(MIN_GAP_CC));
        Limits { target_cc, release_cc }
    }

    /// Writes the limiter temperatures for `default` at `target` and `release`, byte offsets
    /// of 32-bit little-endian words in `bytes`.
    pub(crate) fn write(&self, bytes: &mut [u8], default: Limits, target: usize, release: usize) {
        let limits = self.apply(default);
        bytes[target..target + 4].copy_from_slice(&limits.target_cc.to_le_bytes());
        bytes[release..release + 4].copy_from_slice(&limits.release_cc.to_le_bytes());
    }
}

#[kunit_tests(asahi_g17_die_limit)]
mod tests {
    use super::*;

    const BUILD: Limits = Limits { target_cc: 11_000, release_cc: 8_000 };
    const STATE_25G83: Limits = Limits { target_cc: 10_100, release_cc: 9_600 };

    fn parse(target: Option<u32>, release: Option<u32>) -> (DieLimit, KVec<Problem>) {
        let mut problems = KVec::new();
        let limit = DieLimit::from_celsius(target, release, |p| {
            let _ = problems.push(p, GFP_KERNEL);
        });
        (limit, problems)
    }

    #[test]
    fn absent_keeps_every_build_default() {
        let (limit, problems) = parse(None, None);
        assert!(problems.is_empty());
        assert!(!limit.is_set());
        assert_eq!(limit.apply(BUILD), BUILD);
        assert_eq!(limit.apply(STATE_25G83), STATE_25G83);
    }

    #[test]
    fn board_target_lowers_but_never_raises() {
        let (limit, problems) = parse(Some(108), None);
        assert!(problems.is_empty());
        assert_eq!(limit.apply(BUILD), Limits { target_cc: 10_800, release_cc: 8_000 });
        // A build whose own default is lower keeps it.
        assert_eq!(limit.apply(STATE_25G83), STATE_25G83);
    }

    #[test]
    fn nonsense_is_clamped_or_ignored() {
        let (limit, problems) = parse(Some(150), None);
        assert_eq!(&problems[..], &[Problem::TargetClamped(150)]);
        assert_eq!(limit.apply(BUILD), BUILD);

        let (limit, problems) = parse(Some(20), None);
        assert_eq!(&problems[..], &[Problem::TargetIgnored(20)]);
        assert!(!limit.is_set());

        let (limit, problems) = parse(Some(u32::MAX), Some(u32::MAX));
        assert_eq!(
            &problems[..],
            &[Problem::TargetClamped(u32::MAX), Problem::ReleaseIgnored(u32::MAX)]
        );
        assert_eq!(limit.apply(BUILD), BUILD);

        let (_, problems) = parse(Some(100), Some(100));
        assert_eq!(&problems[..], &[Problem::ReleaseIgnored(100)]);
    }

    #[test]
    fn release_stays_below_a_lowered_target() {
        let (limit, problems) = parse(Some(95), Some(70));
        assert!(problems.is_empty());
        assert_eq!(limit.apply(BUILD), Limits { target_cc: 9_500, release_cc: 7_000 });
        // 25G83 power state: target 95, its 96 C release pulled below the target.
        assert_eq!(limit.apply(STATE_25G83), Limits { target_cc: 9_500, release_cc: 7_000 });
        let (limit, _) = parse(Some(95), None);
        assert_eq!(limit.apply(STATE_25G83), Limits { target_cc: 9_500, release_cc: 9_400 });
    }
}
