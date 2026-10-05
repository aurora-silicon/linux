# Shared xART gigalocker safety gate

The XART `.gl` file occupies APFS-managed storage. Writes require the explicit
`xart_writes=1` opt-in and an APFS-resolved extent with a single owner, no
snapshots or clones, and valid existing records. `xart_start_sector` is only
an assertion against that resolved extent.

The write path is limited to controlled J414s hardware tests. Restoring an
older raw image does not guarantee recovery of SEP anti-replay state. Keep a
recoverable partition image before testing writes.

## Supported storage layout

The APFS locator accepts only a complete checkpoint, single-node object maps
and file-system tree, and one unencrypted 6 MiB `.gl` extent. Unsupported
layouts fail closed.

Extent validation rejects missing, overlapping, shared, cloned, snapshotted or
mismatched references. Before enabling writes, the kernel checks volume flags,
pending revert fields, inode flags and extent ownership on its block handle.
Malformed or duplicate xART records prevent writes.

The record update order is to allocate a fresh 0x9000-byte slot, write it,
synchronize the disk cache, then clear 0x1000 bytes at the former slot.
Records are validated and duplicate selection is handled when the file opens.
Extent ownership checks alone do not validate record-update recovery or SEP
anti-replay recovery.

## Verification before writes

1. Verify that extent locking keeps the physical mapping stable for the
   entire write, including across APFS copy-on-write, snapshots, encryption
   and remapping.
2. Verify record ordering, duplicate selection, failed-write recovery,
   barriers/cache synchronization and revision behavior in controlled
   non-production tests.
3. Run controlled power-cycle tests with our builds. Check APFS checksums,
   the `.gl` mapping and records before and after each test, with a recoverable
   partition image available.

## J414s read-only hardware checks

Our read-only build verified checkpoint xid 136, one unencrypted 6 MiB `.gl`
extent at APFS blocks 1236–2771, and 11 live records with valid payload CRCs.
The volume flags were `0x1`, with zero snapshots and pending revert fields,
and no inode clone flag. The sole physical extent-reference record had kind
`APFS_KIND_NEW`, owner 16 and reference count 1. Mailbox and RNG operation were
verified; xART writes and keybag provisioning were not exercised.

Four synthetic tests passed for the extent-ownership checks. The supported
layout and ownership checks cover physical mapping and sharing only; they do
not enable general APFS writes.

APFS field definitions: [Apple File System Reference](https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf).
