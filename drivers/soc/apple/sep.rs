// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj
//! Driver for the Apple SEP (Secure Enclave Processor): attaches to a running
//! SEP over the AP mailbox, drives Touch ID enrol/verify via `/dev/sep-bio`,
//! and registers a hwrng and a SEP-backed `trusted` key source.
#![recursion_limit = "2048"]

#[cfg(not(CONFIG_OF_DYNAMIC))]
compile_error!("apple_sep requires CONFIG_OF_DYNAMIC: the SEP and DART nodes ship disabled and are enabled with a device-tree changeset");

mod bio;
mod catacomb;
mod control;
mod der;
mod dt;
mod fv;
mod hwrng;
mod image;
mod keybag;
mod proto;
mod refkey;
mod refkey_seal;
mod rxring;
mod sbio;
mod scrd;
mod sensor;
mod shim;
mod shmem;
mod sks;
mod store;
mod transfer;
mod trusted;
mod xarm;
mod xart_store;

use kernel::{
    device,
    dma,
    driver,
    new_mutex,
    of,
    platform,
    prelude::*,
    soc::apple::mailbox::{
        MailCallback,
        Mailbox,
        Message, //
    },
    sync::{
        aref::ARef,
        atomic::{
            Atomic,
            Relaxed, //
        },
        new_condvar,
        Arc,
        CondVar,
        CondVarTimeoutResult,
        Mutex, //
    },
    time,
    types::ForeignOwnable,
    workqueue::{
        self,
        impl_has_delayed_work,
        impl_has_work,
        new_delayed_work,
        new_work,
        DelayedWork,
        Work,
        WorkItem, //
    }, //
};

const SETTLE_MS: time::Msecs = 200;
const FIRST_RESPONSE_MS: time::Msecs = 3000;

const SETTLE_WORK_ID: u64 = 1;

const ENROL_WORK_ID: u64 = 2;
static_assert!(ENROL_WORK_ID != SETTLE_WORK_ID && ENROL_WORK_ID != 0);
const VERIFY_WORK_ID: u64 = 3;
static_assert!(VERIFY_WORK_ID != ENROL_WORK_ID && VERIFY_WORK_ID != SETTLE_WORK_ID);
static_assert!(VERIFY_WORK_ID != 0);

const EXCHANGE_TIMEOUT_MS: time::Msecs = 15000;

const PHASE_ATTACH: u32 = 0;
const PHASE_EXCHANGE: u32 = 1;
const PHASE_READY: u32 = 2;

const ENDPOINTS_BEFORE_EXCHANGE: usize = 7;

const OOL_SIZE_XARM: usize = 0x8000;

const OOL_SIZE_SBIO: usize = 0x4000;

const OOL_SIZE_SCRD: usize = 0x4000;

const SBIO_TIMEOUT_MS: time::Msecs = 5000;

const SKS_ALLOC: usize = 0x8000;

const SKS_SECRET_LEN: usize = 32;
const SKS_MAX_CAPTURE: usize = 8;

const SCRD_MAX_CAPTURE: usize = 8;

const SCRD_TIMEOUT_MS: time::Msecs = 2000;

const SKS_TIMEOUT_MS: time::Msecs = 2000;

const SKS_TIMEOUT_PER_KIB_MS: time::Msecs = 6000;

const SKS_TIMEOUT_MAX_MS: time::Msecs = 30_000;

const SKS_MAX_ABANDONED: usize = 8;

const fn sks_timeout_for(image_len: usize) -> time::Msecs {
    let kib = image_len / 1024;
    let kib = if kib > u16::MAX as usize {
        u16::MAX as time::Msecs
    } else {
        kib as time::Msecs
    };
    let scaled = SKS_TIMEOUT_MS.saturating_add(SKS_TIMEOUT_PER_KIB_MS.saturating_mul(kib));
    if scaled > SKS_TIMEOUT_MAX_MS {
        SKS_TIMEOUT_MAX_MS
    } else {
        scaled
    }
}

static_assert!(sks_timeout_for(64) == SKS_TIMEOUT_MS);
static_assert!(sks_timeout_for(1508) > SKS_TIMEOUT_MS);
static_assert!(sks_timeout_for(usize::MAX) == SKS_TIMEOUT_MAX_MS);

const SKS_MAX_SET_ASIDE: u32 = 16;

const DMA_RING_SIZE: usize = 4 * (1 << 12);

const OOL_WRITE_POLL_MS: u32 = 1;
const OOL_WRITE_POLL_ATTEMPTS: u32 = 200;

const OOL_POISON_INBOUND: u8 = 0xA5;
const OOL_POISON_OUTBOUND: u8 = 0x5A;

const PROTECTED_DATA_AVAILABLE: bool = true;

const RNG_MAX_WORDS_PER_READ: usize = 16;

extern "C" {
    fn sep_cancel_work_sync(work: *mut c_void);
    fn sep_cancel_delayed_work_sync(work: *mut c_void);
}

const MAX_ENDPOINTS: usize = 64;

#[derive(Clone, Copy)]
struct Endpoint {
    id: u8,
    fourcc: proto::Fourcc,
    have_descriptor: bool,
    have_config: bool,
    descriptor_msg0: u64,
    descriptor_msg1: u32,
    config_msg0: u64,
    config_msg1: u32,
}

impl Endpoint {
    fn new(id: u8) -> Self {
        Endpoint {
            id,
            fourcc: proto::Fourcc::ZERO,
            have_descriptor: false,
            have_config: false,
            descriptor_msg0: 0,
            descriptor_msg1: 0,
            config_msg0: 0,
            config_msg1: 0,
        }
    }
}

struct OolPair {
    endpoint: u8,
    allocated: usize,
    declared_in: usize,
    declared_out: usize,
    inbound: shmem::ShMem,
    outbound: shmem::ShMem,
    registered: bool,
}

fn sks_declared_sizes() -> (usize, usize) {
    (0x8000, 0x4000)
}

struct Hex<'a>(&'a [u8]);

impl kernel::fmt::Display for Hex<'_> {
    fn fmt(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        use core::fmt::Write;
        const DIGITS: &[u8; 16] = b"0123456789abcdef";
        for (i, byte) in self.0.iter().enumerate() {
            if i > 0 {
                f.write_char(' ')?;
            }
            f.write_char(DIGITS[(*byte >> 4) as usize] as char)?;
            f.write_char(DIGITS[(*byte & 0x0f) as usize] as char)?;
        }
        Ok(())
    }
}

const SKS_LOCK_STATE_VARIANT: u32 = 1;

#[derive(Clone, Copy)]
enum LockState {
    Unlocked,
}

impl LockState {
    const fn wire(self) -> i32 {
        match self {
            LockState::Unlocked => 0,
        }
    }
}

static_assert!(LockState::Unlocked.wire() == 0);

const SKS_LOCK_STATE_FLAGS: u64 = 0;

enum SbioOutcome {
    Ok(KVec<u8>),
    // status 0x01
    PrerequisiteMissing,
    // status 0x16; NOT "malformed" — that mapping is another service's status space
    Status16,
    Other,
}

const BRINGUP_FRESH: u32 = 0;
const BRINGUP_IDENTIFIED: u32 = 1;
const BRINGUP_ESTABLISHED: u32 = 2;

pub(crate) struct PatchLoaded(());

pub(crate) struct ParametersApplied(());

pub(crate) struct CalibrationBlob(kernel::firmware::Firmware);

impl CalibrationBlob {
    fn new(fw: kernel::firmware::Firmware) -> CalibrationBlob {
        CalibrationBlob(fw)
    }

    pub(crate) fn bytes(&self) -> &[u8] {
        self.0.data()
    }
}

struct EnrolMaterial {
    special: crate::sks::SpecialHandle,
    secret: Secret,
}

struct ImageContext<'a> {
    sep: &'a SepData,
}

impl Drop for ImageContext<'_> {
    fn drop(&mut self) {
        let op = crate::sbio::sbio_image_cleanup();
        let _ = self.sep.sbio_call(&op);
    }
}

