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

Linux checks the endpoint at boot and sends it nothing (`drivers/soc/apple/sse.rs`, `diag/sse`); events the service announces are counted in `diag/sse_events`. Full endpoint parity
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

## Boot, sleep/wake and shutdown behaviour

**Observed:**

- **Boot:** macOS sends **nothing** on `sse` at boot (confirmed across full-boot
  captures on J473 macOS 27.2). OOL buffers (1 page = 16 KiB each way, layout
  `0x01010101`) are registered lazily on first userspace (`nfcd`/`passd`) call.
- **Ordinary sleep/wake:** macOS sends no SEP-wide wake acknowledgment on
  `sse`. Where a Secure Element / NFC daemon is present (J616s macOS 27.0),
  `nfcd` issues a consumer-initiated sharing-state revalidation (`tag=0x01`,
  32-byte request, 4-byte reply) after wake. On desktop Macs with no SE/NFC
  consumer (J473), zero messages are sent.
- **Shutdown:** the host sends nothing on `sse`. Inside the SEP, xART shutdown
  disabling notifies `sse` internally without AP traffic on endpoint `0x0c`.
- **Unsolicited events:** the enclave may announce asynchronous notifications
  on `tag=0x02` (no OOL body). They are not requests and must not be answered.
- **Impact of attach-only on Linux:** zero SEP errors, zero timeouts, zero
  unhandled messages, and zero suspend/wake regressions across J473, J616s,
  J713, J615, M1 Pro, M2 Air and MacBook Neo (`diag/sse=idle`,
  `diag/sse_events=0`, `diag/unhandled_messages=0`).

## Minimal behaviour and full-support assessment

- **Minimal correct behaviour (implemented in `drivers/soc/apple/sse.rs`):**
  validate discovery identity (`"sse "`) and one-page layout (`0x01010101`),
  report `diag/sse` (`idle`/`absent`), count asynchronous `tag=0x02` events in
  `diag/sse_events` without answering, count any other arrival (including
  unsolicited probes `tag=0x03` and continuations `tag=0x04`) as unhandled, and
  send nothing. Even the read-shaped protocol query (`"cESS"`, op 1) is not
  sent because its side-effect freedom on external SE hardware is unproven and
  no Linux consumer uses its answer.
- **Full support (out of scope for Linux):** full `sse` support exists solely
  to pair the Secure Enclave with an Apple Secure Element for Apple Pay /
  Wallet payment authorization, which also requires Apple's proprietary SE
  provisioning and payment stack. Without an open Linux Secure Element consumer
  there is no user-facing benefit; keep `sse` attach-only unless a real
  in-kernel SE consumer appears, and never expose a raw APDU or SEP relay.

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
