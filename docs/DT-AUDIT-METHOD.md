# Reference Mac DT audit method

These reports audit integration source at
`43c790fed6bb57a12d7ee3df474a15da790b56e6` against saved, board-specific
IODeviceTree captures dated 2026-11-05. They are an offline source audit.
They do not establish that every device works or that every Apple firmware
property should have a Linux DT equivalent.

## Evidence handling

The original plists contain unit identity, boot information, firmware and
calibration. They stay outside public source. Reports use hardware facts:
device paths, bus addresses, register windows, interrupts and topology.
No calibration, tuning arrays, firmware bytes or account/session records
are copied into these documents or patches.

IODeviceTree is the macOS view of the saved ADT, rather than the original
unflattened ADT blob. Added IOKit properties and runtime service objects are
not assumed to be boot-time hardware descriptions. `IO*` metadata and the
`chosen`, NVRAM/options, manifest and per-unit property values are excluded.

## Comparison

1. Enumerate each board's `arm-io` devices and descendants. Retain a private
   property-name/resource ledger so unsupported devices are not silently lost.
2. Translate ADT little-endian register tuples through `arm-io/ranges` before
   comparing with compiled Linux DT addresses. Bus-child `reg` values are
   addresses or stream IDs, not AP MMIO tuples.
3. Match devices semantically: an ADT ASC can become a processor plus mailbox;
   a multi-instance DART can become several Linux IOMMUs; firmware mapper
   children become `iommus` specifiers; a display function can be represented
   by graph endpoints. Register overlap is only a candidate match.
4. Check compatibles against actual integration driver match tables. The
   presence of a family-related driver is insufficient to alias a new SoC.
5. Separate missing hardware descriptions from deliberately disabled devices,
   missing loader handoff, firmware-owned functionality, unsupported transports
   and properties without a Linux binding. Existing PRs/branches are recorded
   before implementing another copy.

For every mapped node, review `reg`, interrupts, power/reset dependencies,
DMA/DART stream wiring, clock sources, status and graph routes. ADT
`clock-gates`, `power-gates`, `iommu-parent`, function/GPIO programs and
interrupt arrays require translation; they are not copied byte-for-byte.
PMGR gates often cover both clock and power functions, and firmware may own
additional windows that the AP driver does not access.

## Property disposition

| Property class | Linux treatment |
|---|---|
| Hardware resources and topology | Translate to bindings and compare per device. A smaller AP register window can be intentional; do not equate overlap with correctness. |
| Device/service identity | Use public Linux compatibles and service-child names consumed by the driver. |
| Firmware segments, memory maps, DMA apertures, OPPs | Loader/runtime handoff where the existing binding requires it; static source placeholders are not missing per-unit values. |
| Calibration, filter/decimator coefficients, display/audio tuning | Keep outside DTS; extract from the same Mac through the supported provisioning/handoff path. |
| IOKit bookkeeping, firmware policy, debug metadata, boot manifests | No automatic Linux property equivalent. Missing names alone are not defects. |
| Sensor catalogues and thermal policy | SMC/firmware enumeration and a qualified Linux policy; ADT does not establish trip points or cooling coefficients. |

The root-level inventory is handled separately from `arm-io`:

| Root node(s) in the captures | Treatment |
|---|---|
| `memory`, `vram`, `cpus` | Memory/carveout layout is filled by the loader; CPU topology is in the SoC DTS. A saved allocation is not a static board address. |
| `chosen`, `options`, `iboot-syscfg`, `filesystems-props` | Boot state, NVRAM, manifests and identity; no per-unit contents are published. |
| `pram`, `socd-trace-ram` | Firmware crash/trace memory, not proof of an AP-owned persistent RAM region. No arbitrary ramoops reservation is added. |
| `hibernate`, `machine-timeouts`, `sshot-options`, `amfm`, `defaults` | Firmware/policy descriptions with no established one-to-one Linux binding; their full semantics remain unqualified. |
| `buttons` | SMC/input services, not an assumed bank of GPIO keys. |
| `port-usb-c-*`, `port-hdmi-1`, `port-t818-1` | Physical-port metadata maps to Type-C/display graphs and the MagSafe HPM endpoint. J413 has neither the third USB-C port nor built-in HDMI. |
| `pcie-sdreader-helper` (J316s) | Auxiliary firmware helper; the actual reader is represented on PCIe. It is not another SD host. |
| `backlight`, `sacm` | Display firmware/policy; board reports list the unmodeled functions. |
| `product/camera`, `product/audio` | Product identity/provisioning input, not another ISP or audio controller. |
| `aliases` (J413) | ADT references, translated to Linux loader aliases where required rather than copied as Apple phandle numbers. |

Firmware nub children under ASC devices, DART mapper children, USB DRD
host/device children and MCA audio-data children also do not have one-to-one
Linux node names. They are accounted for through the parent transport,
mailbox, IOMMU specifier, USB role and DAI graph. Their remaining proprietary
properties are not silently classified as implemented; complete semantic
property parity requires separate firmware/driver work.

## Frozen DTBs

Both reference Macs are recorded as using the 7.1 DTB set with source baseline
`04a63b11701363386e3691b7ffa7e64eef477fa7` inside `boot.bin`.
The queued appendix work compares compiled baseline and integration DTBs after resolving
phandles to paths. They exclude phandle allocation and symbol-table churn.
Node renames, additions, removals and changed properties remain visible.

That is an exact *source* delta. It must not be described as a byte-accurate
comparison with the currently installed DTBs without saved executing DTBs
and their loader fixups. The M2 has a recorded ANE status-only modification
within the old set; a source diff must not falsely report it as disabled in
the running machine. No boot-chain change is part of this audit.

## Validation

Use the controller buildbox slot for W=1 DT builds, `dt_binding_check`, and
board-wide `CHECK_DTBS=1` validation. Preserve diagnostics: Kbuild deliberately
does not fail the command for every schema validation error. Report focused
binding results separately from whole-board baseline warnings.

No reference hardware is used. Follow-up boots must preserve fallback and
the installed source identity and record rollback evidence. The new AOP
motion children remain disabled pending loader handoff and sample
qualification. Independently, the motion driver refuses probe when the
bootloader calibration properties are missing.