struct OpenEnrolment<'a> {
    sep: &'a SepData,
    armed: bool,
}

impl OpenEnrolment<'_> {
    fn completed(mut self) {
        self.armed = false;
        self.sep.enrol_open.store(false, Relaxed);
    }
}

impl Drop for OpenEnrolment<'_> {
    fn drop(&mut self) {
        if !self.armed {
            return;
        }
        let _ = self.sep.sbio_call(&crate::sbio::sbio_cancel_operation());
        self.sep.enrol_open.store(false, Relaxed);
    }
}

enum ImageOutcome {
    // `complete` flag from 0xbfe
    Progress {
        stage: u32,
        percent: u32,
        complete: bool,
        has_template: bool,
    },
    Retry,
    NoFinger,
    Failed(u32),
}

enum CaptureWait {
    Ready(u32),
    Timeout,
    Fault(u8),
    Abandon,
}

const ENROL_STATUS_SENSOR: u32 = 1;
const ENROL_STATUS_ENCLAVE: u32 = 2;
const ENROL_STATUS_TOO_MANY: u32 = 3;
const ENROL_STATUS_TIMEOUT: u32 = 4;
#[derive(Clone, Copy)]
enum LoadAnswer {
    StatusZero,
    ColdTransition,
    // status 0x101: SEP re-activated the component from its own xART records
    AlreadyActive,
}

impl LoadAnswer {
    fn describe(self) -> &'static CStr {
        match self {
            LoadAnswer::StatusZero => c"status 0",
            LoadAnswer::ColdTransition => c"the cold-transition status 0x8002",
            LoadAnswer::AlreadyActive => c"the already-active status 0x101",
        }
    }
}

enum RestoreOutcome {
    Restored,
    AlreadyActive,
    NoStoredFile,
    Failed,
    // 0x8002 cold-transition; tolerated only for the lockout record, never a catacomb
    EmptyTolerated,
}

const PRIVATE_TYPE_CATACOMB_MASTER: u8 = 0xF2;
const PRIVATE_TYPE_CATACOMB_OWNER: u8 = 0xF3;
const PRIVATE_TYPE_CATACOMB_USER: u8 = 0xF4;
const PRIVATE_TYPE_LOCKOUT: u8 = 0xF5;
// 0xF1 is the identity index's type; a colliding type silently mis-restores
static_assert!(PRIVATE_TYPE_CATACOMB_MASTER != 0xF1);
static_assert!(PRIVATE_TYPE_CATACOMB_OWNER != PRIVATE_TYPE_CATACOMB_MASTER);
static_assert!(PRIVATE_TYPE_CATACOMB_USER != PRIVATE_TYPE_CATACOMB_OWNER);
static_assert!(PRIVATE_TYPE_CATACOMB_USER != PRIVATE_TYPE_CATACOMB_MASTER);
static_assert!(PRIVATE_TYPE_LOCKOUT != PRIVATE_TYPE_CATACOMB_USER);
static_assert!(PRIVATE_TYPE_LOCKOUT != PRIVATE_TYPE_CATACOMB_OWNER);
static_assert!(PRIVATE_TYPE_LOCKOUT != PRIVATE_TYPE_CATACOMB_MASTER);

const ENROL_STATUS_UNFILED: u32 = 6;
static_assert!(ENROL_STATUS_UNFILED != ENROL_STATUS_RETRY);
static_assert!(ENROL_STATUS_UNFILED != ENROL_STATUS_ENCLAVE);
static_assert!(ENROL_STATUS_UNFILED != ENROL_STATUS_SENSOR);
static_assert!(ENROL_STATUS_UNFILED != ENROL_STATUS_TIMEOUT);
static_assert!(ENROL_STATUS_UNFILED != ENROL_STATUS_TOO_MANY);

const ENROL_STATUS_RETRY: u32 = 5;
static_assert!(ENROL_STATUS_RETRY != ENROL_STATUS_TIMEOUT);
static_assert!(ENROL_STATUS_RETRY != ENROL_STATUS_ENCLAVE);
static_assert!(ENROL_STATUS_RETRY != ENROL_STATUS_SENSOR);

const ENROL_MAX_CAPTURES: u32 = 12;

const ENROL_POLL_MS: u32 = 2;

const ENROL_CAPTURE_TIMEOUT_MS: u32 = 60_000;
const ENROL_POLL_ATTEMPTS: u32 = ENROL_CAPTURE_TIMEOUT_MS / ENROL_POLL_MS;
static_assert!(ENROL_POLL_ATTEMPTS * ENROL_POLL_MS == ENROL_CAPTURE_TIMEOUT_MS);

const ENROL_REPOSITION_MS: u32 = 1800;

const MATCH_SETTLE_MS: u32 = ENROL_REPOSITION_MS;

const ENROL_IDLE_TIMEOUT_MS: u32 = 2000;

const PATCH_POLL_MS: u32 = 20;
const PATCH_POLL_ATTEMPTS: u32 = 250;

const CALIBRATION_FIRMWARE: &CStr = c"apple/mesa_calibration.bin";

const SBIO_PROBE_USER_ID: i32 = 1000;
static_assert!(SBIO_PROBE_USER_ID >= crate::sks::SKS_DESIGNATE_USER_MIN);
static_assert!(SBIO_PROBE_USER_ID == bio::ENROL_USER_ID);
static_assert!(SBIO_PROBE_USER_ID > 0);

const SKS_LOAD_REPLY_LEN: usize = 8;

#[derive(Clone, Copy)]

#[must_use]
struct Healthy(());

struct SksRequest {
    name: &'static CStr,
    msg: Message,
    img: image::RequestImage,
}

struct SksOutcome {
    reply: crate::sks::SksReply,
    response: Secret,
}

pub(crate) struct Secret(pub(crate) KVec<u8>);

impl Secret {
    pub(crate) fn empty() -> Secret {
        Secret(KVec::new())
    }
}

impl core::ops::Deref for Secret {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        &self.0
    }
}

impl Drop for Secret {
    fn drop(&mut self) {
        image::wipe(&mut self.0);
    }
}

#[derive(Clone, Copy)]
struct Abandoned {
    selector: u8,
    seq: u8,
    label: &'static CStr,
    waited_ms: u64,
}

struct SksProbe {
    active: bool,
    captured: KVec<Message>,
    label: Option<&'static CStr>,
    unsolicited: u32,
    abandoned: [Option<Abandoned>; SKS_MAX_ABANDONED],
    abandoned_next: usize,
}

impl SksProbe {
    fn new() -> Self {
        SksProbe {
            active: false,
            captured: KVec::new(),
            label: None,
            unsolicited: 0,
            abandoned: [None; SKS_MAX_ABANDONED],
            abandoned_next: 0,
        }
    }
}

struct ScrdProbe {
    active: bool,
    captured: KVec<Message>,
}

impl ScrdProbe {
    fn new() -> Self {
        ScrdProbe {
            active: false,
            captured: KVec::new(),
        }
    }
}

struct XarmState {
    deferred_query: Option<u8>,
    serviced: u32,
    refused: u32,
    os_uuid: Option<[u8; 16]>,
}

impl XarmState {
    fn new() -> Self {
        XarmState {
            deferred_query: None,
            serviced: 0,
            refused: 0,
            os_uuid: None,
        }
    }
}

struct EndpointTable {
    eps: KVec<Endpoint>,
    discovery_msgs: u32,
    unknown_types: u32,
    dirty: bool,
}

impl EndpointTable {
    fn new() -> Self {
        EndpointTable {
            eps: KVec::new(),
            discovery_msgs: 0,
            unknown_types: 0,
            dirty: false,
        }
    }

    fn slot(&mut self, id: u8) -> Result<usize> {
        if let Some(i) = self.eps.iter().position(|e| e.id == id) {
            return Ok(i);
        }
        if self.eps.len() >= MAX_ENDPOINTS {
            return Err(ENOSPC);
        }
        self.eps.push(Endpoint::new(id), GFP_KERNEL)?;
        self.dirty = true;
        Ok(self.eps.len() - 1)
    }
}


