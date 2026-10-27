# Physical transfer certificate: residual state, epochs and replay inputs

This is a proof obligation for the playback-grant draft, not a physical
certificate. No TAS2764, TAS2770 or AOP route is enabled by this document.
The examples below are newly authored mathematical systems, not models of
those devices. Keep the existing safe caps until all obligations are met.

## A bound that composes with the ledger

Let `x[n]` be the **delivered private-copy words**, normalized by their actual
PCM format. For each speaker, x must be its actually billed slot; a norm on
all channels combined does not justify independent per-slot charging. Include
channel selection, duplication, cross-coupling, rate conversion and every
transform after that boundary. Let `w[n] >= 0` upper-bound the energy supplied
to each active thermal model during frame n. Define a nonnegative storage
function `V_e(z)` in joules for all downstream state z in epoch e. A sufficient
certificate is

```
w[n] + V_e(z[n+1]) - V_e(z[n]) <= (P0 + F * |x[n]|^p) / fs
V_e(z_at_admission) <= B
```

Here `p=2` corresponds to transfer 1, `p=1` to transfer 2, and F is **no larger
than** the full-input power the daemon actually charges for that speaker.
If a derived transfer norm needs a larger F, it does not fit this contract:
neither the `tail_us` field nor an unchanged amplifier gain can hide an
input-proportional deficit. The certificate must cover both thermal-model
inputs and the actual allowed load, temperature, supply and gain range.
The nonnegative continuous background P0, including idle noise, must not
exceed the safe power the daemon already integrates indefinitely. A larger
background cannot be charged to a finite residual tail. The exact checker
uses P0=0 only for its noiseless mathematical examples.

Summing the inequality over any finite continuation gives

```
sum(w) <= P0 * N/fs + F/fs * sum(|x|^p) + V_e(z_start)
```

after dropping the nonnegative final storage. It also covers an infinite
continuation when the relevant sums converge. The fixed ABI tail must satisfy
`F * tail_us / 1e6 >= B`, rounded upward, with no u32 overflow. This is an
energy reserve, not a decay timeout. Finite norm plus finite residual is
required; an arbitrary frame-rate conversion cannot reuse a tail expressed
in frames at the old rate. Unknown or unrepresentable bounds mean NONE.

The bound must apply to every suffix used for regranting, not just to a whole
recording from a known-zero initial state. Safe-cached raw words can populate
state before a downstream volume control. A safe-ticket refund, completed
descriptor, closed file or new owner does not reset that state. Keep the
universal residual reservation on every projection; preserve already-issued
energy debt separately. Do not repeatedly integrate the same fixed residual
as measured heat, or subtract it from lifetime counters as a refund.

## The cross term cannot be discarded

Separately proving a zero-state induced norm and a zero-input residual norm
does not automatically prove their sum with the same constants. If output
is `y_new + y_old`, its squared norm includes a cross term. Even the single
sample `y_new=y_old=1` has energy 4 rather than 2. One sufficient conversion is

```
||y||^2 <= (1+epsilon)*gamma^2*||x||^2 + (1+1/epsilon)*B0
```

for epsilon > 0. Include both factors, or supply the storage inequality above
directly. In particular, a gain-one zero-state bound plus a measured decay
integral is not yet a transfer-1 certificate with that same gain.

For a state-space realization `z'=Az+Bx`, `y=Cz+Dx`, a sufficient squared-norm
storage proof uses `V=z'Pz`, P positive semidefinite, and the block matrix

```
[ A'PA-P+C'C       A'PB+C'D      ] <= 0
[ B'PA+D'C        B'PB+D'D-g^2 I ]
```

where prime means transpose. Units and conversion to thermal power must be
included. Validate inequalities over coefficient, arithmetic and calibration
uncertainty, rather than accepting a floating-point fit at nominal values.
Nonlinear clipping, limiter modes, noise and diagnostic injection require
their own dissipativity argument; a small-signal matrix does not cover them.

## Epoch transitions and exhaustion

An allowed state transition `z_new=R*z_old+d` needs a bound

```
V_new(z_new) - V_old(z_old) <= J_transition
```

