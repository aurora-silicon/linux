M3 runtime compute batching
===========================

``/sys/module/asahi/parameters/m3_compute_batch_override`` selects the existing
adjacent-compute batch limit for subsequent packets. Root may write 0 through
16. Zero uses the boot-time ``asahi.m3_compute_batch_size`` value, clamped to
1..16 as before; malformed and out-of-range writes fail without replacing the
active value. The default is zero, preserving existing boot behavior.

The runtime snapshots the effective limit once per packet. Only consecutive
compute commands in that packet may share a batch. Per-slot storage, command
ordering, VM ownership, and full retirement at engine/VM boundaries remain
unchanged. This does not change the submission UAPI or enable early tiling.

Qualification should start with compute batch 1 and compare a larger limit
only after a clean GPU smoke check. Run compute readback and rendered-frame
checks again after changing the value; record the renderer, resolution, render
batch, scheduling policy, and thermal/power conditions with any performance
result. Keep the existing thermal policy enabled. Restore zero to return to
the boot setting without another reboot.

This control permits measurement; its addition alone is not a performance
improvement. The M3 desktop campaign's native Quake 60 FPS target and browser
hover qualification remain open until measured on the candidate kernel.
