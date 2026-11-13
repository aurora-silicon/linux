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

| Endpoint | Public ioreg child in the captured SEP subtree | Linux coverage | Feature association | Specification |
| --- | --- | --- | --- | --- |
| xars | AppleSEPXARTService | No client | Second xART service; relationship to xarm unknown | [xars](xars.md) |
| hibe | HibernationService | Checked at boot, nothing sent (`diag/hibe`); no Linux hibernation writer | Secure hibernation: wrapped image key and resume authorisation | [hibe](hibe.md) |
| hdcp | AppleSEPHDCPManager | No client | Protected display links | [hdcp](hdcp.md) |
| pnon | No child visible | No client | Nonce service, inferred from name | [pnon](pnon.md) |
| skdl | CoreKDLDriver / CoreKDLUserClient | No client | CoreKDL: signed kernel-extension deny-list gating Apple Pay and FairPlay (public `corekdld` manual) | [skdl](skdl.md) |
| sse | AppleSSE / AppleSSEUserClient | Protocol query once after bring-up (`diag/sse`); no Secure Element operations | Secure Enclave to Secure Element sharing state; nfcd consumer | [sse](sse.md) |
| stac | AppleTrustedAccessoryManager / AppleTrustedAccessoryManagerUserClient | No client | Trusted accessories; bluetoothd consumer observed | [stac](stac.md) |
| scrd | No child visible | Partial credential/context codec | Credential policy and biometric authorization | [scrd](scrd.md) |
| sks | No child visible | Partial key-store codec | Keybags, enclave keys, storage encryption | [sks](sks.md) |

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
