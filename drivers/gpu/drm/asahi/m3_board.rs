// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Board facts for the M3 runtime, read from the hardware and the device tree instead of being
//! fixed to one board. The per-SoC facts are in [`crate::m3_soc`].

use kernel::{bindings, c_str, device, device::Core, io::resource::Resource, of, platform, prelude::*, uapi};

use crate::m3_resources::{
    reserved_resource,
    Region,
    Resources, //
};
use crate::m3_soc::Soc;

/// Whether the root node's compatible list contains `compatible`.
fn board_is(compatible: &[u8]) -> bool {
    let Some(root) = kernel::of::root() else {
        return false;
    };
    let Ok(board) = root.get_property::<KVec<u8>>(c_str!("compatible")) else {
        return false;
    };
    board.split(|b| *b == 0).any(|s| s == compatible)
}

/// Whether this is a board the M3 runtime is validated on (`soc.validated_boards`).
/// `asahi.m3_expose=auto` registers the render node and `asahi.m3_backend=auto` starts the
/// runtime only on these.
pub(crate) fn runtime_validated_board(soc: &Soc) -> bool {
    soc.validated_boards.iter().any(|board| board_is(board))
}

/// Whether the fused core-enable mask (SGX+0xe01500) describes a usable GPU of `soc`: at least
/// one core, and no core outside its core slots (2 x 10 on T6030). The mask varies with the SKU
/// (0x6f5fc on a 14-core T6030, 0x7fdff on an 18-core one); absent cores must never be enabled.
pub(crate) fn core_mask_valid(soc: &Soc, mask: u32) -> bool {
    mask != 0 && mask >> (soc.cores_per_cluster * soc.clusters) == 0
}

/// Per-cluster UAPI core masks for a core-enable mask of `soc`.
pub(crate) fn core_masks(soc: &Soc, mask: u32) -> [u32; uapi::DRM_ASAHI_MAX_CLUSTERS as usize] {
    let mut masks = [0; uapi::DRM_ASAHI_MAX_CLUSTERS as usize];
    for (cluster, slot) in masks.iter_mut().enumerate().take(soc.clusters as usize) {
        *slot = (mask >> (cluster as u32 * soc.cores_per_cluster)) & ((1 << soc.cores_per_cluster) - 1);
    }
    masks
}

/// The highest GPU frequency in the device tree OPP table, in kHz, or None when the GPU node
/// has no populated OPP table.
pub(crate) fn max_frequency_khz(pdev: &platform::Device<Core>) -> Option<u32> {
    let node = pdev.as_ref().of_node()?;
    let opps = node.parse_phandle(c_str!("operating-points-v2"), 0)?;
    let mut max_hz: u64 = 0;
    for opp in opps.children() {
        if let Ok(hz) = opp.get_property::<u64>(c_str!("opp-hz")) {
            max_hz = max_hz.max(hz);
        }
    }
    u32::try_from(max_hz / 1000).ok().filter(|khz| *khz != 0)
}

/// Register windows the runtime maps: name, base, minimum size. The same on T6030 and T8122.
const REG_WINDOWS: [(&CStr, u64, u64); 2] = [
    (c_str!("asc"), 0x2_9240_0000, 0x4000),
    (c_str!("sgx"), 0x2_9000_0000, 0x100_0000),
];

/// Reserved regions of the firmware handoff, in `Resources::regions` order.
const REGIONS: [&CStr; 6] = [
    c_str!("ttbs"),
    c_str!("pagetables"),
    c_str!("handoff"),
    c_str!("shared-l2"),
    c_str!("fw-text"),
    c_str!("fw-data"),
];

/// Static reserved-memory nodes that describe a handoff region when the GPU node does not list
/// it under its own name: the bootloader fills these nodes (the UAT regions of the firmware's
/// page-table handoff) whether or not the GPU node references them. The second-level table of
/// the firmware's upper page-table tree ("shared-l2") is reserved this way without being in the
/// GPU node's memory-region list.
const REGION_NODES: [(&CStr, &CStr); 4] = [
    (c_str!("ttbs"), c_str!("/reserved-memory/uat-ttbs")),
    (c_str!("pagetables"), c_str!("/reserved-memory/uat-pagetables")),
    (c_str!("handoff"), c_str!("/reserved-memory/uat-handoff")),
    (c_str!("shared-l2"), c_str!("/reserved-memory/uat-pagetables-l2")),
];

