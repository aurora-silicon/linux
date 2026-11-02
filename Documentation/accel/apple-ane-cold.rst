.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Cold-acquired H17 firmware backend
=================================

This backend shares the H16/H17 channel transport, LOAD and EXECUTE wire
formats and UUID completion helpers. It does not take over a running Exclave
engine. The J700 board DT enables the ANE and its three DARTs; the driver
starts the engine at boot only after a raw loader hand-off (below) and
otherwise leaves it alone with one informational line.

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
evidence, not an operator opt-in, and no DTS supplies it. A warm handoff,
kexec, unknown loader or missing evidence must omit it.

``t8140.dtsi`` carries two disabled ``/reserved-memory`` placeholders,
``ane_fw_text: ane-firmware-text@0`` and ``ane_fw_data: ane-firmware-data@0``
(``compatible = "apple,asc-mem"``, ``reg = <0 0 0 0>``, ``no-map``), which
the ANE node already references by ``memory-region`` with the names
``firmware-text`` and ``firmware-data``, as on the T6041. The loader fills
``reg`` from that boot's ADT ANE ``segment-ranges`` (TEXT 0xc0000 bytes,
DATA 0x2ac000 bytes, 16 KiB aligned, never compiled-in addresses), renames
each node to its physical address and sets ``status = "okay"``. Both stay
``no-map``, never ``reusable``, and outside System RAM. The DVA pair in
``apple,firmware-iovas`` is 0x10000000000 and 0x100000c0000; a loader that
rewrites it from the ADT must produce exactly these values.

TEXT must match the measured 26.6.2 H17 SHA-256 pin in the driver. A loader
may add ``apple,preload-data-sha256``, the SHA-256 of the full live DATA
segment it handed off; the driver then checks its private copy against it.
Without it the driver checks the copy against the live DATA it copied. The
driver copies both reserved segments into private memory and preserves every
iBoot-written DATA byte. No firmware image is shipped.

Nothing else comes from the loader: ``reg``, the DAPF ranges, the three DARTs
(with their ``apple,dma-range``) and the node status are static DT.

Start at boot
-------------

At module load (or initcall when built in), before the platform driver
registers, the driver reads the hand-off without touching the engine: the
marker, ``apple,firmware-preloaded``, both reservations enabled, ``no-map``,
not reusable and outside System RAM at the exact sizes and DVAs, and TEXT
against the pin. With all of it, a work item runs the stages below through
``program`` once the driver is registered, so ``/dev/accel/accel0`` exists on
every boot. Without it, the driver logs which part was missing and leaves the
engine alone; the platform probe refuses quietly with -ENODEV.

Stages and recovery
-------------------

The boot work item runs these stages. The platform device's
``firmware_stage`` attribute reports ``unverified``, ``probe``, ``map``,
``boot``, ``startup``, ``program`` or ``quarantined``; for diagnosis, with a
hand-off present, the same actions can be written by root in order::

  echo probe > firmware_stage
  echo map > firmware_stage
  echo boot > firmware_stage
  echo startup > firmware_stage
  echo program > firmware_stage

The engine block and its DARTs sit behind ANE_SYS, and an access to them
while it is off stalls the bus. ANE_SYS is no cold marker: it can read on
while genpd reports off, and generic DART runtime PM or a refused platform
probe turns it off again. Every register is therefore read only once its
block is known to be powered, in the order the U2 runs measured as safe.

``probe`` reads only PMGR: ANE_SYS, the ANE CPU power state (low byte off)
and the sub-block power state (0x300). With the loader's raw-boot evidence it
moves to ``probe``; it writes nothing and never touches the engine or DARTs.

``map`` repeats the PMGR check, then retains the module and power lifetime,
blocks system sleep (and kexec when built in) and acquires genpd ANE_SYS. It
reads PWGATE (request 3, acknowledge 0), turns the CPU power state on, clears
the PWGATE request and turns the sub-block on. Only then does it read and
check the engine and DARTs: CE state and error, CPU control (0) and status
(0x6a), RVBAR, IRQ pending, scratch, and unlocked, unfaulted DARTs of version
0x203 with no stream enabled. DAPF records and invalid TTBR payloads are
logged, never used as cold markers. Nothing in the engine has been written at
that point; a failed check quarantines with power retained. ``map`` then
completes CE, attaches only SID0 on three DARTs to the private paging domain,
writes and reads back the four DT-provided DAPF ranges, maps their pages as
identity MMIO, and maps preloads, command pool, IPC and heap at their
documented DVAs.

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

``program`` creates the shared H16/H17 program session on this transport and
registers the ABI 3 accel node (``DRM_ANE_CHIP_H17``). Userspace passes the
original H17 HWX and its four host-built sections, as on H16; the kernel
validates them, places every section in private mappings, binds a linked
program's ``__DATA`` scratch to kernel-owned memory and runs the
0x2d/EXECUTE/0x2d sequence with UUID completion. Operation records with a
kernel table must bind the kernel BAR to resource index 1007; legacy tag 2 is
refused before LOAD because the measured H17 firmware asserts on it. The
firmware frees each process pool after UNLOAD, often only during a later
program; that FREE is acknowledged and its backing retained until reboot.

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
accepted for any live exact allocation once the program stage started, since
the firmware frees a process pool after UNLOAD, often only during a later
program. Its backing and quota stay pinned and are never recycled, so the
12 MiB allocation quota bounds the program lifetimes of one firmware boot
(each process pool is 2 MiB). Duplicate, interior and unsolicited FREEs
refuse. The canonical FREE form remains unproven.

Provenance
----------

Register and wire facts were taken from the supplied clean H16/H17 field
specifications and the measured H17 cold-boot and harness results. No private
implementation source or firmware bytes are included. The operation-record
geometry is also documented by the MIT-licensed public ``libane/ane_m2.c``
in https://github.com/joshuaswarren/omarchy-ane. The H17 resource-index rule
overrides that older generation's kernel-tag convention.