struct MachineRefKey {
    blob: KVec<u8>,
    pub_raw: KVec<u8>,
}

#[pin_data]
struct SepData {
    dev: ARef<device::Device>,

    #[pin]
    mbox: Mutex<Option<Mailbox<SepData>>>,

    #[pin]
    shmem: Mutex<Option<shmem::ShMem>>,

    #[pin]
    endpoints: Mutex<EndpointTable>,

    #[pin]
    control: Mutex<control::ControlState>,
    #[pin]
    control_wq: CondVar,

    sks_wedged: Atomic<u32>,

    sks_seq: Atomic<u32>,

    // u32, not u8: the kernel's Rust atomics have no u8 AtomicType
    phase: Atomic<u32>,

    #[pin]
    ool_xarm: Mutex<Option<OolPair>>,

    #[pin]
    ool_sbio: Mutex<Option<OolPair>>,

    #[pin]
    dma_ring: Mutex<Option<shmem::ShMem>>,

    #[pin]
    sbio_rx: Mutex<transfer::Reassembly>,

    #[pin]
    sbio_wq: CondVar,

    sbio_ready: Atomic<bool>,

    templates_restored: Atomic<bool>,

    sensor_calibrated: Atomic<bool>,

    enrol_open: Atomic<bool>,

    #[pin]
    enrol_material: Mutex<Option<EnrolMaterial>>,

    #[pin]
    enrol_identity_candidates: Mutex<KVec<[u8; bio::UUID_LEN]>>,

    last_capture_end_ns: Atomic<u64>,

    device_view_synced: Atomic<bool>,

    keybag_designated: Atomic<bool>,

    bringup_started: Atomic<bool>,
    touchid_started: Atomic<bool>,
    touchid_failed: Atomic<bool>,

    bringup: Atomic<u32>,

    #[pin]
    ool_sks: Mutex<Option<OolPair>>,

    #[pin]
    sks_probe: Mutex<SksProbe>,
    #[pin]
    sks_wq: CondVar,

    #[pin]
    ool_scrd: Mutex<Option<OolPair>>,
    #[pin]
    scrd_probe: Mutex<ScrdProbe>,
    #[pin]
    scrd_wq: CondVar,

    sensor_present: Atomic<bool>,

    #[pin]
    bio_session: Mutex<bio::Session>,

    #[pin]
    bio_index: Mutex<bio::IdentityIndex>,

    #[pin]
    bio_dev: Mutex<Option<shim::BioChardev>>,

    #[pin]
    store: Mutex<Option<xart_store::Store>>,

    #[pin]
    host_store: Mutex<Option<store::Store>>,

    #[pin]
    xarm: Mutex<XarmState>,

    #[pin]
    rng: Mutex<Option<hwrng::HwRngHandle>>,

    #[pin]
    machine_refkey: Mutex<Option<MachineRefKey>>,

    #[pin]
    fv_volumes: Mutex<KVec<fv::VolumeMap>>,

    rng_shutdown: Atomic<bool>,

    rng_failures: Atomic<u64>,

    rx: rxring::RxRing,

    rx_count: Atomic<u64>,
    settle_mark: Atomic<u64>,
    settle_idle_ticks: Atomic<u64>,

    registered: Atomic<bool>,

    shutting_down: Atomic<bool>,

    #[pin]
    rx_work: Work<SepData>,

    #[pin]
    settle_work: DelayedWork<SepData, SETTLE_WORK_ID>,

    #[pin]
    enrol_work: Work<SepData, ENROL_WORK_ID>,

    #[pin]
    verify_work: Work<SepData, VERIFY_WORK_ID>,
}

impl_has_work! {
    impl HasWork<Self, 0> for SepData { self.rx_work }
    impl HasWork<Self, ENROL_WORK_ID> for SepData { self.enrol_work }
    impl HasWork<Self, VERIFY_WORK_ID> for SepData { self.verify_work }
}

impl_has_delayed_work! {
    impl HasDelayedWork<Self, 1> for SepData { self.settle_work }
}

// SAFETY: every field is either internally synchronised (the mutexes, the
// atomics, the lock-free ring) or immutable after construction. The DMA buffer
// is only touched in probe, before the SEP has been told it exists, and at
// unbind, after the mailbox has been stopped.
unsafe impl Send for SepData {}
// SAFETY: see above.
unsafe impl Sync for SepData {}

impl SepData {
    fn new(pdev: &platform::Device<device::Core>) -> Result<Arc<SepData>> {
        let built = shmem::build(pdev)?;
        let dev: &device::Device<device::Core> = pdev.as_ref();

        let buf = built.buf;

        let ool_xarm = Self::alloc_ool(
            dev,
            xarm::EP_XARM,
            OOL_SIZE_XARM,
            (OOL_SIZE_XARM, OOL_SIZE_XARM),
        )?;
        let ool_sbio = Self::alloc_ool(
            dev,
            proto::EP_SBIO,
            OOL_SIZE_SBIO,
            (OOL_SIZE_SBIO, OOL_SIZE_SBIO),
        )?;
        let sks_declared = sks_declared_sizes();
        let ool_sks = Self::alloc_ool(dev, proto::EP_SKS, SKS_ALLOC, sks_declared)?;
        let ool_scrd = Self::alloc_ool(
            dev,
            proto::EP_SCRD,
            OOL_SIZE_SCRD,
            (OOL_SIZE_SCRD, OOL_SIZE_SCRD),
        )?;
        let dma_ring = dma::Coherent::<u8>::zeroed_slice(dev, DMA_RING_SIZE, GFP_KERNEL)?;

        let xart_writes = *module_parameters::xart_writes.value() != 0;
        let store = match xart_store::Store::open(xart_writes) {
            Ok(store) => {
                let (slots, records, revision, malformed, duplicates, repaired, writable) =
                    store.summary();
                dev_info!(
                    dev,
                    "xART: {} slots, {} live records, max revision {}, {} malformed, {} duplicate, {} repaired; writes {}\n",
                    slots,
                    records,
                    revision,
                    malformed,
                    duplicates,
                    repaired,
                    if writable { "ENABLED" } else { "disabled" }
                );
                Some(store)
            }
            Err(e) => {
                dev_err!(
                    dev,
                    "shared xART mapping '{}' is unavailable or invalid: {:?}\n",
                    xart_store::STORE_PATH,
                    e
                );
                return Err(e);
            }
        };

        let mut host_store = match store::Store::open() {
            Ok(store) => Some(store),
            Err(e) => {
                dev_warn!(dev, "Linux host-state store unavailable: {:?}\n", e);
                None
            }
        };

        let bio_index = match host_store.as_mut().map(bio::IdentityIndex::load) {
            Some(Ok(index)) => index,
            Some(Err(_)) => bio::IdentityIndex::new(),
            None => bio::IdentityIndex::new(),
        };

        Arc::pin_init(
            try_pin_init!(SepData {
                dev: ARef::<device::Device>::from(dev),
                mbox <- new_mutex!(None),
                shmem <- new_mutex!(Some(buf)),
                endpoints <- new_mutex!(EndpointTable::new()),
                control <- new_mutex!(control::ControlState::new()),
                control_wq <- new_condvar!("SepData::control_wq"),
                sks_seq: Atomic::new(0),
                sks_wedged: Atomic::new(0),
                bringup: Atomic::new(BRINGUP_FRESH),
                keybag_designated: Atomic::new(false),
                bringup_started: Atomic::new(false),
                touchid_started: Atomic::new(false),
                touchid_failed: Atomic::new(false),
                enrol_open: Atomic::new(false),
                sensor_calibrated: Atomic::new(false),
                templates_restored: Atomic::new(false),
                enrol_material <- new_mutex!(None),
                enrol_identity_candidates <- new_mutex!(KVec::new()),
                last_capture_end_ns: Atomic::new(0),
                device_view_synced: Atomic::new(false),
                phase: Atomic::new(PHASE_ATTACH),
                ool_xarm <- new_mutex!(Some(ool_xarm)),
                ool_sbio <- new_mutex!(Some(ool_sbio)),
                ool_sks <- new_mutex!(Some(ool_sks)),
                sks_probe <- new_mutex!(SksProbe::new()),
                sks_wq <- new_condvar!("SepData::sks_wq"),
                ool_scrd <- new_mutex!(Some(ool_scrd)),
                scrd_probe <- new_mutex!(ScrdProbe::new()),
                scrd_wq <- new_condvar!("SepData::scrd_wq"),
                dma_ring <- new_mutex!(Some(dma_ring)),
                sbio_rx <- new_mutex!(transfer::Reassembly::new()),
                sbio_wq <- new_condvar!("SepData::sbio_wq"),
                sbio_ready: Atomic::new(false),
                sensor_present: Atomic::new(false),
                bio_session <- new_mutex!(bio::Session::new()),
                bio_index <- new_mutex!(bio_index),
                bio_dev <- new_mutex!(None),
                store <- new_mutex!(store),
                host_store <- new_mutex!(host_store),
                xarm <- new_mutex!(XarmState::new()),
                rng <- new_mutex!(None),
                machine_refkey <- new_mutex!(None),
                fv_volumes <- new_mutex!(KVec::new()),
                rng_shutdown: Atomic::new(false),
                rng_failures: Atomic::new(0),
                rx: rxring::RxRing::new(),
                rx_count: Atomic::new(0),
                settle_mark: Atomic::new(0),
                settle_idle_ticks: Atomic::new(0),
                registered: Atomic::new(false),
                shutting_down: Atomic::new(false),
                rx_work <- new_work!("SepData::rx_work"),
                enrol_work <- new_work!("SepData::enrol_work"),
                verify_work <- new_work!("SepData::verify_work"),
                settle_work <- new_delayed_work!("SepData::settle_work"),
            }),
            GFP_KERNEL,
        )
    }

