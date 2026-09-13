// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

//! Device-tree work: bringing the SEP and its DART out of `status = "disabled"`
//! at module init, and the boot-persistent marker that stops the one-shot
//! registration from being sent twice.

use kernel::bindings;
use kernel::error::to_result;
use kernel::prelude::*;

const SEP_COMPATIBLE: &CStr = c"apple,sep";
const IOMMUS_PROP: &CStr = c"iommus";

const STATUS_PROP: &CStr = c"status";
const STATUS_OKAY: &CStr = c"okay";

// SEP DMA is offset BIT(40) above the DART input; one 64-bit value = two cells.
const DMA_OFFSET_PROP: &CStr = c"apple,dma-offset";
// [0x100, 0] = BIT(40); [0x1, 0] would be BIT(32) and miss the page tables.
const DMA_OFFSET_CELLS: [u32; 2] = [0x100, 0x0];

const DMA_RANGE_PROP: &CStr = c"apple,dma-range";
const DMA_RANGE_CELLS: [u32; 4] = [0, 0, 1, 0];

const REGISTERED_PROP: &CStr = c"apple,sep-shmem-registered-iova";
const CHOSEN_PATH: &CStr = c"/chosen";
const PREBOOT_UUID_PROP: &CStr = c"apfs-preboot-uuid";

pub(crate) struct DtNode(*mut bindings::device_node);

impl DtNode {
    fn find_compatible(compatible: &CStr) -> Option<DtNode> {
        // SAFETY: `of_find_compatible_node` accepts NULL for `from` and `type`,
        // takes a reference on the node it returns, and returns NULL when there
        // is no match.
        let np = unsafe {
            bindings::of_find_compatible_node(
                core::ptr::null_mut(),
                core::ptr::null(),
                compatible.as_char_ptr(),
            )
        };
        (!np.is_null()).then_some(DtNode(np))
    }

    pub(crate) fn of_device(dev: &kernel::device::Device) -> Option<DtNode> {
        // SAFETY: `dev` is valid for the duration of the borrow, and its
        // `of_node` is either NULL or a live node the device holds a reference
        // to.
        let np = unsafe { (*dev.as_raw()).of_node };
        if np.is_null() {
            return None;
        }
        // SAFETY: `np` is a live node; take our own reference for `DtNode`.
        unsafe { bindings::of_node_get(np) };
        Some(DtNode(np))
    }

    fn parse_phandle(&self, name: &CStr, index: i32) -> Option<DtNode> {
        // SAFETY: `self.0` is a valid node per the type invariant;
        // `of_parse_phandle` takes a reference on what it returns and returns
        // NULL when the property or the index is absent.
        let np = unsafe { bindings::of_parse_phandle(self.0, name.as_char_ptr(), index) };
        (!np.is_null()).then_some(DtNode(np))
    }

    fn as_ptr(&self) -> *mut bindings::device_node {
        self.0
    }

    fn is_available(&self) -> bool {
        // SAFETY: `self.0` is a valid node per the type invariant.
        unsafe { bindings::of_device_is_available(self.0) }
    }

    pub(crate) fn has_property(&self, name: &CStr) -> bool {
        // SAFETY: `self.0` is a valid node per the type invariant; a NULL
        // length pointer is accepted.
        let p = unsafe {
            bindings::of_find_property(self.0, name.as_char_ptr(), core::ptr::null_mut())
        };
        !p.is_null()
    }

}

impl Drop for DtNode {
    fn drop(&mut self) {
        // SAFETY: we own one reference, taken by the function that produced
        // `self.0`.
        unsafe { bindings::of_node_put(self.0) };
    }
}

