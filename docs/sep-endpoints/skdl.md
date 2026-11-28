# skdl: CoreKDL kernel-extension deny-list service

## Purpose, client and parity

**Observed:** `sep-endpoint,skdl` matches `CoreKDLDriver`, with
`CoreKDLUserClient` published as its user-client class. The public `corekdld`
manual identifies its purpose: maintaining a signed kernel-extension deny-list
that gates Apple Pay and FairPlay operations. Linux discovers `skdl` in the
later group (`0x01010101`, one 16 KiB page each way) on all supported profiles.

Linux checks the endpoint at boot and sends it nothing (`drivers/soc/apple/skdl.rs`,
`diag/skdl`). Leaving it attach-only causes zero SEP errors, zero timeouts,
zero unhandled messages and zero power or wake regressions across J473, J616s,
J713, J615, M1 Pro, M2 Air and MacBook Neo.

## Boot, sleep/wake and shutdown behaviour

**Observed:**

- **Boot (macOS 27.2 full-boot captures on J473):** `CoreKDLDriver` registers
  one 16 KiB OOL page per direction and sends two generic-transfer (`tag=0xfc`,
  same 28-byte header as `sbio`/`stac`) requests:
  1. `op 0x01` (set protocol version): 8-byte `u64` body (`1`) -> empty
     status-0 reply.
  2. `op 0x06` (submit loaded-kernel receipt): `u64` receipt length followed by
     the signed receipt bytes -> empty status-0 reply.
- **Ordinary sleep/wake:** zero `skdl` messages (confirmed in SEPWIRE short
  sleep/wake captures; only cold hibernation resume re-negotiates).
- **Shutdown:** zero host `skdl` messages (SEP internally notifies `skdl`
  during `xars` xART disable).
- **Runtime:** `corekdld` queries installed policy version, offending-extension
  records (70 bytes each: 20-byte code directory hash, 10-byte team ID,
  32-byte bundle ID hash, 8-byte version), or protected-service allowed/blocked
  state when a protected service asks.

## Minimal behaviour and full-support assessment

- **Minimal correct behaviour (implemented in `drivers/soc/apple/skdl.rs`):**
  validate discovery identity (`"skdl"`) and one-page layout (`0x01010101`),
  report `diag/skdl` (`idle`/`absent`), count any unsolicited arrival as
  unhandled, and send nothing. Linux must not send `set_version` or a synthetic
  receipt without a genuine receipt producer and consumer: duplicate/volatile
  bookkeeping semantics are unproven and no Linux feature consumes the result.
  Clean-room generic-transfer framing helpers and fail-closed offender-list
  bounds validation are unit-tested in `apple_sep_skdl`.
- **Full support (out of scope for Linux):** CoreKDL exists to attest signed
  macOS kext receipts to the SEP in order to gate Apple Pay and FairPlay.
  Linux loads ELF kernel modules rather than Apple-signed kexts and has neither
  an Apple-verifiable receipt producer nor an Apple Pay / FairPlay consumer.
  Keep `skdl` attach-only unless an actual enclave-gated Linux service and
  verified measurement path exist; never forge a receipt or override a deny
  state in host software.

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