    fn attach(&self, sep_node: &dt::DtNode) -> Result<()> {
        let (iova, size) = {
            let guard = self.shmem.lock();
            let buf = guard.as_ref().ok_or(EINVAL)?;
            (buf.dma_handle(), buf.len())
        };

        let msg = proto::shmem_registration(iova, size)?;


        if dt::registration_already_sent(sep_node) {
            return Err(EBUSY);
        }

        if let Err(e) = dt::mark_registration_sent(sep_node, iova) {
            dev_err!(
                self.dev,
                "could not record the registration marker ({:?}); refusing to send, since without it an rmmod/insmod cycle could send it twice and park the SEP\n",
                e
            );
            return Err(e);
        }
        self.registered.store(true, Relaxed);

        self.mbox.lock().as_ref().ok_or(EINVAL)?.send(msg, false)?;

        // no ack on 0xFE; success is the discovery burst on 0xFD
        Ok(())
    }

    fn send(&self, msg: Message) -> Result<()> {
        self.mbox.lock().as_ref().ok_or(ENODEV)?.send(msg, false)
    }


    fn control_request(&self, op: &proto::ControlOp) -> Result<Option<proto::ControlReply>> {
        let (idx, tag) = self.control.lock().alloc()?;
        let msg = proto::encode_control(op, tag);

        if let Err(e) = self.send(msg) {
            self.control.lock().abandon(idx);
            return Err(e);
        }

        let mut remaining = time::msecs_to_jiffies(op.timeout_ms());
        let mut guard = self.control.lock();
        loop {
            if let Some(reply) = guard.take_reply(idx) {
                guard.release(idx);
                drop(guard);
                if reply.tag != tag {
                    return Err(EIO);
                }
                return Ok(Some(reply));
            }

            if remaining == 0 {
                let tag = guard.abandon(idx);
                let retired = guard.retired_count();
                drop(guard);
                if op.expects_reply() {
                    dev_warn!(
                        self.dev,
                        "control <- {}: no reply within {} ms; tag 0x{:02x} retired ({} retired in total)\n",
                        op.name(),
                        op.timeout_ms(),
                        tag,
                        retired
                    );
                }
                return Ok(None);
            }

            match self
                .control_wq
                .wait_interruptible_timeout(&mut guard, remaining)
            {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Timeout => remaining = 0,
            }
        }
    }

    fn get_entropy_word(&self) -> Result<u32> {
        let op = proto::op_get_entropy();
        if !op.uses_reserved_tag() {
            return Err(EINVAL);
        }

        self.control.lock().entropy_begin()?;

        let msg = proto::encode_control(&op, proto::TAG_ENTROPY);
        if let Err(e) = self.send(msg) {
            self.control.lock().entropy_end();
            return Err(e);
        }

        let mut remaining = time::msecs_to_jiffies(op.timeout_ms());
        let mut guard = self.control.lock();
        let result = loop {
            if let Some(value) = guard.entropy_take() {
                break Ok(value);
            }
            if self.rng_shutdown.load(Relaxed) {
                break Err(ECANCELED);
            }
            if remaining == 0 {
                break Err(ETIMEDOUT);
            }
            match self
                .control_wq
                .wait_interruptible_timeout(&mut guard, remaining)
            {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Timeout => remaining = 0,
            }
        };
        guard.entropy_end();
        result
    }

    fn control_survey(&self) {

        for param in [0x00u8, 0x01, 0xff] {
            let _ = self.control_request(&proto::op_nop(param));
        }

        let _ = self.control_request(&proto::op_security_mode());

        let mut words = [0u32; 4];
        let mut got = 0;
        for w in words.iter_mut() {
            match self.get_entropy_word() {
                Ok(v) => {
                    *w = v;
                    got += 1;
                }
                Err(_) => {
                    break;
                }
            }
        }

        if got == words.len() {
            match self.register_hwrng() {
                Ok(()) => {},
                Err(e) => dev_err!(self.dev, "hwrng: registration failed: {:?}\n", e),
            }
        } else {
            dev_err!(
                self.dev,
                "hwrng: not registering, the source did not answer during the survey\n"
            );
        }

        let _state = self.control.lock();
    }


    fn alloc_ool(
        dev: &device::Device<device::Core>,
        endpoint: u8,
        allocated: usize,
        declared: (usize, usize),
    ) -> Result<OolPair> {
        if declared.0 > allocated || declared.1 > allocated {
            return Err(EINVAL);
        }

        let inbound = dma::Coherent::<u8>::zeroed_slice(dev, allocated, GFP_KERNEL)?;
        let outbound = dma::Coherent::<u8>::zeroed_slice(dev, allocated, GFP_KERNEL)?;

        // SAFETY: nothing is registered yet, so the SEP does not know these
        // buffers exist and cannot be touching them.
        unsafe {
            inbound.as_mut().fill(OOL_POISON_INBOUND);
            outbound.as_mut().fill(OOL_POISON_OUTBOUND);
        }

        Ok(OolPair {
            endpoint,
            allocated,
            declared_in: declared.0,
            declared_out: declared.1,
            inbound,
            outbound,
            registered: false,
        })
    }

    fn control_request_required(&self, op: &proto::ControlOp) -> Result<proto::ControlReply> {
        match self.control_request(op)? {
            Some(reply) => Ok(reply),
            None => Err(ETIMEDOUT),
        }
    }

    fn register_ool(&self, slot: &Mutex<Option<OolPair>>) -> Result<()> {
        let guard = slot.lock();
        if guard.as_ref().is_some_and(|b| b.registered) {
            return Ok(());
        }

        let (endpoint, allocated, declared_in, declared_out, in_iova, out_iova) = {
            let b = guard.as_ref().ok_or(EINVAL)?;
            (
                b.endpoint,
                b.allocated,
                b.declared_in,
                b.declared_out,
                b.inbound.dma_handle(),
                b.outbound.dma_handle(),
            )
        };

        if declared_in > allocated || declared_out > allocated {
            return Err(EINVAL);
        }

        // drop the buffer lock before blocking control requests; the drain path takes it
        drop(guard);

        self.control_request_required(&proto::op_ool_inbound_size(endpoint, declared_in as u32))?;
        self.control_request_required(&proto::op_ool_inbound_addr(endpoint, in_iova))?;
        self.control_request_required(&proto::op_ool_outbound_size(endpoint, declared_out as u32))?;
        self.control_request_required(&proto::op_ool_outbound_addr(endpoint, out_iova))?;

        {
            // deref first, or the guard's inherent as_mut wins over Option::as_mut
            let mut guard = slot.lock();
            let pair: &mut Option<OolPair> = &mut guard;
            if let Some(b) = pair.as_mut() {
                b.registered = true;
            }
        }
        Ok(())
    }

