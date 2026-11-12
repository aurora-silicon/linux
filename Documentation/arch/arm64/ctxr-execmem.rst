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

    ``base`` and ``size`` are in bytes, page aligned and not 0. See
    ``Documentation/devicetree/bindings/chosen/aurora,ctxr-exec-window.yaml``.

Command line (bring-up / VM)
    ::

        ctxr_exec_window=<size>@<base>   e.g. ctxr_exec_window=128M@0x40200000
        ctxr_exec_enforce=<bool>         (default: enforcing)

    ``size`` and ``base`` accept the usual ``K``/``M``/``G`` suffixes, and both
    are required. The last use of an option wins. The kernel must be at EL2.
    When the device tree also carries a window, the command-line one is used
    only if it is identical; a different one is ignored with a warning and the
    device-tree window is used, because the whole firmware window has to be
    reserved (see below). A value that doesn't parse is ignored with a warning.

The secondary CPUs can only run code inside the window, and the boot CPU isn't
confined. So when a window is given but can't be used, the kernel logs why,
leaves the secondaries in the bootloader and boots on the boot CPU alone, as with
``nosmp``. That happens when the device-tree property is malformed (not four
cells, a base or size of 0 or not page aligned, or a window that wraps), when the
image doesn't fit the window or the window isn't in DRAM, when it can't be
reserved, when no free memory is left in it for executable allocations, when the
command-line window isn't inside the device-tree one, and when the kernel isn't
running at EL2 (``kvm-arm.mode=nvhe`` or ``protected``, for example). With no
window from either place, nothing changes.

The kernel doesn't check the device tree's window against ``CTXR_D``. The
bootloader reads ``CTXR_D`` when it chooses the window and reserves the rest of
the armed range, so the check belongs there. The registers also don't exist on
every Apple core, and an undefined system register read this early can't be
recovered.

``ctxr_exec_enforce=`` only decides whether a violation found by the boot-time
verifier (see below) is fatal, for a window from either place. Whether the
hardware confines a core does not depend on it, so everything else in this
document applies whenever a window is active, enforcing or not.

How it works
============

* ``ctxr_exec_reserve()`` (from ``arm64_memblock_init()``) parses the window,
  checks that ``[_text, _end)`` fits inside it, and reserves it from memblock.
* ``execmem_arch_setup()`` carves the window's free memory — a snapshot of the
  ranges still free when the window was reserved, so the image and everything
  else already reserved stay out — into a ``gen_pool`` and registers it with the
  generic confinement core
  (``CONFIG_EXECMEM_CONFINE``). The executable ranges (module text, BPF,
  kprobes and, by inheritance, ftrace) are flagged ``EXECMEM_CONFINE``.
* A confined allocation reserves its virtual area in the normal module range —
  preserving the +/-2 GiB branch reachability to the kernel — but backs it with
  physical pages drawn from the window. Module *data* is not confined.
* If the window's pool is exhausted the allocation fails, exactly as a hardware
  CTXR fault would prevent executing from outside the window; there is no buddy
  fallback.
* Code in the window can't execute at EL1 on a confined core. On an Apple core
  with a part number from 0x50 that implements VHE, the boot CPU's first entry
  switches to VHE in ``init_kernel_el`` without running at EL1, with or without a
  window: the J616s (T6040) boot CPU doesn't survive the trip through EL1 inside a
  window, and the kernel can't tell this early whether it is in one. Such a boot
  CPU stays in VHE whatever ``kvm-arm.mode`` and ``id_aa64mmfr1.vh`` say. Any
  other boot CPU enters as it always has, through EL1 and ``finalise_el2``. Once
  the boot CPU has reserved a window at EL2, it sets a flag that later entries
  read with the MMU off, and those switch to VHE the same way. With no window,
  later entries go as they always have.

Userspace
=========

On a core that enforces the window, EL0 can't execute from it either. The fetch
takes a permission fault that the page tables don't explain, so nothing can fix
it up. Anything EL0 can execute therefore lives outside the window:

* The vDSO's code is part of the image, so userspace gets a copy in ordinary
  memory.
* The whole reserved window stays out of the page allocator. The reservation
  covers the entire window (which is why a command-line sub-window is refused),
  ``free_reserved_area()`` skips window pages so freed ``__init`` memory and an
  initrd in the window stay reserved, a CMA area that overlaps the window isn't
  activated, and a reserved-memory node whose init fails stays reserved. RAM in
  the window that the pool doesn't use is not available to the rest of the
  system, so a window should be no larger than the executable code needs.

* The zero page is part of the image too, and every untouched anonymous page
  that is read before it is written maps it, executable mappings included. With
  a window active, ``arch_setup_zero_pages()`` allocates the zero pages from
  memblock instead, outside the window.

The hardware CTXR is what actually stops an out-of-window EL0 fetch on a
confined core. As a diagnostic backstop, when a window is active an EL0
instruction permission fault on a user-executable mapping of a window page logs
the task, address and PFN and kills it with an uncatchable ``SIGSEGV`` rather
than letting it retry forever. The PFN is taken from the mapping's folio, or for
a ``VM_PFNMAP`` mapping with no struct page (a driver's ``remap_pfn_range()`` of
an image buffer) through ``follow_pfnmap()``; a special ``VM_MIXEDMAP`` PTE with
no struct page isn't resolved here and is left to the hardware fault. Any other
fault takes the usual path.

EFI runtime services
====================

EFI runtime code runs at EL2 on whichever core ``efi_rts_wq`` happens to be on,
so on a confined core it has to be inside the window too. When a window is
active and any ``EFI_RUNTIME_SERVICES_CODE`` region is outside it, or isn't
aligned to the kernel's page size, the kernel warns once and turns runtime
services off for that boot, as ``efi=noruntime`` would. When all of it is inside,
runtime services stay on.

What a confined machine gives up
================================

CPU hotplug
    The kernel runs at EL2 with no monitor below it, so no PSCI, and CPUs start
    through spin-table, which has no way to take a CPU down. The CPUs have no
    ``online`` file in sysfs, and anything that needs them offline fails.

kexec and crash kernels
    The relocation code runs from an ordinary page, outside the window, and a
    crash dump can start on a confined core, where it would hang during a panic.
    While a window is active, ``machine_kexec_prepare()`` refuses both, so
    ``kexec_load()`` and ``kexec_file_load()`` fail with ``EOPNOTSUPP``. A
    ``crashkernel=`` reservation would then only waste memory, so it is skipped
    with a warning when a window is active.

Hibernate
    Resume runs from a ``trans_pgd`` page outside the window. This is unchanged
    and untested under a window.

Verifying confinement
=====================

At ``late_initcall`` the kernel sweeps its page tables, and ``efi_mm`` when
runtime services are on, and logs the number of EL1-executable leaf mappings and
how many resolve outside the window. It also counts window pages that reached
the page allocator, and confined allocations whose backing is outside the
window, as violations. Any violation is fatal unless ``ctxr_exec_enforce=0`` is
given. This is also what stands in for the hardware fault when testing in a VM.
The check runs once: memory freed after it, such as init memory, relies on
``free_reserved_area()`` skipping window pages.

When a window is active, ``/sys/kernel/execmem_confine/`` reports it:
``window_base`` and ``window_size``, and the pool's ``pool_size``, ``pool_free``
and ``pool_largest_free`` (bytes). These make an allocation failure (for example
an ``insmod`` ``ENOMEM``) on a running machine diagnosable, and give a tool a
stable place to read the window from.
