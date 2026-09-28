.. SPDX-License-Identifier: GPL-2.0-only OR MIT

M3 runtime render batching
=========================

The M3 runtime groups adjacent render commands into bounded ordered batches.
``asahi.m3_render_batch_size`` selects the boot limit, clamped to 1..16; its
default remains 1. Compute batching and early tiling are independent settings.

For live comparisons, root may write 0..16 to
``/sys/module/asahi/parameters/m3_render_batch_override``. Zero, the default,
uses the boot limit. Values 1..16 override that limit for subsequent packets.
The value is atomic and each packet snapshots it once, before starting work.
Invalid writes fail without changing the previous setting. No userspace GPU
API changes are required.

For example, when booted with render batch size 4::

    echo 8 | sudo tee /sys/module/asahi/parameters/m3_render_batch_override
    cat /sys/module/asahi/parameters/m3_render_batch_override

Restore the boot setting with::

    echo 0 | sudo tee /sys/module/asahi/parameters/m3_render_batch_override

Changing this limit never skips resource dependencies, engine/VM boundaries,
completion events, stamp checks or retirement. It does not enable early tiling
or modify compute batches. Larger batches still require hardware qualification:
compare complete-pixel dependent-pass checks and a fixed interactive workload,
not just submission throughput. Keep a qualified boot entry and restore the
previous limit if correctness or latency regresses. A zero override preserves
the existing boot configuration without requiring a reboot.
