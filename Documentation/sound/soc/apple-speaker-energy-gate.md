# Playback energy gate design — review draft

Status: implementation foundations only. No capability is advertised and no
production protection is relaxed by this series. The runtime wiring and the
acceptance campaign below remain required before this can satisfy t-3188.

The daemon's capture stream describes past output. It cannot certify the next
playback period. The replacement contract spends finite per-speaker energy on
immutable samples at the hardware descriptor boundary. Thermal feed-forward
drives the grants; V/I may tighten them. Existing kernels and every unsupported
route retain the current conservative policy.

## Submission boundary

MCA playback uses ADMAC, not SIO. `admac_cyclic_write_one_desc()` supplies a
finite descriptor; `admac_cyclic_write_desc()` refills a four-entry FIFO. A
hardware-active descriptor can exist outside that FIFO. Source PCM can be
mmap writable and the software cyclic transaction may repeat a period. Thus
neither an ALSA copy callback nor a period callback after playback is the gate.

The intended ADMAC integration copies each submitted period to a private DMA
buffer, applies the gate there, then publishes its descriptor with the normal
DMA ordering. A private buffer is not reused until that descriptor's completion
report. A finite pool must cover the FIFO, the active descriptor and completion
lag; pool exhaustion emits no unaccounted descriptor. Every repeat spends again.
Allocation, mapping and format validation happen before trigger, never in IRQ.
An IRQ stall can drain only already charged descriptors, never replay a raw
userspace ring. DMA/report failures retain outstanding energy as debt.

The same boundary is needed on J616s's `aop_spkr.rs` speaker ADMAC channel. The
firmware service's buffering and downstream sample transforms must be bounded
before that route advertises support. T8140's software-gain/float conversion is
a separate route and cannot inherit a TAS qualification.

## Owner and epoch

The existing `Speaker Volume Unlock` lock remains the authority. The future
grant control must be locked by the identical `snd_ctl_file`, as must every
existing speaker volume control. It does not independently unlock an amplifier.
Existing lease expiry, failed volume writes, suspend and owner close still lock
the amplifiers. Grant expiry uses absolute boottime, including idle and suspend;
it cannot extend the playing-time lease.

One monotonic identity binds owner, route, format, sample rate, all physical
speaker mappings and all downstream gain/processing bounds. Changes revoke
unspent credit before they become audible. Stopped, paused, drained, prepared,
restarted and suspended streams cannot reuse an old grant. Live route changes
must either synchronously quiesce and reprepare all affected streams, or refuse
the change. Unsupported and ambiguous mappings stay conservative. All possible
routes, including direct ALSA and both front ends, must be covered before the
card advertises the capability. No module parameter or command-line kernel
switch participates in this decision.

## Units, counters and transactions

For signed PCM normalized to Q31, a sample costs
`ceil(abs(sample)^2 / 2^32)` units, i.e. Q30 full-scale-squared frames.
Quantization rounds energy upward, including the most negative word. A rate
epoch converts units to normalized seconds; the profile's worst amplifier
voltage squared over the smallest valid load converts that to joules. The
normalizer must validate S16_LE, S24_LE-in-32 and S32_LE separately.

Each physical speaker has its own counter, even when speakers share one PCM
slot. The most restrictive of all speakers using a slot limits that sample.
The kernel clamps before publication. Exhaustion never permits a negative
balance. Internal arithmetic overflow revokes credit. The standalone prototype
uses zero output on exhaustion; this is deliberately stricter than a nonzero
safe-level tail, and is not the final fallback policy.

Renewal supplies an absolute cumulative ceiling based on an atomic snapshot,
not an amount to add. The snapshot sequence changes on every submission,
renewal and revocation. A stale transaction or replay fails without adding
credit. A new epoch never resets the lifetime submitted/completed counters or
forgives queued energy. A completion token is single use, FIFO ordered and tied
to the exact immutable submission; it cannot acknowledge another epoch's debt.
Read failure or counter saturation cannot be interpreted as zero debt.

