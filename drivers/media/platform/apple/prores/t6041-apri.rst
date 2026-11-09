.. SPDX-License-Identifier: GPL-2.0-only

===============================
T6041 ProRes initialization data
===============================

The T6041 ProRes encoder requires ``apple/prores-t6041-init.bin``.  This file
is generated from the copy of AppleProResHW installed on the same Mac and is
not distributed with Linux.  T6000 and T8140 do not request this file.

The driver requests and validates the whole file before mapping device
registers.  A missing or invalid file therefore makes the T6041 device fail
to probe without partially initializing the encoder.

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
or arbitrary write commands.  File integrity and installation provenance are
the responsibility of the vendorfw manifest that carries the per-Mac file.
