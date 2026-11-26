# SEP endpoint client specifications

These are clean-room design specifications for the seven unserved SEP endpoints
and the partially served credential and key-store endpoints. They describe the
evidence available on 2026-11-11. **The seven unserved endpoints do not yet have
verified command codecs.** Their proposed operations are Linux design vocabulary,
not recovered Apple selectors. An implementer must resolve the stated wire gaps
before sending any of those operations.

## Evidence and confidence

The evidence consists of public IORegistry service names, our own Linux boot
discovery and diagnostic observations, licensed Linux source, and the primary
public references below. No Apple binary, disassembly, firmware payload, tuning
table, or binary-derived research was used to write these specifications.
Raw IORegistry dumps and machine records are not included in this source tree.

The source baseline for existing codecs is
[`7851a25a10711784421894ca4d3f10606adfb0e6`](https://github.com/aurora-silicon/linux/tree/7851a25a10711784421894ca4d3f10606adfb0e6/drivers/soc/apple),
under GPL-2.0-only OR MIT. The documentation worktree started at
`41c40553f5d2885578d07ddc4914b6c14046aaaa`; it lacks some later codecs and
diagnostics. A source inventory is evidence of code presence, not hardware
qualification or coverage of every macOS command.

Each assertion is classified as:

- **Observed:** directly present in an IORegistry or diagnostic capture.
- **Source:** implemented in the pinned licensed Linux source.
- **Inference:** suggested by a service name or consumer; requires confirmation.
- **Proposed:** a Linux architecture or API decision, with no implied Apple wire value.
- **Unknown:** the supplied evidence does not establish the fact.

## Endpoint inventory

Every supported Apple silicon SEP advertises the same 12 endpoints
(`cntl`, `sbio`, `scrd`, `sse`, `hdcp`, `xars`, `sks`, `xarm`, `hibe`,
`pnon`, `skdl`, `stac`) across M1 Pro (T6000), M2 (T8112), M3 (T8122),
M4 (T8132), M4 Pro (T6040) and A18 Pro (T8140).

| Ep | Endpoint | Public ioreg child | Linux coverage (`main`) | Coverage gaps / notes | Specification |
| --- | --- | --- | --- | --- | --- |
| `0x00` | `cntl` | (SEP manager) | Full (`sep.rs`, `control.rs`, `proto.rs`, `pm.rs`) | Discovery, OOL registration, hwrng, sleep/wake handshake complete | [transport](transport.md) |
| `0x08` | `sbio` | (biometric) | Active (`sbio.rs`) | Built-in Touch ID enrol/match/catacomb/lockout; external Magic Keyboard Touch ID sensor relay and display-sleep `PROTECTED_CONFIG` refresh remain | — |
| `0x0a` | `scrd` | No child visible | Active (`scrd.rs`) | 6 selectors implemented (init, tracked create, externalize, delete, verify policy, verify ACL); broader macOS policy set and multi-requirement parser remain | [scrd](scrd.md) |
| `0x0c` | `sse` | `AppleSSE` / `AppleSSEUserClient` | Attach-only (`sse.rs`, `diag/sse`, `diag/sse_events`) | Sends nothing; unsolicited events counted. Full support out of scope (Apple Pay / Wallet SE pairing only) | [sse](sse.md) |
| `0x0e` | `hdcp` | `AppleSEPHDCPManager` | No client | Needs SEP client + DCP HDCP relay + DRM Content Protection wiring | [hdcp](hdcp.md) |
| `0x10` | `xars` | `AppleSEPXARTService` | No client | Host-initiated xART control: shutdown unmount/flush barrier is the main parity item | [xars](xars.md) |
| `0x12` | `sks` | No child visible | Active (`sks.rs`, `keybag.rs`, `userbag.rs`, `refkey*.rs`) | 24 selectors implemented (keybags, class keys, ref-keys, FDE/VEK); additional macOS session/timer variants remain | [sks](sks.md) |
| `0x13` | `xarm` | `AppleSEPXARTService` | Active (`xarm.rs`, `xart_store.rs`, `xart_guard.rs`) | Enclave-initiated root/session read/write/delete, OS UUID, xART guard complete | — |
| `0x14` | `hibe` | `HibernationService` | Attach-only (`hibe.rs`, `diag/hibe`) | Sends nothing; prepare-reply layout typed and wiped on drop. Full support gated on Linux Apple-silicon hibernation (see plan in `hibe.md`) | [hibe](hibe.md) |
| `0x15` | `pnon` | No child visible | Read-only digest (`pnon.rs`, `diag/pnon`) | Reads blessed LocalPolicy nonce digest (`0x0e`) on 5-7 page layout (`0x05050707`) only; 2-4 page layout (`0x02020404`, M1 Pro 13.5 / A18 Pro 26.6.2) pending in draft PR #2810; additional read queries (remote/recovery digests, boot mode, pairing verification) and mutating LocalPolicy/hibernation transactions not yet implemented | [pnon](pnon.md) |
| `0x17` | `skdl` | `CoreKDLDriver` / `CoreKDLUserClient` | Attach-only (`skdl.rs`, `diag/skdl`) | Sends nothing; generic-transfer boot framing and offender-list bounds check typed. Full support out of scope (Apple Pay / FairPlay deny-list only) | [skdl](skdl.md) |
| `0x18` | `stac` | `AppleTrustedAccessoryManager` / `UserClient` | Power-state online (`stac.rs`, `diag/stac`) | Sends online (`0x0f`) at bring-up and after every enclave wake; Magic Keyboard with Touch ID pairing/session/auth ops and HID transport remain (earlier draft PR #1810 closed; needs per-device interface and lifecycle wiring) | [stac](stac.md) |

The IORegistry captures include the AppleSEPManager subtree on T8112, macOS
build 25G83, and the wider T6000 registry and class queries on build 25C56.
They establish matching children for six of the nine requested endpoints.
It does **not** establish a direct child binding for AppleKeyStore,
AppleFDEKeyStore, or AppleCredentialManager. These names identify the related
macOS service families, observed elsewhere in the T6000 registry; the routing
to sks/scrd needs transport evidence. A missing child does not prove absence of
a macOS client. The T6000 class queries for AppleSEPKeyStore and
AppleSEPCredentialManager return no service; a query label is not an observed
driver. The boot-log sender AppleSEPKeyStore is observed independently.

## Boot and power-state evidence

The four Linux profiles advertise the same endpoint set. The early group is
cntl, hibe, stac, pnon, hdcp, xars, xarm; their exact order varies. The later
group is sbio, skdl, sse, scrd, sks; sbio and skdl can trade order. sks is last
in the inspected boots. The later group follows the xarm persistent-state
exchange; this is temporal evidence, not proof that xars has completed any
transaction or that all clients may start at that point.

Discovery descriptors/configuration messages belong to the discovery service.
Their type values are **not** command selectors on the advertised endpoint.
The configuration data differs across profiles for pnon. No interpretation of
its subfields has been established; do not turn it into a capability table.

Saved diagnostic boots report no unhandled messages and no sleep cycles.
A subsequent read-only T8112 snapshot also reports those values. This means
no unsolicited traffic reached the driver's default dispatch branch during
those observation windows. It says nothing about requests the host never sent,
commands handled incompletely by sks/scrd, or sleep/wake/use ordering in macOS.

## Shared implementation contract

Read [transport and validation](transport.md) before implementing an endpoint.
Bind services by discovered identity, preserve protocol/profile differences,
and give every request a named field schema with documented bounds. Existing
numeric mappings stay in the licensed codec source; this series does not copy
opcode tables, opaque discovery words, magic headers, or observed Apple words.
Unknown fields stay unknown rather than acquiring names from guesses.

All nine endpoints need coverage accounting for full SEP interface parity.
Feature parity is more specific: ordinary desktop operation does not require
every optional service. An advertised but unsupported service must remain
reported as unsupported. Enclave-enforced authorization, key custody and replay
protection cannot be replaced by a host Boolean, cached success, or software key.

## Primary public references

- [Apple: Secure Enclave](https://support.apple.com/en-au/guide/security/sec59b0b31ff/web): security architecture; no endpoint opcode specification.
- [Apple: Apple silicon system architecture](https://developer.apple.com/videos/play/wwdc2020/10686/): secure hibernation protects saved memory against disclosure, modification and replay.
- [Apple open-source XNU hibernation I/O](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOHibernateIO.cpp) and [header](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOHibernatePrivate.h): APSL-2.0 source references, not copied code or a hibe endpoint codec.
- [Apple: Magic Keyboard with Touch ID](https://support.apple.com/en-ca/guide/security/secf60513daa/web): secure pairing architecture; does not identify stac wire commands.
- [Apple: LocalPolicy contents](https://support.apple.com/en-gb/guide/security/secc745a0845/web): policy nonce concept; does not identify pnon routing.
- [Apple: NFC and SE platform security](https://support.apple.com/en-gb/guide/security/secda20f3f41/web): iPhone architecture, useful context only; not proof of a Mac sse protocol.
- [Linux: DRM connector protection](https://docs.kernel.org/gpu/drm-kms.html#standard-connector-properties): integration contract for hdcp.
- [Linux: hibernation](https://docs.kernel.org/power/swsusp.html): image/restore ordering; platform support must also be implemented.

Public documentation establishes feature goals, not the missing private IPC
layouts. Consult the per-endpoint evidence gaps before attempting a client.
