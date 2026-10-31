G17 capacity diagnostics
=======================

The read-only ``/sys/kernel/debug/aurora-g17/capacity`` file samples the live
firmware session. It does not wake firmware or reclaim resources. An absent
session reports ``online=0``. The format is diagnostic, not a stable ABI.
Counts exclude reserved identities. Sub-pool locks and atomic snapshots mean
that independent rows need not describe exactly the same instant.

``used`` is the number of allocated entries unless stated below:

* Physical QIDs are device-lifetime reservations, including installed queues.
  An idle installed QID is reused by its existing engine, never cancelled or
  retyped. Published QIDs need not return to zero after the last client exits.
* Render slot ``used`` excludes released, inactive slots that can be reassigned.
  ``retained`` includes those slots; ``active``, ``quarantined`` and ``kill_held``
  explain why a released owner can still occupy capacity. A published context
  kill retains its render holds until processor stop under the current policy.
* Compute slots include retained graphs and off-lock construction reservations.
  ``reusable`` counts graphs immediately available for another owner. A vacant
  graph still needs fresh pool backing; a populated recycled graph cannot rebind
  until its pool release was consumed and it was rearmed. Per-queue rows show
  these states and the command and item-ring budgets.
* QoS, execution-context, render-pool, parameter-buffer and metrics rows sample
  their actual allocators. A parameter buffer can retain either a graph owner
  or command references. Neither an error fence nor logical close returns a
  firmware-owned reference.
* Context rows show the 63 work-state nodes and two stable render ordinals.
  A context can occur in several graph rows; do not sum duplicate contexts.
* Cached-object reservations include active, returned and poisoned mappings.
  Returned mappings can be reused; poisoned mappings wait for processor stop.
  Reaching this cache's admission cap falls back to uncached allocation.
* Dynamic VA rows count allocator reservations including unmapped guards. The
  largest hole helps distinguish fragmentation from aggregate exhaustion;
  alignment and alias-VM collisions can still prevent a particular allocation.
  Removal can progress during this diagnostic walk. Values are observations,
  not promises that a subsequent placement will succeed.
* Refusal counters name allocation attempts, including attempts subsequently
  retried successfully. They are module-wide cumulative counters; pool gauges
  refer to this device. Compare counter deltas with failed submits and fences.

Shared admission budget
-----------------------

The 128 physical identities cover one tiling/fragment pair for each of the
58 logical execution contexts, two bootstrap reservations, and ten shared
compute channels: ``2 * 58 + 2 + 10 = 128``. Logical render owners no longer
reserve a speculative second pair. Its 16 in-flight commands share the same
physical pair; admission waits, interruptibly and with the existing two-second
job budget, when that pair is full. The DRM queued-job credit budget is retained.

Compute sizing is validated at ioctl admission, but a physical channel is not
reserved until DRM dependencies permit the job to run. This matters for reverse
timeline chains: the host signal can be submitted only after their earlier
ioctls return. An idle compute channel can yield to another logical queue only
after exact retirement, with no pending kill, growth, quarantine, replay,
predecessor or pool release. Its original QID and ring remain installed.

Capacity pressure waits outside device locks and preparation leases. A short
reclaim transaction takes the usual session/epoch lease, polls consumed pool
releases and may publish idle compute-pool releases. Render backing is reclaimed
only if never registered or after its free-list release was consumed, all
associated graphs are idle,
and no admission borrower or off-lock builder can use it. Backing destruction
remains deferred until after the device mutex is released; the next owner
rearms the same pool using the existing epoch/descriptor protocol. A deadline
never frees an unwitnessed resource. Terminal errors are preserved.

Logical close is recorded before scanning installed graphs. Late compute
constructors and rearms recheck that flag, so an off-lock reservation cannot
resurrect a frontend that already closed.

The VA diagnostic walk retains UAT ownership but drops the device mutex before
acquiring VM/TTBAT locks, so mapping destruction and its firmware flushes cannot
invert the diagnostic lock order.

Compute close and failure ownership
-----------------------------------

A never-installed graph can cancel its unpublished QID. An installed graph
stays allocated after close. If a detached graph was rebound and closed before
its first new publication, an empty, non-quarantined command FIFO retains its
original idle witness: no future completion exists to rehabilitate that owner.

