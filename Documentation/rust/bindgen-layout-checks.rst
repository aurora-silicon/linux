.. SPDX-License-Identifier: GPL-2.0

Checking generated binding layouts
==================================

Kernel bindings retain bindgen's layout checks. With the configured Rust target,
these are unconditional constant assertions, checked while the bindings crate
is compiled. They compare Rust sizes, alignments and field offsets against the
values obtained from Clang, without executing kernel code.

Do not disable these checks to make an incompatible binding generator compile.
A type can have the right size and still have incorrect member offsets. For
example, an empty GNU C struct may have size zero and alignment 64; losing that
alignment moves later fields in an enclosing struct without necessarily
changing the enclosing size.

Known generator requirements
----------------------------

Unmodified bindgen 0.73.2 loses explicit alignment on defined zero-sized C
structs. It also omits the named anonymous record member accepted by Clang's
``-fms-extensions`` in a declaration such as::

    struct payload { unsigned long words[20]; };
    union container {
        struct payload;
        unsigned long other;
    };

The generator used for this tree must preserve these layouts. Use a corrected
bindgen build through the normal ``BINDGEN=/path/to/bindgen`` make variable.
``tools/aurora-bindgen/`` contains pinned patches, a private build recipe and
compile-only valid-input tests. It never installs a system-wide tool. The
associated build provenance must retain the generator source revision,
patches, binary digest, and valid-input C/Rust layout checks. A version string
alone does not identify a locally corrected tool. Do not add dummy kernel
storage, hardcode task offsets, or change scheduler configuration to conceal a
binding generator error.

After a binding layout correction, rebuild the kernel Image and all matching
modules. The generated bindings are consumed by the Rust kernel crate and by
multiple drivers; rebuilding just one module can leave incompatible layouts in
the same kernel. Keep the old Image and its own modules together as a fallback.

The minimum accepted bindgen version is 0.71.1. Its Rust 1.77 ``offset_of``
feature enables unconditional const layout assertions; the configured Rust
target 1.85 selects that path rather than ``#[test]``-only functions. The pinned
0.73.2 tool has the same behavior. See the source links and explicit target
accounting in ``tools/aurora-bindgen/README.rst``.
