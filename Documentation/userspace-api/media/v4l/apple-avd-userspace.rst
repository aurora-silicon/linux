.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later

AVD userspace distribution decision (draft, 2026-10-31)
====================================================

Ship GStreamer's native stateless decoder and one VA-API bridge. Keep a
request-enabled FFmpeg build isolated for qualification. Enable Interchange
only after import and scanout qualification; this kernel draft establishes
the allocation and selection contract, not a battery-life result.

Package changes proposed
------------------------

.. flat-table:: Distribution package plan
    :header-rows: 1

    * - Distribution
      - Native stateless path
      - Player and browser path
    * - Fedora
      - ``gstreamer1-plugins-bad-free`` built with ``-Dgst-plugins-bad:v4l2codecs=enabled``;
        ``gstreamer1``, ``gstreamer1-plugins-base-tools``,
        ``gstreamer1-plugins-good``
      - Proposed ``libva-v4l2-request-avd``, plus ``libva-utils``, ``mpv``,
        ``firefox`` and an aarch64 VA-API-enabled ``chromium`` build
    * - Arch ARM
      - ``gst-plugins-bad`` built with ``-Dgst-plugins-bad:v4l2codecs=enabled``;
        ``gst-plugins-base``, ``gst-plugins-good``, ``gstreamer``
      - Same proposed bridge, ``libva-utils``, ``mpv``, ``firefox``, ``chromium``
    * - Omarchy on Arch ARM
      - The same Arch packages
      - The same bridge and browser patches; proposed ``aurora-avd-video``
        integration package owns opt-in launchers, without editing user dotfiles

Rebuild the GStreamer Bad plug-ins against headers containing the AX formats.
The table uses the monorepo Meson option; a standalone Bad plug-ins build
uses ``-Dv4l2codecs=enabled`` instead.
Runtime element discovery must find ``v4l2slh264dec``, ``v4l2slh265dec`` and
``v4l2slvp9dec``; require ``v4l2slav1dec`` only where hardware and firmware
advertise AV1. GStreamer is the native reference pipeline, not an mpv or
browser backend.

For the bridge, use the AVD early-export and lifetime fixes from
`omacom/libva-v4l2_request, commit 4f60c51f7841dd74bb61c94a4ad31e630bbdab01
<https://github.com/omacom/libva-v4l2_request/tree/4f60c51f7841dd74bb61c94a4ad31e630bbdab01>`_
as the packaging candidate, pending our conformance run. It is GPL-3.0-or-later.
Build with Meson, prefix ``/usr``, and ``-Ddriverdir=/usr/lib64/dri`` on Fedora
or ``-Ddriverdir=/usr/lib/dri`` on Arch. Install only
``v4l2_request_drv_video.so``. Declare a package conflict with any package
owning that same module, including Fedora's existing
``libva-v4l2-request``. Keep libva and the GPU's Mesa driver installed.

Keep distro FFmpeg and its ABI for mpv and Firefox. The bridge uses their
existing VA-API clients, so neither needs FFmpeg Request API patches.
Package a separately pinned request-enabled FFmpeg only under
``/opt/aurora/avd-qualification`` for benchmark and conformance work; do not
replace system ``libavcodec``. Upstream FFmpeg stateless support remains a
dependency of Firefox's separate native-stateless proposal.

Application configuration proposed
----------------------------------

The integration package should install application-specific opt-in launchers
under ``/usr/bin/aurora-avd-{mpv,firefox,chromium}``. Each launcher sets only
``LIBVA_DRIVER_NAME=v4l2_request`` for its child and preserves the arguments.
Do not set a session-wide driver override.

* mpv: initial launcher arguments ``--hwdec=vaapi-copy --gpu-api=opengl``.
  This is a copying compatibility baseline. Promote to ``--hwdec=vaapi``
  after import qualification; neither option proves DCP direct scanout.
