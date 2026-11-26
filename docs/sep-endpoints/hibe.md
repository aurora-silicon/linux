# hibe: secure hibernation

## Purpose, client and parity

**Observed:** `sep-endpoint,hibe` matches `HibernationService`; it is in the early
discovery group on all four Linux profiles. **Inference:** it supplies the SEP
side of secure hibernation. Apple publicly describes Apple silicon hibernation
as protecting saved memory against disclosure, modification and replay
([architecture presentation](https://developer.apple.com/videos/play/wwdc2020/10686/)).
That feature description does not publish hibe's IPC.

Linux checks the endpoint at boot and sends it nothing, as macOS does outside a real hibernation (`drivers/soc/apple/hibe.rs`, `diag/hibe`). It is needed for parity with secure hibernation;
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

## Boot, sleep/wake and shutdown behaviour

**Observed:**

- **Boot:** macOS sends **nothing** on `hibe` at normal boot (confirmed across
  full-boot captures on J473 macOS 27.2 and J616s macOS 27.0). The host checks
  the one-page layout (`0x01010101`) and allocates buffers without registering
  them or sending a request.
- **Ordinary sleep/wake (`s2idle` / S2R):** zero `hibe` messages. Ordinary
  suspend uses the control-endpoint sleep handshake (`cntl`), not `hibe`.
- **Shutdown:** zero `hibe` messages.
- **True hibernation only:** before image commit, macOS registers OOL buffers
  and issues `prepare` (obtaining a 64-byte opaque wrapped image key, a 32-bit
  boot word, and a 64-byte opaque resume authorization). On a cold hibernation
  boot, it sends `resume` with the retained resume authorization and clears the
  buffer after any outcome.
- **Impact of attach-only on Linux:** zero SEP errors, zero timeouts, zero
  unhandled messages, and zero suspend/wake regressions across J473, J616s,
  J713, J615, M1 Pro, M2 Air and MacBook Neo (`diag/hibe=idle`,
  `diag/unhandled_messages=0`). Calling `prepare` without a matching image and
  resume is unsafe because secure hibernation provides anti-replay with no
  recovered abort operation and may rotate enclave freshness state.

## Minimal behaviour and full-support plan

- **Minimal correct behaviour (implemented in `drivers/soc/apple/hibe.rs`):**
  validate discovery identity (`"hibe"`) and one-page layout (`0x01010101`),
  report `diag/hibe` (`idle`/`absent`), register no OOL buffers, send nothing,
  count any arrival as unhandled, and keep `Prepared` reply material zeroized
  on drop via volatile writes.
- **Full support plan (Linux hibernation with SEP-wrapped key):**
  1. **Platform prerequisites (outside SEP; see `tools/aurora-sep/HIBERNATION.md`):**
     implement ARM64 CPU offlining/restart (replacing spin-table no-`cpu_die`),
     cold post-power-loss firmware restore for DCP/GPU/coprocessors (freeze/thaw/
     restore PM callbacks instead of retained firmware), and firmware memory-map
     preservation across cold boot.
  2. **Owned wire capture:** record one attended macOS true hibernation cycle
     (`hibernatemode 25`, physical power-button wake) to confirm the exact
     `hibe` `prepare`/`resume` mailbox words and ordering relative to `pnon`
     hibernation preparation and disk commit.
  3. **In-kernel PM client (`drivers/soc/apple/hibe.rs` + `pnon.rs`):** expose
     two internal-only kernel calls — `prepare_image()` (pre-allocated OOL
     buffers, late before image commit, returns opaque wrapped key + boot word
     + resume auth, scrubs DMA buffers) and `authorize_resume()` (cold
     hibernation wake only, single-use, scrubs auth buffer regardless of
     outcome). Never export keys, auth blobs or raw selectors to userspace.
  4. **Image pipeline & fail-closed resume:** integrate the SEP-wrapped key and
     authenticated metadata with Linux's hibernation writer and early initramfs
     resume path. Any enclave refusal, timeout (5 s bound), stale auth or
     metadata mismatch must abandon resume and fall back cleanly to a normal
     cold boot.

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
