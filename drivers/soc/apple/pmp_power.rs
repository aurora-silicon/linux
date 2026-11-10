// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Correlation for PMP's untagged, serialized device-power replies.

pub(crate) const MARKER: u64 = 1 << 53;
pub(crate) const SHIFT: u32 = 44;
/// Device-power requests put the target and requested state below the opcode.
pub(crate) const POWER_DEVICE_SHIFT: u32 = 40;
pub(crate) const POWER_STATE_SHIFT: u32 = 32;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum Command {
    Idle,
    Pending(u64),
    Complete,
    Failed,
}

impl Command {
    pub(crate) fn begin(&mut self, command: u8, reply_payload: u64) -> bool {
        if *self != Self::Idle || !matches!(command, 0x0e | 0x0f) {
            return false;
        }
        *self = Self::Pending(MARKER | ((u64::from(command) + 1) << SHIFT) | reply_payload);
        true
    }

    pub(crate) fn reply(&mut self, message: u64) {
        *self = match *self {
            Self::Pending(expected) if message == expected => Self::Complete,
            // No request ID is echoed. A timeout, duplicate, or unexpected
            // reply loses correlation until a new PMP session is established.
            _ => Self::Failed,
        };
    }
}

#[kernel::macros::kunit_tests(apple_pmp_command)]
mod tests {
    use super::*;

    #[test]
    fn observed_gfx_reply_completes_only_an_outstanding_request() {
        let mut state = Command::Idle;
        assert!(state.begin(0x0f, 0));
        assert!(!state.begin(0x0f, 0));
        state.reply(0x0021000000000000);
        assert_eq!(state, Command::Complete);
        state.reply(0x0021000000000000);
        assert_eq!(state, Command::Failed);
    }

    #[test]
    fn nonzero_reply_payload_must_match_exactly() {
        let mut state = Command::Idle;
        let payload = 1;
        assert!(state.begin(0x0f, payload));
        state.reply(MARKER | (0x10 << SHIFT) | payload);
        assert_eq!(state, Command::Complete);
    }

    #[test]
    fn zero_payload_completion_cannot_complete_a_tagged_request() {
        let mut state = Command::Idle;
        let payload = 1;
        assert!(state.begin(0x0f, payload));
        state.reply(MARKER | (0x10 << SHIFT));
        assert_eq!(state, Command::Failed);
    }

    #[test]
    fn late_ack_cannot_complete_a_new_command_after_timeout() {
        let mut state = Command::Idle;
        assert!(state.begin(0x0f, 0));
        state = Command::Failed;
        assert!(!state.begin(0x0f, 0));
        state.reply(0x0021000000000000);
        assert_eq!(state, Command::Failed);
    }

    #[test]
    fn wrong_opcode_or_nonzero_reply_payload_is_not_success() {
        for reply in [0x0020f00000000000, 0x0021000000000001] {
            let mut state = Command::Idle;
            assert!(state.begin(0x0f, 0));
            state.reply(reply);
            assert_eq!(state, Command::Failed);
        }
    }
}
