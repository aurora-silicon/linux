# scrd: credential contexts and policy evaluation

## Purpose, client and parity

**Source:** the existing Linux client implements enclave credential contexts,
policy checks and ACL evaluation, used with sbio and sks. This is partial
coverage, not a generic macOS credential-manager replacement.

**Observed macOS names:** `AppleCredentialManager` and
`AppleCredentialManagerUserClient` appear in the wider T6000 registry. Neither
the T6000 nor T8112 SEP subtree shows a child under `sep-endpoint,scrd`.
The relationship to AppleCredentialManager is consistent with the Linux codec
but a direct registry binding is unproven. Do not invent an
`AppleSEPCredentialManager` match: that class query returned no service in the
saved T6000 capture. A boot log explicitly records scrd registering for xART
disable notification; this does not describe a credential operation.

scrd is required for enclave-authoritative credential/biometric policy parity,
including authenticating access to protected keys. An sbio match alone or a
userspace success flag cannot replace its policy decision. Broader macOS
policies, credential classes and constraint combinations remain unenumerated.

## Existing messages and fields

The [source pin](README.md) has six logical command selectors. Their numeric
definitions live in `scrd.rs`; this spec gives named fields rather than copying
wire constants. The shared carrier/header are in [transport.md](transport.md).

| Implemented command | Named request fields after the logical header | Reply/body interpretation in current Linux |
| --- | --- | --- |
| INITIALIZE | initialization_options in command_options | Transport status; no complete general initialization-reply schema established |
| CONTEXT_CREATE_TRACKED | session_user_identity | context_reference, tracking_payload, Boolean response; exact shape checked by tracked_create_reference |
| CONTEXT_EXTERNALIZE | context_reference | Existing callers check status; general output semantics remain incomplete |
| CONTEXT_DELETE | context_reference | Status; lifetime/cancellation behavior needs broader coverage |
| VERIFY_POLICY | context_reference, policy_name, preflight, maximum_credential_age/options, parameter_count, parameter_elements | satisfied, optional first requirement_type/requirement_state/requirement_flags and length-delimited remainder |
| VERIFY_ACL_CONSTRAINT | context_reference, constraint_length, constraint_DER, operation_length, operation_name, evaluation_options, reserved_fields | Consumer-specific result; no claim of a complete generic ACL reply parser |

The Linux VERIFY_POLICY builders cover `TouchIdEnrollment` and
`DeviceOwnerAuthenticationWithBiometrics`. The latter supplies a typed user-id
parameter. The ACL builder accepts a constraint and operation as separate
length-delimited inputs. A policy name, an ACL DER key and a command selector
are distinct namespaces; no full macOS policy list is established here.

**Source limitations:** the logical header's command_options field is not a
universal semantic enum. The carrier's request kind does not uniquely identify
individual commands. `parse_policy_answer` exposes only the first requirement;
it must not be treated as a full requirement-tree decoder. Status-only callers
do not establish every reply field's meaning.

## Boot, sleep/wake and use ordering

**Observed Linux:** scrd is discovered in the late group before sks. The client
registers its buffers lazily when a credential operation needs them.
**Source enrollment path:** initialize -> create tracked context for the user
-> externalize context -> verify the user's secret through sks using that context
-> evaluate the enrollment policy -> hand the verified context to the biometric
operation. Failures stop authorization.

**Source unlock path:** the later codec supports policy preflight, reporting
unsatisfied biometric requirements, then final evaluation after matching. Source
presence does not prove every caller uses this ordering correctly. Key ACL
evaluation must remain bound to the actual key operation and context.

**Unknown macOS:** complete boot initialization, policy/credential registration,
context deletion order, unsolicited notifications, sleep/wake credential expiry
and context retention. The saved boot log's wall-clock jumps preclude treating
its printed timestamps as a strict total order.

**Proposed:** create a context scoped to the requesting user and operation,
evaluate preflight, obtain only the required evidence through sbio/sks, evaluate
again, then delete/release the context. Cancel pending evaluations before PM
teardown. On wake, reuse a context only if its protocol-defined validity is
proven; otherwise reestablish it and require the enclave's current decision.

## Linux client design

Extend the current internal credential service instead of introducing raw
userspace SEP access. Make context ownership and cleanup explicit, including
error, timeout, disconnect and resume paths. Correlate each policy result with
its context, operation and user; serialize the transport if replies lack enough
identity. Never authenticate from a cached `satisfied` field or a host-maintained
PIN/password comparison. Parse the complete needed requirement set, preserving
AND/OR semantics and rejected or unknown constraint types.

Expose bounded policy operations through existing key/biometric interfaces.
Capabilities must state which policies and ACL forms are supported. Delete
context resources after use without reusing a buffer still owned by SEP. Follow
the [shared contract](transport.md) for stale replies and resets.

## Coverage gaps and acceptance

Missing evidence includes full command enumeration, all credential kinds,
policy/ACL combinations, tracking payload semantics, additional requirements,
error codes, async notifications and power-state behavior. Read existing
credential-manager logs and passive exchanges before defining new codecs.
No authentication or enrollment was triggered by this research.

Future tests must distinguish a successful transport reply from a satisfied
policy; reject wrong-user, wrong-operation and stale contexts; prove unmet
biometric/password/compound constraints remain unmet; and cover deletion,
cancellation, timeout and resume. A global unhandled counter cannot measure
these gaps because scrd already has a dispatch handler.
