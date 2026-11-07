# DT gaps: J614S

## Identity and scope

Board DTS: `arch/arm64/boot/dts/apple/t6040-j614s.dts`.
The hardware resource inventory identifies the T6041 peripheral die.

Source: per-board 27.2 hardware-description template; no matching live dump in the supplied collection. Only hardware identities and resource mappings are recorded here; no tuning or factory data is reproduced.

## Result

This resource coverage audit still has unresolved entries and does not establish complete board support. 420 arm-io records were examined. Hardware blocks added in this series remain disabled. A resource match does not prove IRQ, clock, power or DMA stream correctness.

**Audit basis:** DTS source analysis; compiled-DTB reconciliation pending.

| Record classification | Count |
| --- | ---: |
| bus device | 18 |
| gap | 33 |
| partial resources | 57 |
| resources | 76 |
| service review | 104 |
| shared hardware | 7 |
| stream | 125 |

## Remaining work

- Close every explicit GAP and service-review entry below, including nested endpoints and shared resource ownership.
- Some template SPMI records are alternative endpoints at the same address. Runtime model selection is needed before representing these as simultaneous devices.
- Reconcile larger firmware resource windows with the driver-specific Linux register layout; do not expand an active device aperture without driver review.
- Bindings and resource presence do not supply per-device calibration, firmware or an initialization contract.
- Validate this board with `W=1` and `dtbs_check`, then perform a separately approved boot before enabling newly described engines.

## Dependencies

- t-5538 owns J616s and shared T604x resource descriptions. This branch builds on its commit 1a6cd012; J616s board files are not duplicated.
- linux PR #1042 owns Stockholm/NFC; #1054 owns AOP devmotion6 and temperature services; #1052 owns USB4 tunnel bring-up.
- linux PR #1092 owns J616c fan enablement.
- Newly introduced boards are description scaffolding and require endpoint/lifecycle qualification before hardware enablement.

## Every arm-io record

Paths in the first column are relative to `arm-io`. Stream entries name the owning translation block; their SID and aperture semantics still need review. Service-review entries have no independently decoded MMIO resource; this is not a waiver of their hardware or binding requirements.

