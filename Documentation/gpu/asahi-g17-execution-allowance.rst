Bounded G17 execution allowance
==============================

Selection and scope
-------------------

73f already selected native ray queries in Compute and both Render halves.
It did not cover untagged helper/transfer commands ordered behind them on
another engine. The scheduler watchdog is per engine: an untagged follower
can reach its two-second poll while a legitimate long producer still runs.
The failure location in a runtime settlement path is not itself proof that
the long producer hung. This change addresses that selection gap; the exact
reported hardware failure attribution still requires qualification.

After every command and destination in one submit ioctl has been validated,
select a long execution allowance if any command contains validated native
intersection state (CDM, VDM or fragment), or the explicit AS-build hint.
Apply that selection to every command in that ioctl before enqueueing any.
VDM covers vertex execution; tessellation and geometry stages may also use
CDM pre-graphics streams, so both engines must participate. An untagged
ordered transfer/helper in that submission receives the same finite budget.
An ordinary submission retains its original deadline. Selection never changes
VM access checks, firmware dependencies, fences, admission, teardown, recovery
ownership or resource limits, and does not persist into another ioctl.

Build kernels use ordinary compute and compute-based blits, not intersection
state. Neither SCS allocation, attachment count, stream byte size nor the
existing WIDE_VISIBILITY flag identifies an AS build reliably. Use the
existing per-command compute flags field with DRM_ASAHI_COMPUTE_AS_BUILD,
advertised only on G17 by DRM_ASAHI_FEATURE_BOUNDED_AS_BUILD. Older kernels
reject that flag; Mesa must test the feature before setting it. This is an
untrusted hint, never proof that the stream builds an AS or makes progress.
All existing command/resource validation still runs. Reserved/unknown flags
remain rejected. Userspace can at most obtain the same fixed allowance as
native ray-query metadata already permits.

Bound and sizing
----------------

Keep the scheduler interval at 2,000 ms. Only a published selected job can
consume four additional polls: (1 + 4) * 2,000 = 10,000 ms. The counter belongs
to the scheduler job and never resets at a replay, wake or resubmission within
that job. Exhausted and invalid counters cannot wrap. Ordinary/unpublished
jobs cannot consume this credit. Cross-VM attribution has its own pre-existing
8-credit bound: at most 26,000 ms of combined poll budgets if all 4 execution
and 8 cross-VM credits are consumed. These are watchdog policy budgets, not
hard real-time wall-clock guarantees (workqueue scheduling can delay a poll).
Existing replay/retirement grace is independent; this patch does not alter it.

The public CTS ray-query stress scene has 20,000 levels * 3 primitives =
60,000 primitives and 60,000 result rays. Ten seconds requires 6,000 rays/s;
two seconds requires 30,000 rays/s. In an adversarial all-overlap traversal,
60,000 * 60,000 is 3.6 billion potential primitive visits, requiring 360
million visits/s at ten seconds versus 1.8 billion/s at two seconds. This is
a conservative sizing model, not a measured G17P rate or an assertion that
all tests perform every possible visit. Native traversal includes generated
shader, candidate and continuation work, so hardware peak ray throughput
cannot be substituted for end-to-end CTS throughput.

The requested million-geometry build scale (1,048,576 geometries) requires
104,858 geometries/s at ten seconds versus 524,288/s at two seconds, before
accounting for geometry uploads, leaf generation, Morton sorting and native
encoding. Geometry count and primitive count are distinct; this is not a
universal throughput promise. Retaining the established 10-second allowance
provides 5x the old budget without giving arbitrary shaders an unlimited
watchdog exemption. Splitting or optimizing larger builds remains necessary
if real jobs exceed it. No new hardware measurements were made for this patch.

Comparison
----------

The G13-G15 Asahi scheduler fallback in queue/mod.rs is 100,000 ms for all
jobs, including software ray traversal and build compute. The G17 execution
allowance is one tenth of that scheduler fallback and ordinary G17 work
continues to use 2,000 ms. Firmware fault handling is separate from the host
scheduler fallback, on both paths.

Apple documents MTLCommandBufferError.timeout for command buffers that take
too long. Its public API does not specify a portable numeric macOS watchdog
limit or an entitlement to unlimited GPU execution. Therefore 10 seconds here
is a Linux policy choice, not a claim of macOS parity or a macOS guarantee.

Public references:

* https://github.com/KhronosGroup/VK-GL-CTS/blob/main/external/vulkancts/modules/vulkan/ray_query/vktRayQueryStressTests.cpp
* https://developer.apple.com/documentation/metal/mtlcommandbuffererror-swift.struct/timeout

Validation
----------

asahi_g17_execution_allowance KUnit and the host runner execute the same
production selection, flag admission and finite-credit functions. Cases cover
CDM traversal, each render half, AS builds without intersection state, mixed
helper submissions, ordinary following submissions, unpublished jobs,
exhaustion/overflow and independent cross-VM attribution credits. Software
gates cannot prove completion or correctness of the seven graphics stress
cases or the 29 large AS-build cases; qualify those under a separate approved
Neo hardware task using matching Mesa and kernel artifacts.
