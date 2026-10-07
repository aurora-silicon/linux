.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Apple DCP H17P backlight integration
===================================

The backlight policy implements the clean display specification's BACKLIGHT-1
through BACKLIGHT-4, with IOMFB-3 and LIFETIME-1 through LIFETIME-3 constraints.
It extends the existing Asahi backlight driver. It contains no H17P brightness
wire layout, calibration table, property command, or guessed initial level.
Policy activation requires explicit successful configuration. Existing
unconfigured H17P sessions, including T8142, keep their established DAC,
property/powerlog and DPMS paths. Older firmware remains on its existing path.
Neo startup must configure the policy before exposing the backlight or starting
its DPMS/brightness traffic; a firmware-version match alone does not activate
this policy.

Admission and startup
---------------------

The central display worker must call ``dcp_backlight_configure()`` from process
context after linking the CRTC and admitting all required records. Its arguments
are the validated maximum commanded nits, an optional inherited level, an
optional documented ADT default, and a callback that schedules the central
outbound queue. Maximum must fit the backlight core's signed integer range.
An inherited zero is valid and takes precedence over the default. With neither
level available, configuration returns ``-ENODATA``. The policy backlight is registered only after configuration succeeds.
Configuration must precede any existing backlight registration; it returns
``-EBUSY`` if a device has already been published. Configuration does not send a command.

The device exposes ``apple-panel-bl`` with platform type, linear scale and nits
units. Its maximum is the supplied ceiling; the older 509-nit DAC cap does not
apply to configured H17P policy. Consumer matching accepts the DCP device and its DRM device.
Registration freezes takeover before publishing the backlight. A decoded
powerlog value may seed the state through ``dcp_backlight_seed()`` before that
point. All samples after Linux takes control, including per-frame zeros, are
ignored. The older property and powerlog parsers do not seed an active policy implicitly.
Gather a sample before configuration and pass it as the inherited argument if
asynchronous registration could otherwise win the race.

``brightness`` is the requested restore target. ``actual_brightness`` is the
last completed commanded level, initially the inherited/default estimate. This
is not a photometric measurement. Updates step immediately; no kernel fade or
initial-brightness/blank-level override is introduced.

Queue and lifetime contract
---------------------------

The callback only schedules/coalesces work. It must not submit an RPC directly,
take DRM modeset locks, or run a present inline in a receive/commit callback.
It remains valid until the central queue and backlight workers are drained at
teardown. Policy access is protected by the brightness spinlock; the callback
is called after dropping it. Configuration also takes the registration mutex.

The single outbound queue handles normal and brightness-only presents. A static
or inactive screen uses the last *accepted* surface, including its cached
geometry and IOVA. It must hold that surface's framebuffer reference and mapping
before calling ``dcp_backlight_prepare()`` with ``have_surface = true``. A DRM
plane state containing a rejected replacement is not an accepted surface.
The policy's boolean is a caller assertion, not a framebuffer reference.
Allocate any retirement bookkeeping during atomic prepare, never here.

Prepare reserves a sequence and returns commanded nits. ``-ENODATA`` means no
configuration or surface; ``-EBUSY`` means a prior brightness transaction is
outstanding; ``-EALREADY`` means no brightness change is pending. These results
do not clear pending work. The serializer places the returned level only in
admitted fields of the next valid present. A normal present without a brightness
change keeps the inherited/completed level; its clean layout must support that.
The sequence is a software cookie, not a wire present ID.

Associate the cookie with the central transaction and its strict wire ID. Call
``dcp_backlight_complete(sequence, true)`` only after both accepted submission
and matching completion. On serialization/submission rejection call it with
``false``; actual remains unchanged and the update is pending again. Wrong or
duplicate sequence callbacks have no effect. On a firmware crash retain the
surface and stop new submissions; there is no local reset/retry ladder.
After a transaction drains, inspect ``dcp_backlight_pending()`` and schedule the
next update or defer until the pipe becomes usable. Requests received during a
transaction remain pending. The central queue must bound static-screen latency
to 200 ms; this still requires a hardware timing test.

A brightness-only completion never authorizes framebuffer retirement, including
retirement associated with a rejected replacement. Existing generic retention
helpers must be called only for the corresponding accepted replacement. This
policy never arms, releases, or unmaps a framebuffer.

DPMS and suspend
----------------

Internal-panel H17P ``dcp_poweroff()`` and ``dcp_poweron()`` change the policy and
schedule the retained-surface queue. They bypass the legacy pipe-off/on path,
including surface clearing and final release. Until configuration, they warn
and leave the pipe intact; they cannot claim a completed hardware blank.
Startup must independently establish/adopt the live pipe before enabling the
policy. External outputs retain their existing power handling.

DPMS off, a backlight core blank, or backlight core suspend forces effective nits
to zero while preserving the user target. Brightness changes during any blank
update the target. DPMS on restores it only when core blank/suspend is cleared.
The backlight core's suspend/resume callbacks feed this state; the central PM
worker still owns queue draining, domain lifetime and resume order. Do not call
unqualified H17P sleep/pipe-off methods as a suspend fallback. Keep the last
accepted surface mapped across soft-off and resume.

Required records and qualification
----------------------------------

The following SPEC admission records must supply facts before production use:

* R-ADT: panel ceiling/calibration in commanded nits and a documented default
  if no inherited sample is available. No expected 525-nit ceiling is hardcoded.
* R-BACKLIGHT: takeover sample source, units, validity and timing; decoded
  powerlog semantics; commanded-nits conversion, valid update/enable fields,
  field offsets, widths and byte order in the present.
* R-SWAP: complete surface layout and successful submit/completion correlation
  for normal and retained-surface presents, including inactive CRTC handling.
* R-DPMS: which admitted brightness/property actions blank and restore the live
  pipe without clearing its surface, plus wake ordering. No new numeric property
  or guessed DPMS payload is emitted by this policy.
* R-SERIALIZE: ordered brightness/present/property/diagnostic timeline and busy
  deferral through the central queue; static-screen update latency.
* R-TEARDOWN: safe final release and suspend/resume boundary. Soft-off supplies
  no such release boundary.

Run ``apple-dcp-backlight`` KUnit tests with ``CONFIG_DRM_APPLE_KUNIT_TEST``.
They cover takeover/default/zero handling, range validation, twenty DPMS cycles,
cached restore, both suspend/DPMS orders, static-surface deferral, busy sequencing,
rejection and stale completion. They do not qualify firmware fields or GNOME.
Hardware acceptance requires O-BACKLIGHT, O-BACKLIGHT-TAKEOVER (10 Hz PBwo with
no jump above 0.05 W), O-BACKLIGHT-CRC, O-DPMS (twenty cycles), and O-SUSPEND.
