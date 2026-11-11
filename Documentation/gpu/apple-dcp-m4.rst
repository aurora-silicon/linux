.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Apple DCP on M4
===============

The J713/T8132 internal panel uses the common Apple display driver,
``appledrm``. The display subsystem owns the DRM device, DMA GEM objects,
planes, CRTC and connector. The DCP is a component of that device. Its
firmware-26.6.2 RPC interface is implemented by ``iomfb_m4.c``; it does not
register another DRM device or expose a second KMS implementation.

The backend requires ``APPLE_DART=y`` and
``APPLE_DART_J713_DCP=y`` with 16-KiB ARM64 pages. Its current retained
noncoherent allocation path uses built-in DMA implementation interfaces and
is not a modular DART ABI. Ordinary DART and other Apple display generations
remain independently available with modular DART. An unsupported J713 profile
refuses adoption before changing the inherited display state.

The backend adopts the qualified running firmware session from the boot
loader. It validates the board, firmware identity, original framebuffer,
reserved firmware memory and inherited DART mappings. Firmware-private DMA
uses the DCP's SID23 domain; framebuffer DMA uses the display subsystem's
SID0 domain. Imported buffers and local dumb buffers use the common DRM DMA
GEM path. A scanout address must satisfy the M4 aperture and layout checks.

Presentation and ownership
--------------------------

Atomic commits use the common plane and CRTC callbacks. Producer fences are
waited through the DRM atomic helpers before presentation. The versioned
backend serializes its RPC transactions and retains framebuffer references
while firmware can access their DMA addresses. Both the accepted A407 reply
and the matching D590 completion are required before the preceding surface
can be retired. A timeout or an ambiguous firmware outcome retains the
affected surfaces; a successful brightness-only update is not a replacement
surface receipt.

Probe and component binding prepare the running session and scanout backing
without replacing the inherited framebuffer. The first startup surface is
submitted only after aperture ownership has transferred and the common DRM
device is registered, with modeset locks preventing a userspace commit from
racing activation. Failed accepted atomic commits still deliver their DRM
completion events; their out-fences carry the error. DRM owns signaling and
releasing the associated completion objects.

Only the qualified internal-panel mode and linear primary-plane formats are
exposed. Other firmware interfaces and external-display roles continue to
use their existing backends. Sharing the DRM framework does not make their
wire messages interchangeable.

Boot and validation
-------------------

The candidate boot admits the inherited display DARTs and loads ``appledrm``
before the GPU starts: the G16 probe defers until the display registers. It
does not use the retired qualification
driver's ``ready``, ``startup`` or ``kms`` sysfs triggers. The boot loader's
older ``apple,t8132-display-diagnostics`` name remains accepted as an input
handoff ABI; the common display driver owns the device in either case.

CPU regressions live under ``tools/testing/selftests/drivers/gpu/apple-dcp``.
They preserve independent wire fixtures and exercise the production protocol
and plane-validation paths. Hardware validation requires a fresh kernel
boot, verified module ownership, bounded page-flip and blank/unblank checks,
and a restored accelerated compositor. CPU tests and a successful kernel
build do not establish display hardware acceptance.

The retained firmware session still has no qualified final release/reset
boundary. Display DART registration pins its module, state, mappings, memory
reservations and backing until a platform reset. Unexpected device removal
quarantines the domain and unregisters its IRQ/IOMMU callbacks without freeing
state that a retained domain or firmware can reach. The existing kexec guard
is acquired before IOMMU registration can publish host memory; normal and
crash kexec execution remain blocked for this session. Ordinary reboot or
power-off leaves mappings intact until the platform resets the hardware.
Unload and system suspend/resume require separate qualification; logical
display blanking does not establish those boundaries.

Firmware policy limits
----------------------

The D408 clock callback still returns the qualified hostless 48/24 MHz
fallbacks; these values are not measurements of the active display clock.
D003 explicitly declines realtime bandwidth service. The resulting firmware
frequency and bandwidth diagnostics are unresolved integration gaps, even
when fixed-mode presentation works. A matching clock provider and a fully
initialized PMC bandwidth owner are needed before advertising those services.
A powered PMC word or a BWR enable bit alone is not proof of mailbox readiness.
Do not publish guessed clock rates or the previously withdrawn PMC scratch
mapping to suppress these diagnostics. Hardware qualification must preserve
and report them instead of treating a working compositor as policy parity.

The retained profiles select strict DMA as their default domain. Administrative
changes of the IOMMU group type to DMA-FQ are not qualified; keep the default
strict domain for both display and radio groups.
