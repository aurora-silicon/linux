// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj
//! Touch ID: the biometric endpoint (SBIO) and its `/dev/sep-bio` interface.
//! The enclave matches; no biometric image ever crosses to userspace.

use super::*;
use crate::proto::*;
use crate::sks::SKS_AUTH_TOKEN_LEN;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

impl SepData {
    fn sbio_expect_ok(&self, op: &crate::sbio::SbioOp) -> Option<KVec<u8>> {
        match self.sbio_call(op) {
            SbioOutcome::Ok(payload) => Some(payload),
            _ => None,
        }
    }

    pub(crate) fn sbio_call(&self, op: &crate::sbio::SbioOp) -> SbioOutcome {
        let done = match self.sbio_transfer(op) {
            Ok(done) => done,
            Err(_) => {
                return SbioOutcome::Other;
            }
        };

        let Some(err) = done.status.answered() else {
            return SbioOutcome::Other;
        };

        match err as u16 {
            crate::sbio::SBIO_STATUS_OK => SbioOutcome::Ok(done.payload),
            crate::sbio::SBIO_STATUS_PREREQUISITE => SbioOutcome::PrerequisiteMissing,
            crate::sbio::SBIO_STATUS_16 => SbioOutcome::Status16,
            _ => SbioOutcome::Other,
        }
    }

    fn sbio_relay(&self, relay: &crate::sbio::SbioRelay<'_>) -> Option<KVec<u8>> {
        match self.sbio_transfer_raw(relay.opcode(), relay.name(), relay.payload()) {
            Ok(done) if done.status.is_ok() => Some(done.payload),
            Ok(_) => None,
            Err(_) => None,
        }
    }

    fn sync_device_view(&self) -> bool {
        let synced = matches!(
            self.sbio_call(&crate::sbio::sbio_update_device_list()),
            SbioOutcome::Ok(_)
        );

        let policy_ok = match self.sbio_call(&crate::sbio::sbio_match_policy()) {
            SbioOutcome::Ok(policy) if policy.len() == crate::sbio::SBIO_MATCH_POLICY_LEN => true,
            SbioOutcome::Ok(_) => false,
            _ => false,
        };

        synced && policy_ok
    }

    fn note_capture_end(&self) {
        self.last_capture_end_ns.store(shim::boottime_ns(), Relaxed);
    }

    fn settle_before_capture(&self) {
        let last = self.last_capture_end_ns.load(Relaxed);
        if last == 0 {
            return;
        }
        let elapsed_ms = shim::boottime_ns().saturating_sub(last) / 1_000_000;
        if elapsed_ms >= u64::from(MATCH_SETTLE_MS) {
            return;
        }
        let remaining = MATCH_SETTLE_MS - elapsed_ms as u32;
        let _ = sensor::idle();
        kernel::time::delay::fsleep(kernel::time::Delta::from_millis(i64::from(remaining)));
    }

    fn pace_between_captures(&self) {
        let _ = sensor::idle();

        if !self.await_sensor_state(sensor::STATE_IDLE, c"idle, between captures") {
            dev_warn!(
                self.dev,
                "enrol: sensor did not idle within {} ms; continuing the reposition wait anyway (pause is for the person)\n",
                ENROL_IDLE_TIMEOUT_MS
            );
        }

        kernel::time::delay::fsleep(kernel::time::Delta::from_millis(i64::from(
            ENROL_REPOSITION_MS,
        )));
    }

    fn stash_enrol_identity(&self, record: &[u8]) {
        let mut found: KVec<[u8; bio::UUID_LEN]> = KVec::new();
        crate::sbio::enrol_identity_candidates(record, SBIO_PROBE_USER_ID, |_at, uuid| {
            let _ = found.push(uuid, GFP_KERNEL);
        });
        *self.enrol_identity_candidates.lock() = found;
    }

    fn identity_just_enrolled(&self) -> Option<[u8; bio::UUID_LEN]> {
        let candidates = core::mem::take(&mut *self.enrol_identity_candidates.lock());
        let listed = self.enclave_identities_for(SBIO_PROBE_USER_ID)?;

        let mut confirmed: KVec<[u8; bio::UUID_LEN]> = KVec::new();
        for uuid in candidates.iter() {
            if listed.iter().any(|u| u == uuid) && !confirmed.iter().any(|u| u == uuid) {
                if confirmed.push(*uuid, GFP_KERNEL).is_err() {
                    break;
                }
            }
        }

        if confirmed.len() == 1 {
            return Some(confirmed[0]);
        }

        let index = self.bio_index.lock();
        let mut new_identity = None;
        for uuid in listed.iter() {
            if index.contains_uuid(uuid) {
                continue;
            }
            if new_identity.is_some() {
                return None;
            }
            new_identity = Some(*uuid);
        }
        new_identity
    }

    fn reconcile_identities(&self) {
        let Some(listed) = self.enclave_identities_for(SBIO_PROBE_USER_ID) else {
            return;
        };

        if listed.is_empty() {
            return;
        }

        let dropped = {
            let mut index = self.bio_index.lock();
            match index.reconcile_to(&listed) {
                Ok(dropped) => dropped,
                Err(_) => {
                    return;
                }
            }
        };

        let held = {
            let index = self.bio_index.lock();
            index.total()
        };
        if dropped.is_empty() && held == listed.len() {
            return;
        }
        let persisted = {
            let index = self.bio_index.lock();
            self.with_host_store(|store| index.persist(store))
        };
        match persisted {
            Some(Ok(())) => {},
            _ => dev_warn!(
                self.dev,
                "sbio: the reconciled index could not be written; it is correct in memory for this boot and will be rebuilt at the next attach\n"
            ),
        }
    }

    fn enclave_identities_for(&self, user_id: i32) -> Option<KVec<[u8; bio::UUID_LEN]>> {
        let op = crate::sbio::sbio_list_identities();
        let SbioOutcome::Ok(reply) = self.sbio_call(&op) else {
            return None;
        };

        let records = crate::sbio::IdentityRecords::new(&reply)?;

        let mut out = KVec::new();
        let mut failed = false;
        records.each_built_in_for(user_id, |identity| {
            if out.push(*identity.uuid(), GFP_KERNEL).is_err() {
                failed = true;
            }
        });
        if failed {
            return None;
        }
        Some(out)
    }

    fn finish_enrolment(&self, outcome: core::result::Result<[u8; bio::UUID_LEN], u32>) {
        let filed = {
            let mut session = self.bio_session.lock();
            let mut index = self.bio_index.lock();
            bio::enrol_finish(&mut session, &mut index, outcome)
        };
        if !filed {
            return;
        }
        // lock ordering: store lock outside the session lock (UAF otherwise)
        if outcome.is_ok() {
            let index = self.bio_index.lock();
            let saved = self.with_host_store(|store| index.persist(store));
            match saved {
                Some(Ok(())) => {},
                Some(Err(e)) => dev_err!(
                    self.dev,
                    "enrol: enclave holds a new template but the host index could not be written ({:?})\n",
                    e
                ),
                None => {}
            }
        }
        self.bio_wake();
    }

    fn bio_wake(&self) {
        if let Some(dev) = self.bio_dev.lock().as_ref() {
            dev.wake();
        }
    }

    pub(crate) fn attach_sensor(&self) {
        if let Err(e) = sensor::register_driver() {
            self.sensor_present.store(false, Relaxed);
            dev_err!(
                self.dev,
                "sensor: could not register the SPI driver: {:?}\n",
                e
            );
            return;
        }

        if let Err(e) = dt::enable_spi_sensor(sensor::CONTROLLER_BASE, sensor::CHIP_SELECT) {
            self.sensor_present.store(false, Relaxed);
            dev_warn!(
                self.dev,
                "sensor: could not enable the SPI bus at 0x{:x} or create the sensor node ({:?}); no sensor\n",
                sensor::CONTROLLER_BASE,
                e
            );
            return;
        }

        let bound = sensor::is_bound();
        self.sensor_present.store(bound, Relaxed);
        if bound && sensor::power_line().is_none() {
            dev_warn!(
                self.dev,
                "sensor: no power line ({}); an unpowered sensor answers sixteen zero bytes like a dead bus\n",
                sensor::power_source().name()
            );
        }
    }

    fn open_enrolment_context(&self, user: crate::sbio::UserId) -> bool {
        let listed = match self.enclave_identities_for(SBIO_PROBE_USER_ID) {
            Some(list) => list.len(),
            None => {
                return self.enrol_into_existing_context(user);
            }
        };

        let Some(proof) = crate::sbio::NoExistingCatacomb::from_zero_identities(listed) else {
            return self.enrol_into_existing_context(user);
        };

        self.open_fresh_context(user, &proof)
    }

    fn enrol_into_existing_context(&self, user: crate::sbio::UserId) -> bool {
        if !self.activate_protected_config(user) {
            return false;
        }
        true
    }

    fn open_fresh_context(
        &self,
        user: crate::sbio::UserId,
        proof: &crate::sbio::NoExistingCatacomb,
    ) -> bool {
        let system = crate::sbio::sbio_select_context(crate::sbio::ContextScope::SYSTEM, proof);
        if self.sbio_expect_ok(&system).is_none() {
            return false;
        }

        for (who, kind, what) in [
            (
                crate::sbio::CatacombUser::MASTER,
                PRIVATE_TYPE_CATACOMB_MASTER,
                c"master catacomb",
            ),
            (
                crate::sbio::CatacombUser::OWNER,
                PRIVATE_TYPE_CATACOMB_OWNER,
                c"owner catacomb",
            ),
        ] {
            if !self.save_catacomb(who, kind, what) {
                return false;
            }
        }

        let per_user =
            crate::sbio::sbio_select_context(crate::sbio::ContextScope::user(user), proof);
        if self.sbio_expect_ok(&per_user).is_none() {
            return false;
        }

        if !self.activate_protected_config(user) {
            return false;
        }

        true
    }

    fn activate_protected_config(&self, user: crate::sbio::UserId) -> bool {
        let op = crate::sbio::sbio_protected_config(user);
        let Some(config) = self.sbio_expect_ok(&op) else {
            return false;
        };
        if config.len() != crate::sbio::SBIO_PROTECTED_CONFIG_LEN {
            return false;
        }
        true
    }

    fn begin_enrolment_on_enclave(&self) -> Option<OpenEnrolment<'_>> {
        if self.enrol_open.load(Relaxed) {
            self.enrol_open.store(false, Relaxed);
            let _ = self.sbio_call(&crate::sbio::sbio_cancel_operation());
        }

        let user = crate::sbio::UserId::new(SBIO_PROBE_USER_ID)?;

        // 0x03 answers -3 until a user key bag is designated
        if !self.keybag_designated.load(Relaxed) {
            return None;
        }

        if self.enrol_material.lock().is_none() {
            return None;
        }

        // 0x03 answers 0x1 until an enrolment context exists
        if !self.open_enrolment_context(user) {
            return None;
        }

        let Some(acm_handle) = self.establish_passcode_validated_context(user) else {
            dev_err!(
                self.dev,
                "enrol: could not establish the PasscodeValidated SCRD credential; not falling back to a transient SKS token (a catacomb sealed under one does not survive a reboot)\n"
            );
            return None;
        };
        let op =
            crate::sbio::sbio_begin_enrol(user, crate::sbio::BE_AUTH_TYPE_ACM_CONTEXT, &acm_handle);

