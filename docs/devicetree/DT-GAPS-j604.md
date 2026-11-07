# DT gaps: J604

## Identity and scope

Board DTS: `arch/arm64/boot/dts/apple/t8132-j604.dts`.
The hardware resource inventory identifies the T8132 peripheral die.

Source: preferred live hardware description, 27.0/t8132-j604.txt, cross-checked against the per-board 27.2 template. Only hardware identities and resource mappings are recorded here; no tuning or factory data is reproduced.

## Result

This resource coverage audit still has unresolved entries and does not establish complete board support. 384 arm-io records were examined. Hardware blocks added in this series remain disabled. A resource match does not prove IRQ, clock, power or DMA stream correctness.

**Audit basis:** DTS source analysis; compiled-DTB reconciliation pending.

| Record classification | Count |
| --- | ---: |
| bus device | 1 |
| gap | 51 |
| partial resources | 31 |
| resources | 65 |
| service review | 117 |
| stream | 119 |

## Remaining work

- Close every explicit GAP and service-review entry below, including nested endpoints and shared resource ownership.
- Some template SPMI records are alternative endpoints at the same address. Runtime model selection is needed before representing these as simultaneous devices.
- Reconcile larger firmware resource windows with the driver-specific Linux register layout; do not expand an active device aperture without driver review.
- Bindings and resource presence do not supply per-device calibration, firmware or an initialization contract.
- Validate this board with `W=1` and `dtbs_check`, then perform a separately approved boot before enabling newly described engines.

## Dependencies

- linux PR #1035: existing cooled-board CPU DVFS enablement, kept separate; this change does not duplicate it.
- linux PR #1042 owns Stockholm endpoint bring-up; this series describes the disabled bus without enabling NFC.
- The integration base is 43c790fe.

## Every arm-io record

Paths in the first column are relative to `arm-io`. Stream entries name the owning translation block; their SID and aperture semantics still need review. Service-review entries have no independently decoded MMIO resource; this is not a waiver of their hardware or binding requirements.

