.. SPDX-License-Identifier: GPL-2.0-only

===================================
Apple N1 Centauri Wi-Fi & Bluetooth
===================================

The Apple N1 wireless SoC exposes a three-function PCIe endpoint after
firmware boot:

* **Function 0 (Control):** Firmware bootstrap, secondary FTAB handoff,
  CCHI control channel, and cross-function recovery/sleep coordination.
* **Function 1 (Alpha):** IEEE 802.11 Wi-Fi MAC/PHY transport over ACIPC
  command, event, beacon, and data transfer/completion rings.
* **Function 2 (Beta):** Bluetooth HCI/ACL/LE transport over ACIPC rings.

Driver Topology
===============

Two driver families share the same PCI vendor/device IDs and separate by
PCIe host controller compatibility:

* **T8142 / T8152 hosts** (e.g. ``apple,j813``, ``apple,j815``,
  ``apple,j873g``, ``apple,j804``):

  - Wi-Fi & Control: ``drivers/net/wireless/apple/centauri/``
    (``CONFIG_APPLE_CENTAURI``, module ``apple_centauri``)
  - Bluetooth: ``drivers/bluetooth/btcentauri.c``
    (``CONFIG_BT_CENTAURI``, module ``btcentauri``)

* **T6050 hosts** (``apple,t6050-pcie``):

  - Wi-Fi & Control: ``drivers/net/wireless/apple/centauri/t6050/``
    (``CONFIG_APPLE_CENTAURI_T6050``, module ``apple_centauri_t6050``)
  - Bluetooth: ``drivers/bluetooth/btcentauri_t6050.c``
    (``CONFIG_BT_HCICENTAURI``, module ``btcentauri_t6050``)

Wi-Fi Capabilities
==================

Both Wi-Fi backends implement ``cfg80211`` station-mode interfaces with:

* **Bands and Capabilities:** 2.4 GHz, 5 GHz, and 6 GHz. Firmware system
  command 8 provides HT (TLV 262), VHT (TLV 191), and HE (TLV 255)
  capabilities which populate ``ht_cap`` (2.4/5 GHz), ``vht_cap`` (5 GHz),
  and per-band HE ``iftype_data`` (2.4/5/6 GHz, with 6 GHz capability
  words derived from VHT). Active channel lists and passive/radar/width
  constraints are populated from firmware command 19 and re-applied on
  regulatory updates. Split 6 GHz scanning (``WIPHY_FLAG_SPLIT_SCAN_6GHZ``)
  and multi-BSSID reporting (``support_mbssid``) are enabled.
* **Scanning:** Up to 59 channels per firmware scan command (command
  ``0x1000f``) and up to 4 SSIDs per scan (TLV 258). Scan aborts
  (command ``0x10001``) pass the active 1-byte scan handle (``1..31``)
  and treat firmware status ``342`` (handle already retired) as benign.
* **Security & Offload:** Open networks (2.4/5 GHz), WPA2-PSK,
  WPA2-PSK-SHA256, and WPA3-SAE (``NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK``
  and ``NL80211_EXT_FEATURE_SAE_OFFLOAD``). Supported ciphers include
  CCMP-128, GCMP-128, GCMP-256, BIP-CMAC-128, and mixed-mode WPA2 TKIP
  group cipher (disallowed with SAE and on 6 GHz). Autonomous firmware
  roam scans are suspended after association (command ``0x20016``) unless
  ``roam_scan=1`` is set. Firmware disconnect events (``0x80000003``)
  report the decoded IEEE 802.11 reason code and local-initiation flag to
  ``cfg80211_disconnected()``.
* **Link Telemetry, Survey, Regulatory & MAC Address:** Startup command 0
  populates ``wiphy->fw_version`` (exposed via ``ethtool -i`` and
  ``nl80211``). ``.get_channel``, ``.get_station``, ``.dump_station``, and
  ``.get_tx_power`` query firmware interface status (command 20, TLV 268)
  for live RSSI and active channel width (20/40/80/160 MHz), chip counters
  (command ``0xb0001`` bit 1, TLV ``0xf901``) for per-association TX
  retries, over-the-air TX failures, live RX bitrate, and fallback TX
  bitrate, frame counters (command ``0xb0001`` bit 19, TLV ``0xf915``) for
  per-association beacon RX and beacon loss counts, TX power/rate history
  (command ``0xb0001`` bit 29, TLV ``0xf920``, supporting both 280-byte
  and 320-byte record layouts) for live Legacy/HT/VHT/HE/EHT TX bitrate
  and combined TX power in dBm, PHY dump records (command ``0xb0001`` bit
  32, TLV ``0xf923``) for per-chain RSSI and fallback noise floor
  (selecting radio core 0 for 2.4 GHz and core 1 with core-0 fallback for
  5/6 GHz), and per-association packet/byte/failure/drop counters.
  ``.dump_survey`` queries firmware station statistics (command ``0xb0001``
  bit 7, TLV ``0xf908``, falling back to PHY dump TLV ``0xf923``) for
  channel noise floor in dBm and CCA statistics (command ``0xb0001`` bit 3,
  TLV ``0xf903``, selecting radio slice 1 for 2.4 GHz and slice 2 with
  slice-1 fallback for 5/6 GHz) for channel active, busy, and transmit
  time in milliseconds. On bring-up, the firmware's initial country code
  (command 10) is passed to ``regulatory_hint()`` when valid. Runtime MAC
  address changes are supported via ``.ndo_set_mac_address`` while idle.
