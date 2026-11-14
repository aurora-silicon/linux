.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Asahi client engine time
========================

Asahi reports the following optional keys in each DRM file's fdinfo, following
:doc:`drm-usage-stats`::

    drm-engine-vertex:   123456 ns
    drm-engine-fragment: 234567 ns
    drm-engine-compute:  345678 ns

``vertex`` is the tiling/vertex stage of render commands, ``fragment`` is their
fragment stage, and ``compute`` is compute dispatch. These are separate timelines:
vertex and fragment can overlap. There is no separately submitted blit engine in
the driver's command ABI. Copies executed by compute or render are included in
the engine that executes them.

The counters belong to the open DRM file, rather than to a PID or a queue handle.
All queues and VMs of that file contribute to the same ledger. Duplicated or
inherited file descriptors share counters and must be deduplicated using
``drm-client-id``. Closing a queue or VM does not reset its file's counters, and
completions retain the ledger until their firmware work retires. A new DRM file
starts at zero, even when another file in the same process has submitted work.

Time source and limits
----------------------

Only completed stage intervals are reported. A long-running command contributes
after retirement, so polling during execution can temporarily show no change.
Counters accumulate monotonically, saturating at the largest unsigned 64-bit
value. Conversion rounds down by less than one nanosecond per retired stage.

G13 and G14 use the firmware's internal start/end timestamp cells. The existing
timestamp microsequence operations run even when the application requests no
timestamp output. Internal buffers were already allocated for each command;
fdinfo never reads an application's timestamp buffer.

The G15 and native G16 backends reuse the internal timestamp cells already read
by their successful retirement statistics path. The shared G17 execution backend,
including T6040/G16S and T8140/G17P, charges the timestamp pairs delivered by its
once-only completion path. These backends use the firmware's 24 MHz command timer;
the classic backend uses its hardware configuration's timer frequency.

These are firmware stage envelopes, not hardware shader occupancy measurements.
They can include firmware scheduling, preemption and stalls between the stage's
start and end operations. The shared G17 backend runs several commands of one
queue at once, so their stages overlap. It charges each engine of a queue the
union of that queue's stage intervals (exact, because a queue's stages retire in
the order they started), so a queue never counts more than the wall time on one
engine. Queues of one file still add up. The other backends sum their intervals
without merging overlapping commands on the same engine. Missing, zero-start or
reversed intervals are ignored. Failed or cancelled commands do not contribute
incomplete intervals;
successful stages that retired before a later failure remain counted. An
unexecuted G17 replay contributes only when it eventually retires successfully.
There is no submit-to-completion wall-time fallback in the current backends.

``drm-cycles-*`` is intentionally absent. The timestamp clock is independent of
the variable GPU execution clock. Treating command timer ticks as GPU busy
cycles, or multiplying time by a sampled/maximum GPU frequency, would give
incorrect frequency-weighted utilization. Per-client execution-cycle counters
would require a separate hardware measurement source.

On the M5 and the A18 Pro each engine also has the frequency keys::

    drm-curfreq-compute: 1620000000 Hz
    drm-maxfreq-compute: 1620000000 Hz

``drm-curfreq-<engine>`` is ``achieved_frequency_hz`` of ``agx_stats`` (see
below): the frequency of the performance state the firmware reports, 0 Hz while
the GPU cores are gated. It is absent while no state is known, as right after
the GPU powers up. ``drm-maxfreq-<engine>`` is the highest frequency of the
performance states. One clock
drives the three engines, so both values are the device's, the same for every
engine and every client. They show the clock at the time of the read and are
not a weight for the engine times: without ``drm-cycles-*`` there is nothing to
scale. Other GPUs print neither key.

Accounting cost
---------------

