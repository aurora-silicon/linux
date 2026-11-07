# J316s device-tree audit

Status: partial offline audit; the task's exhaustive-property and installed-DTB
acceptance criteria are not yet established.

Audit date: 2026-11-07. Integration baseline:
`43c790fed6bb57a12d7ee3df474a15da790b56e6`.

J316s is the 16-inch **M1 Pro, T6000**. T6001 is M1 Max and its
16-inch board is **J316c**. They share DTS includes, but a J316s capture
does not qualify J316c. No J316c capture was available for this audit.

The evidence is this J316s's saved IODeviceTree capture from 2026-11-05.
Register addresses below are translated through its `arm-io/ranges`.
The capture, firmware data, calibration, unit identifiers and boot manifests
are excluded from this repository. See [the audit method](DT-AUDIT-METHOD.md)
for scope and the distinction between a missing device, a firmware service
and a property that must be supplied at boot.

## Changes prepared by this audit

| ADT evidence | DTS change | Driver / limits |
|---|---|---|
| `/arm-io/aop/iop-aop-nub/accel` and `gyro` | Add disabled `accel` and `gyro` children on J316s and document the T6000 motion compatibles | `drivers/iio/common/aop_sensors/aop_motion.rs`; needs bootloader calibration, otherwise probe leaves the sensor off. Samples and calibration handoff are unqualified. |
| `/arm-io/spi4`: physical `0x39b110000`, size `0x4000`, AIC IRQ 1110 | Describe SPI4, disabled | `drivers/spi/spi-apple.c`; no display-controller child or pin setup is enabled. |
| `/arm-io/nub-spmi1`: first register window at `0x2920a9300` | Describe the polled controller window, disabled | `drivers/spmi/spmi-apple-controller.c`; downstream PMIC/radio support and ownership remain unqualified. |
| `/arm-io/clvr-spmi0`, `clvr-spmi1`, `clvr-spmi2`: first windows at `0x28e501100`, `0x28e509100`, `0x28e511100` | Describe the three polled controller windows, disabled | Same driver; no regulator, voltage, IRQ or power policy is changed. |
| `nub-spmi1/spmi-spmu` (Maverick, SID 14) and `clvr-spmi{0,1,2}/spmi-monaco{0,1,2}` (Monaco, SID 15 each) | Add read-only NVMEM providers below the disabled buses; document Monaco identity | `drivers/nvmem/apple-spmi-nvmem.c`; no regulator maps or additional NVMEM cells are invented. |
| `/arm-io/aop-spmi0`, `aop-spmi1`: first windows at `0x293829300`, `0x29382d300` | Describe the two polled controller windows, disabled | Same driver; this does not transfer bus ownership or expose an NFC device. |

The bus nodes reside in `t600x-die0.dtsi`, so other T600x boards inherit
disabled descriptions. Only the J316s resource evidence was checked here.
The motion children are board-specific; they are not inferred on J316c.
The AOP core creates service devices itself and does not explicitly check
child-node availability before attaching the firmware node. Consequently,
`status = "disabled"` alone is not a verified runtime gate for these services;
the driver's missing-calibration refusal is an independent prerequisite.
Qualify availability handling before introducing a calibration handoff.

## Remaining device and property gaps

