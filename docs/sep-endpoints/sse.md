# sse: AppleSSE service

## Purpose, client and parity

**Observed:** `sep-endpoint,sse` matches `AppleSSE`, which publishes
`AppleSSEUserClient`. The saved registry includes user-client creators `passd`
and `nfcd`. Linux advertises sse in the later discovery group.

**Inference:** those consumers suggest involvement in Secure Element/Wallet
services. The acronym expansion and exact SEP-side responsibility are not
proven. Apple's [NFC and SE architecture](https://support.apple.com/en-gb/guide/security/secda20f3f41/web)
distinguishes the Secure Element from the Secure Enclave and describes protected
authorization between them on iPhone. It does not establish the Mac sse wire
protocol, available hardware, or payment functionality on Linux.

Linux now sends the service one request, the protocol query, once after the bring-up (`drivers/soc/apple/sse.rs`, `diag/sse`). No sharing, provisioning or authorisation request is sent. Full endpoint parity
requires resolving it. Feature parity is conditional on actual Mac hardware and
service availability; neither NFC transactions nor a Wallet feature can be
declared supported from this endpoint's presence alone. It must not be confused
with the separate Secure Storage Component or with a general RNG.

## Messages and fields

Only discovery and user-client attachment are observed. No request/reply body,
service opcode, key exchange, authorization token or Secure Element transport
mapping is established. User-client method indices are not SEP opcodes.

**Proposed candidate operations, not recovered commands:**

| Candidate operation | Logical fields to investigate | Needed evidence |
| --- | --- | --- |
| Discover service | protocol_version, supported_features, peer_identity | Which Mac feature and hardware peer it actually serves |
| Establish peer session | peer_identity, session_handle, public_handshake_data | Peer authentication, key custody, replay binding |
| Authorize operation | session_handle, operation_identity, authentication_context, intent_proof | Whether SEP performs this step and which proof it accepts |
| Complete/close operation | transaction_identity, peer_status, generation | Token consumption, cancellation and session lifetime |

Do not assign APDU fields or a generic signing command merely from `nfcd` being
attached. Payment credentials and opaque authorization material are not debug
payloads.

## Boot, sleep/wake and use ordering

**Observed:** late Linux discovery and macOS consumers attached by the time of
the snapshot. User-client attachment order is not request order. No SSE boot
initialization, sleep/wake, provisioning or transaction trace was captured.

**Proposed:** discover capabilities -> authenticate the real peer if required
-> create narrowly scoped sessions -> obtain enclave-authoritative authorization
for the verified operation -> consume or close the transaction. At sleep/teardown,
cancel or drain requests and make token lifetime explicit. On wake, establish
peer/session validity before reusing handles; a restored host cache is not proof.

## Linux client design

Keep the SEP codec separate from any physical Secure Element/NFC transport.
Choose the Linux integration only after identifying the Mac use case; do not
publish an unrestricted APDU or SEP proxy. Maintain peer and transaction
generation, validate authorization context and prevent replay across sessions.
Never recreate private peer keys, trusted intent or enclave approval in host
software. A Linux client alone does not supply provisioning or service access.
Follow the [common contract](transport.md).

## Remaining evidence and acceptance

Read already-captured AppleSSE, passd and nfcd logs, recording only sanitized
metadata and endpoint traffic timing. Do not provision credentials, initiate a
payment, or dump credential/token bodies during a read-only collection. Resolve
the actual Mac feature, peer identity, authorization and key lifetimes. Future
tests must cover wrong peer, missing authorization, stale token, abort and
resume. This research performs none of those operations.
