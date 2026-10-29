.. SPDX-License-Identifier: GPL-2.0-only

Native APIF guest preparation
=============================

Apply ``arch/arm64/configs/aurora-apif.config`` after the existing Asahi
fragment, then run ``olddefconfig``. It enables the native transport, DART,
SART, ANS, PCIe and PMP providers. Both SART components must be built in:
``APPLE_SART_APIF`` requires ``APPLE_SART=y``. These drivers use the native
Linux build workflow and need no external build VM or wrapper.

The boot producer owns physical addresses, grants, calibration and the stopped
handoff. Linux does not reconstruct them from a machine name. A generated
version-1 description can be migrated on a copy with Python libfdt::

    tools/aurora-apif/prepare_guest.py producer.dtb native-apif.dtb \
        --pcie-layout t6050-gen3-x1

The output must be a new file. Validation failure writes nothing. This tool
does not install, boot or modify hardware. Use the result only with the matching
Aura version-2 monitor at CPU IPA ``0x61ff0000``. The version-1 transport remains
in the tree for other consumers; its sixteen alias pages do not overlap v2.

DART unit IDs, stream counts, native force masks, DMA apertures and retained
SIDs are preserved. Provider phandles remain stable, legacy unit-address names
are removed, and standard aliases/symbol paths are repaired. The final-page
allocation guard formerly hidden in the PMP consumer match becomes explicit
provider metadata. PMP status is preserved unless ``--enable-pmp`` is supplied.
Unrelated device nodes and opaque data are preserved.

ANS requires four distinct 16 KiB ``no-map`` queue reservations with the
native addresses already pinned by firmware, in ``admin-sq``, ``admin-cq``,
``io-sq``, ``io-cq`` order. The producer must declare coherent memory and
``asahi,rtkit-quiesced`` after actually stopping both RTKit peers and controller
DMA. The converter refuses live or incomplete handoffs; it never allocates
replacement pages or invents ownership. The native driver independently
checks translations and requires an identity DMA aperture.

A preboot PCIe cycle additionally requires the explicitly selected T6050
Gen3 x1 layout, five named resources, stopped ``apple,preinitialized`` state
and existing ``apple,rid-to-sid`` tuples matching ``iommu-map``. MSI address
and AIC range remain producer-supplied. There is no automatic board/layout
inference or N1 radio driver activation in this migration.

CPU resource overlap is checked for root and identity CPU buses. Translation
through nonempty bus ranges and PCI BAR encodings remains the producer's
responsibility. The tool does not prove firmware state, native permissions,
thermal policy or hardware qualification. Old live-session m5 trees lacking
queue reservations cannot be converted into a safe stopped handoff merely by
setting a boolean. Update the producer first, retain its original DTB, and
qualify boot, storage durability, MSI, USB DMA, PMP and teardown separately.

The matched T6050 Abbey RTC is migrated to ``apple,abbey-pmu-rtc`` only
when its five named cells describe the counter, offset, alarm, control
and interrupt mask. This leaves the separate Baku RTC binding unchanged.

T6050 right-port USB DART must name its ATC2_USB power domain independently
of the DWC3 consumer. Its DART gate has no local power-state register and
requires the shared controller parent before native OPEN reads DART registers.
The migration rejects an enabled T6050 USB controller using native DART16
unless both nodes carry the producer-supplied shared power-domain dependency.
It preserves that dependency on the renamed APIF IOMMU node; platform PM
attachment therefore precedes IOMMU probe and native access. This does not
replace physical hotplug/DMA qualification.

The T6050 GPU keeps its separate compatibility-operation page at
``0x61f3c000`` with register name ``x1n1-uat``. The generic APIF v2 page
does not replace that GPU ABI. Migration preserves the GPU apertures,
CPU release offsets, guest-device mask and QSPI/NVRAM description.
It accepts the legacy ANS ``nvmmu`` resource only to remove that mediated
window after validating the stopped handoff. The explicitly selected
T6050 PCIe layout normalizes its historical ``config,port,phy,intr2axi,perst``
resource order to ``ecam,port,phy,perst,intr2axi`` without changing extents.

Kernel configuration check
--------------------------

T6050 boot NOR uses ``CONFIG_SPI_APPLE_QSPI``, not ``CONFIG_SPI_APPLE``.
The APIF fragment explicitly enables QSPIMC, SPI memory, MTD, SPI-NOR and
fixed-partition support. Apply it before ``olddefconfig``, then check the
resolved configuration against the enabled producer nodes::

    tools/aurora-apif/check_guest_config.py native-apif.dtb build/.config

The checker covers the known M5 guest peripherals and their required build
options. It follows disabled parent nodes, accepts built-in or modular
drivers by default, and reports unmatched compatibles as unaudited rather
than unsupported. ``--built-in`` requires every matched option to be ``y``;
otherwise the corresponding modules must be supplied by the deployment.
This checks configuration coverage, not hardware behavior or module contents.
Migration can also take ``--kernel-config build/.config``; missing known
driver options then reject the migration before writing its output.