        match self.sbio_call(&op) {
            SbioOutcome::Ok(_payload) => {
                self.enrol_open.store(true, Relaxed);
                Some(OpenEnrolment {
                    sep: self,
                    armed: true,
                })
            }
            _ => None,
        }
    }

    fn restore_all_components(&self) -> bool {
        // 0x6b is re-read before every component, not once up front.
        let user_id = SBIO_PROBE_USER_ID;
        let mut any = false;
        let mut missing_files = false;
        let mut user_outcome = RestoreOutcome::NoStoredFile;

        // 0x8002 (cold transition) is a success only on the owner component.
        for (id, kind, what, cold_ok) in [
            (
                crate::sbio::CatacombUser::MASTER.value(),
                PRIVATE_TYPE_CATACOMB_MASTER,
                c"master catacomb",
                false,
            ),
            (
                crate::sbio::CatacombUser::OWNER.value(),
                PRIVATE_TYPE_CATACOMB_OWNER,
                c"owner catacomb",
                true,
            ),
            (user_id, PRIVATE_TYPE_CATACOMB_USER, c"user catacomb", false),
        ] {
            let Some(reply) = self.read_component_states() else {
                return false;
            };
            let states = crate::sbio::ComponentStates::new(&reply);
            let outcome = self.restore_catacomb(states.state_for(id), id, kind, what, cold_ok);
            match outcome {
                RestoreOutcome::Restored | RestoreOutcome::AlreadyActive => any = true,
                RestoreOutcome::NoStoredFile => missing_files = true,
                RestoreOutcome::Failed | RestoreOutcome::EmptyTolerated => {}
            }
            if id == user_id {
                user_outcome = outcome;
            }
        }

        let user_ok = matches!(
            user_outcome,
            RestoreOutcome::Restored | RestoreOutcome::AlreadyActive
        );
        let lockout = match self.restore_lockout() {
            RestoreOutcome::Restored | RestoreOutcome::AlreadyActive => true,
            RestoreOutcome::EmptyTolerated => true,
            RestoreOutcome::NoStoredFile => {
                missing_files = true;
                false
            }
            RestoreOutcome::Failed => false,
        };

        if missing_files {
            dev_warn!(
                self.dev,
                "sbio: at least one of four artefacts has no file on disk; if enrolled before the four-artefact save, re-enrol once to write all four\n"
            );
        }

        let _ = any;
        user_ok && lockout
    }

    fn read_component_states(&self) -> Option<KVec<u8>> {
        let op = crate::sbio::sbio_context_state();
        let SbioOutcome::Ok(reply) = self.sbio_call(&op) else {
            return None;
        };

        let states = crate::sbio::ComponentStates::new(&reply);
        if states.count() == 0 {
            return None;
        }
        Some(reply)
    }

    fn restore_catacomb(
        &self,
        state: Option<u32>,
        id: i32,
        kind: u8,
        what: &CStr,
        cold_ok: bool,
    ) -> RestoreOutcome {
        let Some(state) = state else {
            return RestoreOutcome::Failed;
        };
        match crate::sbio::component_action(state) {
            crate::sbio::ComponentAction::AlreadyActive => RestoreOutcome::AlreadyActive,
            crate::sbio::ComponentAction::Unsupported => RestoreOutcome::Failed,
            crate::sbio::ComponentAction::Load => {
                let Some(blob) = self.read_stored(kind, what) else {
                    return RestoreOutcome::NoStoredFile;
                };

                let at = crate::sbio::SBIO_SAVED_USER_ID_AT;
                if blob.len() < at + 4 {
                    return RestoreOutcome::Failed;
                }
                let carried =
                    i32::from_le_bytes([blob[at], blob[at + 1], blob[at + 2], blob[at + 3]]);
                if carried != id {
                    return RestoreOutcome::Failed;
                }

                let Some(request) = crate::sbio::SbioLoadCatacomb::new(&blob) else {
                    return RestoreOutcome::Failed;
                };

                match self.sbio_transfer_raw(request.opcode(), request.name(), request.payload()) {
                    Ok(done) if done.status.is_ok() => {
                        self.confirm_active(id, what, LoadAnswer::StatusZero)
                    }
                    Ok(done)
                        if done.status.answered()
                            == Some(crate::sbio::SBIO_STATUS_COLD_TRANSITION)
                            && cold_ok =>
                    {
                        self.confirm_active(id, what, LoadAnswer::ColdTransition)
                    }
                    Ok(done)
                        if done.status.answered()
                            == Some(crate::sbio::SBIO_STATUS_COLD_TRANSITION) =>
                    {
                        RestoreOutcome::Failed
                    }
                    // 0x101 ALREADY_ACTIVE is a success
                    Ok(done)
                        if done.status.answered()
                            == Some(crate::sbio::SBIO_STATUS_ALREADY_ACTIVE) =>
                    {
                        self.confirm_active(id, what, LoadAnswer::AlreadyActive)
                    }
                    _ => RestoreOutcome::Failed,
                }
            }
        }
    }

    fn confirm_active(&self, id: i32, what: &CStr, answer: LoadAnswer) -> RestoreOutcome {
        let Some(reply) = self.read_component_states() else {
            dev_err!(
                self.dev,
                "sbio: the {} answered {} and 0x6b could not be re-read; treating unknown as failure\n",
                what,
                answer.describe()
            );
            return RestoreOutcome::Failed;
        };
        let states = crate::sbio::ComponentStates::new(&reply);

        match states.state_for(id) {
            Some(state) if state & crate::sbio::COMPONENT_STATE_ACTIVE != 0 => {
                RestoreOutcome::Restored
            }
            Some(state) if state & crate::sbio::COMPONENT_STATE_COLD != 0 => RestoreOutcome::Failed,
            Some(_) => RestoreOutcome::Failed,
            None => RestoreOutcome::Failed,
        }
    }

    fn restore_lockout(&self) -> RestoreOutcome {
        let Some(blob) = self.read_stored(PRIVATE_TYPE_LOCKOUT, c"bio lockout record") else {
            return RestoreOutcome::NoStoredFile;
        };
        let Some(request) = crate::sbio::SbioLoadLockout::new(&blob) else {
            return RestoreOutcome::Failed;
        };
        match self.sbio_transfer_raw(request.opcode(), request.name(), request.payload()) {
            Ok(done) if done.status.is_ok() => RestoreOutcome::Restored,
            Ok(done)
                if matches!(
                    done.status,
                    transfer::DeviceStatus::Answered(crate::sbio::SBIO_STATUS_COLD_TRANSITION)
                ) =>
            {
                RestoreOutcome::EmptyTolerated
            }
            Ok(_) => RestoreOutcome::Failed,
            Err(_) => RestoreOutcome::Failed,
        }
    }

    fn read_stored(&self, kind: u8, _what: &CStr) -> Option<KVec<u8>> {
        if crate::catacomb::is_kind(kind) {
            if let Some(blob) = crate::catacomb::read(kind) {
                return Some(blob);
            }
        }
        match self.with_host_store(|store| store.read(&store::Key::root(kind))) {
            Some(Ok(Some(blob))) => Some(blob),
            Some(Ok(None)) => None,
            Some(Err(_)) => None,
            None => None,
        }
    }

    fn save_all_components(&self, user: crate::sbio::UserId) -> bool {
        let mut all = true;
        for (who, kind, what) in [
            (
                crate::sbio::CatacombUser::MASTER,
                PRIVATE_TYPE_CATACOMB_MASTER,
                c"master catacomb (user -1)",
            ),
            (
                crate::sbio::CatacombUser::OWNER,
                PRIVATE_TYPE_CATACOMB_OWNER,
                c"owner catacomb (user 501)",
            ),
            (
                crate::sbio::CatacombUser::enrolling(user),
                PRIVATE_TYPE_CATACOMB_USER,
                c"user catacomb",
            ),
        ] {
            if !self.save_catacomb(who, kind, what) {
                all = false;
            }
        }
        if !self.save_lockout() {
            all = false;
        }
        all
    }

    fn save_after_match(&self, user: crate::sbio::UserId) -> bool {
        if !self.save_lockout() {
            dev_err!(
                self.dev,
                "verify: post-match lockout persistence failed; refusing to claim the AP and SEP anti-replay state are synchronized\n"
            );
            return false;
        }

        if !self.save_catacomb(
            crate::sbio::CatacombUser::enrolling(user),
            PRIVATE_TYPE_CATACOMB_USER,
            c"post-match user catacomb",
        ) {
            dev_err!(
                self.dev,
                "verify: post-match user-catacomb persistence failed; the next boot may reject the older AP blob\n"
            );
            return false;
        }

        if !self.save_catacomb(
            crate::sbio::CatacombUser::MASTER,
            PRIVATE_TYPE_CATACOMB_MASTER,
            c"post-match system (id -1) material snapshot",
        ) {
            dev_err!(
                self.dev,
                "verify: post-match system material snapshot failed; next boot may reinstall pre-match material and the user load could cold-transition (0x8002)\n"
            );
            return false;
        }

        if !self.resnapshot_identity_keybag() {
            dev_err!(
                self.dev,
                "verify: post-match identity-bag snapshot failed; the newly confirmed catacomb has no matching durable bag image\n"
            );
            return false;
        }

        true
    }

    fn save_catacomb(&self, who: crate::sbio::CatacombUser, kind: u8, _what: &CStr) -> bool {
        let selector = crate::sbio::SaveSelector::new(who);

        let Some(blob) = self.sbio_expect_ok(&crate::sbio::sbio_save_catacomb(&selector)) else {
            return false;
        };
        if blob.len() < crate::sbio::SBIO_SAVED_MIN || blob.len() > crate::sbio::SBIO_SAVED_MAX {
            return false;
        }

        let at = crate::sbio::SBIO_SAVED_USER_ID_AT;
        let carried = i32::from_le_bytes([blob[at], blob[at + 1], blob[at + 2], blob[at + 3]]);
        if carried != who.value() {
            return false;
        }

        if crate::catacomb::write(kind, &blob).is_err() {
            return false;
        }

        if self
            .sbio_expect_ok(&crate::sbio::sbio_confirm_save(&selector))
            .is_none()
        {
            return false;
        }
        true
    }

    fn save_lockout(&self) -> bool {
        let Some(blob) = self.sbio_expect_ok(&crate::sbio::sbio_save_lockout()) else {
            return false;
        };
        if blob.is_empty() {
            return false;
        }
        match self
            .with_host_store(|store| store.write(&store::Key::root(PRIVATE_TYPE_LOCKOUT), &blob))
        {
            Some(Ok(())) => true,
            Some(Err(_)) => false,
            None => false,
        }
    }

    fn enable_sbio(&self) -> Result<()> {
        if self.sbio_ready.load(Relaxed) {
            return Ok(());
        }

        self.register_ool(&self.ool_sbio)?;

        self.sbio_ready.store(true, Relaxed);
        Ok(())
    }

    fn attach_bringup(&self) {
        if !self.sensor_present.load(Relaxed) {
            dev_warn!(
                self.dev,
                "sbio: no sensor bound at attach; catacomb restore ran but capture is unavailable\n"
            );
            return;
        }
        let Some(patch) = self.wake_sensor() else {
            return;
        };
        if !self.complete_bringup(patch) {
            dev_warn!(
                self.dev,
                "sbio: attach-time sensor bring-up did not complete; matching remains unavailable until a later bring-up succeeds.\n"
            );
        }
    }

    fn bring_sensor_online(&self) -> bool {
        let Some(mut patch) = self.wake_sensor() else {
            return false;
        };

        if !self.sensor_calibrated.load(Relaxed) {
            if !self.calibrate_sensor() {
                return false;
            }
            self.sensor_calibrated.store(true, Relaxed);
            let Some(reloaded_patch) = self.wake_sensor() else {
                return false;
            };
            patch = reloaded_patch;
        }

        self.complete_bringup(patch)
    }

    pub(crate) fn run_bringup(&self) {
        if self.bringup_started.xchg(true, Relaxed) {
            return;
        }
        if let Err(e) = self.enable_sks() {
            dev_warn!(self.dev, "bringup: could not enable the key store ({:?}); keybag and ref-key operations are unavailable\n", e);
            return;
        }
        if *module_parameters::provision_keybag.value() != 0 {
            if *module_parameters::xart_writes.value() == 0 {
                dev_err!(
                    self.dev,
                    "bringup: provision_keybag=1 requires xart_writes=1\n"
                );
                return;
            }
            if !self.sks_provision_identity_keybag() {
                dev_err!(
                    self.dev,
                    "bringup: identity-keybag provisioning failed; no retry this boot\n"
                );
                return;
            }
        }
        if let Err(e) = self.register_bio() {
            dev_err!(
                self.dev,
                "bringup: could not publish /dev/{} after SKS became ready: {:?}\n",
                bio::DEVICE_NAME,
                e
            );
        }
    }

    fn activate_touchid(&self) -> Result<()> {
        if self.touchid_started.load(Relaxed) {
            return Ok(());
        }
        if self.touchid_failed.load(Relaxed) {
            return Err(EIO);
        }

        let mut store = store::Store::open().inspect_err(|e| {
            dev_err!(
                self.dev,
                "Touch ID: opening the host state failed: {:?}\n",
                e
            );
        })?;
        let index = bio::IdentityIndex::load(&mut store).inspect_err(|e| {
            dev_err!(
                self.dev,
                "Touch ID: loading the host identity index failed: {:?}\n",
                e
            );
        })?;
        *self.host_store.lock() = Some(store);
        *self.bio_index.lock() = index;

        self.attach_sensor();
        self.enable_sbio().inspect_err(|e| {
            dev_err!(
                self.dev,
                "Touch ID: registering the biometric buffers failed: {:?}\n",
                e
            );
        })?;
        let stored = match keybag::read(keybag::Slot::Identity) {
            Ok(keybag::State::Present(stored)) => stored,
            Ok(keybag::State::Absent(_)) => {
                dev_err!(self.dev, "Touch ID: no persisted identity keybag exists\n");
                return Err(ENOENT);
            }
            Err(e) => {
                dev_err!(
                    self.dev,
                    "Touch ID: reading the persisted identity keybag failed: {:?}\n",
                    e
                );
                return Err(e);
            }
        };
        if !self.sks_ready() {
            dev_err!(
                self.dev,
                "Touch ID: the key-store endpoint is unavailable\n"
            );
            return Err(ENODEV);
        }
        let (handle, uuid) = self.sks_recover(&stored).ok_or_else(|| {
            dev_err!(
                self.dev,
                "Touch ID: the persisted identity keybag did not recover\n"
            );
            EIO
        })?;
        self.sks_designate_user_keybag(handle, stored.secret());
        self.sks_machine_refkey(handle, stored.secret());
        let prepared = self.cold_match_continue(handle, uuid);
        self.ensure_restored_after(prepared);
        self.attach_bringup();
        self.touchid_started.store(true, Relaxed);
        Ok(())
    }

    fn prepare_bio_open(&self) -> Result<()> {
        if let Err(e) = self.activate_touchid() {
            self.touchid_failed.store(true, Relaxed);
            dev_err!(
                self.dev,
                "Touch ID activation failed: {:?}; refusing retries this boot because a partial keybag load is not safely repeatable\n",
                e
            );
            return Err(e);
        }
        Ok(())
    }

    pub(crate) fn run_verify(&self) {
        if !self.templates_restored.load(Relaxed) {
            dev_err!(
                self.dev,
                "verify: refusing before touching the sensor; cold-match prep or restore did not complete, enclave holds no template (every match refused 0x1). A restore failure, not a non-matching finger\n"
            );
            self.finish_verify(
                bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE),
                [0u8; bio::TOKEN_LEN],
            );
            return;
        }

        let Some(token_bytes) = self.mint_token_bytes() else {
            self.finish_verify(
                bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE),
                [0u8; bio::TOKEN_LEN],
            );
            return;
        };

        self.settle_before_capture();

        // The enclave refuses a capture from an uncalibrated sensor (0x65 answers 1).
        if !self.bring_sensor_online() {
            self.finish_verify(bio::VerifyOutcome::Failed(ENROL_STATUS_SENSOR), token_bytes);
            let _ = sensor::idle();
            return;
        }

        // The match uses the plain 0x23->0x06->0x65 pipeline, with no SCRD step.

        let outcome = self.verify_one_image();
        let _ = sensor::idle();
        sensor::power(false);
        self.note_capture_end();

        let definitive = matches!(
            &outcome,
            bio::VerifyOutcome::Matched(_) | bio::VerifyOutcome::NoMatch
        );
        if definitive {
            let Some(user) = crate::sbio::UserId::new(SBIO_PROBE_USER_ID) else {
                self.finish_verify(
                    bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE),
                    token_bytes,
                );
                return;
            };
            if !self.save_after_match(user) {
                self.finish_verify(
                    bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE),
                    token_bytes,
                );
                return;
            }
        }
        self.finish_verify(outcome, token_bytes);
    }

    fn verify_one_image(&self) -> bio::VerifyOutcome {
        let Some(user) = crate::sbio::UserId::new(SBIO_PROBE_USER_ID) else {
            return bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE);
        };

        if sensor::start_capture().is_err() {
            return bio::VerifyOutcome::Failed(ENROL_STATUS_SENSOR);
        }
        let advertised = match self.await_capture() {
            CaptureWait::Ready(n) => n,
            CaptureWait::Timeout => {
                return bio::VerifyOutcome::Failed(ENROL_STATUS_TIMEOUT);
            }
            CaptureWait::Fault(_state) => {
                return bio::VerifyOutcome::Failed(ENROL_STATUS_SENSOR);
            }
            CaptureWait::Abandon => return bio::VerifyOutcome::Failed(ENROL_STATUS_SENSOR),
        };

        let capture = match sensor::read_capture(advertised) {
            Ok(capture) => capture,
            Err(sensor::CaptureError::Checksum { .. }) | Err(sensor::CaptureError::Length(_)) => {
                return bio::VerifyOutcome::Failed(ENROL_STATUS_RETRY);
            }
            Err(sensor::CaptureError::Bus(_)) => {
                return bio::VerifyOutcome::Failed(ENROL_STATUS_SENSOR);
            }
            Err(sensor::CaptureError::NoMemory) => {
                return bio::VerifyOutcome::Failed(ENROL_STATUS_SENSOR)
            }
        };

        if self
            .sbio_expect_ok(&crate::sbio::sbio_prepare_image_processing())
            .is_none()
        {
            return bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE);
        }
        let _image = ImageContext { sep: self };

        // MATCHING: byte 0x12 set, 0x13 clear; last-image flag at byte 9 (enrol byte 8)
        let init = crate::sbio::sbio_image_processing_init(
            crate::sbio::ImagePurpose::Matching,
            true,
            true,
            0,
            user,
            crate::shim::monotonic_ns(),
        );
        if self.sbio_expect_ok(&init).is_none() {
            return bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE);
        }

        let relay = crate::sbio::SbioRelay::new(&capture);
        if self.sbio_relay(&relay).is_none() {
            dev_err!(
                self.dev,
                "verify: enclave refused the capture (status above); no comparison performed, not a non-match. Reported as a failure so userspace does not tell the user their finger was rejected\n"
            );
            return bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE);
        }
        drop(capture);

        let Some(assessment) = self.sbio_expect_ok(&crate::sbio::sbio_image_assessment()) else {
            return bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE);
        };
        if assessment.len() <= crate::sbio::ASSESS_USABLE_MATCH {
            return bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE);
        }

        if assessment[crate::sbio::ASSESS_USABLE_MATCH] == 0 {
            return bio::VerifyOutcome::Failed(ENROL_STATUS_RETRY);
        }

        let Some(result) = self.sbio_expect_ok(&crate::sbio::sbio_match_result()) else {
            return bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE);
        };
        let Some(parsed) = crate::sbio::MatchResult::parse(&result) else {
            return bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE);
        };

        if !parsed.matches(user) {
            return bio::VerifyOutcome::NoMatch;
        }

        // match result: user id groups templates, UUID at offset 0x04 identifies one
        let identity = *parsed.identity_uuid();
        let known = self.bio_index.lock().contains_uuid(&identity);

        if !known {
            return bio::VerifyOutcome::Failed(ENROL_STATUS_ENCLAVE);
        }

        bio::VerifyOutcome::Matched(bio::MatchEvidence::from_enclave_reply(identity))
    }

    pub(crate) fn run_enrolment(&self) {
        if !self.bring_sensor_online() {
            self.finish_enrolment(Err(ENROL_STATUS_SENSOR));
            let _ = sensor::idle();
            return;
        }

        let Some(open) = self.begin_enrolment_on_enclave() else {
            self.finish_enrolment(Err(ENROL_STATUS_ENCLAVE));
            let _ = sensor::idle();
            return;
        };

        let mut counter: u32 = 0;
        let mut enrolment_completed = false;
        let mut identity: Option<[u8; bio::UUID_LEN]> = None;

        let outcome = loop {
            if !bio::enrol_is_live(&self.bio_session.lock()) {
                break None;
            }
            if counter >= ENROL_MAX_CAPTURES {
                break Some(Err(ENROL_STATUS_TOO_MANY));
            }

            match self.enrol_one_image(counter) {
                ImageOutcome::Progress {
                    stage,
                    percent,
                    complete,
                    has_template,
                } => {
                    counter = counter.saturating_add(1);
                    let woke = bio::enrol_advance(
                        &mut self.bio_session.lock(),
                        stage,
                        percent,
                        bio::Guidance::LiftAndMove,
                    );
                    if woke {
                        self.bio_wake();
                    }
                    // completion is the flag at offset 0xbfe, not a derived stage count
                    if complete {
                        if !has_template {
                            break Some(Err(ENROL_STATUS_ENCLAVE));
                        }
                        enrolment_completed = true;
                        break Some(Ok(()));
                    }
                }
                ImageOutcome::Retry => {
                    counter = counter.saturating_add(1);
                }
                ImageOutcome::NoFinger => {
                    break Some(Err(ENROL_STATUS_TIMEOUT));
                }
                ImageOutcome::Failed(status) => break Some(Err(status)),
            }

            self.pace_between_captures();
        };

        if enrolment_completed {
            open.completed();

            if let Some(user) = crate::sbio::UserId::new(SBIO_PROBE_USER_ID) {
                if !self.save_all_components(user) {
                    dev_warn!(
                        self.dev,
                        "enrol: enrolment succeeded but at least one of four artefacts was not persisted; works until the next reboot (see which component above)\n"
                    );
                } else {
                    self.templates_restored.store(true, Relaxed);
                    if !self.resnapshot_identity_keybag() {
                        dev_warn!(
                            self.dev,
                            "enrol: catacombs saved but post-commit identity-bag snapshot was not; template works this boot but is not reboot-persistent\n"
                        );
                    }
                }
            }

            identity = self.identity_just_enrolled();

            self.reconcile_identities();
        }

        let _ = sensor::idle();
        sensor::power(false);
        self.note_capture_end();

        if let Some(result) = outcome {
            let filed = match (result, identity) {
                (Ok(()), Some(uuid)) => Ok(uuid),
                (Ok(()), None) => Err(ENROL_STATUS_UNFILED),
                (Err(status), _) => Err(status),
            };
            self.finish_enrolment(filed);
        }
    }

    fn register_sensor(&self, id: &sensor::Identifier) -> bool {
        let stage = self.bringup.load(Relaxed);

        if stage >= BRINGUP_ESTABLISHED {
            if self
                .sbio_expect_ok(&crate::sbio::sbio_clear_state())
                .is_none()
            {
                return false;
            }
        }

        if stage == BRINGUP_FRESH {
            let op = crate::sbio::sbio_register_sensor(id);
            match self.sbio_call(&op) {
                SbioOutcome::Ok(_payload) => {
                    self.bringup.store(BRINGUP_IDENTIFIED, Relaxed);
                }
                _ => {
                    return false;
                }
            }
        }

        let serial = match sensor::read_sensor_serial() {
            Ok(serial) => serial,
            Err(_) => {
                return false;
            }
        };

        let op = crate::sbio::sbio_register_sensor_serial(&serial);
        match self.sbio_call(&op) {
            SbioOutcome::Ok(_payload) => {
                self.bringup.store(BRINGUP_ESTABLISHED, Relaxed);
                true
            }
            _ => false,
        }
    }

    fn wake_sensor(&self) -> Option<PatchLoaded> {
        let _source = sensor::power_source();
        let cycled = sensor::power_cycle();
        if !sensor::cs_timing().is_hardware() {
            dev_warn!(
                self.dev,
                "sensor: chip-select timing not reaching hardware; software emulation cannot time this sensor, silent status expected (mode 2 is the other half)\n"
            );
        }

        let mut answered_without_identifier = false;

        for delay in sensor::POWER_ON_READ_DELAYS_MS {
            if delay > 0 {
                kernel::time::delay::fsleep(kernel::time::Delta::from_millis(i64::from(delay)));
            }
            let st = match sensor::status() {
                Ok(st) => st,
                Err(_) => {
                    return None;
                }
            };
            if st.is_silent() {
                continue;
            }

            if let sensor::Offset12::Identifier(id) = st.offset12() {
                if id != 0 && !sensor::KNOWN_IDENTIFIERS.contains(&id) {
                    return None;
                }
            }

            let Some(id) = st.identifier_to_register() else {
                answered_without_identifier = true;
                continue;
            };
            if !self.register_sensor(&id) {
                return None;
            }

            // The patch decision is made on status[8], not on state 9.
            if st.patch_ack() == sensor::PATCH_ACCEPTED {
                return Some(PatchLoaded(()));
            }
            return self.apply_sensor_patch(st.patch_ack());
        }

        dev_err!(
            self.dev,
            "sensor gated: all {} wake reads returned sixteen zero bytes. Power {}. CS timing: {}. Mode 2 (CPOL=1 CPHA=0)\n",
            sensor::POWER_ON_READ_DELAYS_MS.len(),
            if cycled { c"was cycled" } else { c"was NOT cycled" },
            sensor::cs_timing().name()
        );
        if answered_without_identifier {
            return None;
        }
        None
    }

    fn calibrate_sensor(&self) -> bool {
        let blob = match kernel::firmware::Firmware::request(CALIBRATION_FIRMWARE, &self.dev) {
            Ok(fw) => CalibrationBlob::new(fw),
            Err(e) => {
                dev_err!(
                    self.dev,
                    "sensor: calibration blob {} could not be loaded ({:?}); per-device factory data, cannot be synthesised, stopping\n",
                    CALIBRATION_FIRMWARE,
                    e
                );
                return false;
            }
        };

        let Some(challenge) = self.sbio_expect_ok(&crate::sbio::sbio_module_challenge()) else {
            return false;
        };
        if challenge.len() != crate::sbio::SBIO_MODULE_CHALLENGE_LEN {
            return false;
        }

        let challenge = sensor::Params::new(challenge);
        if sensor::send_module_challenge(&challenge).is_err() {
            return false;
        }

        let reply = match sensor::read_module_reply() {
            Ok(reply) => reply,
            Err(_) => {
                return false;
            }
        };

        // commit the sensor's reply, not the challenge; both are 64 bytes at count 0x4b
        let Some(commit) = crate::sbio::sbio_module_commit(&reply) else {
            return false;
        };
        let Some(_serial) = self.sbio_expect_ok(&commit) else {
            return false;
        };

        let request = match crate::sbio::SbioCalibration::new(&blob) {
            Ok(request) => request,
            Err(_) => {
                return false;
            }
        };
        if self
            .sbio_transfer_raw(request.opcode(), request.name(), request.payload())
            .ok()
            .filter(|done| done.status.is_ok())
            .is_none()
        {
            dev_err!(
                self.dev,
                "sensor: 0x5b LOAD_CALIBRATION failed. Without it every capture is refused with status 1.\n"
            );
            return false;
        }

        let _ = self.sbio_call(&crate::sbio::sbio_diagnostics());

        true
    }

    fn complete_bringup(&self, patch: PatchLoaded) -> bool {
        let Some(params) = self.apply_sensor_parameters() else {
            return false;
        };

        let op = crate::sbio::sbio_complete_init(patch, params);
        if self.sbio_expect_ok(&op).is_none() {
            dev_err!(
                self.dev,
                "sensor: 0x01 COMPLETE_INIT failed — status above\n"
            );
            return false;
        }

        let listed = self.sync_device_view();

        if listed && !self.device_view_synced.xchg(true, Relaxed) {
            let count = self.log_identity_count(c"after device-view synchronisation");
            match count {
                Some(0) => {
                    self.templates_restored.store(false, Relaxed);
                }
                Some(n) if self.templates_restored.load(Relaxed) && self.prove_restore() => {
                    dev_warn!(
                        self.dev,
                        "Touch ID: restored {} enrolled identity/identities; enrolment survived reboot\n",
                        n
                    );
                }
                Some(_) => {
                    self.templates_restored.store(false, Relaxed);
                }
                None => {
                    self.templates_restored.store(false, Relaxed);
                }
            }
            if self.templates_restored.load(Relaxed) {
                self.reconcile_identities();
            }
        }

        true
    }

    fn apply_sensor_parameters(&self) -> Option<ParametersApplied> {
        // 0x5d and 0x5c: an empty blob is a failure.
        self.relay_parameters(
            &crate::sbio::sbio_coverage_params(),
            &sensor::Geometry::COVERAGE,
            false,
        )?;
        self.relay_parameters(
            &crate::sbio::sbio_operation_params(),
            &sensor::Geometry::OPERATION,
            false,
        )?;
        // 0x6a: an empty blob is success, and relaying nothing is correct.
        self.relay_parameters(
            &crate::sbio::sbio_transparent_channel(),
            &sensor::Geometry::TRANSPARENT,
            true,
        )?;

        Some(ParametersApplied(()))
    }

    fn relay_parameters(
        &self,
        op: &crate::sbio::SbioOp,
        geom: &sensor::Geometry,
        empty_is_success: bool,
    ) -> Option<usize> {
        let blob = match self.sbio_call(op) {
            SbioOutcome::Ok(b) => sensor::Params::new(b),
            _ => {
                return None;
            }
        };

        if blob.is_empty() {
            if empty_is_success {
                return Some(0);
            }
            return None;
        }

        let relayed = blob.len();
        match sensor::send_encrypted_parameters(&blob, geom) {
            Ok(()) => Some(relayed),
            Err(sensor::ParamsError::Empty) => None,
            Err(sensor::ParamsError::TooLong(n, capacity)) => {
                dev_err!(
                    self.dev,
                    "sensor: {}'s blob is {} bytes and this frame holds {} (declared 0x{:x} less the 9 bytes of header and CRC)\n",
                    op.name(),
                    n,
                    capacity,
                    geom.declared()
                );
                None
            }
            Err(sensor::ParamsError::Transfer(_e)) => None,
        }
    }

    fn establish_session(&self) -> bool {
        let share = match self.sbio_call(&crate::sbio::sbio_request_session_share()) {
            SbioOutcome::Ok(sh) => sh,
            SbioOutcome::Status16 => {
                return false;
            }
            SbioOutcome::PrerequisiteMissing => {
                return false;
            }
            SbioOutcome::Other => {
                return false;
            }
        };

        // 0x30 reserved for this reply; length is a floor, take the first 40
        if share.len() < sensor::SESSION_SHARE_LEN {
            return false;
        }
        let mut out = [0u8; sensor::SESSION_SHARE_LEN];
        out.copy_from_slice(&share[..sensor::SESSION_SHARE_LEN]);

        // relay it: class 0x72, no CRC, no padding
        if sensor::send_session_share(&out).is_err() {
            return false;
        }

        let reply = match sensor::read_session_reply() {
            Ok(r) => r,
            Err(_) => {
                return false;
            }
        };

        match self.sbio_transfer(&crate::sbio::sbio_commit_session_share(&reply)) {
            Ok(done) if done.status.is_ok() => true,
            Ok(_) => false,
            Err(_) => false,
        }
    }

    fn init_sequence_counter(&self) -> bool {
        let challenge = match self.sbio_call(&crate::sbio::sbio_request_challenge()) {
            SbioOutcome::Ok(c) => c,
            SbioOutcome::PrerequisiteMissing => {
                if !self.establish_session() {
                    return false;
                }
                match self.sbio_call(&crate::sbio::sbio_request_challenge()) {
                    SbioOutcome::Ok(c) => c,
                    _ => {
                        return false;
                    }
                }
            }
            SbioOutcome::Status16 => {
                return false;
            }
            SbioOutcome::Other => {
                return false;
            }
        };
        if challenge.len() < sensor::CHALLENGE_LEN {
            return false;
        }
        let mut out = [0u8; sensor::CHALLENGE_LEN];
        out.copy_from_slice(&challenge[..sensor::CHALLENGE_LEN]);

        if sensor::send_challenge(&out).is_err() {
            return false;
        }

        let reply = match sensor::read_challenge_reply() {
            Ok(r) => r,
            Err(_) => {
                return false;
            }
        };

        match self.sbio_transfer(&crate::sbio::sbio_commit_challenge(&reply)) {
            Ok(done) if done.status.is_ok() => true,
            Ok(_) => false,
            Err(_) => false,
        }
    }

    fn apply_sensor_patch(&self, _ack_before: u8) -> Option<PatchLoaded> {
        let blob = match self.sbio_call(&crate::sbio::sbio_fetch_patch()) {
            SbioOutcome::Ok(b) => b,
            SbioOutcome::PrerequisiteMissing => {
                if !self.init_sequence_counter() {
                    return None;
                }
                match self.sbio_call(&crate::sbio::sbio_fetch_patch()) {
                    SbioOutcome::Ok(b) => b,
                    _ => {
                        return None;
                    }
                }
            }
            SbioOutcome::Status16 => {
                return None;
            }
            SbioOutcome::Other => {
                return None;
            }
        };
        if blob.is_empty() {
            return None;
        }

        // enable command is mandatory; skipping it leaves the sensor in state 9
        if sensor::setup_patch_enable().is_err() {
            return None;
        }

        if !self.await_sensor_state(sensor::STATE_IDLE, c"idle, before sending the patch") {
            return None;
        }

        if sensor::send_patch(&blob).is_err() {
            return None;
        }

        // acceptance is at status[8], not at the state
        for _attempt in 0..PATCH_POLL_ATTEMPTS {
            kernel::time::delay::fsleep(kernel::time::Delta::from_millis(i64::from(PATCH_POLL_MS)));
            let st = match sensor::status() {
                Ok(st) => st,
                Err(_) => {
                    return None;
                }
            };
            if st.patch_ack() == sensor::PATCH_ACCEPTED {
                if let Ok(after) = sensor::status() {
                    if after.state == sensor::STATE_NEEDS_PATCH {
                        return None;
                    }
                }
                return Some(PatchLoaded(()));
            }
        }

        let _ = sensor::status();
        None
    }

    fn await_sensor_state(&self, want: u8, _why: &CStr) -> bool {
        for _attempt in 0..PATCH_POLL_ATTEMPTS {
            match sensor::status() {
                Ok(st) if st.state == want => {
                    return true;
                }
                Ok(_) => {}
                Err(_) => {
                    return false;
                }
            }
            kernel::time::delay::fsleep(kernel::time::Delta::from_millis(i64::from(PATCH_POLL_MS)));
        }
        false
    }

    fn enrol_one_image(&self, counter: u32) -> ImageOutcome {
        if sensor::start_capture().is_err() {
            return ImageOutcome::Failed(ENROL_STATUS_SENSOR);
        }

        let available = match self.await_capture() {
            CaptureWait::Ready(n) => n,
            CaptureWait::Timeout => return ImageOutcome::NoFinger,
            CaptureWait::Fault(_) => {
                return ImageOutcome::Failed(ENROL_STATUS_SENSOR);
            }
            CaptureWait::Abandon => return ImageOutcome::Failed(ENROL_STATUS_SENSOR),
        };

        let capture = match sensor::read_capture(available) {
            Ok(c) => c,
            Err(sensor::CaptureError::Checksum {
                advertised: _advertised,
                computed: _computed,
            }) => {
                return ImageOutcome::Retry;
            }
            Err(sensor::CaptureError::Length(_n)) => {
                return ImageOutcome::Retry;
            }
            Err(sensor::CaptureError::Bus(_e)) => {
                return ImageOutcome::Failed(ENROL_STATUS_SENSOR);
            }
            Err(sensor::CaptureError::NoMemory) => {
                return ImageOutcome::Failed(ENROL_STATUS_SENSOR)
            }
        };

        let Some(user) = crate::sbio::UserId::new(SBIO_PROBE_USER_ID) else {
            return ImageOutcome::Failed(ENROL_STATUS_ENCLAVE);
        };
        if self
            .sbio_expect_ok(&crate::sbio::sbio_prepare_image_processing())
            .is_none()
        {
            return ImageOutcome::Failed(ENROL_STATUS_ENCLAVE);
        }
        let _image = ImageContext { sep: self };

        let init = crate::sbio::sbio_image_processing_init(
            crate::sbio::ImagePurpose::Enrolment,
            counter == 0,
            false,
            counter,
            user,
            crate::shim::monotonic_ns(),
        );
        if self.sbio_expect_ok(&init).is_none() {
            return ImageOutcome::Failed(ENROL_STATUS_ENCLAVE);
        }

        let relay = crate::sbio::SbioRelay::new(&capture);
        if self.sbio_relay(&relay).is_none() {
            return ImageOutcome::Failed(ENROL_STATUS_ENCLAVE);
        }
        drop(capture);

        let Some(assessment) = self.sbio_expect_ok(&crate::sbio::sbio_image_assessment()) else {
            return ImageOutcome::Failed(ENROL_STATUS_ENCLAVE);
        };
        if assessment.len() < crate::sbio::ASSESS_MIN_LEN {
            return ImageOutcome::Failed(ENROL_STATUS_ENCLAVE);
        }
        let usable = assessment[crate::sbio::ASSESS_USABLE_ENROL] != 0;
        if !usable {
            return ImageOutcome::Retry;
        }

        let Some(result) = self.sbio_expect_ok(&crate::sbio::sbio_enrolment_result()) else {
            return ImageOutcome::Failed(ENROL_STATUS_ENCLAVE);
        };
        let Some(parsed) = crate::sbio::EnrolmentResult::parse(&result) else {
            return ImageOutcome::Failed(ENROL_STATUS_ENCLAVE);
        };

        if parsed.complete() {
            self.stash_enrol_identity(&result);
        }

        let percent = parsed.progress_percent();

        ImageOutcome::Progress {
            stage: (percent * bio::ENROL_STAGES)
                .div_ceil(100)
                .min(bio::ENROL_STAGES),
            percent,
            complete: parsed.complete(),
            has_template: parsed.has_template(),
        }
    }

    fn await_capture(&self) -> CaptureWait {
        let mut previous: Option<[u8; sensor::STATUS_LEN]> = None;
        let mut armed_reported = false;

        for attempt in 0..ENROL_POLL_ATTEMPTS {
            if !bio::capture_is_live(&self.bio_session.lock()) {
                return CaptureWait::Abandon;
            }

            let st = match sensor::status() {
                Ok(st) => st,
                Err(_) => {
                    return CaptureWait::Abandon;
                }
            };

            if attempt == 0 || previous != Some(st.raw) {
                previous = Some(st.raw);
            }

            if st.state == sensor::STATE_NEEDS_PATCH {
                return CaptureWait::Fault(st.state);
            }

            let guide = match st.state {
                sensor::STATE_ARMED => Some(bio::Guidance::Place),
                sensor::STATE_READING => Some(bio::Guidance::HoldStill),
                _ => None,
            };
            if let Some(guide) = guide {
                if bio::enrol_guide(&mut self.bio_session.lock(), guide) {
                    self.bio_wake();
                }
            }

            if st.state == sensor::STATE_ARMED && !armed_reported {
                armed_reported = true;
            }

            if let sensor::Offset12::Available(count) = st.offset12() {
                if count == 0 {
                    return CaptureWait::Timeout;
                }
                return CaptureWait::Ready(count);
            }

            kernel::time::delay::fsleep(kernel::time::Delta::from_millis(i64::from(ENROL_POLL_MS)));
        }
        let _ = sensor::status();
        CaptureWait::Timeout
    }

    pub(crate) fn on_sbio(&self, msg: Message) {
        let f = proto::decode(&msg);
        let marker = f.tag;

        if !self.sbio_ready.load(Relaxed) {
            return;
        }

        // notification and the enclave's DMA are not ordered; buffer may still be poison
        if !self.ool_await_written(&self.ool_sbio, 0, transfer::HEADER_LEN) {
            self.sbio_fail_unwritten();
            return;
        }

        let header = match self.ool_read(&self.ool_sbio, 0, transfer::HEADER_LEN) {
            Ok(h) => h,
            Err(_) => {
                self.sbio_fail();
                return;
            }
        };
        let packet = match transfer::Packet::decode(&header) {
            Ok(p) => p,
            Err(_) => {
                self.sbio_fail();
                return;
            }
        };

        let chunk = packet.chunk as usize;
        let payload = if chunk == 0 {
            KVec::new()
        } else if !self.ool_await_written(&self.ool_sbio, transfer::HEADER_LEN, chunk) {
            self.sbio_fail_unwritten();
            return;
        } else {
            match self.ool_read(&self.ool_sbio, transfer::HEADER_LEN, chunk) {
                Ok(p) => p,
                Err(_) => {
                    self.sbio_fail();
                    return;
                }
            }
        };

        let progress = self.sbio_rx.lock().on_chunk(marker, &packet, &payload);

        match progress {
            transfer::Progress::NeedMore(cont) => {
                self.sbio_continue(&cont);
            }
            transfer::Progress::Complete => {
                // 0xFE requests the peer's next packet and acks the final one; nothing to send
                self.sbio_wq.notify_all();
            }
            transfer::Progress::Ignored => {}
            transfer::Progress::Grant => {
                self.sbio_wq.notify_all();
            }
            transfer::Progress::Notification => {}
            transfer::Progress::Failed => {
                self.sbio_wq.notify_all();
            }
        }
    }

    fn sbio_continue(&self, cont: &transfer::Continuation) {
        let mut header = [0u8; transfer::HEADER_LEN];
        if cont.packet().encode(&mut header).is_err() {
            self.sbio_fail();
            return;
        }
        if self.ool_write(&self.ool_sbio, 0, &header).is_err() {
            self.sbio_fail();
            return;
        }
        if self.send(crate::sbio::encode_sbio_continue(cont)).is_err() {
            self.sbio_fail();
        }
    }

    fn sbio_fail(&self) {
        self.sbio_rx.lock().abort();
        self.sbio_wq.notify_all();
    }

    fn sbio_fail_unwritten(&self) {
        self.sbio_rx
            .lock()
            .abort_with(transfer::DeviceStatus::BufferNeverWritten);
        self.sbio_wq.notify_all();
    }

    fn sbio_transfer(&self, op: &crate::sbio::SbioOp) -> Result<transfer::Completed> {
        self.sbio_transfer_raw(op.opcode(), op.name(), op.payload())
    }

    fn sbio_transfer_raw(
        &self,
        opcode: u16,
        name: &'static CStr,
        payload: &[u8],
    ) -> Result<transfer::Completed> {
        if !self.sbio_ready.load(Relaxed) {
            return Err(ENODEV);
        }

        self.sbio_rx.lock().begin(u32::from(opcode))?;

        if let Err(e) = self.sbio_start(opcode, name, payload) {
            let mut guard = self.sbio_rx.lock();
            guard.abort();
            let _ = guard.take_done();
            return Err(e);
        }

        let mut remaining = time::msecs_to_jiffies(SBIO_TIMEOUT_MS);
        let mut guard = self.sbio_rx.lock();
        loop {
            if let Some(done) = guard.take_done() {
                return Ok(done);
            }
            if remaining == 0 {
                guard.abort();
                let _ = guard.take_done();
                drop(guard);
                return Err(ETIMEDOUT);
            }
            match self
                .sbio_wq
                .wait_interruptible_timeout(&mut guard, remaining)
            {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Timeout => remaining = 0,
            }
        }
    }

    fn sbio_start(&self, opcode: u16, name: &'static CStr, payload: &[u8]) -> Result<()> {
        if crate::sbio::encode_sbio_raw(opcode, 0).is_none() {
            return Err(EPERM);
        }

        let total = payload.len();
        if total > transfer::MAX_TRANSACTION as usize {
            return Err(EINVAL);
        }

        let capacity = OOL_SIZE_SBIO - transfer::HEADER_LEN;

        self.sbio_rx.lock().begin_send(u32::from(opcode));
        let result = self.sbio_send_chunks(opcode, name, payload, capacity);
        self.sbio_rx.lock().end_send();
        result
    }

    fn sbio_send_chunks(
        &self,
        opcode: u16,
        name: &'static CStr,
        payload: &[u8],
        capacity: usize,
    ) -> Result<()> {
        let total = payload.len();
        let mut offset = 0usize;
        let mut first = true;
        let mut seq: u16 = 0;
        let mut request = KVec::new();

        loop {
            let chunk = core::cmp::min(total - offset, capacity);

            let packet = transfer::Packet {
                version: 1,
                total: total as u32,
                offset: offset as u32,
                flags: 0,
                err: 0,
                opcode: u32::from(opcode),
                chunk: chunk as u32,
            };

            request.clear();
            request.resize(transfer::HEADER_LEN + chunk, 0u8, GFP_KERNEL)?;
            packet.encode(&mut request)?;
            request[transfer::HEADER_LEN..].copy_from_slice(&payload[offset..offset + chunk]);

            self.ool_write(&self.ool_sbio, 0, &request)?;

            let msg = if first {
                crate::sbio::encode_sbio_raw(opcode, seq)
            } else {
                crate::sbio::encode_sbio_next(opcode, seq)
            }
            .ok_or(EPERM)?;
            self.send(msg)?;

            offset += chunk;
            first = false;
            seq = seq.wrapping_add(1);
            if offset >= total {
                return Ok(());
            }

            self.sbio_await_grant(name, offset, total)?;
        }
    }

    fn sbio_await_grant(&self, _name: &'static CStr, _sent: usize, _total: usize) -> Result<()> {
        let mut remaining = time::msecs_to_jiffies(SBIO_TIMEOUT_MS);
        let mut guard = self.sbio_rx.lock();
        loop {
            if guard.take_grant() {
                return Ok(());
            }
            if remaining == 0 {
                drop(guard);
                return Err(ETIMEDOUT);
            }
            match self
                .sbio_wq
                .wait_interruptible_timeout(&mut guard, remaining)
            {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Timeout => remaining = 0,
            }
        }
    }

    pub(crate) fn register_bio(&self) -> Result<()> {
        let mut guard = self.bio_dev.lock();
        if guard.is_some() {
            return Ok(());
        }

        let ctx = core::ptr::from_ref(self).cast_mut().cast::<c_void>();

        // SAFETY: `ctx` is this `SepData`, kept alive by the `Arc` in the
        // driver's private data. `remove()` drops the registration before that
        // `Arc` can go, and the shim's unregister clears its stored context
        // under the same lock every callback takes, so once it returns, no
        // callback can observe this pointer again.
        let dev = unsafe {
            shim::BioChardev::register(
                bio::DEVICE_NAME,
                bio::DEVICE_MODE,
                ctx,
                bio_open_trampoline,
                bio_release_trampoline,
                bio_ioctl_trampoline,
                bio_ready_trampoline,
            )
        }?;

        *guard = Some(dev);
        Ok(())
    }

    pub(crate) fn bio_open(&self) -> Result<()> {
        let mut session = self.bio_session.lock();
        bio::open(&mut session)?;
        drop(session);
        if let Err(e) = self.prepare_bio_open() {
            bio::release(&mut self.bio_session.lock());
            return Err(e);
        }
        Ok(())
    }

    pub(crate) fn bio_release(&self) {
        bio::release(&mut self.bio_session.lock());
        if let Some(dev) = self.bio_dev.lock().as_ref() {
            dev.wake();
        }
    }

    pub(crate) fn queue_enrolment(this: Arc<SepData>) {
        if workqueue::system()
            .enqueue::<Arc<SepData>, ENROL_WORK_ID>(this.clone())
            .is_err()
        {
            this.finish_enrolment(Err(ENROL_STATUS_SENSOR));
        }
    }

    pub(crate) fn queue_verify(this: Arc<SepData>) {
        if workqueue::system()
            .enqueue::<Arc<SepData>, VERIFY_WORK_ID>(this.clone())
            .is_err()
        {
            this.finish_verify(
                bio::VerifyOutcome::Failed(ENROL_STATUS_SENSOR),
                [0u8; bio::TOKEN_LEN],
            );
        }
    }

    fn finish_verify(&self, outcome: bio::VerifyOutcome, token_bytes: [u8; bio::TOKEN_LEN]) {
        let filed = bio::verify_finish(&mut self.bio_session.lock(), outcome, token_bytes);
        if filed {
            self.bio_wake();
        }
    }

    fn mint_token_bytes(&self) -> Option<[u8; bio::TOKEN_LEN]> {
        let mut bytes = [0u8; bio::TOKEN_LEN];
        for chunk in bytes.chunks_mut(4) {
            match self.get_entropy_word() {
                Ok(word) => chunk.copy_from_slice(&word.to_le_bytes()[..chunk.len()]),
                Err(_) => {
                    return None;
                }
            }
        }
        Some(bytes)
    }

    pub(crate) fn bio_ioctl(&self, cmd: u32, arg: usize) -> Result<bio::Handled> {
        if cmd == bio::IOC_ATTEST {
            return self.bio_attest(arg);
        }
        let handled = {
            let mut session = self.bio_session.lock();
            let index = self.bio_index.lock();
            let mut ctx = bio::Context {
                session: &mut session,
                index: &index,
                sensor_present: self.sensor_present.load(Relaxed),
            };
            bio::ioctl(&mut ctx, cmd, arg)?
        };

        // 0x57 sent with the session lock released (it waits on a mailbox reply)
        if let Some(identity) = handled.delete_identity {
            self.delete_identity(&identity)?;
        }

        let mut delete_all_err: Option<Error> = None;
        for identity in handled.delete_identities.iter() {
            if let Err(e) = self.delete_identity(identity) {
                delete_all_err = delete_all_err.or(Some(e));
            }
        }
        if let Some(e) = delete_all_err {
            return Err(e);
        }

        if handled.wake {
            self.bio_wake();
        }
        Ok(handled)
    }

    fn bio_attest(&self, arg: usize) -> Result<bio::Handled> {
        let user = kernel::uaccess::UserPtr::from_addr(arg);
        let req: bio::Attest =
            kernel::uaccess::UserSlice::new(user, core::mem::size_of::<bio::Attest>())
                .reader()
                .read()?;
        let (sig, pubk) = self.refkey_attest_sign(&req.challenge)?;
        if sig.len() > bio::ATTEST_SIG_MAX || pubk.len() != bio::ATTEST_PUB_LEN {
            return Err(EIO);
        }
        let mut out = bio::Attest {
            sig_len: sig.len() as u32,
            challenge: req.challenge,
            public: [0u8; bio::ATTEST_PUB_LEN],
            signature: [0u8; bio::ATTEST_SIG_MAX],
            reserved: [0u8; 3],
        };
        out.public.copy_from_slice(&pubk);
        out.signature[..sig.len()].copy_from_slice(&sig);
        kernel::uaccess::UserSlice::new(user, core::mem::size_of::<bio::Attest>())
            .writer()
            .write(&out)?;
        Ok(bio::Handled {
            ret: 0,
            wake: false,
            start_enrol: false,
            start_verify: false,
            delete_identity: None,
            delete_identities: KVec::new(),
        })
    }

    fn ensure_restored_after(&self, _prepared: bool) {
        let restored = self.restore_all_components();
        self.templates_restored.store(restored, Relaxed);
        if !restored {
            dev_err!(
                self.dev,
                "matching unavailable this boot: restore did not complete, enclave holds no template (every match refused 0x1). Not the sensor or the finger; on-disk enrolments intact\n"
            );
            return;
        }

        if self.keybag_designated.load(Relaxed) {
            if let Some(user) = crate::sks::DesignateUser::new(SBIO_PROBE_USER_ID) {
                let special = user.special_handle();
                if let Ok(keybag::State::Present(stored)) = keybag::read(keybag::Slot::Identity) {
                    let _ = self.sks_step(crate::sks::SKS_LOCK_STATE_NAME, |healthy| {
                        self.sks_req_unlock_special(special, stored.secret(), healthy)
                    });
                }
            }
        }

        // enclave's match arm asserts a non-empty ACM context (<= 32 bytes)
        if let Some(user) = crate::sbio::UserId::new(SBIO_PROBE_USER_ID) {
            self.establish_scrd_match_context(user);
        }
    }

    fn cold_match_continue(
        &self,
        source: crate::sks::KeyBagHandle,
        uuid: [u8; keybag::UUID_LEN],
    ) -> bool {
        if !self.keybag_designated.load(Relaxed) {
            return false;
        }

        let Some(user) = crate::sks::DesignateUser::new(SBIO_PROBE_USER_ID) else {
            return false;
        };
        let special = user.special_handle();
        self.log_identity_count(c"before the cold-match preparation");

        let uuid_ok = self
            .sks_send(self.sks_req_copy_uuid_special(special))
            .and_then(|out| self.sks_uuid_from_reply(&out));
        match uuid_ok {
            Some(got) if got == uuid => {}
            Some(_) => {
                return false;
            }
            None => {
                return false;
            }
        }

        if self.sks_send(self.sks_req_unload_keybag(source)).is_none() {
            return false;
        }

        true
    }

    pub(crate) fn sep_random(&self, buf: &mut [u8]) -> Result<()> {
        for chunk in buf.chunks_mut(4) {
            let word = self.get_entropy_word()?;
            let bytes = word.to_le_bytes();
            let take = chunk.len();
            chunk.copy_from_slice(&bytes[..take]);
        }
        Ok(())
    }

    pub(crate) fn wrapped_from_copy_reply(
        &self,
        out: &SksOutcome,
        _from: &CStr,
    ) -> Option<KVec<u8>> {
        let body = self.sks_report_response(c"COPY_KEYBAG", out)?;
        if out.reply.status != 0 || image::operation_status(body).unwrap_or(-1) != 0 {
            return None;
        }
        let (blob, _) = image::read_blob(body, 4)?;
        if blob.is_empty() {
            return None;
        }
        let mut copy = KVec::new();
        if copy.extend_from_slice(blob, GFP_KERNEL).is_err() {
            return None;
        }
        Some(copy)
    }

    fn prove_restore(&self) -> bool {
        let Some(user) = crate::sbio::UserId::new(SBIO_PROBE_USER_ID) else {
            return false;
        };
        let op = crate::sbio::sbio_free_identity_count(user);
        let Some(reply) = self.sbio_expect_ok(&op) else {
            return false;
        };
        if reply.len() != crate::sbio::SBIO_FREE_COUNT_REPLY_LEN {
            return false;
        }
        let count = u32::from_le_bytes([reply[0], reply[1], reply[2], reply[3]]);
        if count > crate::sbio::SBIO_FREE_COUNT_MAX {
            return false;
        }
        if count == crate::sbio::SBIO_FREE_COUNT_MAX {
            return true;
        }
        true
    }

    fn log_identity_count(&self, _when: &CStr) -> Option<usize> {
        let op = crate::sbio::sbio_list_identities();
        let SbioOutcome::Ok(reply) = self.sbio_call(&op) else {
            return None;
        };
        let records = crate::sbio::IdentityRecords::new(&reply)?;
        Some(records.count())
    }

    fn enclave_lists(&self, uuid: &[u8; bio::UUID_LEN]) -> Option<bool> {
        let op = crate::sbio::sbio_list_identities();
        let SbioOutcome::Ok(reply) = self.sbio_call(&op) else {
            return None;
        };
        let records = crate::sbio::IdentityRecords::new(&reply)?;
        Some(records.lists_uuid(uuid))
    }

    fn delete_identity(&self, identity: &crate::sbio::IdentityV1) -> Result<()> {
        if self.enclave_lists(identity.uuid()) == Some(false) {
            self.forget_identity(identity.uuid(), c"a host entry the enclave never had");
            return Ok(());
        }

        let op = crate::sbio::sbio_delete_identity(identity);

        if self.sbio_expect_ok(&op).is_none() {
            dev_err!(
                self.dev,
                "bio: 0x57 failed, status above; host index left unchanged (forgetting a template the enclave still holds would report a deletion that did not happen)\n"
            );
            return Err(EIO);
        }

        self.forget_identity(identity.uuid(), c"removed from the enclave");
        Ok(())
    }

    fn forget_identity(&self, uuid: &[u8; bio::UUID_LEN], why: &CStr) {
        let persisted = {
            let mut index = self.bio_index.lock();
            index.remove(uuid);
            self.with_host_store(|store| index.persist(store))
        };
        match persisted {
            Some(Ok(())) => {},
            _ => dev_warn!(
                self.dev,
                "bio: {} ({}) but the host index could not be written; correct in memory this boot, rebuilt at next attach\n",
                Hex(uuid),
                why
            ),
        }
    }

    pub(crate) fn bio_ready(&self) -> bool {
        self.bio_session.lock().ready()
    }
}

