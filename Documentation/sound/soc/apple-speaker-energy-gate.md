> Historical prototype design and validation, carried from public Linux #495.
> This branch's new Neo scope and current qualification limits are in
> apple-neo-energy-grant.rst. Earlier test counts below are not new results.

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

Counters use Q20 normalized frames at a fixed reference rate of 48000 Hz;
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

## Version 2 units and residual state

The unshipped ABI is now version 2 with Q20 normalized units. Ten years of
continuous full-scale input fit in the lifetime counters; Q30 could naturally
saturate within days. Per-sample billing still rounds upward and grants down.

The final 24 bytes carry immutable per-speaker `tail_us` bounds: residual
energy expressed as full-input-power-equivalent microseconds. Every grant,
power window and temperature projection reserves that debt. A qualified bound
must cover arbitrary prior input and allowed route/rate/gain/clock transitions,
including DSP state populated by safe-cached raw data before DVC. Ticket refunds
cannot erase that state. Unknown tails remain unqualified; the actual TAS
helpers still return NONE. No coefficients or bounds are guessed.

## Required acceptance

KUnit exercises the ledger, payload ownership and actual macaudio control/trigger
handlers. A host wrapper of the same C core and payload helper supports immutable
queue simulations; it has no device access and cannot qualify a real transfer.
The 30,144 retained continuous cases, historical rows, 63 unchanged listening
replays and crash/stall/expiry tests must pass on the integrated source. Listening
metrics must include sample attenuation as well as control gain: <=1 dB and
exactly 0 dB on J616 at 30%/60%. No old test is waived or its limit raised.

The software validation described here used no hardware, speaker playback,
recording or deployment. Physical qualification requires its own approved
measurement procedure; no physical certificate is claimed by these results.
The orchestrator files the independent review after implementation and evidence.

## Current validation and blocking work

The runtime draft includes the ADMAC descriptor adapter, version 2 grant ABI,
TAS2764/TAS2770 state read-back, macaudio owner/lease integration and production
thermal-model grant issuance. Qualified reference paths use the exact shared C
ledger and immutable payload helper. Actual TAS helpers still return transfer
NONE, so new-kernel speaker paths retain safe hardware caps. No real transfer
has been qualified and no new device is authorized for louder output.

Buildbox validation, using normal resource admission and no devices:

* **31/31 KUnit tests pass** (18 ledger, 4 payload ownership, 9 actual macaudio
  control/trigger tests). The stale Q30 integer-boundary expectation in an
  earlier run is retained and corrected for Q20; no safety assertion is removed.
  Live owner close is tested in both ALSA control-release orders with queued debt.
* **ARM64 C object compilation passes** for ADMAC, macaudio, the energy ledger
  and both TAS codecs. This is not a complete bootable-kernel or AOP Rust build.
* The **30,144-case continuous campaign passes all 221,844 thermal rows** with
  the version 2 gate/model. **14,251 cases use the declared reference gate;
  15,893 exercise conservative fallback.** All 865 retained counterexamples
  pass too (189 reference-gated / 676 fallback). Input identities, thermal
  limits and continuous assertion oracles are unchanged. Reference certificates
  do not establish physical transfer bounds.
* The separately retained **72 shared historical rows pass** under conservative
  fallback, with **140 total integrated peak rows and zero over-limit rows**.
  These reporting-only fixtures use the current daemon plus frozen diagnostic
  exporters and the retained independent thermal integrator; they are not a
  gate-qualified transfer run. The two different-source packaged-Neo archive
  rows remain explicitly unmatched and are not counted as reruns.
* The v22 standard suite reported **321 passed, 1 failed, 1 ignored**. The
  separately run 30,144-case campaign is the ignored extended test. Twenty of
  the original 21 failures now pass, using explicit reference-contract fixtures
  where the test requires grant authority and independent gain/word oracles for
  missing time. The remaining ordinary-listening assertion is still failing.
