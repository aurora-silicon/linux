# J873G device-tree audit

SoC: T8152. Source: live device ADT. Audited 459 ADT node records.
The table covers the devices directly attached to the SoC bus. Firmware
service containers, mapper children and transport endpoints are distinguished
from independent MMIO controllers. This is a resource-description draft,
not a claim that all of this board's devices can be driven on Linux.

New register complexes remain disabled. Their bindings retain the complete
aperture list and AIC interrupt wiring where translation is unambiguous;
DMA mapper references are translated to IOMMU phandles and stream IDs.
No firmware, calibration or register programming values are included.

## Remaining work

- Qualify firmware handoff, reset, clocks and power ownership before enabling
  a new block. Composite descriptions require review against the eventual
  IP-specific driver interface, including shared windows.
- Complete the transport children and firmware-service bindings, pinmux,
  port graphs and board wiring. Listing a controller does not cover its
  attached peripherals.
- Resolve every provider/routing gap called out below. No guessed
  address, interrupt parent or power-domain relationship was substituted.
- Validate on this board; no boot or peripheral test was performed.

The PMGR2 gate table requires a verified provider translation. Existing
J873 drivers remain board-specific; J804 does not inherit its fan policy,
PCIe reset wiring, GPU handoff or radio identity.

## SoC-bus coverage

