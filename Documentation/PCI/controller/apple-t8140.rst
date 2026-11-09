.. SPDX-License-Identifier: GPL-2.0-only

T8140 radio PCIe bootstrap
=========================

The J700 radio link needs one validated PIODMA config read of function 0's
vendor/device word before native ECAM works. This is a bounded PCIe bootstrap,
not a generic DMA channel provider. Linux owns the SID17 DMA mapping. After
completion, the host validates byte, word and dword ECAM reads for MT7932
Wi-Fi and Bluetooth, bridge forwarding, assigned BARs and cold bus-master
state before publishing either function to drivers. Link power states remain
disabled for this qualified pair. Other downstream devices are not admitted.

Selection and compatibility
---------------------------

Build ``CONFIG_PCIE_APPLE_T8140_BOOTSTRAP=y`` with the host, DART and ASPM.
The host's ``apple,piodma`` phandle selects the engine; the engine's
``apple,pcie-host`` must point back to that same T8140 host. No machine name
or module parameter selects this path. Without the host property, downstream
config access stays closed and the host remains root-port-only. Malformed
links fail rather than silently selecting the root-only path.

The production compatible is ``apple,t8140-pcie-bootstrap``. Existing Stage 2
DTBs using only ``apple,t8140-piodma-diagnostic`` remain supported. New DTBs
list the production compatible followed by the legacy compatible, so older
kernels still recognize them. J700 receives its live DT from Stage 2; updating
a kernel's packaged DTB alone does not update the boot-time DT.

Ownership and failure
---------------------

The state machine distinguishes CPU-owned allocation, lifecycle admission,
consumed attempt, irreversible engine ownership, completed read and admitted
radios. PM and kexec vetoes are acquired before PCI children exist. Contention
on the system transition or kexec lock defers admission without consuming the
attempt. The bounded retry and delayed host attachment retry only this
software admission. They never retry an engine command.

A failed probe frees an unpublished coherent allocation and drops its power
reference. Host unwind releases temporary vetoes only after all children have
been removed and only before engine publication. A preflight failure consumes
the attempt, but does not pin lifecycle vetoes after host unwind. The first
engine write marks ownership irreversible, even if initialization or transfer
validation subsequently fails. The supplier, arena, IRQ and power reference
then remain retained until a full external hardware reset. Both successful
and failed retained states reject another command or a release transition.

Completion IRQ/tag checks, a cleared command bit, FIFO capacity and stable
canaries establish admission observations. None establishes that the engine
has stopped retaining descriptor/completion addresses or can no longer issue
DMA. There is no qualified stop, drain, pointer invalidation or engine reset
sequence in the public implementation. A PCIe link reset affects the link;
it does not prove that SID17 engine ownership ended. Therefore the arena
cannot be freed after admission, and a failed bootstrap cannot safely be
retried merely by resetting the link.

For the same reason, replaying the bootstrap in a kexec kernel or after image
restore is insufficient. A new kernel may reuse the old DMA storage and DART
mappings before it could establish any new engine state. Hibernation can
restore memory over retained ownership. Hibernation, restore, deeper suspend,
kexec, host removal, user resets, hotplug, VFIO assignment and native AER/DPC
recovery remain refused. Supporting these operations requires a proven engine
teardown/reset and endpoint recovery contract first. Renaming the provider
does not qualify those operations.

Suspend-to-idle is allowed only after radio admission, with no IRQ fault,
idle command/FIFO checks and intact arena contents. Resume restores the
configuration banks after genpd power restoration without submitting another
command. A failed or incomplete admission still refuses suspend-to-idle.
Do not remove the supplier or its IOMMU while retained ownership exists.

History and guard audit
-----------------------

This implementation is derived from the GPL-2.0-only public Linux source and
its history. The original file refers to an unnamed accompanying provenance
ledger that is not distributed in this tree. That reference is not evidence
of teardown or reset semantics. No private implementation is used to extend
the hardware contract here.