    fn with_store<R>(&self, f: impl FnOnce(&mut xart_store::Store) -> R) -> Option<R> {
        let mut guard = self.store.lock();
        let store: &mut Option<xart_store::Store> = &mut guard;
        store.as_mut().map(f)
    }

    fn with_host_store<R>(&self, f: impl FnOnce(&mut store::Store) -> R) -> Option<R> {
        let mut guard = self.host_store.lock();
        let store: &mut Option<store::Store> = &mut guard;
        store.as_mut().map(f)
    }

    fn prepare_os_uuid(&self) {
        let hi = *module_parameters::os_uuid_hi.value();
        let lo = *module_parameters::os_uuid_lo.value();
        if hi != 0 || lo != 0 {
            let mut uuid = [0u8; 16];
            uuid[..8].copy_from_slice(&hi.to_be_bytes());
            uuid[8..].copy_from_slice(&lo.to_be_bytes());
            self.xarm.lock().os_uuid = Some(uuid);
            dev_info!(
                self.dev,
                "xART: using explicit OS UUID {:016x}-{:016x}\n",
                hi,
                lo
            );
            return;
        }

        if let Some(uuid) = dt::preboot_uuid() {
            self.xarm.lock().os_uuid = Some(uuid);
            dev_info!(self.dev, "xART: using /chosen/apfs-preboot-uuid\n");
        } else {
            self.xarm.lock().os_uuid = None;
            dev_warn!(self.dev, "xART: /chosen/apfs-preboot-uuid is unavailable\n");
        }
    }

    fn on_xarm(&self, msg: Message) {
        let req = crate::xarm::decode_xarm(&msg);

        if xarm::is_silent(req.opcode) {
            self.xarm.lock().serviced += 1;
            return;
        }

        let ready = self.ool_xarm.lock().as_ref().is_some_and(|b| b.registered);

        if !ready {
            if !xarm::needs_buffers(req.opcode) {
                self.xarm.lock().deferred_query = Some(req.tag);
                return;
            }

            let mut state = self.xarm.lock();
            state.refused += 1;
            drop(state);
            self.fail_xarm(req.tag);
            return;
        }

        self.service_xarm(&req);
    }

    fn service_xarm(&self, req: &crate::xarm::XarmRequest) {
        let want = req.length as usize;
        if want > OOL_SIZE_XARM {
            self.fail_xarm(req.tag);
            return;
        }

        if want > 0 && !self.ool_await_written(&self.ool_xarm, 0, want) {
            self.fail_xarm(req.tag);
            return;
        }
        let payload = match self.ool_read(&self.ool_xarm, 0, want) {
            Ok(p) => p,
            Err(_) => {
                self.fail_xarm(req.tag);
                return;
            }
        };

        let mut staging = KVec::new();
        if staging.resize(OOL_SIZE_XARM, 0u8, GFP_KERNEL).is_err() {
            self.fail_xarm(req.tag);
            return;
        }

        let os_uuid = self.xarm.lock().os_uuid;
        let serviced = self.with_store(|store| {
            xarm::service(
                req,
                &payload,
                &mut staging,
                store,
                PROTECTED_DATA_AVAILABLE,
                os_uuid,
            )
        });
        let Some(done) = serviced else {
            self.fail_xarm(req.tag);
            return;
        };

        if done.reply_bytes > 0 {
            if self.ool_write(&self.ool_xarm, 0, &staging[..done.reply_bytes]).is_err() {
                self.fail_xarm(req.tag);
                return;
            }
        }

        self.xarm.lock().serviced += 1;

        self.send_xarm_reply(&done.reply);
    }

    fn ool_read(&self, slot: &Mutex<Option<OolPair>>, off: usize, len: usize) -> Result<KVec<u8>> {
        let mut guard = slot.lock();
        let pair: &mut Option<OolPair> = &mut guard;
        let buffers = pair.as_mut().ok_or(ENODEV)?;
        if off + len > buffers.allocated {
            return Err(EINVAL);
        }

        let mut payload = KVec::new();
        if len > 0 {
            // SAFETY: the SEP fills this buffer while preparing a request and
            // then waits for the host's reply, so it is quiescent here, and the
            // driver is the only other accessor.
            let src = unsafe { &buffers.outbound.as_ref()[off..off + len] };
            payload.extend_from_slice(src, GFP_KERNEL)?;
        }

        Ok(payload)
    }

    fn ool_await_written(&self, slot: &Mutex<Option<OolPair>>, off: usize, len: usize) -> bool {
        for _ in 0..OOL_WRITE_POLL_ATTEMPTS {
            {
                let guard = slot.lock();
                let Some(buffers) = guard.as_ref() else {
                    return false;
                };
                if off + len > buffers.allocated {
                    return true;
                }
                // SAFETY: the region is within the allocation as checked above,
                // and the driver is the only host-side accessor.
                let region = unsafe { &buffers.outbound.as_ref()[off..off + len] };
                if region.iter().any(|&b| b != OOL_POISON_OUTBOUND) {
                    return true;
                }
            }
            kernel::time::delay::fsleep(kernel::time::Delta::from_millis(i64::from(OOL_WRITE_POLL_MS)));
        }
        false
    }

    fn ool_write(&self, slot: &Mutex<Option<OolPair>>, off: usize, bytes: &[u8]) -> Result<()> {
        let guard = slot.lock();
        let buffers = guard.as_ref().ok_or(ENODEV)?;
        if off + bytes.len() > buffers.allocated {
            return Err(ENOSPC);
        }
        // SAFETY: the SEP reads this buffer only after the reply message is
        // sent, which happens after this returns; the driver is the only writer.
        unsafe { buffers.inbound.as_mut()[off..off + bytes.len()].copy_from_slice(bytes) };
        Ok(())
    }

    fn fail_xarm(&self, tag: u8) {
        self.send_xarm_reply(&crate::xarm::XarmReply {
            tag,
            status: xarm::STATUS_FAILED,
            length: 0,
            args: [0; 3],
        });
    }

    fn send_xarm_reply(&self, reply: &crate::xarm::XarmReply) {
        let msg = crate::xarm::encode_xarm_reply(reply);
        let _ = self.send(msg);
    }

    fn release_deferred_query(&self) {
        let Some(tag) = self.xarm.lock().deferred_query.take() else {
            return;
        };
        let mut reply = crate::xarm::XarmReply {
            tag,
            status: xarm::STATUS_OK,
            length: 0,
            args: [0; 3],
        };
        reply.args[0] = u8::from(PROTECTED_DATA_AVAILABLE);
        self.xarm.lock().serviced += 1;
        self.send_xarm_reply(&reply);
    }


    fn on_scrd(&self, msg: Message) {
        let mut probe = self.scrd_probe.lock();
        if probe.active {
            if probe.captured.len() < SCRD_MAX_CAPTURE {
                let _ = probe.captured.push(msg, GFP_KERNEL);
            }
            drop(probe);
            self.scrd_wq.notify_all();
        }
    }

    fn scrd_arm(&self) {
        let mut probe = self.scrd_probe.lock();
        probe.captured.clear();
        probe.active = true;
    }

    fn scrd_disarm(&self) -> KVec<Message> {
        let mut probe = self.scrd_probe.lock();
        probe.active = false;
        core::mem::take(&mut probe.captured)
    }

    fn scrd_wait(&self, ms: time::Msecs, until: usize) {
        let mut remaining = time::msecs_to_jiffies(ms);
        let mut guard = self.scrd_probe.lock();
        loop {
            if guard.captured.len() >= until || remaining == 0 {
                return;
            }
            match self
                .scrd_wq
                .wait_interruptible_timeout(&mut guard, remaining)
            {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Timeout => remaining = 0,
            }
        }
    }

