.. SPDX-License-Identifier: GPL-2.0-only

Apple speaker playback energy control
====================================

``Speaker Energy Grant`` is an optional 512-byte ALSA BYTES control. Its
little-endian layout is ``struct macaudio_energy_ctl`` in
``include/uapi/sound/apple-speaker.h``. It neither replaces nor independently
unlocks ``Speaker Volume Unlock``. Lock the volume and amplifier-gain controls,
then ``Speaker Volume Unlock``, then this control, through the same control
file. Closing either authority revokes credit and the existing volume lease.

Read flags distinguish an available interface from a configured, qualified
transfer. REQUIRED means that this kernel requires a grant even while a stream
is unprepared or the transfer is unqualified: do not fall back to the old magic
unlock in that state. New gate-capable TAS paths hold the existing hardware safe
caps until qualification and a complete grant. Kernels without this interface
retain the daemon's conservative policy. Unknown transfer identifiers, absent certificates, mismatched card
identities, formats or maps must keep conservative protection. Codec register
read-back of a gain or HPF selector is not itself a transient energy bound.
The TAS state queries currently provide read-back facts without inventing an
HPF certificate; they do not authorize higher output on that basis.

Units and receipts
------------------

Energy counters use Q20 normalized frames at the fixed reference rate of
48000 Hz. Transfer 1 meters squared amplitude and requires a certified
sample-energy-to-heat bound. Transfer 2 meters absolute amplitude; with a
certified voltage path and current ceiling it bounds arbitrary V/I phase by
``|V| * Imax``. It is more conservative than a resistive power estimate.
Source counters always meter squared amplitude for the source power budgets.
Version 2 uses Q20 units so ten years of continuous full-scale input fits in
u64 lifetime counters (v1's Q30 prototype could saturate in days). Each
submitted hardware sample is rounded upward in its declared units;
rate changes never reinterpret lifetime counters. Source counters describe
the immutable data before attenuation. Submitted/completed counters describe
reserved output energy. Refunded energy is a subset of completed energy that
provably remained below the hardware safe cap for its entire lifetime.
Charge a model with ``delta(completed) - delta(refunded)``, subtracting integers
before converting a delta to joules. Unresolved ``submitted - completed``
remains future debt regardless of delayed reports, pause or ownership changes.

Counters never reset on owner, rate or stream changes. Reconfiguration requires
all completion tickets retired. Saturation, missing ownership, failed stop or
inconsistent completions cannot be interpreted as an empty queue. A failed
stop quarantines DMA storage and the channel; it cannot recycle the memory.

An atomic read returns a boottime clock sample and a grant sequence. Ordinary
descriptor traffic does not change the grant sequence: an absolute ceiling
computed from a receipt remains an upper bound despite concurrent spending.
A grant or revocation changes that sequence. Replay, stale owner/route epochs
and malformed writes revoke permission. No caller may add a credit amount to
the current balance; write a new absolute cumulative ceiling instead.

Safe-level queued prefix
------------------------

While every amplifier is proved locked at its existing safe level, private
copies may retain raw PCM. They remain tagged unbudgeted and cannot cross into
a higher-gain epoch until a grant pays for their entire still-pending energy.
Grant admission disables raw caching before that test, under the ledger lock.
The existing hardware volume ranges are raised only after all such copies
have been charged. Thus quiet playback need not wait behind additional muted
periods when the first grant arrives.

If a raw prefix is unaffordable, the write installs protected-copy mode but
returns ``EAGAIN`` without raising the hardware caps. New copies are attenuated
in path while the old prefix drains under the hardware lock. Re-read the
receipt before retrying; do not replay the same sequence. A safe-epoch marker
allows retirement to refund a charged copy only if no amplifier range could
have exceeded the safe level at any time since it was submitted. A partially
played or possibly loud copy is charged in full, including on stop/discard.

Writes and expiry
-----------------

For a grant write, clear the read flags, retain the immutable identity fields,
and supply the read epoch/sequence, a ceiling per speaker, Q31 gains no greater
than unity (``0x80000000``), and an absolute boottime deadline. The deadline must
be future and no more than 250 ms away. The per-speaker ``tail_us`` fields are immutable qualified bounds, in
full-input-power-equivalent microseconds, and must match the receipt.
``MACAUDIO_ENERGY_REVOKE`` requests revocation without issuing a new grant.

A successful grant renews the same existing volume-unlock lease, capped by its
deadline. It cannot survive idle gaps or suspend. The old magic ping can renew
only within the grant's deadline; it cannot manufacture credit or extend it.
Absent, exhausted or expired credit applies the existing safe-level envelope
to new samples. An IRQ or daemon stall can emit only already reserved immutable
descriptors, whose full energy and downstream tail must remain reserved by the
model. Raising an amp range never repairs or rewrites a submitted DMA buffer.

Fixed speaker ASI selection, HPF and over-current retry controls are read-only
to userspace. ALSA INACTIVE alone is a UI hint, so it cannot enforce a transfer
certificate. This does not freeze the headphone codec's controls.

Route changes require all playback front ends closed. FE shutdown proves DMA
quiescence before releasing that exclusion. Speaker/headphone muxes and pin
switches cannot attach an already running raw front end to a speaker route.
Stop and suspend revoke credit; restart must prepare under the current epoch.

Readiness notifications
-----------------------

Prepare notifies the grant control after the protected descriptor path is
ready. Userspace must distinguish this wake-up from a sample-rate edge: the
rate notification can arrive before prepare. Every open FE must have the
protected path prepared for the current epoch before higher caps are allowed.
An open raw headphone FE therefore keeps speaker caps safe even if an automatic
jack/DAPM pin update could attach it to speakers. Unknown live source history
keeps the conservative full-scale programme-budget assumption.

Residual state
--------------

``tail_us`` bounds residual downstream energy for arbitrary prior full-scale
input, including raw data received while DVC was held safe and every allowed
route/rate/gain transition. It is not inferred from a cutoff or from silence.
The model reserves ``full_input_power * tail_us / 1000000`` in every grant and
power window, and includes it in temperature projections. Safe-ticket refunds
do not forgive stored DSP state before DVC. A producer lacking a complete
qualified norm/state/clock-loss bound must advertise no transfer; zero is not
a substitute for unknown. The real TAS helpers still supply no certificate.
