.. SPDX-License-Identifier: GPL-2.0-only

MediaTek MT7932 radio on the MacBook Neo
========================================

J700 MT7932 Wi-Fi uses cfg80211 with firmware-managed WPA2-PSK, WPA3-SAE
and Enhanced Open (OWE) authentication on 2.4 and 5 GHz. Bluetooth uses a
transport on the radio's second PCIe function.

The Wi-Fi driver carries configuration, scan IE and WPA2 AKM compatibility
fixes on top of its original import. Existing source notices are retained.
Co-authored by DJ (DjDeveloperr), Ace (Acelogic), and Ryan Murray.

Platform and configuration
--------------------------

Use the T8140 PCIe bootstrap described in
``Documentation/PCI/controller/apple-t8140.rst``. Build the provider with
``CONFIG_PCIE_APPLE_T8140_BOOTSTRAP=y`` and select it through reciprocal
host/engine DT links. Public m1n1 must populate the J700 ``wifi0`` endpoint's
own ``local-mac-address``; a zero placeholder is rejected.

The tested configuration includes ``CONFIG_MT7932_FULLMAC=m``,
``CONFIG_CFG80211=m``, ``CONFIG_BT_MTK7932_PCIE=m``, ``CONFIG_BT_BREDR=y``,
``CONFIG_BT_LE=y``, ``CONFIG_CRYPTO_AES=y`` and ``CONFIG_CRYPTO_CMAC=y``.
The Bluetooth transport can be built with modular Bluetooth and system
sleep. The separately selected bootstrap provider has its own lifecycle
restrictions. A runtime guard refuses kexec while radio admission or retained
bootstrap DMA ownership is active. Kernels may still support kexec elsewhere.
The station uses 2.4 GHz channels 1-13 and the 5 GHz channels 36-64,
100-144 and 149-165 that the cfg80211 regulatory domain permits. Radar (DFS)
channels stay passive: the firmware listens on them while the station is not
associated, but never probes or joins an access point there, and scans made
while associated skip them. A channel whose restrictions the driver cannot
apply (reduced power, no OFDM, PSD limits) is disabled rather than used, and
so is a channel the country package does not permit: every power limit of
that channel in the applied country package is left undefined.

Use the ordinary cfg80211 regulatory database and applicable country policy.
The validated first-admission fallback is kernel country 00 with firmware XZ.
A modular cfg80211 loads ``regulatory.db`` from the root filesystem; a
built-in one tries before the root filesystem is mounted and then needs
``iw reg reload``.

The radio functions appear while the kernel boots, before the root
filesystem is mounted, and ``mt7932-fullmac`` reads its firmware while
probing. Build it as a module so that udev loads it from the root
filesystem, or provide its firmware in the initramfs. The driver disables
ASPM and clock power management on its link before starting the device,
matching the tested admission sequence, which used the PCIe ASPM
performance policy.

``mt7932_bt_pcie`` defers its probe until ``mt7932-fullmac`` is bound to
function 0, so the Bluetooth firmware always starts after the Wi-Fi firmware,
as in the tested sequence, however the drivers are loaded. Bluetooth therefore
stays unbound while the Wi-Fi driver is missing or has failed, including when
a failed Wi-Fi function stays bound to keep DMA it could not stop. A device link
makes the driver core unbind Bluetooth before Wi-Fi; unloading
``mt7932_bt_pcie`` does not affect Wi-Fi. Wi-Fi pins its module before publishing
DMA pointers, and refuses function reset while its Bluetooth sibling is live.
The probe rejects a function that is
not cold, which is what now prevents a reprobe after a failed or uncertain
admission. The transport also
disables ASPM on the shared link. Like the Wi-Fi driver, it reads its firmware
while probing, so build it as a module. The tested Wi-Fi driver owns function
0 and Bluetooth owns function 1.

Use a saved NetworkManager WPA2 profile matching the current interface.
KDE audio requires BlueZ, PipeWire, its PulseAudio compatibility service,
WirePlumber and Bluetooth audio plugins in the logged-in user's session.

Country packages
----------------

Every regulatory change selects a country package again, without a reload.
For country CC the driver uses the first of these that exists and is valid:

1. ``policy-v2/CC.bin``: J7RP version 2 (version 1 plus 6 GHz VLP pages);
2. ``policy/CC.bin``: J7RP version 1 (2.4 and 5 GHz, at most 16 KiB);
3. ``policy/world-XZ.bin``: the world package.

