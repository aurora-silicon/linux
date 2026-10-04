.. SPDX-License-Identifier: GPL-2.0-only

Private bindgen with checked C/Rust layouts
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

Build in a fresh private directory::

    python3 tools/aurora-bindgen/build.py --output-dir /absolute/new/tool-build

Python 3.12 or newer, Cargo/Rust, Clang/libclang and patch are required. Cargo's
locked dependency graph is used, with at most two build jobs. ``--archive`` can
supply an already downloaded source archive; its digest is still checked. The
script builds and runs compile-only valid-input checks, then prints the exact
``BINDGEN=/absolute/path/to/bindgen`` make argument. It installs nothing globally.
Use that argument for the complete kernel Image and matching modules build.

Tests and target accounting
---------------------------

``test.py`` independently compiles the C/C++ fixtures and generated Rust. It
covers empty alignments 1/2/8/64/128, nesting, packed empty records, anonymous
struct/union members, field access and ordinary tag-only declarations. It
requires unconditional const layout assertions in the generated output.
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

Repository guard and supported tool versions
--------------------------------------------

The kernel keeps bindgen layout checks enabled. Its minimum bindgen is 0.71.1,
and its fixed ``--rust-target 1.85`` enables the ``offset_of`` feature (available
from Rust 1.77). In both official 0.71.1 and the pinned 0.73.2 generator, that
feature selects unconditional const size/alignment/offset assertions rather
than ``#[test]`` functions. A normal bindings crate compile therefore checks
layouts without requiring a Rust unit-test configuration. This is source-backed
for the accepted minimum and pinned tool; future generator changes must retain
that capability. Never remove the assertions to accept an incompatible tool.

Primary sources:

* https://github.com/rust-lang/rust-bindgen/blob/v0.71.1/bindgen/features.rs
* https://github.com/rust-lang/rust-bindgen/blob/v0.71.1/bindgen/codegen/mod.rs
* https://github.com/rust-lang/rust-bindgen/tree/v0.73.2

The complete kernel build remains the configuration-specific layout gate; these
small generator fixtures do not replace it or establish runtime GPU stability.