The following public commits explain the guards retained by this provider
and its host. Commit IDs refer to the history containing base
``4db3a5a22c43`` and are usable with ``git show``.

* ``55039de66e40``: one fixed request, exact MMIO/ECAM ranges, 16 KiB pages,
  SID17 translated low-36-bit DART aperture, prefixed engine pointers,
  descriptor/data bounds, pristine engine state, IRQ budget/status/tag checks,
  expected response and canaries. These restrict the unqualified engine to
  the observed address and request contract; all remain enforced.
* ``55039de66e40`` and ``4429c97c6c7a``: typed radio identities, root forwarding,
  translated endpoint DMA and cold bus-master checks protect publication.
  They remain prerequisites, including admission in each radio driver's probe.
* ``f41094962726`` and ``bb2f7f1f075c``: unscan failed unpublished children and
  require completed admission, so a later rescan cannot publish failed radios.
* ``dbe56bcf475c``, ``17f1649e4eee``: pre-child PM/kexec admission, nonblocking
  lock order, shutdown/consumed-attempt checks and ordered state publication
  prevent transition/probe deadlocks and exposure before guards exist.
* ``69a531f01634``, ``17f1649e4eee`` and ``0886eb8081df``: bounded contention
  retry and delayed attachment on system_dfl_wq preserve progress without a
  hardware retry. Permanent host references and non-joining cancellation avoid
  waiting for a worker behind the host device lock.
* ``67eec79324c6``: device prepare veto covers snapshot ioctls whose notifier
  ran before supplier probe. It complements the hibernation/restore notifier.
* ``bda690fe1fb5`` and DjDeveloperr's ``e91592e904f5``: retained suspend-to-idle
  checks and configuration-only resume remain. The suspend decision stays in
  device prepare, because the notifier runs before the target state is set.
* ``de84c23e4bed``, ``8edd500f3f6d``, ``90e8abce3609`` and ``c4badcb98927``:
  retained hierarchy removal, reset, assignment, hotplug and AER/DPC refusal
  remain. Permanent refusals return EOPNOTSUPP; admission contention defers.
* ``3a0f71240ae3`` and ``d9efe5417547``: the builtin kexec interlock and its
  unload exceptions remain; an attempted transition cannot reclaim the arena.
* ``f45a057bda82``, ``dab09d9a6c9b``, ``b5144ea3ee99``, ``ecb830163924`` and
  ``e23e829f3319``: the shared PCIe-C IRQ gate, interrupt ordering and callback
  lifetime fixes remain untouched. A bootstrap cleanup must not undo them.

Retired selection and presentation guards:

* ``4a78a9f29455`` and ``1238f8c09656`` kept an immutable enumerate parameter.
  ``3ce7b58f735d`` removed it: DT omission already selects root-only operation.
* ``b5773247555f`` restricted the provider to J700/T8140 machine names. The
  reciprocal DT links, exact controller resources, DART/SID, root and radio
  identity checks now carry that restriction without a machine-name switch.
* Diagnostic file, Kconfig and driver names are replaced by production names.
  The old compatible alone remains an ABI. Raw packet and IRQ traces are debug
  messages; the single success info line follows completed host admission.

Validation
----------

``CONFIG_PCIE_APPLE_T8140_BOOTSTRAP_KUNIT_TEST`` tests ownership transitions,
pre-publication unwind, retained failure, sleep admission, arena bounds and
IRQ validation without hardware. The test does not qualify engine teardown.
Owner qualification must check both radio functions, Wi-Fi association and
Bluetooth power before and after three RTC-woken suspend-to-idle cycles.
Preserve error/warning logs against the baseline and keep the previous fleet
kernel as the rollback entry. Retained failure requires full hardware reset.

J700's ``wifi0`` alias and PCI endpoint node allow the public bootloader to
pass the unit's own Wi-Fi address through ``local-mac-address``. The static
zero placeholder is not a usable station address.