Country 00 uses only the world package. ``policy/`` holds version 1 only and
``policy-v2/`` version 2 only; a package in the wrong directory is refused.
Kernels without version 2 support never read ``policy-v2/``, so version 2
packages can be installed while those kernels remain bootable.

A country without a usable package of its own falls back to the world
package and logs one ``REGULATORY_FALLBACK`` line. cfg80211 keeps the
requested country and its channel list; the firmware gets domain XZ and
only the channels both the country and the world package permit. The world
package has no 6 GHz section, so 6 GHz stays off. Installing the country's
package takes effect on the next regulatory change. Only a missing or
invalid world package leaves the radio without a regulatory domain.

The applied package can be read back, as root, from
``/sys/kernel/debug/ieee80211/phyN/mt7932_policy``: requested country,
firmware domain, source directory and path, J7RP version, size, the first
eight bytes of its SHA-256, the 6 GHz primaries in the firmware domain, the
fallback reason and the result of each lookup.

Station security
----------------

The driver advertises PSK and SAE with CCMP only. Firmware performs SAE
authentication and the four-way handshake and owns PTK, GTK and IGTK;
there is no host key-install interface or external-auth exchange.

SAE offload accepts nl80211 WPA version 3, the combined version 2/3 mask,
and version 2 for older offload clients. PSK requires the version 2 bit.
The first AKM always selects the firmware arm; a version mask does not
permit security fallback.

For SAE, userspace supplies SAE authentication, an 8-63 byte password and
required or optional management frame protection (MFP). Firmware uses required
MFP for the SAE arm. On a PSK+SAE transition network, the first AKM supplied
by userspace selects the arm: PSK keeps the existing binary-PMK request and
disables MFP, while SAE uses the password and required MFP. PSK with required
MFP and PSK-SHA256 are unsupported.

BIP-CMAC-128 is not advertised because WPA2 with required MFP has not been
implemented or qualified. Advertising BIP would also make supplicants request
required MFP on WPA2 networks. iwd requires BIP-CMAC-128 before selecting SAE,
so it remains WPA2-only with this driver, including on transition networks;
it cannot join a pure-SAE network. SAE offload support here targets
NetworkManager with wpa_supplicant, pending the hardware qualification below.

The standard SAE PWE settings (unspecified, hunting-and-pecking,
hash-to-element and both) are accepted; firmware chooses the exchange without
a host PWE field. Exact PWE policy behavior on 2.4 and 5 GHz still needs
qualification. SAE-PK, SAE-EXT-KEY and additional ciphers are not advertised. OWE and
6 GHz support are described below.

J700 hardware testing with NetworkManager and wpa_supplicant on a PSK+SAE
transition network has confirmed, on 2.4 GHz channel 6 and 5 GHz channel 149:

* SAE with required MFP (``key-mgmt=sae``), including DHCP, DNS and HTTPS;
* PSK with MFP disabled, and a ``wpa-psk`` profile with optional MFP, which
  joins the transition network as PSK;
* repeated reconnection through NetworkManager after each change of profile.

The request layout, transition admission and PMK derivation also have
automated tests. A pure WPA3 network, exact PWE policy behavior, WPA2
throughput regression comparisons and long-duration reliability still need
qualification. 2.4 GHz throughput is lower than 5 GHz with every security
mode.

The SAE key cipher is at offset 0x44; offset 0x48 is the 16-bit key-flags
field and stays zero.

Enhanced Open (OWE)
-------------------

The station advertises the OWE AKM and ``OWE_OFFLOAD`` on the supported 2.4
and 5 GHz channels. Use an OWE profile with CCMP, open-system authentication
and required management frame protection, without a PSK or SAE password.
Firmware performs the Diffie-Hellman exchange, generates RSN information and
owns the handshake and key installation. Userspace must honor the offload
feature instead of expecting a host-generated Diffie-Hellman element in the
association response.

Before the first OWE association of a firmware session, the driver reserves
temporary live peer IDs and performs a typed disconnect and queue cleanup.
An OWE authentication timeout, firmware rejection or firmware disconnect
then permits one retry inside the connect worker. Before retrying, the
driver waits for its typed disconnect and retires queues and the old peer
through the normal ownership fences. A
failed retirement, cancellation or RF failure ends the request. The retry
shares a ten-second association-event deadline and programs the station
identity again before association. No further retry is made by the worker.

