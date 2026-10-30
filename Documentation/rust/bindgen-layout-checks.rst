.. SPDX-License-Identifier: GPL-2.0

Checking generated binding layouts
==================================

Every Rust build retains bindgen's unconditional compile-time assertions for
size, alignment and field offsets. A mismatch stops the bindings crate build
with an ``E0080`` error naming the C type and property. These assertions are
required even when KUnit is disabled; ``BINDGEN_LAYOUT_TESTS=0`` cannot disable
them. Build the complete Image and all modules with the same generator.

A successful C build does not validate Rust field offsets. In particular,
``CONFIG_SCHEDSTATS=n`` leaves a zero-sized, cacheline-aligned
``sched_statistics`` in ``task_struct``. Stock bindgen 0.73.2 loses its alignment
and can shift subsequent fields, including ``mm``. The ARM64 defconfig +
Asahi configuration exposed a 40-byte difference: Rust read a scheduler list
pointer as the current mm and Asahi's RSS charge faulted in
``percpu_counter_add_batch``. The C RSS helpers and allocator agreed on the
counter layout; the pointer supplied to them was wrong. Enabling SCHEDSTATS
only masks the generator defect and is not a requirement of the driver.

Known generator limitations
---------------------------

Unmodified bindgen 0.73 does not reproduce every C layout used by the kernel:

* It omits a named anonymous record member, which Clang accepts with
  ``-fms-extensions``, in a declaration such as::

      struct payload { unsigned long words[20]; };
      union container {
          struct payload;
          unsigned long other;
      };

* It can drop the explicit alignment of a defined zero-sized C struct, such
  as ``struct empty {} __attribute__((aligned(64)));``.

The earlier Fedora configuration exposed only trailing size differences for
some namespace types. That observation does not extend to defconfig: an
aligned empty member can also shift field offsets. Neither size nor offset
mismatches are safe to ignore. The affected types depend on the generator
version and the resolved kernel configuration, so validate each build.

Corrected generator
-------------------

``tools/aurora-bindgen/`` contains pinned patches for bindgen 0.73.2 that
repair both cases, a local build recipe and compile-only tests; see
``tools/aurora-bindgen/README.rst``. Prepare it once before configuration::

    python3 tools/aurora-bindgen/build.py
    make LLVM=1 ...

The top-level Makefile automatically selects
``tools/aurora-bindgen/build/bin/bindgen`` from the source tree, also for ``O=``
builds. Packagers prepare this entry point in their build roots. Make does not
fetch tools or install anything system-wide. If the entry point is absent,
Kbuild uses ``bindgen`` from PATH and the same mandatory assertions apply.

The generated bindings are consumed by the Rust kernel crate and by several
drivers. After changing the bindings generator, rebuild the kernel Image and
all modules together.

The assertions need bindgen 0.71.1 or later (the kernel minimum) together with
the configured ``--rust-target 1.85``: that selects unconditional constant
assertions rather than ``#[test]`` functions. They are checked without a
Rust test configuration.

Check the recorded bindings command, not just the toolchain version banner.
Command-line overrides remain available for generator regression tests.
The defect can affect any generated aligned empty record and containing types
under other configurations; all emitted size/alignment/offset checks remain
mandatory. They do not cover C-only types or opaque internal fields. See the
generator README for the upstream status and related empty-struct issues.
