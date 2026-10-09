# Hibernation on Apple silicon

Aurora has no supported hibernation path on Apple silicon yet. The J413 M2
MacBook Air request is tracked in [#99](https://github.com/iconidentify/aurora-linux/issues/99).
The kernel's [availability check](../../kernel/power/hibernate.c) explicitly
refuses `apple,arm-platform`, independently of `CONFIG_HIBERNATION`.
`CONFIG_ARCH_HIBERNATION_POSSIBLE` describes generic ARM64 support; it does not
establish Apple platform support.

## Platform prerequisites

* **CPU park and restart:** [T8112 CPUs](../../arch/arm64/boot/dts/apple/t8112.dtsi)
  use `spin-table`. Its [CPU operations](../../arch/arm64/kernel/smp_spin_table.c)
  provide startup but no `cpu_die` callback. [ARM64 CPU offlining](../../arch/arm64/kernel/smp.c)
  refuses that operation, and [image creation](../../arch/arm64/kernel/hibernate.c)
  independently rejects CPUs that cannot leave the running kernel. A supported
  CPU shutdown/restart mechanism is required; disabling the check is not one.
* **Devices after a cold boot:** [Apple DRM](../../drivers/gpu/drm/apple/apple_drv.c)
  registers suspend/resume callbacks without hibernation freeze/restore
  callbacks. [DCP](../../drivers/gpu/drm/apple/dcp.c) keeps its power domains on
  during sleep because full shutdown is not supported. Hibernation must restore
  display and GPU firmware sessions and DMA ownership after actual power loss,
  rather than rely on retained firmware. NVMe and DART already have system-sleep
  callbacks; their presence alone does not qualify image restoration.
* **Boot and image compatibility:** the resume kernel must safely recover the
  old physical-memory image while respecting the new boot's firmware-reserved
  memory and device mappings. [m1n1 boot setup](https://github.com/AsahiLinux/m1n1/blob/3e354a2467f4f724f254362626cae0633918e0c1/src/kboot.c)
  supplies CPU release addresses and derives usable RAM from the current boot's
  BootArgs and firmware memory map. ARM64 checks the saved kernel build and CPU
  identity in its [hibernation header](../../arch/arm64/kernel/hibernate.c).
  The matching kernel, modules and boot entry must remain available while an
  image is pending.
* **Storage and authentication:** an image needs persistent storage, enough
  free swap capacity, and an initramfs resume path before mounting the saved
  system's filesystems. Encryption keys must be available after power-off;
  volatile zram cannot hold that image. The [SEP sleep notifier](../../drivers/soc/apple/pm_shim.c)
  already fences captures and entropy requests for hibernation events. That
  does not establish cold-resume Touch ID or secure session restoration.

## Qualification before enabling a user command

First implement CPU lifecycle and cold device recovery, then qualify an
isolated J413 build with the kernel's staged [PM tests](https://docs.kernel.org/power/basic-pm-debugging.html).
Actual image tests must cover repeated reboot and power-off restoration with
the same kernel, both firmware versions recorded, and a usable recovery entry.
They must preserve files and the running session, return working storage,
network and GPU/display output, keep the desktop locked until authentication,
and pass password, fingerprint and fresh sudo authentication. Corrupt or
incompatible images must be rejected without restoring stale memory or
mounting filesystems before the resume decision.

The J413 report records one successful `s2idle` cycle, including secure wake
and authentication. It establishes neither hibernation nor repeated-resume
reliability or long-idle battery use. `s2idle` keeps memory powered. Saving work
and shutting down is the current powered-off option, without preserving the
running session.

The [kernel hibernation documentation](https://docs.kernel.org/power/swsusp.html)
covers image storage and resume ordering. Enabling a config option, adding swap
or unmasking a target does not supply the platform prerequisites above.
