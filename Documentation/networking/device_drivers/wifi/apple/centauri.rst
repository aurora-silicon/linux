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
  persistent transport faults, including silent Alpha stage halts
  detected by ``link_worker`` or ``scan_worker``. A restart first copies
  the crash region the firmware reserves in its secondary memory, which
  it then frees, to debugfs ``firmware_crash_saved``, and the Alpha DRAM
  log, when ``fwlog_start`` set one up, to ``firmware_log_saved``. The
  Alpha execution stage that stopped the firmware is logged with it.
* **Peer-to-Peer AWDL (``awdl0``):** On T8142/T8152 hosts,
  ``centauri_awdl.c`` registers a virtual Ethernet interface (``awdl0``,
  module parameter ``awdl_enable``, default 1) on firmware interface ID 1.
  All values below are as traced on the host side of a working AWDL link
  or as captured over the air from nearby peers.

  Bringing ``awdl0`` up opens the shared data rings (RX 12, 15, 18, 78 and
  79 next to TX 76), queries the firmware defaults (command 8), sets the
  MAC address (command 1) and starts the interface (command 2) with these
  TLVs: an empty general configuration (``0xfe08``), the election
  parameters (``0xff05``, 19 bytes: flags, election ID, distance from the
  top master, a reserved byte, top master address and metric, own
  metric; own metric 65 at start), ``0xfe00`` (43 bytes: byte 8 enables
  synchronization, byte 9 is the sync role, 2, and bytes 10..15 the
  master to synchronize to), the MIF/PSF sync parameters (``0xfe01``,
  PSF periods 110 and 0) and the sync parameters (``0xff04``, the
  firmware's 33-byte header followed by the channel sequence in a
  (flags, channel) pair per slot). It then sends the channel sequence
  (``0xff12``, 38 bytes, 16 slots of four-slot dwell blocks), ``0xfe0a``
  and the frame templates, and opens the multicast TX ring 26
  (``tid=6``).

  The channel sequence follows the station: on a 2.4 GHz channel that
  channel only; on the lower channel of a 5 GHz 40 MHz pair, channel 6
  and that channel; otherwise channels 6 and 149. 5 GHz slots use the
  80 MHz operating class 128. Writing ``awdl_chanseq`` in debugfs
  replaces it until the module is reloaded.

  The firmware writes the sync, channel-sequence and election TLVs of
  each action frame itself; the host appends the rest through
  ``0xfe02`` (sync frames) and ``0xfe03`` (master-indication frames), in
  the order captured from peers. Sync frames carry service parameters,
  the election tree, the 47-byte data-path state (country code and
  social-channel bitmask), HT capabilities, the firmware's VHT
  capabilities element (command 8 TLV ``0x00bf``) and the version
  (``0xa2``); master-indication frames carry the election tree,
  data-path state, HT and VHT capabilities, the hostname, the version,
  service parameters and any service-response TLVs written to debugfs
  ``awdl_service_tlvs``. Every 3.15 s the driver sends the election,
  ``0xfe00`` and sync parameters again, then ``0xfe01`` with the sync
  parameters, the templates and ``0xfe0a``.

  Received PSF/MIF action frames (management ring 4, behind a radiotap
  header) give the sender's election state and sync parameters. When a
  frame's top master beats ours and its sync parameters carry AW
  extension length 16, a presence mode and a receive time, the driver
  follows the sender: it first sends ``0xfe00`` with the sender as the
  master to synchronize to, byte 16 set and that frame's timing
  (receive time at bytes 17..20, TX delay at 21..24, down counter, AW
  period and AW length at 25..30, presence mode at 31, AW sequence at
  32..33), then the new election state and templates. The receive time
  is the radiotap timestamp field, else TSFT; the TX delay is the action
  frame header's actual minus planned transmit time. Our own address is
  never followed, and after 5 s without a frame from the parent the
  driver is top master again.

  Every peer heard in an action frame or on received data, reported by
  the peer state event (``0x2c``) or addressed by unicast TX is added to
  the firmware peer cache with the host channel sequence; a peer is not
  given to the firmware again within 5 s. Unicast TX (or an ``add``
  written to ``awdl_peers``) opens the one per-peer TX ring (27, sharing
  completion ring 76); opening a second unicast ring faulted the
  firmware about 1.5 s later on the J815. The ring is handed to another
  peer only after 5 s unused. Stopping ``awdl0`` or recycling the peer
  ring sends the 4-byte transfer-ring close message (opcode ``0x0003``)
  before ``CEN_AWDL_CMD_STOP`` or reopening the ring.

  When the firmware reports ``fw_llc_encap == 0`` the driver adds the
  16-byte LLC/SNAP + AWDL data header on TX and removes it on RX (also
  accepting 8-byte RFC1042 frames). Received data is demultiplexed by the
  3-bit interface ID in completion footer bits 10:8. Debugfs
  ``awdl_status``, ``awdl_peers``, ``awdl_chanseq``, ``awdl_arpa`` and
  ``awdl_service_tlvs`` show and update the state. ``awdl_status`` counts
  adoptions, reverts, refreshes and rejected frames.

  Userspace keeps IPv6 duplicate address detection off on ``awdl0``
  (``accept_dad=0``) and leaves the interface unmanaged by network
  managers; the driver does not change either.

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
   python3 tools/asahi/centauri-awdl-tests.py --negative-controls
   python3 tools/asahi/centauri-wifi-pm-tests.py --negative-controls
   python3 tools/asahi/centauri-wifi-avoid-tests.py
   python3 tools/asahi/centauri-bt-arena-tests.py --negative-controls
   python3 tools/asahi/centauri-recovery-tests.py --negative-controls
   python3 tools/asahi/centauri-physical-recovery-tests.py

In-kernel KUnit tests are available via ``CONFIG_APPLE_CENTAURI_KUNIT_TEST``
and ``CONFIG_APPLE_CENTAURI_T6050_RECOVERY_KUNIT``.