* Queue/restart, owner-death/crash, stalls, expiry, failed-stop, readiness and
  source-fallback simulations pass. These do not establish hardware behaviour.

The unchanged 1 dB quiet-programme assertion now includes actual native-word
RMS attenuation in the C gate as well as analogue controls. With the same
bounded-current reference certificate as the thermal campaign, the largest
simulated mean losses are **41.708 dB on J316 at 60%**, **79.794 dB on J413 at
60%**, and **1.544 dB on J413 tweeters at 30%**. The failure is retained, not
waived. A different certificate cannot be selected only for listening.

A complete approved downstream gain/filter-state/clock-tail bound and a
justified pre-DAC input mapping are missing. Post-amplifier V/I records do not
uniquely determine queued PCM through an unknown HPF or clipping path. Therefore
no valid gate-in-loop 63-replay acceptance or J616 zero-loss result is claimed.
The useful daemon flags remain; unqualified profiles are not promoted.

The safe-and-loud objective remains incomplete. Both PRs stay draft. Independent
review is for the orchestrator to file. **Do not merge or deploy.** Those
software checks used no hardware, playback (including silence), microphone
recording or deployment.

## J616 coverage and reference-envelope feasibility

The latest standard suite, v29, reports **321 passed, 2 failed, 1 ignored**.
It adds `j616_ordinary_quiet_programme_has_zero_extra_attenuation` using the same
600-second programme, cubic UI scaling, 477 measured active seconds and native
queued-word RMS meter as the existing J316/J413 check. J616 requires exactly
zero additional attenuation at both 30% and 60%. No runtime policy, input level,
thermal limit or existing assertion changed. The extended continuous test was
executed separately in v21; it is not a skipped acceptance waiver.

The synthetic J616 30% check returns exactly 0 dB for every speaker. At 60%,
woofers lose 16.521 dB on average; the tweeter metric is infinite because some
active 10 ms cells contain zero delivered native words. This is a failed
synthetic test, not a successful 63-recording replay or a physical measurement.
The existing J316/J413 assertion fails too. All other 321 standard checks pass.

A necessary energy bound explains why tuning the limiter alone cannot make the
current **linear reference accounting envelope** meet these budgets. In one
constant-input metric cell let `u` be digital gain in [0,1], `c >= 0` analogue
dB attenuation, `q = mean(u^2)`, `d = -10 log10(q)` and `k = ln(10)/20`.
Weighted Jensen and `1-exp(-x) <= x` give

```
mean(u exp(-k c)) >= q exp(-k mean(c)/q)
                 >= 1 - 2 k (d + mean(c)).
```

This permits arbitrary clipping/pulsing and correlation between digital and
analogue attenuation inside a metric cell. Summing cells gives an optimistic
minimum linear billed dose for any result within the measured mean dB budget.
Native input quantization is allowed for by rounding the input dose downward.
No queue debt, residual tail, numerical margin or safe-baseline heat is added.

Independently, integrating the two-node model over `H` seconds and eliminating
the magnet-temperature integral gives

```
(Rc+Rm) integral(P) = tauc Delta(Tc) + taum Delta(Tm) + integral(Tc-Ta)
                  <= (tauc+taum+H) (L-Ta).
```

The upper bound assumes both nodes start at least at ambient and remain below
hard limit `L`; it even grants a completely cold start after the excluded first
minute. It uses the hard 150 C limit rather than the stricter working ceiling.
For the measured 540-second span of the unchanged programme:

| Profile at 60% | Budget | Minimum linear-envelope dose | Maximum model dose | Necessary mean loss |
| --- | ---: | ---: | ---: | ---: |
| J413 tweeter, V/I model | 1 dB | 370.481 J | 200.672 J | at least 1.541 dB |
| J616 tweeter, V/I model | 0 dB | 795.768 J | 646.905 J | at least 0.408 dB |

