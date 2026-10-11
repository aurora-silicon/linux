.. SPDX-License-Identifier: GPL-2.0-only

N1 firmware and PCI transport
=============================

The Control function owns the firmware bootstrap and its DMA allocations.
Alpha and Beta use their own PCI-function DMA mappings. The driver requires
an APIF host with the supported isolated Gen3 x1 reset layout. All endpoint
DMA/MSI must be quiesced and the endpoint left in the stopped ROM personality;
live sessions cannot be adopted by this interface.

The Control PCI DT node supplies ``firmware-name``,
``apple,centauri-platform-id`` and ``apple,centauri-protocol = <27>``. The
platform identifier and personalized FTAB must agree with the actual device.
These are firmware inputs supplied by the boot producer, rather than a
machine-name lookup in the driver. No firmware or calibration data is shipped
in this series. The driver loads the firmware directly and starts asynchronously;
a missing file or failed phase requires correcting the input and reprobe.

The host's native DART reset requires exclusive Linux ownership and no retained
firmware streams on that unit. The boot producer must resolve ownership before
handoff. This driver cannot discard retained streams to make a reset succeed.
A failed link transition prevents further endpoint accesses. Published DMA and
vectors remain pinned when shutdown cannot be proved; a platform reset is then
required. This is an error path, not a supported live-session handoff.

Bootstrap command rings are bounded and used once. Radio data-plane ring sizes
and batching are separate from bootstrap. The transport builds without the
Wi-Fi consumer; it still starts the shared firmware for the later Alpha/Beta
consumers. Wi-Fi and Bluetooth are separate follow-up changes. Hardware
qualification of this cleaned series, including suspend/resume, remains pending.
