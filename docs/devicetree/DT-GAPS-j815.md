# J815 device-tree audit

SoC: T8142. Source: board-specific static ADT. Audited 506 ADT node records.
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

## SoC-bus coverage

| ADT device | Description | Location or unresolved detail |
| --- | --- | --- |
| iop-voicetrigger-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| centauri | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| displaymanager | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| bluetooth | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| aod2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc0-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc1-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| display-crossbar0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| aic | existing description | `&aic`; auxiliary apertures still require a driver-specific audit. |
| aop-exclave-ioreporting | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| pmgrtools | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| uart0 | existing description | `&serial0`; auxiliary apertures still require a driver-specific audit. |
| dockchannel-uart | existing description | `&dockchannel`; auxiliary apertures still require a driver-specific audit. |
| mtp-aop-mux | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dcp-sac-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio1 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| mapper-gfx-asc | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| iop-audio-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| audio-hp-mic-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| audio-shared-dart-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| smctempsensor0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| cpu-debug-interface | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| aft | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| timesync | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| fillmore | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| sapt | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| isp-exclave-s-proxy | unresolved address translation | No covering parent range; do not guess a physical address. |
| isp-exclave-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| eic | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| device-supports-gf2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| spi2 | disabled resource description | `&hw_spi2`; clock provider not yet translated |
| spi4 | disabled resource description | `&hw_spi4`; clock provider not yet translated |
| i2c1 | disabled resource description | `&hw_i2c1` |
| i2c3 | disabled resource description | `&hw_i2c3` |
| fpwm0 | disabled resource description | `&hw_fpwm0` |
| aop-spmi0 | disabled resource description | `&hw_aop_spmi0`; mixed/non-AIC interrupt routing needs decoding |
| apcie | disabled resource description | `&hw_apcie` |
| dart-apcie0 | disabled resource description | `&hw_dart_apcie0` |
| apcie0-piodma | disabled resource description | `&hw_apcie0_piodma` |
| acio-cpu0 | disabled resource description | `&hw_acio_cpu0` |
| apciec0 | disabled resource description | `&hw_apciec0`; power/clock gate needs a verified provider |
| dart-apciec0 | disabled resource description | `&hw_dart_apciec0` |
| apciec0-piodma | disabled resource description | `&hw_apciec0_piodma` |
| acio0 | disabled resource description | `&hw_acio0` |
| dart-acio0 | disabled resource description | `&hw_dart_acio0` |
| atc0-dpxbar | disabled resource description | `&hw_atc0_dpxbar` |
| atc0-dpin0 | disabled resource description | `&hw_atc0_dpin0` |
| atc0-dpin1 | disabled resource description | `&hw_atc0_dpin1` |
| acio-cpu1 | disabled resource description | `&hw_acio_cpu1` |
| apciec1 | disabled resource description | `&hw_apciec1`; power/clock gate needs a verified provider |
| dart-apciec1 | disabled resource description | `&hw_dart_apciec1` |
| apciec1-piodma | disabled resource description | `&hw_apciec1_piodma` |
| acio1 | disabled resource description | `&hw_acio1` |
| dart-acio1 | disabled resource description | `&hw_dart_acio1` |
| atc1-dpxbar | disabled resource description | `&hw_atc1_dpxbar` |
| atc1-dpin0 | disabled resource description | `&hw_atc1_dpin0` |
| atc1-dpin1 | disabled resource description | `&hw_atc1_dpin1` |
| mcc | disabled resource description | `&hw_mcc` |
| aic-timebase | disabled resource description | `&hw_aic_timebase` |
| wdt | disabled resource description | `&hw_wdt` |
| error-handler | disabled resource description | `&hw_error_handler` |
| aes | disabled resource description | `&hw_aes` |
| gpio0 | disabled resource description | `&hw_gpio0` |
| aop-gpio | disabled resource description | `&hw_aop_gpio` |
| nub-gpio | disabled resource description | `&hw_nub_gpio` |
| smc-gpio | disabled resource description | `&hw_smc_gpio` |
| aop | disabled resource description | `&hw_aop` |
| dart-aop | disabled resource description | `&hw_dart_aop` |
| aop-exclave-mailbox | disabled resource description | `&hw_aop_exclave_mailbox` |
| aop2 | disabled resource description | `&hw_aop2` |
| pmgr | disabled resource description | `&hw_pmgr` |
| event-log-handler | disabled resource description | `&hw_event_log_handler` |
| pmp | disabled resource description | `&hw_pmp` |
| dart-pmp | disabled resource description | `&hw_dart_pmp` |
| sep | disabled resource description | `&hw_sep`; clock provider not yet translated |
| dart-sep | disabled resource description | `&hw_dart_sep` |
| esm | disabled resource description | `&hw_esm` |
| sio | disabled resource description | `&hw_sio`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-sio | disabled resource description | `&hw_dart_sio` |
| ans | disabled resource description | `&hw_ans`; DMA mapper requires an IP-specific split; clock provider not yet translated |
| sart-ans | disabled resource description | `&hw_sart_ans` |
| smc | disabled resource description | `&hw_smc` |
| aon-ptd | disabled resource description | `&hw_aon_ptd` |
| rsm | disabled resource description | `&hw_rsm` |
| dart-rsm | disabled resource description | `&hw_dart_rsm` |
| dockchannel-mtp | disabled resource description | `&hw_dockchannel_mtp` |
| mtp | disabled resource description | `&hw_mtp` |
| dart-mtp | disabled resource description | `&hw_dart_mtp` |
| qspi | disabled resource description | `&hw_qspi`; clock provider not yet translated |
| i2c2 | disabled resource description | `&hw_i2c2` |
| atc-phy0 | disabled resource description | `&hw_atc_phy0`; power/clock gate needs a verified provider |
| dart-usb0 | disabled resource description | `&hw_dart_usb0`; power/clock gate needs a verified provider |
| usb-drd0 | disabled resource description | `&hw_usb_drd0`; power/clock gate needs a verified provider |
| atc-phy1 | disabled resource description | `&hw_atc_phy1`; power/clock gate needs a verified provider |
| dart-usb1 | disabled resource description | `&hw_dart_usb1`; power/clock gate needs a verified provider |
| usb-drd1 | disabled resource description | `&hw_usb_drd1`; power/clock gate needs a verified provider |
| nub-spmi0 | disabled resource description | `&hw_nub_spmi0`; mixed/non-AIC interrupt routing needs decoding |
| nub-spmi1 | disabled resource description | `&hw_nub_spmi1`; mixed/non-AIC interrupt routing needs decoding |
| nub-spmi-a0 | disabled resource description | `&hw_nub_spmi_a0`; mixed/non-AIC interrupt routing needs decoding |
| disp0 | disabled resource description | `&hw_disp0`; power/clock gate needs a verified provider; clock provider not yet translated |
| dcp0-expert | disabled resource description | `&hw_dcp0_expert`; power/clock gate needs a verified provider |
| dcp | disabled resource description | `&hw_dcp`; power/clock gate needs a verified provider |
| dart-dcp | disabled resource description | `&hw_dart_dcp` |
| dart-disp0 | disabled resource description | `&hw_dart_disp0` |
| dart-disp0-isoc | disabled resource description | `&hw_dart_disp0_isoc` |
| admac-disp0 | disabled resource description | `&hw_admac_disp0`; power/clock gate needs a verified provider |
| admac-disp0-ced-ssw | disabled resource description | `&hw_admac_disp0_ced_ssw` |
| dispext0 | disabled resource description | `&hw_dispext0`; clock provider not yet translated |
| dcpext0-expert | disabled resource description | `&hw_dcpext0_expert` |
| dcpext0 | disabled resource description | `&hw_dcpext0`; power/clock gate needs a verified provider |
| dart-dcpext0 | disabled resource description | `&hw_dart_dcpext0` |
| dart-dispext0 | disabled resource description | `&hw_dart_dispext0` |
| dart-dispext0-isoc | disabled resource description | `&hw_dart_dispext0_isoc` |
| admac-dispext0 | disabled resource description | `&hw_admac_dispext0`; power/clock gate needs a verified provider |
| admac-dispext0-ced-ssw | disabled resource description | `&hw_admac_dispext0_ced_ssw` |
| dispext1 | disabled resource description | `&hw_dispext1`; clock provider not yet translated |
| dcpext1-expert | disabled resource description | `&hw_dcpext1_expert` |
| dcpext1 | disabled resource description | `&hw_dcpext1`; power/clock gate needs a verified provider |
| dart-dcpext1 | disabled resource description | `&hw_dart_dcpext1` |
| dart-dispext1 | disabled resource description | `&hw_dart_dispext1` |
| dart-dispext1-isoc | disabled resource description | `&hw_dart_dispext1_isoc` |
| admac-dispext1 | disabled resource description | `&hw_admac_dispext1`; power/clock gate needs a verified provider |
| admac-dispext1-ced-ssw | disabled resource description | `&hw_admac_dispext1_ced_ssw` |
| dptx0-aux | disabled resource description | `&hw_dptx0_aux` |
| dptx1-aux | disabled resource description | `&hw_dptx1_aux` |
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
| scdc | disabled resource description | `&hw_scdc` |
| dart-scdc | disabled resource description | `&hw_dart_scdc` |
| isp | disabled resource description | `&hw_isp`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-isp | disabled resource description | `&hw_dart_isp` |
| ane | disabled resource description | `&hw_ane`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-ane | disabled resource description | `&hw_dart_ane` |
| sgx | disabled resource description | `&hw_sgx`; power/clock gate needs a verified provider; clock provider not yet translated |
| gfx-asc | disabled resource description | `&hw_gfx_asc`; DMA mapper requires an IP-specific split, power/clock gate needs a verified provider; clock provider not yet translated |
| gfx1-asc | disabled resource description | `&hw_gfx1_asc`; DMA mapper requires an IP-specific split, power/clock gate needs a verified provider; clock provider not yet translated |
| admac-base-ns | disabled resource description | `&hw_admac_base_ns`; power/clock gate needs a verified provider |
| admac-leap-ns | disabled resource description | `&hw_admac_leap_ns`; power/clock gate needs a verified provider |
| admac-aop-ced-ssw | disabled resource description | `&hw_admac_aop_ced_ssw` |
| admac-leap-s | disabled resource description | `&hw_admac_leap_s`; power/clock gate needs a verified provider |
| admac-leap-s-rx-0-proxy | disabled resource description | `&hw_admac_leap_s_rx_0_proxy` |
| dbgCPU | disabled resource description | `&hw_dbgCPU` |
| secure-repair | disabled resource description | `&hw_secure_repair` |
| apple-processor-trace | disabled resource description | `&hw_apple_processor_trace` |
| hib-exclave-watchdog | disabled resource description | `&hw_hib_exclave_watchdog` |

