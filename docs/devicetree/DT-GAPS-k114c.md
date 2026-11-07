# K114C device-tree audit

SoC: T6050. Source: board-specific static ADT. Audited 1080 ADT node records.
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
| pwm2 | existing description | `&fpwm2`; auxiliary apertures still require a driver-specific audit. |
| exdisplaypipe | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| exdisplaypipe-s-proxy | unresolved address translation | No covering parent range; do not guess a physical address. |
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
| dcp-exclave-mailbox | unresolved address translation | No covering parent range; do not guess a physical address. |
| dcp-exclave-ioreporting | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
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
| pwm1 | existing description | `&fpwm1`; auxiliary apertures still require a driver-specific audit. |
| timesync | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| fillmore | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| isp-exclave-s-proxy | unresolved address translation | No covering parent range; do not guess a physical address. |
| msg | unresolved binding | An IP-specific binding is still required. |
| msghub | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| :sotra_h17d_5 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| :sotra_h17d_6 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| device-supports-gf2 | interface or firmware service | No independent MMIO aperture; requires transport/service binding. |
| nub-spmi1 | disabled resource description | `&hw_nub_spmi1`; mixed/non-AIC interrupt routing needs decoding |
| nub-spmi4 | disabled resource description | `&hw_nub_spmi4`; mixed/non-AIC interrupt routing needs decoding |
| i2c0 | disabled resource description | `&hw_i2c0`; power/clock gate needs a verified provider |
| i2c3 | disabled resource description | `&hw_i2c3`; power/clock gate needs a verified provider |
| i2c4 | disabled resource description | `&hw_i2c4`; power/clock gate needs a verified provider |
| i2c6 | disabled resource description | `&hw_i2c6`; power/clock gate needs a verified provider |
| i2c8 | disabled resource description | `&hw_i2c8`; power/clock gate needs a verified provider |
| spi2 | disabled resource description | `&hw_spi2`; power/clock gate needs a verified provider; clock provider not yet translated |
| spi4 | disabled resource description | `&hw_spi4`; power/clock gate needs a verified provider; clock provider not yet translated |
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
| sio | disabled resource description | `&hw_sio`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-sio | disabled resource description | `&hw_dart_sio`; power/clock gate needs a verified provider |
| pmp0 | disabled resource description | `&hw_pmp0`; power/clock gate needs a verified provider |
| dart-pmp0 | disabled resource description | `&hw_dart_pmp0` |
| sep | disabled resource description | `&hw_sep`; power/clock gate needs a verified provider; clock provider not yet translated |
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
| disp0 | disabled resource description | `&hw_disp0`; power/clock gate needs a verified provider; clock provider not yet translated |
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
| scaler0 | disabled resource description | `&hw_scaler0`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-scaler0 | disabled resource description | `&hw_dart_scaler0`; power/clock gate needs a verified provider |
| scaler1 | disabled resource description | `&hw_scaler1`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-scaler1 | disabled resource description | `&hw_dart_scaler1`; power/clock gate needs a verified provider |
| ave0 | disabled resource description | `&hw_ave0`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-ave0 | disabled resource description | `&hw_dart_ave0`; power/clock gate needs a verified provider |
| ave1 | disabled resource description | `&hw_ave1`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-ave1 | disabled resource description | `&hw_dart_ave1`; power/clock gate needs a verified provider |
| apr0 | disabled resource description | `&hw_apr0`; power/clock gate needs a verified provider |
| dart-apr0 | disabled resource description | `&hw_dart_apr0`; power/clock gate needs a verified provider |
| dart-apr1 | disabled resource description | `&hw_dart_apr1`; power/clock gate needs a verified provider |
| jpeg0 | disabled resource description | `&hw_jpeg0`; power/clock gate needs a verified provider; clock provider not yet translated |
| jpeg1 | disabled resource description | `&hw_jpeg1`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-jpeg0 | disabled resource description | `&hw_dart_jpeg0`; power/clock gate needs a verified provider |
| ane0 | disabled resource description | `&hw_ane0`; power/clock gate needs a verified provider; clock provider not yet translated |
| dart-ane0 | disabled resource description | `&hw_dart_ane0`; power/clock gate needs a verified provider |
| avd0 | disabled resource description | `&hw_avd0`; power/clock gate needs a verified provider; clock provider not yet translated |
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

## Child devices and service interfaces