These are deliberately loose necessary bounds on the reference envelope, not
predictions of actual loudspeaker dissipation. A squared/other tighter transfer
may be justified by complete public path/load facts, but must then be validated
consistently across thermal and listening campaigns. Neither these inequalities
nor a selector on a related TI part qualifies TAS2764/TAS2770/AOP. Real transfer
helpers continue to return NONE. The missing physical contract and pre-DAC
replay inputs remain blockers; both PRs remain draft.

## Physical transfer qualification

For each actual route and speaker, establish a bound of the form

```
E_output(x, initial_state, epochs)
    <= K * sum(native_normalized_PCM^2) / sample_rate + B(initial_state, epochs)
```

or a justified alternative norm supported by the grant accounting. Units,
per-speaker transforms and the relation to both thermal power inputs must be
explicit. Account for sense scale/quantization/error, gain calibration, channel
cross-coupling, rate/format conversion, speaker/load variation, clipping and all
supported fixed DSP modes. A small-signal fit at one gain is not that bound.
Keep reference experiments distinct from physical certificates.

The residual term must cover arbitrary prior full-scale input and every allowed
gain/reset/clock transition. An observed decay below the sense noise floor
does not establish zero remaining energy. Obtain a justified stable-state norm
and decay/tail bound, or enforce a proven reset/quiescence sequence before
authority can rise. That sequence itself needs an independent safe envelope.
Neither descriptor retirement nor a refunded safe copy proves DSP quiescence.
Do not disable the HPF or inject clock/reset faults into a live speaker test to
replace the missing argument. Keep the independently reserved exhaustion tail.

Topology must be checked first. In the pinned public DT, both J316 and J413
speaker links use SN012776/TAS2764-class codecs over MCA. Their AOP microphone
device is not evidence for an AOP speaker transform. A result from these machines
cannot qualify TAS2770, another chassis, or the AOP speaker route. Unsupported
routes must retain transfer NONE and conservative protection.

## Replay provenance and acceptance

Sense-only recorder formats contain post-amplifier V/I. Format 4's receipts and
grants also do not contain native queued playback words. Retain corresponding
pre-DAC payloads with format, frame/descriptor boundaries, per-speaker slot map,
clock/gain/route epochs, applied controls and a synchronized electrical trace.
Alternatively justify a bounded inverse, including initial state, lost/clipped
samples, quantization and filtering. Evaluate every admissible inverse input;
choosing a convenient representative does not prove a listening guarantee.

Once a physical bound and replay mapping exist, use the same certificate for
the full continuous, counterexample, historical, listening and fault campaigns.
Include queued-word and analogue attenuation in the unchanged metric. Retain
all 63 original matrix rows, <=1 dB general cost and exactly zero J616 cost at
30%/60%. Repeat crash/stall/expired-grant tests in simulation without stopping
hardware protection. A finite transfer probe alone cannot close any of these
acceptance gates. Keep the PRs draft until all required evidence passes and the
orchestrator's independent review is complete.

## Residual and epoch proof obligations

The [transfer certificate argument](apple-speaker-transfer-certificate.md) gives the storage-energy
inequality required to cover future input and arbitrary downstream state,
including their cross term. It also specifies paid or nonincreasing epoch
transitions and a set-valued alternative to a unique pre-DAC replay inverse.
The exact synthetic examples do not qualify any physical route or change
production protection.


## Speaker admission is independent of the legacy unsafe parameter

A DT Speaker/Speakers link is admitted only for an enabled, supported amplifier
and speaker layout with a negative configured safe volume. Generic fallback
and unsupported SSM3515 backends remain disabled even if the legacy unsafe
parameter is set. Protection setup failures abort registration and fixed
transform controls remain read-only. SPKR_NONE is always refused when a speaker
backend reaches control fixup. No unsafe parameter value can skip those gates.
The small admission hardening change is also published against aurora-wip so
it can be reviewed and merged independently of the energy-gate draft.