| ADT device | Description | Location or unresolved detail |
| --- | --- | --- |
| displaymanager | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| lan0-sync | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| centauri | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| bluetooth | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| apcie | existing description | `&pcie`; auxiliary apertures still require a driver-specific audit. |
| dart-apcie0 | existing description | `&pcie0_dart`; auxiliary apertures still require a driver-specific audit. |
| atc0-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc1-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc3-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| display-crossbar0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| aic | existing description | `&aic`; auxiliary apertures still require a driver-specific audit. |
| soc-reporter | existing description | `&pmgr`; auxiliary apertures still require a driver-specific audit. |
| gpio0 | existing description | `&pinctrl_ap`; auxiliary apertures still require a driver-specific audit. |
| aop-exclave-ioreporting | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| pmgr-child | existing description | `&pmgr`; auxiliary apertures still require a driver-specific audit. |
| pmgr | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| smc | existing description | `&smc`; auxiliary apertures still require a driver-specific audit. |
| uart0 | unresolved binding | An IP-specific binding is still required. |
| dockchannel-uart | existing description | `&dockchannel`; auxiliary apertures still require a driver-specific audit. |
| qspi | existing description | `&qspi`; auxiliary apertures still require a driver-specific audit. |
| dp2hdmi-gpio0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dcp-sac-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio1 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| ane-exclave-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| sgx | existing description | `&gpu`; auxiliary apertures still require a driver-specific audit. |
| gfx-asc | existing description | `&gfx_asc`; auxiliary apertures still require a driver-specific audit. |
| gfx1-asc | existing description | `&gfx1_asc`; auxiliary apertures still require a driver-specific audit. |
| mapper-gfx-asc | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| iop-audio-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| smctempsensor0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| cpu-debug-interface | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| aft | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| timesync | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| fillmore | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| sapt | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| lan0-sync | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| eic | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| device-supports-gf2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dart-apcie2 | disabled resource description | `&hw_dart_apcie2` |
| apcie2-piodma | disabled resource description | `&hw_apcie2_piodma` |
| i2c0 | disabled resource description | `&hw_i2c0` |
| i2c1 | disabled resource description | `&hw_i2c1` |
| i2c3 | disabled resource description | `&hw_i2c3` |
| spi3 | disabled resource description | `&hw_spi3`; clock provider not yet translated |
| nub-spmi-a0 | disabled resource description | `&hw_nub_spmi_a0`; mixed/non-AIC interrupt routing needs decoding |
| aop-spmi0 | disabled resource description | `&hw_aop_spmi0`; mixed/non-AIC interrupt routing needs decoding |
| apcie0-piodma | disabled resource description | `&hw_apcie0_piodma` |
| acio-cpu0 | disabled resource description | `&hw_acio_cpu0` |
| apciec0 | disabled resource description | `&hw_apciec0`; power/clock gate needs a verified provider |
| dart-apciec0 | disabled resource description | `&hw_dart_apciec0` |
| apciec0-piodma | disabled resource description | `&hw_apciec0_piodma` |
| acio0 | disabled resource description | `&hw_acio0`; power/clock gate needs a verified provider |
| dart-acio0 | disabled resource description | `&hw_dart_acio0` |
| atc0-dpxbar | disabled resource description | `&hw_atc0_dpxbar` |
| atc0-dpin0 | disabled resource description | `&hw_atc0_dpin0` |
| atc0-dpin1 | disabled resource description | `&hw_atc0_dpin1` |
| acio-cpu1 | disabled resource description | `&hw_acio_cpu1` |
| apciec1 | disabled resource description | `&hw_apciec1`; power/clock gate needs a verified provider |
| dart-apciec1 | disabled resource description | `&hw_dart_apciec1` |
| apciec1-piodma | disabled resource description | `&hw_apciec1_piodma` |
| acio1 | disabled resource description | `&hw_acio1`; power/clock gate needs a verified provider |
| dart-acio1 | disabled resource description | `&hw_dart_acio1` |
| atc1-dpxbar | disabled resource description | `&hw_atc1_dpxbar` |
| atc1-dpin0 | disabled resource description | `&hw_atc1_dpin0` |
| atc1-dpin1 | disabled resource description | `&hw_atc1_dpin1` |
| acio-cpu3 | disabled resource description | `&hw_acio_cpu3` |
| apciec3 | disabled resource description | `&hw_apciec3`; power/clock gate needs a verified provider |
| dart-apciec3 | disabled resource description | `&hw_dart_apciec3` |
| apciec3-piodma | disabled resource description | `&hw_apciec3_piodma` |
| acio3 | disabled resource description | `&hw_acio3`; power/clock gate needs a verified provider |
| dart-acio3 | disabled resource description | `&hw_dart_acio3` |
| atc3-dpxbar | disabled resource description | `&hw_atc3_dpxbar` |
| atc3-dpin0 | disabled resource description | `&hw_atc3_dpin0` |
| atc3-dpin1 | disabled resource description | `&hw_atc3_dpin1` |
| mcc | disabled resource description | `&hw_mcc` |
| aic-timebase | disabled resource description | `&hw_aic_timebase` |
| wdt | disabled resource description | `&hw_wdt` |
| error-handler | disabled resource description | `&hw_error_handler` |
| soc-misc | disabled resource description | `&hw_soc_misc` |
| aes | disabled resource description | `&hw_aes`; power/clock gate needs a verified provider |
| aop-gpio | disabled resource description | `&hw_aop_gpio` |
| nub-gpio | disabled resource description | `&hw_nub_gpio` |
| aop | disabled resource description | `&hw_aop` |
| aop-exclave-mailbox | disabled resource description | `&hw_aop_exclave_mailbox` |
| aop2 | disabled resource description | `&hw_aop2` |
| event-log-handler | disabled resource description | `&hw_event_log_handler` |
| pmp | disabled resource description | `&hw_pmp` |
| sep | disabled resource description | `&hw_sep`; clock provider not yet translated |
| dart-sep | disabled resource description | `&hw_dart_sep` |
| esm | disabled resource description | `&hw_esm` |
| sio | disabled resource description | `&hw_sio`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-sio | disabled resource description | `&hw_dart_sio` |
| dart-sbr | disabled resource description | `&hw_dart_sbr` |
| ans | disabled resource description | `&hw_ans`; DMA mapper requires an IP-specific split; clock provider not yet translated |
| sart-ans | disabled resource description | `&hw_sart_ans` |
| aon-ptd | disabled resource description | `&hw_aon_ptd` |
| i2c2 | disabled resource description | `&hw_i2c2` |
| dptx-phy | disabled resource description | `&hw_dptx_phy`; power/clock gate needs a verified provider |
| atc-phy0 | disabled resource description | `&hw_atc_phy0`; power/clock gate needs a verified provider |
| dart-usb0 | disabled resource description | `&hw_dart_usb0` |
| usb-drd0 | disabled resource description | `&hw_usb_drd0` |
| atc-phy1 | disabled resource description | `&hw_atc_phy1`; power/clock gate needs a verified provider |
| dart-usb1 | disabled resource description | `&hw_dart_usb1` |
| usb-drd1 | disabled resource description | `&hw_usb_drd1` |
| atc-phy2 | disabled resource description | `&hw_atc_phy2`; power/clock gate needs a verified provider |
| dart-usb2 | disabled resource description | `&hw_dart_usb2` |
| usb-drd2 | disabled resource description | `&hw_usb_drd2` |
| atc-phy3 | disabled resource description | `&hw_atc_phy3`; power/clock gate needs a verified provider |
| dart-usb3 | disabled resource description | `&hw_dart_usb3` |
| usb-drd3 | disabled resource description | `&hw_usb_drd3` |
| nub-spmi0 | disabled resource description | `&hw_nub_spmi0`; mixed/non-AIC interrupt routing needs decoding |
| nub-spmi1 | disabled resource description | `&hw_nub_spmi1`; mixed/non-AIC interrupt routing needs decoding |
| disp0 | disabled resource description | `&hw_disp0`; clock provider not yet translated |
| dcp0-expert | disabled resource description | `&hw_dcp0_expert` |
| dcp | disabled resource description | `&hw_dcp`; power/clock gate needs a verified provider |
| dart-dcp | disabled resource description | `&hw_dart_dcp` |
| dart-disp0 | disabled resource description | `&hw_dart_disp0` |
| dart-disp0-isoc | disabled resource description | `&hw_dart_disp0_isoc` |
| admac-disp0 | disabled resource description | `&hw_admac_disp0` |
| admac-disp0-ced-ssw | disabled resource description | `&hw_admac_disp0_ced_ssw` |
| dispext0 | disabled resource description | `&hw_dispext0`; clock provider not yet translated |
| dcpext0-expert | disabled resource description | `&hw_dcpext0_expert` |
| dcpext0 | disabled resource description | `&hw_dcpext0`; power/clock gate needs a verified provider |
| dart-dcpext0 | disabled resource description | `&hw_dart_dcpext0` |
| dart-dispext0 | disabled resource description | `&hw_dart_dispext0` |
| dart-dispext0-isoc | disabled resource description | `&hw_dart_dispext0_isoc` |
| admac-dispext0 | disabled resource description | `&hw_admac_dispext0` |
| admac-dispext0-ced-ssw | disabled resource description | `&hw_admac_dispext0_ced_ssw` |
| dispext1 | disabled resource description | `&hw_dispext1`; clock provider not yet translated |
| dcpext1-expert | disabled resource description | `&hw_dcpext1_expert` |
| dcpext1 | disabled resource description | `&hw_dcpext1`; power/clock gate needs a verified provider |
| dart-dcpext1 | disabled resource description | `&hw_dart_dcpext1` |
| dart-dispext1 | disabled resource description | `&hw_dart_dispext1` |
| dart-dispext1-isoc | disabled resource description | `&hw_dart_dispext1_isoc` |
| admac-dispext1 | disabled resource description | `&hw_admac_dispext1` |
| admac-dispext1-ced-ssw | disabled resource description | `&hw_admac_dispext1_ced_ssw` |
| dptx0-aux | disabled resource description | `&hw_dptx0_aux`; power/clock gate needs a verified provider |
| dptx1-aux | disabled resource description | `&hw_dptx1_aux`; power/clock gate needs a verified provider |
| scaler0 | disabled resource description | `&hw_scaler0`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-scaler | disabled resource description | `&hw_dart_scaler` |
| jpeg0 | disabled resource description | `&hw_jpeg0`; power/clock gate needs a verified provider; clock provider not yet translated |
| jpeg1 | disabled resource description | `&hw_jpeg1`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-jpeg | disabled resource description | `&hw_dart_jpeg` |
| ave | disabled resource description | `&hw_ave`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-ave | disabled resource description | `&hw_dart_ave` |
| avd | disabled resource description | `&hw_avd`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-avd | disabled resource description | `&hw_dart_avd` |
| apr | disabled resource description | `&hw_apr`; power/clock gate needs a verified provider |
| dart-apr | disabled resource description | `&hw_dart_apr` |
| ane | disabled resource description | `&hw_ane`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-ane | disabled resource description | `&hw_dart_ane` |
| ane1 | disabled resource description | `&hw_ane1`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-ane1 | disabled resource description | `&hw_dart_ane1` |
| admac-base-ns | disabled resource description | `&hw_admac_base_ns`; power/clock gate needs a verified provider |
| admac-leap-ns | disabled resource description | `&hw_admac_leap_ns`; power/clock gate needs a verified provider |
| admac-aop-ced-ssw | disabled resource description | `&hw_admac_aop_ced_ssw` |
| dbgCPU | disabled resource description | `&hw_dbgCPU` |
| dart-tlbi-aperture | disabled resource description | `&hw_dart_tlbi_aperture` |
| apple-processor-trace | disabled resource description | `&hw_apple_processor_trace` |
| rsm (static template only) | disabled resource description | `&hw_rsm`; absent from the preferred live snapshot, kept disabled. |

