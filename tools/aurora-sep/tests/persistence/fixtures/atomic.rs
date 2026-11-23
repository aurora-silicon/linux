// SPDX-License-Identifier: GPL-2.0-only OR MIT
use std::sync::atomic::{AtomicBool, Ordering::Relaxed};
const PRIVATE_TYPE_CATACOMB_MASTER: u8 = 0xf2;
const PRIVATE_TYPE_CATACOMB_OWNER: u8 = 0xf3;
const PRIVATE_TYPE_CATACOMB_USER: u8 = 0xf4;
const SBIO_PROBE_USER_ID: i32 = 501;
const SLOT_USER: i32 = 0;
const SLOT_MASTER: i32 = 1;
const SLOT_OWNER: i32 = 2;
const MOUNT: i32 = 5;
const RENAME: i32 = 8;
const TEMP_SYNC: i32 = 9;
const DIR_SYNC: i32 = 10;
const READ_OPEN: i32 = 16;
const DIR_PERM: i32 = 24;
const DIR_APPEND: i32 = 26;
const CREATOR_MAP: i32 = 30;
const DIR_NO_FSYNC: i32 = 31;
const DIR_NO_RENAME: i32 = 32;
const TEMP_CLOSE: i32 = 34;
extern "C" {
    fn fixture_reset();
    fn fixture_done();
    fn fixture_fault(stage: i32, error: i32);
    fn fixture_write_fault(at: usize, error: i32, zero: i32);
    fn fixture_short(n: usize);
    fn fixture_count(n: i32) -> i32;
    fn fixture_slot_mountpoint(idx: i32, on: i32);
    fn fixture_slot_write_open(idx: i32, err: i32);
    fn fixture_slot_stale(
        idx: i32,
        present: i32,
        kind: i32,
        mountpoint: i32,
        may_delete_err: i32,
        security_err: i32,
        unlink_err: i32,
    );
}
struct Fixture;
impl Fixture {
    fn new() -> Self {
        unsafe { fixture_reset() };
        Self
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        unsafe { fixture_done() };
    }
}
fn fault(stage: i32, error: i32) {
    unsafe { fixture_fault(stage, error) }
}
fn count(n: i32) -> i32 {
    unsafe { fixture_count(n) }
}
fn user_read() -> Result<Option<KVec<u8>>> {
    catacomb::read_checked(PRIVATE_TYPE_CATACOMB_USER)
}
fn current() -> Vec<u8> {
    user_read().unwrap().unwrap().to_vec()
}
fn original() {
    catacomb::write(PRIVATE_TYPE_CATACOMB_USER, b"original valid catacomb").unwrap();
}
fn seed_all_catacombs() {
    catacomb::write(PRIVATE_TYPE_CATACOMB_USER, b"user valid catacomb").unwrap();
    catacomb::write(PRIVATE_TYPE_CATACOMB_MASTER, b"master valid catacomb").unwrap();
    catacomb::write(PRIVATE_TYPE_CATACOMB_OWNER, b"owner valid catacomb").unwrap();
}
struct Status(bool);
impl Status {
    fn is_ok(&self) -> bool {
        self.0
    }
}
impl std::fmt::Display for Status {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "ok")
    }
}
struct Done {
    status: Status,
    payload: KVec<u8>,
}
struct FakeMutex<T>(RefCell<T>);
impl<T> FakeMutex<T> {
    fn new(v: T) -> Self {
        Self(RefCell::new(v))
    }
    fn lock(&self) -> std::cell::RefMut<'_, T> {
        self.0.borrow_mut()
    }
}
struct EnrolMaterial {
    snapshot_identity: Option<()>,
}
struct XarmState {
    os_uuid: Option<()>,
}
struct EnrolPrerequisites {
    lane: bool,
    live_count: Option<usize>,
    bag_bound: bool,
    catacomb_readable: [bool; 3],
    catacomb_writable: [bool; 3],
    keybag_writable: bool,
    host_store_capacity: bool,
}
struct BioIndex;
impl BioIndex {
    fn can_persist_next(&self, _: &()) -> bool {
        true
    }
}
mod keybag {
    use super::*;
    pub enum Slot {
        Identity,
    }
    impl Slot {
        pub fn path(&self) -> &'static CStr {
            c"/var/lib/aurora-sep-keybag.bin"
        }
    }
    pub enum State {
        Present(()),
    }
    pub fn read(_: Slot) -> Result<State> {
        Ok(State::Present(()))
    }
}
mod bio {
    use super::*;
    #[derive(Debug, PartialEq, Eq)]
    pub struct Handled;
    pub fn require_admin() -> Result<()> {
        Ok(())
    }
    pub fn validate_enrol_start_arg(_: usize) -> Result<()> {
        Ok(())
    }
    pub fn idle_for_mutation(_: &()) -> bool {
        true
    }
    pub fn ok() -> Result<Handled> {
        Ok(Handled)
    }
}
mod sensor {
    pub fn is_bound() -> bool {
        true
    }
}
struct SepData {
    dev: (),
    who: i32,
    confirms: Cell<usize>,
    deletes: Cell<usize>,
    context_opens: Cell<usize>,
    confirm_ok: Cell<bool>,
    lockout_ok: Cell<bool>,
    post_select_fault_owner_write: Cell<bool>,
    deletion_unsaved: AtomicBool,
    touchid_started: AtomicBool,
    keybag_designated: AtomicBool,
    empty_context_prepared: AtomicBool,
    xarm: FakeMutex<XarmState>,
    enrol_material: FakeMutex<Option<EnrolMaterial>>,
    bio_session: FakeMutex<()>,
    bio_index: FakeMutex<BioIndex>,
    catacomb_writer: FakeMutex<()>,
    // Each SEP-side step with the directory syncs completed before it.
    ops: RefCell<Vec<(&'static str, i32)>>,
}
impl SepData {
    fn new() -> Self {
        Self::for_user(501)
    }
    fn master() -> Self {
        Self::for_user(-1)
    }
    fn for_user(who: i32) -> Self {
        Self {
            dev: (),
            who,
            confirms: Cell::new(0),
            deletes: Cell::new(0),
            context_opens: Cell::new(0),
            confirm_ok: Cell::new(true),
            lockout_ok: Cell::new(true),
            post_select_fault_owner_write: Cell::new(false),
            deletion_unsaved: AtomicBool::new(false),
            touchid_started: AtomicBool::new(true),
            keybag_designated: AtomicBool::new(true),
            empty_context_prepared: AtomicBool::new(false),
            xarm: FakeMutex::new(XarmState { os_uuid: Some(()) }),
            enrol_material: FakeMutex::new(Some(EnrolMaterial {
                snapshot_identity: Some(()),
            })),
            bio_session: FakeMutex::new(()),
            bio_index: FakeMutex::new(BioIndex),
            catacomb_writer: FakeMutex::new(()),
            ops: RefCell::new(Vec::new()),
        }
    }
    fn ops(&self) -> Vec<(&'static str, i32)> {
        self.ops.borrow().clone()
    }
    fn sbio_transfer(&self, op: &sbio::Operation) -> Result<Done> {
        let mut blob = vec![0xab; sbio::SBIO_SAVED_MIN];
        blob[8..12].copy_from_slice(&self.who.to_le_bytes());
        let ok = if op.0 {
            self.confirms.set(self.confirms.get() + 1);
            self.ops.borrow_mut().push(("confirm", count(2)));
            self.confirm_ok.get()
        } else {
            self.ops.borrow_mut().push(("save", count(2)));
            true
        };
        Ok(Done {
            status: Status(ok),
            payload: KVec(blob),
        })
    }
    fn save_lockout(&self) -> bool {
        self.ops.borrow_mut().push(("lockout", count(2)));
        self.lockout_ok.get()
    }
    fn enclave_lists(&self, _: &[u8; 16]) -> Option<bool> {
        Some(true)
    }
    fn forget_identity(&self, _: &[u8; 16], _: &CStr) {}
    fn read_component_states(&self) -> Option<Vec<u8>> {
        None
    }
    fn sbio_expect_ok(&self, _: &sbio::Operation) -> Option<()> {
        self.deletes.set(self.deletes.get() + 1);
        self.ops.borrow_mut().push(("delete", count(2)));
        Some(())
    }
    fn require_linux_bio_lane(&self) -> Result<()> {
        Ok(())
    }
    fn xart_writable(&self) -> bool {
        true
    }
    fn enclave_identities_for(&self, _: i32) -> Option<Vec<[u8; 16]>> {
        Some(Vec::new())
    }
    fn with_host_store<R>(&self, f: impl FnOnce(&()) -> R) -> Option<R> {
        Some(f(&()))
    }
    fn wait_until_awake(&self) -> Result<()> {
        Ok(())
    }
    fn capture_begin(&self) -> bool {
        true
    }
    fn capture_end(&self) {}
    fn open_enrolment_context(&self, _: sbio::UserId) -> bool {
        self.context_opens.set(self.context_opens.get() + 1);
        self.ops.borrow_mut().push(("select_context", count(2)));
        let saved = catacomb::write(PRIVATE_TYPE_CATACOMB_MASTER, b"fresh master").is_ok()
            && catacomb::write(PRIVATE_TYPE_CATACOMB_OWNER, b"fresh owner").is_ok();
        if saved && self.post_select_fault_owner_write.get() {
            unsafe { fixture_slot_write_open(SLOT_OWNER, 13) };
        }
        saved
    }
}
mod sbio {
    #[derive(Clone, Copy)]
    pub struct CatacombUser(i32);
    impl CatacombUser {
        pub const MASTER: Self = Self(-1);
        pub const OWNER: Self = Self(501);
        pub fn value(&self) -> i32 {
            self.0
        }
    }
    #[derive(Clone, Copy)]
    pub struct UserId(i32);
    impl UserId {
        pub fn new(v: i32) -> Option<Self> {
            Some(Self(v))
        }
    }
    pub struct IdentityV1 {
        uuid: [u8; 16],
    }
    impl IdentityV1 {
        pub fn test() -> Self {
            Self { uuid: [0x42; 16] }
        }
        pub fn uuid(&self) -> &[u8; 16] {
            &self.uuid
        }
    }
    pub struct ComponentStates;
    impl ComponentStates {
        pub fn new(_: &[u8]) -> Self {
            Self
        }
        pub fn state_for(&self, _: i32) -> Option<u32> {
            None
        }
    }
    pub const COMPONENT_STATE_ACTIVE: u32 = 1;
    pub struct SaveSelector;
    impl SaveSelector {
        pub fn new(_: CatacombUser) -> Self {
            Self
        }
    }
    pub struct Operation(pub bool);
    pub fn sbio_save_catacomb(_: &SaveSelector) -> Operation {
        Operation(false)
    }
    pub fn sbio_confirm_save(_: &SaveSelector) -> Operation {
        Operation(true)
    }
    pub fn sbio_delete_identity(_: &IdentityV1) -> Operation {
        Operation(false)
    }
    pub const SBIO_SAVED_MIN: usize = 0x22;
    pub const SBIO_SAVED_MAX: usize = 0x4b000;
    pub const SBIO_SAVED_USER_ID_AT: usize = 8;
}
fn save(s: &SepData) -> bool {
    s.save_catacomb(
        sbio::CatacombUser::OWNER,
        PRIVATE_TYPE_CATACOMB_USER,
        c"test user",
    )
}
fn save_master(s: &SepData) -> bool {
    s.save_catacomb(
        sbio::CatacombUser::MASTER,
        PRIVATE_TYPE_CATACOMB_MASTER,
        c"test master",
    )
}
#[test]
fn success_serialization_and_crc_round_trip() {
    let _f = Fixture::new();
    original();
    let b = vec![0x55; 10001];
    catacomb::write(PRIVATE_TYPE_CATACOMB_USER, &b).unwrap();
    assert_eq!(current(), b);
}
#[test]
fn maximum_size_round_trip() {
    let _f = Fixture::new();
    let b = vec![0x89; 1 << 20];
    catacomb::write(PRIVATE_TYPE_CATACOMB_USER, &b).unwrap();
    assert_eq!(current(), b);
}
#[test]
fn oversized_blob_preserves_previous() {
    let _f = Fixture::new();
    original();
    let old = current();
    assert_eq!(
        catacomb::write(PRIVATE_TYPE_CATACOMB_USER, &vec![0; (1 << 20) + 1]),
        Err(ENOSPC)
    );
    assert_eq!(current(), old);
}
#[test]
fn invalid_kind_preserves_previous() {
    let _f = Fixture::new();
    original();
    assert_eq!(catacomb::write(0, &[1]), Err(EINVAL));
    assert_eq!(current(), b"original valid catacomb");
}
#[test]
fn header_error_preserves_valid_previous() {
    let _f = Fixture::new();
    original();
    unsafe { fixture_write_fault(0, 5, 0) };
    assert_eq!(
        catacomb::write(PRIVATE_TYPE_CATACOMB_USER, b"replacement"),
        Err(EIO)
    );
    assert_eq!(current(), b"original valid catacomb");
}
#[test]
fn partial_payload_error_preserves_valid_previous() {
    let _f = Fixture::new();
    original();
    unsafe { fixture_write_fault(18, 5, 0) };
    assert_eq!(
        catacomb::write(PRIVATE_TYPE_CATACOMB_USER, b"replacement"),
        Err(EIO)
    );
    assert_eq!(current(), b"original valid catacomb");
}
#[test]
fn temporary_fsync_error_preserves_valid_previous() {
    let _f = Fixture::new();
    original();
    fault(TEMP_SYNC, 5);
    assert_eq!(
        catacomb::write(PRIVATE_TYPE_CATACOMB_USER, b"replacement"),
        Err(EIO)
    );
    assert_eq!(current(), b"original valid catacomb");
}
#[test]
fn rename_error_preserves_valid_previous() {
    let _f = Fixture::new();
    original();
    fault(RENAME, 5);
    assert_eq!(
        catacomb::write(PRIVATE_TYPE_CATACOMB_USER, b"replacement"),
        Err(EIO)
    );
    assert_eq!(current(), b"original valid catacomb");
}
#[test]
fn directory_fsync_error_keeps_complete_visible_replacement() {
    let _f = Fixture::new();
    original();
    fault(DIR_SYNC, 5);
    assert_eq!(
        catacomb::write(PRIVATE_TYPE_CATACOMB_USER, b"replacement"),
        Err(EIO)
    );
    assert_eq!(current(), b"replacement");
}
#[test]
fn short_write_round_trip() {
    let _f = Fixture::new();
    unsafe { fixture_short(3) };
    catacomb::write(PRIVATE_TYPE_CATACOMB_USER, b"replacement").unwrap();
    assert_eq!(current(), b"replacement");
}
#[test]
fn save_confirms_only_after_durable_replacement() {
    let _f = Fixture::new();
    original();
    let s = SepData::new();
    assert!(save(&s));
    assert_eq!(s.confirms.get(), 1);
    assert_eq!(count(2), 2);
    // Only the master save refreshes the lockout record.
    assert_eq!(s.ops(), [("save", 1), ("confirm", 2)]);
}
#[test]
fn writable_requires_a_destination_rename_can_replace() {
    let _f = Fixture::new();
    seed_all_catacombs();
    assert!(catacomb::writable(PRIVATE_TYPE_CATACOMB_USER));
    assert!(catacomb::writable(PRIVATE_TYPE_CATACOMB_MASTER));
    // A file bind mount on USER opens for writing, but rename over it fails (EBUSY).
    // MASTER remains replaceable: checks must inspect the requested basename.
    unsafe { fixture_slot_mountpoint(SLOT_USER, 1) };
    let path = c"/var/lib/apple-sep-catacomb-user.bin";
    assert!(shim::StoreFile::open_writable_existing(path).is_ok());
    assert!(!catacomb::writable(PRIVATE_TYPE_CATACOMB_USER));
    assert!(catacomb::writable(PRIVATE_TYPE_CATACOMB_MASTER));
    assert_eq!(current(), b"user valid catacomb");
}
#[test]
fn writable_still_requires_writable_open_on_replaceable_destination() {
    let _f = Fixture::new();
    seed_all_catacombs();
    let path = c"/var/lib/apple-sep-catacomb-user.bin";
    unsafe { fixture_slot_write_open(SLOT_USER, 13) };
    assert!(shim::replaceable(path).is_ok());
    assert!(!catacomb::writable(PRIVATE_TYPE_CATACOMB_USER));
    assert!(catacomb::writable(PRIVATE_TYPE_CATACOMB_MASTER));
}
#[test]
fn writable_requires_a_writable_parent() {
    let _f = Fixture::new();
    seed_all_catacombs();
    fault(DIR_PERM, 13);
    assert!(!catacomb::writable(PRIVATE_TYPE_CATACOMB_USER));
    assert!(!catacomb::writable(PRIVATE_TYPE_CATACOMB_MASTER));
}
#[test]
fn delete_boundary_sends_command_only_when_all_catacombs_replaceable() {
    let _f = Fixture::new();
    seed_all_catacombs();
    let s = SepData::new();
    let id = sbio::IdentityV1::test();
    assert_eq!(s.delete_identity(&id, false), Ok(true));
    assert_eq!(s.deletes.get(), 1);
}
#[test]
fn delete_boundary_refuses_bind_mounted_catacomb_with_zero_deletes() {
    for slot in [SLOT_USER, SLOT_MASTER, SLOT_OWNER] {
        let _f = Fixture::new();
        seed_all_catacombs();
        unsafe { fixture_slot_mountpoint(slot, 1) };
        let s = SepData::new();
        let id = sbio::IdentityV1::test();
        assert_eq!(s.delete_identity(&id, false), Err(EIO));
        assert_eq!(s.deletes.get(), 0);
    }
}
#[test]
fn delete_boundary_refuses_append_only_and_unwritable_parent_with_zero_deletes() {
    for (stage, err) in [(DIR_APPEND, 1), (DIR_PERM, 13), (MOUNT, 30)] {
        let _f = Fixture::new();
        seed_all_catacombs();
        fault(stage, err);
        let s = SepData::new();
        let id = sbio::IdentityV1::test();
        assert_eq!(s.delete_identity(&id, false), Err(EIO));
        assert_eq!(s.deletes.get(), 0);
    }
}
#[test]
fn delete_boundary_refuses_blocked_reserved_temp_with_zero_deletes() {
    for (slot, kind, mp, md, sec) in [
        (SLOT_USER, 2, 0, 0, 0),
        (SLOT_MASTER, 3, 0, 0, 0),
        (SLOT_OWNER, 1, 1, 0, 0),
        (SLOT_USER, 1, 0, 1, 0),
        (SLOT_MASTER, 1, 0, 0, 1),
    ] {
        let _f = Fixture::new();
        seed_all_catacombs();
        unsafe { fixture_slot_stale(slot, 1, kind, mp, md, sec, 0) };
        let s = SepData::new();
        let id = sbio::IdentityV1::test();
        assert_eq!(s.delete_identity(&id, false), Err(EIO));
        assert_eq!(s.deletes.get(), 0);
        assert_eq!(count(8), 0);
    }
}
#[test]
fn delete_boundary_refuses_unwritable_open_with_zero_deletes() {
    let _f = Fixture::new();
    seed_all_catacombs();
    unsafe { fixture_slot_write_open(SLOT_USER, 13) };
    let s = SepData::new();
    let id = sbio::IdentityV1::test();
    assert_eq!(s.delete_identity(&id, false), Err(EIO));
    assert_eq!(s.deletes.get(), 0);
}
#[test]
fn delete_boundary_refuses_unmapped_creator_id_with_zero_deletes() {
    let _f = Fixture::new();
    seed_all_catacombs();
    fault(CREATOR_MAP, 1);
    let s = SepData::new();
    let id = sbio::IdentityV1::test();
    assert_eq!(s.delete_identity(&id, false), Err(EIO));
    assert_eq!(s.deletes.get(), 0);
}
#[test]
fn delete_boundary_refuses_missing_dir_fsync_and_rename_with_zero_deletes() {
    for stage in [DIR_NO_FSYNC, DIR_NO_RENAME] {
        let _f = Fixture::new();
        seed_all_catacombs();
        let renames_before = count(0);
        let attempts_before = count(7);
        fault(stage, 1);
        let s = SepData::new();
        let id = sbio::IdentityV1::test();
        assert_eq!(s.delete_identity(&id, false), Err(EIO));
        assert_eq!(s.deletes.get(), 0);
        assert_eq!(count(0), renames_before);
        assert_eq!(count(7), attempts_before);
    }
}
#[test]
fn prepare_enrol_succeeds_and_retains_post_save_validation() {
    {
        let _f = Fixture::new();
        let s = SepData::new();
        assert_eq!(s.bio_prepare_enrol(0), Ok(bio::Handled));
        assert_eq!(s.context_opens.get(), 1);
        assert!(s.empty_context_prepared.load(Relaxed));
        assert!(catacomb::is_saved(PRIVATE_TYPE_CATACOMB_OWNER));
        assert!(catacomb::is_saved(PRIVATE_TYPE_CATACOMB_MASTER));
    }
    // Post-save writable() refusal still fails with EROFS.
    {
        let _f = Fixture::new();
        let s = SepData::new();
        s.post_select_fault_owner_write.set(true);
        assert_eq!(s.bio_prepare_enrol(0), Err(EROFS));
        assert_eq!(s.context_opens.get(), 1);
        assert!(!s.empty_context_prepared.load(Relaxed));
    }
}
#[test]
fn prepare_enrol_refuses_before_creating_owner_or_selecting_context() {
    // Append-only parent: must not strand an empty owner file or call SELECT_CONTEXT.
    {
        let _f = Fixture::new();
        fault(DIR_APPEND, 1);
        let s = SepData::new();
        assert_eq!(s.bio_prepare_enrol(0), Err(EROFS));
        assert_eq!(s.context_opens.get(), 0);
        assert!(catacomb::is_absent(PRIVATE_TYPE_CATACOMB_OWNER));
    }
    // Non-regular entry at owner's reserved temporary name.
    {
        let _f = Fixture::new();
        unsafe { fixture_slot_stale(SLOT_OWNER, 1, 2, 0, 0, 0, 0) };
        let s = SepData::new();
        assert_eq!(s.bio_prepare_enrol(0), Err(EEXIST));
        assert_eq!(s.context_opens.get(), 0);
        assert!(catacomb::is_absent(PRIVATE_TYPE_CATACOMB_OWNER));
        assert_eq!(count(8), 0);
    }
    // Non-regular entry at master's reserved temporary name.
    {
        let _f = Fixture::new();
        unsafe { fixture_slot_stale(SLOT_MASTER, 1, 3, 0, 0, 0, 0) };
        let s = SepData::new();
        assert_eq!(s.bio_prepare_enrol(0), Err(EROFS));
        assert_eq!(s.context_opens.get(), 0);
        assert!(catacomb::is_absent(PRIVATE_TYPE_CATACOMB_OWNER));
        assert_eq!(count(8), 0);
    }
}
#[test]
fn prepare_enrol_refuses_unmapped_creator_id_before_creating_owner() {
    let _f = Fixture::new();
    fault(CREATOR_MAP, 1);
    let s = SepData::new();
    assert_eq!(s.bio_prepare_enrol(0), Err(EROFS));
    assert_eq!(s.context_opens.get(), 0);
    assert!(catacomb::is_absent(PRIVATE_TYPE_CATACOMB_OWNER));
}
#[test]
fn prepare_enrol_refuses_missing_dir_fsync_before_creating_owner() {
    let _f = Fixture::new();
    fault(DIR_NO_FSYNC, 1);
    let s = SepData::new();
    assert_eq!(s.bio_prepare_enrol(0), Err(EROFS));
    assert_eq!(s.context_opens.get(), 0);
    assert!(catacomb::is_absent(PRIVATE_TYPE_CATACOMB_OWNER));
}
#[test]
fn prepare_enrol_refuses_existing_master_with_failed_writable_open() {
    let _f = Fixture::new();
    catacomb::write(PRIVATE_TYPE_CATACOMB_MASTER, b"existing master").unwrap();
    unsafe { fixture_slot_write_open(SLOT_MASTER, 13) };
    let s = SepData::new();
    assert_eq!(s.bio_prepare_enrol(0), Err(EROFS));
    assert_eq!(s.context_opens.get(), 0);
    assert!(catacomb::is_absent(PRIVATE_TYPE_CATACOMB_OWNER));
}
#[test]
fn prepare_enrol_unwinds_cleanly_on_parent_fsync_failure_in_owner_sync() {
    let _f = Fixture::new();
    fault(DIR_SYNC, 5);
    let s = SepData::new();
    assert_eq!(s.bio_prepare_enrol(0), Err(EIO));
    assert_eq!(s.context_opens.get(), 0);
    assert!(!s.empty_context_prepared.load(Relaxed));
    assert_eq!(count(2), 1);
}
#[test]
fn publishable_does_not_read_existing_catacomb_and_refuses_unreadable_unwritable() {
    let _f = Fixture::new();
    catacomb::write(PRIVATE_TYPE_CATACOMB_USER, b"existing user").unwrap();
    // Existing and writable, then absent: admitted without reading.
    assert!(catacomb::publishable(PRIVATE_TYPE_CATACOMB_USER));
    assert!(catacomb::publishable(PRIVATE_TYPE_CATACOMB_MASTER));
    // Existing but not writable: refused, still without reading.
    unsafe { fixture_slot_write_open(SLOT_USER, 13) };
    assert!(!catacomb::publishable(PRIVATE_TYPE_CATACOMB_USER));
    assert_eq!(count(9), 0);
    // Neither writable nor provably missing: refused.
    fault(READ_OPEN, 13);
    assert!(!catacomb::publishable(PRIVATE_TYPE_CATACOMB_USER));
    assert!(!catacomb::publishable(PRIVATE_TYPE_CATACOMB_MASTER));
    assert_eq!(count(9), 0);
}
#[test]
fn preflight_enrol_distinguishes_absent_and_unwritable_optional_catacombs() {
    // First enrollment (live_count == Some(0)): OWNER present, USER and MASTER
    // truly absent under a healthy parent must be admitted.
    {
        let _f = Fixture::new();
        catacomb::write(PRIVATE_TYPE_CATACOMB_OWNER, b"owner valid catacomb").unwrap();
        let s = SepData::new();
        assert_eq!(s.preflight_enrol(Some(0)), Ok(()));
    }
    // Optional existing USER whose writable open fails must be refused.
    {
        let _f = Fixture::new();
        catacomb::write(PRIVATE_TYPE_CATACOMB_OWNER, b"owner valid catacomb").unwrap();
        catacomb::write(PRIVATE_TYPE_CATACOMB_USER, b"existing user").unwrap();
        unsafe { fixture_slot_write_open(SLOT_USER, 13) };
        let s = SepData::new();
        assert_eq!(s.preflight_enrol(Some(0)), Err(EROFS));
    }
    // Optional existing MASTER whose writable open fails must be refused.
    {
        let _f = Fixture::new();
        catacomb::write(PRIVATE_TYPE_CATACOMB_OWNER, b"owner valid catacomb").unwrap();
        catacomb::write(PRIVATE_TYPE_CATACOMB_MASTER, b"existing master").unwrap();
        unsafe { fixture_slot_write_open(SLOT_MASTER, 13) };
        let s = SepData::new();
        assert_eq!(s.preflight_enrol(Some(0)), Err(EROFS));
    }
    // Truly absent USER/MASTER with an occupied non-regular .new must fail.
    {
        let _f = Fixture::new();
        catacomb::write(PRIVATE_TYPE_CATACOMB_OWNER, b"owner valid catacomb").unwrap();
        unsafe { fixture_slot_stale(SLOT_USER, 1, 2, 0, 0, 0, 0) };
        let s = SepData::new();
        assert_eq!(s.preflight_enrol(Some(0)), Err(EROFS));
    }
    // Truly absent USER/MASTER under unmapped creator ID or missing dir fsync must fail.
    for stage in [CREATOR_MAP, DIR_NO_FSYNC] {
        let _f = Fixture::new();
        catacomb::write(PRIVATE_TYPE_CATACOMB_OWNER, b"owner valid catacomb").unwrap();
        fault(stage, 1);
        let s = SepData::new();
        assert_eq!(s.preflight_enrol(Some(0)), Err(EROFS));
    }
    // Existing USER/MASTER/OWNER (live_count == Some(1)) under unmapped creator ID or
    // missing dir fsync must also fail.
    for stage in [CREATOR_MAP, DIR_NO_FSYNC] {
        let _f = Fixture::new();
        seed_all_catacombs();
        fault(stage, 1);
        let s = SepData::new();
        assert_eq!(s.preflight_enrol(Some(1)), Err(EROFS));
    }
}
#[test]
fn master_save_publishes_then_confirms_then_refreshes_lockout() {
    let _f = Fixture::new();
    let s = SepData::master();
    assert!(save_master(&s));
    assert_eq!(s.ops(), [("save", 0), ("confirm", 1), ("lockout", 1)]);
}
#[test]
fn master_directory_sync_error_skips_confirm_and_lockout() {
    let _f = Fixture::new();
    fault(DIR_SYNC, 5);
    let s = SepData::master();
    assert!(!save_master(&s));
    assert_eq!(s.ops(), [("save", 0)]);
}
#[test]
fn master_refused_confirm_skips_lockout() {
    let _f = Fixture::new();
    let s = SepData::master();
    s.confirm_ok.set(false);
    assert!(!save_master(&s));
    assert_eq!(s.ops(), [("save", 0), ("confirm", 1)]);
}
#[test]
fn master_lockout_refresh_failure_after_confirm_only_warns() {
    let _f = Fixture::new();
    let s = SepData::master();
    s.lockout_ok.set(false);
    assert!(save_master(&s));
    assert_eq!(s.ops(), [("save", 0), ("confirm", 1), ("lockout", 1)]);
}
#[test]
fn save_with_payload_error_keeps_old_and_never_confirms() {
    let _f = Fixture::new();
    original();
    unsafe { fixture_write_fault(18, 5, 0) };
    let s = SepData::new();
    assert!(!save(&s));
    assert_eq!(s.confirms.get(), 0);
    assert_eq!(current(), b"original valid catacomb");
}
#[test]
fn save_with_file_fsync_error_keeps_old_and_never_confirms() {
    let _f = Fixture::new();
    original();
    fault(TEMP_SYNC, 5);
    let s = SepData::new();
    assert!(!save(&s));
    assert_eq!(s.confirms.get(), 0);
    assert_eq!(current(), b"original valid catacomb");
}
#[test]
fn save_with_temp_close_error_keeps_old_and_never_confirms() {
    let _f = Fixture::new();
    original();
    fault(TEMP_CLOSE, 5);
    let s = SepData::new();
    assert!(!save(&s));
    assert_eq!(s.confirms.get(), 0);
    assert_eq!(count(0), 1);
    assert_eq!(count(3), 0);
    fault(TEMP_CLOSE, 0);
    assert_eq!(current(), b"original valid catacomb");
}
#[test]
fn save_with_rename_error_keeps_old_and_never_confirms() {
    let _f = Fixture::new();
    original();
    fault(RENAME, 5);
    let s = SepData::new();
    assert!(!save(&s));
    assert_eq!(s.confirms.get(), 0);
    assert_eq!(current(), b"original valid catacomb");
}
#[test]
fn save_with_directory_fsync_error_never_confirms() {
    let _f = Fixture::new();
    original();
    fault(DIR_SYNC, 5);
    let s = SepData::new();
    assert!(!save(&s));
    assert_eq!(s.confirms.get(), 0);
    assert_eq!(current().len(), sbio::SBIO_SAVED_MIN);
}
