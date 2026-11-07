# DT gaps: J516S

## Identity and scope

Board DTS: `arch/arm64/boot/dts/apple/t6030-j516s.dts`.
The hardware resource inventory identifies the T6030 peripheral die.

Source: per-board 27.2 firmware hardware-description template. No live dump for this board was available in the supplied live collection. Only hardware identities and resource mappings are recorded here; no tuning or factory data is reproduced.

## Result

This is a resource coverage audit, not a claim of complete board support. 380 arm-io records were examined. Hardware blocks added in this series remain disabled. A resource match does not prove IRQ, clock, power or DMA stream correctness.

**Audit basis:** DTS source analysis; compiled-DTB reconciliation pending.

| Record classification | Count |
| --- | ---: |
| bus device | 7 |
| firmware service | 92 |
| gap | 33 |
| partial resources | 39 |
| resources | 89 |
| shared hardware | 7 |
| stream | 113 |

## Remaining work

- Close every explicit GAP below, including nested endpoints and shared resource ownership.
- Reconcile larger firmware resource windows with the driver-specific Linux register layout; do not expand an active device aperture without driver review.
- Bindings and resource presence do not supply per-device calibration, firmware or an initialization contract.
- Validate this board with `W=1` and `dtbs_check`, then perform a separately approved boot before enabling newly described engines.

## Dependencies

- linux PR #1031: existing M3 DTS cleanup, kept separate from this resource expansion.
- linux PR #1033 and #1086: binding and M3 DTS updates already present in the integration base.

## Every arm-io record

Paths in the first column are relative to `arm-io`. Stream entries name the owning translation block; their SID and aperture semantics still need review. Firmware aggregate/service entries are retained for audit visibility and are not silently counted as separate physical devices.

