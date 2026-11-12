# hibe: secure hibernation

## Purpose, client and parity

**Observed:** `sep-endpoint,hibe` matches `HibernationService`; it is in the early
discovery group on all four Linux profiles. **Inference:** it supplies the SEP
side of secure hibernation. Apple publicly describes Apple silicon hibernation
as protecting saved memory against disclosure, modification and replay
([architecture presentation](https://developer.apple.com/videos/play/wwdc2020/10686/)).
That feature description does not publish hibe's IPC.

Linux has no hibe client. It is needed for parity with secure hibernation;
ordinary suspend-to-idle success does not satisfy this requirement. A hibe
driver is only one prerequisite: CPU offlining/restart, cold device recovery,
firmware memory-map compatibility, image storage and the early resume path must
also work. See the existing [platform assessment](../../tools/aurora-sep/HIBERNATION.md).

## Messages and fields

No verified hibe service message or opcode is available from these captures.
Initialization, key lifetime, image commitment, restore authorization and
abort/invalidation layouts are unknown. The public
[XNU header](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOHibernatePrivate.h)
describes hibernation metadata and authentication material; its structures are
not evidence of SEP mailbox field offsets or selectors.

**Proposed operations, not recovered commands:**

| Candidate operation | Logical fields | Needed evidence |
| --- | --- | --- |
| Prepare image | image_identity, boot_identity, policy_binding, image_format | Session/key custody and allowed callers |
| Commit image | image_identity, authenticated_metadata, image_digest, generation | What is authenticated; when anti-replay state changes |
| Authorize restore | image_identity, policy_binding, authentication_proof, generation | Bootloader/initramfs handoff and enclave decision |
| Abort/consume image | image_identity, transaction_identity, reason | Atomic invalidation and single-use semantics |

These fields are requirements to investigate, not proof that hibe transmits a
key or uses each candidate operation. Keep offsets, widths and enum values
unassigned until a clean source or owned trace proves them.

## Boot, sleep/wake and use ordering

**Observed:** only discovery and macOS service matching. No hibernation cycle
or hibe command stream was captured. Normal suspend and cold image restoration
must be documented separately; the generic Linux SEP PM notifier is not a
hibe transaction.

**Proposed:** establish the verified service/session while allocation and normal
I/O are available -> freeze users and devices -> construct and authenticate the
image -> durably commit the image and associated enclave state in the recovered
order -> power off. On cold boot, select the compatible image before mounting
saved filesystems, obtain enclave authorization, verify image integrity and
freshness, restore devices/memory, and consume/invalidate the image according
to the proven protocol. Abort must leave a defined bootable state.

## Linux client design

Provide an internal hibernation service used by the PM image pipeline and early
resume component. Do not expose raw key export or arbitrary SEP requests.
Represent image/session handles as opaque references and bind them to the
correct boot policy and image generation. Preallocate resources before the
atomic/noirq stages; the actual protocol must determine which calls are legal
there. Avoid a dependency on a userspace daemon during early restore.

Enclave refusal or an incompatible image causes a clean cold boot with explicit
image rejection. Host-only encryption and a host-controlled freshness flag do
not meet the enclave-backed parity goal. No platform availability check is
removed by this design.

## Remaining evidence and acceptance

Use existing public hibernation logs and passive traces of previously authorized
cycles; do not cause hibernation during a read-only collection. Resolve commit
ordering, key destruction, authenticated image scope, policy binding, warm/cold
restore differences and failure recovery. Future qualification needs image
tamper/replay rejection, interrupted writes, failed authentication, repeated
cold restores and cold-boot fallback, in addition to all platform prerequisites.
Those are future tests, not results of this research.
