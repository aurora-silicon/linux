# sks: keybags, enclave keys and storage keys

## Purpose, client and parity

**Source:** sks provides the existing Linux keybag/key-store protocol, reference
key operations and storage-key operations. Related macOS public registry service
names are `AppleKeyStore`, `AppleKeyStoreUserClient`, `AppleFDEKeyStore`,
`AppleCredentialManager` and `AppleCredentialManagerUserClient`, observed in the
wider T6000 registry. The runtime boot-log sender is `AppleSEPKeyStore`.
Neither saved SEP subtree shows a direct child below `sep-endpoint,sks`;
the precise routing among these services needs separate evidence. The
AppleSEPKeyStore class query itself returned no registry service.

This endpoint is required for enclave-backed key custody, password keybags,
protected signing/sealing and storage-encryption parity. Implemented commands
do not establish complete Data Protection, FileVault, FIDO or recovery behavior.
Enclave key custody and authorization must be maintained through every path;
host root must not bypass them by changing a local policy flag.

## Existing messages and fields

**Source:** `sks.rs`, `image.rs`, `userbag.rs`, `fv.rs`, `refkey.rs`, `der.rs`,
`trusted.rs` and `refkey_seal.rs` at the [source pin](README.md).
The carrier and request-image fields are specified in [transport.md](transport.md).
`sks.rs` defines twenty-four top-level operation selectors in that revision.
Their numeric definitions remain in the licensed source, not in copied tables.

In the table, **common** means the operation's `structure_version` and
`client_identity`, when present. Blob means the existing bounded length-prefixed
body encoding, not a CPU pointer. Reply schemas are operation-specific; all
transport status, response image and operation status checks still apply.

| Existing selector name | Request fields beyond the common prefix | Known response / coverage limit |
| --- | --- | --- |
| GET_CAPABILITIES | request_blob | structure_version, capability_word, blob; used for image-version negotiation |
| SET_ENV | environment_blob containing environment_version, encryption_mode, mode_options and reserved region | Accepted structure version; profile-specific initialization |
| CREATE_KEYBAG | bag_type, profile_argument, secret_blob, auxiliary_blob, identity_blob, profile-specific trailers | Operation status and created bag handle; identity and per-user builders differ |
| LOAD_KEYBAG | wrapped_bag | operation_status, temporary_bag_handle |
| COPY_KEYBAG | bag_handle | wrapped serialized bag; source and designated handles have different lifetimes |
| COPY_UUID | bag_handle | bag identity; identity is not a credential |
| DESIGNATE_KEYBAG | source_handle, user/session_handle, secret_blob or empty_blob, profile-specific flags | Designation result; source handle and designated session are distinct |
| UNLOAD_KEYBAG | source_handle | Status; do not confuse unloading a handle with securely deleting persistent state |
| CHANGE_SECRET | user_handle, old_secret, new_secret, flags, trailer, reserved_fields | Version/result check; wrapped record persistence must follow accepted change |
| CHANGE_LOCK_STATE | bag_handle, requested_state, secret_blob, flags | Lock-state result; requested state is not proof of completed locking |
| DEVICE_STATE_TRANSITION | user_handle, requested_state, flags, secret_blob | Profile-specific state result; flags precede secret in this builder |
| GET_DEVICE_STATE | user_handle, query_options | Parsed DER state including enclave lock phase/state |
| GET_CONFIGURATION | user_handle | Parsed DER configuration; supported keys are bounded by current decoder |
| HANDLE_TIMER_FIRE | timer_header_fields | expired_handle, timer_kind, reserved_fields, opaque_token, opaque_counter; the request field meanings are not fully named |
| VERIFY_SECRET | designated_handle, secret_blob, optional_context_blob, verification_flags | structure_variant, returned_state_flags; context and password-validation modes differ |
| PERFORM_OPERATION | bag_handle, envelope_version, operation_DER | operation-specific DER/blob result; create/sign/attest/seal are nested operations, not new top-level opcodes |
| NEW_PFK | volume_handle, parameters_DER, protection_class, options, context_blob, auxiliary_blob | Wrapped file-key result; complete filesystem integration not implied |
| UNWRAP_PFK | volume_handle, parameters_DER, protection_class, wrapped_key, wrapped_wrapping_key, auxiliary_blob, options | Unwrapped file-key result in the existing storage path |
| UNWRAP_MEDIA_KEY | wrapped_media_key, bag_handle, protection_class | Parsed storage-key response; not general key export |
| NEW_VEK | parameters_DER, secret, KEK_record, domain_binding | Wrapped VEK record; no independent proof of macOS volume compatibility |
| UNWRAP_VEK | parameters_DER, options, secret, KEK_record, VEK_record, auxiliary_blob | Parsed key result; rejection and record shapes remain profile-dependent |
| NEW_KEK | parameters_DER, options, secret, domain_binding | Wrapped KEK record for the implemented storage flow |
| SET_PROTECTION | parameters_DER, action, flags, secret_blob, unlock_record, volume_key | Class-key load/unload result; different actions share this selector |
| GET_BLOB_STATE | parameters_DER, volume_key | Opaque/parsed blob state according to the storage consumer |