## Child devices and service interfaces

These entries require an endpoint/transport audit in addition to the parent
controller resources above. Mapper records describe stream IDs rather than
independently probed devices. Conditional template records and their tuning
containers are not separate hardware devices; select the silicon variant
using a resolved live tree before changing resources. No tuning properties
from those records are reproduced here.

| Hardware path below the SoC bus | Classification and remaining audit |
| --- | --- |
| `spi2/mesa` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `spi4/dp855` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `spi4/dp855/lcd-pmicwp` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/dp855/lcd-pmic` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/dp855/lcd-sswp` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/dp855/lcd-ss` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/dp855/tcon-registers` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/dp855/lcd-eeprom` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `i2c1/audio-codec-output` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c1/mogul-las` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c3/audio-speaker` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c3/audio-speaker-left-woofer-2` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c3/audio-speaker-left-tweeter` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c3/audio-speaker-right-woofer-1` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c3/audio-speaker-right-woofer-2` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c3/audio-speaker-right-tweeter` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `fpwm0/kbd-backlight` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop-spmi0/stockholm-spmi` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop-spmi0/stockholm-spmi/stockholm` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-voicetrigger-controller/audio-vt-device` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie/pci-bridge0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie/pci-bridge0/centauri-control` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie/pci-bridge0/centauri-alpha` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie/pci-bridge0/centauri-beta` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
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
| `aop/iop-aop-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/aop-audio` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/aop-voicetrigger` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/accel` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/gyro` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/als` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/aop-gapf-filters` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-aop/mapper-aop` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-scm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-exclave-aop` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-aop2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-admac-base-ns` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-admac-leap-ns` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-admac-base-s` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-admac-leap-s` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `aop2/iop-aop2-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `pmgr/clpc` | Power/firmware interface; provider topology and handoff pending. |
| `pmgr/soc-tuner` | Power/firmware interface; provider topology and handoff pending. |
| `pmgr/ppm` | Power/firmware interface; provider topology and handoff pending. |
| `pmp/iop-pmp-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-pmp/mapper-pmp` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `sep/iop-sep-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-sep/mapper-sep` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sep/mapper-sep-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sep/mapper-sep-mpm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `sio/iop-sio-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `sio/iop-sio-nub/sio-dma` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-sio/mapper-sio` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sio/mapper-aes` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `ans/iop-ans-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-pmu` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-charger-util-0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-charger-util-1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-charger-util` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-rsm/mapper-rsm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dockchannel-mtp/mtp-transport` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/multi-touch` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/keyboard` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/stm` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/actuator` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/tp-accel` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `mtp/iop-mtp-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-mtp/mapper-mtp` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `qspi/spinor` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `qspi/spinor/firmware` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `qspi/spinor/anvram` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `qspi/spinor/syscfg` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `i2c2/atcrt0` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c2/atcrt1` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `dart-usb0/mapper-usb0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `usb-drd0/usb-drd0-port-hs` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `usb-drd0/usb-drd0-port-ss` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-usb1/mapper-usb1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `usb-drd1/usb-drd1-port-hs` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `usb-drd1/usb-drd1-port-ss` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi0/pmu-main` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi0/btm` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi1/pmu-aux` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi-a0/hpm0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi-a0/hpm1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi-a0/hpm5` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dcp/iop-dcp-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-dcp/mapper-dcp` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0/mapper-disp0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0/mapper-disp0-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0-isoc/mapper-disp0-dpa` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dcpext0/iop-dcpext0-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-dcpext0/mapper-dcpext0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext0/mapper-dispext0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext0/mapper-dispext0-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext0-isoc/mapper-dispext0-dpa` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dcpext1/iop-dcpext1-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-dcpext1/mapper-dcpext1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
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
| `dart-apr/mapper-apr-scdc` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apr/mapper-apr-ve` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `scdc/iop-scdc-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-scdc/mapper-scdc` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp/mapper-isp` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp/mapper-isp-mpm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp/mapper-isp-epipe0-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp/mapper-isp-epipe1-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp/mapper-isp-piodma0-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp/mapper-isp-piodma1-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-iso7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane/mapper-ane-mpm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `gfx-asc/iop-gfx-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `gfx1-asc/iop-gfx1-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-hp-mic` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-hp-mic/audio-hp-mic` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-mca-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-mca-loopback/audio-mca-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-leap-internal-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-leap-internal-loopback/audio-leap-internal-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-speaker` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-speaker/audio-speaker` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-speaker-tap` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-speaker-tap/audio-speaker-tap` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-output` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-output/audio-codec-output` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-input` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-input/audio-codec-input` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/lp-mic-device` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/lp-mic-io-buffer-device` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/siri-listening-device` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/lp-mic-injection-device` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/lp-mic-injection-device/lp-mic-injection-device` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `admac-base-ns/base-ns-adma-tx-0` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-base-ns/base-ns-adma-rx-0` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-base-ns/base-ns-adma-rx-1` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-base-ns/base-ns-adma-tx-2` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-base-ns/base-ns-adma-rx-2` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-base-ns/base-ns-adma-tx-3` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-base-ns/base-ns-adma-rx-3` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-base-ns/base-ns-adma-tx-5` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-leap-ns/leap-ns-adma-rx-0` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-leap-ns/leap-ns-adma-tx-4` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-leap-ns/leap-ns-adma-rx-4` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-leap-s/leap-s-adma-rx-0` | Subdevice register interface; binding, parent translation and ownership audit pending. |
