# xars: second xART service

## Purpose, client and parity

**Observed:** `sep-endpoint,xars` matches `AppleSEPXARTService`, the same class
that matches xarm. Both appear in the early discovery group on all four Linux
profiles. **Inference:** xars participates in persistent enclave state or its
anti-replay support. Its exact role, distinction from xarm, request direction
and storage namespace are **unknown**. It must not be called a mirror, backup,
or interchangeable xarm transport without evidence.

Linux dispatch serves xarm but has no xars handler at the [source pin](README.md).
Full endpoint parity requires resolving xars. Whether current Linux keybags,
rollback anchors or any individual feature require it is not established.
The current xarm exchange succeeding does not establish xars redundancy.

## Messages and fields

**Observed:** discovery advertises a service name and configuration record.
No xars service request/reply body is present in the inspected evidence.
Command selectors, header, payload fields, operation status, buffer direction,
length units, transaction identity and persistence acknowledgement are unknown.
Discovery configuration is not a command schema.

**Proposed Linux vocabulary, not wire opcodes:**

| Candidate operation | Logical fields to resolve | Evidence required before implementation |
| --- | --- | --- |
| Describe service | protocol_version, supported_actions, namespace | Verified capability request/reply or licensed codec |
| Read persistent record | record_identity, offset, requested_length | Direction, authority and namespace mapping |
| Commit persistent record | record_identity, generation, data_length, opaque_data, integrity_metadata | Proof xars actually performs this operation; durability and replay semantics |
| Complete exchange | transaction_identity, completion_status | Acknowledgement ordering and retry/idempotency contract |

These candidates frame questions; none establishes that xars is a host storage
service. Do not register an xarm-sized buffer or forward xars messages to xarm.

## Boot, sleep/wake and use ordering

**Observed Linux:** xars is advertised before the later key/credential group.
xarm's exchange precedes that later group. There is no observed xars transaction
to place between those events. **macOS:** the snapshot proves matching only;
boot request order, ordinary use and sleep/wake messages remain unknown.

**Proposed:** discover -> identify protocol -> establish separate namespace and
transport -> satisfy verified persistent-state obligations -> publish readiness.
On sleep, drain any proven transactions and finish durability acknowledgements
before releasing buffers. On wake, reconcile generations before accepting new
writes if the recovered protocol requires it. These steps depend on learning
the protocol and are not a claim about macOS behavior.

## Linux client design

Use a separate endpoint service behind the common SEP transport. Reuse the
storage backend only after proving the namespaces and on-disk formats agree;
keep authorization and generation tracking endpoint-specific. Store opaque
enclave records without interpreting secrets. The enclave must enforce replay
rules; a host generation counter alone is insufficient. Never report success
before durable completion, and do not retry a possibly committed mutation
without a documented identity/idempotency rule.

Failure leaves dependent services unavailable with a typed error. Do not create
fresh state to mask an unreadable record. Keep macOS-owned state outside Linux
write ownership. The [shared validation contract](transport.md) also applies.

## Remaining evidence and acceptance

Passively inspect existing macOS XART-service logs and approved transport traces
for direction, record identity and acknowledgements. Compare xars/xarm activity
within the same boot and event window; two matching classes alone are inadequate.
Before implementation, resolve storage ownership, power-loss recovery, rollback
behavior and version changes. Future tests must prove namespace isolation,
durable acknowledgement and rejection of stale records. None was exercised here.
