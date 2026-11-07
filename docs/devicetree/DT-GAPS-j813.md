# J813 device-tree audit

SoC: T8142. Source: live device ADT. Audited 504 ADT node records.
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
| i2c1 | existing description | `&i2c1`; auxiliary apertures still require a driver-specific audit. |
| i2c3 | existing description | `&i2c3`; auxiliary apertures still require a driver-specific audit. |
| fpwm0 | existing description | `&fpwm0`; auxiliary apertures still require a driver-specific audit. |
| iop-voicetrigger-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| centauri | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| displaymanager | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| bluetooth | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| apcie | existing description | `&pcie0`; auxiliary apertures still require a driver-specific audit. |
| dart-apcie0 | existing description | `&dart_apcie0`; auxiliary apertures still require a driver-specific audit. |
| aod2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc0-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc1-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| display-crossbar0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| aic | existing description | `&aic`; auxiliary apertures still require a driver-specific audit. |
| wdt | existing description | `&wdt`; auxiliary apertures still require a driver-specific audit. |
| gpio0 | existing description | `&pinctrl_ap`; auxiliary apertures still require a driver-specific audit. |
| nub-gpio | existing description | `&pinctrl_nub`; auxiliary apertures still require a driver-specific audit. |
| aop | existing description | `&aop`; auxiliary apertures still require a driver-specific audit. |
| dart-aop | existing description | `&aop_dart`; auxiliary apertures still require a driver-specific audit. |
| aop-exclave-ioreporting | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| pmgr | existing description | `&pmgr`; auxiliary apertures still require a driver-specific audit. |
| event-log-handler | existing description | `&pmgr`; auxiliary apertures still require a driver-specific audit. |
| pmp | existing description | `&pmp`; auxiliary apertures still require a driver-specific audit. |
| dart-pmp | existing description | `&pmp_dart`; auxiliary apertures still require a driver-specific audit. |
| ans | existing description | `&nvme`; auxiliary apertures still require a driver-specific audit. |
| sart-ans | existing description | `&sart`; auxiliary apertures still require a driver-specific audit. |
| smc | existing description | `&smc`; auxiliary apertures still require a driver-specific audit. |
| pmgrtools | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| uart0 | existing description | `&serial0`; auxiliary apertures still require a driver-specific audit. |
| dockchannel-uart | existing description | `&dockchannel`; auxiliary apertures still require a driver-specific audit. |
| dockchannel-mtp | existing description | `&mtp_dockchannel`; auxiliary apertures still require a driver-specific audit. |
| mtp | existing description | `&mtp`; auxiliary apertures still require a driver-specific audit. |
| dart-mtp | existing description | `&mtp_dart`; auxiliary apertures still require a driver-specific audit. |
| mtp-aop-mux | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| qspi | existing description | `&qspi`; auxiliary apertures still require a driver-specific audit. |
| atc-phy0 | existing description | `&atcphy0`; auxiliary apertures still require a driver-specific audit. |
| dart-usb0 | existing description | `&dwc3_0_dart_0`; auxiliary apertures still require a driver-specific audit. |
| usb-drd0 | existing description | `&dwc3_0`; auxiliary apertures still require a driver-specific audit. |
| atc-phy1 | existing description | `&atcphy1`; auxiliary apertures still require a driver-specific audit. |
| dart-usb1 | existing description | `&dwc3_1_dart_0`; auxiliary apertures still require a driver-specific audit. |
| usb-drd1 | existing description | `&dwc3_1`; auxiliary apertures still require a driver-specific audit. |
| nub-spmi0 | existing description | `&nub_spmi0`; auxiliary apertures still require a driver-specific audit. |
| nub-spmi-a0 | existing description | `&nub_spmi_a0`; auxiliary apertures still require a driver-specific audit. |
| disp0 | existing description | `&dcp`; auxiliary apertures still require a driver-specific audit. |
| dcp | existing description | `&dcp`; auxiliary apertures still require a driver-specific audit. |
| dart-dcp | existing description | `&dcp_dart`; auxiliary apertures still require a driver-specific audit. |
| dart-disp0 | existing description | `&disp0_dart`; auxiliary apertures still require a driver-specific audit. |
| dcp-sac-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio1 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dart-avd | existing description | `&avd_dart`; auxiliary apertures still require a driver-specific audit. |
| isp | existing description | `&isp`; auxiliary apertures still require a driver-specific audit. |
| dart-isp | existing description | `&isp_dart0`; auxiliary apertures still require a driver-specific audit. |
| sgx | existing description | `&gpu`; auxiliary apertures still require a driver-specific audit. |
| gfx-asc | existing description | `&gfx_mbox`; auxiliary apertures still require a driver-specific audit. |
| gfx1-asc | existing description | `&gfx1_mbox`; auxiliary apertures still require a driver-specific audit. |
| mapper-gfx-asc | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| iop-audio-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| admac-base-ns | existing description | `&admac_base_ns`; auxiliary apertures still require a driver-specific audit. |
| audio-hp-mic-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| audio-shared-dart-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| smctempsensor0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| cpu-debug-interface | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| aft | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| timesync | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| fillmore | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| sapt | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| secure-repair | existing description | `&pinctrl_nub`; auxiliary apertures still require a driver-specific audit. |
| isp-exclave-s-proxy | unresolved address translation | No covering parent range; do not guess a physical address. |
| isp-exclave-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| eic | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| device-supports-gf2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| spi2 | disabled resource description | `&hw_spi2` |
| spi4 | disabled resource description | `&hw_spi4` |
| aop-spmi0 | disabled resource description | `&hw_aop_spmi0`; mixed/non-AIC interrupt routing needs decoding |
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
| error-handler | disabled resource description | `&hw_error_handler` |
| aes | disabled resource description | `&hw_aes` |
| aop-gpio | disabled resource description | `&hw_aop_gpio` |
| smc-gpio | disabled resource description | `&hw_smc_gpio` |
| aop-exclave-mailbox | disabled resource description | `&hw_aop_exclave_mailbox` |
| aop2 | disabled resource description | `&hw_aop2` |
| sep | disabled resource description | `&hw_sep` |
| dart-sep | disabled resource description | `&hw_dart_sep` |
| esm | disabled resource description | `&hw_esm` |
| sio | disabled resource description | `&hw_sio`; power/clock gate needs a verified provider |
| dart-sio | disabled resource description | `&hw_dart_sio` |
| aon-ptd | disabled resource description | `&hw_aon_ptd` |
| rsm | disabled resource description | `&hw_rsm` |
| dart-rsm | disabled resource description | `&hw_dart_rsm` |
| i2c2 | disabled resource description | `&hw_i2c2` |
| nub-spmi1 | disabled resource description | `&hw_nub_spmi1`; mixed/non-AIC interrupt routing needs decoding |
| dcp0-expert | disabled resource description | `&hw_dcp0_expert`; power/clock gate needs a verified provider |
| dart-disp0-isoc | disabled resource description | `&hw_dart_disp0_isoc` |
| admac-disp0 | disabled resource description | `&hw_admac_disp0`; power/clock gate needs a verified provider |
| admac-disp0-ced-ssw | disabled resource description | `&hw_admac_disp0_ced_ssw` |
| dispext0 | disabled resource description | `&hw_dispext0` |
| dcpext0-expert | disabled resource description | `&hw_dcpext0_expert` |
| dcpext0 | disabled resource description | `&hw_dcpext0`; power/clock gate needs a verified provider |
| dart-dcpext0 | disabled resource description | `&hw_dart_dcpext0` |
| dart-dispext0 | disabled resource description | `&hw_dart_dispext0` |
| dart-dispext0-isoc | disabled resource description | `&hw_dart_dispext0_isoc` |
| admac-dispext0 | disabled resource description | `&hw_admac_dispext0`; power/clock gate needs a verified provider |
| admac-dispext0-ced-ssw | disabled resource description | `&hw_admac_dispext0_ced_ssw` |
| dispext1 | disabled resource description | `&hw_dispext1` |
| dcpext1-expert | disabled resource description | `&hw_dcpext1_expert` |
| dcpext1 | disabled resource description | `&hw_dcpext1`; power/clock gate needs a verified provider |
| dart-dcpext1 | disabled resource description | `&hw_dart_dcpext1` |
| dart-dispext1 | disabled resource description | `&hw_dart_dispext1` |
| dart-dispext1-isoc | disabled resource description | `&hw_dart_dispext1_isoc` |
| admac-dispext1 | disabled resource description | `&hw_admac_dispext1`; power/clock gate needs a verified provider |
| admac-dispext1-ced-ssw | disabled resource description | `&hw_admac_dispext1_ced_ssw` |
| dptx0-aux | disabled resource description | `&hw_dptx0_aux` |
| dptx1-aux | disabled resource description | `&hw_dptx1_aux` |
| scaler0 | disabled resource description | `&hw_scaler0`; power/clock gate needs a verified provider |
| dart-scaler | disabled resource description | `&hw_dart_scaler` |
| jpeg0 | disabled resource description | `&hw_jpeg0`; power/clock gate needs a verified provider |
| jpeg1 | disabled resource description | `&hw_jpeg1`; power/clock gate needs a verified provider |
| dart-jpeg | disabled resource description | `&hw_dart_jpeg` |
| ave | disabled resource description | `&hw_ave`; power/clock gate needs a verified provider |
| dart-ave | disabled resource description | `&hw_dart_ave` |
| avd | disabled resource description | `&hw_avd`; power/clock gate needs a verified provider |
| apr | disabled resource description | `&hw_apr`; power/clock gate needs a verified provider |
| dart-apr | disabled resource description | `&hw_dart_apr` |
| scdc | disabled resource description | `&hw_scdc` |
| dart-scdc | disabled resource description | `&hw_dart_scdc` |
| ane | disabled resource description | `&hw_ane`; power/clock gate needs a verified provider |
| dart-ane | disabled resource description | `&hw_dart_ane` |
| admac-leap-ns | disabled resource description | `&hw_admac_leap_ns`; power/clock gate needs a verified provider |
| admac-aop-ced-ssw | disabled resource description | `&hw_admac_aop_ced_ssw` |
| admac-leap-s | disabled resource description | `&hw_admac_leap_s`; power/clock gate needs a verified provider |
| admac-leap-s-rx-0-proxy | disabled resource description | `&hw_admac_leap_s_rx_0_proxy` |
| dbgCPU | disabled resource description | `&hw_dbgCPU` |
| apple-processor-trace | disabled resource description | `&hw_apple_processor_trace` |
| hib-exclave-watchdog | disabled resource description | `&hw_hib_exclave_watchdog` |
