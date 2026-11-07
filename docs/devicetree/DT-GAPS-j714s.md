# J714S device-tree audit

SoC: T6050. Source: board-specific static ADT. Audited 1066 ADT node records.
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

Existing SoC and board support comes from linux#935, linux#945 and
linux#1025; these dependencies are extended, not replaced. The static ADTs
contain template CPU records for absent dies/cores. CPU harvesting and
release addresses must be supplied by the boot loader.

## SoC-bus coverage

| ADT device | Description | Location or unresolved detail |
| --- | --- | --- |
| dynamic-pil | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| displaymanager | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| i2c1 | existing description | `&i2c1`; auxiliary apertures still require a driver-specific audit. |
| i2c2 | existing description | `&i2c2`; auxiliary apertures still require a driver-specific audit. |
| iop-voicetrigger-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| isp0 | unresolved binding | An IP-specific binding is still required. |
| isp-exclave-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| pwm0 | existing description | `&fpwm0`; auxiliary apertures still require a driver-specific audit. |
| apcie0 | existing description | `&pcie0`; auxiliary apertures still require a driver-specific audit. |
| dart-apcie0 | existing description | `&dart_apcie0`; auxiliary apertures still require a driver-specific audit. |
| acio-phy-cpu0 | unresolved binding | An IP-specific binding is still required. |
| acio-phy-cpu1 | existing description | `&atc1_phy_mbox`; auxiliary apertures still require a driver-specific audit. |
| acio-phy-cpu2 | existing description | `&atc2_phy_mbox`; auxiliary apertures still require a driver-specific audit. |
| acio-phy-cpu3 | existing description | `&atc3_phy_mbox`; auxiliary apertures still require a driver-specific audit. |
| mtp | existing description | `&mtp`; auxiliary apertures still require a driver-specific audit. |
| dart-mtp | existing description | `&mtp_dart`; auxiliary apertures still require a driver-specific audit. |
| mtp-aop-mux | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| eic | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| aod2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| bluetooth | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| centauri | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| display-crossbar0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc0-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc1-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc2-dpxbar | existing description | `&atc2_xbar`; auxiliary apertures still require a driver-specific audit. |
| atc2-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc3-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| mcc | existing description | `&cio_reconfig0`; auxiliary apertures still require a driver-specific audit. |
| aic | existing description | `&aic`; auxiliary apertures still require a driver-specific audit. |
| wdt | existing description | `&wdt`; auxiliary apertures still require a driver-specific audit. |
| error-handler | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| auss-cpu0 | unresolved binding | An IP-specific binding is still required. |
| gpio0 | existing description | `&pinctrl_ap`; auxiliary apertures still require a driver-specific audit. |
| aop-gpio0 | existing description | `&pinctrl_aop`; auxiliary apertures still require a driver-specific audit. |
| aop | existing description | `&aop`; auxiliary apertures still require a driver-specific audit. |
| dart-aop | existing description | `&aop_dart`; auxiliary apertures still require a driver-specific audit. |
| aop-exclave-ioreporting | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| pmgr | existing description | `&pmgr0`; auxiliary apertures still require a driver-specific audit. |
| pmp1 | unresolved address translation | No covering parent range; do not guess a physical address. |
| dart-pmp1 | unresolved address translation | No covering parent range; do not guess a physical address. |
| esm | unresolved address translation | No covering parent range; do not guess a physical address. |
| ans | existing description | `&nvme`; auxiliary apertures still require a driver-specific audit. |
| sart-ans | existing description | `&ans_sart`; auxiliary apertures still require a driver-specific audit. |
| smc | existing description | `&smc`; auxiliary apertures still require a driver-specific audit. |
| uart0 | unresolved binding | An IP-specific binding is still required. |
| dockchannel-uart | existing description | `&dockchannel`; auxiliary apertures still require a driver-specific audit. |
| qspi | existing description | `&qspi`; auxiliary apertures still require a driver-specific audit. |
| dp2hdmi-gpio0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc-phy1 | existing description | `&atc1_phy`; auxiliary apertures still require a driver-specific audit. |
| dart-usb1 | existing description | `&usb_drd1_dart_0`; auxiliary apertures still require a driver-specific audit. |
| usb-drd1 | existing description | `&usb_drd1`; auxiliary apertures still require a driver-specific audit. |
| atc-phy2 | existing description | `&atc2_phy`; auxiliary apertures still require a driver-specific audit. |
| dart-usb2 | existing description | `&usb_drd2_dart_0`; auxiliary apertures still require a driver-specific audit. |
| usb-drd2 | existing description | `&usb_drd2`; auxiliary apertures still require a driver-specific audit. |
| atc-phy3 | existing description | `&atc3_phy`; auxiliary apertures still require a driver-specific audit. |
| usb-auss0 | unresolved binding | An IP-specific binding is still required. |
| nub-spmi0 | existing description | `&nub_spmi0`; auxiliary apertures still require a driver-specific audit. |
| nub-spmi-a0 | existing description | `&nub_spmi_a0`; auxiliary apertures still require a driver-specific audit. |
| dcp0-sac-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dispext0 | existing description | `&dcpext0`; auxiliary apertures still require a driver-specific audit. |
| dcpext0 | existing description | `&dcpext0`; auxiliary apertures still require a driver-specific audit. |
| dart-dcpext0 | existing description | `&dart_dcpext0`; auxiliary apertures still require a driver-specific audit. |
| dart-dispext0 | existing description | `&dart_dispext0`; auxiliary apertures still require a driver-specific audit. |
| dp-audio1 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio3 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio4 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| :sotra_h17cr1_42 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| :sotra_h17cr2_42 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| :sotra_h17c_48 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| :sotra_h17d_97 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| mapper-gfx-asc | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| sapt | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| iop-audio-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| admac-base-ns | existing description | `&admac_base_ns`; auxiliary apertures still require a driver-specific audit. |
| audio-shared-dart-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| audio-hp-mic-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| audio-sllb-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| smctempsensor0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| cpu-debug-interface | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| nub-swd0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| pwm1 | existing description | `&fpwm0`; auxiliary apertures still require a driver-specific audit. |
| timesync | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| fillmore | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| isp-exclave-s-proxy | unresolved address translation | No covering parent range; do not guess a physical address. |
| msg | unresolved binding | An IP-specific binding is still required. |
| msghub | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| :sotra_h17d_5 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| :sotra_h17d_6 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| device-supports-gf2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| i2c5 | disabled resource description | `&hw_i2c5`; power/clock gate needs a verified provider |
| i2c7 | disabled resource description | `&hw_i2c7`; power/clock gate needs a verified provider |
| i2c0 | disabled resource description | `&hw_i2c0`; power/clock gate needs a verified provider |
| i2c3 | disabled resource description | `&hw_i2c3`; power/clock gate needs a verified provider |
| i2c4 | disabled resource description | `&hw_i2c4`; power/clock gate needs a verified provider |
| i2c6 | disabled resource description | `&hw_i2c6`; power/clock gate needs a verified provider |
| i2c8 | disabled resource description | `&hw_i2c8`; power/clock gate needs a verified provider |
| spi2 | disabled resource description | `&hw_spi2`; power/clock gate needs a verified provider |
| spi4 | disabled resource description | `&hw_spi4`; power/clock gate needs a verified provider |
| nub-spmi-a1 | disabled resource description | `&hw_nub_spmi_a1`; mixed/non-AIC interrupt routing needs decoding |
| dart-isp0 | disabled resource description | `&hw_dart_isp0`; power/clock gate needs a verified provider |
| apcie0-piodma | disabled resource description | `&hw_apcie0_piodma` |
| dart-apcie1 | disabled resource description | `&hw_dart_apcie1` |
| apcie1-piodma | disabled resource description | `&hw_apcie1_piodma` |
| dockchannel-mtp | disabled resource description | `&hw_dockchannel_mtp` |
| secure-repair | disabled resource description | `&hw_secure_repair` |
| apciec0 | disabled resource description | `&hw_apciec0`; power/clock gate needs a verified provider |
| dart-apciec0 | disabled resource description | `&hw_dart_apciec0` |
| apciec0-piodma | disabled resource description | `&hw_apciec0_piodma` |
| acio0 | disabled resource description | `&hw_acio0`; power/clock gate needs a verified provider |
| dart-acio0 | disabled resource description | `&hw_dart_acio0` |
| acio-cpu0 | disabled resource description | `&hw_acio_cpu0`; power/clock gate needs a verified provider |
| apciec1 | disabled resource description | `&hw_apciec1`; power/clock gate needs a verified provider |
| dart-apciec1 | disabled resource description | `&hw_dart_apciec1` |
| apciec1-piodma | disabled resource description | `&hw_apciec1_piodma` |
| acio1 | disabled resource description | `&hw_acio1`; power/clock gate needs a verified provider |
| dart-acio1 | disabled resource description | `&hw_dart_acio1` |
| acio-cpu1 | disabled resource description | `&hw_acio_cpu1`; power/clock gate needs a verified provider |
| apciec2 | disabled resource description | `&hw_apciec2`; power/clock gate needs a verified provider |
| dart-apciec2 | disabled resource description | `&hw_dart_apciec2` |
| apciec2-piodma | disabled resource description | `&hw_apciec2_piodma` |
| acio2 | disabled resource description | `&hw_acio2`; power/clock gate needs a verified provider |
| dart-acio2 | disabled resource description | `&hw_dart_acio2` |
| acio-cpu2 | disabled resource description | `&hw_acio_cpu2`; power/clock gate needs a verified provider |
| atc0-dpxbar | disabled resource description | `&hw_atc0_dpxbar` |
| atc0-dpin0 | disabled resource description | `&hw_atc0_dpin0` |
| atc0-dpin1 | disabled resource description | `&hw_atc0_dpin1` |
| atc0-dpin2 | disabled resource description | `&hw_atc0_dpin2` |
| atc0-dpin3 | disabled resource description | `&hw_atc0_dpin3` |
| atc1-dpxbar | disabled resource description | `&hw_atc1_dpxbar` |
| atc1-dpin0 | disabled resource description | `&hw_atc1_dpin0` |
| atc1-dpin1 | disabled resource description | `&hw_atc1_dpin1` |
| atc1-dpin2 | disabled resource description | `&hw_atc1_dpin2` |
| atc1-dpin3 | disabled resource description | `&hw_atc1_dpin3` |
| atc2-dpin0 | disabled resource description | `&hw_atc2_dpin0` |
| atc2-dpin1 | disabled resource description | `&hw_atc2_dpin1` |
| atc2-dpin2 | disabled resource description | `&hw_atc2_dpin2` |
| atc2-dpin3 | disabled resource description | `&hw_atc2_dpin3` |
| atc3-dpxbar | disabled resource description | `&hw_atc3_dpxbar` |
| aic-timebase | disabled resource description | `&hw_aic_timebase` |
| aes | disabled resource description | `&hw_aes`; power/clock gate needs a verified provider |
| nub-gpio0 | disabled resource description | `&hw_nub_gpio0` |
| aop-exclave-mailbox | disabled resource description | `&hw_aop_exclave_mailbox` |
| aop2 | disabled resource description | `&hw_aop2` |
| event-log-handler | disabled resource description | `&hw_event_log_handler` |
| sio | disabled resource description | `&hw_sio`; power/clock gate needs a verified provider |
| dart-sio | disabled resource description | `&hw_dart_sio`; power/clock gate needs a verified provider |
| pmp0 | disabled resource description | `&hw_pmp0`; power/clock gate needs a verified provider |
| dart-pmp0 | disabled resource description | `&hw_dart_pmp0` |
| sep | disabled resource description | `&hw_sep`; power/clock gate needs a verified provider |
| dart-sep | disabled resource description | `&hw_dart_sep` |
| aon-ptd | disabled resource description | `&hw_aon_ptd` |
| rsm | disabled resource description | `&hw_rsm`; power/clock gate needs a verified provider |
| dart-rsm | disabled resource description | `&hw_dart_rsm` |
| atc-phy0 | disabled resource description | `&hw_atc_phy0`; power/clock gate needs a verified provider |
| dart-usb0 | disabled resource description | `&hw_dart_usb0`; power/clock gate needs a verified provider |
| usb-drd0 | disabled resource description | `&hw_usb_drd0`; power/clock gate needs a verified provider |
| dart-auss0 | disabled resource description | `&hw_dart_auss0` |
| nub-spmi2 | disabled resource description | `&hw_nub_spmi2`; mixed/non-AIC interrupt routing needs decoding |
| nub-spmi3 | disabled resource description | `&hw_nub_spmi3`; mixed/non-AIC interrupt routing needs decoding |
| aop-spmi0 | disabled resource description | `&hw_aop_spmi0`; mixed/non-AIC interrupt routing needs decoding |
| dptx0-aux | disabled resource description | `&hw_dptx0_aux`; power/clock gate needs a verified provider |
| dptx1-aux | disabled resource description | `&hw_dptx1_aux`; power/clock gate needs a verified provider |
| disp0 | disabled resource description | `&hw_disp0`; power/clock gate needs a verified provider |
| dart-disp0 | disabled resource description | `&hw_dart_disp0` |
| dcp0-expert | disabled resource description | `&hw_dcp0_expert` |
| dcp | disabled resource description | `&hw_dcp`; power/clock gate needs a verified provider |
| dart-dcp | disabled resource description | `&hw_dart_dcp` |
| dart-disp0-isoc | disabled resource description | `&hw_dart_disp0_isoc` |
| admac-disp0 | disabled resource description | `&hw_admac_disp0` |
| admac-disp0-ced-ssw | disabled resource description | `&hw_admac_disp0_ced_ssw` |
| dcpext0-expert | disabled resource description | `&hw_dcpext0_expert` |
| dart-dispext0-isoc | disabled resource description | `&hw_dart_dispext0_isoc` |
| admac-dispext0 | disabled resource description | `&hw_admac_dispext0` |
| admac-dispext0-ced-ssw | disabled resource description | `&hw_admac_dispext0_ced_ssw` |
| dispext1 | disabled resource description | `&hw_dispext1` |
| dcpext1-expert | disabled resource description | `&hw_dcpext1_expert` |
| dcpext1 | disabled resource description | `&hw_dcpext1`; power/clock gate needs a verified provider |
| dart-dcpext1 | disabled resource description | `&hw_dart_dcpext1` |
| dart-dispext1 | disabled resource description | `&hw_dart_dispext1` |
| dart-dispext1-isoc | disabled resource description | `&hw_dart_dispext1_isoc` |
| admac-dispext1 | disabled resource description | `&hw_admac_dispext1` |
| admac-dispext1-ced-ssw | disabled resource description | `&hw_admac_dispext1_ced_ssw` |
| dispext2 | disabled resource description | `&hw_dispext2` |
| dcpext2-expert | disabled resource description | `&hw_dcpext2_expert` |
| dcpext2 | disabled resource description | `&hw_dcpext2`; power/clock gate needs a verified provider |
| dart-dcpext2 | disabled resource description | `&hw_dart_dcpext2` |
| dart-dispext2 | disabled resource description | `&hw_dart_dispext2` |
| dart-dispext2-isoc | disabled resource description | `&hw_dart_dispext2_isoc` |
| admac-dispext2 | disabled resource description | `&hw_admac_dispext2` |
| admac-dispext2-ced-ssw | disabled resource description | `&hw_admac_dispext2_ced_ssw` |
| dispext3 | disabled resource description | `&hw_dispext3` |
| dcpext3-expert | disabled resource description | `&hw_dcpext3_expert` |
| dcpext3 | disabled resource description | `&hw_dcpext3`; power/clock gate needs a verified provider |
| dart-dcpext3 | disabled resource description | `&hw_dart_dcpext3` |
| dart-dispext3 | disabled resource description | `&hw_dart_dispext3` |
| dart-dispext3-isoc | disabled resource description | `&hw_dart_dispext3_isoc` |
| admac-dispext3 | disabled resource description | `&hw_admac_dispext3` |
| admac-dispext3-ced-ssw | disabled resource description | `&hw_admac_dispext3_ced_ssw` |
| dart-scaler | disabled resource description | `&hw_dart_scaler`; power/clock gate needs a verified provider |
| scaler0 | disabled resource description | `&hw_scaler0`; power/clock gate needs a verified provider |
| dart-scaler0 | disabled resource description | `&hw_dart_scaler0`; power/clock gate needs a verified provider |
| scaler1 | disabled resource description | `&hw_scaler1`; power/clock gate needs a verified provider |
| dart-scaler1 | disabled resource description | `&hw_dart_scaler1`; power/clock gate needs a verified provider |
| ave0 | disabled resource description | `&hw_ave0`; power/clock gate needs a verified provider |
| dart-ave0 | disabled resource description | `&hw_dart_ave0`; power/clock gate needs a verified provider |
| ave1 | disabled resource description | `&hw_ave1`; power/clock gate needs a verified provider |
| dart-ave1 | disabled resource description | `&hw_dart_ave1`; power/clock gate needs a verified provider |
| apr0 | disabled resource description | `&hw_apr0`; power/clock gate needs a verified provider |
| dart-apr0 | disabled resource description | `&hw_dart_apr0`; power/clock gate needs a verified provider |
| dart-apr1 | disabled resource description | `&hw_dart_apr1`; power/clock gate needs a verified provider |
| jpeg0 | disabled resource description | `&hw_jpeg0`; power/clock gate needs a verified provider |
| jpeg1 | disabled resource description | `&hw_jpeg1`; power/clock gate needs a verified provider |
| dart-jpeg0 | disabled resource description | `&hw_dart_jpeg0`; power/clock gate needs a verified provider |
| ane0 | disabled resource description | `&hw_ane0`; power/clock gate needs a verified provider |
| dart-ane0 | disabled resource description | `&hw_dart_ane0`; power/clock gate needs a verified provider |
| avd0 | disabled resource description | `&hw_avd0`; power/clock gate needs a verified provider |
| dart-avd0 | disabled resource description | `&hw_dart_avd0`; power/clock gate needs a verified provider |
| sgx | disabled resource description | `&hw_sgx`; power/clock gate needs a verified provider |
| gfx-asc | disabled resource description | `&hw_gfx_asc`; DMA mapper requires an IP-specific split, power/clock gate needs a verified provider |
| gfx1-asc | disabled resource description | `&hw_gfx1_asc`; DMA mapper requires an IP-specific split, power/clock gate needs a verified provider |
| admac-leap-ns | disabled resource description | `&hw_admac_leap_ns` |
| admac-aop-ced-ssw | disabled resource description | `&hw_admac_aop_ced_ssw` |
| admac-leap-s | disabled resource description | `&hw_admac_leap_s` |
| admac-hp-mic-ec-proxy | disabled resource description | `&hw_admac_hp_mic_ec_proxy` |
| admac-sllb-ec-proxy | disabled resource description | `&hw_admac_sllb_ec_proxy` |
| dbgCPU | disabled resource description | `&hw_dbgCPU` |
| apple-processor-trace | disabled resource description | `&hw_apple_processor_trace` |
