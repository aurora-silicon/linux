# Shared xART gigalocker safety gate

The iBoot APFS container and its XART `.gl` file are shared with macOS. The
kernel owner must remain read-only. `apple_sep.xart_writes=1` is rejected even
with an explicit `xart_start_sector`; the latter is now only an assertion
against the APFS-resolved file extent. Do not enable provisioning services on a
dual-boot installation while this gate is in place.

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
