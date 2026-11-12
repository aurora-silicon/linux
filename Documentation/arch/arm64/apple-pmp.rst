.. SPDX-License-Identifier: GPL-2.0-only OR MIT

T6000 and T8112 PMP boot inputs
==============================

The ``apple,t6000-pmp-v2`` Rust driver starts firmware already loaded in PMP
SRAM. T8112 uses this compatible as a fallback. Linux does not load a PMP
firmware file. The bootloader must preserve the resident image, prepare the
PMP DART protection filter and pass the current boot's calibration.

Inputs read by the driver
-------------------------

* ``compatible`` selects the transport configuration.
* ``reg`` and ``reg-names`` provide ``pmp`` SRAM (at least 0x80000 bytes) and
  ``asc`` control registers (at least 0x4000 bytes).
* ``mboxes`` index 0 supplies RTKit's mailbox. The binding names it ``mbox``.
* ``iommus`` attaches the PMP to its DART. The power-domain resources of the
  PMP, DART, mailbox and report region must remain powered as required.
* ``apple,board-id`` and ``apple,dram-vendor-id`` are required u32 values.
  They patch the BDID and DVID integer boot arguments before ASC RUN.
* ``apple,dram-capacity`` (DCAP), ``apple,dram-channel-disable`` (DCHD),
  ``apple,pmc`` (PMC_), ``apple,pmc-msg-disabled`` (PMCX), and
  ``apple,soc-chip-variant`` (CVAR) are optional u32 boot arguments.
  ``apple,pmc-pmgr`` supplies bit 0 as PMCV and bit 3 as PMCB. Missing optional
  values leave the corresponding resident firmware argument unchanged; the
  protocol defaults are zero. Do not invent later-SoC PMC values for these
  targets.
* ``apple,pio-ranges`` is an optional array of u64 address/size pairs used
  when firmware requests an IOVA table. T6000's seven pairs are described in
  ``t6000.dtsi``. T8112 has no PIO mapper table.
* ``apple,tunable-<name>`` supplies opaque bytes when firmware requests an
  IORegistry value named ``<name>``. This is an open firmware-driven set,
  including tables such as ``soc-device`` and ``ptd-range``. It cannot be
  replaced by a fixed list of tunables in DTS. Missing values are returned
  as missing, and values larger than the firmware buffer are refused.

There is no extra ``apple,pmp-report`` requirement in the Rust driver's
T6000/T8112 probe path. The report provider has its own compatible, MMIO,
power domain and consumer entries. Its startup report is at offset 0x10.
T6000 request/read/update/ACK offsets are 0xf80/0x107c0/0x1000; T8112 uses
0xa00/0x10500/0xa40. The presence of a report provider alone does not start PMP. Its read-only
``/sys/kernel/debug/apple-pmp-report/<device>`` file exposes ``running`` even
when the provider has no fast-die sensors or deferred-start controller.

Bootloader and static DT responsibilities
----------------------------------------

Aura's Stage 2 ``dt_set_pmp()`` traditionally resolves the ``pmp`` alias,
copies ``/chosen/board-id`` and ``/chosen/dram-vendor-id`` as big-endian DT
u32 cells, optionally copies ``iop-pmp-nub/dram-capacity``, and transfers the
remaining ``/arm-io/pmp/iop-pmp-nub`` properties to ``apple,tunable-*`` without
changing byte order. Transport metadata (compatible, phandle, firmware/segment
and region information, preload marker, name, coredump flag) is excluded.
T6040/J616s and T8140 use this common alias path. It is not a source of fixed
board or DRAM-vendor constants.

T6000 and T8112 normally omit that alias. Stage 2 now finds their existing
PMP node by compatible and publishes the same values, preserving the supplied
status even when an alias exists. Thus a disabled node can carry complete
boot data without starting the coprocessor. Invalid scalar widths are refused.
The raw ADT and calibration values are per-boot data and must not be committed.

On T8112, ``APPLE_USE_PMP`` is an explicit experimental DT build profile. It
adds the alias, enables the PMP node and routes existing DCP/AVD/ISP consumers (and the
T8112 GPU) through their PMP report domains. On T8112 it also marks the AVD
and JPEG report entries ``apple,adt-dvfs`` so the bootloader fills their
performance requests from the ADT, and links the CPU clusters and the ANE to
the report provider for their memory/fabric floors. Compile the entire board DT with
this define; a status-only edit does not select that power-domain graph.
Default DTs remain disabled pending hardware qualification. Compile each
qualified board separately rather than enabling this define for a fleet set.

``tools/asahi/t6000-t8112-pmp-dt-tests.py`` also emits a separate J413
``pmp-avd-vmax`` test DTB. It keeps AVD on its physical PMGR core and disables
the native AVD report entry, leaving bit 10 unclaimed for a runtime loader's
core/perf graph. This is a separate cold-boot profile, not a live
fallback from the full PMP graph. The loader must retain all its normal
running, calibration-layout, provider and module-version checks.

Transport match data
--------------------

``HW_CFG_DEFAULT`` is the original T6000 transport configuration and T8112's
fallback: no DMA mask override, no mandatory UUID-version-5 header check and
no CoreAnalytics endpoint 7. Both use application endpoint 0x20. T6000's
PIO area starts at 0xc0000000; T8112's DART window is below 4 GiB. The newer
42-bit windows and mandatory endpoint-7 handshake are not applicable.
Four T6000 and five T8112 DCS states are report/DRAM-description facts; they
do not require a different RTKit transport or an inferred frequency table.

Qualification and failure boundary
----------------------------------

Test Stage 2 in RAM over a proven tether first, with a recoverable baseline
boot. Require successful RTKit startup, endpoint 0x20, registry confirmation,
``running: 1``, and a successful existing consumer request/ACK before attempting
new performance votes. The running bit is retained state, not a freshness
challenge. Compare idle thermals, frequency policy and kernel errors with the
baseline before running AVD performance tests.

Once ASC RUN is set, firmware can own fabric, memory and thermal controllers.
A failed start, crash, stall or ACK timeout is not permission to unbind PMP,
clear RUN, rewrite DART/PIO mappings or restore the consumer graph in place.
Retain the resource graph and recover by rebooting into the baseline. No new
FAST-DIE, energy, PMC/CLPC or memory-cache client is qualified by this profile.
