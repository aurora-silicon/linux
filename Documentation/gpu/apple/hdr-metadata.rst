.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Apple DCPAV HDR10 static metadata
================================

Desktop 26.x external display processors accept HDR10 static link metadata
through the AV endpoint. The atomic modeset first selects the existing PQ
colour element through IOMFB. After a successful modeset, DCPAV selector 0x15
is sent with AFK command 0xc0. Internal panels keep their existing EDR path.
Other firmware ABIs are not admitted to this metadata transport.

The message contains a two-pair Apple 0xd3 dictionary. ``Type`` is the string
``CEA-861:Type1`` and ``Data`` is the 24-byte little-endian CTA Type 1 body,
from DRM's display primaries through MaxFALL, without EOTF or descriptor ID.
Both DRM metadata type fields must be zero. The current output colour element
supplies the EOTF independently.

The connector caches only successful sends. An unchanged dictionary on the
same link and video-service generation is not sent again, and frame swaps
never send metadata. Link teardown, route changes and service replacement
invalidate the cache. Transport failures, short replies and the status word
at message offset 0x40 are checked before the cache is updated.

Removing HDR_OUTPUT_METADATA while the link remains up deliberately leaves
the last metadata active until link teardown. An empty dictionary or a zero
24-byte block is not a supported clear operation. Live-link clearing is
pending a separate protocol research task.

Tests
-----

``CONFIG_DRM_APPLE_HDR_KUNIT_TEST`` tests the encoder's complete byte layout,
message offsets, reserved bytes and reply validation without display hardware.
Use ``drivers/gpu/drm/apple/tests/hdr.kunitconfig`` with the KUnit runner.

Hardware qualification should use an HDR-capable external monitor and a
controller-approved candidate boot. On J616s with the MSI monitor, use kmshdr
or mpv with DRM HDR metadata passthrough and an HDR10 sample carrying known
mastering primaries, luminance, MaxCLL and MaxFALL. Check the monitor's OSD for
HDR metadata, change the metadata without changing the timing, and check the
new values. An HDR indicator alone establishes PQ reception, not the metadata
contents. Check that repeated frame updates do not send selector 0x15, then
remove the property on a live link and confirm no empty or zero metadata is
sent. Finally disconnect/reconnect and confirm metadata is sent again after
the next successful modeset. Repeat with audio disabled to exercise the
video-only AV endpoint path.
