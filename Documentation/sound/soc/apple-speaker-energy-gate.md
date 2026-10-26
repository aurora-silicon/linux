# Playback energy gate design — integrated review draft

The sense PCM reports past output. This design limits future output by spending
finite per-speaker energy on private PCM copies at the ADMAC descriptor boundary.
The daemon issues grants from both thermal models and retains the V/I guard.
Unqualified routes and kernels without the contract keep conservative protection.
This is a draft, with qualification and acceptance work still outstanding.

## Descriptor boundary and ownership

ADMAC copies each submitted period into coherent storage, runs the gate, orders
those writes, then publishes the finite hardware descriptor. The writable mmap
ring is never its source after publication. Every cyclic repeat spends again.
Eight private slots cover the four-entry FIFO, an active descriptor and report
lag. A slot is retained until its FIFO completion token retires exactly once.
Pool, report and accounting errors stop submission. A failed stop quarantines
the storage and channel rather than recycling DMA memory.

Macaudio prepares both playback front ends through the DMAengine adapter. Live
speaker/headphone mux and pin changes are excluded while any playback FE is
open. Shutdown must prove ADMAC quiescence even for an unfiltered headphone FE;
a failed or unsupported proof retains route exclusion and faults protection.
Restart requires prepare. Suspend revokes the grant before userspace freezes.
The AOP speaker path can use its existing DMAengine runtime, but firmware and
codec transforms require their own qualification; none is inferred from MCA.

## Existing lock, grant ABI and epochs

The optional `Speaker Energy Grant` ALSA BYTES control has a fixed, versioned
512-byte little-endian layout. The same control file must own all volume and
amp-gain controls, `Speaker Volume Unlock`, and the grant control. No independent
unlock mechanism, module parameter or boot flag is added. DT supplies topology;
packaged profiles supply card identity, model constants and physical slot order.
J616's playback order differs from its profile order and is explicitly mapped.

A receipt contains the boottime epoch, grant sequence, format/rate/map, transfer
identifier, lifetime submitted/completed and source counters, safe refunds,
unbudgeted raw-prefix debt, absolute ceilings and per-speaker Q31 gains.
Owner/route/format changes cannot reuse permission. Ordinary DMA traffic does
not change the grant sequence: concurrent spending against the same absolute
ceiling cannot add credit. Grants and revocations change the nonce, so replay
fails. Invalid requests revoke; no arithmetic overflow becomes fresh credit.

A grant renews the existing volume lease/work, capped at an absolute deadline
no more than 250 ms away. Idle and suspend do not pause an energy deadline.
The old magic ping cannot extend it. Absent, expired or exhausted credit yields
the safe-level envelope. Already submitted copies retain their full heat debt;
completion or a proven stop retires them conservatively as possibly played.

While all hardware caps are proved safe, private copies may retain raw PCM.
Before raising any cap a grant must pay all still-pending raw copies atomically.
If unaffordable, admission installs protected-copy mode and returns `EAGAIN`
without unlocking; new protected copies replace the raw prefix as it drains.
A safe-epoch marker refunds a charged copy only if its entire lifetime was
provably below the safe hardware cap. Possible partial playback is never refunded.

## Energy and thermal proof

Counters use Q30 normalized frames at a fixed reference rate of 48000 Hz;
rate epochs never reinterpret lifetime totals. S16_LE, S24_LE-in-32 and S32_LE
are normalized separately; output quantization goes toward zero before billing.
Transfer 1 meters squared amplitude and needs a complete certified energy norm.
Transfer 2 meters absolute amplitude and needs certified voltage/current bounds;
`|V| Imax` then covers arbitrary V/I phase. Source counters always meter squared
raw PCM for the existing source power budgets. These transfer identifiers are
contracts, not assertions that a particular amplifier has been qualified.

The production model charges integer differences `completed - refunded` once,
placing each uncertain increment at the latest possible time. Submitted but
uncompleted energy remains future heat debt. V/I has a separate observer that
may tighten the projection and retains the existing fault and ramp checks;
it cannot forgive queued energy or add the same output heat twice. Existing
state restore and clean-save gates remain in force; an active contract cannot
be silently dropped in favour of a less informed model.

For each positive two-node model, set `b = Rm/taum` and
`k = max(Rc/tauc,b)`. Any nonnegative dose E has a future magnet increment at
most bE and a future coil increment at most kE. With indefinite safe power Ps,
use `M = max(Tm,Ta+Ps*Rm)` and `C = max(Tc,M+Ps*Rc)`. Both `M+bE` and `C+kE`
must remain below their working ceilings with numerical margin. Pending energy,
remaining credit and every downstream residual tail must fit inside that dose.
The daemon replaces absolute ceilings and rounds additional credit downward.
It checks both nodes/models and delivered-energy windows before issuing credit.
This sufficient envelope is conservative; it does not establish a loudness pass.

## Transfer qualification and fallback

TAS2764/TAS2770 helpers read back output gain, channel source, slots and HPF state.
They deliberately return no transfer certificate. The public TI datasheets give
gain equations and cutoff selectors, but do not specify the complete transient
energy norm and residual filter-state bound needed here:
[TAS2764, section 8.4.2](https://www.ti.com/lit/ds/symlink/tas2764.pdf) and
[TAS2770, section 8.4.3](https://www.ti.com/lit/ds/symlink/tas2770.pdf).
A cutoff alone is insufficient. No coefficients, firmware gains or speaker
constants are invented. Consequently this draft cannot authorize louder real
TAS output until an approved, complete bound is supplied and implemented.
Unknown profiles retain the existing conservative daemon/kernel behaviour.

Implementation uses public Linux sources and newly authored code. The shared
ledger/payload helpers and host adapter are dual GPL-2.0-only OR MIT; no existing
Linux driver or test was relicensed. No Apple implementation or tuning file,
recording, credential or private lab record is copied into public source.

## Required acceptance

KUnit exercises the ledger, payload ownership and actual macaudio control/trigger
handlers. A host wrapper of the same C core and payload helper supports immutable
queue simulations; it has no device access and cannot qualify a real transfer.
The 30,144 retained continuous cases, historical rows, 63 unchanged listening
replays and crash/stall/expiry tests must pass on the integrated source. Listening
metrics must include sample attenuation as well as control gain: <=1 dB and
exactly 0 dB on J616 at 30%/60%. No old test is waived or its limit raised.

No hardware, speaker playback, recording or deployment is part of this work.
The orchestrator files the independent review after implementation and evidence.
