# J514S native DCP checkpoint

Explicit-load native display prototype for the M3 Pro J514S, using the inherited
DCP firmware UUID DDF38191-93B3-324A-BC8F-643006F5AC82. The corresponding captured
TEXT SHA256 is a0af36d10a8abf2435918ba360e697529ddfaed56b89f9466e76d566b296e876.
No firmware image is included here. Do not generalize to another M3 or firmware.

The isolated m1n1 handoff reserves every DCP firmware segment and publishes the
locked display SID0 and PIODMA SID4 mappings. The power-hold helper uses PMGR's
existing syscon to retain DISP_SYS/FE/CPU before PMP startup; forcing CPU_RUN
on a suspended DCP is not a supported recovery method. Reset by reboot.

Build against the configured UNSTABLE kernel with ARCH=arm64 LLVM=1 and this
directory as M=. Requires DRM_GEM_DMA_HELPER, BACKLIGHT_CLASS_DEVICE and the normal Apple RTKit, DART,
mailbox and PMP providers. A partial kernel build may leave Module.symvers
without the already-built drm_dma_helper exports; restore that dependency's
symbol metadata before modpost. Do not bypass missing-symbol checks.

Load m3_dcp_hold before the M3 PMP overlay/thermal/bridge modules. Under the M3
watchdog/thermal guard, load drm_dma_helper and backlight, then m3_dcp_rtkit kernel_client=1.
After successful startup, root may write background or kms to the control
parameter. KMS takeover removes simpledrm and exposes one native3024x1964 eDP
mode. Stop the display manager before a manual takeover. No automatic probe or
module unload: firmware-visible allocations remain pinned until reboot.

Hardware evidence on2026-09-18 includes successful service initialization,
solid-background completion, three real KMS flips over three prefilled buffers,
all5,939,136 expected snapshot pixels, and the normal Plasma greeter on native
DCP after allowing llvmpipe's3072x1984 padded allocation. The visible framebuffer
and plane must remain3024x1964. Preferred timing metadata says120Hz; the bounded
flip test measured60FPS. A host snapshot is not optical panel readback.

This is a fixed-mode display checkpoint, not a full accelerated-desktop claim.
GPU rendering qualification is separate. Linear XRGB/ARGB only; display domains
are held active, runtime display power management remains unqualified, and the
initial firmware panel charge-parity diagnostic is retained in evidence. A
plane disable blanks to black; panel power-off has not been qualified.

Workspace tools/m3-dcp contains the guarded bootstrap, persistent recovery latch,
offline firmware decoders, lifecycle capture and userspace flip/login checks.
Evidence: _provenance/m3-dcp-20260918 in the source-collection workspace.

## Visible panel fix and reboot validation (2026-09-18)

The earlier checkpoint established software completions and host-buffer pixels,
but the user reported a dark physical panel. PMP startup alone caused the
blackout. The legacy device-power mailbox only returns a constant ACK on this
PMP v2 firmware; it does not establish the requested DISP power state.

Captured macOS PTD state and driver analysis identified request index280 and
ACK index284. The loader now optionally seeds DISP bit7 and ANS bit16 through
an ioremap_np mapping before starting PMP. DISP requires ACK; ANS does not.
The native client verifies status1 and the DISP request/ACK before takeover.
Use the recorded lifecycle --hold --ptd-seed sequence, not the old mailbox.

The user confirmed visible native3024x1964 login output both after takeover
and after a normal UNSTABLE reboot. Guarded login checks observed new frames
and zero reported failures; all five PMP channels remained below47C. This is
visible internal-display support, not accelerated-desktop qualification.

The exact bootstrap/diagnostic source snapshot is in
../../asahi/m3_lab/tools/m3-dcp; the PMP loader is in its sibling
m3-boot-export. Snapshot hashes and validation summaries are under m3_lab.
Runtime deployment remains /root/m3-gpu-tools: lifecycle.py is installed as
m3-dcp-lifecycle.py and start.py as m3-dcp-start.py; boot.py, boot-client.py,
control.py and m3-dcp.service retain their names. The existing guard and
watchdog helpers are required. Preserve the failure latch, normal display
manager, original Asahi fallback and five-second GRUB menu.

## Panel dimensions (2026-09-21)