/// Properties that describe the loaded firmware's segments on the GPU node.
const SEGMENT_PROPS: [&CStr; 3] = [
    c_str!("apple,m3-handoff-version"),
    c_str!("apple,firmware-segment-vas"),
    c_str!("apple,firmware-segment-flags"),
];

/// Whether the GPU node lists the memory region `name` in memory-region-names.
fn lists_region(node: &of::Node, name: &CStr) -> bool {
    node.get_property::<KVec<u8>>(c_str!("memory-region-names"))
        .is_ok_and(|names| names.split(|b| *b == 0).any(|n| n == name.to_bytes()))
}

/// The static reserved-memory node that stands in for the handoff region `name`, if the GPU
/// node does not list the region itself and the device tree has such a node.
fn region_node_path(node: &of::Node, name: &CStr) -> Option<&'static CStr> {
    if lists_region(node, name) {
        return None;
    }
    REGION_NODES
        .iter()
        .find(|(region, _)| region.to_bytes() == name.to_bytes())
        .map(|(_, path)| *path)
}

/// A reference to a device tree node found by path, released on drop.
struct NodeRef(*mut bindings::device_node);

impl NodeRef {
    fn find(path: &CStr) -> Option<Self> {
        // SAFETY: `path` is a NUL-terminated string; the lookup takes a node reference or
        // returns NULL.
        let np = unsafe { bindings::of_find_node_opts_by_path(path.as_char_ptr(), core::ptr::null_mut()) };
        (!np.is_null()).then_some(NodeRef(np))
    }
}

impl Drop for NodeRef {
    fn drop(&mut self) {
        // SAFETY: the reference was taken by `find`.
        unsafe { bindings::of_node_put(self.0) };
    }
}

/// Look up the handoff region `name` through its static reserved-memory node (see
/// [`REGION_NODES`]). Returns None when the GPU node lists the region itself or the device tree
/// has no such node; otherwise the region, which must be an enabled reserved-memory node the
/// kernel reserved at boot. Returns whether the node is no-map as well.
pub(crate) fn static_region(node: &of::Node, name: &CStr) -> Option<Result<(Resource, bool)>> {
    let path = region_node_path(node, name)?;
    let np = NodeRef::find(path)?;
    // SAFETY: `np` holds a node reference for the duration of these read-only queries.
    let (available, rmem, nomap) = unsafe {
        (
            bindings::of_device_is_available(np.0),
            bindings::of_reserved_mem_lookup(np.0),
            bindings::of_property_read_bool(np.0, c_str!("no-map").as_char_ptr()),
        )
    };
    if !available || rmem.is_null() {
        return Some(Err(EINVAL));
    }
    // SAFETY: the reserved-memory table entry lives as long as the kernel.
    let (base, size) = unsafe { ((*rmem).base, (*rmem).size) };
    if base == 0 || size == 0 || base.checked_add(size).is_none_or(|end| end > 1 << 42) {
        return Some(Err(EINVAL));
    }
    let raw = bindings::resource {
        start: base,
        end: base + size - 1,
        flags: bindings::IORESOURCE_MEM as _,
        ..Default::default()
    };
    // SAFETY: Resource is repr(transparent) over Opaque<resource>. The descriptor is owned and
    // has no pointers into temporary data; its range is checked above.
    Some(Ok((unsafe { core::mem::transmute::<bindings::resource, Resource>(raw) }, nomap)))
}

