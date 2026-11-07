# DT gaps: J575D

## Identity and scope

Board DTS: `arch/arm64/boot/dts/apple/t6032-j575d.dts`.
The hardware resource inventory identifies the T6031 peripheral die.

Source: per-board 27.2 hardware-description template; no matching live dump in the supplied collection. Only hardware identities and resource mappings are recorded here; no tuning or factory data is reproduced.

## Result

This resource coverage audit still has unresolved entries and does not establish complete board support. 626 arm-io records were examined. Hardware blocks added in this series remain disabled. A resource match does not prove IRQ, clock, power or DMA stream correctness.

**Audit basis:** DTS source analysis; compiled-DTB reconciliation pending.

| Record classification | Count |
| --- | ---: |
| gap | 202 |
| partial resources | 5 |
| resources | 66 |
| service review | 123 |
| shared hardware | 2 |
| stream | 228 |

## Remaining work

- Close every explicit GAP and service-review entry below, including nested endpoints and shared resource ownership.
- Some template SPMI records are alternative endpoints at the same address. Runtime model selection is needed before representing these as simultaneous devices.
- Reconcile larger firmware resource windows with the driver-specific Linux register layout; do not expand an active device aperture without driver review.
- Bindings and resource presence do not supply per-device calibration, firmware or an initialization contract.
- Validate this board with `W=1` and `dtbs_check`, then perform a separately approved boot before enabling newly described engines.

## Dependencies

- Stacked on linux PR #1097 (M3 family resource descriptions), commit 5ff4ff12.
- linux PR #1031 remains the existing M3 cleanup dependency.
- J575d is an M3 Ultra/T6032 package containing two T6031 peripheral dies. AIC interrupt encoding separates the die ID from the per-die interrupt number.

## Every arm-io record

Paths in the first column are relative to `arm-io`. Stream entries name the owning translation block; their SID and aperture semantics still need review. Service-review entries have no independently decoded MMIO resource; this is not a waiver of their hardware or binding requirements.