including any output emitted during the transition. A common storage function
with nonincreasing transitions permits a fixed residual reserve. Otherwise
pay every positive jump before allowing the transition, or enforce a proven
safe quiescence/reset boundary and re-admit from a bounded state. Merely
changing a receipt nonce cannot pay physical energy. A multiplicative storage
jump needs a reachable-state bound before it can become a fixed joule debit.
Unlimited transitions with positive unpaid jumps do not fit one fixed tail.

Enumerate owner close/reopen, gain changes and ramps, route changes, all
rate/format/clock modes, suspend/resume, reset, supply faults and failed stops.
For stopped or absent clocks, use a wall-time output bound or prove that output
and state evolution stop safely; sample-count decay alone is insufficient.
The safe-exhaustion envelope also covers control latency and state release
when no new descriptor is submitted. The independently safe baseline must
remain valid during that release. No grant, expiry or daemon death may turn
these obligations into an unbudgeted louder tail.

The current TAS helpers return NONE. An implementation enabling a certificate
must demonstrate these obligations at each real control/trigger boundary.
This document does not add transition debits or a certificate to the ABI.

## Why a sense-only inverse is not a unique playback programme

Even a known stable high-pass transfer can hide an input component if its
initial state is unknown. For the illustrative system

```
s[n+1] = a*s[n] + (1-a)*x[n],  0 <= a < 1
y[n] = x[n] - s[n]
```

the two trajectories `(x,s)` and `(x+d,s+d)` produce exactly the same y for
every n while both inputs can remain in [-1,1]. Their queued squared doses
differ. Clipping and quantization add ambiguity; current sensing does not
restore a component removed before the load. This example is not an assertion
about the order or coefficients of a TAS filter. It demonstrates why a cutoff
selector and a post-amplifier recording are insufficient evidence for a unique
inverse. The exact-rational checker in `speakerguardd/tools/check-transfer-contract.py`
verifies this witness and a storage inequality for this example only.

A justified mapping can instead describe a set of admissible input/state/
epoch trajectories consistent with every retained observation and error bound.
It must be nonempty, and listening acceptance must hold for **every** member
under the same physical certificate as thermal acceptance. Choosing the
minimum-energy inverse, removing DC, imposing zero initial state, filling a
loss with silence or aligning to a convenient gain is an additional assumption
unless independently justified. If the resulting interval for the listening
metric straddles the limit, the result is unresolved rather than a pass.

Alternatively retain native pre-DAC payloads, exact valid-bit/container format,
frame order, descriptor boundaries and repeat semantics, the physical speaker
slot map, all gain/route/rate epochs, the input scaling used for each replay,
and a synchronized electrical trace. Authenticate payload and manifest hashes.
Counter totals, raw receipts and grant packets alone lack the waveform needed
to replay counterfactual attenuation. A new recording cannot silently replace
one of the original seven inputs or its historical acceptance row.

Keep the original matrix: seven archives, three volume points (30%, 60%,
recorded), and three source variants. Preserve the original row identities,
baseline and comparator revisions. Integrate the new gate in the candidate
rows; do not turn the 63 runs into 63 unrelated programmes. Retain the original
playing-time, first-minute exclusion, per-speaker mean and pointwise checks,
and count private-word attenuation as well as analogue control attenuation.
J616 at 30%/60% requires zero change, including no positive boost. Losses,
early-end markers and unqualifiable inverse-gain periods remain explicit.

## Public facts and remaining physical evidence

The [TAS2764 datasheet, sections 8.4.2 and 8.5](https://www.ti.com/lit/ds/symlink/tas2764.pdf)
describes HPF selection, gain, volume ramps and shutdown. The
[TAS2770 datasheet, sections 8.4.3 and 8.4.7](https://www.ti.com/lit/ds/symlink/tas2770.pdf)
also describes HPF and mode controls. These facts do not specify a complete
state realization and transition-storage certificate for the actual routes.
Register defaults alone do not prove downstream state is zero. Qualification
must connect physical evidence to the certificate rather than infer it from
the mere presence of an HPF, mute, shutdown or reset control.

A finite protected probe can falsify a candidate transfer and estimate gain,
delay, channel mapping and observed decay. It cannot prove a universal bound
on unseen prior state or untested modes. Keep its electrical observations
separate from that proof and from the unchanged 63-replay acceptance.