pub(crate) struct SbioOp {
    opcode: u16,
    payload: [u8; SBIO_MAX_PAYLOAD],
    payload_len: usize,
    name: &'static CStr,
}

const SBIO_MAX_PAYLOAD: usize = 0x97;

pub(crate) const SBIO_IMAGE_INIT_LEN: usize = 0x97;
static_assert!(SBIO_IMAGE_INIT_LEN <= SBIO_MAX_PAYLOAD);

const fn pad_payload(src: &[u8]) -> [u8; SBIO_MAX_PAYLOAD] {
    let mut out = [0u8; SBIO_MAX_PAYLOAD];
    let mut i = 0;
    while i < src.len() {
        out[i] = src[i];
        i += 1;
    }
    out
}

impl SbioOp {
    pub(crate) fn opcode(&self) -> u16 {
        self.opcode
    }
    pub(crate) fn payload(&self) -> &[u8] {
        &self.payload[..self.payload_len]
    }
    pub(crate) fn name(&self) -> &'static CStr {
        self.name
    }
}

pub(crate) const SBIO_PROTOCOL_GENERATION: u32 = 1;

const OP_SBIO_REGISTER_SENSOR: u16 = 0x80;
pub(crate) fn sbio_register_sensor(id: &crate::sensor::Identifier) -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_REGISTER_SENSOR,
        payload: pad_payload(&id.value().to_le_bytes()),
        payload_len: 2,
        name: c"REGISTER_SENSOR",
    }
}