These entries require an endpoint/transport audit in addition to the parent
controller resources above. Mapper records describe stream IDs rather than
independently probed devices. Conditional template records and their tuning
containers are not separate hardware devices; select the silicon variant
using a resolved live tree before changing resources. No tuning properties
from those records are reproduced here.

| Hardware path below the SoC bus | Classification and remaining audit |
| --- | --- |
| `nub-spmi1/spmi-mosquitoM1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi4/spmi-jerezC0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi4/spmi-jerezC1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `pwm2/mesa-backlight` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `i2c0/sync-hub-config` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c0/sync-hub-prog` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c1/audio-speaker` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c1/audio-speaker-left-woofer-2` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c1/audio-speaker-left-tweeter` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c2/audio-codec-output` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c3/audio-speaker-right-woofer-1` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c3/audio-speaker-right-woofer-2` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c3/audio-speaker-right-tweeter` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `iop-voicetrigger-controller/audio-vt-device` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `i2c4/sd-card` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c6/uatcrt0` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c6/uatcrt1` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c6/uatcrt2` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `i2c8/pcon0` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `spi2/mesa` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `spi4/ixora` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `spi4/ixora/lcd-pmicwp` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/ixora/lcd-pmic` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/ixora/lcd-pmic1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/ixora/lcd-pmic2` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/ixora/lcd-pls` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/ixora/lcd-sswp` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/ixora/lcd-ss` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/ixora/tcon-registers` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/ixora/lcd-eeprom` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `spi4/display-temp-sens1` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `spi4/display-temp-sens2` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `spi4/display-temp-sens3` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `spi4/displayNAND` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `nub-spmi-a1/hpm2` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-isp0/mapper-isp0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp0/mapper-isp-mpm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp0/mapper-isp-epipe0-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp0/mapper-isp-epipe1-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp0/mapper-isp-piodma0-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-isp0/mapper-isp-piodma1-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `pwm0/kbd-backlight` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie0/pci-bridge0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie0/pci-bridge0/centauri-control` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie0/pci-bridge0/centauri-alpha` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie0/pci-bridge0/centauri-beta` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie0/pci-bridge1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apcie0/pci-bridge1/pcie-sdreader` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-apcie0/mapper-apcie0-control` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apcie0/mapper-apcie0-alpha` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apcie0/mapper-apcie0-beta` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apcie0/mapper-apcie0-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apcie1/mapper-apcie1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apcie1/mapper-apcie1-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `acio-phy-cpu0/iop-aciophy0-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `acio-phy-cpu1/iop-aciophy1-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `acio-phy-cpu2/iop-aciophy2-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `acio-phy-cpu3/iop-aciophy3-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/multi-touch` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/kiwi` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/keyboard` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/stm` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/actuator` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dockchannel-mtp/mtp-transport/tp-accel` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `mtp/iop-mtp-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-mtp/mapper-mtp` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
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
| `acio-cpu0/iop-acio0-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
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
| `acio-cpu1/iop-acio1-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `apciec2/pcic2-bridge` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-apciec2/mapper-apciec2-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-8` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-9` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-10` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-tx-11` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-8` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-9` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-10` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-acio2/mapper-acio2-rx-11` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `acio-cpu2/iop-acio2-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `error-handler/amcc-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/dcs-error-handler` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `error-handler/pmgr-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afxni-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins0-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins1-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins2-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins3-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins4-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins5-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins6-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins7-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins8-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins9-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins10-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins11-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins12-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins13-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins14-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afins15-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns0-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns1-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns2-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns3-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns4-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns5-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns6-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns7-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns8-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns9-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/afcns10-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/sep-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/cllt-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/piogw-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/sram-parity-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `error-handler/axi2af-error-handler` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `auss-cpu0/auss-cpu0-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/aop-audio` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/aop-voicetrigger` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/dcp-control` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/als` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/spherecontrol` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/accel` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop/iop-aop-nub/gyro` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-aop/mapper-aop` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-exclave-aop` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-aop2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-admac-leap-ns` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-admac-base-ns` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-admac-leap-s` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-admac-base-s` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-aop/mapper-scm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `aop2/iop-aop2-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `pmgr/soc-tuner` | Power/firmware interface; provider topology and handoff pending. |
| `pmgr/clpc` | Power/firmware interface; provider topology and handoff pending. |
| `pmgr/ppm` | Power/firmware interface; provider topology and handoff pending. |
| `pmgr/pmgrtools` | Power/firmware interface; provider topology and handoff pending. |
| `sio/iop-sio-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `sio/iop-sio-nub/sio-dma` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-sio/mapper-sio` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sio/mapper-aes` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `pmp0/iop-pmp0-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-pmp0/mapper-pmp0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-pmp0/mapper-pmp0-ptd-fwd` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `pmp1/iop-pmp1-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-pmp1/mapper-pmp1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-pmp1/mapper-pmp1-ptd-fwd` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `sep/iop-sep-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-sep/mapper-sep` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-sep/mapper-sep-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `ans/iop-ans-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `ans/ans-pcie` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-pmu` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-charger-util-0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-charger-util-1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-charger-util-2` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-charger-util` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `smc/iop-smc-nub/smc-area51` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `dart-rsm/mapper-rsm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `qspi/spinor` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `qspi/spinor/firmware` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `qspi/spinor/anvram` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `qspi/spinor/syscfg` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-usb0/mapper-usb0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-usb1/mapper-usb1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-usb2/mapper-usb2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-auss0/mapper-auss0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `nub-spmi0/spmi-abbeyL1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi0/spmi-mosquitoM0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi0/btm` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi2/spmi-abbeyF1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi3/spmi-abbeyF2` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi-a0/hpm0` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi-a0/hpm1` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `nub-spmi-a0/hpm5` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop-spmi0/stockholm-spmi` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `aop-spmi0/stockholm-spmi/stockholm` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dptx0-aux/dptx0-aux-core` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dptx0-aux/dptx0-aux-ufp` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dptx1-aux/dptx1-aux-core` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dptx1-aux/dptx1-aux-ufp` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-disp0/mapper-disp0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0/mapper-disp0-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0/mapper-disp0-scl` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-disp0/mapper-disp0-sca` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dcp/iop-dcp-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-dcp/mapper-dcp` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dcp/mapper-dcp-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
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
| `dcpext2/iop-dcpext2-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-dcpext2/mapper-dcpext2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dcpext2/mapper-dcpext2-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext2/mapper-dispext2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext2/mapper-dispext2-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext2-isoc/mapper-dispext2-dpa` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dcpext3/iop-dcpext3-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `dart-dcpext3/mapper-dcpext3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dcpext3/mapper-dcpext3-exclave` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext3/mapper-dispext3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext3/mapper-dispext3-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-dispext3-isoc/mapper-dispext3-dpa` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-scaler/mapper-scaler` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-scaler/mapper-scaler-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-scaler0/mapper-scaler0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-scaler0/mapper-scaler-piodma0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-scaler1/mapper-scaler1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-scaler1/mapper-scaler-piodma1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ave0/mapper-ave0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ave1/mapper-ave1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apr0/mapper-apr0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-apr1/mapper-apr1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-jpeg0/mapper-jpeg0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-jpeg0/mapper-jpeg1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane0/mapper-ane0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane0/mapper-ane0-iso1` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane0/mapper-ane0-iso2` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane0/mapper-ane0-iso3` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane0/mapper-ane0-iso4` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane0/mapper-ane0-iso5` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane0/mapper-ane0-iso6` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane0/mapper-ane0-iso7` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-ane0/mapper-ane0-mpm` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-avd0/mapper-avd0` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-avd0/mapper-avd0-piodma` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `dart-avd0/mapper-avd0-adsbuf` | DMA stream metadata; check the master IOMMU reference and stream ownership. |
| `gfx-asc/iop-gfx-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `gfx1-asc/iop-gfx1-nub` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-hp-mic` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-hp-mic/audio-hp-mic` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-input` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-input/audio-codec-input` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-output` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-codec-output/audio-codec-output` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-leap-internal-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-leap-internal-loopback/audio-leap-internal-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-secure-leap-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-secure-leap-loopback/audio-secure-leap-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-mca-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-mca-loopback/audio-mca-loopback` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-speaker` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-speaker/audio-speaker` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-speaker-tap` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
| `iop-audio-controller/audio-speaker-tap/audio-speaker-tap` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
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
| `admac-leap-ns/leap-ns-adma-tx-4` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-leap-ns/leap-ns-adma-rx-4` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-leap-ns/leap-ns-adma-tx-5` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-leap-s/leap-s-adma-rx-0` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `admac-leap-s/leap-s-adma-rx-5` | Subdevice register interface; binding, parent translation and ownership audit pending. |
| `pwm1/bklb-backlight` | Attached device or firmware service; endpoint binding and board wiring audit pending. |