This inventory describes code, not all messages macOS uses. Reserved fields,
nested DER attributes, class values and selector variants must be audited in
their actual builders/parsers before extension. Do not copy opaque domain blobs
or use an unexplained constant to stand in for field construction. Unknown
timer fields remain opaque rather than acquiring assumed cryptographic meaning.

## Boot, sleep/wake and use ordering

**Observed:** sks is last in the inspected Linux discovery boots, following
the xarm exchange. The T6000 macOS boot log records key-store startup, image
header negotiation, then PRNG reseeding. It also reports early session/mapping
errors. The wall clock changes during that log, so it does not establish a
complete boot ordering or SEP transaction stream.

**Source initialization:** register buffers -> initialize the selected profile
-> recover/load persistent bags before creating new state -> establish the
appropriate designated identity -> prepare protected operations. The Sepos13
profile performs GET_CAPABILITIES then SET_ENV before other key-store requests;
the variant-5 profile deliberately omits those initialization messages. Do not
send the Sepos13 sequence to every machine because an endpoint name matches.

**Source use:** load/create a bag -> designate the intended user/session ->
perform the needed secret/state transition -> operate on keys -> save accepted
wrapped changes -> unload temporary resources when appropriate. A credential
context for VERIFY_SECRET comes from scrd. A reference-key operation also needs
its own ACL and caller binding. The actual storage flow determines whether a
result is an opaque record, hardware key reference or key bytes; do not impose
the non-exportable reference-key API on the existing file-key codec.

**Unknown macOS:** exhaustive command ordering for login/logout, password change,
recovery, timer/grace transitions, sleep/wake, cold restore and async events.
The current Linux PM notifier fences entropy and biometric capture; it is not
proof all keybag operations or credentials are quiesced/invalidated correctly.

**Proposed:** serialize mutations per bag, drain in-flight operations at PM
boundaries and retain the true enclave lock state. On wake, revalidate handles
and query relevant state before admitting protected operations. Do not report
the bag locked while SEP still permits a forbidden use. Password changes require
durable wrapped-record updates and defined recovery after interruption.

## Linux client design

Extend the existing internal key-store service and bounded `/dev/sep-key` and
trusted-key consumers. Use separate objects for persistent wrapped bags,
temporary source handles, designated user sessions, credential contexts and
reference keys. Bind each to owner and transport generation. Keep reference-key
private operations inside the enclave; storage-key handling needs its own
carefully scoped consumer and memory lifetime.

Keep errors from transport delivery, image parsing, enclave operation and host
persistence distinct. Do not create a replacement bag when recovery is
ambiguous. Preserve mutation-intent and accepted-record evidence so an
interrupted operation cannot silently reset identity or attempt counters.
Reject unsupported ACLs and profile versions explicitly. Host-only attempt
counters, a software PIN check or a fallback signing key cannot satisfy parity.

## Coverage gaps and acceptance

The missing portion is a macOS command/variant inventory, nested reference-key
operations and constraints, class/lock-state semantics, persistent attempt
limits, persona/volume-owner/recovery behavior, async messages, and all power
transitions. Existing code presence is not qualification of any of these.
The global unhandled counter does not count unsupported commands never sent,
and messages routed to sks bypass that default branch.

Read existing public registry/log/owned-traffic evidence first. New bags,
password guesses, nonce changes and storage writes are active experiments, not
read-only captures. Future tests must cover wrong-password and stale-record
rejection, counters across reboot, locked-key refusal, accepted password change
across interruption, ACL authorization, policy binding and resumed/cold sessions.
No such active experiments were run by this documentation work.
