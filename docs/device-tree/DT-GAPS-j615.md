# DT gaps: J615

## Identity and scope

Board DTS: `arch/arm64/boot/dts/apple/t8122-j615.dts`.
The hardware resource inventory identifies the T8122 peripheral die.

Source: per-board 27.2 firmware hardware-description template. No live dump for this board was available in the supplied live collection. Only hardware identities and resource mappings are recorded here; no tuning or factory data is reproduced.

## Result

This is a resource coverage audit, not a claim of complete board support. 300 arm-io records were examined. Hardware blocks added in this series remain disabled. A resource match does not prove IRQ, clock, power or DMA stream correctness.

**Audit basis:** DTS source analysis; compiled-DTB reconciliation pending.

| Record classification | Count |
| --- | ---: |
| bus device | 13 |
| gap | 18 |
| partial resources | 33 |
| resources | 71 |
| service review | 79 |
| shared hardware | 4 |
| stream | 82 |

## Remaining work

- Close every explicit GAP and service-review entry below, including nested endpoints and shared resource ownership.
- Some template SPMI records are alternative endpoints at the same address. Runtime model selection is needed before representing these as simultaneous devices.
- Reconcile larger firmware resource windows with the driver-specific Linux register layout; do not expand an active device aperture without driver review.
- Bindings and resource presence do not supply per-device calibration, firmware or an initialization contract.
- Validate this board with `W=1` and `dtbs_check`, then perform a separately approved boot before enabling newly described engines.

## Dependencies

- linux PR #1031: existing M3 DTS cleanup, kept separate from this resource expansion.
- linux PR #1033 and #1086: binding and M3 DTS updates already present in the integration base.

## Every arm-io record

Paths in the first column are relative to `arm-io`. Stream entries name the owning translation block; their SID and aperture semantics still need review. Service-review entries have no independently decoded MMIO resource; this is not a waiver of their hardware or binding requirements.

