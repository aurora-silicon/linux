.. SPDX-License-Identifier: GPL-2.0-only

J775c loader compatibility and DT validation
==========================================

The J775c (Mac Studio M5 Max) board has one final description in
``arch/arm64/boot/dts/apple/t6050-j775c.dts``. The staging fragments and
separate ANE trial wrapper are not part of the board source. Consolidating
the resolved description preserves the normal board's compiled DTB byte
for byte, including its enabled and disabled device policy.

The description retains the GPU layout required by the installed Aura
b6e6faf4 loader: the GPU path below ``/soc@2300000000``, the
``apple,agx-t6050`` fallback compatible, and the ASC/SGX resource pair.
Migrating to the modern GPU layout requires a separately qualified loader
update. Do not remove this contract merely to satisfy schema validation.

The MTP DART remains disabled after its activation failed qualification.
The ANE DARTs retain their ANE_SYS suppliers. Neither the consolidation nor
the removal of the trial wrapper enables additional hardware.

Historical hardware evidence
----------------------------

At PR #1619 head 32149e08e8b191a500b40435eb0820cc78ec22fe, the former
``t6050-j775c-ane.dtb`` trial booted with SHA256
``dc88aa139f329bb568e48ba7ba80417e7d2c01ec6be49514a95ca24285dfd8d9``.
The combined kernel revision and test limits are recorded at:

https://github.com/aurora-silicon/linux/pull/1619#issuecomment-6816611851

That result applies only to the stated trial composition. It is not a new
qualification of this rebased head, the normal board DTB, GPU rendering,
audio playback, SD media, suspend, hotplug or other boards.

Rebase and remaining gates
-------------------------

The rebased stack starts at integration commit
``677be395b9d2e15d98696107ebf88191f08efbf9``. It includes Satya Benson's
shared disabled ACIO routers from PR #1579, the adopted-link Clock PM
policy from PR #1618, and the final board state from PR #1619. The original
PRs and their branches remain unchanged.

The rebase retains K114c in the existing Max router activation guards,
keeps the current PCIe radio and SD helpers, and preserves the T8142 ANE
binding constraints alongside the separate T6050 layout. J714s and J716c
explicitly retain their previously enabled routers and mailboxes when
inheriting the shared disabled inventory. The production-C Clock PM host
test covers the locked and unlocked PCI APIs, errors and adopted-host
gate; it does not qualify physical links or other M5 boards.

Full-schema validation still reports the legacy GPU compatible-list
``oneOf`` failure. ``scripts/dtc/apple-dt-check.sh`` compares normalized
warning sets for all Apple DTBs at base and head. It has no warning
allowlist or board exception mechanism. ADT coverage waivers cannot waive
schema diagnostics.

Keep this work draft until the GPU loader migration and affected-board
hardware qualification are complete. A later migration must recheck GPU
handoff and HDMI on J775c; old hardware receipts do not qualify a new
loader or kernel composition.
