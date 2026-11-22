# pnon: nonce-service hypothesis

## Purpose, client and parity

**Observed:** `sep-endpoint,pnon` is advertised in the early group on all four
Linux profiles. No child macOS driver is visible under that nub in the captured
AppleSEPManager subtree. Its public client identity is therefore **unknown**;
do not fill the gap with an invented Apple driver name.

**Inference:** the name suggests a nonce service. Apple documents a LocalPolicy
nonce protected through secure storage in its
[LocalPolicy description](https://support.apple.com/en-gb/guide/security/secc745a0845/web),
but that does not establish pnon as its transport. The connection to boot
personalization, attestation or another nonce domain remains unverified.

Linux has no pnon client. It needs investigation for full endpoint parity.
Whether any particular Linux boot/security feature requires this endpoint
cannot be decided until its nonce domain and callers are identified. It is not
an alternative entropy endpoint just because nonces may be random.

## Messages and fields

Only discovery descriptors and configuration records are evidenced. The pnon
configuration data differs between the inspected T6040 and other profiles;
its bytes have no verified meanings here. Do not interpret them as opcodes,
versions, buffer sizes or permissions.

Service selectors, request/reply headers, nonce length, nonce domain,
persistence, rotation and consumption rules are all unknown.

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
