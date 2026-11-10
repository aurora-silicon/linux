.. SPDX-License-Identifier: GPL-2.0-only

GL9755 board power sequencing
============================

Status
------

The sequencing core is built only by the KUnit option. It has no live PCI,
GPIO, IRQ or MMC adapter and does not alter the always-powered SD reader.
In particular, do not initialize it against an enumerated, powered endpoint:
its initial condition is an empty, fully powered-off slot with config access
already excluded. A future adapter must safely establish that condition.

The SMC GPIO driver currently has no IRQ implementation. The Apple SD root
port has no independent insertion interrupt binding or callback that can
restore the rail, retrain and rescan. Endpoint MSI/card-detect interrupts
cannot wake an endpoint whose rail is off. Therefore no new DT property,
module parameter or runtime power switch is provided here.

Adapter requirements
--------------------

* Prove an always-on insertion event, including its wake behavior in s2idle,
  before enabling no-media gating. The helper event is a wake hint; steady
  card presence comes from debounced SDHCI state with the board polarity.
* Deliver helper events through threaded IRQ/work, never hard IRQ context.
  All events take the PCI rescan/remove lock before the power-state mutex.
  Callback implementations must not acquire these locks recursively. MMC
  teardown must not wait for work that is itself blocked on either lock.
* Exclude and drain external config accesses before disabling the port. A
  rescan lock alone does not exclude sysfs config access, PCI error recovery,
  or PM callbacks. The adapter must supply that exclusion as well as order
  recovery, suspend and rescan against this state machine.
* Power the rail, wait 100 ms, enable/train the port at Gen1, allow config,
  then rescan. Request Gen2 only with a usable endpoint; a Gen2 failure must
  retain Gen1 and must not cycle media power.
* Quiesce MMC and remove the PCI endpoint before blocking config, disabling
  the port and dropping the rail. Recheck card presence during quiesce and
  veto removal if media or outstanding I/O still owns it. Keep the helper
  armed throughout the off state. Never use the unidentified I2C sideband.
* Failed enable operations may have partially changed hardware. Cleanup
  disables the port before the rail, and waits 250 ms before a retry. There
  are at most three bring-up attempts. A cleanup error stops retries. A
  failed port disable retains the rail; a failed rail disable retains its
  conservative on state. Neither is reported as OFF_NO_MEDIA. A subsequent
  media-change event retries shutdown without accessing endpoint config.
  A helper received in this error state is retained until cleanup succeeds.
* Unknown card presence or a quiesce veto retains power. Suspend retains a
  live endpoint, leaves an off slot off, and defers helper work to resume.
  Resume without a pending helper event never accesses an off endpoint.

Tests
-----

Run on a Linux build host::

  python3 tools/testing/kunit/kunit.py run --jobs=8 \
    --build_dir=out/kunit-gli \
    --kunitconfig=drivers/mmc/host/sdhci-pci-gli-pwrseq.kunitconfig \
    'sdhci-pci-gli-pwrseq*'

Mocks check the bus lock and state mutex at every operation, intermediate
states, rail/port/config ordering, removal before power-off, bounded retry
and both delays. Faults cover partially successful enables, enumeration,
card-state reads, quiesce, port/rail shutdown and Gen2. Empty/spurious wakes,
stale removals, reinsertion at quiesce, repeated wakes, concurrent event
threads and deferred suspend work exercise the media decisions. No tests access hardware.

Hardware qualification plan
---------------------------

First verify this preparatory change on a J316s with the normal driver and
DT: no-card boot must retain the existing powered/enumerated reader, and
insert, mount/read, unmount, remove, reinsert and s2idle must behave as before.
This change alone cannot demonstrate a power saving.

After a complete adapter is separately reviewed, run the following with
controller-authorized access to the intended machine and a disposable test
card. Capture kernel logs, state/callback traces and PCI topology. Observe
rail state only through a qualified GPIO readback or power measurement.

1. No-card boot: establish the empty-slot condition, quiesce/remove the
   GL9755, block config, disable the port, then drop the rail. Verify no
   config request reaches the off endpoint and the helper stays armed.
2. Insert: verify helper wake, rail on, at least 100 ms settling, Gen1 port
   training and rescan before Gen2. Confirm exactly one host is registered.
3. Mount/read: read a known file and verify its digest. Keep it mounted across
   idle and s2idle; verify no rail or port disable, no DMA/IOMMU errors and
   preserved data. Never physically remove a mounted test filesystem.
4. Unmount and remove: verify removal/debounce and MMC/block teardown before
   PCI removal, then port off before rail off. Verify at least 250 ms off
   time. Reinsert rapidly during debounce and quiesce to exercise veto and
   queued insertion handling; verify eventual enumeration without duplicate
   hosts or stale block devices.
5. Suspend/resume: repeat with no card, with a mounted card, with insertion
   during suspend and removal before suspend. Verify insertion wakes s2idle
   when off, and resume never blindly accesses config with the rail off.
6. Fault injection: fail rail enable, Gen1 training and rescan separately.
   Verify three attempts maximum and both off after successful cleanup.
   Fail Gen2 and verify usable Gen1 without cycling media. Fail shutdown and
   verify a truthful error state, config exclusion and no unsafe rail drop.
7. Confirm no new I2C client, scan or transaction to the unidentified sideband
   address occurs throughout the suite.

Rollback is the prior kernel/DT with the normal always-powered reader. Keep
failure logs and traces; unmount the test card before switching kernels.