    fn scrd_zero_buffers(&self) -> Result<()> {
        let mut guard = self.ool_scrd.lock();
        let pair: &mut Option<OolPair> = &mut guard;
        let buffers = pair.as_mut().ok_or(ENODEV)?;
        // SAFETY: nothing is outstanding on this endpoint here; the enclave
        // touches these only between a request going out and its reply, and the
        // driver is the only other accessor.
        unsafe {
            buffers.inbound.as_mut()[..buffers.allocated].fill(0);
            buffers.outbound.as_mut()[..buffers.allocated].fill(0);
        }
        Ok(())
    }

    fn scrd_transact(
        &self,
        _name: &'static CStr,
        request: u8,
        payload: &[u8],
        timeout_ms: time::Msecs,
    ) -> Option<(crate::scrd::ScrdReply, KVec<u8>)> {
        if !self.ool_registered(&self.ool_scrd) {
            return None;
        }
        if self.scrd_zero_buffers().is_err() {
            return None;
        }
        if !payload.is_empty() {
            if self.ool_write(&self.ool_scrd, 0, payload).is_err() {
                return None;
            }
        }

        let msg = crate::scrd::encode_scrd(request, payload.len());
        self.scrd_arm();
        if self.send(msg).is_err() {
            let _ = self.scrd_disarm();
            return None;
        }

        let mut examined = 0usize;
        let correlating = loop {
            self.scrd_wait(timeout_ms, examined + 1);
            let next = {
                let probe = self.scrd_probe.lock();
                let m = probe.captured.get(examined).copied();
                if m.is_some() {
                    examined += 1;
                }
                m
            };
            let Some(candidate) = next else { break None };
            let decoded = crate::scrd::decode_scrd_reply(&candidate);
            if decoded.request == request {
                break Some(candidate);
            }
            if examined >= SCRD_MAX_CAPTURE {
                break None;
            }
        };

        let leftover = self.scrd_disarm();
        let correlating = match correlating {
            Some(m) => Some(m),
            None => leftover
                .iter()
                .skip(examined)
                .copied()
                .find(|m| crate::scrd::decode_scrd_reply(m).request == request),
        };

        let raw = correlating?;

        let reply = crate::scrd::decode_scrd_reply(&raw);
        let mut response = KVec::new();
        if reply.response_size > 0 {
            if let Ok(bytes) = self.ool_read(&self.ool_scrd, 0, reply.response_size as usize) {
                response = bytes;
            }
        }
        Some((reply, response))
    }

    fn scrd_ready(&self) -> bool {
        if self.ool_registered(&self.ool_scrd) {
            return true;
        }
        if !self.endpoint_present(proto::EP_SCRD) {
            return false;
        }
        if self.register_ool(&self.ool_scrd).is_err() {
            return false;
        }
        true
    }

    fn scrd_command(&self, cmd: &crate::scrd::ScrdCommand) -> Option<(i32, KVec<u8>)> {
        let (reply, out) =
            self.scrd_transact(cmd.name(), cmd.request(), cmd.payload(), SCRD_TIMEOUT_MS)?;
        Some((reply.status, out))
    }

    fn establish_passcode_validated_context(
        &self,
        user: crate::sbio::UserId,
    ) -> Option<[u8; crate::scrd::SCRD_ACM_HANDLE_LEN]> {
        if !self.scrd_ready() {
            return None;
        }

        let (special, secret) = {
            let guard = self.enrol_material.lock();
            let material = guard.as_ref()?;
            let mut copy = KVec::new();
            copy.extend_from_slice(&material.secret, GFP_KERNEL).ok()?;
            (material.special, Secret(copy))
        };

        let (status, _) = self.scrd_command(&crate::scrd::scrd_initialize())?;
        if status != 0 {
            return None;
        }

        let (status, out) = self.scrd_command(&crate::scrd::scrd_context_create_tracked(user.value()))?;
        if status != 0 || out.len() < crate::scrd::SCRD_ACM_HANDLE_LEN {
            return None;
        }
        let mut acm_handle = [0u8; crate::scrd::SCRD_ACM_HANDLE_LEN];
        acm_handle.copy_from_slice(&out[..crate::scrd::SCRD_ACM_HANDLE_LEN]);

        let (status, _) = self.scrd_command(&crate::scrd::scrd_context_externalize(&acm_handle))?;
        if status != 0 {
            return None;
        }

        let out = self.sks_send(self.sks_req_verify_secret(special, &secret, &acm_handle))?;
        if out.reply.status != 0 {
            return None;
        }

        let (status, out) = self.scrd_command(&crate::scrd::scrd_verify_touchid_enrollment(&acm_handle))?;
        let satisfied = out.len() >= 4
            && u32::from_le_bytes([out[0], out[1], out[2], out[3]]) != 0;
        if status != 0 || !satisfied {
            return None;
        }

        Some(acm_handle)
    }

    fn establish_scrd_match_context(&self, user: crate::sbio::UserId) {
        if !self.scrd_ready() {
            return;
        }
        let Some(du) = crate::sks::DesignateUser::new(SBIO_PROBE_USER_ID) else {
            return;
        };
        let special = du.special_handle();
        let stored = match keybag::read(keybag::Slot::Identity) {
            Ok(keybag::State::Present(s)) => s,
            _ => {
                return;
            }
        };

        let _ = (|| -> Option<[u8; crate::scrd::SCRD_ACM_HANDLE_LEN]> {
            let (status, _) = self.scrd_command(&crate::scrd::scrd_initialize())?;
            if status != 0 {
                return None;
            }
            let (status, out) =
                self.scrd_command(&crate::scrd::scrd_context_create_tracked(user.value()))?;
            if status != 0 || out.len() < crate::scrd::SCRD_ACM_HANDLE_LEN {
                return None;
            }
            let mut acm_handle = [0u8; crate::scrd::SCRD_ACM_HANDLE_LEN];
            acm_handle.copy_from_slice(&out[..crate::scrd::SCRD_ACM_HANDLE_LEN]);
            let (status, _) = self.scrd_command(&crate::scrd::scrd_context_externalize(&acm_handle))?;
            if status != 0 {
                return None;
            }
            let out = self.sks_send(self.sks_req_verify_secret(special, stored.secret(), &acm_handle))?;
            if out.reply.status != 0 {
                return None;
            }
            self.scrd_command(&crate::scrd::scrd_verify_touchid_enrollment(&acm_handle))?;
            Some(acm_handle)
        })();
    }


    fn hwrng_fill(&self, buf: *mut u8, max: usize, wait: bool) -> c_int {
        if !wait {
            return 0;
        }

        let words = core::cmp::min(max / 4, RNG_MAX_WORDS_PER_READ);
        let mut written: usize = 0;
        let mut failure: Option<Error> = None;

        for i in 0..words {
            if self.rng_shutdown.load(Relaxed) {
                failure = Some(ENODEV);
                break;
            }
            match self.get_entropy_word() {
                Ok(value) => {
                    // SAFETY: the core guarantees `buf` is valid for `max`
                    // bytes and aligned for any type, and `i < max / 4`.
                    unsafe { buf.add(i * 4).cast::<u32>().write_unaligned(value) };
                    written += 4;
                }
                Err(e) => {
                    failure = Some(e);
                    break;
                }
            }
        }

        if written > 0 {
            return written as c_int;
        }

        // must return a negative errno, not 0: a blocking reader would spin on 0
        let e = failure.unwrap_or(EIO);
        let n = self.rng_failures.load(Relaxed).wrapping_add(1);
        self.rng_failures.store(n, Relaxed);
        if n == 1 {
            dev_err!(
                self.dev,
                "hwrng: read produced nothing ({:?}); further failures will not be logged\n",
                e
            );
        }
        e.to_errno()
    }