| Firmware hardware/service record | Classification | Linux description or gap |
| --- | --- | --- |
| `spi2` | resources | `/soc/spi@3ad108000` |
| `spi2/mesa` | bus device | `/soc/spi@3ad108000/fingerprint@0` |
| `spi4` | resources | `/soc/spi@3ad110000` |
| `spi4/dp855` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-pmicwp` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-pmic` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-sswp` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/tcon-registers` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-eeprom` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c0` | resources | `/soc/i2c@3ad010000` |
| `i2c0/pcon0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c1` | resources | `/soc/i2c@3ad014000` |
| `i2c1/audio-codec-output` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c3` | resources | `/soc/i2c@3ad01c000` |
| `i2c3/audio-speaker` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c3/audio-speaker-left-woofer-2` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c3/audio-speaker-left-tweeter` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c3/audio-speaker-right-woofer-1` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c3/audio-speaker-right-woofer-2` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c3/audio-speaker-right-tweeter` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c4` | resources | `/soc/i2c@3ad020000` |
| `i2c4/avellino-pre` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c4/avellino-post` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c4/sd-card` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `fpwm1` | resources | `/soc/pwm@3ad044000` |
| `fpwm1/kbd-backlight` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apcie2` | gap | **GAP:** no matching hardware resource node |
| `dart-apcie2/mapper-apcie2` | stream | Stream of `dart-apcie2`; stream/resource linkage needs review |
| `dart-apcie2/mapper-apcie2-piodma` | stream | Stream of `dart-apcie2`; stream/resource linkage needs review |
| `apcie2-piodma` | gap | **GAP:** no matching hardware resource node |
| `nub-spmi-a1` | resources | `/soc/spmi@388a08000` |
| `nub-spmi-a1/hpm3` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi0` | resources | `/soc/spmi@390914000` |
| `aop-spmi0/spmi-wifibt` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi0/stockholm-spmi` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi0/stockholm-spmi/stockholm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1` | resources | `/soc/spmi@390b14000` |
| `iop-voicetrigger-controller` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `iop-voicetrigger-controller/audio-vt-device` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dynamic-pil` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dynamic-pil/cil` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `displaymanager` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `wlan` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `bluetooth` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `apcie` | partial resources | `/soc/pcie@1cb0000000`; `/soc/iommu@490000000`; **GAP:** 26 larger or unmatched resource window(s) |
| `apcie/pci-bridge0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie/pci-bridge0/wlan` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie/pci-bridge0/bluetooth-pcie` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie/pci-bridge2` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie/pci-bridge2/pcie-sdreader` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apcie0` | resources | `/soc/iommu@490000000` |
| `dart-apcie0/mapper-apcie0-wlan` | stream | Stream of `/soc/iommu@490000000`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-bt` | stream | Stream of `/soc/iommu@490000000`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-piodma` | stream | Stream of `/soc/iommu@490000000`; stream/resource linkage needs review |
| `apcie0-piodma` | resources | `/soc/dma-controller@490030000` |
| `aod2` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `acio-cpu0` | resources | `/soc/mailbox@401108000`; `/soc/cio@401ac0000` |
| `acio-cpu0/iop-acio0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec0` | resources | `/usb4-pcie-tunnel-0/pcie@1c50000000` |
| `apciec0/pcic0-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec0` | resources | `/usb4-pcie-tunnel-0/iommu@404000000` |
| `dart-apciec0/mapper-apciec0-piodma` | stream | Stream of `/usb4-pcie-tunnel-0/iommu@404000000`; stream/resource linkage needs review |
| `apciec0-piodma` | resources | `/soc/dma-controller@404068000` |
| `acio0` | resources | `/soc/cio@401ac0000/nhi@f00000`; `/soc/cio@401ac0000` |
| `dart-acio0` | resources | `/soc/cio@401ac0000/iommu@a80000` |
| `dart-acio0/mapper-acio0-tx-0` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-1` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-2` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-3` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-4` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-5` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-6` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-7` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-8` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-9` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-10` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-11` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-0` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-1` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-2` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-3` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-4` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-5` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-6` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-7` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-8` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-9` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-10` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-11` | stream | Stream of `/soc/cio@401ac0000/iommu@a80000`; stream/resource linkage needs review |
| `atc0-dpxbar` | resources | `/soc/mux@40304c000` |
| `atc0-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc0-dpin0` | resources | `/soc/displayport@401e50000` |
| `atc0-dpin1` | resources | `/soc/displayport@401e58000` |
| `acio-cpu1` | resources | `/soc/mailbox@409108000`; `/soc/cio@409ac0000` |
| `acio-cpu1/iop-acio1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec1` | resources | `/usb4-pcie-tunnel-1/pcie@1c60000000` |
| `apciec1/pcic1-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec1` | resources | `/usb4-pcie-tunnel-1/iommu@40c000000` |
| `dart-apciec1/mapper-apciec1-piodma` | stream | Stream of `/usb4-pcie-tunnel-1/iommu@40c000000`; stream/resource linkage needs review |
| `apciec1-piodma` | resources | `/soc/dma-controller@40c068000` |
| `acio1` | resources | `/soc/cio@409ac0000/nhi@f00000`; `/soc/cio@409ac0000` |
| `dart-acio1` | resources | `/soc/cio@409ac0000/iommu@a80000` |
| `dart-acio1/mapper-acio1-tx-0` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-1` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-2` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-3` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-4` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-5` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-6` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-7` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-8` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-9` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-10` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-11` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-0` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-1` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-2` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-3` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-4` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-5` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-6` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-7` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-8` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-9` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-10` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-11` | stream | Stream of `/soc/cio@409ac0000/iommu@a80000`; stream/resource linkage needs review |
| `atc1-dpxbar` | resources | `/soc/mux@40b04c000` |
| `atc1-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc1-dpin0` | resources | `/soc/displayport@409e50000` |
| `atc1-dpin1` | resources | `/soc/displayport@409e58000` |
| `atc2-dpxbar` | gap | **GAP:** no matching hardware resource node |
| `atc2-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `acio-cpu3` | gap | **GAP:** no matching hardware resource node |
| `acio-cpu3/iop-acio3-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec3` | gap | **GAP:** no matching hardware resource node |
| `apciec3/pcic3-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec3` | gap | **GAP:** no matching hardware resource node |
| `dart-apciec3/mapper-apciec3-piodma` | stream | Stream of `dart-apciec3`; stream/resource linkage needs review |
| `apciec3-piodma` | gap | **GAP:** no matching hardware resource node |
| `acio3` | gap | **GAP:** no matching hardware resource node |
| `dart-acio3` | gap | **GAP:** no matching hardware resource node |
| `dart-acio3/mapper-acio3-tx-0` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-1` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-2` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-3` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-4` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-5` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-6` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-7` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-8` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-9` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-10` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-tx-11` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-0` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-1` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-2` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-3` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-4` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-5` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-6` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-7` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-8` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-9` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-10` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `dart-acio3/mapper-acio3-rx-11` | stream | Stream of `dart-acio3`; stream/resource linkage needs review |
| `atc3-dpxbar` | gap | **GAP:** no matching hardware resource node |
| `atc3-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc3-dpin0` | gap | **GAP:** no matching hardware resource node |
| `atc3-dpin1` | gap | **GAP:** no matching hardware resource node |
| `display-crossbar0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `mcc` | partial resources | `/soc/power-management@380780000`; `/soc/memory-controller@220000000`; `/soc/pmp-report@3803c0000`; **GAP:** 9 larger or unmatched resource window(s) |
| `aic` | resources | `/soc/interrupt-controller@381000000` |
| `aic-timebase` | gap | **GAP:** no matching hardware resource node |
| `wdt` | partial resources | `/soc/watchdog@3882b0000`; **GAP:** 4 larger or unmatched resource window(s) |
| `error-handler` | partial resources | `/soc/power-management@380700000`; `/soc/pmp-report@3803c0000`; `/soc/power-management@382800000`; `/soc/memory-controller@220000000`; **GAP:** 22 larger or unmatched resource window(s) |
| `dwi` | resources | `/soc/dwi@380200000` |
| `pwm` | resources | `/soc/pwm@3ad040000` |
| `aes` | resources | `/soc/crypto@3ad00c000` |
| `gpio0` | resources | `/soc/pinctrl@39a000000` |
| `aop-gpio` | resources | `/soc/pinctrl@390824000` |
| `nub-gpio` | resources | `/soc/pinctrl@3881f0000` |
| `smc-gpio` | resources | `/soc/pinctrl@38c820000` |
| `aop` | resources | `/soc/aop@390600000` |
| `aop/iop-aop-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-audio` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-voicetrigger` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/accel` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/gyro` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/als` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-pio-filters` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-aop` | resources | `/soc/iommu@390f00000`; `/soc/filter@390f10000` |
| `dart-aop/mapper-aop` | stream | Stream of `/soc/iommu@390f00000` / `/soc/filter@390f10000`; stream/resource linkage needs review |
| `dart-aop/mapper-exclave-aop` | stream | Stream of `/soc/iommu@390f00000` / `/soc/filter@390f10000`; stream/resource linkage needs review |
| `dart-aop/mapper-admac-base-ns` | stream | Stream of `/soc/iommu@390f00000` / `/soc/filter@390f10000`; stream/resource linkage needs review |
| `dart-aop/mapper-admac-base-s` | stream | Stream of `/soc/iommu@390f00000` / `/soc/filter@390f10000`; stream/resource linkage needs review |
| `dart-aop/mapper-admac-leap-ns` | stream | Stream of `/soc/iommu@390f00000` / `/soc/filter@390f10000`; stream/resource linkage needs review |
| `dart-aop/mapper-admac-leap-s` | stream | Stream of `/soc/iommu@390f00000` / `/soc/filter@390f10000`; stream/resource linkage needs review |
| `pmgr` | partial resources | `/soc/power-management@380700000`; `/soc/power-management@388280000`; `/soc/performance-controller@210e20000`; `/soc/performance-controller@211e20000`; `/soc/serial@38812c000`; `/soc/power-management@380780000`; `/soc/display-sideband@3882b8044`; `/soc/pmp-report@3803c0000`; `/soc/power-management@382800000`; **GAP:** 47 larger or unmatched resource window(s) |
| `pmgr/clpc` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `pmgr/soc-tuner` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `pmgr/ppm` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `event-log-handler` | partial resources | `/soc/power-management@380700000`; `/soc/power-management@388280000`; `/soc/power-management@382800000`; **GAP:** 55 larger or unmatched resource window(s) |
| `pmp` | partial resources | `/soc/pmp@380500000`; `/soc/pmp-report@3803c0000`; **GAP:** 3 larger or unmatched resource window(s) |
| `pmp/iop-pmp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-pmp` | resources | `/soc/iommu@380300000` |
| `dart-pmp/mapper-pmp` | stream | Stream of `/soc/iommu@380300000`; stream/resource linkage needs review |
| `sep` | partial resources | `/soc/sep@3aa600000`; **GAP:** 1 larger or unmatched resource window(s) |
| `sep/iop-sep-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/Ocelot` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/xART` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/InvalidateHmac` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-sep` | resources | `/soc/iommu@3a8c80000` |
| `dart-sep/mapper-sep` | stream | Stream of `/soc/iommu@3a8c80000`; stream/resource linkage needs review |
| `dart-sep/mapper-sep-mpm` | stream | Stream of `/soc/iommu@3a8c80000`; stream/resource linkage needs review |
| `dart-sep/mapper-sep-exclave` | stream | Stream of `/soc/iommu@3a8c80000`; stream/resource linkage needs review |
| `esm` | gap | **GAP:** no matching hardware resource node |
| `sio` | gap | **GAP:** no matching hardware resource node |
| `sio/iop-sio-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sio/iop-sio-nub/sio-dma` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-sio` | resources | `/soc/iommu@3ad060000` |
| `dart-sio/mapper-sio` | stream | Stream of `/soc/iommu@3ad060000`; stream/resource linkage needs review |
| `dart-sio/mapper-aes` | stream | Stream of `/soc/iommu@3ad060000`; stream/resource linkage needs review |
| `ans` | gap | **GAP:** no matching hardware resource node |
| `ans/iop-ans-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sart-ans` | gap | **GAP:** no matching hardware resource node |
| `smc` | partial resources | `/soc/smc@38c600000`; **GAP:** 3 larger or unmatched resource window(s) |
| `smc/iop-smc-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-pmu` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-1` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-3` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aon-ptd` | resources | `/soc/interrupt-controller@38c840000` |
| `pmgrtools` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `rsm` | gap | **GAP:** no matching hardware resource node |
| `dart-rsm` | gap | **GAP:** no matching hardware resource node |
| `dart-rsm/mapper-rsm` | stream | Stream of `dart-rsm`; stream/resource linkage needs review |
| `uart0` | partial resources | `/soc/serial@3ad200000`; **GAP:** 1 larger or unmatched resource window(s) |
| `uart0/debug-console` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-uart` | partial resources | `/soc/serial@38812c000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dockchannel-mtp` | partial resources | `/soc/fifo@394b14000`; `/soc/fifo@394b14000/input@0`; **GAP:** 1 larger or unmatched resource window(s) |
| `dockchannel-mtp/mtp-transport` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/multi-touch` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/keyboard` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/stm` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/actuator` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/tp-accel` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mtp` | partial resources | `/soc/mtp@394600000`; **GAP:** 2 larger or unmatched resource window(s) |
| `mtp/iop-mtp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-mtp` | resources | `/soc/iommu@394800000`; `/soc/filter@394810000` |
| `dart-mtp/mapper-mtp` | stream | Stream of `/soc/iommu@394800000` / `/soc/filter@394810000`; stream/resource linkage needs review |
| `mtp-aop-mux` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `qspi` | resources | `/soc/spi@3ad118000` |
| `qspi/spinor` | gap | **GAP:** SPI NOR endpoint is not described |
| `qspi/spinor/firmware` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `qspi/spinor/anvram` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `qspi/spinor/syscfg` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c2` | resources | `/soc/i2c@3ad018000` |
| `i2c2/atcrt0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c2/atcrt1` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c2/atcrt3` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dp2hdmi-gpio0` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `atc-phy2` | gap | **GAP:** no matching hardware resource node |
| `atc-phy0` | partial resources | `/soc/phy@402a90000`; `/soc/mux@40304c000`; `/usb4-pcie-tunnel-0/pcie@1c50000000`; **GAP:** 3 larger or unmatched resource window(s) |
| `dart-usb0` | resources | `/soc/iommu@402f00000`; `/soc/iommu@402f80000` |
| `dart-usb0/mapper-usb0` | stream | Stream of `/soc/iommu@402f00000` / `/soc/iommu@402f80000`; stream/resource linkage needs review |
| `usb-drd0` | partial resources | `/soc/usb@402280000`; `/soc/phy@402a90000`; **GAP:** 8 larger or unmatched resource window(s) |
| `usb-drd0/usb-drd0-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd0/usb-drd0-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `atc-phy1` | partial resources | `/soc/phy@40aa90000`; `/soc/mux@40b04c000`; `/usb4-pcie-tunnel-1/pcie@1c60000000`; **GAP:** 3 larger or unmatched resource window(s) |
| `dart-usb1` | resources | `/soc/iommu@40af00000`; `/soc/iommu@40af80000` |
| `dart-usb1/mapper-usb1` | stream | Stream of `/soc/iommu@40af00000` / `/soc/iommu@40af80000`; stream/resource linkage needs review |
| `usb-drd1` | partial resources | `/soc/usb@40a280000`; `/soc/phy@40aa90000`; **GAP:** 8 larger or unmatched resource window(s) |
| `usb-drd1/usb-drd1-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd1/usb-drd1-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `atc-phy3` | gap | **GAP:** no matching hardware resource node |
| `dart-usb3` | gap | **GAP:** no matching hardware resource node |
| `dart-usb3/mapper-usb3` | stream | Stream of `dart-usb3`; stream/resource linkage needs review |
| `usb-drd3` | gap | **GAP:** no matching hardware resource node |
| `usb-drd3/usb-drd3-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd3/usb-drd3-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `nub-spmi0` | gap | **GAP:** no matching hardware resource node |
| `nub-spmi0/pmu-main` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi0/btm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi1` | resources | `/soc/spmi@388614000` |
| `nub-spmi1/pmu-aux` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0` | partial resources | `/soc/spmi@388908000`; **GAP:** 2 larger or unmatched resource window(s) |
| `nub-spmi-a0/hpm0` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0/hpm1` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0/hpm5` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `disp0` | partial resources | `/soc/power-management@382800000`; **GAP:** 5 larger or unmatched resource window(s) |
| `dcp0-expert` | partial resources | `/soc/display-sideband@3882b8044`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcp` | gap | **GAP:** no matching hardware resource node |
| `dcp/iop-dcp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcp` | gap | **GAP:** no matching hardware resource node |
| `dart-dcp/mapper-dcp` | stream | Stream of `dart-dcp`; stream/resource linkage needs review |
| `dart-disp0` | partial resources | `/soc/iommu@30a314000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-disp0/mapper-disp0` | stream | Stream of `/soc/iommu@30a314000`; stream/resource linkage needs review |
| `dart-disp0/mapper-disp0-piodma` | stream | Stream of `/soc/iommu@30a314000`; stream/resource linkage needs review |
| `dcp-sac-controller` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `dp-audio0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext0` | partial resources | `/soc/power-management@382800000`; **GAP:** 5 larger or unmatched resource window(s) |
| `dcpext0-expert` | partial resources | `/soc/display-sideband@3882b8044`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcpext0` | gap | **GAP:** no matching hardware resource node |
| `dcpext0/iop-dcpext0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext0` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext0/mapper-dcpext0` | stream | Stream of `dart-dcpext0`; stream/resource linkage needs review |
| `dart-dispext0` | partial resources | `/soc/iommu@286314000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-dispext0/mapper-dispext0` | stream | Stream of `/soc/iommu@286314000`; stream/resource linkage needs review |
| `dart-dispext0/mapper-dispext0-piodma` | stream | Stream of `/soc/iommu@286314000`; stream/resource linkage needs review |
| `dp-audio1` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext1` | partial resources | `/soc/power-management@382800000`; **GAP:** 5 larger or unmatched resource window(s) |
| `dcpext1-expert` | partial resources | `/soc/display-sideband@3882b8044`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcpext1` | gap | **GAP:** no matching hardware resource node |
| `dcpext1/iop-dcpext1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext1` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext1/mapper-dcpext1` | stream | Stream of `dart-dcpext1`; stream/resource linkage needs review |
| `dart-dispext1` | partial resources | `/soc/iommu@48a314000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-dispext1/mapper-dispext1` | stream | Stream of `/soc/iommu@48a314000`; stream/resource linkage needs review |
| `dart-dispext1/mapper-dispext1-piodma` | stream | Stream of `/soc/iommu@48a314000`; stream/resource linkage needs review |
| `dp-audio2` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `scaler0` | resources | `/soc/scaler@505000000` |
| `dart-scaler` | resources | `/soc/iommu@505260000` |
| `dart-scaler/mapper-scaler` | stream | Stream of `/soc/iommu@505260000`; stream/resource linkage needs review |
| `dart-scaler/mapper-scaler-piodma` | stream | Stream of `/soc/iommu@505260000`; stream/resource linkage needs review |
| `jpeg0` | resources | `/soc/jpeg@3a5000000` |
| `dart-jpeg0` | resources | `/soc/iommu@3a5020000` |
| `dart-jpeg0/mapper-jpeg0` | stream | Stream of `/soc/iommu@3a5020000`; stream/resource linkage needs review |
| `jpeg1` | resources | `/soc/jpeg@3a5060000` |
| `dart-jpeg1` | resources | `/soc/iommu@3a5080000` |
| `dart-jpeg1/mapper-jpeg1` | stream | Stream of `/soc/iommu@3a5080000`; stream/resource linkage needs review |
| `ave` | resources | `/soc/video-encoder@421100000` |
| `dart-ave` | resources | `/soc/iommu@421080000`; `/soc/iommu@4210a0000`; `/soc/iommu@421094000`; `/soc/filter@4210b0000` |
| `dart-ave/mapper-ave` | stream | Stream of `/soc/iommu@421080000` / `/soc/iommu@4210a0000` / `/soc/iommu@421094000` / `/soc/filter@4210b0000`; stream/resource linkage needs review |
| `avd` | gap | **GAP:** no matching hardware resource node |
| `dart-avd` | gap | **GAP:** no matching hardware resource node |
| `dart-avd/mapper-avd` | stream | Stream of `dart-avd`; stream/resource linkage needs review |
| `dart-avd/mapper-avd-piodma` | stream | Stream of `dart-avd`; stream/resource linkage needs review |
| `dart-avd/mapper-avd-adsbuf` | stream | Stream of `dart-avd`; stream/resource linkage needs review |
| `apr` | resources | `/soc/video-codec@281000000` |
| `dart-apr` | resources | `/soc/iommu@281020000` |
| `dart-apr/mapper-apr` | stream | Stream of `/soc/iommu@281020000`; stream/resource linkage needs review |
| `ispRtb` | partial resources | `/soc/isp@48c000000`; `/soc/video-encoder@421100000`; `/soc/pinctrl@39a000000`; **GAP:** 3 larger or unmatched resource window(s) |
| `ispRtb/iop-isp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `isp` | partial resources | `/soc/isp@48c000000`; `/soc/video-encoder@421100000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-isp` | resources | `/soc/iommu@48e8c0000`; `/soc/iommu@48e8e0000`; `/soc/iommu@48e900000`; `/soc/iommu@48e8f4000`; `/soc/iommu@48e914000` |
| `dart-isp/mapper-isp` | stream | Stream of `/soc/iommu@48e8c0000` / `/soc/iommu@48e8e0000` / `/soc/iommu@48e900000` / `/soc/iommu@48e8f4000` / `/soc/iommu@48e914000`; stream/resource linkage needs review |
| `dart-isp/mapper-isp-mpm` | stream | Stream of `/soc/iommu@48e8c0000` / `/soc/iommu@48e8e0000` / `/soc/iommu@48e900000` / `/soc/iommu@48e8f4000` / `/soc/iommu@48e914000`; stream/resource linkage needs review |
| `ane` | partial resources | `/soc/filter@501810000`; `/soc/video-encoder@421100000`; `/soc/pmp-report@3803c0000`; `/soc/performance-controller@211e20000`; **GAP:** 6 larger or unmatched resource window(s) |
| `dart-ane` | partial resources | `/soc/filter@501810000`; **GAP:** 3 larger or unmatched resource window(s) |
| `dart-ane/mapper-ane` | stream | Stream of `/soc/filter@501810000`; stream/resource linkage needs review |
| `dart-ane/mapper-ane-mpm` | stream | Stream of `/soc/filter@501810000`; stream/resource linkage needs review |
| `sgx` | gap | **GAP:** no matching hardware resource node |
| `gfx-asc` | gap | **GAP:** no matching hardware resource node |
| `gfx-asc/iop-gfx-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mapper-gfx-asc` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `iop-audio-controller` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `iop-audio-controller/audio-hp` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-hp/audio-hp-mic` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-leap-internal-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-leap-internal-loopback/audio-leap-internal-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-mca-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-mca-loopback/audio-mca-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-speaker` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-speaker/audio-speaker` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-codec-output` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-codec-output/audio-codec-output` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-codec-input` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-codec-input/audio-codec-input` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/lp-mic-device` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/lp-mic-io-buffer-device` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/siri-listening-device` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `admac-base-ns` | resources | `/soc/dma-controller@393800000`; `/soc/aop@390600000` |
| `admac-leap-ns` | resources | `/soc/dma-controller@393300000`; `/soc/aop@390600000` |
| `smctempsensor0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `cpu-debug-interface` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `dbgCPU` | resources | `/soc/debug@389700000` |
| `aft` | resources | `/soc/trace@220180000` |
| `timesync` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `fillmore` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `sapt` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `siri-exclave-timebase` | gap | **GAP:** no matching hardware resource node |
| `apple-processor-trace` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |

## Other root records

Boot metadata subtrees are identified but their properties and private data are not reproduced. Runtime memory allocations must be supplied by the loader rather than copied from a firmware template.

| Root hardware or metadata record | Linux responsibility / review |
| --- | --- |
| `chosen` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `options` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `cpus` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu0` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu1` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu2` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu3` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu4` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu5` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu6` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu7` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu8` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu9` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `aliases` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `memory` | Loader-owned runtime memory or reserved-memory allocation; not a static template address. |
| `pram` | Loader-owned runtime memory or reserved-memory allocation; not a static template address. |
| `vram` | Loader-owned runtime memory or reserved-memory allocation; not a static template address. |
| `socd-trace-ram` | Loader-owned runtime memory or reserved-memory allocation; not a static template address. |
| `hibernate` | Loader-owned runtime memory or reserved-memory allocation; not a static template address. |
| `amfm` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `pcie-sdreader-helper` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `buttons` | **GAP:** reconcile GPIO key and wakeup relationships. |
| `port-usb-c-1` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-usb-c-2` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-usb-c-4` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-hdmi-1` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-magsafe3-1` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `backlight` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `sacm` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `defaults` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `product` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `iboot-syscfg` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `filesystems-props` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `osenvironments` | Boot metadata; no independent hardware device. Subtree properties omitted. |