A superseded compute binding remains owned until the replacement publication
retires. An exact terminal drain requires closed recovery, valid tickets, a
covering kick stamp, a fully consumed item ring and matching command accounting.
After that witness, predecessor ownership can retire even if the public fence
failed. An acknowledged context kill must settle its commands before the same
cleanup. An unpublished predecessor remains available for close rollback.
Collector backpressure retains the command and predecessor for another pass.

Off-lock root reclamation requests another device pass so idle bindings can
release their exited contexts and QoS leases. A closing queue records its idle
time when its last ordinary or spared command retires, allowing warm-pool
expiry to run even if close preceded retirement.

These transitions do not make an error fence into a lifetime witness, cancel
an installed QID, relax ray-traversal kill exclusions, or release a published
render kill hold. Unwitnessed work remains retained.

Tests
-----

GPU KUnit covers predecessor cleanup, finite allocator rollback and repeated
closed, quarantined render leases after an explicit drain witness. The host
``tools/asahi/g17-capacity-fault-tests.py`` compiles the production compute close,
drain, kill, reclaim and recycle methods with mock MMIO. It exercises 512 fault/close and 512 acknowledged-kill cycles,
incomplete stamp/cursor/recovery witnesses, collector backpressure and rollback.
Thirteen mutation controls cover close/fault/kill/reclaim cleanup, shared
compute retirement and kill guards, pool-release admission, and private-pool
release/borrower/construction witnesses, deferred physical admission, and
logical close during off-lock construction. The shared-pool host adapter repeats
512 four-client cycles and keeps destruction outside the device lock.
These tests do not simulate firmware or establish hardware recovery behavior.

Render admission uses one monotonic absolute deadline across physical identity
retries and the inner pair wait. Hazard/quarantine observations cannot renew
that deadline. Pair waits check packet cancellation, logical close and device
loss before acquiring a lease, and sleep at most 10 ms (rounded to a jiffy) so
cancellation requires no release notification. Timeout/cancellation removes the
waiter's footprint without releasing any active or quarantined lease.

``g17-admission-deadline-tests.py`` compiles the production wait loop with a
controlled clock and condition variable. It covers all-pairs-busy/no-progress,
unrelated wake storms that report unchanged jiffy budgets, cancellation without
notifications, signal cleanup and an already-expired outer deadline. Native
``asahi_g17_admission_deadline`` KUnit covers real timed waits and cancellation
as well as absolute-deadline arithmetic.

Compute admission starts one monotonic deadline after dependencies become ready.
The same packet admission context crosses binding-loss retries, shared aperture
and scheduler construction, pool growth, physical-capacity reclamation and the
preparation gate. Every retry checks packet cancellation, logical close, VM
status and device loss. Shared-constructor and closed-gate condition waits use
at most 10 ms slices (rounded to a jiffy); wake reports never reset the deadline.
An already-spent budget refuses even immediately available capacity. Expiry
returns EBUSY and cancellation returns ECANCELED before a new publication.
These are cooperative wait bounds, not preemption of synchronous allocation or
mutex holders. Post-allocation checks reject installation after expiry.

Growth drops its outer preparation lease before re-entry. Failed growth keeps
its physical token through abort, drops tentative mappings before returning an
unpublished QID, and retains installed firmware-owned backing until its existing
witness. Constructor failure clears its builder flag even if the packet expired
while the off-lock allocation was running.

``asahi_g17_compute_deadline`` KUnit exercises real preparation-gate timeouts,
cancellation without events, spent budgets and recovery/re-entry churn.
``g17-compute-deadline-tests.py`` executes production ready-packet admission,
its inner witness wait and gate entry with a controlled clock, including the
repeated shared-binding theft interleaving. ``g17-compute-constructor-tests.py``
executes both shared constructors. ``g17-compute-growth-deadline-tests.py``
executes the complete growth wait and abort. Their deliberate mutations must
fail at specific deadline, cancellation or lifetime assertions. The pool-release
adapter has only reachable mutation controls and rejects unchanged mutations.
Firmware/MMIO and allocation boundaries in the host adapters remain mocks.