| Firmware hardware/service record | Classification | Linux description or gap |
| --- | --- | --- |
| `displaymanager` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `i2c1` | resources | `/i2c@429014000` |
| `i2c1/audio-speaker` | bus device | `/i2c@429014000/codec@38` |
| `i2c1/audio-speaker-left-woofer-2` | bus device | `/i2c@429014000/codec@39` |
| `i2c1/audio-speaker-left-tweeter` | bus device | `/i2c@429014000/codec@3a` |
| `i2c2` | resources | `/i2c@429018000` |
| `i2c2/audio-codec-output` | bus device | `/i2c@429018000/codec@4b` |
| `i2c3` | resources | `/i2c@42901c000` |
| `i2c3/audio-speaker-right-woofer-1` | bus device | `/i2c@42901c000/codec@3b` |
| `i2c3/audio-speaker-right-woofer-2` | bus device | `/i2c@42901c000/codec@3c` |
| `i2c3/audio-speaker-right-tweeter` | bus device | `/i2c@42901c000/codec@3d` |
| `iop-voicetrigger-controller` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `iop-voicetrigger-controller/audio-vt-device` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c4` | resources | `/i2c@429020000` |
| `i2c4/sd-card` | gap | **GAP:** I2C device not described under this bus |
| `i2c6` | resources | `/i2c@429028000` |
| `i2c6/uatcrt0` | gap | **GAP:** I2C device not described under this bus |
| `i2c6/uatcrt1` | gap | **GAP:** I2C device not described under this bus |
| `i2c6/uatcrt2` | gap | **GAP:** I2C device not described under this bus |
| `i2c8` | resources | `/i2c@429030000` |
| `i2c8/pcon0` | gap | **GAP:** I2C device not described under this bus |
| `spi2` | resources | `/spi@429108000` |
| `spi2/mesa` | bus device | `/spi@429108000/fingerprint@0` |
| `spi4` | resources | `/spi@429110000` |
| `spi4/dp855` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `spi4/dp855/lcd-pmicwp` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-pmic` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-sswp` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/tcon-registers` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-eeprom` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/display-temp-sens1` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `spi4/display-temp-sens2` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `spi4/display-temp-sens3` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `ispRtb` | partial resources | `/isp@580000000`; `/ane@484000000`; **GAP:** 2 larger or unmatched resource window(s) |
| `ispRtb/iop-isp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `isp0` | partial resources | `/isp@580000000`; `/ane@484000000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-isp0` | partial resources | `/iommu@5828c0000`; `/iommu@5828e0000`; `/iommu@582900000`; **GAP:** 3 larger or unmatched resource window(s) |
| `dart-isp0/mapper-isp0` | stream | Stream of `/iommu@5828c0000` / `/iommu@5828e0000` / `/iommu@582900000`; stream/resource linkage needs review |
| `pwm0` | resources | `/pwm@429040000` |
| `pwm0/kbd-backlight` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0` | partial resources | `/pcie@1cb0000000`; **GAP:** 30 larger or unmatched resource window(s) |
| `apcie0/pci-bridge0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge0/wlan` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge0/bluetooth-pcie` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge1` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge1/pcie-sdreader` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apcie0` | resources | `/iommu@410000000` |
| `dart-apcie0/mapper-apcie0-wlan` | stream | Stream of `/iommu@410000000`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-bt` | stream | Stream of `/iommu@410000000`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-piodma` | stream | Stream of `/iommu@410000000`; stream/resource linkage needs review |
| `apcie0-piodma` | resources | `/dma@410030000` |
| `dart-apcie1` | resources | `/iommu@411000000` |
| `dart-apcie1/mapper-apcie1` | stream | Stream of `/iommu@411000000`; stream/resource linkage needs review |
| `dart-apcie1/mapper-apcie1-piodma` | stream | Stream of `/iommu@411000000`; stream/resource linkage needs review |
| `apcie1-piodma` | resources | `/dma@411030000` |
| `acio-phy-cpu0` | resources | `/mailbox@383148000`; `/phy@382a90000` |
| `acio-phy-cpu0/iop-aciophy0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `acio-phy-cpu1` | resources | `/mailbox@38b148000`; `/phy@38aa90000` |
| `acio-phy-cpu1/iop-aciophy1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `acio-phy-cpu2` | resources | `/mailbox@393148000`; `/phy@392a90000` |
| `acio-phy-cpu2/iop-aciophy2-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `acio-phy-cpu3` | resources | `/mailbox@39b148000`; `/phy@39aa90000` |
| `acio-phy-cpu3/iop-aciophy3-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp` | partial resources | `/fifo@514b14000`; `/fifo@514b14000/input@0`; **GAP:** 1 larger or unmatched resource window(s) |
| `dockchannel-mtp/mtp-transport` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/multi-touch` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/keyboard` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/stm` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/actuator` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/tp-accel` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mtp` | partial resources | `/mtp@514600000`; **GAP:** 2 larger or unmatched resource window(s) |
| `mtp/iop-mtp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-mtp` | partial resources | `/iommu@514800000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-mtp/mapper-mtp` | stream | Stream of `/iommu@514800000`; stream/resource linkage needs review |
| `mtp-aop-mux` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `secure-repair` | resources | `/pinctrl@5081e4000` |
| `dynamic-pil` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dynamic-pil/cil` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aod2` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `apciec0` | partial resources | `/phy@382a90000`; **GAP:** 7 larger or unmatched resource window(s) |
| `apciec0/pcic0-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec0` | resources | `/iommu@384000000` |
| `dart-apciec0/mapper-apciec0-piodma` | stream | Stream of `/iommu@384000000`; stream/resource linkage needs review |
| `apciec0-piodma` | resources | `/dma@384068000` |
| `acio0` | gap | **GAP:** no matching hardware resource node |
| `dart-acio0` | resources | `/iommu@381a80000` |
| `dart-acio0/mapper-acio0-tx-0` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-1` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-2` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-3` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-4` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-5` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-6` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-7` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-8` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-9` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-10` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-11` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-0` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-1` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-2` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-3` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-4` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-5` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-6` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-7` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-8` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-9` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-10` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-11` | stream | Stream of `/iommu@381a80000`; stream/resource linkage needs review |
| `acio-cpu0` | partial resources | `/mailbox@381108000`; **GAP:** 1 larger or unmatched resource window(s) |
| `acio-cpu0/iop-acio0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec1` | partial resources | `/phy@38aa90000`; **GAP:** 7 larger or unmatched resource window(s) |
| `apciec1/pcic1-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec1` | resources | `/iommu@38c000000` |
| `dart-apciec1/mapper-apciec1-piodma` | stream | Stream of `/iommu@38c000000`; stream/resource linkage needs review |
| `apciec1-piodma` | resources | `/dma@38c068000` |
| `acio1` | gap | **GAP:** no matching hardware resource node |
| `dart-acio1` | resources | `/iommu@389a80000` |
| `dart-acio1/mapper-acio1-tx-0` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-1` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-2` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-3` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-4` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-5` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-6` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-7` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-8` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-9` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-10` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-11` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-0` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-1` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-2` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-3` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-4` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-5` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-6` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-7` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-8` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-9` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-10` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-11` | stream | Stream of `/iommu@389a80000`; stream/resource linkage needs review |
| `acio-cpu1` | partial resources | `/mailbox@389108000`; **GAP:** 1 larger or unmatched resource window(s) |
| `acio-cpu1/iop-acio1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec2` | partial resources | `/phy@392a90000`; **GAP:** 7 larger or unmatched resource window(s) |
| `apciec2/pcic2-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec2` | resources | `/iommu@394000000` |
| `dart-apciec2/mapper-apciec2-piodma` | stream | Stream of `/iommu@394000000`; stream/resource linkage needs review |
| `apciec2-piodma` | resources | `/dma@394068000` |
| `acio2` | gap | **GAP:** no matching hardware resource node |
| `dart-acio2` | resources | `/iommu@391a80000` |
| `dart-acio2/mapper-acio2-tx-0` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-1` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-2` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-3` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-4` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-5` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-6` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-7` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-8` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-9` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-10` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-11` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-0` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-1` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-2` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-3` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-4` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-5` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-6` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-7` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-8` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-9` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-10` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-11` | stream | Stream of `/iommu@391a80000`; stream/resource linkage needs review |
| `acio-cpu2` | partial resources | `/mailbox@391108000`; **GAP:** 1 larger or unmatched resource window(s) |
| `acio-cpu2/iop-acio2-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `wlan` | shared hardware | `/pcie@1cb0000000/pci@0,0/wifi@0,0` |
| `bluetooth` | shared hardware | `/pcie@1cb0000000/pci@0,0/bluetooth@0,1` |
| `display-crossbar0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc0-dpxbar` | resources | `/mux@38324c000` |
| `atc0-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc0-dpin0` | resources | `/dpin@381e50000` |
| `atc0-dpin1` | resources | `/dpin@381e58000` |
| `atc1-dpxbar` | resources | `/mux@38b24c000` |
| `atc1-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc1-dpin0` | resources | `/dpin@389e50000` |
| `atc1-dpin1` | resources | `/dpin@389e58000` |
| `atc2-dpxbar` | resources | `/mux@39324c000` |
| `atc2-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc2-dpin0` | resources | `/dpin@391e50000` |
| `atc2-dpin1` | resources | `/dpin@391e58000` |
| `atc3-dpxbar` | resources | `/mux@39b24c000` |
| `atc3-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `mcc` | partial resources | `/clock-controller@502800000`; `/ane@484000000`; `/memory-controller@220000000`; **GAP:** 29 larger or unmatched resource window(s) |
| `aic` | partial resources | `/interrupt-controller@502400000`; **GAP:** 1 larger or unmatched resource window(s) |
| `aic-timebase` | resources | `/aic-timebase@502580000` |
| `wdt` | partial resources | `/watchdog@50836c000`; **GAP:** 4 larger or unmatched resource window(s) |
| `error-handler` | partial resources | `/clock-controller@502800000`; `/ane@484000000`; `/memory-controller@220000000`; `/error-handler@24f000000`; **GAP:** 53 larger or unmatched resource window(s) |
| `dwi` | resources | `/dwi@5029b0000` |
| `aes` | resources | `/crypto@42900c000` |
| `gpio0` | resources | `/pinctrl@51c000000` |
| `aop-gpio0` | resources | `/pinctrl@510824000` |
| `nub-gpio0` | resources | `/pinctrl@5081e4000` |
| `smc-gpio0` | resources | `/pinctrl@50c824000` |
| `aop` | partial resources | `/aop@510600000`; **GAP:** 1 larger or unmatched resource window(s) |
| `aop/iop-aop-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-audio` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-voicetrigger` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/als` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/accel` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/gyro` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-aop` | partial resources | `/iommu@510f00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-aop/mapper-aop` | stream | Stream of `/iommu@510f00000`; stream/resource linkage needs review |
| `dart-aop/mapper-admac-base-ns` | stream | Stream of `/iommu@510f00000`; stream/resource linkage needs review |
| `dart-aop/mapper-admac-base-s` | stream | Stream of `/iommu@510f00000`; stream/resource linkage needs review |
| `dart-aop/mapper-admac-leap-ns` | stream | Stream of `/iommu@510f00000`; stream/resource linkage needs review |
| `dart-aop/mapper-admac-leap-s` | stream | Stream of `/iommu@510f00000`; stream/resource linkage needs review |
| `pmgr` | partial resources | `/clock-controller@502800000`; `/cpufreq@210e20000`; `/cpufreq@211e20000`; `/ane@484000000`; **GAP:** 5 larger or unmatched resource window(s) |
| `pmgr/soc-tuner` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `pmgr/clpc` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `pmgr/ppm` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `event-log-handler` | partial resources | `/clock-controller@502800000`; `/watchdog@50836c000`; `/power-management@504800000`; `/ane@484000000`; `/gpu@288000000`; **GAP:** 1 larger or unmatched resource window(s) |
| `sio` | partial resources | `/dma-controller@429e00000`; **GAP:** 2 larger or unmatched resource window(s) |
| `sio/iop-sio-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sio/iop-sio-nub/sio-dma` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-sio` | resources | `/iommu@429060000` |
| `dart-sio/mapper-sio` | stream | Stream of `/iommu@429060000`; stream/resource linkage needs review |
| `dart-sio/mapper-aes` | stream | Stream of `/iommu@429060000`; stream/resource linkage needs review |
| `pmp` | partial resources | `/pmp@500700000`; `/ane@484000000`; **GAP:** 2 larger or unmatched resource window(s) |
| `pmp/iop-pmp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-pmp` | resources | `/iommu@500300000` |
| `dart-pmp/mapper-pmp` | stream | Stream of `/iommu@500300000`; stream/resource linkage needs review |
| `dart-pmp/mapper-pmp-ptd-fwd` | stream | Stream of `/iommu@500300000`; stream/resource linkage needs review |
| `sep` | partial resources | `/sep@406600000`; **GAP:** 1 larger or unmatched resource window(s) |
| `sep/iop-sep-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/Ocelot` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/xART` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/InvalidateHmac` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-sep` | resources | `/iommu@404c80000` |
| `dart-sep/mapper-sep` | stream | Stream of `/iommu@404c80000`; stream/resource linkage needs review |
| `ans` | partial resources | `/nvme@44dcc0000`; **GAP:** 6 larger or unmatched resource window(s) |
| `ans/iop-ans-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `ans/ans-pcie` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sart-ans` | partial resources | `/sart@40dc50000`; `/nvme@44dcc0000`; **GAP:** 1 larger or unmatched resource window(s) |
| `smc` | partial resources | `/smc@50c600000`; **GAP:** 3 larger or unmatched resource window(s) |
| `smc/iop-smc-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-pmu` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-1` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-2` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aon-ptd` | resources | `/aon-ptd@50cdc0000` |
| `pmgrtools` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `rsm` | gap | **GAP:** no matching hardware resource node |
| `dart-rsm` | gap | **GAP:** no matching hardware resource node |
| `dart-rsm/mapper-rsm` | stream | Stream of `dart-rsm`; stream/resource linkage needs review |
| `uart0` | resources | `/serial@429200000` |
| `uart0/debug-console` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-uart` | partial resources | `/serial@50883c000`; **GAP:** 1 larger or unmatched resource window(s) |
| `qspi` | resources | `/spi@429118000` |
| `qspi/spinor` | bus device | `/spi@429118000/flash@0` |
| `qspi/spinor/firmware` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `qspi/spinor/anvram` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `qspi/spinor/syscfg` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dp2hdmi-gpio0` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `atc-phy0` | partial resources | `/phy@382a90000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-usb0` | resources | `/iommu@382f00000`; `/iommu@382f80000` |
| `dart-usb0/mapper-usb0` | stream | Stream of `/iommu@382f00000` / `/iommu@382f80000`; stream/resource linkage needs review |
| `usb-drd0` | partial resources | `/usb@382280000`; `/phy@382a90000`; **GAP:** 6 larger or unmatched resource window(s) |
| `usb-drd0/usb-drd0-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd0/usb-drd0-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `atc-phy1` | partial resources | `/phy@38aa90000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-usb1` | resources | `/iommu@38af00000`; `/iommu@38af80000` |
| `dart-usb1/mapper-usb1` | stream | Stream of `/iommu@38af00000` / `/iommu@38af80000`; stream/resource linkage needs review |
| `usb-drd1` | partial resources | `/usb@38a280000`; `/phy@38aa90000`; **GAP:** 6 larger or unmatched resource window(s) |
| `usb-drd1/usb-drd1-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd1/usb-drd1-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `atc-phy2` | partial resources | `/phy@392a90000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-usb2` | resources | `/iommu@392f00000`; `/iommu@392f80000` |
| `dart-usb2/mapper-usb2` | stream | Stream of `/iommu@392f00000` / `/iommu@392f80000`; stream/resource linkage needs review |
| `usb-drd2` | partial resources | `/usb@392280000`; `/phy@392a90000`; **GAP:** 6 larger or unmatched resource window(s) |
| `usb-drd2/usb-drd2-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd2/usb-drd2-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `atc-phy3` | partial resources | `/phy@39aa90000`; **GAP:** 2 larger or unmatched resource window(s) |
| `nub-spmi0` | partial resources | `/spmi@509014000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi0/spmi-abbeyL1` | bus device | `/spmi@509014000/pmic@e` |
| `nub-spmi0/btm` | bus device | `/spmi@509014000/battery-monitor@b` |
| `nub-spmi1` | partial resources | `/spmi@509054000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi1/spmi-abbeyF1` | bus device | `/spmi@509054000/pmic@e` |
| `nub-spmi2` | partial resources | `/spmi@509094000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi2/spmi-abbeyF2` | bus device | `/spmi@509094000/pmic@e` |
| `nub-spmi3` | partial resources | `/spmi@5090d4000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi4` | partial resources | `/spmi@509114000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi-a0` | partial resources | `/spmi@509188000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi-a0/hpm0` | bus device | `/spmi@509188000/usb-pd@c` |
| `nub-spmi-a0/hpm1` | bus device | `/spmi@509188000/usb-pd@a` |
| `nub-spmi-a0/hpm5` | bus device | `/spmi@509188000/usb-pd@8` |
| `nub-spmi-a1` | partial resources | `/spmi@509198000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi-a1/hpm2` | bus device | `/spmi@509198000/usb-pd@c` |
| `aop-spmi0` | partial resources | `/spmi@510914000`; **GAP:** 3 larger or unmatched resource window(s) |
| `aop-spmi0/stockholm-spmi` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi0/stockholm-spmi/stockholm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi0/spmi-wifibt` | bus device | `/spmi@510914000/pmic@e` |
| `disp0` | partial resources | `/dcp@402e00000`; `/power-management@504800000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-disp0` | resources | `/iommu@402300000`; `/dcp@402e00000` |
| `dart-disp0/mapper-disp0` | stream | Stream of `/iommu@402300000` / `/dcp@402e00000`; stream/resource linkage needs review |
| `dart-disp0/mapper-disp0-piodma` | stream | Stream of `/iommu@402300000` / `/dcp@402e00000`; stream/resource linkage needs review |
| `dcp0-expert` | resources | `/display-expert@4023a8050` |
| `dcp` | partial resources | `/dcp@402e00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcp/iop-dcp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcp` | resources | `/iommu@402340000` |
| `dart-dcp/mapper-dcp` | stream | Stream of `/iommu@402340000`; stream/resource linkage needs review |
| `dcp0-sac-controller` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `dp-audio0` | shared hardware | `/audio-controller-0`; `/audio-controller-3`; `/audio-controller-4`; `/audio-controller-1`; `/audio-controller-2` |
| `dispext0` | partial resources | `/dcp@3a2e00000`; `/power-management@504800000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dcpext0-expert` | resources | `/display-expert@3a23a8050` |
| `dcpext0` | partial resources | `/dcp@3a2e00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcpext0/iop-dcpext0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext0` | resources | `/iommu@3a2340000` |
| `dart-dcpext0/mapper-dcpext0` | stream | Stream of `/iommu@3a2340000`; stream/resource linkage needs review |
| `dart-dispext0` | resources | `/iommu@3a2300000`; `/dcp@3a2e00000` |
| `dart-dispext0/mapper-dispext0` | stream | Stream of `/iommu@3a2300000` / `/dcp@3a2e00000`; stream/resource linkage needs review |
| `dart-dispext0/mapper-dispext0-piodma` | stream | Stream of `/iommu@3a2300000` / `/dcp@3a2e00000`; stream/resource linkage needs review |
| `dp-audio1` | shared hardware | `/audio-controller-0`; `/audio-controller-3`; `/audio-controller-4`; `/audio-controller-1`; `/audio-controller-2` |
| `dispext1` | partial resources | `/dcp@602e00000`; `/power-management@504800000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dcpext1-expert` | resources | `/display-expert@6023a8050` |
| `dcpext1` | partial resources | `/dcp@602e00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcpext1/iop-dcpext1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext1` | resources | `/iommu@602340000` |
| `dart-dcpext1/mapper-dcpext1` | stream | Stream of `/iommu@602340000`; stream/resource linkage needs review |
| `dart-dispext1` | resources | `/iommu@602300000`; `/dcp@602e00000` |
| `dart-dispext1/mapper-dispext1` | stream | Stream of `/iommu@602300000` / `/dcp@602e00000`; stream/resource linkage needs review |
| `dart-dispext1/mapper-dispext1-piodma` | stream | Stream of `/iommu@602300000` / `/dcp@602e00000`; stream/resource linkage needs review |
| `dp-audio2` | shared hardware | `/audio-controller-0`; `/audio-controller-3`; `/audio-controller-4`; `/audio-controller-1`; `/audio-controller-2` |
| `dispext2` | partial resources | `/power-management@504800000`; **GAP:** 4 larger or unmatched resource window(s) |
| `dcpext2-expert` | gap | **GAP:** no matching hardware resource node |
| `dcpext2` | gap | **GAP:** no matching hardware resource node |
| `dcpext2/iop-dcpext2-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext2` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext2/mapper-dcpext2` | stream | Stream of `dart-dcpext2`; stream/resource linkage needs review |
| `dart-dispext2` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext2/mapper-dispext2` | stream | Stream of `dart-dispext2`; stream/resource linkage needs review |
| `dart-dispext2/mapper-dispext2-piodma` | stream | Stream of `dart-dispext2`; stream/resource linkage needs review |
| `dp-audio3` | shared hardware | `/audio-controller-0`; `/audio-controller-3`; `/audio-controller-4`; `/audio-controller-1`; `/audio-controller-2` |
| `dispext3` | partial resources | `/power-management@504800000`; **GAP:** 4 larger or unmatched resource window(s) |
| `dcpext3-expert` | gap | **GAP:** no matching hardware resource node |
| `dcpext3` | gap | **GAP:** no matching hardware resource node |
| `dcpext3/iop-dcpext3-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext3` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext3/mapper-dcpext3` | stream | Stream of `dart-dcpext3`; stream/resource linkage needs review |
| `dart-dispext3` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext3/mapper-dispext3` | stream | Stream of `dart-dispext3`; stream/resource linkage needs review |
| `dart-dispext3/mapper-dispext3-piodma` | stream | Stream of `dart-dispext3`; stream/resource linkage needs review |
| `dp-audio4` | shared hardware | `/audio-controller-0`; `/audio-controller-3`; `/audio-controller-4`; `/audio-controller-1`; `/audio-controller-2` |
| `scaler0` | resources | `/scaler@521000000` |
| `dart-scaler0` | resources | `/iommu@521260000` |
| `dart-scaler0/mapper-scaler0` | stream | Stream of `/iommu@521260000`; stream/resource linkage needs review |
| `dart-scaler0/mapper-scaler-piodma0` | stream | Stream of `/iommu@521260000`; stream/resource linkage needs review |
| `scaler1` | gap | **GAP:** no matching hardware resource node |
| `dart-scaler1` | gap | **GAP:** no matching hardware resource node |
| `dart-scaler1/mapper-scaler1` | stream | Stream of `dart-scaler1`; stream/resource linkage needs review |
| `dart-scaler1/mapper-scaler-piodma1` | stream | Stream of `dart-scaler1`; stream/resource linkage needs review |
| `ave0` | partial resources | `/video-codec@283100000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-ave0` | partial resources | `/iommu@2830a0000`; `/iommu@283080000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-ave0/mapper-ave0` | stream | Stream of `/iommu@2830a0000` / `/iommu@283080000`; stream/resource linkage needs review |
| `ave1` | gap | **GAP:** no matching hardware resource node |
| `dart-ave1` | gap | **GAP:** no matching hardware resource node |
| `dart-ave1/mapper-ave1` | stream | Stream of `dart-ave1`; stream/resource linkage needs review |
| `apr0` | resources | `/video-codec@301000000` |
| `dart-apr0` | resources | `/iommu@301020000` |
| `dart-apr0/mapper-apr0` | stream | Stream of `/iommu@301020000`; stream/resource linkage needs review |
| `jpeg0` | partial resources | `/jpeg@487000000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-jpeg0` | resources | `/iommu@487020000` |
| `dart-jpeg0/mapper-jpeg0` | stream | Stream of `/iommu@487020000`; stream/resource linkage needs review |
| `jpeg1` | partial resources | `/jpeg@487060000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-jpeg1` | resources | `/iommu@487080000` |
| `dart-jpeg1/mapper-jpeg1` | stream | Stream of `/iommu@487080000`; stream/resource linkage needs review |
| `ane0` | resources | `/ane@484000000` |
| `dart-ane0` | resources | `/iommu@485800000`; `/iommu@485820000`; `/iommu@485840000`; `/ane@484000000` |
| `dart-ane0/mapper-ane0` | stream | Stream of `/iommu@485800000` / `/iommu@485820000` / `/iommu@485840000` / `/ane@484000000`; stream/resource linkage needs review |
| `dart-ane0/mapper-ane0-mpm` | stream | Stream of `/iommu@485800000` / `/iommu@485820000` / `/iommu@485840000` / `/ane@484000000`; stream/resource linkage needs review |
| `avd0` | partial resources | `/avd@280000000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-avd0` | resources | `/iommu@281020000` |
| `dart-avd0/mapper-avd0` | stream | Stream of `/iommu@281020000`; stream/resource linkage needs review |
| `dart-avd0/mapper-avd0-piodma` | stream | Stream of `/iommu@281020000`; stream/resource linkage needs review |
| `dart-avd0/mapper-avd0-adsbuf` | stream | Stream of `/iommu@281020000`; stream/resource linkage needs review |
| `sgx` | partial resources | `/gpu@288000000`; **GAP:** 1 larger or unmatched resource window(s) |
| `gfx-asc` | resources | `/gpu@288000000`; `/mailbox@28a600000` |
| `gfx-asc/iop-gfx-nub0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mapper-gfx-asc` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `sapt` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `iop-audio-controller` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `iop-audio-controller/audio-codec-input` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-codec-input/audio-codec-input` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-codec-output` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-codec-output/audio-codec-output` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-hp` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-hp/audio-hp-mic` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-leap-internal-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-leap-internal-loopback/audio-leap-internal-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-mca-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-mca-loopback/audio-mca-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-speaker` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/audio-speaker/audio-speaker` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/lp-mic-device` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/lp-mic-io-buffer-device` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `iop-audio-controller/siri-listening-device` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `admac-base-ns` | resources | `/dma-controller@513800000`; `/aop@510600000` |
| `admac-leap-ns` | resources | `/dma-controller@513300000`; `/aop@510600000` |
| `smctempsensor0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `cpu-debug-interface` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `dbgCPU` | resources | `/cpu-debug@508700000` |
| `nub-swd0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `pwm1` | resources | `/pwm@429040000` |
| `timesync` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `fillmore` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
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
| `cpus/cpu10` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu11` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu12` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu13` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu14` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu15` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `aliases` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `memory` | Loader-owned runtime memory or reserved-memory allocation; not a static template address. |
| `pram` | Loader-owned runtime memory or reserved-memory allocation; not a static template address. |
| `vram` | Loader-owned runtime memory or reserved-memory allocation; not a static template address. |
| `socd-trace-ram` | Loader-owned runtime memory or reserved-memory allocation; not a static template address. |
| `hibernate` | Loader-owned runtime memory or reserved-memory allocation; not a static template address. |
| `amfm` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `buttons` | **GAP:** reconcile GPIO key and wakeup relationships. |
| `port-usb-c-1` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-usb-c-2` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-usb-c-3` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-hdmi0` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-magsafe3-1` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `backlight` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `backlight/twodbl` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `pcie-sdreader-helper` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `sacm` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `defaults` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `product` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `iboot-syscfg` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `filesystems-props` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `osenvironments` | Boot metadata; no independent hardware device. Subtree properties omitted. |