| Firmware hardware/service record | Classification | Linux description or gap |
| --- | --- | --- |
| `gpio1` | gap | **GAP:** no matching hardware resource node |
| `aop-gpio1` | gap | **GAP:** no matching hardware resource node |
| `nub-gpio1` | gap | **GAP:** no matching hardware resource node |
| `i2c7` | gap | **GAP:** no matching hardware resource node |
| `i2c7/uatcrt4` | gap | **GAP:** I2C device not described under this bus |
| `i2c7/uatcrt5` | gap | **GAP:** I2C device not described under this bus |
| `sio1` | gap | **GAP:** no matching hardware resource node |
| `sio1/iop-sio1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sio1/iop-sio1-nub/sio1-dma` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-sio1` | gap | **GAP:** no matching hardware resource node |
| `dart-sio1/mapper-sio1` | stream | Stream of `dart-sio1`; stream/resource linkage needs review |
| `apcie1` | gap | **GAP:** no matching hardware resource node |
| `displaymanager` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `i2c0` | gap | **GAP:** no matching hardware resource node |
| `i2c0/gen-lock-hub` | gap | **GAP:** I2C device not described under this bus |
| `i2c1` | gap | **GAP:** no matching hardware resource node |
| `i2c1/audio-speaker` | gap | **GAP:** I2C device not described under this bus |
| `i2c2` | gap | **GAP:** no matching hardware resource node |
| `i2c2/audio-codec-output` | gap | **GAP:** I2C device not described under this bus |
| `i2c4` | gap | **GAP:** no matching hardware resource node |
| `i2c4/sd-card` | gap | **GAP:** I2C device not described under this bus |
| `i2c5` | gap | **GAP:** no matching hardware resource node |
| `i2c5/aqc-a` | gap | **GAP:** I2C device not described under this bus |
| `i2c6` | gap | **GAP:** no matching hardware resource node |
| `i2c6/uatcrt0` | gap | **GAP:** I2C device not described under this bus |
| `i2c6/uatcrt1` | gap | **GAP:** I2C device not described under this bus |
| `i2c6/uatcrt2` | gap | **GAP:** I2C device not described under this bus |
| `i2c6/uatcrt3` | gap | **GAP:** I2C device not described under this bus |
| `i2c8` | gap | **GAP:** no matching hardware resource node |
| `i2c8/pcon1` | gap | **GAP:** I2C device not described under this bus |
| `apcie0` | gap | **GAP:** no matching hardware resource node |
| `apcie0/pci-bridge0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge0/wlan` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge0/bluetooth-pcie` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge1` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge1/pcie-sdreader` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge2` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge2/lan-10gb` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge3` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge3/pcie-xhci` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge3/pcie-xhci/pcie-xhci-hs-port3` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge3/pcie-xhci/pcie-xhci-hs-port4` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge3/pcie-xhci/pcie-xhci-ss-port1` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apcie0/pci-bridge3/pcie-xhci/pcie-xhci-ss-port2` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apcie0` | gap | **GAP:** no matching hardware resource node |
| `dart-apcie0/mapper-apcie0-wlan` | stream | Stream of `dart-apcie0`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-bt` | stream | Stream of `dart-apcie0`; stream/resource linkage needs review |
| `dart-apcie0/mapper-apcie0-piodma` | stream | Stream of `dart-apcie0`; stream/resource linkage needs review |
| `apcie0-piodma` | resources | `/soc@200000000/dma-controller@594010000` |
| `dart-apcie1` | gap | **GAP:** no matching hardware resource node |
| `dart-apcie1/mapper-apcie1` | stream | Stream of `dart-apcie1`; stream/resource linkage needs review |
| `dart-apcie1/mapper-apcie1-piodma` | stream | Stream of `dart-apcie1`; stream/resource linkage needs review |
| `apcie1-piodma` | resources | `/soc@200000000/dma-controller@595010000` |
| `dart-apcie2` | gap | **GAP:** no matching hardware resource node |
| `dart-apcie2/mapper-apcie2` | stream | Stream of `dart-apcie2`; stream/resource linkage needs review |
| `dart-apcie2/mapper-apcie2-piodma` | stream | Stream of `dart-apcie2`; stream/resource linkage needs review |
| `apcie2-piodma` | resources | `/soc@200000000/dma-controller@596010000` |
| `lan-10gb-portmon` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dart-apcie3` | gap | **GAP:** no matching hardware resource node |
| `dart-apcie3/mapper-apcie3` | stream | Stream of `dart-apcie3`; stream/resource linkage needs review |
| `dart-apcie3/mapper-apcie3-piodma` | stream | Stream of `dart-apcie3`; stream/resource linkage needs review |
| `apcie3-piodma` | resources | `/soc@200000000/dma-controller@597010000` |
| `acio-phy-cpu0` | gap | **GAP:** no matching hardware resource node |
| `acio-phy-cpu0/iop-aciophy0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `acio-phy-cpu1` | gap | **GAP:** no matching hardware resource node |
| `acio-phy-cpu1/iop-aciophy1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `acio-phy-cpu2` | gap | **GAP:** no matching hardware resource node |
| `acio-phy-cpu2/iop-aciophy2-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `acio-phy-cpu3` | gap | **GAP:** no matching hardware resource node |
| `acio-phy-cpu3/iop-aciophy3-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `acio-phy-cpu4` | gap | **GAP:** no matching hardware resource node |
| `acio-phy-cpu4/iop-aciophy4-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `acio-phy-cpu5` | gap | **GAP:** no matching hardware resource node |
| `acio-phy-cpu5/iop-aciophy5-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `acio-phy-cpu6` | gap | **GAP:** no matching hardware resource node |
| `acio-phy-cpu6/iop-aciophy6-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `acio-phy-cpu7` | gap | **GAP:** no matching hardware resource node |
| `acio-phy-cpu7/iop-aciophy7-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec0` | gap | **GAP:** no matching hardware resource node |
| `apciec0/pcic0-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec0` | resources | `/soc@200000000/iommu@721008000` |
| `dart-apciec0/mapper-apciec0-piodma` | stream | Stream of `/soc@200000000/iommu@721008000`; stream/resource linkage needs review |
| `apciec0-piodma` | resources | `/soc@200000000/dma-controller@722000000` |
| `acio0` | gap | **GAP:** no matching hardware resource node |
| `dart-acio0` | gap | **GAP:** no matching hardware resource node |
| `dart-acio0/mapper-acio0-tx-0` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-1` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-2` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-3` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-4` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-5` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-6` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-7` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-8` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-9` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-10` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-tx-11` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-0` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-1` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-2` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-3` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-4` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-5` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-6` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-7` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-8` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-9` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-10` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `dart-acio0/mapper-acio0-rx-11` | stream | Stream of `dart-acio0`; stream/resource linkage needs review |
| `acio-cpu0` | gap | **GAP:** no matching hardware resource node |
| `acio-cpu0/iop-acio0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec1` | gap | **GAP:** no matching hardware resource node |
| `apciec1/pcic1-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec1` | resources | `/soc@200000000/iommu@b21008000` |
| `dart-apciec1/mapper-apciec1-piodma` | stream | Stream of `/soc@200000000/iommu@b21008000`; stream/resource linkage needs review |
| `apciec1-piodma` | resources | `/soc@200000000/dma-controller@b22000000` |
| `acio1` | gap | **GAP:** no matching hardware resource node |
| `dart-acio1` | gap | **GAP:** no matching hardware resource node |
| `dart-acio1/mapper-acio1-tx-0` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-1` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-2` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-3` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-4` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-5` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-6` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-7` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-8` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-9` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-10` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-tx-11` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-0` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-1` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-2` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-3` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-4` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-5` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-6` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-7` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-8` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-9` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-10` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `dart-acio1/mapper-acio1-rx-11` | stream | Stream of `dart-acio1`; stream/resource linkage needs review |
| `acio-cpu1` | gap | **GAP:** no matching hardware resource node |
| `acio-cpu1/iop-acio1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec2` | gap | **GAP:** no matching hardware resource node |
| `apciec2/pcic2-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec2` | resources | `/soc@200000000/iommu@f21008000` |
| `dart-apciec2/mapper-apciec2-piodma` | stream | Stream of `/soc@200000000/iommu@f21008000`; stream/resource linkage needs review |
| `apciec2-piodma` | resources | `/soc@200000000/dma-controller@f22000000` |
| `acio2` | gap | **GAP:** no matching hardware resource node |
| `dart-acio2` | gap | **GAP:** no matching hardware resource node |
| `dart-acio2/mapper-acio2-tx-0` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-1` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-2` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-3` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-4` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-5` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-6` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-7` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-8` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-9` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-10` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-tx-11` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-0` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-1` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-2` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-3` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-4` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-5` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-6` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-7` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-8` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-9` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-10` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `dart-acio2/mapper-acio2-rx-11` | stream | Stream of `dart-acio2`; stream/resource linkage needs review |
| `acio-cpu2` | gap | **GAP:** no matching hardware resource node |
| `acio-cpu2/iop-acio2-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec3` | gap | **GAP:** no matching hardware resource node |
| `apciec3/pcic3-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec3` | resources | `/soc@200000000/iommu@1321008000` |
| `dart-apciec3/mapper-apciec3-piodma` | stream | Stream of `/soc@200000000/iommu@1321008000`; stream/resource linkage needs review |
| `apciec3-piodma` | resources | `/soc@200000000/dma-controller@1322000000` |
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
| `acio-cpu3` | gap | **GAP:** no matching hardware resource node |
| `acio-cpu3/iop-acio3-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec4` | gap | **GAP:** no matching hardware resource node |
| `apciec4/pcic4-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec4` | resources | `/soc@2200000000/iommu@721008000` |
| `dart-apciec4/mapper-apciec4-piodma` | stream | Stream of `/soc@2200000000/iommu@721008000`; stream/resource linkage needs review |
| `apciec4-piodma` | resources | `/soc@2200000000/dma-controller@722000000` |
| `acio4` | gap | **GAP:** no matching hardware resource node |
| `dart-acio4` | gap | **GAP:** no matching hardware resource node |
| `dart-acio4/mapper-acio4-tx-0` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-1` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-2` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-3` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-4` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-5` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-6` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-7` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-8` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-9` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-10` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-tx-11` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-0` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-1` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-2` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-3` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-4` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-5` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-6` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-7` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-8` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-9` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-10` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `dart-acio4/mapper-acio4-rx-11` | stream | Stream of `dart-acio4`; stream/resource linkage needs review |
| `acio-cpu4` | gap | **GAP:** no matching hardware resource node |
| `acio-cpu4/iop-acio4-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `apciec5` | gap | **GAP:** no matching hardware resource node |
| `apciec5/pcic5-bridge` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-apciec5` | resources | `/soc@2200000000/iommu@b21008000` |
| `dart-apciec5/mapper-apciec5-piodma` | stream | Stream of `/soc@2200000000/iommu@b21008000`; stream/resource linkage needs review |
| `apciec5-piodma` | resources | `/soc@2200000000/dma-controller@b22000000` |
| `acio5` | gap | **GAP:** no matching hardware resource node |
| `dart-acio5` | gap | **GAP:** no matching hardware resource node |
| `dart-acio5/mapper-acio5-tx-0` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-1` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-2` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-3` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-4` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-5` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-6` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-7` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-8` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-9` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-10` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-tx-11` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-0` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-1` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-2` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-3` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-4` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-5` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-6` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-7` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-8` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-9` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-10` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `dart-acio5/mapper-acio5-rx-11` | stream | Stream of `dart-acio5`; stream/resource linkage needs review |
| `acio-cpu5` | gap | **GAP:** no matching hardware resource node |
| `acio-cpu5/iop-acio5-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `wlan` | shared hardware | `/unresolved/pcie@580000000/pci@0,0/wifi@0,0` |
| `bluetooth` | shared hardware | `/unresolved/pcie@580000000/pci@0,0/bluetooth@0,1` |
| `display-crossbar0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc0-dpxbar` | gap | **GAP:** no matching hardware resource node |
| `atc0-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc0-dpin0` | resources | `/soc@200000000/displayport@701e50000` |
| `atc0-dpin1` | resources | `/soc@200000000/displayport@701e58000` |
| `atc1-dpxbar` | gap | **GAP:** no matching hardware resource node |
| `atc1-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc1-dpin0` | resources | `/soc@200000000/displayport@b01e50000` |
| `atc1-dpin1` | resources | `/soc@200000000/displayport@b01e58000` |
| `atc2-dpxbar` | gap | **GAP:** no matching hardware resource node |
| `atc2-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc2-dpin0` | resources | `/soc@200000000/displayport@f01e50000` |
| `atc2-dpin1` | resources | `/soc@200000000/displayport@f01e58000` |
| `atc3-dpxbar` | gap | **GAP:** no matching hardware resource node |
| `atc3-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc3-dpin0` | resources | `/soc@200000000/displayport@1301e50000` |
| `atc3-dpin1` | resources | `/soc@200000000/displayport@1301e58000` |
| `atc4-dpxbar` | gap | **GAP:** no matching hardware resource node |
| `atc4-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc4-dpin0` | resources | `/soc@2200000000/displayport@701e50000` |
| `atc4-dpin1` | resources | `/soc@2200000000/displayport@701e58000` |
| `atc5-dpxbar` | gap | **GAP:** no matching hardware resource node |
| `atc5-dpphy` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `atc5-dpin0` | resources | `/soc@2200000000/displayport@b01e50000` |
| `atc5-dpin1` | resources | `/soc@2200000000/displayport@b01e58000` |
| `mcc` | resources | `/memory-controller@2903c0000` |
| `aic` | gap | **GAP:** no matching hardware resource node |
| `aic-timebase` | gap | **GAP:** no matching hardware resource node |
| `wdt` | gap | **GAP:** no matching hardware resource node |
| `error-handler` | partial resources | `/soc@200000000/dwi@2929b0000`; `/memory-controller@2903c0000`; **GAP:** 262 larger or unmatched resource window(s) |
| `dwi` | resources | `/soc@200000000/dwi@2929b0000` |
| `aes` | resources | `/soc@200000000/crypto@39100c000` |
| `gpio0` | gap | **GAP:** no matching hardware resource node |
| `aop-gpio0` | gap | **GAP:** no matching hardware resource node |
| `nub-gpio0` | gap | **GAP:** no matching hardware resource node |
| `smc-gpio0` | gap | **GAP:** no matching hardware resource node |
| `aop` | gap | **GAP:** no matching hardware resource node |
| `aop/iop-aop-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/accel` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aop/iop-aop-nub/gyro` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-aop` | gap | **GAP:** no matching hardware resource node |
| `dart-aop/mapper-aop` | stream | Stream of `dart-aop`; stream/resource linkage needs review |
| `dart-aop/mapper-aop-admac` | stream | Stream of `dart-aop`; stream/resource linkage needs review |
| `dart-aop/mapper-scm` | stream | Stream of `dart-aop`; stream/resource linkage needs review |
| `pmgr` | partial resources | `/soc@200000000/dwi@2929b0000`; `/soc@200000000/crypto@39100c000`; `/soc@200000000/video-encoder@3c5100000`; `/soc@200000000/video-encoder@285100000`; `/memory-controller@2903c0000`; **GAP:** 59 larger or unmatched resource window(s) |
| `pmgr/soc-tuner` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `pmgr/clpc` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `pmgr/ppm` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `event-log-handler` | partial resources | `/soc@200000000/video-encoder@285100000`; `/soc@200000000/video-encoder@3c5100000`; `/soc@2200000000/video-encoder@285100000`; `/soc@2200000000/video-encoder@3c5100000`; **GAP:** 50 larger or unmatched resource window(s) |
| `nco` | gap | **GAP:** no matching hardware resource node |
| `sio` | gap | **GAP:** no matching hardware resource node |
| `sio/iop-sio-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sio/iop-sio-nub/sio-dma` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-sio` | gap | **GAP:** no matching hardware resource node |
| `dart-sio/mapper-sio` | stream | Stream of `dart-sio`; stream/resource linkage needs review |
| `dart-sio/mapper-aes` | stream | Stream of `dart-sio`; stream/resource linkage needs review |
| `dart-sio/mapper-admac` | stream | Stream of `dart-sio`; stream/resource linkage needs review |
| `pmp0` | gap | **GAP:** no matching hardware resource node |
| `pmp0/iop-pmp0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-pmp0` | gap | **GAP:** no matching hardware resource node |
| `dart-pmp0/mapper-pmp0` | stream | Stream of `dart-pmp0`; stream/resource linkage needs review |
| `pmp1` | gap | **GAP:** no matching hardware resource node |
| `pmp1/iop-pmp1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-pmp1` | gap | **GAP:** no matching hardware resource node |
| `dart-pmp1/mapper-pmp1` | stream | Stream of `dart-pmp1`; stream/resource linkage needs review |
| `sep` | gap | **GAP:** no matching hardware resource node |
| `sep/iop-sep-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/Ocelot` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/xART` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sep/iop-sep-nub/InvalidateHmac` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-sep` | gap | **GAP:** no matching hardware resource node |
| `dart-sep/mapper-sep` | stream | Stream of `dart-sep`; stream/resource linkage needs review |
| `ans` | gap | **GAP:** no matching hardware resource node |
| `ans/iop-ans-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `ans/ans-pcie` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `sart-ans` | gap | **GAP:** no matching hardware resource node |
| `smc` | gap | **GAP:** no matching hardware resource node |
| `smc/iop-smc-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-pmu` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-1` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smc/iop-smc-nub/smc-charger-util-2` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `aon-ptd` | resources | `/soc@200000000/interrupt-controller@2a4dc0000` |
| `pmgrtools` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `rsm` | resources | `/soc@200000000/rsm@2a0325000` |
| `dart-rsm` | resources | `/soc@200000000/iommu@2a033c000` |
| `dart-rsm/mapper-rsm` | stream | Stream of `/soc@200000000/iommu@2a033c000`; stream/resource linkage needs review |
| `uart0` | gap | **GAP:** no matching hardware resource node |
| `uart0/debug-console` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dockchannel-uart` | gap | **GAP:** no matching hardware resource node |
| `qspi` | gap | **GAP:** no matching hardware resource node |
| `qspi/spinor` | gap | **GAP:** SPI NOR endpoint is not described |
| `qspi/spinor/firmware` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `qspi/spinor/anvram` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `qspi/spinor/syscfg` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dp2hdmi-gpio1` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `lpdptx-phy1` | gap | **GAP:** no matching hardware resource node |
| `admac-sio` | gap | **GAP:** no matching hardware resource node |
| `atc-phy0` | gap | **GAP:** no matching hardware resource node |
| `dart-usb0` | gap | **GAP:** no matching hardware resource node |
| `dart-usb0/mapper-usb0` | stream | Stream of `dart-usb0`; stream/resource linkage needs review |
| `usb-drd0` | gap | **GAP:** no matching hardware resource node |
| `usb-drd0/usb-drd0-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd0/usb-drd0-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-otg0` | gap | **GAP:** no matching hardware resource node |
| `atc-phy1` | gap | **GAP:** no matching hardware resource node |
| `dart-usb1` | gap | **GAP:** no matching hardware resource node |
| `dart-usb1/mapper-usb1` | stream | Stream of `dart-usb1`; stream/resource linkage needs review |
| `usb-drd1` | gap | **GAP:** no matching hardware resource node |
| `usb-drd1/usb-drd1-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd1/usb-drd1-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `atc-phy2` | gap | **GAP:** no matching hardware resource node |
| `dart-usb2` | gap | **GAP:** no matching hardware resource node |
| `dart-usb2/mapper-usb2` | stream | Stream of `dart-usb2`; stream/resource linkage needs review |
| `usb-drd2` | gap | **GAP:** no matching hardware resource node |
| `usb-drd2/usb-drd2-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd2/usb-drd2-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `atc-phy3` | gap | **GAP:** no matching hardware resource node |
| `dart-usb3` | gap | **GAP:** no matching hardware resource node |
| `dart-usb3/mapper-usb3` | stream | Stream of `dart-usb3`; stream/resource linkage needs review |
| `usb-drd3` | gap | **GAP:** no matching hardware resource node |
| `usb-drd3/usb-drd3-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd3/usb-drd3-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `atc-phy4` | gap | **GAP:** no matching hardware resource node |
| `dart-usb4` | gap | **GAP:** no matching hardware resource node |
| `dart-usb4/mapper-usb4` | stream | Stream of `dart-usb4`; stream/resource linkage needs review |
| `usb-drd4` | gap | **GAP:** no matching hardware resource node |
| `usb-drd4/usb-drd4-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd4/usb-drd4-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `atc-phy5` | gap | **GAP:** no matching hardware resource node |
| `dart-usb5` | gap | **GAP:** no matching hardware resource node |
| `dart-usb5/mapper-usb5` | stream | Stream of `dart-usb5`; stream/resource linkage needs review |
| `usb-drd5` | gap | **GAP:** no matching hardware resource node |
| `usb-drd5/usb-drd5-port-hs` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `usb-drd5/usb-drd5-port-ss` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `nub-spmi0` | gap | **GAP:** no matching hardware resource node |
| `nub-spmi0/spmi-chapelL1-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi0/btm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi1` | gap | **GAP:** no matching hardware resource node |
| `nub-spmi1/spmi-chapelF1-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi1/spmi-spitfireE1-9` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi2` | gap | **GAP:** no matching hardware resource node |
| `nub-spmi2/spmi-sebringC1-a` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi2/spmi-sebringC4-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi3` | gap | **GAP:** no matching hardware resource node |
| `nub-spmi3/spmi-sebringC2-a` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi3/spmi-sebringC3-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi4` | gap | **GAP:** no matching hardware resource node |
| `nub-spmi4/spmi-sebringC0-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0` | gap | **GAP:** no matching hardware resource node |
| `nub-spmi-a0/hpm0` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0/hpm1` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0/hpm2` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a0/hpm3` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a1` | gap | **GAP:** no matching hardware resource node |
| `nub-spmi-a2` | gap | **GAP:** no matching hardware resource node |
| `nub-spmi-a2/hpm4` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `nub-spmi-a2/hpm5` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `die1-nub-spmi0` | gap | **GAP:** no matching hardware resource node |
| `die1-nub-spmi0/spmi-chapelL2-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `die1-nub-spmi1` | gap | **GAP:** no matching hardware resource node |
| `die1-nub-spmi1/spmi-chapelF2-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `die1-nub-spmi2` | gap | **GAP:** no matching hardware resource node |
| `die1-nub-spmi2/spmi-sebringD1-a` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `die1-nub-spmi2/spmi-sebringD4-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `die1-nub-spmi3` | gap | **GAP:** no matching hardware resource node |
| `die1-nub-spmi3/spmi-sebringD2-a` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `die1-nub-spmi3/spmi-sebringD3-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `die1-nub-spmi4` | gap | **GAP:** no matching hardware resource node |
| `die1-nub-spmi4/spmi-sebringD0-b` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi0` | gap | **GAP:** no matching hardware resource node |
| `aop-spmi0/spmi-wifibt` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1` | gap | **GAP:** no matching hardware resource node |
| `aop-spmi1/stockholm-spmi` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `aop-spmi1/stockholm-spmi/stockholm` | gap | **GAP:** SPMI endpoint and interrupt/GPIO dependencies need a binding and description |
| `scaler0` | resources | `/soc@200000000/scaler@407000000` |
| `dart-scaler0` | resources | `/soc@200000000/iommu@407200000` |
| `dart-scaler0/mapper-scaler0` | stream | Stream of `/soc@200000000/iommu@407200000`; stream/resource linkage needs review |
| `dart-scaler0/mapper-scaler-piodma0` | stream | Stream of `/soc@200000000/iommu@407200000`; stream/resource linkage needs review |
| `scaler1` | resources | `/soc@200000000/scaler@3c7000000` |
| `dart-scaler1` | resources | `/soc@200000000/iommu@3c7200000` |
| `dart-scaler1/mapper-scaler1` | stream | Stream of `/soc@200000000/iommu@3c7200000`; stream/resource linkage needs review |
| `dart-scaler1/mapper-scaler-piodma1` | stream | Stream of `/soc@200000000/iommu@3c7200000`; stream/resource linkage needs review |
| `scaler2` | resources | `/soc@2200000000/scaler@407000000` |
| `dart-scaler2` | resources | `/soc@2200000000/iommu@407200000` |
| `dart-scaler2/mapper-scaler2` | stream | Stream of `/soc@2200000000/iommu@407200000`; stream/resource linkage needs review |
| `dart-scaler2/mapper-scaler-piodma2` | stream | Stream of `/soc@2200000000/iommu@407200000`; stream/resource linkage needs review |
| `scaler3` | resources | `/soc@2200000000/scaler@3c7000000` |
| `dart-scaler3` | resources | `/soc@2200000000/iommu@3c7200000` |
| `dart-scaler3/mapper-scaler3` | stream | Stream of `/soc@2200000000/iommu@3c7200000`; stream/resource linkage needs review |
| `dart-scaler3/mapper-scaler-piodma3` | stream | Stream of `/soc@2200000000/iommu@3c7200000`; stream/resource linkage needs review |
| `apr0` | resources | `/soc@200000000/video-codec@2c5000000` |
| `dart-apr0` | resources | `/soc@200000000/iommu@2c5028000`; `/soc@200000000/iommu@2c502c000` |
| `dart-apr0/mapper-apr0` | stream | Stream of `/soc@200000000/iommu@2c5028000` / `/soc@200000000/iommu@2c502c000`; stream/resource linkage needs review |
| `apr1` | resources | `/soc@200000000/video-codec@287000000` |
| `dart-apr1` | resources | `/soc@200000000/iommu@287008000` |
| `dart-apr1/mapper-apr1` | stream | Stream of `/soc@200000000/iommu@287008000`; stream/resource linkage needs review |
| `apr2` | resources | `/soc@2200000000/video-codec@2c5000000` |
| `dart-apr2` | resources | `/soc@2200000000/iommu@2c5028000`; `/soc@2200000000/iommu@2c502c000` |
| `dart-apr2/mapper-apr2` | stream | Stream of `/soc@2200000000/iommu@2c5028000` / `/soc@2200000000/iommu@2c502c000`; stream/resource linkage needs review |
| `apr3` | resources | `/soc@2200000000/video-codec@287000000` |
| `dart-apr3` | resources | `/soc@2200000000/iommu@287008000` |
| `dart-apr3/mapper-apr3` | stream | Stream of `/soc@2200000000/iommu@287008000`; stream/resource linkage needs review |
| `jpeg0` | resources | `/soc@200000000/jpeg@2cf000000` |
| `dart-jpeg0` | resources | `/soc@200000000/iommu@2cf004000` |
| `dart-jpeg0/mapper-jpeg0` | stream | Stream of `/soc@200000000/iommu@2cf004000`; stream/resource linkage needs review |
| `jpeg1` | resources | `/soc@200000000/jpeg@2cf008000` |
| `dart-jpeg1` | resources | `/soc@200000000/iommu@2cf00c000` |
| `dart-jpeg1/mapper-jpeg1` | stream | Stream of `/soc@200000000/iommu@2cf00c000`; stream/resource linkage needs review |
| `jpeg2` | resources | `/soc@2200000000/jpeg@2cf000000` |
| `dart-jpeg2` | resources | `/soc@2200000000/iommu@2cf004000` |
| `dart-jpeg2/mapper-jpeg2` | stream | Stream of `/soc@2200000000/iommu@2cf004000`; stream/resource linkage needs review |
| `jpeg3` | resources | `/soc@2200000000/jpeg@2cf008000` |
| `dart-jpeg3` | resources | `/soc@2200000000/iommu@2cf00c000` |
| `dart-jpeg3/mapper-jpeg3` | stream | Stream of `/soc@2200000000/iommu@2cf00c000`; stream/resource linkage needs review |
| `ave0` | resources | `/soc@200000000/video-encoder@285100000` |
| `dart-ave0` | resources | `/soc@200000000/iommu@285030000`; `/soc@200000000/iommu@285040000`; `/soc@200000000/iommu@285020000`; `/soc@200000000/filter@285044000` |
| `dart-ave0/mapper-ave0` | stream | Stream of `/soc@200000000/iommu@285030000` / `/soc@200000000/iommu@285040000` / `/soc@200000000/iommu@285020000` / `/soc@200000000/filter@285044000`; stream/resource linkage needs review |
| `ave1` | resources | `/soc@200000000/video-encoder@3c5100000` |
| `dart-ave1` | resources | `/soc@200000000/iommu@3c5030000`; `/soc@200000000/iommu@3c5040000`; `/soc@200000000/iommu@3c5020000`; `/soc@200000000/filter@3c5044000` |
| `dart-ave1/mapper-ave1` | stream | Stream of `/soc@200000000/iommu@3c5030000` / `/soc@200000000/iommu@3c5040000` / `/soc@200000000/iommu@3c5020000` / `/soc@200000000/filter@3c5044000`; stream/resource linkage needs review |
| `ave2` | resources | `/soc@2200000000/video-encoder@285100000` |
| `dart-ave2` | resources | `/soc@2200000000/iommu@285030000`; `/soc@2200000000/iommu@285040000`; `/soc@2200000000/iommu@285020000`; `/soc@2200000000/filter@285044000` |
| `dart-ave2/mapper-ave2` | stream | Stream of `/soc@2200000000/iommu@285030000` / `/soc@2200000000/iommu@285040000` / `/soc@2200000000/iommu@285020000` / `/soc@2200000000/filter@285044000`; stream/resource linkage needs review |
| `ave3` | resources | `/soc@2200000000/video-encoder@3c5100000` |
| `dart-ave3` | resources | `/soc@2200000000/iommu@3c5030000`; `/soc@2200000000/iommu@3c5040000`; `/soc@2200000000/iommu@3c5020000`; `/soc@2200000000/filter@3c5044000` |
| `dart-ave3/mapper-ave3` | stream | Stream of `/soc@2200000000/iommu@3c5030000` / `/soc@2200000000/iommu@3c5040000` / `/soc@2200000000/iommu@3c5020000` / `/soc@2200000000/filter@3c5044000`; stream/resource linkage needs review |
| `avd0` | gap | **GAP:** no matching hardware resource node |
| `dart-avd0` | gap | **GAP:** no matching hardware resource node |
| `dart-avd0/mapper-avd0` | stream | Stream of `dart-avd0`; stream/resource linkage needs review |
| `dart-avd0/mapper-avd0-piodma` | stream | Stream of `dart-avd0`; stream/resource linkage needs review |
| `dart-avd0/mapper-avd0-adsbuf` | stream | Stream of `dart-avd0`; stream/resource linkage needs review |
| `avd1` | gap | **GAP:** no matching hardware resource node |
| `dart-avd1` | gap | **GAP:** no matching hardware resource node |
| `dart-avd1/mapper-avd1` | stream | Stream of `dart-avd1`; stream/resource linkage needs review |
| `dart-avd1/mapper-avd1-piodma` | stream | Stream of `dart-avd1`; stream/resource linkage needs review |
| `dart-avd1/mapper-avd1-adsbuf` | stream | Stream of `dart-avd1`; stream/resource linkage needs review |
| `ane0` | partial resources | `/soc@200000000/video-encoder@3c5100000`; **GAP:** 8 larger or unmatched resource window(s) |
| `ane0/iop-ane0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-ane0` | gap | **GAP:** no matching hardware resource node |
| `dart-ane0/mapper-ane0` | stream | Stream of `dart-ane0`; stream/resource linkage needs review |
| `ane1` | partial resources | `/soc@2200000000/video-encoder@3c5100000`; **GAP:** 8 larger or unmatched resource window(s) |
| `ane1/iop-ane1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-ane1` | gap | **GAP:** no matching hardware resource node |
| `dart-ane1/mapper-ane1` | stream | Stream of `dart-ane1`; stream/resource linkage needs review |
| `sgx` | gap | **GAP:** no matching hardware resource node |
| `gfx-asc` | gap | **GAP:** no matching hardware resource node |
| `gfx-asc/iop-gfx-nub0` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mapper-gfx-asc` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `mca-switch` | gap | **GAP:** no matching hardware resource node |
| `mca0` | gap | **GAP:** no matching hardware resource node |
| `mca0/mca0a` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca0/mca0a/audio-speaker` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca1` | gap | **GAP:** no matching hardware resource node |
| `mca1/mca1a` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca1/mca1a/audio-loopback` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca2` | gap | **GAP:** no matching hardware resource node |
| `mca2/mca2a` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca2/mca2a/audio-codec-output` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca2/mca2b` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `mca2/mca2b/audio-codec-input` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `smctempsensor0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `cpu-debug-interface` | gap | **GAP:** service or GPIO relationships still need a binding and resource mapping |
| `nub-swd0` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `pwm1` | gap | **GAP:** no matching hardware resource node |
| `timesync` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `fillmore` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext0` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext0` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext0/mapper-dispext0` | stream | Stream of `dart-dispext0`; stream/resource linkage needs review |
| `dart-dispext0/mapper-dispext0-piodma` | stream | Stream of `dart-dispext0`; stream/resource linkage needs review |
| `dcpext0-expert` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dcpext0` | gap | **GAP:** no matching hardware resource node |
| `dcpext0/iop-dcpext0-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext0` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext0/mapper-dcpext0` | stream | Stream of `dart-dcpext0`; stream/resource linkage needs review |
| `dp-audio1` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext1` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext1` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext1/mapper-dispext1` | stream | Stream of `dart-dispext1`; stream/resource linkage needs review |
| `dart-dispext1/mapper-dispext1-piodma` | stream | Stream of `dart-dispext1`; stream/resource linkage needs review |
| `dcpext1-expert` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dcpext1` | gap | **GAP:** no matching hardware resource node |
| `dcpext1/iop-dcpext1-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext1` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext1/mapper-dcpext1` | stream | Stream of `dart-dcpext1`; stream/resource linkage needs review |
| `dp-audio2` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext2` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext2` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext2/mapper-dispext2` | stream | Stream of `dart-dispext2`; stream/resource linkage needs review |
| `dart-dispext2/mapper-dispext2-piodma` | stream | Stream of `dart-dispext2`; stream/resource linkage needs review |
| `dcpext2-expert` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dcpext2` | gap | **GAP:** no matching hardware resource node |
| `dcpext2/iop-dcpext2-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext2` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext2/mapper-dcpext2` | stream | Stream of `dart-dcpext2`; stream/resource linkage needs review |
| `dp-audio3` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext3` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext3` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext3/mapper-dispext3` | stream | Stream of `dart-dispext3`; stream/resource linkage needs review |
| `dart-dispext3/mapper-dispext3-piodma` | stream | Stream of `dart-dispext3`; stream/resource linkage needs review |
| `dcpext3-expert` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dcpext3` | gap | **GAP:** no matching hardware resource node |
| `dcpext3/iop-dcpext3-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext3` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext3/mapper-dcpext3` | stream | Stream of `dart-dcpext3`; stream/resource linkage needs review |
| `dp-audio4` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext4` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext4` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext4/mapper-dispext4` | stream | Stream of `dart-dispext4`; stream/resource linkage needs review |
| `dart-dispext4/mapper-dispext4-piodma` | stream | Stream of `dart-dispext4`; stream/resource linkage needs review |
| `dcpext4-expert` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dcpext4` | gap | **GAP:** no matching hardware resource node |
| `dcpext4/iop-dcpext4-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext4` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext4/mapper-dcpext4` | stream | Stream of `dart-dcpext4`; stream/resource linkage needs review |
| `dp-audio6` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext5` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext5` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext5/mapper-dispext5` | stream | Stream of `dart-dispext5`; stream/resource linkage needs review |
| `dart-dispext5/mapper-dispext5-piodma` | stream | Stream of `dart-dispext5`; stream/resource linkage needs review |
| `dcpext5-expert` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dcpext5` | gap | **GAP:** no matching hardware resource node |
| `dcpext5/iop-dcpext5-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext5` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext5/mapper-dcpext5` | stream | Stream of `dart-dcpext5`; stream/resource linkage needs review |
| `dp-audio7` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext6` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext6` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext6/mapper-dispext6` | stream | Stream of `dart-dispext6`; stream/resource linkage needs review |
| `dart-dispext6/mapper-dispext6-piodma` | stream | Stream of `dart-dispext6`; stream/resource linkage needs review |
| `dcpext6-expert` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dcpext6` | gap | **GAP:** no matching hardware resource node |
| `dcpext6/iop-dcpext6-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext6` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext6/mapper-dcpext6` | stream | Stream of `dart-dcpext6`; stream/resource linkage needs review |
| `dp-audio8` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dispext7` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext7` | gap | **GAP:** no matching hardware resource node |
| `dart-dispext7/mapper-dispext7` | stream | Stream of `dart-dispext7`; stream/resource linkage needs review |
| `dart-dispext7/mapper-dispext7-piodma` | stream | Stream of `dart-dispext7`; stream/resource linkage needs review |
| `dcpext7-expert` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
| `dcpext7` | gap | **GAP:** no matching hardware resource node |
| `dcpext7/iop-dcpext7-nub` | service review | Subordinate firmware service/configuration or logical device; semantic mapping requires review |
| `dart-dcpext7` | gap | **GAP:** no matching hardware resource node |
| `dart-dcpext7/mapper-dcpext7` | stream | Stream of `dart-dcpext7`; stream/resource linkage needs review |
| `dp-audio9` | service review | No independent MMIO resource in this record; service/aggregate semantics require review |
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
| `cpus/cpu16` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu17` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu18` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu19` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu20` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu21` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu22` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu23` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu24` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu25` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu26` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu27` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu28` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu29` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu30` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
| `cpus/cpu31` | `/cpus`; core topology and harvesting require loader/compiled-DTB reconciliation. |
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
| `port-usb-c-4` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-usb-c-5` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-usb-c-6` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-usb-a-1` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-usb-a-2` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `port-hdmi1` | **GAP:** reconcile connector graph, role and GPIO dependencies per port. |
| `pcie-sdreader-helper` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `sacm` | **GAP:** non-arm-io hardware/service record requires semantic mapping. |
| `defaults` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `product` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `iboot-syscfg` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `filesystems-props` | Boot metadata; no independent hardware device. Subtree properties omitted. |
| `osenvironments` | Boot metadata; no independent hardware device. Subtree properties omitted. |