For OWE transition mode, the supplicant selects the encrypted BSS and can use
a directed scan to find its hidden SSID. Pure OWE, transition mode and
first-authentication behavior after a cold boot still need hardware
qualification on every band.

6 GHz country packages
----------------------

The qualified two-stream PHY profile exposes UNII-5..8 with station HE and
HE 6 GHz capabilities, up to 80 MHz. Firmware band 3 is independent of the
single-interface MAC index. Channel numbers always carry their nl80211 band.
Only SAE and OWE with required MFP are admitted on 6 GHz. Hunting-and-pecking
only is refused there; firmware owns H2E. These checks extend the SAE and
OWE layers without another crypto or authentication implementation.

Wildcard 6 GHz scans listen passively. Hidden SSIDs use directed scanning
when NO_IR permits it. RNR-only requests remain passive until target-list
serialization is implemented. Full-band scans are split into bounded
32-channel commands. Firmware event channel layout is checked before OCA
association calibration, queue binding and the two association power pages.

J7RP version 1 remains valid and never authorizes 6 GHz. Version 2 packages
are installed under ``policy-v2/``. Generate them from the original text
tables and the installed version-1 packages::

  python3 tools/asahi/mt7932-policy-6ghz.py --tables /path/to/IZUBA \
    --legacy-dir /path/to/policy --output-dir /path/to/new-policy-root
  python3 tools/asahi/mt7932-policy-6ghz-tests.py

The default is every eligible VLP country, with skipped countries reported.
Use repeated ``--country CC`` options to select a subset. Packages are written
under ``policy-v2/`` with a sorted SHA-256 inventory beside that directory.
Input hashes select the authenticated 25G83 revision. All files are validated
before publication and the output root must be absent or empty. Only code is
distributed; the power tables and generated packages stay on the machine
they come from.

The producer reads VLP sections and the 6 GHz country matrix. Mode 2 in a
channel's operating-class set grants VLP eligibility, intersected with
available per-rate data. It emits 14 native per-rate pages and 14 legacy
pages, including the eight-entry selector-02 and selector-0f association
pages. The unsupported-rate bitmap is not generated, so the firmware's
default rate accounting applies and still needs qualification.

No regulatory model is replaced: cfg80211 flags, including NO_IR and VLP
client restrictions, remain authoritative. The driver consumes EIRP and PSD
limits by reducing native half-dBm rate cells before publication, preserving
unsupported and unpopulated sentinels. Association reuses the reduced pages.
PSD rounding uses allocation bandwidth lower bounds, so it cannot raise a
limit. Unrepresentable reductions disable the affected rate. Legacy-band
power handling is unchanged. LPI/AFC operation is not added by VLP packages.

J7RP v1/v2 byte format and producer contract
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

J7RP is a Linux country-policy container, not an Apple file. The following
contract describes the wire format and the v2 derivation independently of
its Python implementation, so an installer with an existing v1 builder can
implement the extension without importing GPL code. Obtain original tables
from the user's own macOS or IPSW at installation time; do not redistribute
the Apple tables. Examples below contain no original power data.

All offsets are zero-based bytes, relative to the containing structure.
Integers are unsigned little-endian unless specified otherwise. Structures
are packed with no alignment gaps. Alpha2 is exactly two uppercase ASCII
letters, without a terminator. XZ is the firmware world identity; Linux
country 00 uses the v1 file ``policy/world-XZ.bin``. All other v1 files use
``policy/CC.bin``. V2 files use ``policy-v2/CC.bin`` under the same
``mediatek/mt7932/`` firmware root. Never replace a v1 file with v2: older
kernels reject version 2 and packages larger than 16384 bytes.

Outer header and v1 records
^^^^^^^^^^^^^^^^^^^^^^^^^^^

The 16-byte outer header is::

  Offset  Size  Meaning
   0       4    ASCII J7RP
   4       2    Version: 1 or 2
   6       2    Country alpha2
   8       1    Mode-record count M: 2 or 3
   9       1    Legacy table count: 13 (also in v2)
  10       2    Reserved, zero
  12       4    Entire file length, including header and any v2 extension

Reject lengths below 16, v1 lengths above 16384, v2 lengths above 65536,
unknown versions, nonzero reserved fields, or a country different from the
requested firmware country. After the header come M consecutive 328-byte
mode records, then exactly 13 legacy tables in index order. A mode record
is a CID 0xca command body::

  Offset  Size  Meaning
   0       4    Reserved, zero
   4       4    Payload length N, 1..320
   8       N    ASCII chip-configuration string, excluding any NUL
   8+N   320-N  Zero padding