pub(crate) const SBIO_SIGNAL_QUALITY: u32 = 0;

const OP_SBIO_COVERAGE_PARAMS: u16 = 0x5d;
pub(crate) fn sbio_coverage_params() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_COVERAGE_PARAMS,
        payload: pad_payload(&SBIO_SIGNAL_QUALITY.to_le_bytes()),
        payload_len: 4,
        name: c"COVERAGE_PARAMS",
    }
}

const OP_SBIO_OPERATION_PARAMS: u16 = 0x5c;

pub(crate) const SBIO_OPERATION_PARAMS_LEN: usize = match SBIO_PROTOCOL_GENERATION {
    1 => 4,
    6 => 8,
    _ => 0,
};
static_assert!(SBIO_OPERATION_PARAMS_LEN != 0);
static_assert!(SBIO_OPERATION_PARAMS_LEN <= SBIO_MAX_PAYLOAD);

pub(crate) fn sbio_operation_params() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_OPERATION_PARAMS,
        payload: pad_payload(&[]),
        payload_len: SBIO_OPERATION_PARAMS_LEN,
        name: c"OPERATION_PARAMS",
    }
}

const OP_SBIO_TRANSPARENT_CHANNEL: u16 = 0x6a;
pub(crate) fn sbio_transparent_channel() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_TRANSPARENT_CHANNEL,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"TRANSPARENT_CHANNEL",
    }
}

