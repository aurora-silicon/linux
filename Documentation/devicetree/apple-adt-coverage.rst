.. SPDX-License-Identifier: GPL-2.0-only

Apple device coverage manifests
==============================

``scripts/dtc/apple-adt-coverage.py`` checks the reviewed device inventories in
``arch/arm64/boot/dts/apple/adt-coverage`` against compiled board DTBs. Install
Python 3 and ``python3-libfdt`` (also distributed as ``pylibfdt``), then run::

    python3 scripts/dtc/apple-adt-coverage.py \
        --dtb-dir out/arch/arm64/boot/dts/apple

The checker requires every referenced node path to exist and every inventory
entry to be mapped or waived with a reason. Disabled nodes count as represented:
this checks hardware description coverage, not driver support or readiness.
``--board t8112-j413`` selects one board and may be repeated. With no selection,
all board manifests in the directory are checked. Missing DTBs, empty manifests,
duplicate entries, invalid records, wrong-board DTBs and nonexistent paths are errors.

``--report`` prints MISSING, mapped, waived and total counts per board. It allows
MISSING entries for gap inventories, while still failing on malformed input or
broken DT references. The CI job reports counts before running the strict check
beside ``dtbs_check``. A draft inventory with real gaps intentionally fails the
strict step until those gaps have been resolved or reviewed as exemptions.

Manifest format and review
--------------------------

Each board has a ``<soc>-<board>.txt`` file. Non-comment lines contain::

    /adt/device/path 0x12340000 -> /soc/device@12340000
    /adt/bus/child /adt/bus@0x2a -> /soc/i2c@12340000/device@2a
    /adt/unrepresented/device 0x12350000 -> MISSING
    /adt/container - -> WAIVED: bookkeeping container, not a separate device

These are illustrative records, not device assignments. The address is a
translated physical register base, or the ADT bus path and local address for a
bus child. ``-`` means the ADT node has no register address. Duplicate ADT sibling
names carry a ``~2``, ``~3``, etc. suffix so that no record is lost.

Named PMGR table records appear below ``/arm-io/pmgr/power-devices`` and
``/arm-io/pmgr/power-domains``. These collection names identify records inside
the ADT tables. ``-`` also covers records whose source representation does not
provide a usable hardware address; matching those records requires a reviewed
name or label correspondence.

Public manifests contain only device names, addresses, DT paths and reviewed
waiver reasons. Do not add source dumps, property values, tuning data, firmware
strings, serial numbers or private input filenames. The generator and its input
data are maintained separately and are not part of this source tree.

``waivers.txt`` is the hand-maintained policy used during inventory generation.
Each rule is ``board-glob ADT-path-glob | reason``. Path patterns match individual
components; recursive patterns are prohibited. Prefer exact board and path
rules, explain why the record is not a separate Linux hardware description, and
review the resulting manifest diff. Lack of a driver is not itself a waiver.

Regenerate when input inventories change or DT paths move, inspect ambiguous
matches and MISSING records, and verify a sample against both source inventory
and compiled DTB. The public checker verifies inventory consistency; it cannot
discover new hardware omitted from a manifest or prove that an address mapping
is correct without the separately maintained source inventory.

The checker regression tests use synthetic DTS fixtures only::

    python3 scripts/dtc/apple-adt-coverage-test.py
