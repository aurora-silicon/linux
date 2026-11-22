# hdcp: protected display links

## Purpose, client and parity

**Observed:** `sep-endpoint,hdcp` matches `AppleSEPHDCPManager` and is advertised
in the early Linux discovery group. The registry publishes interface-query debug
properties; they do not describe a SEP command layout or prove an authenticated
display session. **Inference:** the service supplies enclave-backed HDCP key or
authentication functions for display-link protection.

Linux has no hdcp endpoint client at the [source pin](README.md). It is required
for HDCP feature parity on supported display routes, not for an unprotected
desktop. It is separate from video decoding, application DRM, and any required
protected rendering path; implementing this service cannot establish those.

## Messages and fields

No service selectors, request bodies or reply status schemas were captured.
Which HDCP versions and which authentication steps SEP owns are unknown.
DDC/AUX protocol messages are not automatically SEP messages.

**Proposed Linux operations, not Apple opcodes:**

| Candidate operation | Logical fields | Unresolved mapping |
| --- | --- | --- |
| Describe interface | display_route, protocol_version, capabilities | Interface identity and SEP/display-firmware ownership |
| Create authentication session | display_route, sink_identity, content_type, generation | Key custody, certificate exchange and versions |
| Process authentication data | session_handle, message_kind, input_length, input_data | Which steps are forwarded versus handled by display firmware |
| Check/close session | session_handle, link_generation, completion_status | Revocation, link loss, reauthentication and key destruction |

Use length-delimited opaque authentication material until its schema is proven.
No generic packet relay or speculative selector scanning is proposed.

## Boot, sleep/wake and use ordering

**Observed:** discovery and service matching only. The snapshot does not prove
the order of link enumeration, authentication, locality checks or encryption
enablement. No sleep/wake or protected playback trace is available.

**Proposed:** enumerate the display route -> discover SEP capabilities -> accept
a DRM protection request -> authenticate the actual sink/topology -> confirm
link encryption -> report protection enabled. On hotplug, topology change,
link failure or sleep, stop reporting protection as enabled. Retain the user's
request and reauthenticate on wake with a fresh link/session generation.

## Linux client design

Integrate an internal SEP HDCP service with the display driver's existing
firmware/DRM ownership. Use standard connector protection and content-type
properties, not a second userspace HDCP interface. Linux's
[connector contract](https://docs.kernel.org/gpu/drm-kms.html#standard-connector-properties)
lets userspace request protection, while only the driver can assert it is
enabled. Loss of protection must be visible and trigger reauthentication as
appropriate.

Keep private HDCP keys inside the enforcing hardware. Define how sink I/O,
display firmware and SEP completions are coordinated without holding DRM locks
across blocking mailbox calls. Enforce authentication deadlines and cancellation
when the display route changes. A successful SEP reply alone is not proof the
wire link is encrypted. Follow the [transport contract](transport.md).

## Remaining evidence and acceptance

Read existing HDCP-manager and display-firmware logs, connector state and passive
authentication traces for a known route. Resolve version negotiation, certificate
and revocation ownership, topology changes, encryption enablement and resume.
Protected playback, link changes and authentication attempts are active tests,
outside this read-only collection. Future tests need actual sink authentication,
failure/revocation handling and correct property transitions on hotplug/resume.
