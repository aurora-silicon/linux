.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Curated M3 GPU reliability backports
===================================

This series ports Eryk Wieliczko's nine M3 commits after completion-state
publication, through ``e9268a4002d924f9c6385dff4972f2d94dc93c7a``, from
his ``eryk_m3_gpu`` branch onto the tested latency3 kernel.
Each port preserves its original author and records the upstream commit.

The series contains:

* ``58a9b568a``: latch a failed submission before later resource reuse;
  retain terminal progress evidence without treating it as forward progress.
* ``7c64de6e7``: expose bounded M3 first-fault snapshots via devcoredump.
* ``cc3a3b104``: allow longer valid vertex work between firmware TA progress
  checks. The finite host completion deadline remains in force.
* ``6fb519667``: claim packet completion once, preserving the first fence
  result across late scheduler callbacks and retaining failed DMA ownership.
* ``6664169b2``: capture the first error outside active-job polling as well,
  with snapshot bounds and failed-read rollback.
* ``2378b7741``: use shared host-verified retirement accounting.
* ``a63738cc7``: validate zero and overflowing GEM sizes before page rounding.
* ``d027554bc``: account live/peak GEM and coherent allocation extents with
  their actual owners, exposed through the read-only memory diagnostic.
* ``e9268a400``: reserve fault snapshot storage before firmware handoff,
  keeping record appends out of reclaim after a GPU failure.

Integration boundaries
----------------------

The curated InitData/device interfaces and existing runtime render-batch
override are preserved. The imported code does not change the graphics
userspace API or require a Mesa modification. The external completed-swap
and sparse two-frame damage code is unchanged. Test configuration retains
render batch 16, compute batch 1, round-robin scheduling and disabled early
tiling; none of those defaults are changed by this series.

Offline tools live in ``tools/asahi-m3``. Upstream lab captures and deployment
helpers are excluded. Fault snapshots contain device-local firmware state:
keep actual captures private and out of version control. Offline decoder
tests use synthetic records, not firmware dumps.

``/sys/kernel/debug/asahi-m3/memory`` reports allocation extents and peak
counts for user GEM, kernel GEM and M3 coherent backing. It is observational
diagnostic data, not a transactional snapshot or a measurement of resident
pages, and imported memory need not represent unique physical storage.
``/sys/kernel/debug/asahi-m3/progress`` retains its existing version-1 fields.

Validation scope
----------------

Run the offline checks listed in ``tools/asahi-m3/README.md`` and the Apple
display host checks in ``tools/testing/selftests/drm/apple/README``. Build
the full Image/modules/DTBs with the intended runtime configuration, then
boot a separate candidate entry and verify GPU rendering/compute, the
dependent-pass complete-pixel reference, display output, input and network.
Keep the tested kernel and matching modules available as a fallback.

Host tests cannot establish live fault recovery. These backports improve
failure handling and observability; they do not establish that a faulting
application can recover without affecting the desktop, or demonstrate a
frame-rate improvement. Live qualification is tracked in
``iconidentify/m3-roadmap#75``.