/// Admit a GPU of `soc` described by the device tree, either statically or by a runtime overlay.
///
/// The device must be a GPU node of `soc` with the ASC and SGX windows at their addresses
/// (at least as large as the runtime maps), the GPU coprocessor mailbox, the six handoff regions
/// by name (from the reserved-memory registry or no-map overlay nodes), and the firmware segment
/// description. Every refusal is logged.
pub(crate) fn admit(pdev: &platform::Device<Core>, soc: &Soc) -> Result<Resources> {
    let dev = pdev.as_ref();
    let refuse = |what: &str, err: Error| {
        dev_info!(dev, "M3: not admitted: {}\n", what);
        err
    };
    let node = dev.of_node().ok_or(ENODEV)?;

    if !board_is(soc.board.as_bytes()) {
        dev_info!(dev, "M3: not admitted: not a {} board\n", soc.name);
        return Err(ENODEV);
    }
    let compatible: KVec<u8> = node.get_property(c_str!("compatible"))?;
    if !compatible.split(|b| *b == 0).any(|s| s == soc.gpu.as_bytes()) {
        dev_info!(dev, "M3: not admitted: GPU node is not {}\n", soc.gpu);
        return Err(ENODEV);
    }
    // The runtime runs the firmware that the bootloader loaded, in place: without the
    // bootloader's description of its segments there is nothing to admit.
    if !lists_region(&node, c_str!("fw-text"))
        || !lists_region(&node, c_str!("fw-data"))
        || SEGMENT_PROPS.iter().any(|p| node.get_property::<KVec<u8>>(p).is_err())
    {
        return Err(refuse(
            "the device tree does not describe the loaded GPU firmware segments (fw-text/fw-data regions and apple,firmware-segment-* properties); the bootloader predates this",
            ENODEV,
        ));
    }
    dev_info!(dev, "M3: board compatible accepted\n");

    for name in REGIONS {
        if lists_region(&node, name) {
            continue;
        }
        match region_node_path(&node, name) {
            Some(path) if NodeRef::find(path).is_some() => {
                dev_info!(dev, "M3: {:?} region: static node {:?}\n", name, path)
            }
            _ => {
                dev_info!(dev, "M3: not admitted: no {:?} memory region\n", name);
                return Err(EINVAL);
            }
        }
    }
    dev_info!(dev, "M3: memory region list accepted\n");

    for (name, base, min_size) in REG_WINDOWS {
        let res = pdev
            .resource_by_name(name)
            .ok_or_else(|| refuse("missing register window", EINVAL))?;
        if res.start() != base || res.size() < min_size {
            dev_info!(
                dev,
                "M3: not admitted: {:?} window {:#x}+{:#x}, need {:#x}+{:#x} or larger\n",
                name,
                res.start(),
                res.size(),
                base,
                min_size
            );
            return Err(EINVAL);
        }
    }
    dev_info!(dev, "M3: register resources accepted\n");

    let mboxes: KVec<u32> = node.get_property(c_str!("mboxes"))?;
    let mbox = (mboxes.len() == 1)
        .then(|| node.parse_phandle(c_str!("mboxes"), 0))
        .flatten()
        .ok_or_else(|| refuse("expected exactly one mailbox", EINVAL))?;
    let compat: KVec<u8> = mbox.get_property(c_str!("compatible"))?;
    let mbox_reg: KVec<u32> = mbox.get_property(c_str!("reg"))?;
    let irq_names: KVec<u8> = mbox.get_property(c_str!("interrupt-names"))?;
    let irqs: KVec<u32> = mbox.get_property(c_str!("interrupts"))?;
    let cells: u32 = mbox.get_property(c_str!("#mbox-cells"))?;
    if !soc.mailbox_compatibles.iter().any(|c| *c == compat.as_slice())
        || mbox_reg.as_slice() != [2, 0x92408000, 0, 0x4000]
        || cells != 0
        || irq_names.as_slice() != b"send-empty\0send-not-empty\0recv-empty\0recv-not-empty\0"
        || irqs.as_slice() != soc.mailbox_interrupts
    {
        return Err(refuse("unexpected GPU mailbox description", EINVAL));
    }
    dev_info!(dev, "M3: mailbox resource accepted\n");

    let mut regions = [Region { base: 0, size: 0 }; 6];
    for (i, name) in REGIONS.iter().enumerate() {
        let res = reserved_resource(&node, name).map_err(|e| {
            dev_info!(dev, "M3: not admitted: {:?} region unusable ({:?})\n", name, e);
            e
        })?;
        regions[i] = Region {
            base: res.start(),
            size: res.size(),
        };
    }
    dev_info!(dev, "M3: reserved resources {:?}\n", regions);

    let version: u32 = node
        .get_property(c_str!("apple,m3-handoff-version"))
        .map_err(|e| refuse("no firmware handoff description", e))?;
    let vas: KVec<u64> = node.get_property(c_str!("apple,firmware-segment-vas"))?;
    let flags: KVec<u32> = node.get_property(c_str!("apple,firmware-segment-flags"))?;
    Resources::validate(
        version,
        regions,
        vas.as_slice().try_into().map_err(|_| EINVAL)?,
        flags.as_slice().try_into().map_err(|_| EINVAL)?,
    )
    .map_err(|e| {
        dev_info!(dev, "M3: not admitted: handoff description rejected ({:?})\n", e);
        EINVAL
    })
}

