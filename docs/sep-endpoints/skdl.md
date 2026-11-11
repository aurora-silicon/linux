# skdl: CoreKDL service

## Purpose, client and parity

**Observed:** `sep-endpoint,skdl` matches `CoreKDLDriver`, with
`CoreKDLUserClient` published as its user-client class. Linux discovers it in
the later group, near sbio and before scrd/sks in the inspected boots.

**Unknown:** the meaning of KDL, the service's actual security operation,
its userspace consumer and the features that depend on it. A key-related name
is insufficient to classify it as key derivation, key distribution, a data
locker or a Secure Storage Component interface. No such expansion is asserted.

Linux has no skdl client. Resolving it is necessary for endpoint parity, but no
specific Linux feature can yet be claimed to depend on it. It must remain a
visible unresolved capability rather than an apparently successful stub.

## Messages and fields

Discovery and matching are observed. There is no verified skdl command stream,
opcode, payload field layout, reply status, buffer direction or handle lifetime
in the inspected evidence. A `CoreKDLUserClient` external method number, if later
observed, must be mapped separately from any SEP selector.

**Proposed research schema, not a protocol:**

| Item | Named fields to identify | Reason |
| --- | --- | --- |
| Service description | protocol_version, capabilities, owner | Establish function before inventing a consumer API |
| Request envelope | operation_identity, transaction_identity, input_length, input_buffer | Establish framing and bounds |
| Reply envelope | operation_identity, status, output_length, output_buffer | Distinguish transport completion from service success |
| Resource lifetime, if present | resource_handle, owner_binding, generation | Prevent cross-owner or stale-handle use |

No create/derive/unlock command is assigned: even those operation names would
be speculation about the service's purpose.

## Boot, sleep/wake and use ordering

**Observed:** Linux skdl discovery belongs to the later group after the xarm
exchange. The relative order of skdl and sbio varies. macOS service matching is
observed, but command order at boot, sleep/wake and actual use is unknown.

**Proposed:** retain discovery for inventory and defer the functional client
until a legitimate consumer and protocol are identified. Establish documented
dependencies rather than relying on the arrival count. If the eventual service
owns resources, its sleep and resume path must explicitly handle outstanding
requests, context retention and transport resets according to captured evidence.

## Linux client design

Initially implement no functional ABI. Once the purpose is proven, attach a
small endpoint codec through the common transport and integrate it with the
appropriate existing Linux subsystem. Expose typed operations with validated
fields; avoid a generic privileged SEP packet relay. Do not duplicate sks,
scrd, keyring or trusted-key functionality based on the acronym alone.

The client must enforce resource ownership and accept success only from the
enclave. Its failure must not silently select a host implementation of a
security decision. The [shared contract](transport.md) governs the eventual
codec, correlation, lengths, timeouts and DMA ownership.

## Remaining evidence and acceptance

Read existing CoreKDL service logs and a wider public registry inventory for
user-client creators. Use approved passive traces of existing service activity;
do not open and enumerate external methods speculatively. Identify one real
feature and its complete request/reply pair before designing a functional API.
Acceptance then needs independently observed semantics, error handling, owner
isolation and lifecycle behavior. Those remain unresolved, not tested.
