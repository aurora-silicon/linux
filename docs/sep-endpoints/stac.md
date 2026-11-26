# stac: trusted accessories (Magic Keyboard with Touch ID)

## Purpose, client and current Linux coverage

`stac` (`0x18`) is the enclave's trusted-accessory service, serving
`AppleTrustedAccessoryManager` for external biometric accessories such as the
Magic Keyboard with Touch ID. It shares the generic-transfer carrier
(`transfer.rs`, `tag=0xfc..0xff`, 28-byte packet header) with `sbio` over two
16 KiB OOL pages each way (`0x02020202`).

**Current Linux coverage (`drivers/soc/apple/stac.rs`):**

- Implements the mandatory power-state handshake: sends `OP_POWER_STATE`
  (`0x0f`) with `POWER_STATE_ONLINE` (`1`) once after bring-up and again after
  every enclave wake from firmware sleep, reporting `diag/stac` and
  `diag/stac_onlines` (verified `online` across J473, J616s, J713, J615,
  M1 Pro, M2 Air and MacBook Neo).

**Known coverage gaps on `stac`:**

1. **Accessory pairing, session and authorization operations:** none of the
   accessory-management operations (host attestation, pairing query/begin/
   finish, unpair, manifest submission, session setup, connection state,
   paired count, or authorization) are merged on `main` yet (earlier draft
   PR #1810 was closed because it relied on module parameters; needs a
   per-device interface and client lifecycle integration).
2. **USB/Bluetooth HID transport & `sbio` sensor relay:** Linux `usbhid` does
   not yet bind the keyboard's vendor Touch ID interface or relay external
   keyboard sensor frames into `sbio`.
3. **Power-off / hibernation recovery:** ordinary suspend needs no pre-sleep
   `stac` message; cold-hibernation accessory session recovery will only be
   needed once both hibernation and the accessory client exist.

## Messages and fields

Beyond the implemented `OP_POWER_STATE` (`0x0f`) online handshake in
`drivers/soc/apple/stac.rs`, the remaining pairing, session and authorization
wire schemas are not yet merged on `main`. IORegistry user-client names
identify consumers, not their SEP method mapping.

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