fn with_changeset<F>(f: F) -> Result<()>
where
    F: FnOnce(*mut bindings::of_changeset) -> Result<bool>,
{
    let mut cs = core::mem::MaybeUninit::<bindings::of_changeset>::uninit();
    let csp = cs.as_mut_ptr();

    // SAFETY: `csp` points at valid uninitialised storage of the right type and
    // `of_changeset_init` initialises it in place.
    unsafe { bindings::of_changeset_init(csp) };

    let res = match f(csp) {
        // SAFETY: `csp` is an initialised changeset.
        Ok(true) => to_result(unsafe { bindings::of_changeset_apply(csp) }),
        Ok(false) => Ok(()),
        Err(e) => Err(e),
    };

    // SAFETY: `csp` is an initialised changeset.
    unsafe { bindings::of_changeset_destroy(csp) };

    res
}

fn queue_u32_array(
    cs: *mut bindings::of_changeset,
    node: &DtNode,
    name: &CStr,
    cells: &[u32],
) -> Result<()> {
    // SAFETY: `cs` is an initialised changeset, `node` is a valid node, `name`
    // is NUL-terminated, and `cells` is valid for `cells.len()` reads. The
    // callee copies both the name and the value.
    to_result(unsafe {
        bindings::of_changeset_add_prop_u32_array(
            cs,
            node.as_ptr(),
            name.as_char_ptr(),
            cells.as_ptr(),
            cells.len(),
        )
    })
}

fn queue_status_okay(cs: *mut bindings::of_changeset, node: &DtNode) -> Result<()> {
    // SAFETY: as above; `of_changeset_update_prop_string` duplicates the string.
    to_result(unsafe {
        bindings::of_changeset_update_prop_string(
            cs,
            node.as_ptr(),
            STATUS_PROP.as_char_ptr(),
            STATUS_OKAY.as_char_ptr(),
        )
    })
}

pub(crate) fn sep_node() -> Option<DtNode> {
    DtNode::find_compatible(SEP_COMPATIBLE)
}

pub(crate) fn preboot_uuid() -> Option<[u8; 16]> {
    // SAFETY: the path is NUL-terminated; a non-NULL result owns one node
    // reference, which `DtNode` releases.
    let raw = unsafe {
        bindings::of_find_node_opts_by_path(CHOSEN_PATH.as_char_ptr(), core::ptr::null_mut())
    };
    let chosen = (!raw.is_null()).then_some(DtNode(raw))?;
    let mut text = core::ptr::null();
    // SAFETY: `chosen` is live, the property name is NUL-terminated, and
    // `text` is a valid output pointer.
    if unsafe {
        bindings::of_property_read_string(
            chosen.as_ptr(),
            PREBOOT_UUID_PROP.as_char_ptr(),
            &mut text,
        )
    } != 0
    {
        return None;
    }

    let mut uuid = bindings::uuid_t { b: [0; 16] };
    // SAFETY: the property is NUL-terminated and `uuid` is a valid output.
    if unsafe { bindings::uuid_parse(text, &mut uuid) } != 0 || uuid.b.iter().all(|&b| b == 0) {
        return None;
    }
    Some(uuid.b)
}