* Firefox: rebuild with ``--enable-v4l2`` and the broker/filter change below.
  The opt-in profile needs ``media.ffmpeg.vaapi.enabled=true`` where that
  preference exists and ``media.hardware-video-decoding.enabled=true``.
  Treat a blocklisted codec as a qualification failure to investigate;
  ``media.hardware-video-decoding.force-enabled=true`` is a diagnostic
  override, not a shipped default. Keep RDD sandboxing enabled.
* Chromium: build with ``use_vaapi=true`` for aarch64. For H.264-capable
  distribution builds use ``proprietary_codecs=true`` and
  ``ffmpeg_branding="Chrome"``. The initial launcher uses
  ``--ozone-platform=wayland --use-gl=angle --use-angle=gl
  --enable-features=AcceleratedVideoDecodeLinuxGL``. Test the pinned browser
  version's device broker as well as its DMA-BUF early-export path; preserve
  GPU sandboxing. Enable ``AcceleratedVideoDecodeLinuxZeroCopyGL`` only after
  import qualification. Verify the actual decoder in ``chrome://media-internals``.

Firefox minimal change
----------------------

`Mozilla bug 2080696 <https://bugzilla.mozilla.org/show_bug.cgi?id=2080696>`_
has an unmerged candidate patch and a reporter's sandbox-enabled AVD test.
Current ``AddV4l2Dependencies()`` admits M2M video nodes but omits their media
controller. ``RDDSandboxPolicy`` admits video ioctls, but omits media/request
ioctls. Backport the narrow topology-based broker change:

* Record ``st_rdev`` for each admitted M2M node. Admit read/write access to
  a media node only if ``MEDIA_IOC_G_TOPOLOGY`` links it to such a video node.
* Admit read-only access to that node's exact
  ``/sys/dev/char/<major>:<minor>/uevent`` for the bridge's devnode lookup.
* Under ``MOZ_ENABLE_V4L2``, admit required media-controller and request-fd
  ioctls in RDD. The attachment admits ioctl type ``'|'``; security review
  should assess limiting it to the bridge's exact ioctl set.

A wildcard allowance for every media/video node is unnecessary. Camera and
unrelated media nodes must stay denied. Test topology failure, malformed
counts and concurrent device removal as well as successful decode. Keep
``MOZ_DISABLE_RDD_SANDBOX`` unset. This is a candidate fix, not local proof.
The larger `native stateless Firefox path (bug 1969297)
<https://bugzilla.mozilla.org/show_bug.cgi?id=1969297>`_ also needs decoder
setup and modifier-aware imports; it is not the minimal shipping change.

Interchange and release criteria
-------------------------------

Add AX-to-DRM mappings to GStreamer v4l2codecs and the bridge before selecting
AX capture. Negotiate ``DMA_DRM`` caps and modifier ``0x0c00000000000003``;
retain the modifier and opaque image layout through VA DRM-PRIME export,
browser imports, compositor and DCP. Do not expose AX03 as linear P010.
When a consumer cannot import the modifier, negotiate linear capture for
the entire reference pool. Do not switch an allocated pool in place.

Qualification must cover linear and AX pixels, references, 8/10-bit output,
resolution changes, early export, restart, sandbox-denial tests, and software
fallback. Verify DCP ``IN_FORMATS`` and actual plane assignment separately
from successful GPU import. Measure power only after proving scanout.
All hardware proof and package enablement in this decision remain TODO.

Sources
-------

* `GStreamer stateless plug-in <https://gstreamer.freedesktop.org/documentation/v4l2codecs/index.html>`_
* `GStreamer DMA-BUF negotiation <https://gstreamer.freedesktop.org/documentation/additional/design/dmabuf.html>`_
* `Chromium VA-API guidance <https://github.com/chromium/chromium/blob/main/docs/gpu/vaapi.md>`_

* `Fedora package split <https://packages.fedoraproject.org/pkgs/gstreamer1-plugins-base/>`_
* `Arch ARM package <https://archlinuxarm.org/packages/aarch64/gst-plugins-bad>`_