/// Whether the GPU node links to a PMP instance (`apple,pmp`) through which the GPU power vote
/// is cast. Without one, GPU power is left to the power domain.
pub(crate) fn has_pmp_link(pdev: &platform::Device<Core>) -> bool {
    pdev.as_ref()
        .of_node()
        .is_some_and(|node| node.get_property::<u32>(c_str!("apple,pmp")).is_ok())
}

/// A GPU firmware image the runtime can identify.
#[derive(Debug)]
pub(crate) struct KnownImage {
    /// Human-readable identity, for logs.
    pub(crate) name: &'static str,
    uuid: Option<[u8; 16]>,
    stkg_sha256: Option<[u8; 32]>,
    /// The InitData magic (InitData+0) this firmware expects.
    pub(crate) initdata_magic: u64,
}

/// T6030 GPU firmware images known to the M3 runtime. An image is accepted when every recorded
/// identity (UUID, hash) matches.
pub(crate) static KNOWN_IMAGES: [KnownImage; 2] = [
    KnownImage {
        name: "J514S RTKit-2419.140.12",
        uuid: None,
        stkg_sha256: Some(crate::m3_firmware::TEXT_SHA256),
        initdata_magic: 0x0c08_e21e_8380_0490,
    },
    KnownImage {
        name: "g15s build b0 (firmware 14.8.3) RTKit-2419.140.12",
        uuid: Some([
            0xdb, 0xf3, 0x7c, 0x40, 0xea, 0xd5, 0x37, 0x60, 0x94, 0x41, 0x99, 0x50, 0x18, 0x78, 0x29,
            0x55,
        ]),
        stkg_sha256: None,
        initdata_magic: 0x0c08_e21e_8380_0490,
    },
];

const IMAGE_INFO_OFFSET: usize = 0x4200;
/// Size of the identifying part of the image-info header.
const IMAGE_INFO_SIZE: usize = 0x38;
/// First word of the header: a branch over it (`b +0x44`).
const IMAGE_INFO_BRANCH: u32 = 0x1400_0011;
/// Header magic, "uuid".
const IMAGE_INFO_MAGIC: u32 = 0x6469_7575;

/// Parsed image-info header: UUID, patchbay offset and size, TEXT size.
struct ImageInfo {
    uuid: [u8; 16],
    patchbay: core::ops::Range<usize>,
    text_size: usize,
}

fn image_info(text: &[u8]) -> Option<ImageInfo> {
    let hdr = text.get(IMAGE_INFO_OFFSET..IMAGE_INFO_OFFSET + IMAGE_INFO_SIZE)?;
    let word = |i: usize| u32::from_le_bytes([hdr[i], hdr[i + 1], hdr[i + 2], hdr[i + 3]]);
    if word(0) != IMAGE_INFO_BRANCH || word(4) != IMAGE_INFO_MAGIC || word(8) != 5 {
        return None;
    }
    let mut uuid = [0u8; 16];
    uuid.copy_from_slice(&hdr[0x14..0x24]);
    let start = word(0x2c) as usize;
    let end = start.checked_add(word(0x30) as usize)?;
    Some(ImageInfo {
        uuid,
        patchbay: start..end,
        text_size: word(0x34) as usize,
    })
}

