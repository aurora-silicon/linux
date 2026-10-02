.. SPDX-License-Identifier: GPL-2.0-only

MT7932 radio bring-up on the MacBook Neo
======================================

This contribution combines the existing J700 MT7932 fullmac Wi-Fi driver with an
opt-in PCIe Bluetooth transport on the public Aurora platform. Wi-Fi uses
cfg80211 and NetworkManager with firmware-managed WPA2-CCMP authentication.
It is not an mt76/mac80211 softmac implementation.

The Wi-Fi import is pinned to
``aurora-silicon/linux-neo-cleanroom-eryk-with-wifi``, branch
``wifi/width-qualification-20260912``, commit
``610cb463c03f9bc68a5d020e7ca443af1fda4856``. The import commit preserves all
26 driver files byte-for-byte. A separate commit carries the three tested
compatibility changes: bounded 64/65-record configuration parsing, native scan
IE encoding, and selected WPA2 PSK admission with trailing AKM alternatives.
This contribution imports an existing implementation; it does not claim an
independent rewrite. Existing source notices are retained. Co-authored by
DJ (DjDeveloperr), Ace (Acelogic), and Ryan Murray.

Platform and configuration
--------------------------

Use the separately reviewed T8140 PCIe bootstrap described in
``Documentation/PCI/controller/apple-t8140.rst``. Its explicit kernel opt-in is
``pcie_apple_piodma_diag.enumerate=1``. Public m1n1 must populate the J700
``wifi0`` endpoint's own ``local-mac-address``; a zero placeholder is rejected.

The tested configuration includes ``CONFIG_MT7932_FULLMAC=m``,
``CONFIG_CFG80211=y``, ``CONFIG_BT_MTK7932_PCIE=y``, ``CONFIG_BT_BREDR=y``,
``CONFIG_BT_LE=y``, ``CONFIG_CRYPTO_AES=y`` and ``CONFIG_CRYPTO_CMAC=y``.
The bootstrap and Bluetooth experiment exclude suspend and kexec.
Use the ordinary cfg80211 regulatory database and applicable country policy.
The validated first-admission fallback is kernel country 00 with firmware XZ.

After the root filesystem and the local firmware packages are available,
select PCIe ASPM performance policy before loading ``mt7932-fullmac``. This
matches the tested admission sequence. Bluetooth's gate defaults closed;
validate the cold, unbound ``14c3:793b`` function, enable
``/sys/module/mt7932_bt_pcie/parameters/enable``, then request its PCI probe.
Do not reprobe after a failed or uncertain Bluetooth admission. The tested
Wi-Fi driver owns function 0 and Bluetooth owns function 1.

NetworkManager supplies the existing saved WPA2 profile. Use a unique profile
name or UUID and the current interface name; an older ``neo-wifi0`` profile
will not match an interface named ``wlan0``. KDE audio requires BlueZ,
PipeWire, its PulseAudio compatibility service, WirePlumber and the Bluetooth
audio plugins in the logged-in user's session.

Local firmware inputs
---------------------

No firmware, calibration, unit address, credentials or captured command
payloads are distributed in this contribution. Install lawfully obtained
local inputs for the target unit under ``/lib/firmware``. Do not substitute
another unit's calibration or address. A missing or invalid input must remain
a startup failure rather than an excuse to invent calibration or power data.

Wi-Fi requests the following files under ``mediatek/mt7932/``:

* ``IZUBA_WIFI_MT7932_patch_mcu_1_2_hdr.bin`` and ``IZUBA_W7932_2.bin``:
  the original firmware containers accepted by the driver's parser.
* ``ppr.bin``: the original power-on calibration request payload.
* ``wcal.bin`` and ``oca2.bin``: the unit's factory calibration; OCA2 retains
  its original BLOB container, rather than an older replay-stream wrapper.
* ``config-original.bin``: the bounded J7CF package with the original 64 or
  65 configuration records. Required keys and record shapes are validated.