pub(crate) fn enable_sep_and_dart() -> Result<()> {
    let sep = sep_node().ok_or_else(|| {
        pr_err!(
            "apple_sep: no device-tree node with compatible '{}'\n",
            SEP_COMPATIBLE
        );
        ENODEV
    })?;

    let dart = sep.parse_phandle(IOMMUS_PROP, 0).ok_or_else(|| {
        pr_err!(
            "apple_sep: SEP node has no '{}' phandle; cannot find its DART\n",
            IOMMUS_PROP
        );
        ENODEV
    })?;

    let dart_available = dart.is_available();
    let need_offset = !dart.has_property(DMA_OFFSET_PROP);
    let need_range = !dart.has_property(DMA_RANGE_PROP);

    pr_info!(
        "apple_sep: SEP DART state: enabled={}, apple,dma-offset present={}, apple,dma-range present={}\n",
        dart_available,
        !need_offset,
        !need_range
    );

    if dart_available && (need_offset || need_range) {
        pr_err!(
            "apple_sep: SEP DART already enabled but missing apple,dma-offset/apple,dma-range; the BIT(40) offset applies only at DART probe. Reboot and load this module first.\n"
        );
        return Err(EBUSY);
    }

    if need_offset || need_range || !dart_available {
        with_changeset(|cs| {
            let mut queued = false;
            // Properties before status: the DART probes inside of_changeset_apply().
            if need_offset {
                queue_u32_array(cs, &dart, DMA_OFFSET_PROP, &DMA_OFFSET_CELLS)?;
                queued = true;
            }
            if need_range {
                queue_u32_array(cs, &dart, DMA_RANGE_PROP, &DMA_RANGE_CELLS)?;
                queued = true;
            }
            if !dart_available {
                queue_status_okay(cs, &dart)?;
                queued = true;
            }
            Ok(queued)
        })?;
        pr_info!(
            "apple_sep: SEP DART changeset applied (offset BIT(40), range 0..4GiB, status okay)\n"
        );
    } else {
        pr_info!("apple_sep: SEP DART already enabled and configured\n");
    }

    if sep.is_available() {
        pr_info!("apple_sep: SEP node already enabled\n");
    } else {
        with_changeset(|cs| {
            queue_status_okay(cs, &sep)?;
            Ok(true)
        })?;
        pr_info!("apple_sep: SEP node enabled; platform device should now exist\n");
    }

    Ok(())
}

pub(crate) fn registration_already_sent(sep: &DtNode) -> bool {
    sep.has_property(REGISTERED_PROP)
}

pub(crate) fn mark_registration_sent(sep: &DtNode, iova: u64) -> Result<()> {
    let cells = [(iova >> 32) as u32, iova as u32];
    with_changeset(|cs| {
        queue_u32_array(cs, sep, REGISTERED_PROP, &cells)?;
        Ok(true)
    })
}

impl DtNode {
    fn reg_base(&self) -> Option<u64> {
        // SAFETY: `struct resource` is plain integers and pointers, so an
        // all-zero value is valid. Zeroed rather than `default()` because
        // bindgen's generated structs do not derive `Default`.
        let mut res: bindings::resource = unsafe { core::mem::zeroed() };
        // SAFETY: `self.0` is a live node and `res` is a valid out-parameter.
        let rc = unsafe { bindings::of_address_to_resource(self.0, 0, &mut res) };
        if rc != 0 {
            None
        } else {
            Some(res.start)
        }
    }

    fn child_with_reg(&self, value: u32) -> Option<DtNode> {
        let mut child: *mut bindings::device_node = core::ptr::null_mut();
        loop {
            // SAFETY: `of_get_next_child` takes a live parent and the previous
            // child, which it drops for us; NULL starts the iteration.
            child = unsafe { bindings::of_get_next_child(self.0, child) };
            if child.is_null() {
                return None;
            }
            let mut got: u32 = 0;
            // SAFETY: `child` is live for this iteration and `got` is a valid
            // out-parameter.
            let rc = unsafe {
                bindings::of_property_read_variable_u32_array(
                    child,
                    c"reg".as_char_ptr(),
                    &mut got,
                    1,
                    1,
                )
            };
            if rc >= 0 && got == value {
                return Some(DtNode(child));
            }
        }
    }
}

fn node_at_address(base: u64) -> Option<DtNode> {
    let mut np: *mut bindings::device_node = core::ptr::null_mut();
    loop {
        // SAFETY: `of_find_node_with_property` accepts NULL to start and drops
        // the reference to the previous node for us.
        np = unsafe { bindings::of_find_node_with_property(np, c"reg".as_char_ptr()) };
        if np.is_null() {
            return None;
        }
        let node = DtNode(np);
        if node.reg_base() == Some(base) {
            return Some(node);
        }
    }
}

pub(crate) const SENSOR_COMPATIBLE: &CStr = c"apple,mesa-fingerprint";

const SENSOR_NODE_NAME: &CStr = c"mesa@0";

