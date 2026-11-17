# Shared SEP transport and validation contract

This contract separates existing Linux transport facts from proposed client
rules. See the [evidence definitions and source pin](README.md).

## Existing transport facts

**Source:** `proto.rs`, `sep.rs`, `registration.rs`, `shmem.rs`, `image.rs`,
`sks.rs`, `scrd.rs`, `pm.rs`, and `pm_shim.c` at the pinned revision.

The mailbox carrier has `message_word` and `auxiliary_word`. The common decoder
extracts `endpoint`, `tag`, `message_type`, `parameter`, and `data` from the
message word. **These are transport fields, not universal endpoint semantics.**
sks and scrd reinterpret parts of that carrier through their own codecs.

Discovery identifies each service using `advertised_endpoint`, `service_name`,
`descriptor_kind`, and `configuration_data`. The received numeric identity is
not an instruction to guess a client's command set. Preserve the original
configuration data for research without assigning meanings to unknown bits.

Bulk data uses separately registered host/enclave shared buffers. Registration
describes `target_endpoint`, `inbound_size`, `inbound_address`, `outbound_size`,
and `outbound_address`. Addresses are DMA/IOVA values, not CPU pointers. The
existing code uses named size/address operations and validates representation
limits. Registration readiness does not authenticate a payload.

The existing sks request has `endpoint`, `operation_selector`,
`transaction_sequence`, `image_length`, and `auxiliary_word`. The reply exposes
`operation_selector`, `transaction_sequence`, `transport_status`, and
`response_length`. The response image and operation-specific status must also
be checked. The selector, sequence, and advertised buffer limit all participate
in admission; successful transport delivery alone is not successful execution.

The existing scrd carrier has `endpoint`, `request_kind`, `payload_length`, and
`auxiliary_word`. A reply has `request_kind`, `response_length`, and
`transport_status`. Its logical request header has `protocol_marker`,
`command_selector`, `command_options`, `reserved`, and `version`, followed by
command-specific fields. `command_options` is not uniformly understood outside
the implemented operations. There is no sks-like sequence in this scrd codec;
do not invent one on the wire.

The existing codec field types are:

| Schema | Named fields and types, in codec order |
| --- | --- |
| Common decode | endpoint: u8; tag: u8; message_type: u8; parameter: u8; data: u32 |
| sks request carrier | endpoint: u8; operation_selector: u8; transaction_sequence: u8; unused carrier region; image_length: u16 |
| sks reply carrier | endpoint: u8; operation_selector plus reply indicator: u8; transaction_sequence: u8; transport_status: i8; unused carrier region; response_length: u16 |
| scrd request carrier | endpoint: u8; request_kind: u8; payload_length: u16; unused carrier region |
| scrd reply carrier | endpoint: u8; request_kind: u8; response_length: u16; transport_status: i32 |
| scrd logical header | protocol_marker: opaque bytes; command_selector: u8; command_options: u8; reserved: u8; version: u8 |

Multibyte scrd carrier integers and the implemented body scalar/blob lengths
are little-endian. These descriptions refer to `message_word`; use the pinned
encoder for the unused region and auxiliary word. They do not define offsets
for any of the seven unserved endpoint payloads.

sks request images have `header_length`, `digest`, `version`, `timestamp`,
`flags`, reserved/header identity fields, `trailer`, and `body`. The existing
body writer uses little-endian scalar fields and length-prefixed, aligned
blobs. Digest coverage depends on image version. Those rules are specific to
this codec; a digest is not proof of enclave authorization. Reuse the existing
builder and version negotiation rather than duplicating an opaque image.

## Proposed client state machine

`absent -> discovered -> transport_ready -> service_ready -> quiescing`

Loss of transport enters `unavailable`; it never preserves `service_ready`
solely because the previous boot completed successfully. Each endpoint defines
what establishes readiness. Discovery is sufficient only for inventory.

1. Match the discovered service, then choose a documented protocol profile.
2. Establish buffer ownership and bounded request/reply lengths.
3. Perform only verified initialization operations in their documented order.
4. Admit feature operations only after all relevant dependencies are ready.
5. Quiesce outstanding operations before power/transport teardown. Retain DMA
   ownership until a reply, cancellation acknowledgement, or verified reset.
6. Revalidate service state after resume. Never replay a mutation merely
   because its original reply timed out.

The existing PM notifier first holds entropy, then stops/holds biometric
captures. On the post event it releases captures, then entropy. It handles
suspend, hibernation, and restore notifications while preserving SEP power
domains. This host fencing does not implement hibe, prove enclave context
retention, or define a macOS endpoint sleep protocol.

## Proposed codec rules

- Each field needs a name, direction, encoding, units, bounds, lifetime, source,
  and error behavior. Mark unverified offsets, enum values and reserved bits
  explicitly. Do not transmit a candidate layout.
- Serialize exchanges when correlation cannot distinguish concurrent replies.
  Retired sequences must not collide with late replies; if the protocol cannot
  distinguish generations, quarantine the channel until safely resynchronized.
- Validate endpoint, request identity, status and complete body shape before
  using a reply. An unknown status is failure, not authorization.
- Bound allocations and array counts; use checked length arithmetic. A claimed
  response length must fit the registered buffer and the operation's schema.
- Zero secret material after ownership ends. Do not wipe or reuse a DMA buffer
  still owned by the enclave. No payload/key/credential dumps in diagnostics.
- Keep callbacks short and defer work that may sleep. Define the lock order
  between mailbox dispatch, buffer ownership, endpoint serialization and PM.
- Expose aggregate counts and last error class per endpoint. Preserve the
  global unhandled counter's meaning; consuming a message in a stub must not
  make the endpoint appear implemented.

## Read-only evidence collection

Existing inventory can be extended by bounded reads of the named SEP diag
files, saved boot journals, and public ioreg service trees. Record timestamps,
OS/driver revisions, start/end counter values and which event actually occurred.
Read existing logs; enabling probes or debug settings is a state change and
requires separate authorization. A collector must not trigger sleep,
hibernation, keybag changes, authentication, accessory pairing, nonce rotation,
protected-content playback or speculative SEP requests as a "read-only" test.

Inventory reads do not expose the missing request bodies. If no approved
passive trace exists, retain the protocol gap. Capture schemas should record
`direction`, `endpoint`, `request_identity`, `payload_length`, `status`,
`monotonic_time`, `transport_generation`, and `lifecycle_event`, with secrets
redacted before export. A user-client selector and a SEP opcode are different
namespaces and require separate evidence.

## Qualification for a future implementation

Use independent parser fixtures, truncated/oversized replies, invalid status,
wrong request identity, late replies, sequence reuse, teardown and DMA ownership
checks. Feature tests must prove the enclave's decision is authoritative under
replay and invalid-credential attempts. Reboot, sleep and cold restore need
separate coverage, with profile/version recorded. These are acceptance criteria
for future clients, not tests performed by this documentation series.