The v1 parser checks the zero word and the length range. The producer must
retain the original country's mode strings and padding; the v2 extension
never regenerates or changes these records.

Every legacy table is a complete CID 0x5d command body, without a separate
record wrapper. Its little-endian length at offset 2 includes its own
header. Use that length to find the next table; every table must fit
entirely within the file. Tables 1..9 have this layout::

  Offset  Size  Meaning
   0       1    Version 3
   1       1    Tag 0x0a
   2       2    Body length = 44 + 122*C
   4       1    Entry count C
   5       1    Band: 1 for tables 1..2, 2 for tables 3..9
   6       1    Last page in band: 1 for tables 2 and 9, otherwise 0
   7       1    Reserved, zero
   8       2    Alpha2, identical to outer header
  10       2    Reserved, zero
  12      32    One-stream padding bitmap, indexed by channel number
  44     122*C  Entries: one channel byte followed by 121 rate bytes

Table 1 has eight entries (channels 1..8); table 2 has five (9..13),
except world XZ has six (9..14). Channel 14 data is not Linux permission.
Tables 3..8 have eight entries each; table 9 has five. Their ordered
53-channel namespace is::

  36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,
  100,102,104,106,108,110,112,114,116,118,120,122,124,
  126,128,132,134,136,138,140,142,144,149,151,153,155,
  157,159,161,163,165,167,169,171,173,175,177,181

Rate-byte projection is specified below and is shared with v2. All XZ
per-rate tables carry the same padding bitmap; the parser additionally
checks XZ's 2.4 GHz channel order.

The four remaining legacy tables have a zero-initialized 44-byte header:
byte 0 is zero, byte 1 is 2, offsets 2..3 contain total length, and bytes
4..7 and payloads are as follows. Unassigned bytes remain zero::

  Index  Length  Bytes 4,5,6,7  Payload at offset 44
   0      532     4,2,0,5       360-byte unsupported-rate bitmap,
                                then 128 zero bytes
  10      144    20,1,1,1       20 SAR entries, each index byte 0..19
                                followed by four signed power bytes
  11       58    13,1,1,4       13 signed common-path backoff bytes
                                for channels 1..13, then one zero byte
  12      265    17,1,1,2       17 SDB entries, each index byte 0..16
                                followed by twelve signed power bytes

Table 10 also carries alpha2 at offsets 8..9; offsets 10..11 are zero.
The kernel validates each fixed length, version and type, and table 10's
country. SAR columns are siso_wf0, siso_wf1, mimo_wf0, mimo_wf1. SDB columns
are cck_siso_wf0/1, ofdm_siso_wf0/1, ofdma_siso_wf0/1, cck_mimo_wf0/1,
ofdm_mimo_wf0/1, ofdma_mimo_wf0/1, in that order. SAR ``x`` and SDB
``X``/``x`` encode 0x7f. Missing common-path backoffs encode zero.
V2 copies all of these v1 bodies verbatim, including the unsupported-rate
bitmap; it does not derive an additional 6 GHz unsupported-rate bitmap.

For v1, the thirteenth table must end exactly at EOF. For v2 it must be
followed immediately by the extension, with no padding. Copy the validated
v1 file, change only the outer version to 2 and the total length, and append
the extension. Non-XZ v1 sizes are 10119 (two modes) or 10447 (three modes).
The producer rejects a v1 country, version, internal table identity or
length mismatch before publishing any output.

6 GHz eligibility and authenticated inputs
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Authenticate both complete ASCII files by SHA-256 before parsing::

  IZUBA_TxPwrLimit6G_MT79x1.dat
    bc22e249526867b62542ef61231c14b7027ae49f8c926ad6d73029c742dc53bc
  IZUBA_db.dat
    a61e0b70cbc9a381fc41ff9afa8f036cbd032cadad66209134d4a5adfa18c8b2

A country is eligible only when CC is two uppercase ASCII letters and all
of the following hold:

* A ``[CC,VLP]`` power section exists. LP and SP sections cannot substitute.
* The ``<6gCC,...>`` matrix contains a row for CC.
* A structurally valid ``policy/CC.bin`` version-1 input matches CC,
  including the per-rate and SAR table countries.
* At least one primary ``ch = 1+4*i``, i = 0..58, has operating-class
  token 2 in its matrix cell and at least one projected rate byte other
  than 0xc4 or 0x7f. Match slash-delimited tokens, not substrings.

