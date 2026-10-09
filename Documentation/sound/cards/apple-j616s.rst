.. SPDX-License-Identifier: GPL-2.0-only

Apple J616s audio bring-up
==========================

Status
------

J616sAP (Mac16,7), chip 0x6040, board 6 has bounded m1n1 proxy audio
evidence. No Linux boot, ALSA card, PCM or mixer ABI has been qualified.
This document records the implementation and test boundary; it does not
enable a device-tree node, add a compatible or select a speaker model.

The native display names are MacBook Pro Microphone, External Microphone,
External Headphones and MacBook Pro Speakers. They are macOS names, not
ALSA identifiers.

Routes
------

The rates below are nominal profile values, not clock measurements.

.. list-table::
   :header-rows: 1
   :widths: 20 35 45

   * - Route
     - Proxy transport
     - Evidence
   * - Internal high-power microphone (hpai)
     - admac-leap-ns, stream 10, split RX0
     - Three-channel Float32LE at 48 kHz; completed 2 MiB and 6 MiB
       captures with recognizable audio
   * - Internal low-power microphone (lpai)
     - Owned 768,000-byte ring on AOP DART stream 8
     - Producer reports and recognizable reconstructed speech;
       snapshot-boundary integrity remains unqualified
   * - Headset microphone (cin, with trailing space)
     - admac-base-ns, stream 8, RX2
     - Mono 24-bit samples in 32-bit slots at 48 kHz; recognizable audio
   * - Headphones (cout)
     - admac-base-ns, stream 8, TX2; native coS (trailing space)/ms02
     - Stereo 24-bit samples in 32-bit slots at 48 kHz; user-confirmed
       left/right finite output and passing codec/controller restoration
   * - Speakers (spkr)
     - admac-base-ns, stream 8, TX0; MCA group 0, ms00
     - Six 24-bit samples in 32-bit slots at 48 kHz; all six drivers heard
       individually, first left/right woofers also paired for five seconds

A trailing space in a FourCC is significant. DMA channel and DART stream
numbers are not ALSA PCM indices. These tests use separate owned mappings;
they do not establish concurrent ownership of stream 8.

The native speaker provider order and bus addresses are:

.. list-table::
   :header-rows: 1

   * - Provider index
     - Speaker
     - Bus/address
   * - 0
     - Left woofer 1
     - i2c1:0x38
   * - 1
     - Right woofer 1
     - i2c3:0x3b
   * - 2
     - Left woofer 2
     - i2c1:0x39
   * - 3
     - Right woofer 2
     - i2c3:0x3c
   * - 4
     - Left tweeter
     - i2c1:0x3a
   * - 5
     - Right tweeter
     - i2c3:0x3d

All six SN012776 amps passed identity and native protected shutdown
configuration and now have user-confirmed individual tone output. The second
woofers use slots 2/3 at 300 Hz; tweeters use slots 4/5 at 2 kHz. Each new test
lasts one second with peak 0.125 and 20 ms fades. Matching TX reports, zero
residue/DART faults, mute before TX stop, protected shutdown and exact
controller/GPIO restoration pass, followed by watchdog recovery. DVC is 0x65
for the second woofers and left tweeter, 0x78 for the right tweeter. These are
finite test settings, not calibrated system volume. Simultaneous all-six and
continuous playback remain unqualified.
Native feedback describes RX1 with twelve 16-bit slots and sample-width
field zero. Signedness, packing, I/V order, scales and frame integrity
still require qualification; this is not an established S16_LE PCM contract.

Implementation order
--------------------

1. Resolve current ADT registers, clocks, resets, DMA parents and mapper
   streams. Bind the AOP firmware and EPIC version-2 command/reply layouts,
   fresh service identities and profile sizes. Reject malformed replies
   and nonzero return codes before using their payloads.
2. Implement LP owned-buffer publication and producer readiness before
   the tested HQ path. LP idle state alone does not release the mapping.
   Preserve the three-channel HQ float capture; a normalized mono preview
   does not qualify beamforming.
3. Integrate the CS42L84 at i2c2 address 0x4b using its own jack detection,
   input bias, PLL and ASP lifecycle. The existing codec driver already
   sets CS42L84_MIC_DET_CTL4_LATCH_TO_VP; the working proxy input
   requires that detection latch. Matching TX framing alone is insufficient.
   Qualify input and output independently, including muted stop and clock
   restoration, before simultaneous headset duplex.
