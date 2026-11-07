# DT gaps: J514M

## Identity and scope

Board DTS: `arch/arm64/boot/dts/apple/t6034-j514m.dts`.
The hardware resource inventory identifies the T6031 peripheral die. T6034 is the harvested six-channel T6031 variant.

Source: per-board 27.2 firmware hardware-description template. No live dump for this board was available in the supplied live collection. Only hardware identities and resource mappings are recorded here; no tuning or factory data is reproduced.

## Result

This is a resource coverage audit, not a claim of complete board support. 420 arm-io records were examined. Hardware blocks added in this series remain disabled. A resource match does not prove IRQ, clock, power or DMA stream correctness.

**Audit basis:** DTS source analysis; compiled-DTB reconciliation pending.

| Record classification | Count |
| --- | ---: |
| bus device | 7 |
| firmware service | 102 |
| gap | 43 |
| partial resources | 43 |
| resources | 98 |
| shared hardware | 4 |
| stream | 123 |

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
| `dockchannel-mtp` | partial resources | `/soc/fifo@2acb14000`; `/soc/fifo@2acb14000/input@0`; **GAP:** 1 larger or unmatched resource window(s) |
| `dockchannel-mtp/mtp-transport` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-mtp/mtp-transport/multi-touch` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-mtp/mtp-transport/keyboard` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-mtp/mtp-transport/stm` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-mtp/mtp-transport/actuator` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-mtp/mtp-transport/tp-accel` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mtp` | partial resources | `/soc/mtp@2ac400000`; **GAP:** 2 larger or unmatched resource window(s) |
| `mtp/iop-mtp-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-mtp` | resources | `/soc/iommu@2ac808000`; `/soc/filter@2ac80c000` |
| `dart-mtp/mapper-mtp` | stream | Stream of `/soc/iommu@2ac808000` / `/soc/filter@2ac80c000`; stream/resource linkage needs review |
| `mtp-aop-mux` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `i2c1` | resources | `/soc/i2c@391014000` |
| `i2c1/audio-speaker` | bus device | `/soc/i2c@391014000/codec@38` |
| `i2c1/audio-speaker-left-woofer-2` | bus device | `/soc/i2c@391014000/codec@39` |
| `i2c1/audio-speaker-left-tweeter` | bus device | `/soc/i2c@391014000/codec@3a` |
| `i2c2` | resources | `/soc/i2c@391018000` |
| `i2c2/audio-codec-output` | bus device | `/soc/i2c@391018000/codec@4b` |
| `i2c3` | resources | `/soc/i2c@39101c000` |
| `i2c3/audio-speaker-right-woofer-1` | bus device | `/soc/i2c@39101c000/codec@3b` |
| `i2c3/audio-speaker-right-woofer-2` | bus device | `/soc/i2c@39101c000/codec@3c` |
| `i2c3/audio-speaker-right-tweeter` | bus device | `/soc/i2c@39101c000/codec@3d` |
| `i2c4` | resources | `/soc/i2c@391020000` |
| `i2c4/sd-card` | gap | **GAP:** I2C device not described under this bus |
| `i2c6` | resources | `/soc/i2c@391028000` |
| `i2c6/atcrt0` | gap | **GAP:** I2C device not described under this bus |
| `i2c6/atcrt1` | gap | **GAP:** I2C device not described under this bus |
| `i2c6/atcrt2` | gap | **GAP:** I2C device not described under this bus |
| `i2c8` | resources | `/soc/i2c@391030000` |
| `i2c8/pcon0` | gap | **GAP:** I2C device not described under this bus |
| `spi2` | resources | `/soc/spi@391108000` |
| `spi2/mesa` | gap | **GAP:** fingerprint endpoint is not described |
| `spi4` | resources | `/soc/spi@391110000` |
| `spi4/dp855` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `spi4/dp855/lcd-pmicwp` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/dp855/lcd-pmic` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/dp855/lcd-sswp` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/dp855/lcd-ss` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/dp855/tcon-registers` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/dp855/lcd-eeprom` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `spi4/display-temp-sens1` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `spi4/display-temp-sens2` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `spi4/display-temp-sens3` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `isp0` | partial resources | `/soc/isp@38c000000`; `/soc/video-encoder@3c5100000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-isp0` | resources | `/soc/iommu@38e4a8000`; `/soc/iommu@38e4b4000`; `/soc/iommu@38e4bc000`; `/soc/iommu@38e4b0000`; `/soc/iommu@38e4b8000`; `/soc/filter@38e4ac000` |
| `dart-isp0/mapper-isp0` | stream | Stream of `/soc/iommu@38e4a8000` / `/soc/iommu@38e4b4000` / `/soc/iommu@38e4bc000` / `/soc/iommu@38e4b0000` / `/soc/iommu@38e4b8000` / `/soc/filter@38e4ac000`; stream/resource linkage needs review |
| `alc0` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `alc0/audio-leap-mic` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `alc4` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `alc4/audio-leap-internal-loopback` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `admac-aop-audio` | partial resources | `/soc/dma-controller@2a8980000`; **GAP:** 1 larger or unmatched resource window(s) |
| `pwm0` | resources | `/soc/pwm@391040000` |
| `pwm0/kbd-backlight` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apcie0` | partial resources | `/soc/pcie@580000000`; **GAP:** 23 larger or unmatched resource window(s) |
| `apcie0/pci-bridge0` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apcie0/pci-bridge0/wlan` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apcie0/pci-bridge0/bluetooth-pcie` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apcie0/pci-bridge1` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apcie0/pci-bridge1/pcie-sdreader` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-apcie0` | resources | `/soc/iommu@594000000` |
| `dart-apcie0/mapper-apcie0-wlan` | stream | Stream of `/soc/iommu@594000000`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-bt` | stream | Stream of `/soc/iommu@594000000`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-piodma` | stream | Stream of `/soc/iommu@594000000`; stream/resource linkage needs review |
| `apcie0-piodma` | resources | `/soc/dma-controller@594010000` |
| `aod2` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dart-apcie1` | resources | `/soc/iommu@595000000` |
| `dart-apcie1/mapper-apcie1` | stream | Stream of `/soc/iommu@595000000`; stream/resource linkage needs review |
| `dart-apcie1/mapper-apcie1-piodma` | stream | Stream of `/soc/iommu@595000000`; stream/resource linkage needs review |
| `apcie1-piodma` | resources | `/soc/dma-controller@595010000` |
| `apciec0` | gap | **GAP:** no matching hardware resource node |
| `apciec0/pcic0-bridge` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-apciec0` | resources | `/soc/iommu@721008000` |
| `dart-apciec0/mapper-apciec0-piodma` | stream | Stream of `/soc/iommu@721008000`; stream/resource linkage needs review |
| `apciec0-piodma` | resources | `/soc/dma-controller@722000000` |
| `acio0` | partial resources | `/soc/cio@701ac0000/nhi@f00000`; `/soc/cio@701ac0000`; **GAP:** 1 larger or unmatched resource window(s) |
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
| `acio-cpu0` | partial resources | `/soc/mailbox@701108000`; **GAP:** 1 larger or unmatched resource window(s) |
| `acio-cpu0/iop-acio0-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apciec1` | gap | **GAP:** no matching hardware resource node |
| `apciec1/pcic1-bridge` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-apciec1` | resources | `/soc/iommu@b21008000` |
| `dart-apciec1/mapper-apciec1-piodma` | stream | Stream of `/soc/iommu@b21008000`; stream/resource linkage needs review |
| `apciec1-piodma` | resources | `/soc/dma-controller@b22000000` |
| `acio1` | partial resources | `/soc/cio@b01ac0000/nhi@f00000`; `/soc/cio@b01ac0000`; **GAP:** 1 larger or unmatched resource window(s) |
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
| `acio-cpu1` | partial resources | `/soc/mailbox@b01108000`; **GAP:** 1 larger or unmatched resource window(s) |
| `acio-cpu1/iop-acio1-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `apciec2` | gap | **GAP:** no matching hardware resource node |
| `apciec2/pcic2-bridge` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-apciec2` | resources | `/soc/iommu@f21008000` |
| `dart-apciec2/mapper-apciec2-piodma` | stream | Stream of `/soc/iommu@f21008000`; stream/resource linkage needs review |
| `apciec2-piodma` | resources | `/soc/dma-controller@f22000000` |
| `acio2` | partial resources | `/soc/cio@f01ac0000/nhi@f00000`; `/soc/cio@f01ac0000`; **GAP:** 1 larger or unmatched resource window(s) |
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
| `acio-cpu2` | partial resources | `/soc/mailbox@f01108000`; **GAP:** 1 larger or unmatched resource window(s) |
| `acio-cpu2/iop-acio2-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `wlan` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `bluetooth` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `display-crossbar0` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `atc0-dpxbar` | resources | `/soc/mux@70304c000` |
| `atc0-dpphy` | shared hardware | `/soc/phy@703000000` |
| `atc0-dpin0` | resources | `/soc/displayport@701e50000` |
| `atc0-dpin1` | resources | `/soc/displayport@701e58000` |
| `atc1-dpxbar` | resources | `/soc/mux@b0304c000` |
| `atc1-dpphy` | shared hardware | `/soc/phy@b03000000` |
| `atc1-dpin0` | resources | `/soc/displayport@b01e50000` |
| `atc1-dpin1` | resources | `/soc/displayport@b01e58000` |
| `atc2-dpxbar` | resources | `/soc/mux@f0304c000` |
| `atc2-dpphy` | shared hardware | `/soc/phy@f03000000` |
| `atc2-dpin0` | resources | `/soc/displayport@f01e50000` |
| `atc2-dpin1` | resources | `/soc/displayport@f01e58000` |
| `atc3-dpxbar` | resources | `/soc/mux@130304c000` |
| `atc3-dpphy` | shared hardware | `/soc/phy@1303000000` |
| `mcc` | resources | `/soc/pmp_report@2903c0000`; `/soc/memory-controller@2903c0000`; `/soc/power-management@292818000`; `/soc/error-handler@388c40000` |
| `aic` | resources | `/soc/interrupt-controller@292400000` |
| `aic-timebase` | resources | `/soc/interrupt-controller@292400000` |
| `wdt` | partial resources | `/soc/watchdog@2a02d4000`; **GAP:** 4 larger or unmatched resource window(s) |
| `error-handler` | resources | `/soc/error-handler@388c40000`; `/soc/event-log@292800000` |
| `dwi` | resources | `/soc/dwi@2929b0000` |
| `aes` | resources | `/soc/crypto@39100c000` |
| `gpio0` | resources | `/soc/pinctrl@2b3000000` |
| `aop-gpio0` | resources | `/soc/pinctrl@2a8824000` |
| `nub-gpio0` | resources | `/soc/pinctrl@2a01f0000` |
| `smc-gpio0` | resources | `/soc/pinctrl@2a4820000` |
| `aop` | partial resources | `/soc/aop@2a8c00000`; **GAP:** 3 larger or unmatched resource window(s) |
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
| `dart-aop` | resources | `/soc/iommu@2a8818000`; `/soc/filter@2a881c000` |
| `dart-aop/mapper-aop` | stream | Stream of `/soc/iommu@2a8818000` / `/soc/filter@2a881c000`; stream/resource linkage needs review |
| `dart-aop/mapper-aop-admac` | stream | Stream of `/soc/iommu@2a8818000` / `/soc/filter@2a881c000`; stream/resource linkage needs review |
| `dart-aop/mapper-scm` | stream | Stream of `/soc/iommu@2a8818000` / `/soc/filter@2a881c000`; stream/resource linkage needs review |
| `pmgr` | partial resources | `/soc/error-handler@388c40000`; `/soc/event-log@292800000`; `/soc/cpufreq@210e20000`; `/soc/cpufreq@211e20000`; `/soc/cpufreq@212e20000`; `/soc/power-management@292818000`; `/soc/pmp_report@2903c0000`; `/soc/gpu@40a400000`; `/soc/power-management@408e80000`; **GAP:** 39 larger or unmatched resource window(s) |
| `pmgr/soc-tuner` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `pmgr/clpc` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `pmgr/ppm` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `event-log-handler` | resources | `/soc/event-log@292800000`; `/soc/watchdog@2a02d4000` |
| `nco` | resources | `/soc/clock-controller@29003c000` |
| `sio` | resources | `/soc/dma-controller@391c00000` |
| `sio/iop-sio-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `sio/iop-sio-nub/sio-dma` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-sio` | resources | `/soc/iommu@391004000` |
| `dart-sio/mapper-sio` | stream | Stream of `/soc/iommu@391004000`; stream/resource linkage needs review |
| `dart-sio/mapper-aes` | stream | Stream of `/soc/iommu@391004000`; stream/resource linkage needs review |
| `dart-sio/mapper-admac` | stream | Stream of `/soc/iommu@391004000`; stream/resource linkage needs review |
| `pmp0` | partial resources | `/soc/pmp@290700000`; `/soc/power-management@2903d0000`; **GAP:** 10 larger or unmatched resource window(s) |
| `pmp0/iop-pmp0-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-pmp0` | resources | `/soc/iommu@290300000` |
| `dart-pmp0/mapper-pmp0` | stream | Stream of `/soc/iommu@290300000`; stream/resource linkage needs review |
| `sep` | resources | `/soc/sep@38a400000` |
| `sep/iop-sep-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `sep/iop-sep-nub/Ocelot` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `sep/iop-sep-nub/xART` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `sep/iop-sep-nub/InvalidateHmac` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-sep` | resources | `/soc/iommu@388ac0000` |
| `dart-sep/mapper-sep` | stream | Stream of `/soc/iommu@388ac0000`; stream/resource linkage needs review |
| `ans` | partial resources | `/soc/nvme@34dcc0000`; **GAP:** 6 larger or unmatched resource window(s) |
| `ans/iop-ans-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `ans/ans-pcie` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `sart-ans` | resources | `/soc/sart@34dc50000`; `/soc/nvme@34dcc0000` |
| `smc` | partial resources | `/soc/smc@2a4400000`; **GAP:** 3 larger or unmatched resource window(s) |
| `smc/iop-smc-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smc/iop-smc-nub/smc-pmu` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smc/iop-smc-nub/smc-charger-util-0` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smc/iop-smc-nub/smc-charger-util-1` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smc/iop-smc-nub/smc-charger-util-2` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smc/iop-smc-nub/smc-charger-util` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `aon-ptd` | resources | `/soc/interrupt-controller@2a4dc0000` |
| `pmgrtools` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `rsm` | resources | `/soc/rsm@2a0325000` |
| `dart-rsm` | resources | `/soc/iommu@2a033c000` |
| `dart-rsm/mapper-rsm` | stream | Stream of `/soc/iommu@2a033c000`; stream/resource linkage needs review |
| `uart0` | resources | `/soc/serial@391200000` |
| `uart0/debug-console` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dockchannel-uart` | gap | **GAP:** no matching hardware resource node |
| `qspi` | resources | `/soc/spi@391118000` |
| `qspi/spinor` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `qspi/spinor/firmware` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `qspi/spinor/anvram` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `qspi/spinor/syscfg` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dp2hdmi-gpio0` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `admac-sio` | partial resources | `/soc/dma-controller@393200000`; **GAP:** 1 larger or unmatched resource window(s) |
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
| `nub-spmi0` | partial resources | `/soc/spmi@2a1014000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi0/spmi-chapelL1-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi0/btm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi1` | resources | `/soc/spmi@2a1054000` |
| `nub-spmi1/spmi-chapelF1-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi1/spmi-spitfireE1-9` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi2` | resources | `/soc/spmi@2a1094000` |
| `nub-spmi2/spmi-sebringC1-a` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi2/spmi-sebringC4-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi3` | resources | `/soc/spmi@2a10d4000` |
| `nub-spmi3/spmi-sebringC2-a` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi3/spmi-sebringC3-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi4` | resources | `/soc/spmi@2a1114000` |
| `nub-spmi4/spmi-sebringC0-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0` | partial resources | `/soc/spmi@2a1188000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi-a0/hpm0` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0/hpm1` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0/hpm5` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a1` | partial resources | `/soc/spmi@2a1198000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi-a1/hpm2` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a2` | gap | **GAP:** no matching hardware resource node |
| `aop-spmi0` | resources | `/soc/spmi@2a8894000` |
| `aop-spmi0/spmi-wifibt` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1` | resources | `/soc/spmi@2a88d4000` |
| `aop-spmi1/stockholm-spmi` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi/stockholm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi-p1` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi-p1/stockholm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi-juno` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi-juno/stockholm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `disp0` | partial resources | `/soc/iommu@385300000`; `/soc/power-management@2903d0000`; **GAP:** 4 larger or unmatched resource window(s) |
| `dart-disp0` | partial resources | `/soc/iommu@385300000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-disp0/mapper-disp0` | stream | Stream of `/soc/iommu@385300000`; stream/resource linkage needs review |
| `dart-disp0/mapper-disp0-piodma` | stream | Stream of `/soc/iommu@385300000`; stream/resource linkage needs review |
| `dcp0-expert` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dcp` | gap | **GAP:** no matching hardware resource node |
| `dcp/iop-dcp-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-dcp` | gap | **GAP:** no matching hardware resource node |
| `dart-dcp/mapper-dcp` | stream | Stream of `dart-dcp`; stream/resource linkage needs review |
| `dcp0-sac-controller` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `dp-audio0` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `scaler0` | resources | `/soc/scaler@407000000` |
| `dart-scaler0` | resources | `/soc/iommu@407200000` |
| `dart-scaler0/mapper-scaler0` | stream | Stream of `/soc/iommu@407200000`; stream/resource linkage needs review |
| `dart-scaler0/mapper-scaler-piodma0` | stream | Stream of `/soc/iommu@407200000`; stream/resource linkage needs review |
| `scaler1` | resources | `/soc/scaler@3c7000000` |
| `dart-scaler1` | resources | `/soc/iommu@3c7200000` |
| `dart-scaler1/mapper-scaler1` | stream | Stream of `/soc/iommu@3c7200000`; stream/resource linkage needs review |
| `dart-scaler1/mapper-scaler-piodma1` | stream | Stream of `/soc/iommu@3c7200000`; stream/resource linkage needs review |
| `apr0` | resources | `/soc/video-codec@2c5000000` |
| `dart-apr0` | resources | `/soc/iommu@2c5028000`; `/soc/iommu@2c502c000` |
| `dart-apr0/mapper-apr0` | stream | Stream of `/soc/iommu@2c5028000` / `/soc/iommu@2c502c000`; stream/resource linkage needs review |
| `apr1` | resources | `/soc/video-codec@287000000` |
| `dart-apr1` | resources | `/soc/iommu@287008000` |
| `dart-apr1/mapper-apr1` | stream | Stream of `/soc/iommu@287008000`; stream/resource linkage needs review |
| `jpeg0` | resources | `/soc/jpeg@2cf000000` |
| `dart-jpeg0` | resources | `/soc/iommu@2cf004000` |
| `dart-jpeg0/mapper-jpeg0` | stream | Stream of `/soc/iommu@2cf004000`; stream/resource linkage needs review |
| `jpeg1` | resources | `/soc/jpeg@2cf008000` |
| `dart-jpeg1` | resources | `/soc/iommu@2cf00c000` |
| `dart-jpeg1/mapper-jpeg1` | stream | Stream of `/soc/iommu@2cf00c000`; stream/resource linkage needs review |
| `ave0` | resources | `/soc/video-encoder@285100000` |
| `dart-ave0` | resources | `/soc/iommu@285030000`; `/soc/iommu@285040000`; `/soc/iommu@285020000`; `/soc/filter@285044000` |
| `dart-ave0/mapper-ave0` | stream | Stream of `/soc/iommu@285030000` / `/soc/iommu@285040000` / `/soc/iommu@285020000` / `/soc/filter@285044000`; stream/resource linkage needs review |
| `ave1` | resources | `/soc/video-encoder@3c5100000` |
| `dart-ave1` | resources | `/soc/iommu@3c5030000`; `/soc/iommu@3c5040000`; `/soc/iommu@3c5020000`; `/soc/filter@3c5044000` |
| `dart-ave1/mapper-ave1` | stream | Stream of `/soc/iommu@3c5030000` / `/soc/iommu@3c5040000` / `/soc/iommu@3c5020000` / `/soc/filter@3c5044000`; stream/resource linkage needs review |
| `avd0` | partial resources | `/soc/avd@2cc000000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-avd0` | resources | `/soc/iommu@2cd010000` |
| `dart-avd0/mapper-avd0` | stream | Stream of `/soc/iommu@2cd010000`; stream/resource linkage needs review |
| `dart-avd0/mapper-avd0-piodma` | stream | Stream of `/soc/iommu@2cd010000`; stream/resource linkage needs review |
| `dart-avd0/mapper-avd0-adsbuf` | stream | Stream of `/soc/iommu@2cd010000`; stream/resource linkage needs review |
| `ane0` | partial resources | `/soc/filter@3c9804000`; `/soc/video-encoder@3c5100000`; `/soc/pmp_report@2903c0000`; **GAP:** 7 larger or unmatched resource window(s) |
| `ane0/iop-ane0-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-ane0` | partial resources | `/soc/filter@3c9804000`; **GAP:** 3 larger or unmatched resource window(s) |
| `dart-ane0/mapper-ane0` | stream | Stream of `/soc/filter@3c9804000`; stream/resource linkage needs review |
| `sgx` | partial resources | `/soc/gpu@40a400000`; **GAP:** 1 larger or unmatched resource window(s) |
| `gfx-asc` | resources | `/soc/gpu@40a400000` |
| `gfx-asc/iop-gfx-nub0` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mapper-gfx-asc` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `mca-switch` | partial resources | `/soc/mca@393400000`; **GAP:** 1 larger or unmatched resource window(s) |
| `mca0` | resources | `/soc/mca@393400000` |
| `mca0/mca0a` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca0/mca0a/audio-speaker` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca1` | resources | `/soc/mca@393400000` |
| `mca1/mca1a` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca1/mca1a/audio-loopback` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca2` | resources | `/soc/mca@393400000` |
| `mca2/mca2a` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca2/mca2a/audio-codec-output` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca2/mca2b` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `mca2/mca2b/audio-codec-input` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `smctempsensor0` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `cpu-debug-interface` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `nub-swd0` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `pwm1` | resources | `/soc/pwm@391040000` |
| `timesync` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `fillmore` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dispext0` | partial resources | `/soc/dcp@306c00000`; `/soc/power-management@2903d0000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-dispext0` | resources | `/soc/iommu@305304000`; `/soc/iommu@305300000` |
| `dart-dispext0/mapper-dispext0` | stream | Stream of `/soc/iommu@305304000` / `/soc/iommu@305300000`; stream/resource linkage needs review |
| `dart-dispext0/mapper-dispext0-piodma` | stream | Stream of `/soc/iommu@305304000` / `/soc/iommu@305300000`; stream/resource linkage needs review |
| `dcpext0-expert` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dcpext0` | partial resources | `/soc/dcp@306c00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcpext0/iop-dcpext0-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-dcpext0` | resources | `/soc/iommu@30530c000` |
| `dart-dcpext0/mapper-dcpext0` | stream | Stream of `/soc/iommu@30530c000`; stream/resource linkage needs review |
| `dp-audio1` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dispext1` | partial resources | `/soc/dcp@44ac00000`; `/soc/power-management@2903d0000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-dispext1` | resources | `/soc/iommu@449304000`; `/soc/iommu@449300000` |
| `dart-dispext1/mapper-dispext1` | stream | Stream of `/soc/iommu@449304000` / `/soc/iommu@449300000`; stream/resource linkage needs review |
| `dart-dispext1/mapper-dispext1-piodma` | stream | Stream of `/soc/iommu@449304000` / `/soc/iommu@449300000`; stream/resource linkage needs review |
| `dcpext1-expert` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dcpext1` | partial resources | `/soc/dcp@44ac00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcpext1/iop-dcpext1-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-dcpext1` | resources | `/soc/iommu@44930c000` |
| `dart-dcpext1/mapper-dcpext1` | stream | Stream of `/soc/iommu@44930c000`; stream/resource linkage needs review |
| `dp-audio2` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dispext2` | partial resources | `/soc/dcp@44ec00000`; `/soc/power-management@2903d0000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-dispext2` | resources | `/soc/iommu@44d304000`; `/soc/iommu@44d300000` |
| `dart-dispext2/mapper-dispext2` | stream | Stream of `/soc/iommu@44d304000` / `/soc/iommu@44d300000`; stream/resource linkage needs review |
| `dart-dispext2/mapper-dispext2-piodma` | stream | Stream of `/soc/iommu@44d304000` / `/soc/iommu@44d300000`; stream/resource linkage needs review |
| `dcpext2-expert` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dcpext2` | partial resources | `/soc/dcp@44ec00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcpext2/iop-dcpext2-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-dcpext2` | resources | `/soc/iommu@44d30c000` |
| `dart-dcpext2/mapper-dcpext2` | stream | Stream of `/soc/iommu@44d30c000`; stream/resource linkage needs review |
| `dp-audio3` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dispext3` | partial resources | `/soc/dcp@2cac00000`; `/soc/power-management@2903d0000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-dispext3` | resources | `/soc/iommu@2c9304000`; `/soc/iommu@2c9300000` |
| `dart-dispext3/mapper-dispext3` | stream | Stream of `/soc/iommu@2c9304000` / `/soc/iommu@2c9300000`; stream/resource linkage needs review |
| `dart-dispext3/mapper-dispext3-piodma` | stream | Stream of `/soc/iommu@2c9304000` / `/soc/iommu@2c9300000`; stream/resource linkage needs review |
| `dcpext3-expert` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `dcpext3` | partial resources | `/soc/dcp@2cac00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcpext3/iop-dcpext3-nub` | firmware service | Subordinate firmware service/configuration; no independent decoded MMIO resource |
| `dart-dcpext3` | resources | `/soc/iommu@2c930c000` |
| `dart-dcpext3/mapper-dcpext3` | stream | Stream of `/soc/iommu@2c930c000`; stream/resource linkage needs review |
| `dp-audio4` | firmware service | Firmware service/policy or aggregate; no independent MMIO resource in this record |
| `apple-processor-trace` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
