.. SPDX-License-Identifier: GPL-2.0-only

Apple ANE firmware preflight
===========================

The shared ANE firmware module recognizes the H16 (``apple,t6041-ane``,
physical T6040/M4 Pro) and H17 (``apple,t8140-ane``) generations. Recognition
is not an inference support claim: both remain unavailable for execution.
They never enter the H13 task-manager or H14 disk-firmware boot paths.

H17 ownership
-------------

Linux has no ANE exclave ownership handoff. The module refuses registration
with ``-EOPNOTSUPP`` and one informational message when an H17 node is
present, including a disabled node. This check occurs before platform core
DMA configuration and automatic power-domain attachment. A probe-time
check alone would be too late. Removing ``apple,exclave-assigned`` does not
establish ownership and does not bypass refusal.

H16 preload handoff
------------------

The bootloader must reserve the personalized TEXT and DATA preload segments
as separate, non-reusable ``no-map`` reserved-memory nodes, attach them in
that order through ``memory-region`` and name them ``firmware-text`` and
``firmware-data``. It must also supply ``apple,firmware-iovas`` as two 64-bit
values: ``0x10000000000`` and ``0x100000c0000``. Their sizes must be
``0xc0000`` and ``0x2ac000``. Physical placement is boot-dependent; board DT
sources must not contain captured physical addresses. The reservations must
remain intact until reboot, including after driver failure.

H16 preflight checks resource geometry, firmware identity and these ranges
without MMIO or power access. No disk firmware is requested. Missing
reservations fail with ``-ENODATA``. Malformed reservations fail with
``-EINVAL``. Even a valid handoff currently fails with ``-EOPNOTSUPP``:
the 27.x CONFIG_GET capability reply and runtime-patch allow-list are not
defined sufficiently to implement the required validation gate. No accel
node or program-loading interface is published.

The H16 DT nodes stay disabled. The five real PMGR providers already encode
the system/CPU/MPM/task/base dependency graph. The PMGR binding does not
represent the virtual no-state gates, so no fictitious register-backed
domains are created for them. SID 0 and SID 11 on all three DARTs use the
shared DMA domain; no untested stream remapping is added.

Remaining implementation gates
------------------------------

A later change needs a sanitized, versioned 27.x CONFIG_GET reply contract
covering address width, configuration size, resource limits and supported
targets. It also needs the live channel-table schema (including slot-size
semantics), a decoded allow-list of iBoot runtime patches before programming
an unlatched RVBAR, and a complete HWX target/section/relocation validation
contract. The current section-only ABI does not carry the original HWX
container identity. A caller assertion of subtype is insufficient.

Only after these contracts are implemented and validated may H16 expose
PROG_LOAD, PROC_CREATE and EXEC. Completion must be the matching finished
event, and any uncertain DMA state must remain quarantined until reboot
unless a reset has been proved. H17 additionally requires an ownership
backend before any direct-engine access. There are no module parameters to
bypass these gates.
