// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Shared-buffer release requires a completed probe and a confirmed stop.

/// Dispose of the RTKit handle, retaining its buffers on missing stop proof.
///
/// A failed/partial startup must not call shutdown as a fallback: successful
/// RTKit shutdown runs reinit, which frees shared buffers before handle drop.
/// The callbacks close/drain the handle even when its shared buffers survive.
pub(super) fn release_rtkit<R>(
    rtkit: Option<R>,
    cpu_started: bool,
    probe_complete: bool,
    stopped_endpoints: Option<usize>,
    require_power_ack: bool,
    shutdown: impl FnOnce(&mut R) -> bool,
    retain: impl FnOnce(&mut R),
) -> bool {
    let mut confirmed =
        stopped_endpoints.is_some_and(|count| !cpu_started || (probe_complete && count != 0));
    // AFK ACKs alone do not stop the RTKit system endpoints. On legacy
    // profiles which do not request power quiescence, their shared buffers
    // remain firmware-visible until reboot even after the apps have stopped.
    if cpu_started && !require_power_ack {
        confirmed = false;
    }
    if let Some(mut rtkit) = rtkit {
        if confirmed && cpu_started && require_power_ack {
            confirmed = shutdown(&mut rtkit);
        }
        if !confirmed {
            retain(&mut rtkit);
        }
        drop(rtkit);
    } else if cpu_started {
        // The CPU was handed ownership, but there is no handle to confirm
        // the stop or close its callbacks. Keep the caller's buffers too.
        confirmed = false;
    }
    confirmed
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{cell::RefCell, rc::Rc};

    struct Peer {
        events: Rc<RefCell<Vec<&'static str>>>,
        live: bool,
        retained: bool,
        power_ack: bool,
    }

    impl Drop for Peer {
        fn drop(&mut self) {
            self.events.borrow_mut().push("callbacks closed");
            if self.live {
                self.events.borrow_mut().push(if self.retained {
                    "buffers retained"
                } else {
                    "buffers freed"
                });
            }
        }
    }

    fn run(
        started: bool,
        complete: bool,
        endpoints: Option<usize>,
        power: bool,
        ack: bool,
    ) -> (bool, Vec<&'static str>) {
        let events = Rc::new(RefCell::new(Vec::new()));
        let peer = Peer {
            events: events.clone(),
            live: true,
            retained: false,
            power_ack: ack,
        };
        let release = release_rtkit(
            Some(peer),
            started,
            complete,
            endpoints,
            power,
            |peer| {
                peer.events.borrow_mut().push("power shutdown");
                if peer.power_ack {
                    // This models the C shutdown's successful reinit.
                    peer.events.borrow_mut().push("reinit freed buffers");
                    peer.live = false;
                    true
                } else {
                    false
                }
            },
            |peer| {
                peer.retained = true;
            },
        );
        let trace = events.borrow().clone();
        (release, trace)
    }

    #[test]
    fn failed_afk_stop_never_calls_freeing_power_fallback() {
        let (release, events) = run(true, true, None, true, true);
        assert!(!release);
        assert_eq!(events, ["callbacks closed", "buffers retained"]);
    }

    #[test]
    fn partial_probe_retains_even_if_some_endpoints_acknowledged() {
        let (release, events) = run(true, false, Some(2), true, true);
        assert!(!release);
        assert_eq!(events, ["callbacks closed", "buffers retained"]);
    }

    #[test]
    fn no_endpoint_proof_cannot_release_a_running_processor() {
        let (release, events) = run(true, true, Some(0), false, false);
        assert!(!release);
        assert_eq!(events, ["callbacks closed", "buffers retained"]);
    }

    #[test]
    fn failed_power_ack_retains_before_handle_drop() {
        let (release, events) = run(true, true, Some(2), true, false);
        assert!(!release);
        assert_eq!(
            events,
            ["power shutdown", "callbacks closed", "buffers retained"]
        );
    }

    #[test]
    fn confirmed_power_shutdown_preserves_the_reinit_release_path() {
        let (release, events) = run(true, true, Some(2), true, true);
        assert!(release);
        assert_eq!(
            events,
            ["power shutdown", "reinit freed buffers", "callbacks closed"]
        );
    }

    #[test]
    fn legacy_afk_stop_without_power_proof_keeps_system_buffers() {
        let (release, events) = run(true, true, Some(2), false, false);
        assert!(!release);
        assert_eq!(events, ["callbacks closed", "buffers retained"]);
    }

    #[test]
    fn failure_before_cpu_handoff_releases_unpublished_buffers() {
        let (release, events) = run(false, false, Some(0), true, false);
        assert!(release);
        assert_eq!(events, ["callbacks closed", "buffers freed"]);
    }

    #[test]
    fn missing_handle_retains_caller_buffers_after_cpu_handoff() {
        assert!(!release_rtkit::<()>(
            None,
            true,
            true,
            Some(2),
            false,
            |_| true,
            |_| {}
        ));
        assert!(release_rtkit::<()>(
            None,
            false,
            false,
            Some(0),
            false,
            |_| true,
            |_| {}
        ));
    }
}
