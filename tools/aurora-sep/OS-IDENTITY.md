<!-- SPDX-License-Identifier: GPL-2.0-only OR MIT -->
# Keep the SEP OS identity stable

The SEP OS identity selects the anti-replay lane that the enclave puts an OS
in. Two operating systems that present the same identity share a lane, and
enrolling in one of them invalidates the other's material. Changing the
identity of an installed system orphans everything enrolled under the old
one.

On a normally installed Neo (T8140), m1n1 forwards `/chosen/apfs-preboot-uuid`
from the Apple device tree. It names the booted volume group, so a Linux
install that boots from its own volume group already has its own lane. It is
not macOS's identity unless a boot entry pins macOS's value explicitly, which
is what caused enrolments to collide in the past.

## Selection order

The driver picks the identity in this order and never invents one:

1. If `apple_sep.os_uuid_hi` or `apple_sep.os_uuid_lo` is nonzero, use both
   halves in big-endian order. One zero half is valid. Both halves zero
   means there is no override.
2. Otherwise use `/chosen/apfs-preboot-uuid`, if it is present, well formed
   and nonzero.
3. Otherwise leave the identity unavailable.

`os_identity::select()` implements this order and has KUnit coverage for
explicit precedence, zero halves, byte order, repeated chosen selection and
a missing property. The T8140 profile keeps `IdentitySource::Chosen`, with a
regression test. The `HostPersisted` source declared for other profiles is
not implemented by the selector. Wiring it up would move existing installs
to a new lane, so do not do that without a migration plan. The kernel log
names the source it used and never prints the identity.

## Existing installs

A Fedora Neo with explicit `apple_sep.os_uuid_*` parameters keeps them and
keeps its lane. Carry them unchanged across kernel updates, recovery entries
and boot loader changes. Do not remove them because chosen forwarding exists.

An Omarchy Neo that uses chosen keeps that identity and its enrolments. No
migration or re-enrolment is needed.

A system that is actually pinned to macOS's identity is a separate recovery
case. Stop automatic SEP consumers and plan the recovery with the owner. Do
not change the lane or delete either OS's enrolments automatically.

Installers and boot hooks may preserve an explicit identity, but must never
add, remove or rotate one on an enrolled install. Switching an entry between
explicit and chosen is safe only when both resolve to the same bytes.
Recreating the volume group, restoring boot configuration or reinstalling
can change chosen and needs the same care.

The host state does not record which identity an enrolment was made under.
`aurora-sep-ready` therefore cannot tell whether a migration is needed, and
does not try to.

## Testing changes to identity selection

Never test identity changes on a machine with enrolments or SEP-protected
keys that matter. SEP and xART state is device-wide, so a spare Linux volume
on the same machine does not isolate it. Do not record identities, SEP
payloads, MAC addresses or calibration data in test logs, and do not keep raw
console logs from older kernels or m1n1 builds that print identities.

On a disposable machine:

1. Enrol a disposable Linux and macOS fingerprint using the known-good kernel
   and its unchanged boot entry, and verify both.
2. Boot the candidate with the same volume group and boot arguments. Before
   loading the driver, compare the effective identity with the baseline and
   record only whether they match. Stop if they differ.
3. Verify the existing Linux fingerprint without re-enrolling, reboot and
   verify again, then verify the macOS fingerprint. These checks write
   anti-replay state, so they are not read-only.
4. Repeat with an explicit-parameter entry that was used for the enrolment,
   copying both halves verbatim.
5. Roll back only the kernel and boot entry. Never restore older host-state
   or xART files after the SEP has written, because file rollback does not
   roll back the enclave's counters. On a mismatch or failed verification,
   stop and keep the evidence instead of resetting or re-enrolling.
