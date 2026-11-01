.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Cold-acquired H17 firmware backend
=================================

This experimental backend shares the H16/H17 channel transport, LOAD and
EXECUTE wire formats and UUID completion helpers. It does not take over a
running Exclave engine. Production T8140 nodes remain disabled. The separate
``t8140-j700-ane-test.dtb`` enables only the lead's one-shot test.

The staged observer is installed before platform driver registration. This
avoids the platform bus's automatic genpd power-on and default DMA attachment
before the read-only probe. The existing H14 ABI is never exposed for H17.
Build ``CONFIG_DRM_ACCEL_ANE=y`` and ``CONFIG_APPLE_DART=y`` for acquisition;
an ANE module supports observation only because the kexec blocker requires a
built-in caller. No module parameter opens this backend.

Loader contract
---------------

The loader must publish ``/chosen/apple,ane-cold-boot`` with the exact string
``raw-no-xnu-ane-untouched-v1`` only when the reset path ran no XNU, SPTM,
ExclaveCore or other ANE owner and left the engine untouched. This is boot
evidence, not an operator opt-in. Neither the production nor test DTS supplies
it. A warm handoff, kexec, unknown loader or missing evidence must refuse.

The loader also supplies two ``no-map``, non-reusable reserved-memory nodes,
referenced by ``memory-region`` and named ``firmware-text`` and
``firmware-data``. Their sizes are 0xc0000 and 0x2ac000. Physical addresses
come from that boot's preload records, never from compiled-in lab addresses.
The DVA pair is 0x10000000000 and 0x100000c0000.

TEXT must match the measured 26.6.2 H17 SHA-256 pin in the driver. The loader's
``apple,preload-data-sha256`` is a digest of the full live DATA segment. The
driver copies both reserved segments into private coherent RAM and preserves
every iBoot-written DATA byte. The DATA digest establishes preservation of the
loader's supplied segment; it is not a cross-build immutable-DATA manifest.
A pinned manifest excluding only the individually documented mutable fields
remains required before production enablement. No firmware image is shipped.

Stages and recovery
-------------------

Use the platform device's root-writable ``firmware_stage`` attribute. Reading
it reports ``unverified``, ``probe``, ``map``, ``boot``, ``startup`` or
``quarantined``. Write these actions in order::

  echo probe > firmware_stage
  echo map > firmware_stage
  echo boot > firmware_stage
  echo startup > firmware_stage

``probe`` reads and logs DART version/protect/unprotect/lock, DAPF records,
CPU control/status, RVBAR, child power, PWGATE, CE, IRQ pending and scratch.
It writes no hardware register, ring, power state or mapping. Missing loader
evidence still permits the diagnostic log, then refuses acquisition with one
info line. Physically-on ANE_SYS (0x23ff) is the measured cold state even when
genpd reports off. CPU/subblock, PWGATE and CE must independently be cold;
DAPF and invalid TTBR payloads are not cold markers.

``map`` repeats the cold check, retains the module and power lifetime, blocks
kexec and system sleep, acquires genpd ANE_SYS, powers the children, opens
PWGATE and completes CE before scratch access. Only SID0 on three DARTs is
attached to the private paging domain. Four DT-provided DAPF ranges are
written and read back, and their pages are mapped as identity MMIO.
Preloads, command pool, IPC and heap are mapped at their documented DVAs.

``boot`` runs the scratch handshake, checks the exact request sizes, validates
all eight channel descriptors and arms host rings. ``startup`` sends only
PRINT_ENABLE, START, CONFIG_GET and RESOURCE_INFO_GET, servicing shared
allocations with an absolute deadline. Completion is polled. Neither IRQ 655
nor IRQ 668 is registered or enabled. Secure-mode commands 0x27, 0x28, 0x34
and Exclave commands 0x30, 0x31 are denied at the publication boundary. 0x2f
is shared-event initialization, not an Exclave command, and is not sent.

Any failure after acquisition, including a normal map synchronization timeout,
quarantines the device, retains mappings and power and requires a reboot.
There is no in-place reset or retry. Suspend is refused while retained.
The ANE DART profile also refuses generic suspend/resume and never resets
foreign streams. Rollback is an ordinary reboot to the saved production
entry, never module unload after acquisition.

Program integration and measured limits
--------------------------------------

``ane_fw_program_alloc`` and ``ane_fw_program_run`` are internal interfaces
for the original-HWX frontend; they are not a section-upload userspace ABI.
The original-HWX frontend from the companion load-ABI work is not yet wired
to H17. No public H17 inference ioctl is advertised by this patch.

The backend serializes LOAD/CREATE, 0x2d/EXECUTE/0x2d, TERMINATE and UNLOAD.
Every section and tensor must resolve to a backend-owned mapping. Operation
records with a kernel table must bind the kernel BAR to LOAD slot 1's
resource index 1007. Legacy tag 2 is refused before LOAD because the measured
H17 firmware asserts on it. Custom BARs, multiple outputs and unsupported
resource classes are refused by this initial test path.

Mappings retain the DART driver's normal per-range synchronization, including
mappings made after firmware start, and propagate synchronization errors. A matching terminal
UUID and program/process/procedure IDs is followed by a DMA read barrier and
an exact comparison with the frontend's immutable expected output. The output
is poisoned with the complement of those bytes before submission. A terminal
report without expected bytes quarantines immediately; it never counts as
success and is never retried.

Several programs may run in one firmware boot. The firmware has a direct
submission path that reuses the engine's current BAR context. If that context
belongs to a different program, a job can write through the earlier program's
output binding. This happens even after that program was terminated and
unloaded, and a full TLB invalidation does not prevent it. Bit 0 of the
EXECUTE flags word at offset 0x30 keeps a request off the direct path and
queues it for the full execution loop, which installs a fresh BAR context.
The transport sets that bit in its copy of the first EXECUTE after any change
of program or process, and after any LOAD, CREATE, TERMINATE or UNLOAD, since
firmware IDs may be reused. Later jobs of the same process keep the direct
path. Measured on the J700 with 11 of 11 program switches correct, at about
60 us extra on the switching job only. H16 shares the firmware design but has
not executed under Linux, so its requests are published unchanged. Output
mappings of earlier programs stay pinned and are never reused.
The observed FREE response is DVA|1 with zero length and handle. It is
accepted only for a live exact allocation during teardown; its backing stays
pinned and cannot be recycled. Duplicate, interior and unsolicited FREEs
refuse. The canonical FREE form remains unproven; a delayed FREE beyond the
absolute teardown deadline quarantines rather than allowing another LOAD.

Provenance
----------

Register and wire facts were taken from the supplied clean H16/H17 field
specifications and the measured H17 cold-boot and harness results. No private
implementation source or firmware bytes are included. The operation-record
geometry is also documented by the MIT-licensed public ``libane/ane_m2.c``
in https://github.com/joshuaswarren/omarchy-ane. The H17 resource-index rule
overrides that older generation's kernel-tag convention.
