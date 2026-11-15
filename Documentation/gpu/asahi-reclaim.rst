.. SPDX-License-Identifier: GPL-2.0

Asahi backing reclaim and SG leases
==================================

Ownership
---------

``Object::sg_table()`` returns a leased table borrow. Each successful acquisition
increments an atomic user count, and every error and drop releases that count.
The borrow requires a bound parent device and cannot escape its lease.
``into_lease()`` retains backing for copied DMA addresses without exposing a
reference to the table. The owned lease also retains the GEM object.

Asahi stores an owned lease with each VM/BO association's copied DMA segments.
All GPUVA aliases in that association share one backing lifetime: splitting a
logical range does not create another DMA table. Each independent VM association
holds its own lease. Each VM_BIND preparation also retains a separate lease
through commit or cancellation, including a prepared batch waiting on other
operations. Kernel mapping preparation holds the same protection until its
association and kernel mapping pin have taken ownership.

The C ``shmem->sgt`` pointer is the only table cache. The Rust devres entry is a
cleanup registration, not a cached table reference. Idle release and purge free
and clear the C table under the reservation lock. A subsequent lease calls the
C page/SG helper to rebuild the table before exposing any addresses. Cleanup on
unbind or object destruction tolerates an already empty table. Imported dma-buf
objects return their attachment's existing table and never get a Rust cleanup
registration. Neither release nor purge may free an imported attachment table.

Reclaim transaction and locking
------------------------------

Preparation acquires backing before taking BO or GPUVM reservation locks. The
map callback uses the already leased segment copy; it cannot recursively acquire
the shared reservation through the SG-table getter. Restoration follows the same
order. The shrinker uses nonblocking reservation, UAT and BO locks, avoiding
inversion and recursive reclaim waits during allocation.

Idle reclaim first obtains exclusive VM residency and proves firmware ownership
has ended. Purge requires DONTNEED advice and checks fences and other backing
users. The reservation serializes advice, page arrays and SG-table changes. The
shrinker may revoke an association only when its segment lease is the sole SG
user; a prepared bind or another association prevents this. It clears every
alias's PTEs and completes the device invalidations while that lease still pins
the pages. Only after the complete walk succeeds does it drop the segment cache
and its lease. Partial failures retain backing and invalidation/restoration
state.

Release and purge atomically change a zero lease count into an exclusive reclaim
state. A competing acquisition either increments first and makes release fail,
or sees reclaim in progress and fails with EBUSY. Reclaim never waits on the SG
initialization lock. Releasing the exclusive state publishes completion before
new readers can rebuild the table. C checks still exclude imports, exports,
vmaps, page pins, extra page references and unsignaled reservation fences.

Idle release preserves shmem contents for later page-in and mapping restoration.
Purge truncates contents, sets negative advice and leaves logical aliases
INVALIDATED until unbound. An object whose table was already released can still
be purged; no new table is allocated just to discard it. WILLNEED then reports
that contents were lost.

Verification
------------

The Rust GEM shmem KUnit suite exercises real direct-DMA tables on faux devices:
lease counts, independent mapping/bind owners, mapped refusal, no-user release,
repeated release/rebuild with content preservation, DONTNEED purge, purge after
idle release, vmap exclusion, reclaim/acquire exclusion and imported-table
ownership. Run the GPU KUnit configuration under arm64 QEMU with lockdep, plus
the published J700 configuration build and W=1 for drm/asahi.

Neo hardware test plan (not executed)
------------------------------------

Submit any future test through the controller with explicit Neo device identity,
policy approval and all session, source, resource and ownership gates satisfied.
The source-only change does not authorize a boot, hardware access or a test run.
Preserve the current known-good boot selection, exact source/config/artifact
hashes, logs and recovery instructions before an approved test.

#. Establish a baseline on the approved kernel: record the boot identity,
   relevant memory and swap counters, GPU fault/reset counters and kernel log.
   Use a disposable graphics client and bounded allocations so pressure cannot
   starve the user's session.
#. Allocate VM-private BOs, write deterministic checksums through CPU mappings,
   bind each at multiple GPUVA aliases, run a GPU readback, and wait for successful
   completion. Include two independent BOs and partially split/unbound aliases.
#. Leave the client idle beyond the residency grace period. Apply bounded memory
   pressure from a separate disposable process. Confirm backing is released,
   reclaim progresses without EBUSY spinning, and no lockdep, DMA, page-refcount,
   GPU fault or reset report appears. Remove pressure and submit again. Verify
   all aliases restore and both GPU and CPU checksums are unchanged. Repeat.
#. Hold a mapping/bind owner during pressure, including a prepared VM_BIND batch,
   an active job and a kernel vmap. Confirm reclaim refuses until ownership ends.
   Exercise bind/unbind/split and close concurrently with pressure; verify no
   stale DMA address is reused and a failed operation leaves recoverable state.
#. Mark an idle VM-private BO DONTNEED, retain its handle and logical bindings,
   and apply pressure. Confirm purge frees backing and WILLNEED returns retained=0.
   Repeat after idle reclaim has already released its SG table. Unbind and close
   the purged BO, then allocate a replacement and verify normal GPU operation.
#. Repeat the retention test with an imported dma-buf and an exported BO.
   Confirm Asahi's private reclaim/purge path leaves the importer's attachment
   and exporter-owned backing intact; close both sides without double unmap.
#. Stop pressure, close all disposable clients, capture final counters and logs,
   and compare to the baseline. On any fault, stop the workload, retain evidence,
   and follow the approved rollback plan; do not automatically reset or reboot.
