# Linux host-state store

`/var/lib/aurora-sep-host-state.bin` holds SEP state that only Linux keeps:
the Touch ID identity index and the lockout state. Nothing regenerates it.
The catacombs have files of their own, and the SEP's anti-replay records live
in the shared xART gigalocker.

## Format

The file is exactly 72 blocks of 32 KiB (2359296 bytes). Block 0 is the
superblock: the magic `AURORA-SEP-STOR\x01`, version 1, the slot count 71, the
block size, a generation, a write intent and a table of 71 slot entries. The
record of slot *n* is in block *n* + 1. Writes are copy-on-write: the driver
records an intent naming a free block, writes the record there, then writes the
new table.

The table is rewritten in place, so the format relies on the device writing the
first 4 KiB of the superblock atomically. Every field the driver reads lies in
that range, so after a crash it finds either the old table or the new one.
NVMe storage on Apple silicon has 4 KiB logical blocks
(`/sys/block/<disk>/queue/logical_block_size`). A copy of the file taken while
the driver writes has no such guarantee; see "Copying the store".

Every kernel writes this format byte for byte the same way, so any mix of
kernels can use one valid store in any boot order. The driver's KUnit suite
builds the earlier driver from its own source and checks this in both
directions, including writes cut off at every step.

There is deliberately no checksum. Earlier kernels rewrite the whole superblock
on each write, zeroing every byte they do not use, and never look past the
store's 72 blocks. A checksum inside the superblock would be erased, and one
after the store would be stale, after any write by an earlier kernel, and a
reader could not tell either from damage.

## What the driver refuses

The driver creates the store only when the file does not exist. It never
initialises, repairs or rewrites an existing file that fails validation. Each
time it opens the file, at probe and again when Touch ID is activated, it logs
`host-state store failed validation (REASON), refusing it`, and Touch ID stays
unavailable for that boot. Nothing is written.

| Reason | Meaning |
| --- | --- |
| `Size` | not exactly 2359296 bytes: truncated, extended or an interrupted creation |
| `Header` | the magic, version, slot count or block size is wrong |
| `Reserved` | a byte that the format leaves zero is not zero |
| `Slot(n)` | slot *n* is neither all zero nor a valid record |
| `Duplicate(n)` | slot *n* has the same key as an earlier slot |
| `Intent` | the pending write intent does not name a free block |

A `Size` or `Header` reason is followed by `, empty` or `, all zero` when the
file, no longer than a store, holds nothing but zeros. Without that qualifier
the file holds data, which may be what is left of a store.

Kernels from before this validation erase a file that is short or has another
header, and use the other cases unchecked. A kernel that carries only the fix
that stopped the erasing refuses a short file, an unknown header, a record
longer than its block and an intent that names a used block, with the same log
line, and uses the other cases unchecked.

## Copying the store

Copy the store only while nothing uses Touch ID, for example with fprintd
stopped, and take two copies in a row. Use them only if they are identical:

```sh
umask 077
cp /var/lib/aurora-sep-host-state.bin copy-1
cp /var/lib/aurora-sep-host-state.bin copy-2
cmp copy-1 copy-2 && echo consistent
```

## Checking a store

`host-state-check.py FILE` applies the same rules without writing and prints
`ok: N records` or `refused: REASON`, never record contents. For `Size` and
`Header` it describes the file as the driver does, and for a `Size` that is not
all zero it adds `header present, N records` (for example a truncated store)
or `no header`. Check a consistent copy rather than the live file: a read that
overlaps a write by the driver can see a refusal that is not there.

## Recovery

Keep a private copy of the store (mode 0600, never published) before any
recovery step, and before booting a kernel that would erase it.

- A file in which every byte is zero holds nothing. It is what an interrupted
  first creation leaves, by any kernel, until the superblock is written, and
  the driver reports it as `Size, empty`, `Size, all zero` or
  `Header, all zero`. Confirm it on the file itself:

  ```sh
  f=/var/lib/aurora-sep-host-state.bin
  cmp -n "$(stat -c %s "$f")" "$f" /dev/zero && echo all-zero
  ```

  Only if that prints `all-zero`, remove the file; the driver creates a new
  store on the next boot.
- Anything else needs a recovery decision, including a file that is only
  short: a truncated store usually keeps its header and its first records
  (`Size, header present, N records`). Do not delete the store or restore an
  older copy to make Touch ID work again: the SEP's anti-replay state has moved
  on since that copy, so old records may be refused or replay old state. Do not
  change the Linux OS identity to work around a refused store.
