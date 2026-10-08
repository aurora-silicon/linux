// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Keeps a persisted lookup identity distinct from the enclave bag identity.

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum UuidProvenance {
    AsGenerated,
    ReadBackFromBag,
}

/// Binding established from a successfully recovered source handle. A snapshot
/// must identify that same bag, without changing the host's lookup identity.
#[derive(Clone, Copy)]
pub(crate) struct SnapshotIdentity {
    record_uuid: [u8; 16],
    provenance: UuidProvenance,
    bag_uuid: [u8; 16],
}

impl SnapshotIdentity {
    pub(crate) fn from_loaded(
        record_uuid: [u8; 16],
        provenance: UuidProvenance,
        source_bag_uuid: [u8; 16],
        loaded_by_generated_uuid: bool,
    ) -> Option<Self> {
        if loaded_by_generated_uuid {
            if provenance != UuidProvenance::AsGenerated {
                return None;
            }
        } else if record_uuid != source_bag_uuid {
            return None;
        }
        Some(Self { record_uuid, provenance, bag_uuid: source_bag_uuid })
    }

    pub(crate) fn accepts(
        &self,
        record_uuid: &[u8; 16],
        provenance: UuidProvenance,
        snapshot_uuid: &[u8; 16],
    ) -> bool {
        self.record_uuid == *record_uuid
            && self.provenance == provenance
            && self.bag_uuid == *snapshot_uuid
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const LOOKUP: [u8; 16] = [1; 16];
    const BAG: [u8; 16] = [2; 16];
    const OTHER: [u8; 16] = [3; 16];

    #[test]
    fn sepos13_keeps_lookup_and_bag_identities_distinct() {
        let proof = SnapshotIdentity::from_loaded(
            LOOKUP, UuidProvenance::AsGenerated, BAG, true,
        ).unwrap();
        assert!(proof.accepts(&LOOKUP, UuidProvenance::AsGenerated, &BAG));
        assert!(!proof.accepts(&BAG, UuidProvenance::AsGenerated, &BAG));
        assert!(!proof.accepts(&LOOKUP, UuidProvenance::AsGenerated, &LOOKUP));
    }

    #[test]
    fn changed_record_or_designated_bag_is_rejected() {
        let proof = SnapshotIdentity::from_loaded(
            LOOKUP, UuidProvenance::AsGenerated, BAG, true,
        ).unwrap();
        assert!(!proof.accepts(&OTHER, UuidProvenance::AsGenerated, &BAG));
        assert!(!proof.accepts(&LOOKUP, UuidProvenance::ReadBackFromBag, &BAG));
        assert!(!proof.accepts(&LOOKUP, UuidProvenance::AsGenerated, &OTHER));
    }

    #[test]
    fn blob_recovery_keeps_strict_uuid_equality() {
        for provenance in [UuidProvenance::AsGenerated, UuidProvenance::ReadBackFromBag] {
            assert!(SnapshotIdentity::from_loaded(LOOKUP, provenance, BAG, false).is_none());
            let proof = SnapshotIdentity::from_loaded(BAG, provenance, BAG, false).unwrap();
            assert!(proof.accepts(&BAG, provenance, &BAG));
            assert!(!proof.accepts(&BAG, provenance, &OTHER));
        }
    }

    #[test]
    fn readback_record_cannot_claim_generated_lookup() {
        assert!(SnapshotIdentity::from_loaded(
            LOOKUP, UuidProvenance::ReadBackFromBag, BAG, true,
        ).is_none());
    }
}
