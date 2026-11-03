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

Wi-Fi station consumer
----------------------

``CONFIG_APPLE_CENTAURI_WIFI`` adds the Alpha cfg80211/netdev consumer within
this transport module. It supports the protocol-27 command profile, active
2.4/5 GHz scanning and WPA2-PSK/CCMP station connections with firmware handshake
offload. The matched profile uses a raw 32-byte PMK supplied by cfg80211;
there is no separate passphrase interface. Unknown profiles are rejected by
the Control driver. Passive scan encoding is not established, so disabled and
NO_IR channels are excluded from active scans. WPA3, 6 GHz, roaming and wider
security/radio qualification remain outside the established driver scope.

Data uses the original 2,048-entry rings and coalesced completions. IRQs and
queued packets schedule the consumer; no periodic data poller or diagnostic
packet flow is added. Malformed frames and completion spans cannot index beyond
the coherent window. A lost command completion poisons further commands and
stops new transmission. Its key payload is cleared only after the transfer-ring
tail proves consumption; uncertain ownership keeps backing memory pinned.
Safe IPC teardown scrubs buffers after verified DMA shutdown. The cleaned
consumer still needs hardware traffic and suspend/resume qualification.

Bluetooth consumer
------------------

``CONFIG_BT_HCICENTAURI`` adds the Beta HCI/ACL PCI driver. A managed device
link requires the Control supplier to have established shared firmware and
orders removal and suspend against it. The Beta DT node supplies the factory
``local-mac-address`` in network byte order; no board-name or temporary-address
fallback selects initialization behavior.

Protocol-27 initialization provisions the address, completes firmware setup
and supplies a fresh random seed. The connection-command adaptation is enabled
only after the firmware's capability bit confirms support. Status and connection
completion events are normalized to the standard HCI command being tracked.
The firmware duplicate filter is reset in the HCI command-sync path before
filtered scanning starts; other drivers without that callback keep their
existing behavior. Standard report lengths are checked before applying the
matched legacy scan-response fixup.

The driver uses IRQ-triggered work and the original ring geometry. Diagnostic
capture, unrelated auxiliary rings, manual mode switches and success logging
are excluded. SCO and ISO delivery are not implemented. Coordinated firmware
teardown remains unqualified: a published Beta arena stays pinned on detach,
and rebinding requires a fresh shared-firmware boot. This is not a reset of the
Wi-Fi function. The cleaned driver still needs hardware HCI/ACL, repeated-scan
and suspend/resume qualification.
