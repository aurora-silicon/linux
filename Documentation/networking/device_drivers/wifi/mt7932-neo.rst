.. SPDX-License-Identifier: GPL-2.0-only

MT7932 radio bring-up on the MacBook Neo
======================================

J700 MT7932 Wi-Fi uses cfg80211 and NetworkManager with firmware-managed
WPA2-CCMP authentication. Bluetooth uses an opt-in PCIe transport.

The Wi-Fi import is pinned to
``aurora-silicon/linux-neo-cleanroom-eryk-with-wifi``, commit
``610cb463c03f9bc68a5d020e7ca443af1fda4856``, with configuration, scan IE and
WPA2 AKM compatibility fixes. Existing source notices are retained.
Co-authored by DJ (DjDeveloperr), Ace (Acelogic), and Ryan Murray.

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

Use a saved NetworkManager WPA2 profile matching the current interface.
KDE audio requires BlueZ, PipeWire, its PulseAudio compatibility service,
WirePlumber and Bluetooth audio plugins in the logged-in user's session.

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
  under ``policy/``: J7RP containers with original-derived modes and power
  tables. Relabeling a different country's payload is unsupported.

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

Limitations
-----------

* PCI bootstrap memory remains retained until external reset. Controller
  removal, memory reuse, suspend and kexec are unqualified.
* Bluetooth PCI removal/quiescence is incomplete. Its software queue limit
  does not provide HCI backpressure; saturation can drop an accounted frame.
  Both require correction before production use.
* Arbitrary scan IEs, WPA3/SAE, required MFP, AP/P2P, general country-package
  generation, roaming and long-duration reliability are unqualified.
* SCO/headset microphone, LE Audio/ISO and simultaneous headset audio are
  unqualified.