4. Qualify speaker initialization, native mute/dwell/shutdown and reliable
   I2C completion. Complete protected configuration before activation.
   The paired five-second test had a matching completed DMA report,
   zero residue/DART faults, both mute writes and TX configuration restore,
   then timed out in the later amplifier shutdown wrapper. Watchdog
   recovery was required; complete cleanup was not established.
5. Qualify feedback capture and per-speaker model parameters, then the
   kernel/userspace volume interlock and its failure behavior. Enable
   speaker userspace routing only after those requirements pass.

The existing T8140 AOP audio driver supplies an integration pattern, not a
J616s compatibility match. Its J700 LP ring/stream geometry, CS42L83 jack,
MAX98360A speaker path and feed-forward sense stream differ from this board.
The existing CS42L84 and SN012776 codec drivers are source references;
the J616s machine path and firmware-owned bus still need integration and
live validation. Do not replace SN012776 I/V protection with the J700
feed-forward speakerguardd model.

The PA Semi I2C driver requires XEN (bit 27) completion; the unexplained
bit 29 seen in proxy failures is not an acknowledgement. Keep bounded
completion/error checks and do not continue cleanup after an ambiguous
transfer. DMA ownership must survive uncertainty until recovery; a timeout
or frontend idle reply is not permission to free mapped memory.

The shared Python I2C helper now clears only its observed status snapshot
and rejects XIP before the write. Direct guarded calls pass on both idle
speaker buses; private bounded amp trials use the same policy with their
ownership, errors and XEN/no-XIP gates unchanged. Later shutdown and four
individual playback cleanups pass, but do not prove the intermittent STOP
fault resolved. Existing generic transfer methods still reset FIFOs before
this check; the helper change does not make a whole transfer concurrency-safe.

Next kernel integration step
----------------------------

Add a separate T6040 AOP control-only provider profile and validate service
discovery before an ALSA audio child can probe. The AOP OF table currently
has no T6040 match. The required provider is the RTKit/AOP transport with
42-bit DART mappings, own ADT resources and EPIC version 2; the tested audio
service is on endpoint 0x22. J700 instead uses EPIC version 4, a setup mailbox
and audio on endpoint 0x21.

Make boot-argument overrides profile-specific: the qualified J616s startup
changes only EC0p to the ADT remap address 1 TiB. The existing legacy provider
also overwrites nCal, alig and AOPt; copying that profile is unqualified.
Retain firmware and endpoint DMA if shutdown cannot be confirmed.

Keep automatic legacy audio-child probing gated. Its Rust probe uploads a
632-byte PDM structure, including an explicit alignment byte, and writes
lpai property 301. The older proxy structure is 631 bytes; own J616s evidence
reads 626 bytes and identifies 301 as non-writable. A board-specific child
must instead use
the proven LP owned-ring/producer-readiness sequence before the three-channel
HQ Float32 path. This provider/child ABI mismatch is the first integration
blocker, before Linux card/PCM/control names or userspace profiles can be
qualified. CS42L84 jack integration and SN012776 I/V protection follow that
transport; J700 feed-forward protection is not a substitute.

First Linux inventory and acceptance
------------------------------------

Record the exact kernel, firmware and DT revisions and save probe logs
before adding runtime profiles. The following commands list devices
without starting an audio stream::

    cat /proc/asound/cards
    cat /proc/asound/pcm
    aplay -l
    arecord -l

For each observed card, record its driver and ID, mixer names/types/ranges,
dB metadata and jack events. Use the observed card ID when reading mixer
contents. Do not copy J700 IDs, PCM indices, mixer names or safe-volume
settings.

Record supported PCM formats/rates/channels and explicit channel mapping.
Then qualify bounded capture and headphones, stop/restart, and headset
plug/unplug. Shared-clock duplex and continuous streams require separate
evidence. For speakers, additionally test all-six routing, feedback frame
integrity, measured I/V scaling and thermal parameters, and daemon-exit,
feedback-loss and stalled-stream relock behavior. A successful probe or
audible tone does not satisfy these acceptance checks.

Sources
-------

The published m1n1 bring-up evidence [1]_ and driver contracts [2]_ pin the
proxy evidence used here. Private recordings, firmware and probe harnesses
are not included in this documentation.

.. [1] https://github.com/aurora-silicon/m1n1/blob/fd360ef69054e42724b975d7a4c61e0242b6b341/docs/j616s-bringup.md
.. [2] https://github.com/aurora-silicon/m1n1/blob/fd360ef69054e42724b975d7a4c61e0242b6b341/docs/j616s-driver-contracts.md