The bitmap is precisely the intersection in the last bullet. Channel 2,
bonded centers and padding bits never grant primary permission. A VLP
section with empty groups, all sentinel cells, or power only outside the
matrix's VLP primaries is skipped with a reason. Missing VLP, matrix or v1
inputs are also reported as skips; malformed authenticated data or a
present but invalid v1 package aborts the run. XZ has no 6 GHz data in this
revision and must retain v1 with 6 GHz off. These are data eligibility
rules, not a replacement for cfg80211 regulatory restrictions.

The default candidate set is the sorted union of VLP-section countries and
6 GHz matrix countries. Repeated ``--country CC`` options instead select
that set of uppercase alpha2 values, deduplicated and sorted. Discovery
reports pseudo-country identifiers as skips rather than emitting files for
them. The pinned
revision has 127 VLP sections and 132 matrix rows; section counts alone do
not establish the number of usable outputs.

Power-row derivation and rate-byte projection
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Skip blank lines, comment lines beginning with ``#`` after whitespace, and
version lines beginning with ``{Ver:`` or ``<Ver:``. Sections are
``[CC,mode]``. Groups open with ``<name,...>`` and close with ``</name>`` or
``</end>``. Reject duplicate sections or groups, unknown groups, unclosed
groups, and rows outside groups. Each VLP section must contain exactly
``ofdm, ru26, ru52, ru106, ru242, ru484, ru996, ru996X2`` groups, possibly
with no rows. Trim whitespace around comma fields. A row contains a label
and eight cells for ofdm or 36 for an RU group. Each cell is ``X``, ``x``,
``Y``, or a decimal integer -128..127. X/x encode 0xc4, Y encodes 0x7f;
integers encode their low eight bits as signed two's-complement values in
native half-dBm units. Preserve sentinels rather than treating them as
numeric limits.

Read the first 110 channel columns from the 6gCC header in file order.
Their names are ``6gN`` with distinct N. The trailing columns must be
``vht40,vht80,vht160,c2clpi,vlpp2p,flag``. Each matrix row has country,
110 operating-class cells, six trailing cells and a trailing comma. A
channel cell is 0..3, optionally followed by slash-separated 1..3 tokens.
Reject duplicate countries, wrong row widths or invalid mode sets.

The pinned 110-channel order is channel 2 followed by ascending odd
channels 1..233, retaining width classes 0..3 and excluding channel 231.
The width class W of band-3 channel 2 is 0. For other channels let d=ch-1:
W is 0 when ``d & 3 == 0``, 1 when ``d & 7 == 2``, 2 when
``d & 15 == 6``, 3 when ``d & 31 == 14``; otherwise it is invalid.
These classes control row placement, not Linux channel permissions.

Initialize every channel with all cells X in the following fifteen groups,
in order (parentheses give dense-image width)::

  cck(4), ofdm(8), ht20(24), ht40(24),
  vht20(30), vht40(30), vht80(30), vht160(30),
  ru26(36), ru52(36), ru106(36), ru242(36), ru484(36),
  ru996(36), ru996X2(36)

Process each input group's rows in file order; later writes overwrite earlier
ones. Labels are ``chNNN``, ``chNNN-cdd``, ``chNNN-MMM`` or
``chNNN-MMM-cdd``; both channel numbers must exist in the namespace. Let
start=NNN, end=MMM (or NNN when omitted), g be the group's index above,
and Ws/We their width classes.

* A plain ``chNNN`` label, or any label with Ws=0, writes cells at offset
  zero of group g on start. For ofdm, also write those cells at offset
  zero of ht20 when Ws is 1..3 or start is channel 2.
* For other labels, write on end. Non-ofdm RU groups ru242/ru484/ru996
  select destination group index Ws+11, offset zero; other RU groups
  retain their own index and use offset zero.
* For ofdm with Ws=We, select ht20 offset 8. For (Ws,We)=(2,1) or
  (3,2), select ht20 offset 16 without cdd, or ht40 offset 0 with cdd.
  For (3,1), select ht40 offset 8 without cdd, or offset 16 with cdd.
  Otherwise select ht20 offset (Ws-1)*8. Reject a write beyond a group's
  width.

Concatenate the fifteen groups at their dense widths to obtain a 432-byte
value image V. Project V to 121 rate bytes by walking this descriptor list
in order. Each descriptor gives source offset, repetition count, stride,
and the offsets copied within each repetition::

  Source  Repetitions  Stride  Offsets
  0x000       1          4    0
  0x004       1          8    0,4,6
  0x00d       3          8    0,4,6
  0x025       3          8    0,4,6
  0x03c       3         10    0,3,5
  0x05a       3         10    0,3,5
  0x078       3         10    0,3,5
  0x096       3         10    0,3,5
  0x0b4       3         12    0,3,5
  0x0d8       3         12    0,3,5
  0x0fc       3         12    0,3,5
  0x120       3         12    0,3,5
  0x144       3         12    0,3,5
  0x168       3         12    0,3,5
  0x18c       3         12    0,3,5

For each repetition r and listed offset k append V[source+r*stride+k].
In particular 0x00d and 0x025 are literal offsets; do not replace them
with dense-group boundaries. A wire entry is the channel byte followed by
this projection. Neither the matrix nor the permission bitmap removes
entries from the emitted pages.

The 32-byte one-stream pad is computed once over all channel rows and
reused on every native page. For a channel of width W, compare column 0
and column 10 in each of groups vht20 through vht[20*2**W]. Interpret
both as signed bytes, including sentinels. Set bit ``ch & 7`` of byte
``ch >> 3`` only if every difference (column0 minus column10) is at least
10. Otherwise leave the bit zero. For v1 use the legacy width classes
(base 36 for channels below 149, base 149 otherwise; channels below 15
have W=0) and legacy channel limit 181; v2 uses the band-3 classes above
and limit 233. This bitmap is independent of the primary permission mask.

6 GHz extension header and records
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Immediately following the thirteen legacy tables is a 24-byte header::

  Offset  Size  Meaning
   0       4    ASCII 6GRP
   4       1    Power class: 1 (VLP)
   5       1    Profile EIRP ceiling: 14 dBm
   6       1    Extension record count P, 1..32; this producer emits 28
   7       1    Reserved, zero
   8       8    Nonzero primary permission bitmap; bit i means 1+4*i
  16       2    Alpha2, identical to outer header
  18       1    Rate-cell units contract: 1 (native half-dBm)
  19       5    Reserved, zero

Bits 59..63 must be zero. There is exactly one extension; unsupported
class, ceiling or units values are rejected. Follow the header with P
records, each consisting of a four-byte wrapper and its body::

  Offset  Size  Meaning
   0       1    Firmware CID: 0x5d or 0x95
   1       1    Reserved, zero
   2       2    Body length L, 44..1020, excluding wrapper
   4       L    Body

After P records the file must end exactly. Reject truncation, length
mismatches, unknown CIDs, invalid counts or reserved fields. The canonical
producer emits all fourteen CID 0x5d records first, then all fourteen CID
0x95 records. Each sequence visits all 110 channels in matrix order,
chunked into thirteen pages of eight entries and one of six. The final
page flag is 1 only on the last page of each sequence.

A native CID 0x5d body uses the same 44-byte header and 122-byte entries as
a v1 per-rate body, with version 3, tag 0x2a, band 3 and matching country.
Its body length is 44+122*C; its pad is the band-3 bitmap above. Its total
body lengths are thirteen times 1020 then 776.

A legacy CID 0x95 body has this independent layout::

  Offset  Size  Meaning
   0       4    Zero (no embedded version, tag or length)
   4       1    Count C, 1..8
   5       1    Band 3
   6       1    Last page flag, 0 or 1
   7       1    Reserved, zero
   8      36    Reserved, zero (no country or pad)
  44      33*C  Channel byte followed by 32 bytes of 0x7f per entry

Its body length is 44+33*C, thirteen times 308 then 242. The enclosing
package supplies country identity. Never substitute the native header for
this body or treat its 0x7f bytes as power grants.

The kernel requires exactly one native page whose first entry's channel
is 0x02 and exactly one whose first entry is 0x0f. Both have eight entries
and body length 1020; they are also reused for association. These selectors
are the first channel of each page, not separate wrapper fields and not
permission bits. The entire canonical extension is 18418 bytes; with two
v1 modes the complete file is 28537 bytes (28865 with three modes).

Deterministic publication and installer integration
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Validate all inputs and build all selected packages before writing output.
Use an absent or empty output root; refuse a nonempty one to prevent stale
packages being omitted from the inventory. Under that root write::

  policy-v2/CC.bin
  inventory.tsv
  inventory.sha256

``inventory.tsv`` consists of ASCII records sorted lexicographically by
relative path. Each record is exactly ``policy-v2/CC.bin``, one TAB, the
64 lowercase hexadecimal digits of that file's SHA-256, and LF. There is
no header, BOM, CR, leading slash or metadata record. Only generated
packages appear. An empty result has an empty inventory. Hash the exact
inventory bytes, including the final LF when nonempty. Write
``inventory.sha256`` as that digest, two spaces, ``inventory.tsv`` and LF.
The JSON stdout report gives per-country hashes, sizes, allowed-primary
counts and page counts, skips with reasons, aggregate counts and the
inventory digest. No timestamps or absolute paths enter generated files.
Fresh output directories use mode 0700 and files use 0600.

The authenticated AU reference is 28537 bytes with SHA-256::

  3425de282eb1a3c2c76d8cd65a31305e92f0e5b481d65096c0faa1ac631fea27

Install v2 separately from v1 and keep world fallback v1. cfg80211 flags,
NO_IR, VLP-client restrictions and runtime EIRP/PSD reductions remain
authoritative. The driver preserves 0xc4/0x7f sentinels when reducing
numeric native half-dBm cells. LPI, SP and AFC are outside this contract.

The active association path submits the two native selector pages after
queue binding and does not build a 60-byte TPE. Generic runtime D7
requests for band 2 (6 GHz) remain rejected by the raw planner. RF behavior,
rate accounting and calibration still require hardware qualification.

Local firmware inputs
---------------------

Firmware and calibration are not included. Install local inputs for the
target unit under ``/lib/firmware``; another unit's calibration or address
cannot be substituted. Missing or invalid inputs prevent startup.

Wi-Fi requests the following files under ``mediatek/mt7932/``:

* ``IZUBA_WIFI_MT7932_patch_mcu_1_2_hdr.bin`` and ``IZUBA_W7932_2.bin``:
  original firmware containers.
* ``ppr.bin``: the original power-on calibration request payload.
* ``wcal.bin`` and ``oca2.bin``: the unit's factory calibration; OCA2 uses
  its original BLOB container.
* ``config-original.bin``: the bounded J7CF package with the original 64 or
  65 configuration records, including all required keys.
* ``policy/world-XZ.bin`` and, when available, the matching country package
  under ``policy-v2/`` or ``policy/`` (see `Country packages`_): J7RP
  containers with original-derived modes and power tables. Relabeling a
  different country's payload is unsupported.

Bluetooth requests these files under ``mediatek/``:

* ``MT7932B1_OS_TypeB_2.0.177.0_260706180356.bin`` or the supported fallback
  ``MT7932B1_OS_TypeB_0.1.133.0_260128190103.bin``;
* ``j700-mt7932-btcal.bin`` (388 bytes);
* ``MT7932_PTB_IzubaA_0.1.0.0_20251021141303.ptx`` (198 bytes);
* ``j700-mt7932-bdaddr.bin`` (the unit's six-byte Bluetooth address).

Extraction and packaging require the original local assets. Keep these
assets and unit identities out of commits and public test reports.

Repeatable physical network test
--------------------------------

The manual test activates a saved profile and checks scan, WPA2, DHCP,
gateway replies, DNS, HTTPS, KDE and kernel diagnostics. It is not run
automatically. Run as root, with a fresh attempt name::

  python3 tools/testing/selftests/drivers/net/mt7932_e2e.py \
    --expected-release "$(uname -r)" --profile "YOUR SAVED PROFILE" \
    --connect --band a --attempt five-ghz

Use ``--band bg`` for 2.4 GHz. Require ``WIFI_PHYSICAL_NETWORK_E2E_PASS`` in
the JSON artifact printed on completion. The artifact records three traffic
samples and response hashes, omitting network names, addresses and credentials.

Receive diagnostics
-------------------

``ethtool -S <interface>`` reports lifetime host counters for malformed,
unsupported and integrity-rejected RX records, wrong peers, unprotected
data on protected connections, retry duplicates and allocation failures.
Take before/after differences for each traffic sample; reconnecting does
not reset these counters. Descriptor rejection counters include management
records and off-channel traffic. The other counters describe the admitted
peer's Ethernet receive path.

Retry and A-MSDU counters count received MSDU records, including split
A-MSDU subframes, before protection and duplicate filtering. They are not
over-air MPDU counts, BA-window sizes, TX retries or firmware ACK counters.
Duplicate suppression records sequences only after the protection check,
so a rejected unprotected frame cannot suppress a later decrypted retry.

Transport counters describe how the host services the device:
``irq_service_passes`` counts interrupt-thread passes and
``rx_ring0_records`` to ``rx_ring8_records`` count descriptors consumed per
receive ring. ``irq_unmasked_bits`` accumulates status bits that were
pending outside the enabled interrupt mask, and ``irq_unmasked_passes``
counts the passes that saw any. Work announced only by such a bit waits for
an unrelated interrupt, which shows up as latency rather than loss.
``tx_free_events`` counts TX completion reports. ``bss_absent_events`` and
``bss_absent_ms`` count how often and for how long the firmware reported the
BSS absent, during which TX is held. ``home_beacons`` counts beacons from
the associated BSS that reached the host, and ``rx_2ghz_records``,
``rx_5ghz_records`` and ``rx_6ghz_records`` split admitted Ethernet RX by
band. Like the RX counters, they are lifetime values.

Use the current RX bitrate from ``iw dev <interface> station dump`` while
traffic is running. The driver expires that sample after one second; an
idle or unsupported RX vector can leave it absent. TX bitrate is not
reported because the firmware's TX-vector query cannot attest peer
identity or freshness. Missing TX bitrate is not a zero-rate measurement.

Dynamic debug messages ``ASSOC_HT_CAPABILITIES``,
``ASSOC_HE_CAPABILITIES`` and ``ASSOC_START`` describe the firmware-generated
station capabilities and channel event. ``POWER_SAVE_SUBMITTED`` records
the requested mode and submission status, without asserting firmware
readback. These messages contain neither credentials nor packet payloads.

Limitations
-----------

* PCI bootstrap memory remains retained until external reset. Controller
  removal and memory reuse are unqualified. Runtime guards refuse reset and
  kexec; suspend-to-idle retains the successfully admitted arena.
* Bluetooth system sleep uses a bounded firmware quiesce/restore handshake
  around HCI suspend/resume, preserving its DMA arena. Hibernation is rejected.
  Removal first quiesces firmware, then stops work and interrupts, disables
  bus mastering and drains pending
  PCI transactions before freeing DMA. An unconfirmed drain retains storage
  until reset. A controller that stops answering commands, or reports a
  hardware error, is reset over HCI and set up again; if that setup fails,
  the transport faults. Firmware reloading and warm reprobe are
  unsupported; use a fresh cold boot after removing or faulting an active
  transport.
* The Bluetooth software queue limit does not provide HCI backpressure;
  saturation can drop an accounted frame.
* Arbitrary scan IEs, AP/P2P, country-specific RF behavior, roaming and
  long-duration reliability are unqualified. SAE and required MFP qualification
  are described under Station security.
* SCO/headset microphone, LE Audio/ISO and simultaneous headset audio are
  unqualified.

Station power cache and coexistence
----------------------------------

Each join submits CID05 awake before EA/40. That pre-join submission is
not the power policy of the subsequent associated firmware session. The
host invalidates its applied-mode cache after pre-join initialization so
that the existing post-association worker submits the desired policy,
including awake when station power save is disabled. Successful repeated
requests within that association still coalesce. Sleep remains gated by
TIM discovery and the normal RF, peer and termination checks. A successful
submission is not firmware readback or a measured coexistence improvement.

The association builder retains its twenty-beacon listen interval on all
bands. The earlier proposal to reduce it to one on every 2.4 GHz association
is omitted: there is no BT activity notification connecting these drivers,
and a band-only policy would also affect BT-off associations. Twenty to one
permits twenty times as many listen opportunities, or nineteen additional
opportunities per twenty beacons. At a 100 TU beacon interval (102.4 ms),
the advertised span would fall from 2048 ms to 102.4 ms. These values do not
quantify radio wake duty cycle, milliwatts or battery life; those depend on
firmware, TIM/DTIM and actual traffic. No new module parameter, guessed PTA
command or Bluetooth link-policy override is introduced.

Qualify with paired control/fix boots, BT off/on on 2.4 GHz, local wired
iperf3 in each direction and ten-minute gateway pings. Interleave off/on/off
and on/off/on blocks to expose drift in a busy channel. Compare each
module's BT-on penalty against its adjacent BT-off cells, retaining loss
and tail latency separately. Include a 5 GHz control, functioning BT HID,
and a twenty-second rtc1 suspend/resume followed by a fresh association.
Keep failed cells and independent rollback evidence; host tests alone do
not establish latency, throughput or Bluetooth functionality.