const OP_SBIO_COMPLETE_INIT: u16 = 0x01;
pub(crate) fn sbio_complete_init(
    patch: crate::PatchLoaded,
    params: crate::ParametersApplied,
) -> SbioOp {
    let (_, _) = (patch, params);
    let mut body = [0u8; 8];
    body[0..4].copy_from_slice(&1u32.to_le_bytes());
    body[4..8].copy_from_slice(&1u32.to_le_bytes());
    SbioOp {
        opcode: OP_SBIO_COMPLETE_INIT,
        payload: pad_payload(&body),
        payload_len: 8,
        name: c"COMPLETE_INIT",
    }
}

const CONTEXT_SCOPE_SYSTEM: i32 = -1;
static_assert!(CONTEXT_SCOPE_SYSTEM != 0);
static_assert!(CONTEXT_SCOPE_SYSTEM < 0);

#[derive(Clone, Copy)]
pub(crate) struct ContextScope(i32);

impl ContextScope {
    pub(crate) const SYSTEM: ContextScope = ContextScope(CONTEXT_SCOPE_SYSTEM);

    pub(crate) fn user(id: UserId) -> ContextScope {
        ContextScope(id.value())
    }

    pub(crate) fn value(&self) -> i32 {
        self.0
    }
}

const OP_SBIO_CONTEXT_STATE: u16 = 0x6b;
pub(crate) fn sbio_context_state() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_CONTEXT_STATE,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"CONTEXT_STATE",
    }
}

