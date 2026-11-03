Resident ANS over APIF
======================

``apple,nvme-apif`` selects the native table-6 queue ABI. It keeps the
existing Apple NVMe queue depth, batching, crypto fields, recovery ownership
and synchronous post-M4 flush policy. Linux builds and translates the TCB
and physical page list; Aura forwards selectors and arguments unchanged.
No payload copying or monitor-side NVMe policy is required.

The boot producer must quiesce RTKit and stop controller DMA before Linux
starts. It supplies ``asahi,rtkit-quiesced`` and four distinct reserved-memory
pages named ``admin-sq``, ``admin-cq``, ``io-sq`` and ``io-cq``. Each is coherent,
16 KiB aligned, 16 KiB sized and ``no-map``. Their guest addresses must map to
the native physical pages already registered with the firmware. Linux clears
and reuses those pages; it does not substitute new queue addresses. They stay
reserved after unbind because the native queue registrations survive it.

This profile requires an identity DMA aperture. It checks each request's
physical translation against the DMA address in its command and PRP chain.
An offset aperture is rejected before native request registration. Native
metadata addresses are independently translated, including the scratch TCB
and page list. Queue-control and MAP endpoints return void; UNMAP must return
one before a request's backing can be reused.

A lost MAP response retains that request and prevents new registration until
recovery has stopped the controller and explicitly invalidated every retained
tag. Failed invalidation quarantines the controller and retains its DMA,
requests and APIF client until platform reset. SART unmap failures also retain
Linux-owned backing. These are ownership failures, not successful I/O.

Live RTKit adoption is deliberately excluded from this new profile: controller
CC.EN/RDY alone cannot prove a live mailbox session or authorize inherited
buffers outside guest RAM. Existing raw-controller profiles keep their existing
validated adoption path. The guest producer must provide the stopped handoff
above; a version-1 live-session DT is not a drop-in replacement.

Build and host fault tests do not qualify this path on hardware. Boot,
read/write/flush durability, controller reset and teardown must be tested with
the matching Aura version-2 monitor and generated handoff description.