| ADT device/property | Integration description | Disposition |
|---|---|---|
| AOP `accel-*` and `gyro-*` calibration containers, gyro temperature table, HID usage and service identifiers | Motion nodes were absent; the binding defines three boot-time calibration properties | Nodes added. Calibration contents must come from this Mac at boot. The loader handoff and older-firmware sample format need qualification; do not copy calibration into DTS. |
| AOP audio PDM settings and decimator children | `aop/audio`, AOP ADMAC and the board microphone identity exist | Firmware/calibration handoff, not additional MMIO devices. No tuning arrays are ported. |
| `/arm-io/aop-spmi1/stockholm-spmi`, SID 12; its `stockholm` GPIO child | No NFC/SE child | Hardware is present. [Stockholm work](https://github.com/aurora-silicon/linux/pull/1042) targets other SoCs; integration has no matching T6000 NFC driver. IRQ layout, enable GPIO and ownership need a board-specific port. |
| `aop-spmi0/spmi-wifibt`, `spmi-uwb` | No radio SPMI child | Parent described disabled. These are not replacement Wi-Fi/Bluetooth host interfaces. No matching consumer binding/driver is established. |
| `nub-spmi1/spmi-spmu`, `spmi-viper`; `clvr-spmi{0,1,2}/spmi-monaco{0,1,2}` | Only the primary Maverick PMIC was described | Read-only auxiliary Maverick/Monaco NVMEM descriptions added below disabled buses. Viper still lacks a compatible/consumer. Regulator maps, PMIC functions and bus ownership remain unqualified; generic NVMEM is not a regulator driver. |
| `error-handler` register regions, interrupt list, source metadata; `event-log-handler` registers/IRQs | Neither device is described | `apple_soc_edac.c` matches T6041/T8140 only, with generation-specific source/status handling. T6000 needs driver data and a binding, not a compatible alias. |
| `mcc` configuration and DRAM topology | PMP/PMGR describe performance resources; no T6000 MCC device | No matching MCC driver in integration. Configuration/tuning payloads are not copied. |
| `jpeg0`, `jpeg1`, their DARTs and mapper SID 0 | No JPEG engine/DART nodes at the baseline | Integration's JPEG match table supports T6041/T8140. Existing branch `jpeg/t8112-t6000-layouts` has disabled T6000 descriptions; its T6000 driver refuses probe pending a complete reset profile. Do not duplicate that branch or enable streaming. |
| `apr0` and `dart-apr0` | No ProRes engine/DART | The integration ProRes driver matches T8140 only. The existence of APR on this Mac must not be inferred from `pmp_report_prores`, which T6000 deletes. A T6000 report ID and driver port are unqualified. |
| `scaler0`, `dart-scaler0`, mapper children | No scaler | No matching scaler/CSC driver in integration. |
| `ave0` and `dart-ave0` | AVE plus two DART windows exist, disabled | Existing T6000 AVE match data does not remove the firmware-loader handoff requirement. Keep disabled; do not treat this as missing MMIO. |
| `avd0` PIODMA/window resources | AVD and DART exist; PIODMA additions are separate work | [AVD PIODMA PR](https://github.com/aurora-silicon/linux/pull/710); do not duplicate. Decode functionality is not established by DT validation. |
| `spi4/dp855`; `lcd-pmicwp`, `lcd-pmic`, `lcd-sswp`, `lcd-ss`, `tcon-registers`, `lcd-eeprom` | No display bridge child | SPI4 description added disabled. No DP855 transport/binding is present; its indirect resources cannot be made ordinary I2C children. |
| `spi4/display-temp-sens{1,2,3}`, TMP103 at `0x70`–`0x72` | No panel temperature devices | A TMP103 driver exists, but the DP855-mediated bus does not. Adding direct I2C nodes would describe the wrong transport. |
| `i2c4/sd-card`, address `0x20` | I2C4 exists disabled; PCIe SD reader exists | Auxiliary SD control function lacks a compatible/driver. This is not evidence that the PCIe SD reader is missing. |
| `pwm1` (`pwm,t6000`), sharing the PWM register region | Only FPWM0 is modeled | Plain PWM and FPWM have different interfaces. No compatible-supported plain-PWM driver was found; do not add a second FPWM device over the same registers. |
| `dwi`, `dcp-sac-controller`, `dcp0-expert`, `dcpext{0,1}-expert`, `backlight/twodbl`, `sacm` | DCP/panel/PHY/display graph exists | Firmware display functions; no separate matching AP driver. ADT feature and tuning properties are not proof of Linux feature support. |
| `aes` | No AES device | No matching Apple AES driver. Storage encryption and key ownership require their own implementation. |
| PMU `info-rtc*`, alarm masks/events, fault and scratchpad functions | Primary PMIC NVMEM cells, SMC RTC/reboot and power services exist | Direct PMIC alarm/RTC functions are not modeled. The SMC RTC interface is a separate existing path; a direct PMIC port needs a driver and interrupt ownership proof. |
| `smctempsensor0`, `sacm` thermal descriptions | SMC hwmon, laptop and dual-fan descriptions exist; no board DT thermal policy | ADT is not a Linux trip/cooling policy. Additional die channels and a thermal policy need qualified sensor keys and cooling mappings. Dynamic SMC services must be inventoried at runtime to claim complete sensor coverage. |
| WLAN wake path | Wi-Fi/Bluetooth PCIe children exist; board has no `wakeup-source` | Existing `wifi/brcmf-wowlan-wake` branch contains the associated driver/DT work. Integration lacks its retention/wake implementation; a DT-only flag is insufficient. Physical wake remains unqualified. |
| `cpu-debug-interface`, `nub-swd0`, `timesync`, `apple-processor-trace` | No corresponding endpoint nodes | Debug/trace/timesync consumers are not matched by integration drivers. |

## Existing coverage that is not a new gap

The CPU topology, AIC/timebase, watchdog, GPIO banks, main PMGR, NCO,
PMP/report transport, SEP and sensor SPI2, ANS/SART, SMC, console UART,
SPI1 NOR/NVRAM, keyboard/trackpad SPI3, I2C0–5, FPWM0, SIO/ADMAC/MCA,
ISP and ANE have DTS descriptions. Their presence is not a hardware-test result.

The three external USB-C ports have ATC PHY, USB DRD/DART, ACIO CPU/mailbox,
NHI, PCIe tunnel and graph descriptions. The fourth ATC is the HDMI path;
the board intentionally deletes a fourth USB4 router. Display crossbars,
DP PHYs and DP-audio endpoints are represented through that graph.

The ADT's six SN012776 speaker controls map to I2C1 addresses `0x38`–`0x3a`
and I2C3 `0x3b`–`0x3d`; CS42L84 is on I2C2 at `0x4b`. DTS already has those
codec children and MCA routes. MCA loopback and AOP microphone firmware
services do not require duplicating the codec nodes. Per-unit amp safety
and acoustic tuning remain external provisioning, not static DTS constants.

## Frozen DTB set

The reference boot configuration is recorded as embedding the 7.1
`dtbs-04a63b117` set in `boot.bin`. A new kernel or a DTB installed under
`/boot` does not by itself replace that embedded set. A 7.2/7.3 DART consumer
must not be paired indiscriminately with older fallback kernels: integration
uses five-cell DART IOMMU specifiers on display and other devices.

The source comparison is **04a63b11701363386e3691b7ffa7e64eef477fa7 →
43c790fed6bb57a12d7ee3df474a15da790b56e6**, with phandle numbering resolved
to paths. The generated appendix lists the exact compiled property/node
changes once the queued buildbox comparison completes. That result will
describe the frozen *source baseline*, not a newly read installed
`boot.bin`. Local status edits or loader fixups require an installed-DTB
snapshot to establish the exact executing delta. No target was accessed.

Material missed changes include the display DART aperture conversion;
dcpext0 sharing between HDMI and the three Type-C routes; PCIe-C kernel
initialization and corrected identity windows; ANS DMA coordination;
new disabled AVE resources; ANE performance-controller/PMP resources and
DRAM OPP metadata; USB4 loader aliases; and GPU reserved-memory node naming.
The proposed motion and peripheral-bus nodes are additional to that baseline.

## Verification and remaining acceptance work

Buildbox receipts and the generated resource/frozen-delta appendices are
pending in controller buildbox slots. Hardware boot, sensor samples, NFC,
display routing, wake and audio behavior were not tested. Full board schema
diagnostics must be distinguished from the focused binding check; a zero
`make` exit status alone does not mean `dtbs_check` is clean.

Complete acceptance still requires a J316c ADT, an installed frozen-DTB
snapshot, property-level review of firmware-owned services, loader calibration
handoff, and hardware qualification of any enabled functionality.
