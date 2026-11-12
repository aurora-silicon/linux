# Neo G17P queue-depth B/A recipe

This recipe qualifies the 26A428 publication policy: one firmware-visible command per logical
queue and engine (`Variant::publication_credits`). Run at least four one-shot boots in `B/A/B/A`
order, with two clean boots of each arm, on a kernel base the installed Stage 2 boots.

For every boot, pin the Neo Linux OS UUID and verify the selected one-shot entry, exact kernel and
patch hashes, and a 25-minute dead-man before reboot. `speakerguardd` must remain active and
governing. Do not test or gate on display state, input, idle time, login sessions, playback, or
time of day. Display and keyboard backlight may be turned off best-effort after boot only.

The arms are:

- `A`: the chosen hold-compliant base, without this series.
- `B`: the same base plus this series, with `asahi.g17_publication_credits` unset (0, the
  firmware policy: 1 on 26A428).

On each boot, in this order:

1. Record the one-shot entry, OS UUID pin, dead-man state, `speakerguardd` state, kernel identity,
   patch hashes, module parameter, and the initial `asahi-g17` debugfs capacity snapshot.
2. Run the 36,127-case Vulkan CTS slice with four jobs. Require Pass=18621, unchanged
   classification, zero device loss, and zero AVC.
3. Run isolated `gpubench-stress --test mem.limit --duration 60`. Flush partial metrics before
   `vkQueueWaitIdle` and bound the wait in the harness. Capture
   `asahi_g17:asahi_g17_submit`, `asahi_g17:asahi_g17_schedule`,
   `asahi_g17:asahi_g17_publish`, `asahi_g17:asahi_g17_complete`, and
   `asahi_g17:asahi_g17_output`.
4. Run isolated `gpubench-stress --test queue.depth --duration 60` and require PASS with no
   recovery, firmware crash, device loss, capacity refusal, or descriptor/kick overwrite.
5. If the GPU is healthy, run the complete stress suite once.
6. Save the full kernel log, stress JSON, debugfs snapshots, trace, and both firmware-role crash
   buffers before reboot. Restore the normal fleet entry after each arm.

For `B`, no logical queue may have more than one published front per engine. A schedule event at
phase 2/detail 1 is host backlog held by the owner credit; phase 3 without a later publish is a
kernel publication stall. A successful submit phase 2 names its installed output fence; if that
fence becomes terminal but the application queue point does not, the remaining hole is in the
userspace private timeline. Any B firmware crash rejects the arm even when userspace reports
PASS. If a crash occurs, require RTKit to select `CLGH` through `CLHE` and print the `Crl4`
PC/ESR/FAR rather than a poison header.

Only after two clean B boots, optional performance arms may set `asahi.g17_publication_credits`
to 2, 4, 8, then 16, with two one-shot boots per value and the same order. Stop at the first crash,
timeout, missing stamp, or device loss; this sweep does not change the safe default.