// 0x2d destroys the catacomb; only send with zero existing identities
pub(crate) struct NoExistingCatacomb {
    _seal: (),
}

impl NoExistingCatacomb {
    pub(crate) fn from_zero_identities(count: usize) -> Option<NoExistingCatacomb> {
        match count {
            0 => Some(NoExistingCatacomb { _seal: () }),
            _ => None,
        }
    }
}

const OP_SBIO_SELECT_CONTEXT: u16 = 0x2d;
pub(crate) fn sbio_select_context(scope: ContextScope, proof: &NoExistingCatacomb) -> SbioOp {
    let NoExistingCatacomb { _seal: () } = proof;
    SbioOp {
        opcode: OP_SBIO_SELECT_CONTEXT,
        payload: pad_payload(&scope.value().to_le_bytes()),
        payload_len: 4,
        name: c"SELECT_CONTEXT",
    }
}

pub(crate) const SBIO_PROTECTED_CONFIG_LEN: usize = 32;

const OP_SBIO_PROTECTED_CONFIG: u16 = 0x2b;
pub(crate) fn sbio_protected_config(id: UserId) -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_PROTECTED_CONFIG,
        payload: pad_payload(&id.value().to_le_bytes()),
        payload_len: 4,
        name: c"PROTECTED_CONFIG",
    }
}

const OP_SBIO_BEGIN_ENROL: u16 = 0x03;

pub(crate) const BE_AUTH_TYPE_ACM_CONTEXT: u32 = 0;

pub(crate) const SBIO_BEGIN_ENROL_LEN: usize = 0x44;
static_assert!(SBIO_BEGIN_ENROL_LEN <= SBIO_MAX_PAYLOAD);

const BE_FLAGS: usize = 0;
const BE_USER_ID: usize = 4;
const BE_AUTH_TYPE: usize = 8;
const BE_TOKEN_LEN: usize = 12;
const BE_TOKEN: usize = 16;
static_assert!(BE_TOKEN + SKS_AUTH_TOKEN_LEN <= SBIO_BEGIN_ENROL_LEN);

pub(crate) fn sbio_begin_enrol(
    user: UserId,
    auth_type: u32,
    token: &[u8; SKS_AUTH_TOKEN_LEN],
) -> SbioOp {
    let mut body = [0u8; SBIO_BEGIN_ENROL_LEN];
    body[BE_FLAGS..BE_FLAGS + 4].copy_from_slice(&0u32.to_le_bytes());
    body[BE_USER_ID..BE_USER_ID + 4].copy_from_slice(&user.value().to_le_bytes());
    body[BE_AUTH_TYPE..BE_AUTH_TYPE + 4].copy_from_slice(&auth_type.to_le_bytes());
    body[BE_TOKEN_LEN..BE_TOKEN_LEN + 4]
        .copy_from_slice(&(SKS_AUTH_TOKEN_LEN as u32).to_le_bytes());
    body[BE_TOKEN..BE_TOKEN + SKS_AUTH_TOKEN_LEN].copy_from_slice(token);
    SbioOp {
        opcode: OP_SBIO_BEGIN_ENROL,
        payload: pad_payload(&body),
        payload_len: SBIO_BEGIN_ENROL_LEN,
        name: c"BEGIN_ENROL",
    }
}

const OP_SBIO_MODULE_CHALLENGE: u16 = 0x31;
pub(crate) fn sbio_module_challenge() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_MODULE_CHALLENGE,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"MODULE_CHALLENGE",
    }
}

pub(crate) const SBIO_MODULE_CHALLENGE_LEN: usize = 0x40;

const OP_SBIO_MODULE_COMMIT: u16 = 0x32;
pub(crate) fn sbio_module_commit(reply: &[u8]) -> Option<SbioOp> {
    if reply.is_empty() || reply.len() > SBIO_MAX_PAYLOAD {
        return None;
    }
    Some(SbioOp {
        opcode: OP_SBIO_MODULE_COMMIT,
        payload: pad_payload(reply),
        payload_len: reply.len(),
        name: c"MODULE_COMMIT",
    })
}

const OP_SBIO_LOAD_CALIBRATION: u16 = 0x5b;

pub(crate) const SBIO_CALIBRATION_SOURCE: u32 = 3;

pub(crate) struct SbioCalibration(KVec<u8>);

impl SbioCalibration {
    pub(crate) fn new(blob: &crate::CalibrationBlob) -> Result<SbioCalibration> {
        let bytes = blob.bytes();
        let mut body = KVec::new();
        body.extend_from_slice(&SBIO_CALIBRATION_SOURCE.to_le_bytes(), GFP_KERNEL)?;
        body.extend_from_slice(bytes, GFP_KERNEL)?;
        Ok(SbioCalibration(body))
    }

    pub(crate) fn opcode(&self) -> u16 {
        OP_SBIO_LOAD_CALIBRATION
    }

    pub(crate) fn payload(&self) -> &[u8] {
        &self.0
    }

    pub(crate) fn name(&self) -> &'static CStr {
        c"LOAD_CALIBRATION"
    }
}

const OP_SBIO_UPDATE_DEVICE_LIST: u16 = 0x7b;
pub(crate) fn sbio_update_device_list() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_UPDATE_DEVICE_LIST,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"UPDATE_DEVICE_LIST",
    }
}

pub(crate) const SBIO_MATCH_POLICY_LEN: usize = 2;

const OP_SBIO_MATCH_POLICY: u16 = 0x1f;
pub(crate) fn sbio_match_policy() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_MATCH_POLICY,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"MATCH_POLICY",
    }
}

pub(crate) const SBIO_ENROL_RESULT_LEN: usize = 0xc98;

const ER_PROGRESS: usize = 0x004;
const ER_HAS_TEMPLATE: usize = 0x006;
const ER_COMPLETE: usize = 0xbfe;

static_assert!(ER_PROGRESS < SBIO_ENROL_RESULT_LEN);
static_assert!(ER_HAS_TEMPLATE + 4 <= SBIO_ENROL_RESULT_LEN);
static_assert!(ER_COMPLETE + 4 <= SBIO_ENROL_RESULT_LEN);
static_assert!(ER_COMPLETE > ER_HAS_TEMPLATE + 4);

