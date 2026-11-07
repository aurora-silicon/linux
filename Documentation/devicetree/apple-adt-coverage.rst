.. SPDX-License-Identifier: GPL-2.0-only

Apple device coverage manifests
==============================

``arch/arm64/boot/dts/apple/adt-coverage/<soc>-<board>.txt`` inventories ADT
hardware. Each line gives an ADT device path, translated physical base (or
``/bus/path@0xaddress``), and a DT node path, ``MISSING``, or ``WAIVED: reason``.
For split apertures, a mapped entry may use a secondary register base.
``-`` means no usable register address. Duplicate sibling names have a ``~N``
suffix. Disabled DT nodes count as represented. PMGR table records appear
under ``pmgr/power-devices`` and ``pmgr/power-domains``.

The generator and ADT inputs are private. Public manifests contain only device
names, addresses, DT paths and waiver reasons. ``waivers.txt`` supplies reviewed
exceptions as ``board-glob ADT-path-glob | reason``. Patterns match single path
components; recursive patterns are prohibited. Lack of a driver is no waiver.

With Python 3 and pylibfdt installed, run::

    python3 scripts/dtc/apple-adt-coverage.py \
        --dtb-dir out/arch/arm64/boot/dts/apple --report

``--report`` lists MISSING counts per board while rejecting malformed records,
empty inventories, wrong-board DTBs and absent DT paths. Without it, MISSING
also fails. ``--board`` selects one board and may be repeated.

``dt-checks.yml`` compares base and head DTBs and manifests using
``--baseline-manifest-dir`` and ``--baseline-dtb-dir``. It fails on a per-board
MISSING increase, a newly absent DT path, or removal of inventory devices.
Existing gaps remain visible without failing unrelated changes. When manifests
are first introduced, the head inventory is also measured against the base DTBs
to establish the baseline. A newly added inventory uses the same rule. Both
DTB sets must exist; baseline read errors fail. Newly introduced inventories
must have no broken DT paths, even when the base DTB lacks those paths.

The checker verifies committed inventory consistency. It cannot discover ADT
devices omitted from that inventory or establish the correctness of a mapping.
Regeneration requires review against the private source and compiled board DTB.