An accepted grant cannot revoke bytes already committed to hardware. Their
entire energy remains reserved until completion proves consumption or a
verified stop proves discard. Expiry prevents further high-level submissions;
the existing amplifier lock additionally restores the safe hardware level.
The model must remain safe even when the amplifier write is delayed and all
reserved descriptors finish. A restart must recover the ledger and a safe
thermal upper state, or stay latched. It cannot treat queued energy as cooled.

## Thermal grant and exhaustion tail

The solver considers every active thermal model and both nodes. For the
existing positive cascade model, let `a = Rc/tauc`, `b = Rm/taum`, and
`k = max(a,b)`. An arbitrary nonnegative energy burst E raises the future
magnet by at most bE and the future coil by at most kE. This follows from the
positive impulse response: the coil is a lag of the magnet plus its direct
input. The envelope `(kE,bE)` is nonincreasing under zero added power.

For an indefinite safe tail of power Ps, a sufficient state envelope is
`M = max(Tm, Ta + Ps*Rm)` and `C = max(Tc, M + Ps*Rc)`.
Reserve all unresolved queued joules, all still spendable old credit and all
downstream/filter residual energy before issuing any new credit. Both
`C + kE` and `M + bE` must remain below the applicable ceiling, with numerical
margin. This envelope includes interior extrema and an infinite tail; it does
not assume that a quiet observed prefix predicts quiet upcoming audio.

The initial solver is sufficient but potentially conservative. No listening
claim follows from its proof alone. A tighter solver may use the existing
closed-form model and immutable queue schedule, but must preserve the entire
tail and outstanding debt, including completions that arrive late.

Only completed, authenticated output may advance the model's accounted
boundary. Measured V/I is feedback over matching intervals and may tighten the
upper state; it cannot erase reserved future heat or count it twice. Partial
control writes, stale snapshots and lost telemetry issue no new grant.

## Per-device facts and clean room

DT fixes physical topology and slot selection; packaged daemon profiles fix
thermal models, voltage ceilings and valid loads. Codec maximum analog/digital
gain, downmix, HPF and any firmware/EQ gain and residual tail are part of the
qualification, not guessed from recent sense data. TAS2764/TAS2770 volume locks
and maximum-gain restrictions continue to apply. A bounded PCM energy before
an unbounded downstream transform is not a physical energy bound.

Implementation sources are public GPL Linux and MIT speakerguardd. Only
architectural facts from the private macOS facts document inform this design;
no Apple implementation or tuning file is copied or published.

TI's [TAS2764 datasheet](https://www.ti.com/lit/ds/symlink/tas2764.pdf)
(section 8.4.2) and [TAS2770 datasheet](https://www.ti.com/lit/ds/symlink/tas2770.pdf)
(section 8.4.3) describe a playback HPF and separate digital volume and amplifier
output-level controls. This supports including those stages in the contract;
a listed cutoff frequency by itself is insufficient to specify a transient
energy/tail bound. The prototype deliberately supplies no guessed filter
coefficients or firmware gain. This is a qualification requirement, not a
claim that those filters amplify energy or that the completed gate is infeasible.

## Required acceptance on the integrated source

* KUnit: rounding, extremes, shared slots, exhausted/absent/expired grants,
  replayed/stale renewals, epoch changes, overflow and queued debt.
* DMA boundary tests: mutable mmap source, repeated cyclic period, active plus
  queued descriptors, pool exhaustion, delayed completions, missing reports,
  partial submission, failed stop and unbind. Prove no private buffer reuse
  while hardware can still read it.
* The exact 30,144 continuous cases, 865 retained failures, 17 crossings,
  historical 72 shared rows and every-profile strict/onset matrices, with the
  actual gate model interposed at submission. Both nodes and all models stay
  within limits; existing assertion oracles remain intact.
* All 63 retained replays and quiet schedules, same normalization and reference:
  at most 1 dB extra attenuation; exactly 0 dB on J616 at 30% and 60%. Include
  the kernel's sample attenuation, not only daemon volume-control writes.
* Daemon crash before/after renewal, stalls spanning many periods and expiry,
  owner replacement, gain/rate/route changes, restarts and suspend.

No deployment or speaker playback is authorized. The orchestrator files an
independent review after the implementation and its evidence are available.