pub(crate) const ER_IDENTITY_LEN: usize = 4 + IDENTITY_UUID_LEN;
static_assert!(ER_IDENTITY_LEN == 20);
static_assert!(ER_IDENTITY_LEN <= SBIO_ENROL_RESULT_LEN);

pub(crate) fn enrol_identity_candidates(
    bytes: &[u8],
    user_id: i32,
    mut visit: impl FnMut(usize, [u8; IDENTITY_UUID_LEN]),
) {
    if bytes.len() < ER_IDENTITY_LEN {
        return;
    }
    let wanted = user_id.to_le_bytes();
    for at in 0..=bytes.len() - ER_IDENTITY_LEN {
        if bytes[at..at + 4] != wanted {
            continue;
        }
        let mut uuid = [0u8; IDENTITY_UUID_LEN];
        uuid.copy_from_slice(&bytes[at + 4..at + ER_IDENTITY_LEN]);
        if uuid.iter().all(|&b| b == 0) {
            continue;
        }
        visit(at, uuid);
    }
}

pub(crate) struct EnrolmentResult {
    progress_raw: u8,
    has_template: u32,
    complete: u32,
}

impl EnrolmentResult {
    pub(crate) fn parse(bytes: &[u8]) -> Option<EnrolmentResult> {
        if bytes.len() < SBIO_ENROL_RESULT_LEN {
            return None;
        }
        Some(EnrolmentResult {
            progress_raw: bytes[ER_PROGRESS],
            has_template: u32::from_le_bytes([
                bytes[ER_HAS_TEMPLATE],
                bytes[ER_HAS_TEMPLATE + 1],
                bytes[ER_HAS_TEMPLATE + 2],
                bytes[ER_HAS_TEMPLATE + 3],
            ]),
            complete: u32::from_le_bytes([
                bytes[ER_COMPLETE],
                bytes[ER_COMPLETE + 1],
                bytes[ER_COMPLETE + 2],
                bytes[ER_COMPLETE + 3],
            ]),
        })
    }

    pub(crate) fn progress_percent(&self) -> u32 {
        (u32::from(self.progress_raw) * 100 + 127) / 255
    }

    pub(crate) fn has_template(&self) -> bool {
        self.has_template != 0
    }

    pub(crate) fn complete(&self) -> bool {
        self.complete != 0
    }
}

#[derive(Clone, Copy)]
pub(crate) struct CatacombUser(i32);

impl CatacombUser {
    pub(crate) const MASTER: CatacombUser = CatacombUser(-1);
    pub(crate) const OWNER: CatacombUser = CatacombUser(501);

    pub(crate) fn enrolling(user: UserId) -> CatacombUser {
        CatacombUser(user.value())
    }

    pub(crate) const fn value(&self) -> i32 {
        self.0
    }
}
static_assert!(CatacombUser::MASTER.value() == -1);
static_assert!(CatacombUser::OWNER.value() == 501);
static_assert!(CatacombUser::MASTER.value() != CatacombUser::OWNER.value());
static_assert!(CatacombUser::MASTER.value() < 0);

pub(crate) const SBIO_SAVED_USER_ID_AT: usize = 8;
static_assert!(SBIO_SAVED_USER_ID_AT + 4 <= SBIO_SAVED_MIN);

pub(crate) const SBIO_SAVE_SELECTOR_LEN: usize = 24;
const SS_USER_ID: usize = 0;
static_assert!(SBIO_SAVE_SELECTOR_LEN <= SBIO_MAX_PAYLOAD);

pub(crate) struct SaveSelector([u8; SBIO_SAVE_SELECTOR_LEN]);

impl SaveSelector {
    pub(crate) fn new(user: CatacombUser) -> SaveSelector {
        let mut out = [0u8; SBIO_SAVE_SELECTOR_LEN];
        out[SS_USER_ID..SS_USER_ID + 4].copy_from_slice(&user.value().to_le_bytes());
        SaveSelector(out)
    }

    pub(crate) fn bytes(&self) -> &[u8; SBIO_SAVE_SELECTOR_LEN] {
        &self.0
    }
}

pub(crate) const SBIO_STATUS_COLD_TRANSITION: u32 = 0x8002;
static_assert!(SBIO_STATUS_COLD_TRANSITION != 0);

// 0x101 from a 0x6d load = already active; a success answer, not an error
pub(crate) const SBIO_STATUS_ALREADY_ACTIVE: u32 = 0x101;
static_assert!(SBIO_STATUS_ALREADY_ACTIVE != 0);
static_assert!(SBIO_STATUS_ALREADY_ACTIVE != SBIO_STATUS_COLD_TRANSITION);

pub(crate) const COMPONENT_STATE_COLD: u32 = 0x1;
pub(crate) const COMPONENT_STATE_ACTIVE: u32 = 0x2;
static_assert!(COMPONENT_STATE_COLD != COMPONENT_STATE_ACTIVE);

pub(crate) const COMPONENT_PAIR_LEN: usize = 8;

pub(crate) struct ComponentStates<'a>(&'a [u8]);

impl<'a> ComponentStates<'a> {
    pub(crate) fn new(reply: &'a [u8]) -> ComponentStates<'a> {
        ComponentStates(reply)
    }

    pub(crate) fn count(&self) -> usize {
        self.0.len() / COMPONENT_PAIR_LEN
    }

    pub(crate) fn pair(&self, i: usize) -> Option<(i32, u32)> {
        let at = i.checked_mul(COMPONENT_PAIR_LEN)?;
        let bytes = self.0.get(at..at + COMPONENT_PAIR_LEN)?;
        Some((
            i32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
            u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]),
        ))
    }

    pub(crate) fn state_for(&self, id: i32) -> Option<u32> {
        (0..self.count())
            .filter_map(|i| self.pair(i))
            .find(|(pair_id, _)| *pair_id == id)
            .map(|(_, state)| state)
    }
}

pub(crate) enum ComponentAction {
    AlreadyActive,
    Load,
    Unsupported,
}

pub(crate) fn component_action(state: u32) -> ComponentAction {
    if state & COMPONENT_STATE_ACTIVE != 0 {
        ComponentAction::AlreadyActive
    } else if state & COMPONENT_STATE_COLD != 0 {
        ComponentAction::Load
    } else {
        ComponentAction::Unsupported
    }
}

const OP_SBIO_SAVE_LOCKOUT: u16 = 0x70;
pub(crate) fn sbio_save_lockout() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_SAVE_LOCKOUT,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"SAVE_LOCKOUT",
    }
}

pub(crate) struct SbioLoadLockout<'a>(&'a [u8]);

impl<'a> SbioLoadLockout<'a> {
    pub(crate) fn new(blob: &'a [u8]) -> Option<SbioLoadLockout<'a>> {
        if blob.is_empty() || blob.len() > SBIO_SAVED_MAX {
            return None;
        }
        Some(SbioLoadLockout(blob))
    }

    pub(crate) fn opcode(&self) -> u16 {
        OP_SBIO_LOAD_LOCKOUT
    }

    pub(crate) fn payload(&self) -> &[u8] {
        self.0
    }

    pub(crate) fn name(&self) -> &'static CStr {
        c"LOAD_LOCKOUT"
    }
}
const OP_SBIO_LOAD_LOCKOUT: u16 = 0x71;

const OP_SBIO_SAVE_CATACOMB: u16 = 0x6c;
pub(crate) fn sbio_save_catacomb(selector: &SaveSelector) -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_SAVE_CATACOMB,
        payload: pad_payload(selector.bytes()),
        payload_len: SBIO_SAVE_SELECTOR_LEN,
        name: c"SAVE_CATACOMB",
    }
}

const OP_SBIO_CONFIRM_SAVE: u16 = 0x37;
pub(crate) fn sbio_confirm_save(selector: &SaveSelector) -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_CONFIRM_SAVE,
        payload: pad_payload(selector.bytes()),
        payload_len: SBIO_SAVE_SELECTOR_LEN,
        name: c"CONFIRM_SAVE",
    }
}

pub(crate) const SBIO_SAVED_MIN: usize = 0x22;
pub(crate) const SBIO_SAVED_MAX: usize = 0x4b000;
static_assert!(SBIO_SAVED_MIN < SBIO_SAVED_MAX);
static_assert!(SBIO_SAVED_MAX as u32 <= transfer::MAX_TRANSACTION);

pub(crate) struct SbioLoadCatacomb<'a>(&'a [u8]);

impl<'a> SbioLoadCatacomb<'a> {
    pub(crate) fn new(blob: &'a [u8]) -> Option<SbioLoadCatacomb<'a>> {
        if blob.len() < SBIO_SAVED_MIN || blob.len() > SBIO_SAVED_MAX {
            return None;
        }
        Some(SbioLoadCatacomb(blob))
    }

    pub(crate) fn opcode(&self) -> u16 {
        OP_SBIO_LOAD_CATACOMB
    }

    pub(crate) fn payload(&self) -> &[u8] {
        self.0
    }

    pub(crate) fn name(&self) -> &'static CStr {
        c"LOAD_CATACOMB"
    }
}
const OP_SBIO_LOAD_CATACOMB: u16 = 0x6d;

pub(crate) const IDENTITY_RECORD_LEN: usize = 40;
const IR_SELECTOR: usize = 20;
const IR_SELECTOR_BUILTIN: u32 = 1;
static_assert!(IR_SELECTOR == IDENTITY_V1_LEN);
static_assert!(IR_SELECTOR + 4 <= IDENTITY_RECORD_LEN);

const OP_SBIO_LIST_IDENTITIES: u16 = 0x7a;
pub(crate) fn sbio_list_identities() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_LIST_IDENTITIES,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"LIST_IDENTITIES",
    }
}

pub(crate) struct IdentityRecords<'a>(&'a [u8]);

impl<'a> IdentityRecords<'a> {
    pub(crate) fn new(reply: &'a [u8]) -> Option<IdentityRecords<'a>> {
        if reply.len() % IDENTITY_RECORD_LEN != 0 {
            return None;
        }
        Some(IdentityRecords(reply))
    }

    pub(crate) fn count(&self) -> usize {
        self.0.len() / IDENTITY_RECORD_LEN
    }

    pub(crate) fn record(&self, i: usize) -> Option<(IdentityV1, bool)> {
        let at = i.checked_mul(IDENTITY_RECORD_LEN)?;
        let bytes = self.0.get(at..at + IDENTITY_RECORD_LEN)?;
        let user_id = i32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
        let mut uuid = [0u8; IDENTITY_UUID_LEN];
        uuid.copy_from_slice(&bytes[4..IDENTITY_V1_LEN]);
        let selector = u32::from_le_bytes([
            bytes[IR_SELECTOR],
            bytes[IR_SELECTOR + 1],
            bytes[IR_SELECTOR + 2],
            bytes[IR_SELECTOR + 3],
        ]);
        Some((
            IdentityV1 { user_id, uuid },
            selector == IR_SELECTOR_BUILTIN,
        ))
    }

    pub(crate) fn lists_uuid(&self, uuid: &[u8; IDENTITY_UUID_LEN]) -> bool {
        (0..self.count())
            .filter_map(|i| self.record(i))
            .any(|(identity, _)| identity.uuid == *uuid)
    }

    pub(crate) fn each_built_in_for(&self, user_id: i32, mut visit: impl FnMut(IdentityV1)) {
        for i in 0..self.count() {
            if let Some((identity, built_in)) = self.record(i) {
                if built_in && identity.user_id == user_id {
                    visit(identity);
                }
            }
        }
    }
}

const FIC_DEVICE_BUILTIN: u32 = 1;
pub(crate) const SBIO_FREE_COUNT_LEN: usize = 4 + 20;
static_assert!(SBIO_FREE_COUNT_LEN == SBIO_SAVE_SELECTOR_LEN);
pub(crate) const SBIO_FREE_COUNT_REPLY_LEN: usize = 4;
pub(crate) const SBIO_FREE_COUNT_MAX: u32 = 3;

const OP_SBIO_FREE_IDENTITY_COUNT: u16 = 0x38;
pub(crate) fn sbio_free_identity_count(user: UserId) -> SbioOp {
    let mut body = [0u8; SBIO_FREE_COUNT_LEN];
    body[0..4].copy_from_slice(&user.value().to_le_bytes());
    body[4..8].copy_from_slice(&FIC_DEVICE_BUILTIN.to_le_bytes());
    SbioOp {
        opcode: OP_SBIO_FREE_IDENTITY_COUNT,
        payload: pad_payload(&body),
        payload_len: SBIO_FREE_COUNT_LEN,
        name: c"FREE_IDENTITY_COUNT",
    }
}

const OP_SBIO_DELETE_IDENTITY: u16 = 0x57;
pub(crate) fn sbio_delete_identity(identity: &IdentityV1) -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_DELETE_IDENTITY,
        payload: pad_payload(&identity.wire()),
        payload_len: IDENTITY_V1_LEN,
        name: c"DELETE_IDENTITY",
    }
}

const OP_SBIO_MATCH_RESULT: u16 = 0x09;
pub(crate) fn sbio_match_result() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_MATCH_RESULT,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"MATCH_RESULT",
    }
}

pub(crate) const SBIO_MATCH_RESULT_LEN: usize = 0xca2;