| Firmware hardware/service record | Classification | Linux description or gap |
| --- | --- | --- |
| `spi2` | resources | `/soc/spi@289108000` |
| `spi2/mesa` | gap | **GAP:** fingerprint endpoint is not described |
| `spi4` | resources | `/soc/spi@289110000` |
| `spi4/dp855` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `spi4/dp855/lcd-pmicwp` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/dp855/lcd-pmic` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/dp855/lcd-sswp` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/dp855/lcd-ss` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/dp855/tcon-registers` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/dp855/lcd-eeprom` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `i2c1` | resources | `/soc/i2c@289014000` |
| `i2c1/audio-speaker` | bus device | `/soc/i2c@289014000/codec@38` |
| `i2c1/audio-speaker-left-tweeter` | bus device | `/soc/i2c@289014000/codec@3a` |
| `i2c1/audio-speaker-left-woofer-2` | bus device | `/soc/i2c@289014000/codec@39` |
| `i2c2` | resources | `/soc/i2c@289018000` |
| `i2c2/audio-codec-output` | bus device | `/soc/i2c@289018000/codec@4b` |
| `i2c3` | resources | `/soc/i2c@28901c000` |
| `i2c3/audio-speaker-right-woofer-1` | bus device | `/soc/i2c@28901c000/codec@3b` |
| `i2c3/audio-speaker-right-tweeter` | bus device | `/soc/i2c@28901c000/codec@3d` |
| `i2c3/audio-speaker-right-woofer-2` | bus device | `/soc/i2c@28901c000/codec@3c` |
| `i2c4` | resources | `/soc/i2c@289020000` |
| `i2c4/sd-card` | gap | **GAP:** I2C device not described under this bus |
| `i2c6` | resources | `/soc/i2c@289028000` |
| `i2c6/atcrt0` | gap | **GAP:** I2C device not described under this bus |
| `i2c6/atcrt1` | gap | **GAP:** I2C device not described under this bus |
| `i2c6/atcrt2` | gap | **GAP:** I2C device not described under this bus |
| `i2c8` | resources | `/soc/i2c@289030000` |
| `i2c8/pcon0` | gap | **GAP:** I2C device not described under this bus |
| `pwm0` | resources | `/soc/pwm@289040000` |
| `pwm0/kbd-backlight` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `alc0` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `alc0/audio-leap-mic` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `alc4` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `alc4/audio-leap-internal-loopback` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-apcie1` | resources | `/soc/iommu@595000000` |
| `dart-apcie1/mapper-apcie1` | stream | Stream of `/soc/iommu@595000000`; stream/resource linkage needs review |
| `dart-apcie1/mapper-apcie1-piodma` | stream | Stream of `/soc/iommu@595000000`; stream/resource linkage needs review |
| `apcie1-piodma` | resources | `/soc/dma-controller@595010000` |
| `aod2` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `wlan` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `uart2` | resources | `/soc/serial@289208000` |
| `uart2/wlan-debug` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `bluetooth` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `apcie` | partial resources | `/soc/pcie@580000000`; **GAP:** 18 larger or unmatched resource window(s) |
| `apcie/pci-bridge0` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apcie/pci-bridge0/wlan` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apcie/pci-bridge0/bluetooth-pcie` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apcie/pci-bridge1` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apcie/pci-bridge1/pcie-sdreader` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-apcie0` | resources | `/soc/iommu@594000000` |
| `dart-apcie0/mapper-apcie0-wlan` | stream | Stream of `/soc/iommu@594000000`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-bt` | stream | Stream of `/soc/iommu@594000000`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-piodma` | stream | Stream of `/soc/iommu@594000000`; stream/resource linkage needs review |
| `apcie0-piodma` | resources | `/soc/dma-controller@594010000` |
| `acio-cpu0` | resources | `/soc/mailbox@701108000`; `/soc/cio@701ac0000` |
| `acio-cpu0/iop-acio0-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apciec0` | resources | `/usb4-pcie-tunnel-0/pcie@730000000` |
| `apciec0/pcic0-bridge` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-apciec0` | resources | `/usb4-pcie-tunnel-0/iommu@721008000` |
| `dart-apciec0/mapper-apciec0-piodma` | stream | Stream of `/usb4-pcie-tunnel-0/iommu@721008000`; stream/resource linkage needs review |
| `apciec0-piodma` | resources | `/soc/dma-controller@722000000` |
| `acio0` | resources | `/soc/cio@701ac0000/nhi@f00000`; `/soc/cio@701ac0000` |
| `dart-acio0` | resources | `/soc/cio@701ac0000/iommu@a80000` |
| `dart-acio0/mapper-acio0-tx-0` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-1` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-2` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-3` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-4` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-5` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-6` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-7` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-8` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-9` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-10` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-11` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-0` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-1` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-2` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-3` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-4` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-5` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-6` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-7` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-8` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-9` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-10` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-11` | stream | Stream of `/soc/cio@701ac0000/iommu@a80000`; stream/resource linkage needs review |
| `atc0-dpxbar` | resources | `/soc/mux@70304c000` |
| `atc0-dpphy` | shared hardware | `/soc/phy@703000000` |
| `atc0-dpin0` | resources | `/soc/displayport@701e50000` |
| `atc0-dpin1` | resources | `/soc/displayport@701e58000` |
| `acio-cpu1` | resources | `/soc/mailbox@b01108000`; `/soc/cio@b01ac0000` |
| `acio-cpu1/iop-acio1-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apciec1` | gap | **GAP:** no matching hardware resource node |
| `apciec1/pcic1-bridge` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-apciec1` | resources | `/soc/iommu@b21008000` |
| `dart-apciec1/mapper-apciec1-piodma` | stream | Stream of `/soc/iommu@b21008000`; stream/resource linkage needs review |
| `apciec1-piodma` | resources | `/soc/dma-controller@b22000000` |
| `acio1` | resources | `/soc/cio@b01ac0000/nhi@f00000`; `/soc/cio@b01ac0000` |
| `dart-acio1` | resources | `/soc/cio@b01ac0000/iommu@a80000` |
| `dart-acio1/mapper-acio1-tx-0` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-1` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-2` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-3` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-4` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-5` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-6` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-7` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-8` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-9` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-10` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-11` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-0` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-1` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-2` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-3` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-4` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-5` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-6` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-7` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-8` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-9` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-10` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-11` | stream | Stream of `/soc/cio@b01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `atc1-dpxbar` | resources | `/soc/mux@b0304c000` |
| `atc1-dpphy` | shared hardware | `/soc/phy@b03000000` |
| `atc1-dpin0` | resources | `/soc/displayport@b01e50000` |
| `atc1-dpin1` | resources | `/soc/displayport@b01e58000` |
| `acio-cpu2` | resources | `/soc/mailbox@f01108000`; `/soc/cio@f01ac0000` |
| `acio-cpu2/iop-acio2-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apciec2` | gap | **GAP:** no matching hardware resource node |
| `apciec2/pcic2-bridge` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-apciec2` | resources | `/soc/iommu@f21008000` |
| `dart-apciec2/mapper-apciec2-piodma` | stream | Stream of `/soc/iommu@f21008000`; stream/resource linkage needs review |
| `apciec2-piodma` | resources | `/soc/dma-controller@f22000000` |
| `acio2` | resources | `/soc/cio@f01ac0000/nhi@f00000`; `/soc/cio@f01ac0000` |
| `dart-acio2` | resources | `/soc/cio@f01ac0000/iommu@a80000` |
| `dart-acio2/mapper-acio2-tx-0` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-1` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-2` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-3` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-4` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-5` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-6` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-7` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-8` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-9` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-10` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-11` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-0` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-1` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-2` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-3` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-4` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-5` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-6` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-7` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-8` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-9` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-10` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-11` | stream | Stream of `/soc/cio@f01ac0000/iommu@a80000`; stream/resource linkage needs review |
| `atc2-dpxbar` | resources | `/soc/mux@f0304c000` |
| `atc2-dpphy` | shared hardware | `/soc/phy@f03000000` |
| `atc2-dpin0` | resources | `/soc/displayport@f01e50000` |
| `atc2-dpin1` | resources | `/soc/displayport@f01e58000` |
| `atc3-dpxbar` | resources | `/soc/mux@130304c000` |
| `atc3-dpphy` | shared hardware | `/soc/phy@1303000000` |
| `display-crossbar0` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `mcc` | resources | `/soc/pmp_report@3503c0000`; `/soc/memory-controller@3503c0000`; `/soc/power-management@350780000`; `/soc/error-handler@29cc40000` |
| `aic` | resources | `/soc/interrupt-controller@351000000` |
| `aic-timebase` | resources | `/soc/interrupt-controller@351000000` |
| `wdt` | partial resources | `/soc/watchdog@3642b0000`; **GAP:** 4 larger or unmatched resource window(s) |
| `error-handler` | resources | `/soc/error-handler@29cc40000`; `/soc/event-log@350700000` |
| `dwi` | resources | `/soc/dwi@350200000` |
| `pwm` | resources | `/soc/pwm@289040000` |
| `aes` | resources | `/soc/crypto@28900c000` |
| `gpio0` | partial resources | `/soc/pinctrl@347100000`; **GAP:** 1 larger or unmatched resource window(s) |
| `aop-gpio` | resources | `/soc/pinctrl@374824000` |
| `nub-gpio` | resources | `/soc/pinctrl@3641f0000` |
| `smc-gpio` | resources | `/soc/pinctrl@36c820000` |
| `aop` | partial resources | `/soc/aop@374c00000`; **GAP:** 2 larger or unmatched resource window(s) |
| `aop/iop-aop-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `aop/iop-aop-nub/aop-audio` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `aop/iop-aop-nub/aop-audio/audio-pdm2` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `aop/iop-aop-nub/aop-audio/hfdc-2400000` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `aop/iop-aop-nub/aop-audio/audio-hp` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `aop/iop-aop-nub/aop-audio/audio-lp-mic-in` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `aop/iop-aop-nub/aop-audio/audio-leap-internal-loopback` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `aop/iop-aop-nub/als` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `aop/iop-aop-nub/accel` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `aop/iop-aop-nub/gyro` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-aop` | resources | `/soc/iommu@374818000`; `/soc/filter@37481c000` |
| `dart-aop/mapper-aop` | stream | Stream of `/soc/iommu@374818000` / `/soc/filter@37481c000`; stream/resource linkage needs review |
| `dart-aop/mapper-aop-admac` | stream | Stream of `/soc/iommu@374818000` / `/soc/filter@37481c000`; stream/resource linkage needs review |
| `dart-aop/mapper-scm` | stream | Stream of `/soc/iommu@374818000` / `/soc/filter@37481c000`; stream/resource linkage needs review |
| `pmgr` | partial resources | `/soc/event-log@350700000`; `/soc/error-handler@29cc40000`; `/soc/cpufreq@210e20000`; `/soc/cpufreq@211e20000`; `/soc/power-management@350780000`; `/soc/pmp_report@3503c0000`; `/soc/phy@703000000`; `/soc/phy@b03000000`; `/soc/phy@f03000000`; `/soc/video-encoder@2cd100000`; `/soc/isp@2c8000000`; `/soc/scaler@391000000`; `/soc/video-codec@30d000000`; **GAP:** 46 larger or unmatched resource window(s) |
| `pmgr/clpc` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `pmgr/soc-tuner` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `pmgr/ppm` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `nco` | resources | `/soc/clock-controller@350044000` |
| `event-log-handler` | resources | `/soc/event-log@350700000` |
| `pmp` | partial resources | `/soc/pmp@350500000`; `/soc/dcp@28ec00000`; **GAP:** 3 larger or unmatched resource window(s) |
| `pmp/iop-pmp-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-pmp` | resources | `/soc/iommu@350300000` |
| `dart-pmp/mapper-pmp` | stream | Stream of `/soc/iommu@350300000`; stream/resource linkage needs review |
| `sep` | resources | `/soc/sep@29e400000` |
| `sep/iop-sep-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `sep/iop-sep-nub/Ocelot` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `sep/iop-sep-nub/xART` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `sep/iop-sep-nub/InvalidateHmac` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-sep` | resources | `/soc/iommu@29cac0000` |
| `dart-sep/mapper-sep` | stream | Stream of `/soc/iommu@29cac0000`; stream/resource linkage needs review |
| `sio` | partial resources | `/soc/dma-controller@289c00000`; **GAP:** 2 larger or unmatched resource window(s) |
| `sio/iop-sio-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `sio/iop-sio-nub/sio-dma` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-sio` | resources | `/soc/iommu@289004000` |
| `dart-sio/mapper-sio` | stream | Stream of `/soc/iommu@289004000`; stream/resource linkage needs review |
| `dart-sio/mapper-aes` | stream | Stream of `/soc/iommu@289004000`; stream/resource linkage needs review |
| `dart-sio/mapper-admac` | stream | Stream of `/soc/iommu@289004000`; stream/resource linkage needs review |
| `ans` | partial resources | `/soc/nvme@38dcc0000`; **GAP:** 7 larger or unmatched resource window(s) |
| `ans/iop-ans-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `sart-ans` | resources | `/soc/sart@38dc50000`; `/soc/nvme@38dcc0000` |
| `smc` | partial resources | `/soc/smc@36c400000`; **GAP:** 3 larger or unmatched resource window(s) |
| `smc/iop-smc-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smc/iop-smc-nub/smc-pmu` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smc/iop-smc-nub/smc-charger-util-0` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smc/iop-smc-nub/smc-charger-util-1` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smc/iop-smc-nub/smc-charger-util-2` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smc/iop-smc-nub/smc-charger-util` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `pmgrtools` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `rsm` | resources | `/soc/rsm@364325000` |
| `dart-rsm` | resources | `/soc/iommu@36433c000` |
| `dart-rsm/mapper-rsm` | stream | Stream of `/soc/iommu@36433c000`; stream/resource linkage needs review |
| `uart0` | partial resources | `/soc/serial@289200000`; **GAP:** 1 larger or unmatched resource window(s) |
| `uart0/debug-console` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-uart` | gap | **GAP:** no matching hardware resource node |
| `dockchannel-mtp` | partial resources | `/soc/fifo@37ab30000`; `/soc/fifo@37ab30000/input@0`; **GAP:** 1 larger or unmatched resource window(s) |
| `dockchannel-mtp/mtp-transport` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-mtp/mtp-transport/multi-touch` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-mtp/mtp-transport/keyboard` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-mtp/mtp-transport/stm` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-mtp/mtp-transport/actuator` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-mtp/mtp-transport/tp-accel` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mtp` | partial resources | `/soc/mtp@37a400000`; **GAP:** 2 larger or unmatched resource window(s) |
| `mtp/iop-mtp-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-mtp` | resources | `/soc/iommu@37a808000`; `/soc/filter@37a80c000` |
| `dart-mtp/mapper-mtp` | stream | Stream of `/soc/iommu@37a808000` / `/soc/filter@37a80c000`; stream/resource linkage needs review |
| `mtp-aop-mux` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `spi1` | resources | `/soc/spi@289104000` |
| `spi1/spinor` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `spi1/spinor/firmware` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi1/spinor/anvram` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi1/spinor/syscfg` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dp2hdmi-gpio0` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `admac-sio` | partial resources | `/soc/dma-controller@28b200000`; **GAP:** 1 larger or unmatched resource window(s) |
| `admac-aop-audio` | partial resources | `/soc/dma-controller@374980000`; **GAP:** 1 larger or unmatched resource window(s) |
| `atc-phy0` | partial resources | `/soc/phy@703000000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-usb0` | resources | `/soc/iommu@702f00000`; `/soc/iommu@702f80000` |
| `dart-usb0/mapper-usb0` | stream | Stream of `/soc/iommu@702f00000` / `/soc/iommu@702f80000`; stream/resource linkage needs review |
| `usb-drd0` | partial resources | `/soc/usb@702280000`; `/soc/phy@703000000`; **GAP:** 8 larger or unmatched resource window(s) |
| `usb-drd0/usb-drd0-port-hs` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `usb-drd0/usb-drd0-port-ss` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `usb-otg0` | resources | `/soc/usb@702100000`; `/soc/phy@703000000` |
| `atc-phy1` | partial resources | `/soc/phy@b03000000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-usb1` | resources | `/soc/iommu@b02f00000`; `/soc/iommu@b02f80000` |
| `dart-usb1/mapper-usb1` | stream | Stream of `/soc/iommu@b02f00000` / `/soc/iommu@b02f80000`; stream/resource linkage needs review |
| `usb-drd1` | partial resources | `/soc/usb@b02280000`; `/soc/phy@b03000000`; **GAP:** 8 larger or unmatched resource window(s) |
| `usb-drd1/usb-drd1-port-hs` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `usb-drd1/usb-drd1-port-ss` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `atc-phy2` | partial resources | `/soc/phy@f03000000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-usb2` | resources | `/soc/iommu@f02f00000`; `/soc/iommu@f02f80000` |
| `dart-usb2/mapper-usb2` | stream | Stream of `/soc/iommu@f02f00000` / `/soc/iommu@f02f80000`; stream/resource linkage needs review |
| `usb-drd2` | partial resources | `/soc/usb@f02280000`; `/soc/phy@f03000000`; **GAP:** 8 larger or unmatched resource window(s) |
| `usb-drd2/usb-drd2-port-hs` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `usb-drd2/usb-drd2-port-ss` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `atc-phy3` | partial resources | `/soc/phy@1303000000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi0` | partial resources | `/soc/spmi@364714000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi0/spmi-chapelL1` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi0/spmi-spitfireE1` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi0/btm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi1` | resources | `/soc/spmi@364794000` |
| `nub-spmi1/spmi-chapelF1` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi1/spmi-spitfireE2` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi2` | resources | `/soc/spmi@364814000` |
| `nub-spmi-a0` | partial resources | `/soc/spmi@364908000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi-a0/hpm0` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0/hpm1` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0/hpm5` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a1` | partial resources | `/soc/spmi@364a08000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi-a1/hpm2` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi0` | resources | `/soc/spmi@374894000` |
| `aop-spmi0/spmi-wifibt` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1` | resources | `/soc/spmi@3748d4000` |
| `aop-spmi1/stockholm-spmi` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi/stockholm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi-p1` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi-p1/stockholm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi-juno` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi-juno/stockholm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `lcd0-sac` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `disp0` | partial resources | `/soc/dcp@28ec00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcp0-expert` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dcp` | partial resources | `/soc/dcp@28ec00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcp/iop-dcp-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-dcp` | resources | `/soc/iommu@28d30c000` |
| `dart-dcp/mapper-dcp` | stream | Stream of `/soc/iommu@28d30c000`; stream/resource linkage needs review |
| `dart-disp0` | resources | `/soc/iommu@28d304000`; `/soc/iommu@28d300000` |
| `dart-disp0/mapper-disp0` | stream | Stream of `/soc/iommu@28d304000` / `/soc/iommu@28d300000`; stream/resource linkage needs review |
| `dart-disp0/mapper-disp0-piodma` | stream | Stream of `/soc/iommu@28d304000` / `/soc/iommu@28d300000`; stream/resource linkage needs review |
| `dcp-sac-controller` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `dp-audio0` | shared hardware | `/audio-controller-2`; `/audio-controller-1` |
| `dispext0` | partial resources | `/soc/dcp@2d2c00000`; `/soc/pmp_report@3503c0000`; **GAP:** 5 larger or unmatched resource window(s) |
| `dcpext0-expert` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dcpext0` | partial resources | `/soc/dcp@2d2c00000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dcpext0/iop-dcpext0-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-dcpext0` | resources | `/soc/iommu@2d130c000` |
| `dart-dcpext0/mapper-dcpext0` | stream | Stream of `/soc/iommu@2d130c000`; stream/resource linkage needs review |
| `dart-dispext0` | partial resources | `/soc/iommu@2d1300000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-dispext0/mapper-dispext0` | stream | Stream of `/soc/iommu@2d1300000`; stream/resource linkage needs review |
| `dart-dispext0/mapper-dispext0-piodma` | stream | Stream of `/soc/iommu@2d1300000`; stream/resource linkage needs review |
| `dp-audio1` | shared hardware | `/audio-controller-2`; `/audio-controller-1` |
| `dispext1` | partial resources | `/soc/dcp@2d6c00000`; `/soc/pmp_report@3503c0000`; **GAP:** 5 larger or unmatched resource window(s) |
| `dart-dispext1` | partial resources | `/soc/iommu@2d5300000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-dispext1/mapper-dispext1` | stream | Stream of `/soc/iommu@2d5300000`; stream/resource linkage needs review |
| `dart-dispext1/mapper-dispext1-piodma` | stream | Stream of `/soc/iommu@2d5300000`; stream/resource linkage needs review |
| `dcpext1-expert` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dcpext1` | partial resources | `/soc/dcp@2d6c00000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dcpext1/iop-dcpext1-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-dcpext1` | resources | `/soc/iommu@2d530c000` |
| `dart-dcpext1/mapper-dcpext1` | stream | Stream of `/soc/iommu@2d530c000`; stream/resource linkage needs review |
| `dp-audio2` | shared hardware | `/audio-controller-2`; `/audio-controller-1` |
| `scaler0` | resources | `/soc/scaler@391000000` |
| `dart-scaler` | resources | `/soc/iommu@391200000` |
| `dart-scaler/mapper-scaler` | stream | Stream of `/soc/iommu@391200000`; stream/resource linkage needs review |
| `dart-scaler/mapper-scaler-piodma` | stream | Stream of `/soc/iommu@391200000`; stream/resource linkage needs review |
| `jpeg0` | resources | `/soc/jpeg@299000000` |
| `dart-jpeg0` | resources | `/soc/iommu@299004000` |
| `dart-jpeg0/mapper-jpeg0` | stream | Stream of `/soc/iommu@299004000`; stream/resource linkage needs review |
| `jpeg1` | resources | `/soc/jpeg@299008000` |
| `dart-jpeg1` | resources | `/soc/iommu@29900c000` |
| `dart-jpeg1/mapper-jpeg1` | stream | Stream of `/soc/iommu@29900c000`; stream/resource linkage needs review |
| `ave` | resources | `/soc/video-encoder@2cd100000` |
| `dart-ave` | resources | `/soc/iommu@2cd030000`; `/soc/iommu@2cd040000`; `/soc/iommu@2cd020000`; `/soc/filter@2cd044000` |
| `dart-ave/mapper-ave` | stream | Stream of `/soc/iommu@2cd030000` / `/soc/iommu@2cd040000` / `/soc/iommu@2cd020000` / `/soc/filter@2cd044000`; stream/resource linkage needs review |
| `avd` | partial resources | `/soc/avd@30a000000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-avd` | resources | `/soc/iommu@30b010000` |
| `dart-avd/mapper-avd` | stream | Stream of `/soc/iommu@30b010000`; stream/resource linkage needs review |
| `dart-avd/mapper-avd-piodma` | stream | Stream of `/soc/iommu@30b010000`; stream/resource linkage needs review |
| `dart-avd/mapper-avd-adsbuf` | stream | Stream of `/soc/iommu@30b010000`; stream/resource linkage needs review |
| `apr` | resources | `/soc/video-codec@30d000000` |
| `dart-apr` | resources | `/soc/iommu@30d028000`; `/soc/iommu@30d02c000` |
| `dart-apr/mapper-apr` | stream | Stream of `/soc/iommu@30d028000` / `/soc/iommu@30d02c000`; stream/resource linkage needs review |
| `isp` | partial resources | `/soc/isp@2c8000000`; `/soc/video-encoder@2cd100000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-isp` | resources | `/soc/iommu@2ca4a8000`; `/soc/iommu@2ca4b4000`; `/soc/iommu@2ca4bc000`; `/soc/iommu@2ca4b0000`; `/soc/iommu@2ca4b8000`; `/soc/filter@2ca4ac000` |
| `dart-isp/mapper-isp` | stream | Stream of `/soc/iommu@2ca4a8000` / `/soc/iommu@2ca4b4000` / `/soc/iommu@2ca4bc000` / `/soc/iommu@2ca4b0000` / `/soc/iommu@2ca4b8000` / `/soc/filter@2ca4ac000`; stream/resource linkage needs review |
| `ane` | partial resources | `/soc/filter@309804000`; `/soc/video-encoder@2cd100000`; `/soc/pmp_report@3503c0000`; `/soc/cpufreq@211e20000`; **GAP:** 8 larger or unmatched resource window(s) |
| `ane/iop-ane-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-ane` | partial resources | `/soc/filter@309804000`; **GAP:** 3 larger or unmatched resource window(s) |
| `dart-ane/mapper-ane` | stream | Stream of `/soc/filter@309804000`; stream/resource linkage needs review |
| `sgx` | partial resources | `/soc/gpu@292400000`; **GAP:** 1 larger or unmatched resource window(s) |
| `gfx-asc` | partial resources | `/soc/gpu@292400000`; **GAP:** 2 larger or unmatched resource window(s) |
| `gfx-asc/iop-gfx-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mapper-gfx-asc` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `mca-switch` | partial resources | `/soc/mca@28b400000`; **GAP:** 1 larger or unmatched resource window(s) |
| `mca0` | resources | `/soc/mca@28b400000` |
| `mca0/mca0a` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca0/mca0a/audio-speaker` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca1` | resources | `/soc/mca@28b400000` |
| `mca1/mca1a` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca1/mca1a/audio-loopback` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca2` | resources | `/soc/mca@28b400000` |
| `mca2/mca2a` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca2/mca2a/audio-codec-output` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca2/mca2b` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca2/mca2b/audio-codec-input` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smctempsensor0` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `cpu-debug-interface` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `timesync` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `fillmore` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `apple-processor-trace` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
