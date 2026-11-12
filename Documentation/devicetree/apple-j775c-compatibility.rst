.. SPDX-License-Identifier: GPL-2.0-only

J775c loader compatibility and DT validation
==========================================

The J775c (Mac Studio M5 Max) description retains the GPU layout required
by the installed Aura b6e6faf4 loader: the GPU path below
``/soc@2300000000``, the ``apple,agx-t6050`` fallback compatible, and the
ASC/SGX resource pair. Migrating to the modern GPU layout requires a
separately qualified loader update. Do not remove this contract merely
to satisfy schema validation.

At PR #1619 head 32149e08e8b191a500b40435eb0820cc78ec22fe, the
``t6050-j775c-ane.dtb`` booted with SHA256
``dc88aa139f329bb568e48ba7ba80417e7d2c01ec6be49514a95ca24285dfd8d9``.
The hardware receipt, including the combined kernel revision and test
limits, is recorded at:

https://github.com/aurora-silicon/linux/pull/1619#issuecomment-6816611851

This is a boot and health result, not qualification of GPU rendering,
audio playback, SD media, suspend, hotplug or other boards.

The DT gate still reports ten new warning lines, five per J775c DTB:
unmatched compatible, compatible oneOf failure, excessive reg length,
excessive reg-names length, and reg-names[0] expecting sgx. The modern
G17 binding describes one SGX bank; the retained loader contract uses
ASC and SGX. The edited bindings themselves validate, and ADT coverage
does not regress. These warnings remain visible.

``scripts/dtc/apple-dt-check.sh`` compares normalized warning sets for
all Apple DTBs at base and head. It has no warning allowlist or board
exception mechanism. ADT coverage waivers describe coverage only and
cannot waive schema diagnostics. A PR job compares its synthetic merge
against that merge's first parent, not against the PR's old base or
its merge-base.

The remaining DT gate item is the GPU loader contract migration: qualify
a loader supporting modern GPU paths, compatibles and resources, then
update the DT and recheck GPU handoff and HDMI on hardware. Until then,
preserve the compatibility DT and keep the PR draft while CI fails.