Each file allocates one three-counter ledger, and each G17 queue one three-mark
lane in front of it. Queues and pending completions retain references to them.
Retirement adds one relaxed, saturating atomic update per valid stage, after one
compare-and-swap of the lane's mark on G17, with no new locks or allocations.
The native backends reuse
existing timestamp reads. Classic completion callbacks read two internal cells
per stage; commands without application timestamps also execute two existing
timestamp microsequence operations per stage. Reading fdinfo loads three counters
and does not traverse queues, wait for fences or access hardware. On the M5 and
the A18 Pro a read also takes the device mutex once and drains the statistics
rings, as a read of ``agx_stats`` does, for the frequency keys.

G16S and G17P device utilisation
------------------------------

The shared G17 backend also exposes device-wide utilisation through the existing
``agx_stats`` sysfs attribute. Both firmware graphs use the same decoder for
statistics tag ``0x13``: words 3, 4 and 6 are tiler, renderer and device
percentages. Each complete record contributes one equally weighted sample to
three bounded sums and a common count. Device utilisation is independent of the
two engine percentages; it is not their sum.

Reading ``agx_stats`` drains the statistics rings of both firmware roles under
the device mutex, even without an event notification. The reader observes power
epochs before and after draining and retains the transport's copy-before-consume,
suspension, queued-prefix discard and quarantine rules. It does not wake the GPU
or send a firmware command. Event service continues to drain the same rings.

``utilisation_status`` distinguishes valid, stale and unavailable samples.
Numeric ``util_tiler_percent``, ``util_renderer_percent`` and
``util_device_percent`` values appear only for valid samples, including valid
zero samples. The existing ``unsupported`` value placeholder is retained for
other statuses; consult ``utilisation_status`` for the reason. The source,
sample count and newest host receive timestamp are reported as
``utilisation_source``, ``utilisation_samples`` and ``utilisation_host_ns``.
The averaging/freshness policy is selected by firmware graph: two nominal
periods (16 ms or 32 ms), with the existing measured delivery slack for batched
packets. Suspend, recovery, GPU power epochs and ring quarantine invalidate the
snapshot; old samples cannot become a new epoch's baseline.

The firmware power-gates the GPU cores a few milliseconds after the last work
completes and emits no utilisation record while they are gated. A power gate
therefore ends the averaging epoch but is not a loss of knowledge: the last
sample stays valid until it ages out, after which the gated GPU reads as a valid
zero for every utilisation value until it powers up again. Waking starts a new
epoch, stale until its first record arrives.

These samples have neither a qualified duration nor a client identity. They do
not change ``busy_ns`` or fdinfo's per-client ``drm-engine-*`` counters. A quiet
ring on powered cores becomes stale rather than fabricating a zero-utilisation
sample; only gated cores read as zero. Readers
must not convert these percentages into engine nanoseconds or cycles.

Performance state and busy time on V26A428 (A18 Pro and M5)
-----------------------------------------------------------

The V26A428 firmware, which the A18 Pro (T8140, G17P) and the M5 run on the same
backend and decoder, carries more in its statistics ring. ``agx_stats`` reports
there:

* ``pstate``: the performance state in effect, from the firmware's state
  reports (tag ``0x1b``, words 1 and 3, which must agree). It is an index into
  the performance state table: 1 to 10 on the A18 Pro, 1 to 13 on the M5. The
  interval record before a report (tag ``0x19``) gives its firmware timestamp.
  A report that names state 0 or a state beyond the table is not used. While
  the cores are gated the GPU is off and ``pstate`` reads a valid 0; power-up
  starts a new epoch, ``stale`` until the next report.
* ``achieved_frequency_hz``: the frequency of that state while
  ``pstate_status`` is ``valid``, 0 for state 0. The M5 takes it from its
  board's table. The A18 Pro takes the device tree operating point that the
  state's entry of its performance table A names: its records do not name the
  table, but at its top state a sustained FP32 FMA load completes faster than
  table B's frequency there allows.