* **Data Path, NAPI GRO & Ring Resilience:** RX (ring 12 / completion 207)
  is drained via NAPI (``napi_gro_receive()``) and decodes both standard
  and extended-prefix RX frames, returning buffer cookies to the refill
  ring on error completions. TX (ring 25 / completion 192) enforces a
  software queue and in-flight cap with a bypass watchdog, sets TCP pacing
  shift (``sk_pacing_shift_update()``), frees completed packets via
  ``dev_consume_skb_any()``, decodes both single and coalesced TX
  completions, and exports throughput/backpressure counters via debugfs
  ``data_stats``. Control backends support warm-reset recovery on
  persistent transport faults.

Bluetooth Capabilities
======================

Both Bluetooth transports expose an ``HCI_PCI`` device with:

* **Transport Rings:** HCI command/event rings (1 and 7), LE ACL TX/RX
  rings (4 and 10), BR/EDR ACL TX/RX auxiliary rings (2 and 8), SCO
  TX/RX rings (3 and 9, raw HCI SCO packets), and LE Audio ISO TX/RX
  rings (5 and 11, 1-byte H4 packet indicator prefix). Both drivers use
  2 MSI vectors for interrupt-driven I/O with pending-work continuation;
  ``btcentauri`` falls back to 1 ms polling automatically if MSI
  allocation is unavailable.
* **Controller Provisioning & Quirks:** Factory BD_ADDR provisioning
  from device-tree ``local-bd-address`` (``0xfc06``, ``0xff0b``,
  ``0xfe44``), runtime public address updates via ``hdev->set_bdaddr``
  (``0xfc06``), vendor duplicate-filter flush (``0xff3f`` via
  ``hdev->reset_dup_filter``, automatically falling back to host
  duplicate filtering if firmware rejects the command), LE extended
  advertising PHY and legacy scan-response normalization, wideband speech
  HFP support (``HCI_QUIRK_WIDEBAND_SPEECH_SUPPORTED`` over SCO rings 3
  and 9), host-side LE accept-list scan filtering (reporting accept-list
  size 0 unless ``accept_list=1`` is set), and BR/EDR extended Create
  Connection translation (``0xff7b`` when advertised by ``0xffa3`` bit 9).
* **Resilient Data RX, Timeout Diagnostics & Power Management:**
  Frame-level malformed ACL, SCO, and ISO packets are dropped with
  rate-limited diagnostics without halting the HCI transport, and unknown
  trailing RX metadata attributes are ignored without dropping valid HCI
  payloads. ``hdev->reset`` logs ring/transport state and reschedules I/O
  on HCI command timeouts. Across system suspend, Beta enters ACIPC host
  sleep via BAR0 ``0x1078`` and restores wake state on resume.

Firmware
========

Firmware images are external board-specific FTAB binaries placed under
``/lib/firmware/apple/centauri/`` (e.g. ``j813-ftab.bin``,
``j815-ftab.bin``, ``j873-ftab.bin``, ``j804-ftab.bin`` on T8142/T8152,
or the firmware path named by device tree on T6050).

Testing
=======

Host-executable unit tests (requiring only Python 3 and a C compiler)
live under ``tools/asahi/``:

.. code-block:: sh

   python3 tools/asahi/centauri-scan-tests.py --negative-controls
   python3 tools/asahi/centauri-wifi-pm-tests.py --negative-controls
   python3 tools/asahi/centauri-wifi-avoid-tests.py
   python3 tools/asahi/centauri-bt-arena-tests.py --negative-controls
   python3 tools/asahi/centauri-recovery-tests.py --negative-controls
   python3 tools/asahi/centauri-physical-recovery-tests.py

In-kernel KUnit tests are available via ``CONFIG_APPLE_CENTAURI_KUNIT_TEST``
and ``CONFIG_APPLE_CENTAURI_T6050_RECOVERY_KUNIT``.
