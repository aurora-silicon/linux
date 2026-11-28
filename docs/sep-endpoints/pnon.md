# pnon: BootPolicy and LocalPolicy nonce service

## Purpose, client and current Linux coverage

`pnon` (`0x15`) is the enclave's BootPolicy / LocalPolicy anti-replay service.
It manages the local, remote and recovery policy nonce domains (48-byte SHA-384
digests; raw nonces never leave the enclave), LocalPolicy pairing verification,
boot-mode reporting, LocalPolicy creation/update transactions, and hibernation
preparation.

**Current Linux coverage (`drivers/soc/apple/pnon.rs`):**

- Implemented as a single read-only query after bring-up: `op 0x0e` (read
  blessed LocalPolicy nonce digest) using the `"hcPB"` / `"hrPB"` 32-byte
  envelope, exposed root-only in `diag/local_policy_digest` and `diag/pnon`.
- Currently supports only the 5-7 page discovery layout (`0x05050707`: M2/M3/M4/
  M4 Pro on macOS 27.x SEP firmware; verified `diag/pnon=read` on J616s, J713,
  J615, J413, J473).

**Known coverage gaps on `pnon`:**

1. **2-4 page SEP layout (`0x02020404`):** M1 Pro (13.5 firmware) and MacBook
   Neo (A18 Pro, 26.6.2 firmware) use the older `tag=1` mailbox carrier and
   currently read `diag/pnon=absent`. Support is in draft PR #2810 (awaiting
   hardware rows on the 2-4 page Macs).
2. **Additional read-only BootPolicy queries:** proposed/blessed remote and
   recovery nonce digests, current AP boot mode, and LocalPolicy pairing
   status/verification are not yet exposed in `main`.
3. **Mutating LocalPolicy transactions & hibernation prepare:** nonce
   begin/commit/reset transactions, LocalPolicy create/update/re-sign
   transactions, and pre-hibernation policy preparation are not yet
   implemented. Ordinary Linux suspend does not require `pnon`.

## Messages and fields

Beyond the implemented blessed-digest read (`op 0x0e`, `"hcPB"`/`"hrPB"`
envelope on the `0x05050707` layout in `drivers/soc/apple/pnon.rs`), additional
service selectors, nonce domains, persistence, rotation and consumption rules
remain unmerged on `main`.

**Proposed research vocabulary, not recovered commands:**

| Candidate operation | Logical fields to investigate | Missing proof |
| --- | --- | --- |
| Identify nonce domain | domain_identity, scope, protocol_version | Who owns the service and what freshness means |
| Obtain nonce/reference | domain_identity, purpose, boot_binding | Whether the result is public, secret or only an opaque handle |
| Validate/consume reference | nonce_reference, caller_binding, proof | Whether such an operation exists; replay semantics |
| Rotate/invalidate | domain_identity, generation, authorization | Privilege, persistence and effect on existing policy |

These candidates must not be transmitted until confirmed. A read-like operation
can still allocate, consume or rotate enclave state; do not assume it is safe.

## Boot, sleep/wake and use ordering

**Observed:** early Linux discovery; macOS nub presence. No pnon transaction
was captured at boot, use or sleep/wake. Discovery order cannot determine nonce
lifetime or whether an operation belongs in a bootloader rather than Linux.

**Proposed:** inventory first; bind a client only after domain/ownership are
established. Keep nonce lifetimes scoped to the proven boot, policy, transaction
or session. On resume, preserve or replace state only as specified by the actual
protocol. A cached host value must never extend enclave freshness guarantees.

## Linux client design

Prefer a narrow internal API for the verified boot-policy or attestation
consumer. Keep independent nonce domains separate and make rotation/consumption
explicit operations with authorization. Use opaque references if custody cannot
leave the enclave. Never export a general writable nonce-control device or
substitute `/dev/urandom` for an enclave freshness check. Reject unsupported
versions and leave inventory visible. Apply the [transport rules](transport.md).

## Remaining evidence and acceptance

Read broader public registry service families and existing boot-policy logs;
correlate with approved passive SEP traffic before assigning a client. Do not
invoke policy changes or nonce rotation as a capture step. Resolve domain,
read side effects, caller privilege, replay resistance and reboot/resume
lifetime. Future tests must reject cross-domain use, stale references and
unauthorized rotation. No such tests were run here.