fn sha256(data: &[u8]) -> [u8; 32] {
    let mut digest = [0u8; 32];
    // SAFETY: `data` and `digest` are valid for the duration of the synchronous call.
    unsafe { bindings::sha256(data.as_ptr(), data.len(), digest.as_mut_ptr()) };
    digest
}

/// Lower-case hex of `bytes`, for logs.
fn hex<const N: usize>(bytes: &[u8]) -> [u8; N] {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut out = [b'0'; N];
    for (i, b) in bytes.iter().take(N / 2).enumerate() {
        out[2 * i] = DIGITS[(b >> 4) as usize];
        out[2 * i + 1] = DIGITS[(b & 0xf) as usize];
    }
    out
}

pub(crate) fn identify(
    dev: &device::Device,
    text: &[u8],
    stkg_sha256: &[u8; 32],
    images: &'static [KnownImage],
) -> Option<&'static KnownImage> {
    let info = image_info(text);
    let masked_sha256 = info.as_ref().and_then(|info| {
        let mut copy = KVec::new();
        copy.extend_from_slice(text, GFP_KERNEL).ok()?;
        copy.get_mut(info.patchbay.clone())?.fill(0);
        Some(sha256(&copy))
    });
    let uuid = info.as_ref().map(|info| info.uuid);
    let uuid_hex = hex::<32>(&uuid.unwrap_or([0; 16]));
    let stkg_hex = hex::<64>(stkg_sha256);
    let masked_hex = hex::<64>(&masked_sha256.unwrap_or([0; 32]));
    fn as_str(b: &[u8]) -> &str {
        core::str::from_utf8(b).unwrap_or("?")
    }
    dev_info!(
        dev,
        "M3: loaded GPU firmware: uuid {} (header {}), text sha256 {} (STKG zeroed), {} (patchbay zeroed)\n",
        if uuid.is_some() { as_str(&uuid_hex) } else { "none" },
        if info.as_ref().is_some_and(|i| i.text_size == text.len()) { "ok" } else { "missing or unexpected" },
        as_str(&stkg_hex),
        if masked_sha256.is_some() { as_str(&masked_hex) } else { "n/a" }
    );
    let image = images.iter().find(|image| {
        (image.uuid.is_some() || image.stkg_sha256.is_some())
            && image.uuid.map_or(true, |known| uuid == Some(known))
            && image.stkg_sha256.map_or(true, |known| known == *stkg_sha256)
    });
    match image {
        Some(image) => dev_info!(dev, "M3: GPU firmware identified as {}\n", image.name),
        None => dev_err!(dev, "M3: loaded GPU firmware is not a known image\n"),
    }
    image
}

/// Whether the reserved-memory node behind the GPU memory region `name` is no-map.
///
/// A region without no-map is part of the kernel's linear map, which maps it write-back; any
/// other CPU mapping of it must use the same memory type, because mismatched aliases of the same
/// memory are not allowed. Only no-map regions may be mapped write-combined. Returns true when
/// the region cannot be looked up, which keeps the caller's own choice.
pub(crate) fn region_is_nomap(node: &of::Node, name: &CStr) -> bool {
    if let Some(found) = static_region(node, name) {
        return found.map_or(true, |(_, nomap)| nomap);
    }
    let Ok(names) = node.get_property::<KVec<u8>>(c_str!("memory-region-names")) else {
        return true;
    };
    let Some(index) = names.split(|b| *b == 0).position(|n| n == name.to_bytes()) else {
        return true;
    };
    let Some(region) = node.parse_phandle(c_str!("memory-region"), index) else {
        return true;
    };
    region.get_property::<KVec<u8>>(c_str!("no-map")).is_ok()
}

/// Whether the M3 render node is registered (`asahi.m3_expose`).
///
/// Userspace drivers pick up any registered render node. Until the M3 runtime is validated on a
/// board, a render node there would let a stock userspace driver submit work the board may not
/// run correctly. `auto` (-1) registers it only on the boards the runtime was validated on.
pub(crate) fn expose_render_node(soc: &Soc) -> bool {
    match *crate::module_parameters::m3_expose.value() {
        0 => false,
        v if v > 0 => true,
        _ => runtime_validated_board(soc),
    }
}