fn queue_sensor_node(cs_handle: *mut bindings::of_changeset, parent: &DtNode) -> Result<()> {
    // SAFETY: `cs_handle` is a live changeset and `parent` a live node; the
    // returned node belongs to the changeset, which owns it until applied.
    let node = unsafe {
        bindings::of_changeset_create_node(cs_handle, parent.0, SENSOR_NODE_NAME.as_char_ptr())
    };
    if node.is_null() {
        pr_err!("apple_sep: could not create the sensor node\n");
        return Err(ENOMEM);
    }

    // SAFETY: all four calls take the live changeset, the node just created and
    // a NUL-terminated name; the u32 form takes a slice it copies.
    let rc = unsafe {
        let mut rc = bindings::of_changeset_add_prop_string(
            cs_handle,
            node,
            c"compatible".as_char_ptr(),
            SENSOR_COMPATIBLE.as_char_ptr(),
        );
        // reg on an SPI child is the chip select.
        if rc == 0 {
            rc = add_u32(cs_handle, node, c"reg", 0);
        }
        if rc == 0 {
            rc = add_u32(cs_handle, node, c"spi-max-frequency", 8_000_000);
        }
        // 20 ns setup/hold the controller does not apply itself.
        if rc == 0 {
            rc = add_u32(cs_handle, node, c"spi-cs-setup-delay-ns", 20);
        }
        if rc == 0 {
            rc = add_u32(cs_handle, node, c"spi-cs-hold-delay-ns", 20);
        }
        // SPI mode 2 (CPOL=1, CPHA=0); absent spi-cpha is how CPHA=0 is spelled.
        if rc == 0 {
            rc = bindings::of_changeset_add_prop_bool(cs_handle, node, c"spi-cpol".as_char_ptr());
        }
        rc
    };
    if rc != 0 {
        pr_err!("apple_sep: could not describe the sensor node: {}\n", rc);
        return Err(Error::from_errno(rc));
    }
    Ok(())
}

/// # Safety
unsafe fn add_u32(
    cs_handle: *mut bindings::of_changeset,
    node: *mut bindings::device_node,
    name: &CStr,
    value: u32,
) -> i32 {
    // of_changeset_add_prop_u32 is a static inline missing from the bindings; wrap the array form.
    // SAFETY: per this function's contract; `value` is read as one element.
    unsafe {
        bindings::of_changeset_add_prop_u32_array(cs_handle, node, name.as_char_ptr(), &value, 1)
    }
}

pub(crate) fn enable_spi_sensor(base: u64, cs: u32) -> Result<()> {
    let controller = node_at_address(base).ok_or_else(|| {
        pr_err!(
            "apple_sep: no device-tree node with reg base 0x{:x}; the sensor's SPI bus is not in this tree\n",
            base
        );
        ENODEV
    })?;

    let controller_ok = controller.is_available();
    let existing = controller.child_with_reg(cs);
    pr_info!(
        "apple_sep: sensor SPI bus at 0x{:x}: controller enabled={}, chip-select {} child present={}\n",
        base,
        controller_ok,
        cs,
        existing.is_some()
    );

    if controller_ok && existing.is_some() {
        return Ok(());
    }

    if existing.is_some() && !controller_ok {
        // Enable only the bus; a second child would be two devices at one chip select.
        pr_info!("apple_sep: sensor node already present; enabling only the bus\n");
        return with_changeset(|cs_handle| {
            queue_status_okay(cs_handle, &controller)?;
            Ok(true)
        });
    }

    pr_info!(
        "apple_sep: creating the sensor node (absent from Linux's tree; the firmware exposes it at /arm-io/spi2/mesa)\n"
    );

    with_changeset(|cs_handle| {
        // Controller before child: the SPI core's notifier needs the bus to exist first.
        if !controller_ok {
            queue_status_okay(cs_handle, &controller)?;
        }
        queue_sensor_node(cs_handle, &controller)?;
        Ok(true)
    })
}
