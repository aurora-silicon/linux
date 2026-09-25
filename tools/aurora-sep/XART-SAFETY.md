# Shared xART gigalocker safety gate

The iBoot APFS container and its XART `.gl` file are shared with macOS. The
deployed `m2fix4` kernel remains read-only. The `m2fix5` source has an explicit
`xart_writes=1` opt-in; it accepts writes only when the APFS-resolved extent is
single-owner, unsnapshotted, un-cloned, and all existing records validate.
`xart_start_sector` remains only an assertion against the APFS result. The
opt-in is for the controlled J414s hardware test, not general APFS write
support or a promise that restoring an old raw image reverses SEP anti-replay
state. Do not enable it on other installations without equivalent checks.

## Evidence collected on 2026-09-21

- A read-only inspection of `m2`'s iBoot partition found a complete, valid
  checkpoint at transaction 57, one XART volume (role `0x100`), and one
  unencrypted `.gl` extent: 6 MiB beginning at APFS block 1236. All 11 live
  gigalocker records in that extent passed their payload CRC checks. The raw
  signature heuristic happened to select the same block on this snapshot.
- A synthetic image with the first root moved to slot 10 reproduces a legacy
  locator failure: the highest-scoring window begins 90 APFS blocks after the
  true file start and extends beyond `.gl`. This proves ambiguity in that
  locator, **not** the cause of the previously damaged installation.
- Static inspection of the macOS 27.2 AppleSEPManager kernel collection found
  `gl_rec_write` allocating a fresh 0x9000-byte slot, writing it, then clearing
  0x1000 bytes at the former slot. The low-level write path also calls a disk
  cache-synchronization ioctl. `gl_fixup_rev_init` validates records and
  handles duplicates during open. These observations do not prove that our
  APFS ownership and failure ordering match macOS.

## Before authorizing writes

1. Confirm the exact macOS vnode/raw-device and extent-locking contract,
   including whether any APFS copy-on-write, snapshot, encryption, or remap
   state can invalidate the physical mapping during Linux's lifetime.
2. Verify record ordering, duplicate selection, failed-write recovery,
   barriers/cache synchronization, and revision behavior against both macOS
   code and controlled non-production tests.
3. Compare a pre/post macOS-and-Linux power-cycle trace, with APFS checksums,
   `.gl` mapping and records checked before and after. Keep a recoverable image
   of the iBoot partition before the first write experiment.

The current APFS locator supports only a complete checkpoint, single-node
object maps and file-system tree, and a single unencrypted 6 MiB `.gl` extent.
Unsupported layouts fail closed; this is not a general APFS implementation.

## Read-only J414s recovery image (2026-09-23)

After the minimal stage2 made the SEP mailbox and RNG functional, a fresh
read-only inspection still found one unencrypted 6 MiB `.gl` extent at APFS
block 1236, 11 live records with valid payload CRCs, and no malformed records.
The selected container checkpoint was xid 136. No xART writes or keybag
provisioning were attempted.

The 524,288,000-byte `iBootSystemContainer` partition was copied through a
read-only block handle and compressed to
`work/m2-tb-sep-deploy/iboot-system-container-2026-09-22.img.zst` (7,857,330
bytes, mode 0600). The local archive passed `zstd -t`; its decompressed
SHA-256 matched a separate read of the partition:
`09206fcf0500f5646ea04ec37cf1fc8a0493f1c5ee3271c2af562ed9b39a6d37`.
The compressed archive SHA-256 is
`8a885a2397c26404cf8395ca64b279efc793f9118eae408f2a6d84fe9af1ded8`.
The temporary m2 copy was removed after verification. This is a recovery
artifact, **not** evidence that Linux may safely write APFS-owned `.gl` blocks.

## Narrow write-ownership evidence (2026-09-23)

The live m2 partition SHA-256 still matches the independent backup hash above.
The updated read-only inspector selected checkpoint xid 136 and checked the
current xART volume superblock and checksum-validated extent-reference tree:
volume flags `0x1` (unencrypted only), zero snapshots, zero pending revert
fields, and an inode with no clone flag. The sole `.gl` extent is APFS blocks
1236–2771 (1536 blocks, 6 MiB); its only physical extent-reference record is
kind `APFS_KIND_NEW`, owner 16 (the inode's private ID), reference count 1.
The inspector now rejects missing, overlapping, shared, cloned/snapshotted,
or mismatched references, and four synthetic tests pass. The `m2fix5` kernel
independently checks the same narrow ownership conditions on its block handle
before arming writes and refuses a writable scan with malformed or duplicate
xART records. These checks establish the current physical mapping and lack of
APFS sharing; they do not by themselves establish macOS's full record-update
or SEP anti-replay recovery semantics. The APFS field meanings come from the
[Apple File System Reference](https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf).
