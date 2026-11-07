# Public Mac firmware board coverage

Catalog snapshot: 2026-11-07. The missing public restore build is **27.2 / 26B7124**.
It supplies the two T8152/H18 boards absent from the previous restore-DeviceTree corpus:

| Board | Model | Product | SoC | Restore builds checked | Gap audit |
| --- | --- | --- | --- | --- | --- |
| J804 | Mac18,1 | MacBook Pro (14-inch, M6) | T8152 | 26B7124, 26C5035r | [J804](DT-GAPS-j804.md) |
| J833ct | Mac18,2 | iMac (24-inch, M6) | T8152 | 26B7124, 26C5035r | [J833ct](DT-GAPS-j833ct.md) |

J873g (Mac18,5, Mac mini M6) was already present. The four PROLED boards
K114c/K114s/K116c/K116s were also already covered by the device-specific 26B8125
extracts. They are T6050 boards, not an additional M6 die.

## Cross-checks

The complete [ipsw.me device inventory](https://api.ipsw.me/v4/devices),
[Apple restore catalog](https://mesu.apple.com/assets/macos/com_apple_macOSIPSW/com_apple_macOSIPSW.xml)
and [Apple software lookup service](https://gdmf.apple.com/v2/pmv) agree on the two
new model/board identities. Apple's restore catalog advertises three current
27.2 restore URLs: 26B123, 26B7124 and 26B8125.

Apple Pallas reports 26B7124 for the two new models. Its developer, AppleSeed and
public beta audiences report 27.3 beta 1 / 26C5035r. The beta's supported product
list exactly matches the 66 Mac/virtual-Mac identifiers in the complete public
inventory. The checked beta has no further board names, Mac19 identifiers or
additional newer die. This is a statement about the queried public catalogs;
it does not establish what unreleased hardware exists.

Remote ZIP directory checks give:

| Build | DeviceTree members | Relationship |
| --- | ---: | --- |
| 26B123 | 60 | Original universal corpus, including the virtual Mac |
| 26B8125 | 4 | PROLED device fork |
| 26B7124 | 2 | J804 and J833ct device fork |
| 26C5035r | 66 | Exact union of the three preceding sets |

The historical 11.0.1 extracts retain three additional names absent from modern
restores: J273, J273a and T485. Consequently the previous corpus had 67 unique
names, and the expanded historical union has 69. The modern 66 includes the
virtual Mac; it is not a claim of 66 physical production Macs.

SoC classification uses the public board platform metadata together with each
board's identity. An `arm-io` compatible can name a shared implementation and
must not alone be treated as an exact SoC identification.

## Extraction and remaining implementation

Only DeviceTree ZIP members were extracted, using `ipsw extract --remote --dtree`,
and parsed with `ipsw dtree --json`. The selected members from 26B7124 and
26C5035r were checked against the remote ZIP's uncompressed sizes and CRC32.
No complete IPSW or OTA was downloaded to the host. Firmware and tuning extracts
are excluded from this repository.

Example for the missing device-specific build:

```sh
ipsw extract --remote --dtree --output selected-dtrees \
  https://updates.cdn-apple.com/2026FallFCS/04f50f4e-1485-4efb-bd81-f3d743fdff4a/UniversalMac_27.2_26B7124_Restore.ipsw
```

When querying these models with ipsw 3.1.722, pass both the model ID and board:

```sh
ipsw download ota --platform macos --device Mac18,1 --model J804AP --json
ipsw download ota --platform macos --device Mac18,1 --model J804AP --beta --json
```

Supplying only one identifier fails in that version's embedded model lookup;
the explicit pair reaches Apple's catalog. The older mesu macOS OTA URL returned
HTTP 403, so Pallas provided the OTA evidence. No OTA payload was needed.

The two board gap audits enumerate every top-level subtree and every immediate
peripheral subtree. They explicitly retain missing coverage: the audited base
has neither board DTS nor a common T8152 include. Shared SoC resources and
bindings must be supplied before board support can be validated. This inventory
is not a claim that those boards boot or that disabled peripheral descriptions
are complete.