| Firmware hardware/service record | Classification | Linux description or gap |
| --- | --- | --- |
| `spi2` | resources | `/soc/spi@2a1108000` |
| `spi2/mesa` | bus device | `/soc/spi@2a1108000/fingerprint@0` |
| `spi4` | resources | `/soc/spi@2a1110000` |
| `spi4/dp855` | gap | **GAP:** nested resource requires parent-specific address decoding |
| `spi4/dp855/lcd-pmicwp` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-pmic` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-sswp` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/tcon-registers` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `spi4/dp855/lcd-eeprom` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c1` | resources | `/soc/i2c@2a1014000` |
| `i2c1/audio-speaker` | bus device | `/soc/i2c@2a1014000/codec@38` |
| `i2c1/audio-speaker-left-woofer-2` | bus device | `/soc/i2c@2a1014000/codec@3a` |
| `i2c1/audio-speaker-left-tweeter` | bus device | `/soc/i2c@2a1014000/codec@39` |
| `i2c3` | resources | `/soc/i2c@2a101c000` |
| `i2c3/audio-speaker-right-woofer-1` | bus device | `/soc/i2c@2a101c000/codec@3b` |
| `i2c3/audio-speaker-right-woofer-2` | bus device | `/soc/i2c@2a101c000/codec@3d` |
| `i2c3/audio-speaker-right-tweeter` | bus device | `/soc/i2c@2a101c000/codec@3c` |
| `i2c3/audio-codec-output` | bus device | `/soc/i2c@2a101c000/codec@4b` |
| `i2c4` | resources | `/soc/i2c@2a1020000` |
| `i2c4/avellino-pre` | gap | **GAP:** I2C device not described under this bus |
| `i2c4/avellino-post` | gap | **GAP:** I2C device not described under this bus |
| `fpwm1` | resources | `/soc/pwm@2a1044000` |
| `fpwm1/kbd-backlight` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `alc0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `alc0/audio-leap-mic` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `alc4` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `alc4/audio-leap-internal-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `wlan` | shared hardware | `/soc/pcie@580000000/pci@0,0/wifi@0,0` |
| `bluetooth` | shared hardware | `/soc/pcie@580000000/pci@0,0/bluetooth@0,1` |
| `apcie` | partial resources | `/soc/pcie@580000000`; **GAP:** 18 larger or unmatched resource window(s) |
| `apcie/pci-bridge0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie/pci-bridge0/wlan` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie/pci-bridge0/bluetooth-pcie` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apcie0` | resources | `/soc/iommu@594000000` |
| `dart-apcie0/mapper-apcie0-wlan` | stream | Stream of `/soc/iommu@594000000`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-bt` | stream | Stream of `/soc/iommu@594000000`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-piodma` | stream | Stream of `/soc/iommu@594000000`; stream/resource linkage needs review |
| `apcie0-piodma` | resources | `/soc/dma-controller@594010000` |
| `acio-cpu0` | partial resources | `/soc/mailbox@701108000`; **GAP:** 1 larger or unmatched resource window(s) |
| `acio-cpu0/iop-acio0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec0` | gap | **GAP:** no matching hardware resource node |
| `apciec0/pcic0-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
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
| `atc0-dpxbar` | resources | `/soc/mux@70304c000` |
| `atc0-dpphy` | shared hardware | `/soc/phy@703000000` |
| `atc0-dpin0` | resources | `/soc/displayport@701e50000` |
| `atc0-dpin1` | resources | `/soc/displayport@701e58000` |
| `acio-cpu1` | partial resources | `/soc/mailbox@b01108000`; **GAP:** 1 larger or unmatched resource window(s) |
| `acio-cpu1/iop-acio1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec1` | gap | **GAP:** no matching hardware resource node |
| `apciec1/pcic1-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
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
| `atc1-dpxbar` | resources | `/soc/mux@b0304c000` |
| `atc1-dpphy` | shared hardware | `/soc/phy@b03000000` |
| `atc1-dpin0` | resources | `/soc/displayport@b01e50000` |
| `atc1-dpin1` | resources | `/soc/displayport@b01e58000` |
| `display-crossbar0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `mcc` | resources | `/soc/pmp_report@2d03c0000`; `/soc/memory-controller@2d03c0000`; `/soc/power-management@2d0780000`; `/soc/error-handler@29cc40000` |
| `aic` | resources | `/soc/interrupt-controller@2d1000000` |
| `aic-timebase` | resources | `/soc/interrupt-controller@2d1000000` |
| `wdt` | partial resources | `/soc/watchdog@2e42b0000`; **GAP:** 4 larger or unmatched resource window(s) |
| `error-handler` | resources | `/soc/error-handler@29cc40000`; `/soc/event-log@2d0700000` |
| `dwi` | resources | `/soc/dwi@2d0200000` |
| `pwm` | resources | `/soc/pwm@2a1040000` |
| `aes` | resources | `/soc/crypto@2a100c000` |
| `gpio0` | resources | `/soc/pinctrl@2c7100000` |
| `aop-gpio` | resources | `/soc/pinctrl@2f4824000` |
| `nub-gpio` | resources | `/soc/pinctrl@2e41f0000` |
| `smc-gpio` | resources | `/soc/pinctrl@2ec820000` |
| `aop` | partial resources | `/soc/aop@2f4c00000`; **GAP:** 2 larger or unmatched resource window(s) |
| `aop/iop-aop-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-audio` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-audio/audio-pdm2` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-audio/hfdc-2400000` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-audio/audio-hp` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-audio/audio-lp-mic-in` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/aop-audio/audio-leap-internal-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/accel` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/gyro` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-aop` | resources | `/soc/iommu@2f4818000`; `/soc/filter@2f481c000` |
| `dart-aop/mapper-aop` | stream | Stream of `/soc/iommu@2f4818000` / `/soc/filter@2f481c000`; stream/resource linkage needs review |
| `dart-aop/mapper-aop-admac` | stream | Stream of `/soc/iommu@2f4818000` / `/soc/filter@2f481c000`; stream/resource linkage needs review |
| `dart-aop/mapper-scm` | stream | Stream of `/soc/iommu@2f4818000` / `/soc/filter@2f481c000`; stream/resource linkage needs review |
| `pmgr` | partial resources | `/soc/event-log@2d0700000`; `/soc/cpufreq@210e20000`; `/soc/cpufreq@211e20000`; `/soc/power-management@2d0780000`; `/soc/error-handler@29cc40000`; `/soc/pmp_report@2d03c0000`; `/soc/video-encoder@315100000`; `/soc/isp@2a4000000`; `/soc/gpu@292400000`; `/soc/phy@703000000`; `/soc/phy@b03000000`; `/soc/video-codec@321000000`; **GAP:** 57 larger or unmatched resource window(s) |
| `pmgr/clpc` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `pmgr/soc-tuner` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `pmgr/ppm` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `nco` | resources | `/soc/clock-controller@2d0044000` |
| `event-log-handler` | resources | `/soc/event-log@2d0700000` |
| `pmp` | partial resources | `/soc/pmp@2d0500000`; `/soc/dcp@28ec00000`; **GAP:** 2 larger or unmatched resource window(s) |
| `pmp/iop-pmp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-pmp` | resources | `/soc/iommu@2d0300000` |
| `dart-pmp/mapper-pmp` | stream | Stream of `/soc/iommu@2d0300000`; stream/resource linkage needs review |
| `sep` | partial resources | `/soc/sep@29e400000`; **GAP:** 1 larger or unmatched resource window(s) |
| `sep/iop-sep-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/Ocelot` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/xART` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/InvalidateHmac` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-sep` | resources | `/soc/iommu@29cac0000` |
| `dart-sep/mapper-sep` | stream | Stream of `/soc/iommu@29cac0000`; stream/resource linkage needs review |
| `sio` | resources | `/soc/dma-controller@2a1c00000` |
| `sio/iop-sio-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sio/iop-sio-nub/sio-dma` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-sio` | resources | `/soc/iommu@2a1004000` |
| `dart-sio/mapper-sio` | stream | Stream of `/soc/iommu@2a1004000`; stream/resource linkage needs review |
| `dart-sio/mapper-aes` | stream | Stream of `/soc/iommu@2a1004000`; stream/resource linkage needs review |
| `dart-sio/mapper-admac` | stream | Stream of `/soc/iommu@2a1004000`; stream/resource linkage needs review |
| `ans` | partial resources | `/soc/nvme@30dcc0000`; **GAP:** 6 larger or unmatched resource window(s) |
| `ans/iop-ans-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sart-ans` | resources | `/soc/sart@30dc50000`; `/soc/nvme@30dcc0000` |
| `smc` | partial resources | `/soc/smc@2ec400000`; **GAP:** 3 larger or unmatched resource window(s) |
| `smc/iop-smc-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-pmu` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-1` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `rsm` | resources | `/soc/rsm@2e4325000` |
| `dart-rsm` | resources | `/soc/iommu@2e433c000` |
| `dart-rsm/mapper-rsm` | stream | Stream of `/soc/iommu@2e433c000`; stream/resource linkage needs review |
| `uart0` | partial resources | `/soc/serial@2a1200000`; **GAP:** 1 larger or unmatched resource window(s) |
| `uart0/debug-console` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-uart` | gap | **GAP:** no matching hardware resource node |
| `dockchannel-mtp` | partial resources | `/soc/fifo@2fab14000`; `/soc/fifo@2fab14000/input@0`; **GAP:** 1 larger or unmatched resource window(s) |
| `dockchannel-mtp/mtp-transport` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/multi-touch` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/keyboard` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/stm` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/actuator` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-mtp/mtp-transport/tp-accel` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mtp` | partial resources | `/soc/mtp@2fa400000`; **GAP:** 2 larger or unmatched resource window(s) |
| `mtp/iop-mtp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-mtp` | resources | `/soc/iommu@2fa808000`; `/soc/filter@2fa80c000` |
| `dart-mtp/mapper-mtp` | stream | Stream of `/soc/iommu@2fa808000` / `/soc/filter@2fa80c000`; stream/resource linkage needs review |
| `mtp-aop-mux` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `qspi` | resources | `/soc/spi@2a1118000` |
| `qspi/spinor` | gap | **GAP:** SPI NOR endpoint is not described |
| `qspi/spinor/firmware` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `qspi/spinor/anvram` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `qspi/spinor/syscfg` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `i2c2` | resources | `/soc/i2c@2a1018000` |
| `i2c2/atcrt0` | gap | **GAP:** I2C device not described under this bus |
| `i2c2/atcrt1` | gap | **GAP:** I2C device not described under this bus |
| `admac-sio` | partial resources | `/soc/dma-controller@2a3200000`; **GAP:** 1 larger or unmatched resource window(s) |
| `admac-aop-audio` | partial resources | `/soc/dma-controller@2f4980000`; **GAP:** 1 larger or unmatched resource window(s) |
| `atc-phy0` | partial resources | `/soc/phy@703000000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-usb0` | resources | `/soc/iommu@702f00000`; `/soc/iommu@702f80000` |
| `dart-usb0/mapper-usb0` | stream | Stream of `/soc/iommu@702f00000` / `/soc/iommu@702f80000`; stream/resource linkage needs review |
| `usb-drd0` | partial resources | `/soc/usb@702280000`; `/soc/phy@703000000`; **GAP:** 8 larger or unmatched resource window(s) |
| `usb-drd0/usb-drd0-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd0/usb-drd0-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-otg0` | resources | `/soc/usb@702100000`; `/soc/phy@703000000` |
| `atc-phy1` | partial resources | `/soc/phy@b03000000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-usb1` | resources | `/soc/iommu@b02f00000`; `/soc/iommu@b02f80000` |
| `dart-usb1/mapper-usb1` | stream | Stream of `/soc/iommu@b02f00000` / `/soc/iommu@b02f80000`; stream/resource linkage needs review |
| `usb-drd1` | partial resources | `/soc/usb@b02280000`; `/soc/phy@b03000000`; **GAP:** 8 larger or unmatched resource window(s) |
| `usb-drd1/usb-drd1-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd1/usb-drd1-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `nub-spmi0` | partial resources | `/soc/spmi@2e4714000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi0/pmu-main` | bus device | `/soc/spmi@2e4714000/pmic@e` |
| `nub-spmi0/btm` | bus device | `/soc/spmi@2e4714000/pmic@e` |
| `nub-spmi1` | resources | `/soc/spmi@2e4794000` |
| `nub-spmi1/pmu-aux` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0` | partial resources | `/soc/spmi@2e4908000`; **GAP:** 3 larger or unmatched resource window(s) |
| `nub-spmi-a0/hpm0` | bus device | `/soc/spmi@2e4908000/usb-pd@c` |
| `nub-spmi-a0/hpm1` | bus device | `/soc/spmi@2e4908000/usb-pd@a` |
| `nub-spmi-a0/hpm5` | bus device | `/soc/spmi@2e4908000/usb-pd@8` |
| `aop-spmi0` | resources | `/soc/spmi@2f4894000` |
| `aop-spmi0/spmi-wifibt` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1` | resources | `/soc/spmi@2f48d4000` |
| `aop-spmi1/stockholm-spmi` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi/stockholm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `lcd0-sac` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `disp0` | partial resources | `/soc/dcp@28ec00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcp0-expert` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dcp` | partial resources | `/soc/dcp@28ec00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcp/iop-dcp-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcp` | resources | `/soc/iommu@28d30c000` |
| `dart-dcp/mapper-dcp` | stream | Stream of `/soc/iommu@28d30c000`; stream/resource linkage needs review |
| `dart-disp0` | resources | `/soc/iommu@28d304000`; `/soc/iommu@28d300000` |
| `dart-disp0/mapper-disp0` | stream | Stream of `/soc/iommu@28d304000` / `/soc/iommu@28d300000`; stream/resource linkage needs review |
| `dart-disp0/mapper-disp0-piodma` | stream | Stream of `/soc/iommu@28d304000` / `/soc/iommu@28d300000`; stream/resource linkage needs review |
| `dcp-sac-controller` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `dp-audio0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext0` | partial resources | `/soc/dcp@31ac00000`; `/soc/dcp@28ec00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcpext-expert` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dcpext` | partial resources | `/soc/dcp@31ac00000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dcpext/iop-dcpext-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext` | resources | `/soc/iommu@31930c000` |
| `dart-dcpext/mapper-dcpext` | stream | Stream of `/soc/iommu@31930c000`; stream/resource linkage needs review |
| `dart-dispext0` | resources | `/soc/iommu@319304000`; `/soc/iommu@319300000` |
| `dart-dispext0/mapper-dispext0` | stream | Stream of `/soc/iommu@319304000` / `/soc/iommu@319300000`; stream/resource linkage needs review |
| `dart-dispext0/mapper-dispext0-piodma` | stream | Stream of `/soc/iommu@319304000` / `/soc/iommu@319300000`; stream/resource linkage needs review |
| `dp-audio1` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `scaler0` | resources | `/soc/scaler@31d000000` |
| `dart-scaler` | resources | `/soc/iommu@31d200000` |
| `dart-scaler/mapper-scaler` | stream | Stream of `/soc/iommu@31d200000`; stream/resource linkage needs review |
| `dart-scaler/mapper-scaler-piodma` | stream | Stream of `/soc/iommu@31d200000`; stream/resource linkage needs review |
| `jpeg0` | resources | `/soc/jpeg@299000000` |
| `dart-jpeg0` | resources | `/soc/iommu@299004000` |
| `dart-jpeg0/mapper-jpeg0` | stream | Stream of `/soc/iommu@299004000`; stream/resource linkage needs review |
| `jpeg1` | resources | `/soc/jpeg@299008000` |
| `dart-jpeg1` | resources | `/soc/iommu@29900c000` |
| `dart-jpeg1/mapper-jpeg1` | stream | Stream of `/soc/iommu@29900c000`; stream/resource linkage needs review |
| `ave` | resources | `/soc/video-encoder@315100000` |
| `dart-ave` | resources | `/soc/iommu@315030000`; `/soc/iommu@315040000`; `/soc/iommu@315020000`; `/soc/filter@315044000` |
| `dart-ave/mapper-ave` | stream | Stream of `/soc/iommu@315030000` / `/soc/iommu@315040000` / `/soc/iommu@315020000` / `/soc/filter@315044000`; stream/resource linkage needs review |
| `avd` | partial resources | `/soc/avd@289080000`; **GAP:** 1 larger or unmatched resource window(s) |
| `dart-avd` | resources | `/soc/iommu@289010000` |
| `dart-avd/mapper-avd` | stream | Stream of `/soc/iommu@289010000`; stream/resource linkage needs review |
| `dart-avd/mapper-avd-piodma` | stream | Stream of `/soc/iommu@289010000`; stream/resource linkage needs review |
| `dart-avd/mapper-avd-adsbuf` | stream | Stream of `/soc/iommu@289010000`; stream/resource linkage needs review |
| `apr` | resources | `/soc/video-codec@321000000` |
| `dart-apr` | resources | `/soc/iommu@321028000`; `/soc/iommu@32102c000` |
| `dart-apr/mapper-apr` | stream | Stream of `/soc/iommu@321028000` / `/soc/iommu@32102c000`; stream/resource linkage needs review |
| `isp` | partial resources | `/soc/isp@2a4000000`; `/soc/video-encoder@315100000`; **GAP:** 2 larger or unmatched resource window(s) |
| `dart-isp` | resources | `/soc/iommu@2a64a8000`; `/soc/iommu@2a64b4000`; `/soc/iommu@2a64bc000`; `/soc/iommu@2a64b0000`; `/soc/iommu@2a64b8000`; `/soc/filter@2a64ac000` |
| `dart-isp/mapper-isp` | stream | Stream of `/soc/iommu@2a64a8000` / `/soc/iommu@2a64b4000` / `/soc/iommu@2a64bc000` / `/soc/iommu@2a64b0000` / `/soc/iommu@2a64b8000` / `/soc/filter@2a64ac000`; stream/resource linkage needs review |
| `ane` | partial resources | `/soc/filter@311804000`; `/soc/video-encoder@315100000`; **GAP:** 5 larger or unmatched resource window(s) |
| `ane/iop-ane-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-ane` | partial resources | `/soc/filter@311804000`; **GAP:** 3 larger or unmatched resource window(s) |
| `dart-ane/mapper-ane` | stream | Stream of `/soc/filter@311804000`; stream/resource linkage needs review |
| `sgx` | partial resources | `/soc/gpu@292400000`; **GAP:** 1 larger or unmatched resource window(s) |
| `gfx-asc` | resources | `/soc/gpu@292400000` |
| `gfx-asc/iop-gfx-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mapper-gfx-asc` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `mca-switch` | partial resources | `/soc/i2s@2a3400000`; **GAP:** 2 larger or unmatched resource window(s) |
| `mca0` | resources | `/soc/i2s@2a3400000` |
| `mca0/mca0a` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca0/mca0a/audio-speaker` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca1` | resources | `/soc/i2s@2a3400000` |
| `mca1/mca1a` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca1/mca1a/audio-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca2` | resources | `/soc/i2s@2a3400000` |
| `mca2/mca2a` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca2/mca2a/audio-codec-output` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca2/mca2b` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca2/mca2b/audio-codec-input` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smctempsensor0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `cpu-debug-interface` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `aft` | resources | `/soc/trace@220180000` |
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
| `port-magsafe3-1` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `backlight` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `sacm` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `defaults` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `product` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `iboot-syscfg` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `filesystems-props` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `osenvironments` | Boot metadata; no independent hardware device. Subtree properties omitted. |
