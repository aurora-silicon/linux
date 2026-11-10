.. SPDX-License-Identifier: GPL-2.0-only

===============================
T6041 ProRes initialization data
===============================

The driver carries ProRes initialization rows built in, and on T6041 a
machine may override them with ``apple/prores-t6041-init.bin``, an APRI
container generated for that Mac from its own macOS and not distributed with
Linux.  Without the file the device binds with its built-in rows.  T6000 and
T8140 use their built-in rows and do not request this file.

When the file is present, the driver reads and validates the whole of it
before mapping device registers.  An invalid file makes the device fail to
probe without partially initializing the encoder; it never falls back to the
built-in rows.

APRI version 1
==============

APRI is a little-endian, length-delimited format.  Every integer is an
unsigned 32-bit little-endian value::

    byte[4] magic = "APRI"
    u32 version = 1
    u32 soc = 0x6041
    u32 block_count = 3

    repeated block_count times:
        u32 block_id
        u32 entry_count
        repeated entry_count times:
            u32 register_offset
            u32 mask
            u32 value

The blocks are fixed and ordered: SCODEC-system (ID 1, 82 entries), ProRes
core (ID 2, 80 entries), and SCODEC AXI-to-fabric wrapper (ID 3, 62 entries).
The only valid version 1 size is 2,728 bytes; padding and trailing data are
rejected.

Offsets must be unique within a block and four-byte aligned.  System and core
offsets are below ``0x4000``; wrapper offsets are below ``0x1000000``.  Masks
must be nonzero, and values may not set bits outside their masks.  For each
entry the driver performs::

    new = (old & ~mask) | (value & mask)

After putting the core in MODE-off, the driver applies every row in container
order.  It then installs the descriptor ring and statistics buffers and
continues with the common T6000/T8140 initialization sequence.

The container deliberately has no MMIO addresses, selectors, power policy,
or arbitrary write commands.  File integrity is the responsibility of the
package that installs the per-Mac file.
