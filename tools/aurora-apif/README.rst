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