The J514S firmware's serialized DisplayAttributes contains product identity but
no image dimensions. The board's DCP panel child therefore supplies width-mm=302
and height-mm=196, following the existing M1/M2 panel binding. These are the
full 3024x1964 panel dimensions at Apple's specified 254 ppi, rounded to whole
millimetres (https://support.apple.com/en-ie/117736). Do not use the notch-hidden
189 mm height with full-height native scanout.

KMS copies these dimensions into its preferred mode and DRM connector display
information on each mode probe. An old DT without panel data retains display
operation and emits a warning rather than fabricating dimensions or an EDID.
There is no desktop scale in the driver. Deploy both the module and updated
J514S DT in the m1n1 bundle. tools/m3-dcp/prepare-boot.py preserves the other
40 boards and U-Boot; verify the installed bundle before replacement.

KWin preserves previously saved output configurations. Removing just the scale
key from an existing setup did not trigger automatic scale selection: KWin
retained 1x. To validate automatic selection on this single-panel installation,
stop the display manager, terminate the user session, wait for KWin to exit,
then archive kwinoutputconfig.json before restarting. Retain the archive for
restoring other output preferences; do not discard multi-monitor layouts.

After a reboot into the corrected driver, DRM and KWin both reported 302x196 mm.
With the old setup archived, unmodified KWin 6.7.4 selected 1.7x automatically
(logical desktop 1779x1155) and saved that choice. No replacement scale was
supplied. KWin's current heuristic uses a 150-DPI target for internal outputs
without a detected laptop lid switch; its laptop-with-lid target is 125 DPI.
This distinction is userspace/input policy, not a reason to alter panel size.
The guarded real Plasma check, GPU rendering and screenshot passed. Evidence:
validation/panel-dpi-20260921 (raw screenshot retained in workspace build).

## DRM timeline synchronization (2026-09-22)

Internal and external prototype DRM devices now expose the core SYNCOBJ and
SYNCOBJ_TIMELINE features, as the main apple_drv.c already does. A display-only
node can import fences from the separate render node; it needs no local shader
engine for DRM core to implement timelines. Without these flags, KWin on J514S
did not advertise wp_linux_drm_syncobj_manager_v1. After the guarded normal
boot, it does, and the Vulkan game confirms explicit=1 instead of implicit sync.

`syncobj-probe.py DISPLAY_NODE RENDER_NODE` is CPU-only: it checks capability
queries, exports/imports an opaque timeline in both directions, verifies timeout
before signaling point 7, then successful wait and eventfd notification. It
does not submit GPU work or count as GPU synchronization qualification. The
J514S card1/renderD128 probe passes both directions. Native Vulkan 0 A.D. runs
with real explicit-sync presentation under the normal M3 guard, at 98.102 FPS
in the short fixed-scene sample. The previous implicit diagnostic was 94.901;
this is not a matched sustained performance claim and remains below 120 FPS.

Boot: cd0c4c47-1206-435b-a4b5-b33f8f3a1e73. Private evidence:
`build/m3-0ad-120/dcp-syncobj/` and `0ad-120-vk-explicit-sync/`. External module
variants build, but no external monitor is attached in this qualification.

## Zero-nit internal-panel recovery (2026-09-24)

The recurrent internal-dark/external-working state was retained on boot
`ab566889-62ce-4df2-b5d1-63ab03423a80`. KMS was active and completing frames,
while the qualified firmware snapshot reported backlight enabled, correction
1.0, and `last_backlight_nits = 0`. Direct writes to cached brightness fields
were ineffective; they are not a supported interface and were restored.

Firmware analysis identified the M3 A407 brightness-update byte at 0x354 and
unaligned binary64 nits at 0x35c. M4 uses 0x35e for the latter. The M3 firmware
copies these to transaction+0x76a/+0x778 and converts nits into runtime property
19 through its normal backlight pipeline. A serialized A406/A407 transaction
requesting 140 nits completed successfully (swap 33762) and a subsequent RAM
snapshot reported 140 nits. There was no reboot, DCP reload or desktop restart.
The user subsequently confirmed both screens visible: the internal panel
recovered and the external monitor remained working. This is optical
confirmation of the live recovery, not validation of the unloaded replacement
driver or of repeated wake/hotplug cycles.

The normal driver now exposes `apple-panel-bl` through the backlight class,
with an SDR range of 0..500 nits and an initial request of 140 nits. It sends
zero on CRTC disable and the retained request on enable. Explicit writes to
the backlight device re-send the request even if its value is unchanged, and
work with an idle compositor by retaining and re-submitting the active FB.
Power-on/CRTC-enable invalidate the last acknowledged brightness; only a
completed, successful swap updates that cache. Background-only diagnostic
commands retain the existing brightness. No raw RAM writes are part of this
implementation.

Validation so far: the live 0-to-140 command above, an arm64 module build for
a 7.1.9 Asahi development kernel, and the actual production encoder tested against the
host IEEE754 implementation for all 501 levels, including unchanged bytes
outside the two wire fields, under ASan/UBSan. Run the offline test with:

```
python3 drivers/gpu/drm/asahi/m3_lab/tools/m3-dcp/test-brightness.py \
  drivers/gpu/drm/apple/tools/m3-dcp
```

The complete replacement driver has not been loaded in the preserved boot.
Backlight-class integration, repeated DPMS cycles and hotplug still need a
subsequent hardware validation; the initiating firmware power event has not
yet been identified. Do not describe this checkpoint as a fully qualified
wake/hotplug regression fix.

One temporary diagnostic had an unrelated host-kernel BTI oops before its
first RPC: a private static driver function was called indirectly without a
BTI landing pad. Its abandoned mutex blocked internal submissions for about
422 seconds. The dead owner's address was verified from the register dump;
a later idle worker had reused that task allocation. A same-boot, pointer-
and birth-time-qualified recovery released that abandoned lock; ordinary KMS
submissions resumed. The successful diagnostic used a range-checked direct
branch. Neither diagnostic nor manual mutex recovery belongs in the driver.
The failed module remains pinned and the kernel is tainted until a future
reboot; no forced unload or reboot was attempted. All raw captures, temporary
helper sources/binaries and logs remain local under `build/m3-panel-wake` and
`tools/m3-dcp/live-brightness` in the source-collection workspace.