* ``busy_ns``: the utilisation records carry their own timestamps (words 1 and
  2, 24 MHz ticks), one every 8 ms on the M5 and every 16 ms on the A18 Pro, so
  each record closes a window of known length, which is credited at the
  record's device utilisation. Records are credited in timestamp order, from
  both roles and across power epochs: the driver keeps the timestamp up to
  which it has counted, and a record at or before it adds nothing. A record
  within two periods of that mark closes the window since it; after a longer
  gap it closes only its own period, so time the GPU spent gated is never
  credited. Under a frame-paced load the cores gate many times a second, often
  with one record per power epoch, so the records a power boundary keeps out of
  the utilisation average still count here. Suspend, recovery and quarantine
  keep them out, and ``busy_ns`` reads ``stale`` once a statistics ring is
  quarantined.

The driver asks for full performance while work is in flight, which the
firmware takes as a floor for its state. An A18 Pro at work therefore normally
reports its top state; a lower one is a firmware limit, such as a thermal one.
The tag ``0x19`` records are periodic samples, not busy intervals, and do not
contribute to ``busy_ns``.

``requested_frequency_hz``, ``power_mw`` and ``temperature_raw`` stay
unsupported. ``busy_ns`` is device-wide, as on G13 and G14; it does not change
fdinfo's per-client ``drm-engine-*`` counters.

Deferred shared power telemetry
------------------------------

Current pstate and requested/achieved frequency remain unsupported on G16S
(V25G83); V26A428 reports its state in the statistics ring, see above. The
shared-power snapshot ABI is not sufficiently specified to implement a
read-only reader, which G16S would need, and which would also name the A18
Pro's table directly. For **each** of G16S and G17P, it still needs:

* The firmware-version-to-layout mapping, containing allocation and bounds,
  offsets, widths and stride of the generation/seqlock word, live pstate,
  performance-table selector, residency/throttle counters and PI-controller
  effective-frequency field.
* The generation word's stable/odd/update protocol, including its relation to
  reset and power epochs and the order required for a coherent snapshot.
* The exact selector bit/encoding for performance table A versus B, and the
  valid live pstate range in that table.
* Effective-frequency units, scaling, valid range and overflow rules for both
  layouts, qualified against an independent frequency measurement; G17P's
  units in particular remain unresolved.
* Counter units, wrap/regression/reset semantics, and whether controller fields
  remain valid while the GPU is power-gated.

A future implementation needs bounded seqlock retries and lifecycle invalidation
before publishing these fields. Tag ``0x10`` remains decoded for diagnostics but
is not emitted by the currently qualified firmware graphs. A dormant pstate
record, a host request or a command timer cannot provide achieved frequency.
On G16S, ``drm-curfreq-*`` must wait for a qualified shared snapshot.

Hardware acceptance
-------------------

Validate the same candidate on J616s (G16S) and Neo (G17P), retaining the known
boot fallback. Record the firmware graph, ``aurora-diag gpu``, ``agx_stats``,
the load client's fdinfo and the initial kernel warning count. Poll statistics
at 4--10 Hz through 60 seconds idle, at least 60 seconds of ``glmark2``, then
60 seconds idle. Require utilisation to rise under load and return to fresh
zero samples at idle, with every value in 0--100 and zero new kernel warnings.
Stale or unavailable samples do not satisfy the idle-zero check.

Repeat ten idle/load transitions, suspend/resume and a normal GPU power epoch.
Require no ring quarantine or cross-epoch accumulation, renewed valid samples
after each boundary and no rendering/recovery regression. Confirm only the
submitting client's fdinfo engine counters increase. These checks are runtime
acceptance criteria; host decoder tests alone do not establish a hardware pass.

Host arithmetic tests
---------------------

The ledger uses only ``core`` and can be tested directly from a source checkout::

    rustc --edition 2021 --test drivers/gpu/drm/asahi/usage.rs -o /tmp/asahi-usage-test
    /tmp/asahi-usage-test

Tests cover conversion, missing/reset timestamps, saturation, overlapping stages,
client isolation, out-of-order completions and simultaneous queues sharing a file.
