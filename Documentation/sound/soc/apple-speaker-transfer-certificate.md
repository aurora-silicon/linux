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
w[n] + V_e(z[n+1]) - V_e(z[n]) <= (P0 + F_certificate * |x[n]|^p) / fs
V_e(z_at_admission) <= B
```

Here `p=2` corresponds to transfer 1, `p=1` to transfer 2, and F_certificate is **no larger
than** the full-input power the daemon actually charges for that speaker.
If a derived transfer norm needs a larger F, it does not fit this contract:
neither the `tail_us` field nor an unchanged amplifier gain can hide an
input-proportional deficit. The certificate must cover both thermal-model
inputs and the actual allowed load, temperature, supply and gain range.
The nonnegative continuous background P0, including idle noise, shares the
single safe-power reservation Ps with all unbilled signal energy. Let
`g_fallback` bound the actual free-PCM gain (including safe raw copies and
hardware gain). At every permitted rate/gain/route/clock epoch require the
**joint** bound

```
P0 + F_certificate * g_fallback^p <= Ps
```

For a stateless plant this is necessary as well as sufficient for that
baseline. Separate premises `P0 <= Ps` and
`F_certificate * g_fallback^p <= Ps` spend Ps twice and are invalid.
For stateful paths use the same joint bound in the time-local storage
inequality, with a uniform B across every reachable boundary and epoch.
Free PCM includes clamped/exhausted and hardware-safe refunded descriptors;
raw copies must use their actual downstream gain, not a guessed sample scale.
Paid PCM is charged in full (not just its amount above fallback); its signal
supply is billed and P0 fits inside the same Ps reservation. No grant, expiry,
owner death or stopped-clock interval creates additional free background.
Every continuous/unclocked contribution needs this wall-time bound or a
separately charged reserve. A deficit cannot be charged to a finite tail.

F_certificate may be smaller than the daemon's charged full-input power F,
but that gap must come from a proved tighter physical norm. The existing
ABI has no independent background field and cannot negotiate extra Ps.
If the joint bound cannot be proved for every applicable model and transition,
the helper must return NONE. For example P0=Ps is incompatible with any
nonzero unbilled signal unless its certified coefficient is zero. The exact
checker exercises nonzero background, both p values, positive-headroom
examples and the old double-spend counterexample. It qualifies no hardware.

Summing the inequality over any finite continuation gives

```
sum(w) <= P0 * N/fs + F_certificate/fs * sum(|x|^p) + V_e(z_start)
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

## From storage to thermal state and rolling windows

A whole-recording energy norm is insufficient for a thermal model: energy
stored while the input model cools can be released later. The certificate
needs the time-local supply inequality above and a **uniform reachable-state
bound** `0 <= V[n] <= B` at every physical boundary. Bounding storage just at
the first admission is insufficient. The requirement at every possible suffix
implies this uniform bound, including the safe-cached prefix and exhaustion.

Let `u[n]` be billed energy plus the single joint Ps baseline for frame n,
and let `h[n] >= 0` be any finite forward-time weights. Multiplication of
`w[n]-u[n] <= V[n]-V[n+1]` and summation by parts gives

```
sum(h[n]*(w[n]-u[n]))
 <= h[0]*V[0] + sum((h[n]-h[n-1])*V[n]) - h[N-1]*V[N]
 <= B * (h[0] + sum(max(0, h[n]-h[n-1])))
```

The interior sums start at n=1. This inequality allows signed differences
`w-u`; neither physical energy is negative. It explains what one persistent
residual reservation covers, without repeatedly integrating B as new heat.
For arbitrary weighting kernels the positive variation factor must be kept;
a peak-only factor is not universally valid.

For the two-node thermal model, write `a=Rc/tauc`, `b=Rm/taum`. The magnet
impulse kernel is `b*exp(-t/taum)`. The coil kernel starts at a and solves
`hc'=(hm-hc)/tauc`. It is nonnegative, bounded by `k=max(a,b)` and either
monotonically decreases or rises to one maximum then decreases. For unequal
time constants it is a sum of two decaying exponentials and its derivative
has at most one zero; a nonnegative kernel tending to zero cannot have a
strict interior minimum. For equal time constants it is
`(a+b*t/tauc)*exp(-t/tauc)`, with the same property. Thus their time-reversed
weights have positive variation factors at most b and k respectively.
The continuous integration-by-parts version gives the same result for
energy measures, including impulses.

Consequently, compared with the same thermal model driven by billed energy
and safe background, all historic storage delay/release adds at most `b*B`
to the magnet and `k*B` to the coil at **any** evaluation time. Reserving B in
every thermal projection covers that excess even after a prior residual has
been released. Queued charged energy remains separate debt; retirement,
refunds and a new grant do not reset storage. An impulse envelope that already
bounds billed energy can retain this additive residual bound.

For a rolling energy window use weights equal to one inside the window and
zero outside. There is one positive jump, so actual window energy exceeds
billed-plus-safe window energy by at most B, not one B per grant. Reserve B
in each independently enforced power window. This statement does not permit
dropping the billed energy already in that window, including pre-takeover
history. Source-power windows still need their own authenticated raw history.

All epochs must share this uniform storage interpretation. Nonincreasing
storage jumps preserve the proof; each positive jump must instead be paid
as energy before it can occur. Unbounded unpaid jumps, a new owner nonce or
reinitializing the model break the argument. Changed thermal parameters,
per-speaker slot identities or joule conversion need a fresh composition
proof; the current lifetime ledger cannot silently reinterpret them.

The exact checker verifies the summation identity using its attainable
extremal storage assignment, 450 finite positive two-node kernel examples
(including repeated poles and both pole orderings), 152 rolling windows and
16 paid store/release cycles. These finite examples accompany the analytic
argument; they do not identify or qualify any physical filter or load.

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

For the exact, noiseless illustrative high-pass above, a complete inverse
class can be written explicitly: `x[n]=s0+y[n]+(1-a)*sum(y[:n])`. Intersect
`-1 <= x[n] <= 1` with the independently justified initial-state interval.
An empty intersection rejects the mapping. The queued squared dose is a
convex quadratic in s0: its minimum is at the clipped vertex and maximum
at an interval endpoint. The checker validates that entire example class,
including legal endpoint trajectories and an infeasible observation.
This illustrates a set-valued mapping without selecting a convenient input.
Actual quantization, clipping, missing periods, gain/calibration uncertainty
and mode changes require additional variables and constraints; these exact
example intervals cannot be applied to the original recordings. Dose bounds
alone also do not determine the dynamic listening metric: the counterfactual
gate, grants and analogue controls must be evaluated for all admitted
trajectories with the original timing and measurement windows.

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