## Child devices and service interfaces

These entries require an endpoint/transport audit in addition to the parent
controller resources above. Mapper records describe stream IDs rather than
independently probed devices. Conditional template records and their tuning
containers are not separate hardware devices; select the silicon variant
using a resolved live tree before changing resources. No tuning properties
from those records are reproduced here.

| Hardware path below the SoC bus | Classification and remaining audit |
| --- | --- |
| `dart-apcie2/mapper-apcie2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apcie2/mapper-apcie2-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `i2c1/aqc-a` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `i2c1/audio-codec-output` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `i2c3/audio-speaker` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi3/usb3hub-flash` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi3/usb3hub-flash/usb3hub-fw` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi-a0/hpm0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi-a0/hpm1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi-a0/hpm3` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop-spmi0/stockholm-spmi` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop-spmi0/stockholm-spmi/stockholm` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie/pci-bridge0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie/pci-bridge0/centauri-control` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie/pci-bridge0/centauri-alpha` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie/pci-bridge0/centauri-beta` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie/pci-bridge2` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-apcie0/mapper-apcie0-control` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apcie0/mapper-apcie0-alpha` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apcie0/mapper-apcie0-beta` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apcie0/mapper-apcie0-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `acio-cpu0/iop-acio0-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apciec0/pcic0-bridge` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-apciec0/mapper-apciec0-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-8` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-9` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-10` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-tx-11` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-8` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-9` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-10` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio0/mapper-acio0-rx-11` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `acio-cpu1/iop-acio1-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apciec1/pcic1-bridge` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-apciec1/mapper-apciec1-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-8` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-9` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-10` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-tx-11` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-8` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-9` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-10` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio1/mapper-acio1-rx-11` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `acio-cpu3/iop-acio3-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apciec3/pcic3-bridge` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-apciec3/mapper-apciec3-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-8` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-9` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-10` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-tx-11` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-8` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-9` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-10` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio3/mapper-acio3-rx-11` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `error-handler/dram-sac` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/aop-audio` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/accel` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/gyro` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/als` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop2/iop-aop2-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `pmgr-child/soc-tuner` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `pmgr/clpc` | Power/firmware interface; provider topology and handoff pending. |
| `pmgr/pmgrtools` | Power/firmware interface; provider topology and handoff pending. |
| `pmgr/ppm` | Power/firmware interface; provider topology and handoff pending. |
| `pmp/iop-pmp-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `sep/iop-sep-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-sep/mapper-sep-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sep/mapper-sep` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `sio/iop-sio-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `sio/iop-sio-nub/sio-dma` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-sio/mapper-sio` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sio/mapper-aes` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-aop` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-aop-scm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-aop2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-exclave-aop` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-exclave-aop2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-mtp` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-pmp` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-admac-leap-ns` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-admac-base-ns` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-admac-leap-s` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-admac-base-s` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sbr/mapper-rsm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `ans/iop-ans-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-pmu` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-charger-util-0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-charger-util-1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-charger-util-3` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `qspi/spinor` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `qspi/spinor/firmware` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `qspi/spinor/anvram` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `qspi/spinor/syscfg` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `i2c2/atcrt0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `i2c2/atcrt1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `i2c2/atcrt3` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-usb0/mapper-usb0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-usb1/mapper-usb1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-usb2/mapper-usb2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `usb-drd2/usb-drd2-port-hs/usb2-hub-port1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `usb-drd2/usb-drd2-port-hs/usb2-hub-port4` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `usb-drd2/usb-drd2-port-ss/usb3-hub-port1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `usb-drd2/usb-drd2-port-ss/usb3-hub-port4` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-usb3/mapper-usb3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `nub-spmi0/pmu-main` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi0/btm` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi0/spmi-mosquito` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi1/pmu-aux` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dcp/iop-dcp-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-dcp/mapper-dcp` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dcp/mapper-dcp-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0/mapper-disp0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0/mapper-disp0-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0/mapper-disp0-scl` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0/mapper-disp0-sca` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0-isoc/mapper-disp0-dpa` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dcpext0/iop-dcpext0-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-dcpext0/mapper-dcpext0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dcpext0/mapper-dcpext0-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext0/mapper-dispext0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext0/mapper-dispext0-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext0-isoc/mapper-dispext0-dpa` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dcpext1/iop-dcpext1-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-dcpext1/mapper-dcpext1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dcpext1/mapper-dcpext1-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext1/mapper-dispext1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext1/mapper-dispext1-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext1-isoc/mapper-dispext1-dpa` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dptx0-aux/dptx0-aux-core` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dptx0-aux/dptx0-aux-ufp` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dptx1-aux/dptx1-aux-core` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dptx1-aux/dptx1-aux-ufp` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-scaler/mapper-scaler` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-scaler/mapper-scaler-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-jpeg/mapper-jpeg0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-jpeg/mapper-jpeg1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ave/mapper-ave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-avd/mapper-avd` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-avd/mapper-avd-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-avd/mapper-avd-adsbuf` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apr/mapper-apr` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `ane/iop-ane-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-ane/mapper-ane` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-mpm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `ane1/iop-ane1-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-ane1/mapper-ane1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane1/mapper-ane-mpm1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `gfx-asc/iop-gfx-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `gfx1-asc/iop-gfx1-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-input` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-input/audio-codec-input` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-output` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-output/audio-codec-output` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-mca-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-mca-loopback/audio-mca-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-speaker` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-speaker/audio-speaker` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `admac-base-ns/base-ns-adma-tx-1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `admac-base-ns/base-ns-adma-rx-1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `admac-base-ns/base-ns-adma-tx-4` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `admac-base-ns/base-ns-adma-tx-5` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `admac-base-ns/base-ns-adma-rx-5` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `admac-base-ns/base-ns-adma-tx-6` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `admac-base-ns/base-ns-adma-rx-6` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
