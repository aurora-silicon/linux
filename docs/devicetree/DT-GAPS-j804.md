# J804 device-tree audit

SoC: T8152 (H18), product Mac18,1. Source: redacted community ADT, checked against the same-die live J873 ADT. Audited 518 ADT node records.
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

## Speaker day-one lane

The ADT has no `acoustic-id` property. Speaker identity is represented by
six `sn012776` amplifier endpoints, their channel/position names, `amp_gain`
properties and `speaker-config` containers. The AOP audio service describes
the speaker stream and its audio-data child, speaker tap and headphone/mic
path; the jack codec is CS42L84. These are hardware/topology facts, not an
acoustic tuning bundle. Do not infer an AID or copy gain/configuration data
into DTS. Product/board-to-AID mapping and per-speaker calibration remain
boot-environment work. The speaker path remains disabled until its transport,
power sequencing and protection integration are qualified.

## SoC-bus coverage

| ADT device | Description | Location or unresolved detail |
| --- | --- | --- |
| displaymanager | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dynamic-pil | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| centauri | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| bluetooth | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc0-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc1-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc2-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| atc3-dpphy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| display-crossbar0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| aic | existing description | `&aic`; auxiliary apertures still require a driver-specific audit. |
| aop-exclave-ioreporting | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| pmgr | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| smc | existing description | `&smc`; auxiliary apertures still require a driver-specific audit. |
| uart0 | unresolved binding | An IP-specific binding is still required. |
| dockchannel-uart | existing description | `&dockchannel`; auxiliary apertures still require a driver-specific audit. |
| mtp-aop-mux | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp2hdmi-gpio0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dcp-sac-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio1 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| dp-audio2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| isp-exclave-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| ane-exclave-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| mapper-gfx-asc | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| iop-audio-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| iop-voicetrigger-controller | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| audio-shared-dart-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| audio-hp-mic-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| audio-sllb-proxy | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| smctempsensor0 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| cpu-debug-interface | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| aft | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| timesync | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| fillmore | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| sapt | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| eic | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| device-supports-gf2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| spi2 | disabled resource description | `&hw_spi2` |
| spi4 | disabled resource description | `&hw_spi4` |
| i2c0 | disabled resource description | `&hw_i2c0` |
| i2c1 | disabled resource description | `&hw_i2c1` |
| i2c3 | disabled resource description | `&hw_i2c3` |
| i2c4 | disabled resource description | `&hw_i2c4` |
| fpwm0 | disabled resource description | `&hw_fpwm0` |
| dart-apcie2 | disabled resource description | `&hw_dart_apcie2` |
| apcie2-piodma | disabled resource description | `&hw_apcie2_piodma` |
| nub-spmi-a0 | disabled resource description | `&hw_nub_spmi_a0`; mixed/non-AIC interrupt routing needs decoding |
| nub-spmi-a1 | disabled resource description | `&hw_nub_spmi_a1`; mixed/non-AIC interrupt routing needs decoding |
| aop-spmi0 | disabled resource description | `&hw_aop_spmi0`; mixed/non-AIC interrupt routing needs decoding |
| apcie | disabled resource description | `&hw_apcie` |
| dart-apcie0 | disabled resource description | `&hw_dart_apcie0` |
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
| atc2-dpxbar | disabled resource description | `&hw_atc2_dpxbar` |
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
| soc-reporter | disabled resource description | `&hw_soc_reporter` |
| aes | disabled resource description | `&hw_aes`; power/clock gate needs a verified provider |
| gpio0 | disabled resource description | `&hw_gpio0` |
| aop-gpio | disabled resource description | `&hw_aop_gpio` |
| nub-gpio | disabled resource description | `&hw_nub_gpio` |
| aop | disabled resource description | `&hw_aop` |
| aop-exclave-mailbox | disabled resource description | `&hw_aop_exclave_mailbox` |
| aop2 | disabled resource description | `&hw_aop2` |
| pmgr-child | disabled resource description | `&hw_pmgr_child` |
| event-log-handler | disabled resource description | `&hw_event_log_handler` |
| pmp | disabled resource description | `&hw_pmp` |
| sep | disabled resource description | `&hw_sep` |
| dart-sep | disabled resource description | `&hw_dart_sep` |
| esm | disabled resource description | `&hw_esm` |
| sio | disabled resource description | `&hw_sio`; power/clock gate needs a verified provider |
| dart-sio | disabled resource description | `&hw_dart_sio` |
| dart-sbr | disabled resource description | `&hw_dart_sbr` |
| ans | disabled resource description | `&hw_ans`; DMA mapper requires an IP-specific split |
| sart-ans | disabled resource description | `&hw_sart_ans` |
| aon-ptd | disabled resource description | `&hw_aon_ptd` |
| mtp | disabled resource description | `&hw_mtp` |
| qspi | disabled resource description | `&hw_qspi` |
| i2c2 | disabled resource description | `&hw_i2c2` |
| atc-phy2 | disabled resource description | `&hw_atc_phy2`; power/clock gate needs a verified provider |
| atc-phy0 | disabled resource description | `&hw_atc_phy0`; power/clock gate needs a verified provider |
| dart-usb0 | disabled resource description | `&hw_dart_usb0` |
| usb-drd0 | disabled resource description | `&hw_usb_drd0` |
| atc-phy1 | disabled resource description | `&hw_atc_phy1`; power/clock gate needs a verified provider |
| dart-usb1 | disabled resource description | `&hw_dart_usb1` |
| usb-drd1 | disabled resource description | `&hw_usb_drd1` |
| atc-phy3 | disabled resource description | `&hw_atc_phy3`; power/clock gate needs a verified provider |
| dart-usb3 | disabled resource description | `&hw_dart_usb3` |
| usb-drd3 | disabled resource description | `&hw_usb_drd3` |
| nub-spmi0 | disabled resource description | `&hw_nub_spmi0`; mixed/non-AIC interrupt routing needs decoding |
| nub-spmi1 | disabled resource description | `&hw_nub_spmi1`; mixed/non-AIC interrupt routing needs decoding |
| disp0 | disabled resource description | `&hw_disp0` |
| dcp0-expert | disabled resource description | `&hw_dcp0_expert` |
| dcp | disabled resource description | `&hw_dcp`; power/clock gate needs a verified provider |
| dart-dcp | disabled resource description | `&hw_dart_dcp` |
| dart-disp0 | disabled resource description | `&hw_dart_disp0` |
| dart-disp0-isoc | disabled resource description | `&hw_dart_disp0_isoc` |
| admac-disp0 | disabled resource description | `&hw_admac_disp0` |
| admac-disp0-ced-ssw | disabled resource description | `&hw_admac_disp0_ced_ssw` |
| dispext0 | disabled resource description | `&hw_dispext0` |
| dcpext0-expert | disabled resource description | `&hw_dcpext0_expert` |
| dcpext0 | disabled resource description | `&hw_dcpext0`; power/clock gate needs a verified provider |
| dart-dcpext0 | disabled resource description | `&hw_dart_dcpext0` |
| dart-dispext0 | disabled resource description | `&hw_dart_dispext0` |
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
| dptx0-aux | disabled resource description | `&hw_dptx0_aux`; power/clock gate needs a verified provider |
| dptx1-aux | disabled resource description | `&hw_dptx1_aux`; power/clock gate needs a verified provider |
| scaler0 | disabled resource description | `&hw_scaler0`; power/clock gate needs a verified provider |
| dart-scaler | disabled resource description | `&hw_dart_scaler` |
| jpeg0 | disabled resource description | `&hw_jpeg0`; power/clock gate needs a verified provider |
| jpeg1 | disabled resource description | `&hw_jpeg1`; power/clock gate needs a verified provider |
| dart-jpeg | disabled resource description | `&hw_dart_jpeg` |
| ave | disabled resource description | `&hw_ave`; power/clock gate needs a verified provider |
| dart-ave | disabled resource description | `&hw_dart_ave` |
| avd | disabled resource description | `&hw_avd`; DMA mapper is not resolved, power/clock gate needs a verified provider |
| dart-avd | disabled resource description | `&hw_dart_avd` |
| apr | disabled resource description | `&hw_apr`; power/clock gate needs a verified provider |
| dart-apr | disabled resource description | `&hw_dart_apr` |
| isp | disabled resource description | `&hw_isp`; power/clock gate needs a verified provider |
| dart-isp | disabled resource description | `&hw_dart_isp` |
| isp-exclave-s-proxy | disabled resource description | `&hw_isp_exclave_s_proxy` |
| ane | disabled resource description | `&hw_ane`; power/clock gate needs a verified provider |
| dart-ane | disabled resource description | `&hw_dart_ane` |
| ane1 | disabled resource description | `&hw_ane1`; power/clock gate needs a verified provider |
| dart-ane1 | disabled resource description | `&hw_dart_ane1` |
| sgx | disabled resource description | `&hw_sgx`; power/clock gate needs a verified provider |
| gfx-asc | disabled resource description | `&hw_gfx_asc`; DMA mapper requires an IP-specific split, power/clock gate needs a verified provider |
| gfx1-asc | disabled resource description | `&hw_gfx1_asc`; DMA mapper requires an IP-specific split, power/clock gate needs a verified provider |
| admac-base-ns | disabled resource description | `&hw_admac_base_ns`; power/clock gate needs a verified provider |
| admac-leap-ns | disabled resource description | `&hw_admac_leap_ns`; power/clock gate needs a verified provider |
| admac-aop-ced-ssw | disabled resource description | `&hw_admac_aop_ced_ssw` |
| admac-leap-s | disabled resource description | `&hw_admac_leap_s`; power/clock gate needs a verified provider |
| admac-hp-mic-ec-proxy | disabled resource description | `&hw_admac_hp_mic_ec_proxy` |
| admac-sllb-ec-proxy | disabled resource description | `&hw_admac_sllb_ec_proxy` |
| dbgCPU | disabled resource description | `&hw_dbgCPU` |
| dart-tlbi-aperture | disabled resource description | `&hw_dart_tlbi_aperture` |
| secure-repair | disabled resource description | `&hw_secure_repair` |
| apple-processor-trace | disabled resource description | `&hw_apple_processor_trace` |
