// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Whether a pair's retained render groups, retired by firmware without executing, are
//! published again. Only the whole set is replayed, so no replayed group runs after a live
//! successor, and work a recovery blamed is never run again.

/// One retained group, front first.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Group {
    /// Published, not failed and not classified guilty by a recovery.
    pub(crate) innocent: bool,
    /// A recovery blamed it, whatever class it kept (an earlier recovery may have spared it):
    /// it may have caused the fault.
    pub(crate) blamed: bool,
    /// Carries the strict abandonment witness.
    pub(crate) abandoned: bool,
    /// Already republished the maximum number of times.
    pub(crate) exhausted: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Decision {
    Replay,
    /// A group may still execute: look again later.
    Wait,
    /// Never replay this set; the render timeout settles it.
    Refuse,
}

/// Groups are read in order and the first one that decides stops the walk, so a caller can
/// observe the abandonment lazily. An exhausted front is replayed: the replay fails it, and its
/// successors, which consume its output, with it. An exhausted successor would break the order.
pub(crate) fn decide(groups: impl Iterator<Item = Group>) -> Decision {
    let mut front_exhausted = None;
    let mut successor_exhausted = false;
    for group in groups {
        if !group.innocent || group.blamed {
            return Decision::Refuse;
        }
        if !group.abandoned {
            return Decision::Wait;
        }
        match front_exhausted {
            None => front_exhausted = Some(group.exhausted),
            Some(_) => successor_exhausted |= group.exhausted,
        }
    }
    match front_exhausted {
        Some(true) => Decision::Replay,
        Some(false) if !successor_exhausted => Decision::Replay,
        _ => Decision::Refuse,
    }
}

#[cfg_attr(not(test), kernel::macros::kunit_tests(asahi_g17_render_replay))]
mod tests {
    use super::*;
    fn group(innocent: bool, abandoned: bool, exhausted: bool) -> Group {
        Group { innocent, blamed: false, abandoned, exhausted }
    }
    const OK: Group = Group { innocent: true, blamed: false, abandoned: true, exhausted: false };
    const BLAMED: Group = Group { innocent: true, blamed: true, abandoned: true, exhausted: false };
    #[test]
    fn every_group_abandoned_and_innocent_is_replayed() {
        assert_eq!(decide([OK].into_iter()), Decision::Replay);
        assert_eq!(decide([OK, OK, OK].into_iter()), Decision::Replay);
    }
    #[test]
    fn nothing_retained_is_never_replayed() {
        assert_eq!(decide([].into_iter()), Decision::Refuse);
    }
    #[test]
    fn guilty_work_is_never_replayed() {
        assert_eq!(decide([group(false, true, false)].into_iter()), Decision::Refuse);
        assert_eq!(decide([OK, group(false, true, false)].into_iter()), Decision::Refuse);
    }
    #[test]
    fn blamed_work_is_never_replayed_even_when_spared() {
        // An earlier recovery spared it, so it still counts as innocent; the blame refuses it.
        assert_eq!(decide([BLAMED].into_iter()), Decision::Refuse);
        assert_eq!(decide([BLAMED, OK].into_iter()), Decision::Refuse);
        assert_eq!(decide([OK, BLAMED].into_iter()), Decision::Refuse);
        // Also once the replay limit is reached, where an innocent front alone is replayed.
        let exhausted = Group { exhausted: true, ..BLAMED };
        assert_eq!(decide([exhausted].into_iter()), Decision::Refuse);
    }
    #[test]
    fn a_live_group_waits_and_stops_the_walk() {
        assert_eq!(decide([OK, group(true, false, false)].into_iter()), Decision::Wait);
        // Nothing after the live group is looked at, guilty or not.
        assert_eq!(
            decide([group(true, false, false), group(false, true, false)].into_iter()),
            Decision::Wait
        );
        let mut seen = 0;
        let groups = [group(true, false, false), OK, OK].into_iter().inspect(|_| seen += 1);
        assert_eq!(decide(groups), Decision::Wait);
        assert_eq!(seen, 1);
    }
    #[test]
    fn an_exhausted_successor_refuses_an_exhausted_front_replays() {
        assert_eq!(decide([OK, group(true, true, true)].into_iter()), Decision::Refuse);
        assert_eq!(decide([group(true, true, true), OK].into_iter()), Decision::Replay);
        assert_eq!(
            decide([group(true, true, true), group(true, true, true)].into_iter()),
            Decision::Replay
        );
    }
}
