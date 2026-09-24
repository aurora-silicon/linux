.. SPDX-License-Identifier: GPL-2.0-only

Apple T8140 audio, AOP and ISP qualification
==========================================

The T8140 implementation uses the existing Apple AOP firmware to operate the
serial audio fabric.  The host implements its mailbox and DMA interfaces and
registers native ALSA, IIO and V4L2 devices.  It does not implement the vendor
firmware's signal-processing algorithms.

This board description is a focused bring-up topology: processor, interrupt
and power controllers, DockChannel console, audio, AOP and ISP.  Its intended
qualification environment is a RAM initramfs with a serial console.  The
provided topology does not describe a complete installed desktop platform.

Interfaces
----------

The two audio cards expose different transports:

=========================== ================= ============== ===== ========
Function                    Card / PCM        Representation Rate  Channels
=========================== ================= ============== ===== ========
Low-power microphone        AppleJ700LPAI / 0 S32_LE         16000 2
Headphones                  AppleJ700 / 0     S24_LE         48000 2
Headset microphone          AppleJ700 / 0     S24_LE         48000 1
Internal speakers           AppleJ700 / 1     S32_LE         48000 2
Speaker digital observation AppleJ700 / 2     S32_LE         48000 2
High-quality microphones    AppleJ700 / 3     FLOAT_LE       48000 2
=========================== ================= ============== ===== ========

The low-power microphone uses a firmware-bound source ring on AOP DART stream
9.  Producer reports provide the cursor; the ALSA period is 3200 frames.  The
high-quality microphone uses LEAP RX0 through the ADT-described proxy aperture
at 0x353200000 and DART stream 11.  Its DMA words are IEEE 754 binary32 samples;
representing them as integer PCM produces incorrect audio even when nonzero
bytes are captured successfully.  ALSA can perform conversion in userspace.

The jack uses base-ns ADMAC TX2/RX2 and the AOP cout/cin services.  Its serializer
uses a 125-SCLK frame at 48 kHz with the codec's qualified half-SCLK DSP-A frame
delay.  The speakers use LEAP-ns TX0 and the spkr service.  The observation PCM
is the digital signal sent toward the amplifier, not measured amplifier
current/voltage feedback.

Firmware and calibration
------------------------

The AOP firmware is loaded by the machine's boot chain.  T8140 requires both
its ordinary RTKit mailbox and the separate setup mailbox.  Setup endpoint
buffers belong to the setup-mailbox device's DMA stream; AFK buffers and audio
buffers use their own described DMA apertures.  These contexts must not be
merged merely because they share a DART controller.

CT817 ambient-light initialization additionally requests
``apple/t8140-ct817-cal.bin`` through the firmware loader.  This is an external,
machine-supplied setup packet, not the legacy ``apple/aop-als-cal.bin`` EPIC
calibration file.  The packet is exactly 80 bytes.  Its first two little-endian
64-bit words are operation 0x0746b8d665152e31 and inner length 56; the remainder
is opaque calibration/transport data.  The loader validates this envelope and
preserves all bytes.  No calibration contents are built into the driver.
Missing or malformed CT817 calibration is reported without preventing the
other AOP services from starting.  A usable ALS requires successful calibration;
a registered sensor alone is not evidence that measurements are available.

The ISP uses boot-chain firmware and an externally supplied sensor setfile.
For the qualified IMX558 H17 setfile, the logical length is 0xcba6 bytes and the
firmware transfer is 0xcbc0 bytes with zero padding.  Older sensor setfiles retain
their established minimum lengths.  The transfer padding is not permission to
accept a truncated file.  The bootloader must supply the preserved ISP heap
range in the ``apple,asc-mem`` reserved-memory node.

Firmware/calibration acquisition and redistribution rights are separate from
the host driver's licence.  A source or kernel patch export must not acquire
vendor payloads merely by including a local test initramfs.

Ownership and failure behavior
------------------------------

AOP setup and application endpoints are separate interfaces.  Only advertised
application endpoints in the qualified AFK range are started.  T8140 replies
may omit request tags, so calls on an endpoint are serialized.  After a timeout
or a send failure that may have published a ring command, an untagged endpoint
cannot safely match a delayed reply to a new request and requires
reinitialization; it is not silently reused.

The firmware accepts the low-power source-ring binding once per firmware
lifetime.  The transport owns it across audio-driver reloads.  Host ownership
must be established before publishing an address, and an uncertain binding
must not free or republish the buffer as if nothing reached the firmware.
Transport removal must quiesce child users before closing their command path
and preserve DMA ownership until firmware shutdown is confirmed.

LEAP channel register access needs its power leaves enabled during allocation,
termination and release.  The current implementation retains a register-domain
reference for the channel's registered lifetime, with separate stream references.
This avoids an unpowered register fault during close/release; per-channel ADMAC
runtime power management remains a future optimization.

ISP firmware stays resident between ordinary stream sessions.  It receives a
periodic watchdog service while capturing; a missing watchdog can yield diagnostic
fill rather than usable images.  Full power gating currently requires a cold
boot for recovery.  Warm re-probe/system-resume camera recovery is not claimed.
Malformed returned buffer lists must fail bounds/count checks before changing
buffer ownership, and submitted descriptors have zeroed reserved fields.

Speaker protection
------------------

J700's fixed-gain MAX98360A path applies a software gain while copying S32
samples into the float32 LEAP ring.  The card interlock limits volume until a
protection daemon owns the required controls and renews its lease.  A new owner
inherits no previous owner's lease.  Losing protection invalidates prepared
speaker queues; stale playback, including a paused queue, must not resume after
a copy-time clamp as though old samples had been attenuated.

The speaker PCM is bounded to 32 KiB, or 4096 stereo frames at 48 kHz (85.33 ms).
Other frontends use their actual finite managed-buffer limits.  The bound also
limits how long ordinary gain changes can wait behind previously copied data.
The 250 ms lease interval is not a proven end-to-end acoustic stop deadline:
scheduling, DMA and firmware/amplifier behavior require measurement.

Userspace protection must fail closed on missing, stale, partial or invalid
observation data.  The accompanying model estimates electrical/thermal behavior
from the digital observation signal and documented physical parameters.  It is
not current feedback, an independently validated excursion limiter, or blanket
qualification of arbitrary full-scale signals.  Speaker DSP, protection and
kernel interlock are separate parts of the usable stack.

Diagnostics and qualification
-----------------------------

Normal readings use ALSA, IIO and V4L2.  Routine C driver details use dynamic
debug; this kernel's Rust ``dev_dbg!`` uses the Rust debug-assertions build gate.
Errors that can recur are bounded or rate-limited.  ISP firmware-terminal
logging remains an explicit ``fwlog`` option, disabled by default and bounded
in record size and emission rate.  Normal operation does not dump calibration,
raw command structures or sensor serial numbers.

Offline checks cover source provenance, protocol bounds, failure/ownership
paths, sample formats, device-tree schemas and builds.  They do not establish
acoustic output or optical image quality.  Hardware qualification must record
exact Image, DTB, module, firmware, configuration and userspace identities and
exercise both microphone paths, jack playback/capture/buttons, speaker lease
loss and safe restart, scene-responsive ISP frames and changing ALS readings.
A nonzero buffer, successful probe or frame count alone is insufficient.