    fn register_hwrng(&self) -> Result<()> {
        let mut guard = self.rng.lock();
        if guard.is_some() {
            return Ok(());
        }

        let mut handle = hwrng::HwRngHandle::new()?;
        let ctx = core::ptr::from_ref(self).cast_mut().cast::<c_void>();

        // SAFETY: `ctx` is this `SepData`, which is kept alive by the `Arc` held
        // in the driver's private data. `remove()` sets `rng_shutdown`, wakes
        // any blocked draw, and unregisters, and `hwrng_unregister()` blocks
        // until the core has finished with the device, all before that `Arc`
        // can be dropped. So the pointer cannot outlive the registration.
        unsafe { handle.register(ctx, hwrng_read_trampoline) }?;

        *guard = Some(handle);
        Ok(())
    }


    fn drain(&self) -> u32 {
        let mut n: u32 = 0;
        while let Some(msg) = self.rx.pop() {
            self.dispatch(msg);
            n += 1;
        }
        if n > 0 {
            // single writer, single instance: a load/store pair suffices
            let total = self.rx_count.load(Relaxed).wrapping_add(u64::from(n));
            self.rx_count.store(total, Relaxed);
        }
        n
    }


    fn arm_settle(this: &Arc<SepData>) {
        if this.shutting_down.load(Relaxed) {
            return;
        }
        let delay = time::msecs_to_jiffies(SETTLE_MS);
        let _ = workqueue::system()
            .enqueue_delayed::<Arc<SepData>, SETTLE_WORK_ID>(this.clone(), delay);
    }

    fn settle_tick(this: &Arc<SepData>) {
        match this.phase.load(Relaxed) {
            PHASE_ATTACH => Self::tick_attach(this),
            PHASE_EXCHANGE => Self::tick_exchange(this),
            _ => {}
        }
    }

    fn tick_attach(this: &Arc<SepData>) {
        let now = this.rx_count.load(Relaxed);

        // HW: the SEP takes ~265 ms to answer a registration
        if now == 0 {
            let ticks = this.settle_idle_ticks.load(Relaxed).wrapping_add(1);
            this.settle_idle_ticks.store(ticks, Relaxed);
            if ticks.saturating_mul(u64::from(SETTLE_MS)) < u64::from(FIRST_RESPONSE_MS) {
                Self::arm_settle(this);
                return;
            }
        }

        if now != this.settle_mark.load(Relaxed) {
            this.settle_mark.store(now, Relaxed);
            Self::arm_settle(this);
            return;
        }

        if this.endpoint_count() == 0 {
            this.phase.store(PHASE_READY, Relaxed);
            return;
        }

        this.prepare_os_uuid();

        this.control_survey();

        if let Err(e) = this.register_ool(&this.ool_xarm) {
            dev_err!(
                this.dev,
                "xarm: out-of-line buffer registration failed ({:?}); the persistent-state exchange cannot run\n",
                e
            );
            this.phase.store(PHASE_READY, Relaxed);
            return;
        }

        this.phase.store(PHASE_EXCHANGE, Relaxed);
        this.settle_mark.store(this.rx_count.load(Relaxed), Relaxed);
        this.settle_idle_ticks.store(0, Relaxed);
        this.release_deferred_query();
        Self::arm_settle(this);
    }

    fn tick_exchange(this: &Arc<SepData>) {
        let now = this.rx_count.load(Relaxed);

        if now != this.settle_mark.load(Relaxed) {
            this.settle_mark.store(now, Relaxed);
            this.settle_idle_ticks.store(0, Relaxed);
            Self::arm_settle(this);
            return;
        }

        let endpoints = this.endpoint_count();
        if endpoints <= ENDPOINTS_BEFORE_EXCHANGE {
            let ticks = this.settle_idle_ticks.load(Relaxed).wrapping_add(1);
            this.settle_idle_ticks.store(ticks, Relaxed);
            if ticks.saturating_mul(u64::from(SETTLE_MS)) < u64::from(EXCHANGE_TIMEOUT_MS) {
                Self::arm_settle(this);
                return;
            }
        }

        this.phase.store(PHASE_READY, Relaxed);
        this.run_bringup();
    }

    fn endpoint_present(&self, id: u8) -> bool {
        self.endpoints.lock().eps.iter().any(|e| e.id == id)
    }

    fn endpoint_count(&self) -> usize {
        self.endpoints.lock().eps.len()
    }

    fn dispatch(&self, msg: Message) {
        let f = proto::decode(&msg);

        match f.ep {
            proto::EP_DISCOVER => self.on_discovery(msg, f),

            proto::EP_SHMEM => {},

            proto::EP_CONTROL => self.on_control(msg, f),

            xarm::EP_XARM => self.on_xarm(msg),

            proto::EP_SBIO => self.on_sbio(msg),

            proto::EP_SKS => self.on_sks(msg),

            proto::EP_SCRD => self.on_scrd(msg),

            proto::EP_BOOT => dev_warn!(
                self.dev,
                "unexpected message from boot endpoint 0xff: type 0x{:02x} param 0x{:02x} msg0 {:#018x} msg1 {:#010x}\n",
                f.ty,
                f.param,
                msg.msg0,
                msg.msg1
            ),

            _ep => {},
        }
    }

    fn on_control(&self, msg: Message, f: proto::Fields) {
        if f.ty != proto::CONTROL_REPLY_TYPE {
            return;
        }

        let reply = proto::ControlReply::from_message(&msg);
        let disposition = self.control.lock().deliver(reply);

        match disposition {
            control::Delivery::Entropy => {
                self.control_wq.notify_all();
            }
            control::Delivery::Matched => self.control_wq.notify_all(),
            control::Delivery::Unmatched => {}
        }
    }

    fn on_discovery(&self, msg: Message, f: proto::Fields) {
        let mut table = self.endpoints.lock();
        table.discovery_msgs += 1;

        let id = f.param;

        let idx = match table.slot(id) {
            Ok(i) => i,
            Err(_) => {
                return;
            }
        };

        match f.ty {
            proto::DISCOVER_TYPE_DESCRIPTOR => {
                let cc = proto::fourcc(&msg);
                let e = &mut table.eps[idx];
                e.have_descriptor = true;
                e.descriptor_msg0 = msg.msg0;
                e.descriptor_msg1 = msg.msg1;
                e.fourcc = cc;
                table.dirty = true;
            }
            proto::DISCOVER_TYPE_CONFIG => {
                let e = &mut table.eps[idx];
                e.have_config = true;
                e.config_msg0 = msg.msg0;
                e.config_msg1 = msg.msg1;
                table.dirty = true;
            }
            _ => {
                table.unknown_types += 1;
            }
        }
    }


    fn remove(&self) {
        self.shutting_down.store(true, Relaxed);
        trusted::unregister();
        self.unregister_fv_kernel();

        let dev = self.bio_dev.lock().take();
        if dev.is_some() {
            bio::release(&mut self.bio_session.lock());
            sensor::power(false);
            sensor::unregister_driver();
        }
        drop(dev);

        self.rng_shutdown.store(true, Relaxed);
        self.control_wq.notify_all();
        self.sbio_wq.notify_all();
        self.sks_wq.notify_all();
        self.scrd_wq.notify_all();
        if let Some(mut handle) = self.rng.lock().take() {
            handle.unregister();
        }

        *self.mbox.lock() = None;

        // SAFETY: all four pointers refer to pinned work fields in `self`.
        // Shutdown blocks requeueing, and the mailbox can no longer add work.
        unsafe {
            sep_cancel_work_sync(Work::raw_get(core::ptr::addr_of!(self.rx_work)).cast());
            sep_cancel_work_sync(Work::raw_get(core::ptr::addr_of!(self.enrol_work)).cast());
            sep_cancel_work_sync(Work::raw_get(core::ptr::addr_of!(self.verify_work)).cast());
            sep_cancel_delayed_work_sync(
                DelayedWork::raw_as_work(core::ptr::addr_of!(self.settle_work)).cast(),
            );
        }

        let _ = self.store.lock().take();
        let _ = self.host_store.lock().take();

        for slot in [
            &self.ool_xarm,
            &self.ool_sbio,
            &self.ool_sks,
            &self.ool_scrd,
        ] {
            if let Some(buffers) = slot.lock().take() {
                if buffers.registered {
                    core::mem::forget(buffers);
                }
            }
        }

        if let Some(ring) = self.dma_ring.lock().take() {
            if self.sbio_ready.load(Relaxed) {
                core::mem::forget(ring);
            }
        }

        let mut guard = self.shmem.lock();
        if let Some(buf) = guard.take() {
            if self.registered.load(Relaxed) {
                core::mem::forget(buf);
            } else {
                drop(buf);
            }
        }
    }
}

