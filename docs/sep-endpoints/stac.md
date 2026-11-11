# stac: trusted accessories

## Purpose, client and parity

**Observed:** `sep-endpoint,stac` matches `AppleTrustedAccessoryManager`, whose
user-client class is `AppleTrustedAccessoryManagerUserClient`. The saved
registry has `loginwindow` and `bluetoothd` user-client creators. Linux discovers
stac in the early group.

**Inference:** the service participates in trusted-accessory authentication.
Apple documents [secure pairing for Magic Keyboard with Touch ID](https://support.apple.com/en-ca/guide/security/secf60513daa/web),
using authenticated cryptographic communication between the keyboard and the
Mac's Secure Enclave. This supports a candidate feature dependency, not a proven
stac command mapping. Generic Bluetooth input, all accessory authentication,
and built-in Touch ID must not be conflated with that candidate.

Linux has no stac client at the [source pin](README.md). It is needed for full
endpoint parity and likely for external trusted-biometric accessory parity;
that feature dependency still needs a transaction trace. An ordinary keyboard
being usable is not evidence that trusted Touch ID is available.

## Messages and fields

No stac opcode, framing, credential schema or request/reply body is established.
IORegistry user-client names identify consumers, not their SEP method mapping.

**Proposed operations, not recovered wire commands:**

| Candidate operation | Logical fields | Missing proof |
| --- | --- | --- |
| Inspect trusted accessory | accessory_identity, protocol_version, trust_state | Identity namespace and non-mutating query semantics |
| Establish authenticated channel | accessory_identity, public_credentials, handshake_data, session_handle | Message ownership across transport, accessory and SEP |
| Process trusted event | session_handle, event_identity, authenticated_event, generation | Event freshness and relationship to sbio/scrd |
| Revoke/close trust | accessory_identity, session_handle, authorization | Whether this exists, persistence, revocation and disconnect effects |

Public credentials still need bounds and provenance checks. Secret material,
pairing credentials and authentication proofs must not be copied into logs.

## Boot, sleep/wake and use ordering

**Observed:** early Linux discovery and attached macOS consumers. There are no
captured pairing, authentication or sleep/wake messages. The PM state in a
registry snapshot does not establish a transition sequence.

**Proposed:** discover the service -> identify an accessory on its actual
transport -> establish enclave-verified trust -> associate trusted biometric
events with their intended user and credential context -> authorize via the
existing biometric/credential path. Channel setup is separate from enrollment.
On disconnect/sleep, drain operations and invalidate the appropriate session;
on wake/reconnect, revalidate peer and freshness before accepting events.
The exact persistence/rekey policy requires recovered protocol evidence.

## Linux client design

Provide an internal trusted-accessory service, integrated with the actual
HID/Bluetooth/USB transport and existing sbio/scrd paths. Bluetooth pairing alone
must not establish biometric trust. Maintain accessory identity, authenticated
channel identity and lifecycle generation separately. Reject events belonging
to a different user, stale connection or revoked accessory. Userspace may broker
bounded public handshake data; the enclave must make the trust decision.

Do not report a host verification flag as enclave trust, bypass an existing
credential policy, or duplicate matching/enrollment logic in the transport.
Apply the [shared contract](transport.md) and make unsupported accessory models
explicit.

## Remaining evidence and acceptance

Read existing trusted-accessory, bluetoothd and loginwindow logs plus public
registry metadata for a known external sensor, if already captured. New pairing,
revocation or authentication is active work, outside read-only collection.
Resolve channel establishment, accessory attestation, rekeying, event direction
and sbio/scrd linkage. Future tests must prove untrusted/stale accessories cannot
authorize, and verify reconnect, revoke and resume behavior. None was run here.
