.. SPDX-License-Identifier: GPL-2.0

==========================================
Confining kernel executable code (CTXR)
==========================================

Some Apple-silicon machines (T8140/M4-class and later) have boot firmware that
arms a per-CPU *code text execute range* (CTXR) on the secondary cores: at the
privileged exception level, an instruction may only be fetched from inside a
fixed physical window. Outside it, the fetch takes an instruction abort.

To run the kernel at EL2 behind such a window (instead of dropping it to EL1),
**every** executable physical page must live inside one window: the static image
(placed there by the bootloader) and every run-time executable allocation.

``CONFIG_ARM64_CTXR_EXECMEM`` makes that possible. It is inert unless a window
is supplied at boot, so it is safe to leave enabled on machines that do not need
it.

Supplying the window
====================

Device tree (production)
    The bootloader emits, under ``/chosen``::

        aurora,ctxr-exec-window = <base_hi base_lo size_hi size_lo>;
        aurora,ctxr-exec-enforce = <1>;   /* optional, default 1 */

    A ``base`` of 0 tells the kernel to anchor the window on its own loaded
    image (rounded down to 2 MiB). ``size`` is the window size in bytes.

Command line (bring-up / VM)
    ::

        ctxr_exec_window=<size>[@<base>]   e.g. ctxr_exec_window=128M
        ctxr_exec_enforce=0                 (default: enforcing)

    The command line overrides the device tree. ``size`` accepts the usual
    ``K``/``M``/``G`` suffixes; if ``@base`` is omitted it is derived from the
    kernel image.

How it works
============

* ``ctxr_exec_reserve()`` (from ``arm64_memblock_init()``) parses the window,
  checks that ``[_text, _end)`` fits inside it, and reserves it from memblock.
* ``execmem_arch_setup()`` carves the non-image remainder of the window into a
  ``gen_pool`` and registers it with the generic confinement core
  (``CONFIG_EXECMEM_CONFINE``). The executable ranges (module text, BPF,
  kprobes and, by inheritance, ftrace) are flagged ``EXECMEM_CONFINE``.
* A confined allocation reserves its virtual area in the normal module range —
  preserving the +/-2 GiB branch reachability to the kernel — but backs it with
  physical pages drawn from the window. Module *data* is not confined.
* If the window's pool is exhausted the allocation fails, exactly as a hardware
  CTXR fault would prevent executing from outside the window; there is no buddy
  fallback.
* Code in the window can't execute at EL1 on a confined core, and the window
  isn't known at the first instruction, so cores from the M4 generation on switch
  to VHE in ``init_kernel_el`` on every entry at EL2. Earlier cores don't have
  CTXR and enter as they always have.

Userspace
=========

On a core that enforces the window, EL0 can't execute from it either. The fetch
takes a permission fault that the page tables don't explain, so nothing can fix
it up. Anything EL0 can execute therefore lives outside the window:

* The vDSO's code is part of the image, so userspace gets a copy in ordinary
  memory.
* No window page reaches the page allocator. ``free_reserved_area()`` skips them,
  so freed ``__init`` memory stays reserved, as does an initrd in the window. RAM
  in the window that the pool doesn't use is not available to the rest of the
  system, so a window should be no larger than the executable code needs.

The zero page stays in the window, so a program that jumps into fresh executable
memory before writing it still gets there. When the window is enforcing, an EL0
instruction permission fault on a present, executable mapping of a window page
logs the task and its address and PFN, then kills it with an uncatchable
``SIGSEGV`` rather than retrying. Any other fault takes the usual path.

EFI runtime services
====================

EFI runtime code runs at EL2 on whichever core ``efi_rts_wq`` happens to be on,
so on a confined core it has to be inside the window too. When a window is
enforcing and any ``EFI_RUNTIME_SERVICES_CODE`` region is outside it, or isn't
aligned to the kernel's page size, the kernel warns once and turns runtime
services off for that boot, as ``efi=noruntime`` would. When all of it is inside,
runtime services stay on. A window in reporting mode leaves them on.

kexec and crash kernels
=======================

The relocation code runs from an ordinary page, outside the window, and a crash
dump can start on a confined core, where it would hang during a panic. While a
window is enforcing, ``machine_kexec_prepare()`` therefore refuses both, so
``kexec_load()`` and ``kexec_file_load()`` fail with ``EOPNOTSUPP``.

Verifying confinement
=====================

The feature carries its own verifier, which is what stands in for the hardware
fault when testing in a VM:

* At ``late_initcall`` the kernel sweeps its page tables, and ``efi_mm`` when
  runtime services are on, and reports the number of EL1-executable leaf mappings
  and how many resolve outside the window. When the window is enforcing, a
  non-zero count is fatal. It also counts window pages that reached the page
  allocator as violations.
* ``/sys/kernel/debug/execmem_confine`` re-runs the sweep on read and reports
  window bounds, pool usage and ``PASS``/``FAIL``. Init memory is freed after
  ``late_initcall``, and an initrd can be, so this is the check that covers them.
* Booting with ``ctxr_exec_selftest`` runs two extra checks: that a confined
  allocation is physically inside the window, and that the sweep actually
  detects an executable mapping placed *outside* it (proving the verifier is
  not a no-op).
