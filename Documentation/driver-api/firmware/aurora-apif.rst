.. SPDX-License-Identifier: GPL-2.0-only

===============================================
Aurora Platform Interface over Memory-Mapped I/O
===============================================

Aura is Aurora's EL2-resident monitor for Apple Silicon, providing SPTM
mediation and nested virtualization. APIF is a synchronous, trusted-kernel
transport. Linux supplies native selectors and endpoint arguments; Aura
forwards them unchanged. Invalid native calls can stop the machine. This
interface does not isolate an untrusted kernel.

Wire contract
=============

The version-2 synthetic register page occupies 16 KiB at guest IPA 0x61ff0000.
Registers are little-endian 64-bit fields: magic at 0x000 is
0x4f494d4d46495041 (APIFMMIO), version at 0x008 is 2, maximum operations at
0x010, and feature flags at 0x018 (bit 0 native, bit 1 memory, bit 2 I/O).
A 64-bit write at 0x100 supplies a 16-byte-aligned guest IPA for the batch.
The entire buffer must fit within one guest RAM bank. The register page is
unmapped at guest stage 2; Aura handles its data aborts.

A batch has a 16-byte header (u32 count, u32 flags, s32 status, u32 done),
then count 64-byte operations (u64 selector, u64 arg[6], u64 ret). Flags are
zero. Selector upper/lower halves give native table/endpoint (x16); arguments
are native x0-x5 and ret is native x0, unchanged. Transport status does not
interpret native results. Void and boolean endpoints need different caller
handling. A failed Aura service stops the batch; done is the completed prefix.

Each client owns a physically contiguous buffer and serializes its submissions
with IRQs masked. The transport orders memory around the trapping store and
preserves the Linux stack in x9 across the synchronous redispatch contract.
Device links order supplier lifetime; client revocation drains submissions and
subsequent calls return ENODEV. It is safe to call from atomic context. Provider
and consumers must be described together in the guest DT; this series does not
rewrite existing board DTs or enable the backend on an existing boot.

Ownership services
==================

Table 0xffffffff supplies Aura services (other tables are native selectors):

* 1 TRANSLATE(guest IPA, size): native PA for a contiguous guest range.
* 2 ALLOC_FRAME(owner, address): zeroed 16 KiB native frame, CPU alias removed.
* 3 FREE_FRAME(PA): restore Aura's alias after detachment and retype to type 11.
* 4 MAP_IO(guest IPA, PA, size, readonly): map bounded page-aligned device I/O.
* 5 GUEST_ROOT(): guest stage-2 root PA.
* 6 BOOT_IOMMU(kind, unit, SID, index, output IPA, size): bootstrap snapshot.
* 7 BOOT_TABLE(kind, unit, SID, address, level): existing bootstrap table PA.
* 8 FIND_FRAME(owner, address): registered lent frame PA or zero.
* 9 TAG_FRAME(PA, owner, address): register a unique opaque key after attachment.

Unused arguments are zero. Frame keys are bookkeeping, not an ownership
transition. The pool is capped at 1024 frames. Never free attached or uncertain
frames. MAP_IO does not roll back a partially successful mapping; a failed
mapping requires recovery before its address range can be reused. Initialize
batch status to EINPROGRESS so a missing completion cannot be mistaken for
success. Lost table-operation completion stops further DART transactions.

BOOT_IOMMU kind 0 means DART. Index U64_MAX returns root level, run count and
DVA aperture; another index returns a retained mapping. The 64-byte output is
u64 level, count, base, size, pa, ipa, attrs, reserved. An unavailable IPA is
U64_MAX. BOOT_TABLE levels 0-2 return existing bootstrap tables, while U64_MAX
returns the bootstrap initialized flag. Bootstrap queries never replay maps.

DART and SART providers
=======================

APPLE_DART_APIF supplies an IOMMU provider for apple,dart-apif. It preserves
stream groups, shared IOVA allocation, retained mappings, bounded batches and
partial-prefix rollback. Linux builds native physical page lists, complements
IOMMU allow bits into native deny bits, and sets up typed tables. Both DMA
backing and list-buffer pointers are translated before native calls. Retained
streams remain reserved and are excluded from ordinary power-down.

APPLE_SART_APIF supplies apple,sart-apif through the existing apple_sart API.
The raw MMIO provider continues using its current operations. The APIF provider
tracks Linux-owned regions and respects endpoint-specific return semantics;
unknown firmware grants are not advertised as inherited. Neither provider
prints routine successful calls or runs probe-time mapping self-tests.

The existing arm64 TSO task interface discovers guest ACTLR support through
its toggle/readback probe; APIF does not add a second TSO protocol. The Apple
WFI preservation quirk also covers captured T6050 parts 0x064/0x065 and uses
the same CPU predicate to avoid unsafe timed waits in arbitrary delay callers.
Existing CPU ranges are preserved. This does not claim future Ultra support.

Validation
==========

Build the providers and run the production-function host tests with::

  make ARCH=arm64 LLVM=1 Image
  make -C tools/testing/selftests/aurora-apif run_tests

Host tests exercise framing, prefix completion, ownership ordering, list
translation and endpoint return handling. Hardware validation of this cleaned
version-2 integration is pending. GPU backend changes are outside this series;
legacy version-1 Aura aliases remain available to existing clients.
