Neo speaker energy grants: review draft
======================================

Status and scope
----------------

This series ports the public playback-ledger prototype (Linux PR #495,
``d3c3cdd1dc9d3444c63e74ec7d12c590336706c8``) to aurora-wip and adds the Neo's
immutable LEAP payload format. The earlier ledger, private-copy adapter,
control ownership and takeover rules are retained. Current machine admission,
fixed codec controls, PM handling, speaker identification and ADMAC runtime PM
are preserved from aurora-wip.

**This is not a qualified loudness fix.** No complete uniform Neo downstream
transfer certificate is available in this source. Its transfer helper returns
NONE, so the daemon retains its existing floor and onset reserve. There is no
module parameter, profile option or userspace assertion that can manufacture a
certificate. The same unqualified fallback applies to TAS paths. Qualification is checked before exposing the card. An unqualified PCM prepare
cannot perform codec I/O or take the volume mutex on behalf of the optional
contract, preserving the timing of the legacy fallback. An enforced contract
cannot revert to legacy mode within the device's lifetime.

The independent safety review and lead-owned measurements must precede any
activation or deployment. The sound card's 250 ms volume lock, owner checks,
safe-level fallback, failed-lock stop, prepare epochs and pre-freeze revoke
remain effective. No protection service or fallback is disabled.

Why the Neo needs another format
-------------------------------

Its userspace PCM is S32_LE, but ``aop_t8140.rs`` converts its samples into IEEE
binary32 words before ADMAC. A float magnitude of 0.5 corresponds to full scale
on the speaker wire. Reading those bits as S32 or ordinary FLOAT_LE would bill
the wrong energy. The experimental ABI therefore identifies this payload as
``MACAUDIO_ENERGY_FORMAT_NEO_F32`` (0x1000000e), separately from all ALSA PCM
formats. This field describes the private DMA payload, not the userspace PCM.

The decoder uses integer operations only. It accepts exactly representable
signed-Q31 amplitudes after normalization by two, rejecting nonfinite,
out-of-range and fractional-Q31 words. The producer's integer conversion fits
that domain. Encoding truncates magnitude toward zero, then bills the exact
emitted word with upward rounding. Negative full scale is exactly -0.5; the
unrepresentable positive Q31 endpoint +0.5 is refused. Signed zero emits zero.

A Neo copy-time gain is not an independent hardware cap on old queued words.
Consequently Neo payloads are never cached as unbilled raw data or refunded
because a later volume control was safe. Expired/exhausted Neo copies mute.
Every repeated DMA-ring period is copied privately and charged again before a
finite descriptor is published. Userspace cannot change a published copy.

Conditional safety theorem
--------------------------

The theorem requires a certified complete output path, a bounding thermal
model and a correctly functioning DMA boundary. A digital tap or a finite
frequency sweep alone establishes none of those universal premises.

For the positive two-node model, write::

  M' = (Ta - M + Rm P) / taum
  C' = (M - C + Rc P) / tauc
  b = Rm / taum
  k = max(Rc / tauc, b)

For indefinite safe power Ps, the rectangle with upper bounds
``M0=max(M,Ta+Ps*Rm)`` and ``C0=max(C,M0+Ps*Rc)`` is invariant. A nonnegative
impulse E raises M by bE and C by (Rc/tauc)E. Afterwards C cannot exceed the
larger of those initial increments, so arbitrary nonnegative energy E raises
future M by at most bE and future C by at most kE. Superposition establishes
the same statement for any placement or waveform of that energy.

Before issuing a grant, the daemon requires, for every applicable model::

  M0 + b E < ceiling - numerical_margin
  C0 + k E < ceiling - numerical_margin

E includes all uncompleted paid copies, all remaining authorized credit,
completion uncertainty and the certified downstream residual tail. The joint
certificate must cover background heating and unbilled safe output within the
single Ps reservation, including arbitrary prior state and transitions. The
existing power-window limits also constrain each grant.

The kernel maintains cumulative submitted S and absolute ceiling G, with
``S <= G``. Metering and granting serialize under the same IRQ-safe lock.
Another receipt cannot turn G into an additive top-up; stale sequences revoke.
A descriptor is published only after its immutable samples have been bounded
and its ticket reserved. A queued ticket is never forgiven on owner death,
expiry, suspend, an ordinary stop or an uncertain completion. Failed stop
quarantines the storage. A proven safe hardware epoch may refund TAS copies;
copy-time Neo gain can never justify that refund.

If the daemon dies at any instant, its last transaction either committed in
full or did not commit. All previously published samples are already included
in E. All subsequent samples consume at most G-S; after exhaustion or expiry
they mute on the Neo. Even a stopped process retaining its file descriptors
cannot replenish energy. Refill scheduling delays do not create fresh credit.
Thus arbitrary queue delay, death inside a grant transaction, and death on
either side of a descriptor boundary cannot violate the conditional bound.
The existing lock timeout additionally returns the volume control to its safe
level. This software argument does not certify AOP firmware behavior, DMA
hardware faults or the real amplifier transfer.

Tests and remaining qualification
---------------------------------

KUnit covers the real ledger, immutable payload ownership and macaudio control
handlers. Neo additions exercise the half-scale convention, malformed payloads,
all allowances 0..4095 for both signs, queued-dose retention and muted refills
on death. The daemon tests execute the same pinned C ledger and private-copy
helper, including death/retained-FD cases at every selected descriptor boundary.
These are software tests; their reference transfers cannot qualify a device.

Required next evidence includes a uniform gain/energy and residual-state bound
from DMA payload through AOP/LEAP and the amplifier, across allowed clocks,
routes, starts, stops and arbitrary prior state. The supplied implementation
keeps NONE until that work exists. Legacy listening failures are neither waived
nor changed. See the companion daemon's ``docs/neo-energy-grant.md`` for the
lead's measurement and rollback plan.
