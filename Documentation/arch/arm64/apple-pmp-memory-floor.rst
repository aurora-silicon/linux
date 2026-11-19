.. SPDX-License-Identifier: GPL-2.0

===================================
Apple PMP shared-memory floor words
===================================

This document describes the clean-room host contract used by the Linux PMP
report provider for T6000 CPU shared-memory floors.  It names fields by their
function and does not define firmware policy.

Request word
============

The request is one 64-bit word:

=========================  ========  ================================
Field                      Bits      T6000 CPU-client range
=========================  ========  ================================
default device level       4:0       unused by this request
memory-controller floor    35:32     logical level 0 through 3
constraint lane 1          39:36     zero
constraint lane 2          43:40     zero
constraint lane 3          47:44     zero
fabric floor               51:48     logical level 0 through 2
valid                       61        one for requests and releases
=========================  ========  ================================

The highest qualified T6000 CPU floor is therefore
``valid | memory-controller(3) | fabric(2)``.  A release keeps ``valid`` set
and puts zero in every level field.  Levels are logical performance states,
not CPU operating-point numbers or bandwidth values.

The three T6000 CPU clients use consecutive device-performance entries:

==========================  =====  ==============
Client                      Entry  Update offset
==========================  =====  ==============
efficiency CPU cluster      0x108  0x10840
performance CPU cluster 0   0x109  0x10848
performance CPU cluster 1   0x10a  0x10850
==========================  =====  ==============

Update offsets are relative to the report mapping.  Each entry is eight bytes
in the update aperture.

Transaction order
=================

Before the first device-performance request, the host reads the global DVFS
ready report.  If it is zero, it stores one to the corresponding update entry
and waits for the report to become nonzero.  The achieved-state report is
read-only and is never used as a request.

For each CPU client the host then:

1. sets that client's device request bit;
2. waits for the matching acknowledgment bit;
3. stores the complete 64-bit floor word once.

Release is ordered in reverse: store the valid zero-level word, clear the
device request bit, then wait for acknowledgment to clear.  A host may keep
the request and acknowledgment resident while changing floor levels.

The efficiency and performance-client request bits are 6, 7 and 8.  Device
performance entries are write-only transactions: reading an entry to verify a
store can consume the pending update.  Diagnostics must instead use the
provider-owned shadow plus the device acknowledgment, global ready and
achieved-state reports.  T6000 maps this region as non-posted MMIO so the
ordering above is observable by the controller.

Linux policy and activation
===========================

The provider accepts ``apple,memory-floor`` as ``<entry memory fabric>`` and
checks both logical levels against the SoC ranges before any write.  T6000 DT
contains disabled CPU-client nodes with the qualified maximum.  Enabling a
node is an explicit test policy; production boards do not opt in by default.

The report provider exposes a read-only debugfs discriminator containing its
device-request shadow, the controller acknowledgment word, the global DVFS
ready report and the achieved shared-memory state.  It deliberately does not
read a device-performance request entry.