/// # Safety
///
/// Each is called only by `bio_shim.c`, with the `*const SepData` handed to
/// `BioChardev::register`, which stays valid for the whole registered window.
unsafe extern "C" fn bio_open_trampoline(ctx: *mut c_void) -> c_int {
    // SAFETY: per the contract above.
    let this = unsafe { &*ctx.cast::<SepData>() };
    match this.bio_open() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// # Safety
unsafe extern "C" fn bio_release_trampoline(ctx: *mut c_void) {
    // SAFETY: per the contract above.
    let this = unsafe { &*ctx.cast::<SepData>() };
    this.bio_release();
}

/// # Safety
unsafe extern "C" fn bio_ioctl_trampoline(ctx: *mut c_void, cmd: c_uint, arg: c_ulong) -> c_long {
    // SAFETY: per the contract above.
    let this = unsafe { &*ctx.cast::<SepData>() };
    let handled = match this.bio_ioctl(cmd, arg) {
        Ok(h) => h,
        Err(e) => return e.to_errno() as c_long,
    };

    if handled.start_verify {
        // SAFETY: as for `start_enrol` below: same pointer, same guarantee.
        let borrow = unsafe { kernel::sync::ArcBorrow::<SepData>::from_raw(ctx.cast::<SepData>()) };
        SepData::queue_verify(borrow.into());
    }

    if handled.start_enrol {
        // SAFETY: `ctx` is the pointer the driver's own `Arc<SepData>` was made
        // from and that `Arc` outlives every callback; see the contract on the
        // registration. `ArcBorrow` is what turns that guarantee into an owned
        // reference with the refcount incremented rather than a second owner of
        // the same count.
        let borrow = unsafe { kernel::sync::ArcBorrow::<SepData>::from_raw(ctx.cast::<SepData>()) };
        SepData::queue_enrolment(borrow.into());
    }
    handled.ret as c_long
}

/// # Safety
unsafe extern "C" fn bio_ready_trampoline(ctx: *mut c_void) -> c_int {
    // SAFETY: per the contract above.
    let this = unsafe { &*ctx.cast::<SepData>() };
    c_int::from(this.bio_ready())
}

/// # Safety
///
/// Only ever called by the hwrng core, through `hwrng_shim.c`. `ctx` is the
/// `*const SepData` handed to `HwRngHandle::register`, which stays valid for
/// the whole registered window (see the ordering note in `remove()`), and
/// `data` is valid for `max` bytes.
unsafe extern "C" fn hwrng_read_trampoline(
    ctx: *mut c_void,
    data: *mut c_void,
    max: usize,
    wait: bool,
) -> c_int {
    // SAFETY: per the contract above.
    let this = unsafe { &*ctx.cast::<SepData>() };
    this.hwrng_fill(data.cast::<u8>(), max, wait)
}

impl MailCallback for SepData {
    type Data = Arc<SepData>;

    fn recv_message(data: <Self::Data as ForeignOwnable>::Borrowed<'_>, msg: Message) {
        if data.shutting_down.load(Relaxed) {
            return;
        }
        if !data.rx.push(msg) && data.rx.dropped() == 1 {
            dev_err!(
                data.dev,
                "receive ring full; dropping messages (first was msg0 {:#018x})\n",
                msg.msg0
            );
        }

        let this: Arc<SepData> = data.into();
        let _ = workqueue::system().enqueue::<Arc<SepData>, 0>(this);
    }
}

impl WorkItem for SepData {
    type Pointer = Arc<SepData>;

    fn run(this: Arc<SepData>) {
        if this.shutting_down.load(Relaxed) {
            return;
        }
        if this.drain() > 0 {
            SepData::arm_settle(&this);
        }
    }
}

impl WorkItem<SETTLE_WORK_ID> for SepData {
    type Pointer = Arc<SepData>;

    fn run(this: Arc<SepData>) {
        if this.shutting_down.load(Relaxed) {
            return;
        }
        SepData::settle_tick(&this);
    }
}

impl WorkItem<ENROL_WORK_ID> for SepData {
    type Pointer = Arc<SepData>;

    fn run(this: Arc<SepData>) {
        if this.shutting_down.load(Relaxed) {
            return;
        }
        this.run_enrolment();
    }
}

impl WorkItem<VERIFY_WORK_ID> for SepData {
    type Pointer = Arc<SepData>;

    fn run(this: Arc<SepData>) {
        if this.shutting_down.load(Relaxed) {
            return;
        }
        this.run_verify();
    }
}


struct SepDriver(Arc<SepData>);

const OF_TABLE: kernel::device_id::IdArray<of::DeviceId, (), 1> =
    kernel::device_id::IdArray::new([(of::DeviceId::new(c"apple,sep"), ())]);

impl platform::Driver for SepDriver {
    type IdInfo = ();

    const OF_ID_TABLE: Option<of::IdTable<()>> = Some(&OF_TABLE);

    fn probe(
        pdev: &platform::Device<device::Core>,
        _info: Option<&()>,
    ) -> impl PinInit<Self, Error> {
        let dev: &device::Device<device::Core> = pdev.as_ref();
        if *module_parameters::provision_keybag.value() != 0
            && *module_parameters::xart_writes.value() == 0
        {
            dev_err!(dev, "provision_keybag=1 requires xart_writes=1\n");
            return Err(EINVAL);
        }
        let sep_node = dt::DtNode::of_device(dev).ok_or(ENODEV)?;

        if dt::registration_already_sent(&sep_node) {
            dev_err!(
                dev,
                "the shared-memory registration was already sent on this boot (marker property present). It is one-shot per AP reset and cannot be repeated or withdrawn: reboot to attach again. Not probing.\n"
            );
            return Err(EBUSY);
        }

        let data = SepData::new(pdev)?;

        *data.mbox.lock() = Some(Mailbox::new_byname(dev, c"mbox", data.clone())?);

        data.attach(&sep_node)?;

        if let Err(e) = data.register_fv_kernel() {
            dev_err!(dev, "could not register the FileVault kernel API: {:?}\n", e);
        }

        if let Err(e) = trusted::register(data.clone()) {
            dev_warn!(data.dev, "trusted-keys: registration failed ({:?})\n", e);
        }

        if data.registered.load(Relaxed) {
            SepData::arm_settle(&data);
        }

        Ok(SepDriver(data))
    }
}

impl Drop for SepDriver {
    fn drop(&mut self) {
        self.0.remove();
    }
}


#[pin_data]
struct SepModule {
    // field-init order enables the DT nodes before the driver registers; a failure here aborts module init
    _dt: (),

    #[pin]
    _driver: driver::Registration<platform::Adapter<SepDriver>>,
}

impl kernel::InPlaceModule for SepModule {
    fn init(module: &'static ThisModule) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            _dt: dt::enable_sep_and_dart()?,

            _driver <- driver::Registration::new(
                <Self as kernel::ModuleMetadata>::NAME,
                module,
            ),
        })
    }
}

module! {
    type: SepModule,
    name: "apple_sep",
    description: "Apple SEP coprocessor: warm attach, endpoint discovery and tag-correlated control endpoint",
    license: "Dual MIT/GPL",
    params: {
        xart_writes: u8 {
            default: 0,
            description: "Allow writes to the validated shared xART mapping",
        },
        provision_keybag: u8 {
            default: 0,
            description: "Create the Linux identity keybag when none exists; requires xart_writes=1",
        },
        os_uuid_hi: u64 {
            default: 0,
            description: "High 64 bits of an explicit xART OS UUID",
        },
        os_uuid_lo: u64 {
            default: 0,
            description: "Low 64 bits of an explicit xART OS UUID",
        },
    },
}