* ``policy/world-XZ.bin`` and, when available, the matching country package
  under ``policy/``: J7RP containers with original-derived modes and power
  tables. The parser checks the country and table shapes. Relabeling a
  different country's payload is not a supported conversion.

Bluetooth requests these files under ``mediatek/``:

* ``MT7932B1_OS_TypeB_2.0.177.0_260706180356.bin`` or the supported fallback
  ``MT7932B1_OS_TypeB_0.1.133.0_260128190103.bin``;
* ``j700-mt7932-btcal.bin`` (388 bytes);
* ``MT7932_PTB_IzubaA_0.1.0.0_20251021141303.ptx`` (198 bytes);
* ``j700-mt7932-bdaddr.bin`` (the unit's six-byte Bluetooth address).

Extraction and packaging require the original local assets. The Linux loader
does not grant redistribution rights to them. Do not include those assets or
their unit-specific digests in bug reports, commits or test artifacts.

Repeatable physical network test
--------------------------------

The manual test is installed as a selftest data file rather than an automatic
test: activating a real saved network requires an explicit profile selection.
It selects the profile's requested band and checks an actual scan result,
WPA2 activation, DHCP route, gateway replies, DNS, verified HTTPS response,
KDE processes and fatal kernel/driver diagnostics. It never asks for or prints
the saved password. Run as root, with a fresh attempt name::

  python3 tools/testing/selftests/drivers/net/mt7932_e2e.py \
    --expected-release "$(uname -r)" --profile "YOUR SAVED PROFILE" \
    --connect --band a --attempt five-ghz

Use ``--band bg`` for 2.4 GHz. The JSON artifact path is printed on completion;
each of three traffic samples includes the downloaded response's size and
SHA256. Connection names, SSIDs, unit addresses and credentials are omitted
from the result. A successful host serial command is not a test pass: require
``WIFI_PHYSICAL_NETWORK_E2E_PASS`` in the target artifact.

Qualification and remaining work
--------------------------------

The :download:`qualification artifact <mt7932-neo-qualification.json>` records the exact Image,
module and source identities and the physical results from 2026-10-02.
Both 2.4 and 5 GHz passed scan, WPA2, DHCP, gateway, DNS and verified HTTPS.
Switching bands performed normal peer retirement and fresh authentication.
Wi-Fi's checked module removal/reset/reload also passed before Bluetooth
admission. Five-GHz traffic concurrent with Bluetooth discovery and HCI
off/on passed 54/54 gateway replies and 18 HTTPS downloads with no driver or
kernel faults. The longer 2.4-GHz coexistence run recorded 53/54 replies;
it is explicitly not a zero-loss pass. The lost ping coincided with a
background scan; that timing does not establish a root cause.

The earlier Bluetooth candidate passed two bonded AirPods reconnects and
30 seconds of AAC output, including a user-confirmed audible tone. Concurrent
headset audio was not repeated on the combined candidate because no AirPods
appeared during its discovery runs.

The experimental implementation has concrete production merge blockers:

* PCI bootstrap memory remains retained until external reset; controller
  removal, suspend, kexec and reusable-arena lifetime are not qualified.
* Bluetooth lacks a complete PCI removal/quiescence contract. Suppressed
  bind/unbind attributes do not make PCI device removal safe.
* Bluetooth's finite software queue limit is not HCI backpressure. Saturation
  can drop a frame already accounted by the HCI scheduler; that path needs
  correction and saturation qualification before production use.
* Arbitrary caller scan IEs, WPA3/SAE, required MFP, AP/P2P modes, general
  country-package generation, roaming and long-duration reliability remain
  outside this qualification. Broad compatibility must not be inferred from
  the tested station and firmware combination.
* SCO/headset microphone, LE Audio/ISO and simultaneous headset audio remain
  unqualified. These limitations are separate from the demonstrated A2DP path.
