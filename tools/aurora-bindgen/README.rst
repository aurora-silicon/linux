.. SPDX-License-Identifier: GPL-2.0-only

Default bindgen with checked C/Rust layouts
==========================================

This package builds bindgen 0.73.2 with two generic binding-generation repairs:
preserve explicit alignment on defined zero-sized structs, and retain actual
unnamed named-tag fields exposed by Clang's type-field visitor. The latter is
needed for the Microsoft anonymous-member extension used by kernel namespaces.
Neither repair changes C structure storage or kernel configuration.

``sources.json`` pins the official source archive, Cargo.lock, both patches and
the patched source files. It also records the qualified private tool's digest;
that digest is evidence for that build, not a promise that a different host,
compiler or build path produces identical bytes.
The manifest identifies when that digest predates comment-only patch changes;
each rebuild must record its own binary digest.

Prepare the default generator once before configuring or building the kernel::

    python3 tools/aurora-bindgen/build.py
    make LLVM=1 ...

Python 3.12 or newer, Cargo/Rust, Clang/libclang and patch are required. Cargo's
locked dependency graph is used, with at most two build jobs. The script builds
in ``tools/aurora-bindgen/build/`` and publishes ``build/bin/bindgen`` only after
its compile-only checks pass. The top-level Makefile selects that entry point
automatically for both in-tree and ``O=`` builds. No make variable or environment
setting is needed. It installs nothing globally and make never downloads or
builds the generator implicitly. Preserve the build log and ``build/result.json``.

``--archive`` can supply an already downloaded source archive; its digest is
still checked. ``--output-dir`` supports separate tool experiments; a nondefault
location is not automatically selected. Packagers may instead place their
independently built and tested pinned tool at the same ``build/bin/bindgen``
entry point (a symlink is sufficient). Remove or move a previous build directory
before rebuilding; the script refuses to overwrite it.

If the prepared entry point is absent, Kbuild falls back to ``bindgen`` on PATH.
The mandatory layout assertions still reject incorrect bindings. A command-line
override remains available for generator regression testing.

Both scripts reject optimized Python (``-O``, ``-OO`` or ``PYTHONOPTIMIZE``)
before creating output, since their validation assertions must remain enabled.

Tests and target accounting
---------------------------

``test.py`` independently compiles the C/C++ fixtures and generated Rust. It
covers empty alignments 1/2/8/64/128, nesting, packed empty records, anonymous
struct/union members, field access and ordinary tag-only declarations. It
requires unconditional const layout assertions in the generated output.
An additional bitfield fixture rejects unknown Rust lints in the fixed tool's
output. This fixture checks compiler compatibility, not the layout repairs,
and is excluded from the optional unpatched-baseline comparison.
Nothing is executed on a GPU, and the generated object files are not run.

Example using an existing private tool and the installed unpatched baseline::

    python3 tools/aurora-bindgen/test.py \
      --bindgen /absolute/fixed/bindgen --baseline-bindgen /usr/bin/bindgen \
      --output-dir /absolute/new/check-results

The optional baseline must be an affected unpatched tool: alignment/member
fixtures must fail at compile time, while tag-only controls must pass unchanged.
Without that option only the repaired generator is checked. The Rust target is
always passed explicitly, defaulting to the Rust compiler's reported host; the
Clang target defaults to the same triple. ``--rust-target`` and ``--clang-target``
allow explicit kernel/cross targets, with matching architecture required.
``--rust-sysroot`` and ``--rust-libdir`` support a separately prepared Rust core.
An unavailable Rust target/core emits a machine-readable SKIP and exit 77;
that target has not passed. Other compiler or layout errors fail normally.

Select ``--rustc`` explicitly to check the compiler used for the kernel. The
generator's ``--rust-target 1.85`` selects emitted language features; it does
not select the Rust compiler used by the tests. A compatibility adapter may
remove an exact generated lint attribute unsupported by that compiler, while
retaining all layout assertions. Its underlying binary must be the corrected
generator, and its changes and both tool digests belong in the build record.

Repository guard and supported tool versions
--------------------------------------------

Every kernel build keeps the layout checks (see
``Documentation/rust/bindgen-layout-checks.rst``). Stock bindgen can fail these
checks for valid configurations; use the corrected generator rather than
disabling the assertions or changing configuration to hide a layout bug. Its minimum bindgen is
0.71.1, and its fixed ``--rust-target 1.85`` enables the ``offset_of`` feature
(available from Rust 1.77). In both official 0.71.1 and the pinned 0.73.2
generator, that feature selects unconditional const size/alignment/offset
assertions rather than ``#[test]`` functions. A bindings crate compile therefore checks layouts without requiring a Rust
unit-test configuration. This is source-backed for the accepted minimum and
pinned tool; future generator changes must retain that capability.

Primary sources:

* https://github.com/rust-lang/rust-bindgen/blob/v0.71.1/bindgen/features.rs
* https://github.com/rust-lang/rust-bindgen/blob/v0.71.1/bindgen/codegen/mod.rs
* https://github.com/rust-lang/rust-bindgen/tree/v0.73.2

A complete kernel build remains the
configuration-specific layout check; these small generator fixtures do not
replace it or establish runtime GPU stability.

Upstream status and affected configurations
------------------------------------------

As checked on 2026-10-26, the latest upstream release is 0.73.2 and Linux's
minimum is 0.71.1. Both versions, and bindgen main, skip explicit alignment
handling for zero-sized non-opaque structs. Raising the minimum therefore does
not fix this defect. Upstream issue #551 and fix #552 concern C empty-struct
size, while #1683 concerns the Windows ABI; neither fixes this aligned-empty
case. No exact upstream issue or merged fix was found in the issue/PR search.

The defect is generic: any explicitly aligned empty C record can have the wrong
Rust alignment, and embedding it can shift later fields or change enclosing
size/alignment. Configurations that empty conditional records, change cacheline
alignment or change preceding fields can expose it; it is not specific to
Asahi or ARM64. SCHEDSTATS=y masks this particular scheduler case. Every emitted
layout assertion is compiled regardless of KUnit, but this only covers types
and fields emitted by bindgen, not arbitrary C-only or opaque internals.

* https://github.com/rust-lang/rust-bindgen/releases/tag/v0.73.2
* https://github.com/torvalds/linux/blob/master/scripts/min-tool-version.sh
* https://github.com/rust-lang/rust-bindgen/blob/main/bindgen/codegen/mod.rs
* https://github.com/rust-lang/rust-bindgen/issues/551
* https://github.com/rust-lang/rust-bindgen/pull/552
* https://github.com/rust-lang/rust-bindgen/issues/1683