const MR_USER_ID: usize = 0x000;
const MR_IDENTITY: usize = 0x004;
static_assert!(MR_USER_ID + 4 <= SBIO_MATCH_RESULT_LEN);
static_assert!(MR_IDENTITY + IDENTITY_UUID_LEN <= SBIO_MATCH_RESULT_LEN);
static_assert!(MR_IDENTITY == MR_USER_ID + 4);
static_assert!(SBIO_MATCH_RESULT_LEN != SBIO_ENROL_RESULT_LEN);

pub(crate) const IDENTITY_UUID_LEN: usize = 16;
pub(crate) const IDENTITY_V1_LEN: usize = 4 + IDENTITY_UUID_LEN;
static_assert!(IDENTITY_V1_LEN == 0x14);

#[derive(Clone, Copy)]
pub(crate) struct IdentityV1 {
    user_id: i32,
    uuid: [u8; IDENTITY_UUID_LEN],
}

impl IdentityV1 {
    pub(crate) fn from_index(user_id: i32, uuid: [u8; IDENTITY_UUID_LEN]) -> IdentityV1 {
        IdentityV1 { user_id, uuid }
    }

    pub(crate) fn uuid(&self) -> &[u8; IDENTITY_UUID_LEN] {
        &self.uuid
    }

    pub(crate) fn wire(&self) -> [u8; IDENTITY_V1_LEN] {
        let mut out = [0u8; IDENTITY_V1_LEN];
        out[0..4].copy_from_slice(&self.user_id.to_le_bytes());
        out[4..IDENTITY_V1_LEN].copy_from_slice(&self.uuid);
        out
    }
}

pub(crate) struct MatchResult {
    user_id: i32,
    identity: [u8; IDENTITY_UUID_LEN],
}

impl MatchResult {
    pub(crate) fn parse(bytes: &[u8]) -> Option<MatchResult> {
        if bytes.len() < SBIO_MATCH_RESULT_LEN {
            return None;
        }
        let mut identity = [0u8; IDENTITY_UUID_LEN];
        identity.copy_from_slice(&bytes[MR_IDENTITY..MR_IDENTITY + IDENTITY_UUID_LEN]);
        Some(MatchResult {
            user_id: i32::from_le_bytes([
                bytes[MR_USER_ID],
                bytes[MR_USER_ID + 1],
                bytes[MR_USER_ID + 2],
                bytes[MR_USER_ID + 3],
            ]),
            identity,
        })
    }

    pub(crate) fn identity_uuid(&self) -> &[u8; IDENTITY_UUID_LEN] {
        &self.identity
    }

    pub(crate) fn matches(&self, user: UserId) -> bool {
        self.user_id == user.value()
    }
}

const OP_SBIO_IMAGE_CLEANUP: u16 = 0x22;
pub(crate) fn sbio_image_cleanup() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_IMAGE_CLEANUP,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"IMAGE_CLEANUP",
    }
}

const OP_SBIO_CANCEL: u16 = 0x05;
pub(crate) fn sbio_cancel_operation() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_CANCEL,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"CANCEL_OPERATION",
    }
}

const OP_SBIO_CLEAR_STATE: u16 = 0x19;
pub(crate) fn sbio_clear_state() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_CLEAR_STATE,
        payload: pad_payload(&[]),
        payload_len: 0,
        name: c"CLEAR_STATE",
    }
}

const OP_SBIO_REGISTER_SERIAL: u16 = 0x17;
pub(crate) fn sbio_register_sensor_serial(serial: &crate::sensor::SensorSerial) -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_REGISTER_SERIAL,
        payload: pad_payload(serial.bytes()),
        payload_len: crate::sensor::SENSOR_SERIAL_LEN,
        name: c"REGISTER_SERIAL",
    }
}

const OP_SBIO_DIAGNOSTICS: u16 = 0x63;
pub(crate) const fn sbio_diagnostics() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_DIAGNOSTICS,
        payload: [0; SBIO_MAX_PAYLOAD],
        payload_len: 0,
        name: c"DIAGNOSTICS",
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ImagePurpose {
    Enrolment,
    Matching,
}

impl ImagePurpose {
    const fn offset(self) -> usize {
        match self {
            ImagePurpose::Matching => 0x12,
            ImagePurpose::Enrolment => 0x13,
        }
    }

    const fn last_image_offset(self) -> usize {
        match self {
            ImagePurpose::Enrolment => 0x08,
            ImagePurpose::Matching => 0x09,
        }
    }
}

static_assert!(ImagePurpose::Enrolment.offset() != ImagePurpose::Matching.offset());
static_assert!(ImagePurpose::Matching.offset() == 0x12);
static_assert!(ImagePurpose::Enrolment.offset() == 0x13);
static_assert!(ImagePurpose::Enrolment.last_image_offset() == 0x08);
static_assert!(ImagePurpose::Matching.last_image_offset() == 0x09);
static_assert!(
    ImagePurpose::Enrolment.last_image_offset() != ImagePurpose::Matching.last_image_offset()
);
static_assert!(ImagePurpose::Matching.last_image_offset() < ImagePurpose::Matching.offset());

#[derive(Clone, Copy)]
pub(crate) struct UserId(i32);

impl UserId {
    pub(crate) fn new(value: i32) -> Option<UserId> {
        if value > 0 {
            Some(UserId(value))
        } else {
            None
        }
    }

    pub(crate) const fn value(&self) -> i32 {
        self.0
    }
}

const IPI_FIRST_IMAGE: usize = 0x07;
const IPI_TIMESTAMP: usize = 0x40;
const IPI_CAPTURE_COUNTER: usize = 0x48;
const IPI_DEVICE_KIND: usize = 0x4f;
const IPI_USER_ID: usize = 0x67;

const IPI_DEVICE_BUILTIN: u32 = 1;

static_assert!(IPI_USER_ID + 4 <= SBIO_MAX_PAYLOAD);
static_assert!(IPI_DEVICE_KIND + 4 <= SBIO_MAX_PAYLOAD);
static_assert!(IPI_TIMESTAMP + 8 <= SBIO_MAX_PAYLOAD);
static_assert!(IPI_CAPTURE_COUNTER + 4 <= SBIO_MAX_PAYLOAD);
static_assert!(IPI_FIRST_IMAGE < ImagePurpose::Matching.offset());
static_assert!(ImagePurpose::Enrolment.offset() < IPI_TIMESTAMP);

const OP_SBIO_PREPARE_IMAGE: u16 = 0x23;
pub(crate) fn sbio_prepare_image_processing() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_PREPARE_IMAGE,
        payload: [0; SBIO_MAX_PAYLOAD],
        payload_len: 0,
        name: c"PREPARE_IMAGE_PROCESSING",
    }
}

const OP_SBIO_IMAGE_INIT: u16 = 0x06;
pub(crate) fn sbio_image_processing_init(
    purpose: ImagePurpose,
    first_image: bool,
    last_image: bool,
    capture_counter: u32,
    user: UserId,
    monotonic_ns: u64,
) -> SbioOp {
    let mut payload = [0u8; SBIO_MAX_PAYLOAD];

    if first_image {
        payload[IPI_FIRST_IMAGE] = 1;
    }
    if last_image {
        payload[purpose.last_image_offset()] = 1;
    }
    payload[purpose.offset()] = 1;

    // ns*24/1000 split to dodge u64 overflow and the kernel's absent __udivti3
    let scaled = (monotonic_ns / 1000) * 24 + ((monotonic_ns % 1000) * 24) / 1000;
    payload[IPI_TIMESTAMP..IPI_TIMESTAMP + 8].copy_from_slice(&scaled.to_le_bytes());
    payload[IPI_CAPTURE_COUNTER..IPI_CAPTURE_COUNTER + 4]
        .copy_from_slice(&capture_counter.to_le_bytes());
    payload[IPI_DEVICE_KIND..IPI_DEVICE_KIND + 4]
        .copy_from_slice(&IPI_DEVICE_BUILTIN.to_le_bytes());
    payload[IPI_USER_ID..IPI_USER_ID + 4].copy_from_slice(&user.value().to_le_bytes());

    SbioOp {
        opcode: OP_SBIO_IMAGE_INIT,
        payload,
        payload_len: SBIO_IMAGE_INIT_LEN,
        name: c"IMAGE_PROCESSING_INIT",
    }
}

const OP_SBIO_RELAY_CAPTURE: u16 = 0x65;

pub(crate) struct SbioRelay<'a> {
    capture: &'a crate::sensor::Capture,
}

impl<'a> SbioRelay<'a> {
    pub(crate) fn new(capture: &'a crate::sensor::Capture) -> SbioRelay<'a> {
        SbioRelay { capture }
    }

    pub(crate) fn opcode(&self) -> u16 {
        OP_SBIO_RELAY_CAPTURE
    }

    pub(crate) fn payload(&self) -> &[u8] {
        self.capture.bytes()
    }

    pub(crate) fn name(&self) -> &'static CStr {
        c"RELAY_CAPTURE"
    }
}

const OP_SBIO_ASSESSMENT: u16 = 0x07;
pub(crate) fn sbio_image_assessment() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_ASSESSMENT,
        payload: [0; SBIO_MAX_PAYLOAD],
        payload_len: 0,
        name: c"IMAGE_ASSESSMENT",
    }
}

const OP_SBIO_ENROL_RESULT: u16 = 0x08;
pub(crate) fn sbio_enrolment_result() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_ENROL_RESULT,
        payload: [0; SBIO_MAX_PAYLOAD],
        payload_len: 0,
        name: c"ENROLMENT_RESULT",
    }
}

pub(crate) const ASSESS_MIN_LEN: usize = 0x89;
pub(crate) const ASSESS_USABLE_MATCH: usize = 0x06;
pub(crate) const ASSESS_USABLE_ENROL: usize = 0x07;

static_assert!(ASSESS_USABLE_MATCH < ASSESS_USABLE_ENROL);

pub(crate) const SBIO_SESSION_SHARE_LEN: usize = 40;

pub(crate) const SBIO_STATUS_OK: u16 = 0x00;
pub(crate) const SBIO_STATUS_PREREQUISITE: u16 = 0x01;
pub(crate) const SBIO_STATUS_16: u16 = 0x16;
static_assert!(SBIO_STATUS_PREREQUISITE != SBIO_STATUS_16);
pub(crate) const SBIO_SESSION_MODE: u32 = 1;
static_assert!(SBIO_SESSION_SHARE_LEN <= SBIO_MAX_PAYLOAD);

const OP_SBIO_SESSION_SHARE: u16 = 0x15;
pub(crate) fn sbio_request_session_share() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_SESSION_SHARE,
        payload: pad_payload(&SBIO_SESSION_MODE.to_le_bytes()),
        payload_len: 4,
        name: c"SESSION_SHARE",
    }
}

const OP_SBIO_COMMIT_SESSION: u16 = 0x16;
pub(crate) fn sbio_commit_session_share(share: &[u8; SBIO_SESSION_SHARE_LEN]) -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_COMMIT_SESSION,
        payload: pad_payload(share),
        payload_len: SBIO_SESSION_SHARE_LEN,
        name: c"SESSION_COMMIT",
    }
}
pub(crate) const SBIO_CHALLENGE_LEN: usize = 64;
static_assert!(SBIO_CHALLENGE_LEN <= SBIO_MAX_PAYLOAD);

const OP_SBIO_CHALLENGE: u16 = 0x42;
pub(crate) fn sbio_request_challenge() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_CHALLENGE,
        payload: [0; SBIO_MAX_PAYLOAD],
        payload_len: 0,
        name: c"SEQUENCE_CHALLENGE",
    }
}

const OP_SBIO_COMMIT_CHALLENGE: u16 = 0x18;
pub(crate) fn sbio_commit_challenge(reply: &[u8; SBIO_CHALLENGE_LEN]) -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_COMMIT_CHALLENGE,
        payload: pad_payload(reply),
        payload_len: SBIO_CHALLENGE_LEN,
        name: c"SEQUENCE_COMMIT",
    }
}
const OP_SBIO_FETCH_PATCH: u16 = 0x5f;

const SBIO_PATCH_SUBTYPE: u16 = 2;

pub(crate) fn sbio_fetch_patch() -> SbioOp {
    SbioOp {
        opcode: OP_SBIO_FETCH_PATCH,
        payload: pad_payload(&SBIO_PATCH_SUBTYPE.to_le_bytes()),
        payload_len: 2,
        name: c"FETCH_SENSOR_PATCH",
    }
}

const fn encode_sbio(opcode: u16, marker: u8, seq: u16) -> Message {
    Message {
        msg0: (EP_SBIO as u64)
            | ((marker as u64) << MSG_TAG_SHIFT)
            | ((opcode as u64) << MSG_TYPE_SHIFT)
            | ((seq as u64) << 48),
        msg1: 0,
    }
}

static_assert!(encode_sbio(0x73, transfer::MARKER_FIRST, 0).msg0 == 0x0000_0073_fc08);

pub(crate) fn encode_sbio_raw(opcode: u16, seq: u16) -> Option<Message> {
    Some(encode_sbio(opcode, transfer::MARKER_FIRST, seq))
}

pub(crate) fn encode_sbio_next(opcode: u16, seq: u16) -> Option<Message> {
    Some(encode_sbio(opcode, transfer::MARKER_NEXT, seq))
}

pub(crate) fn encode_sbio_continue(cont: &transfer::Continuation) -> Message {
    encode_sbio(cont.opcode() as u16, transfer::MARKER_REQUEST, cont.seq())
}
