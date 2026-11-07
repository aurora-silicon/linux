# J413 device-tree audit

Status: partial offline audit; the task's exhaustive-property and installed-DTB
acceptance criteria are not yet established.

Audit date: 2026-11-07. Board: **J413, M2 MacBook Air, T8112**.
Integration baseline: `43c790fed6bb57a12d7ee3df474a15da790b56e6`.

Evidence is this J413's saved IODeviceTree capture dated 2026-11-05.
Register addresses are translated through its `arm-io/ranges`. Raw captures,
unit identity, boot manifests, firmware, calibration and tuning remain outside
public source. See [the audit method](DT-AUDIT-METHOD.md).

## Changes prepared by this audit

| ADT evidence | DTS change | Driver / limits |
|---|---|---|
| `/arm-io/aop/iop-aop-nub/accel` and `gyro` | Add disabled J413 motion service children and document the T8112 compatibles | `drivers/iio/common/aop_sensors/aop_motion.rs`; without bootloader calibration the sensor remains off. Older-firmware sample format and calibration handoff need qualification. |
| `/arm-io/spi4`: physical `0x235110000`, size `0x4000`, AIC IRQ 752 | Describe SPI4, disabled | `drivers/spi/spi-apple.c`; no DP855 child or pin configuration is enabled. |
| `/arm-io/nub-spmi1`: first window at `0x23d794000` | Describe the polled controller window, disabled | `drivers/spmi/spmi-apple-controller.c`; no auxiliary PMIC function is enabled. |
| `nub-spmi1/pmu-aux`, Vale at SID 14 | Describe a read-only NVMEM provider below the disabled bus and document Vale identity | `drivers/nvmem/apple-spmi-nvmem.c`; no regulator map or extra NVMEM cells are inferred. |
| `dp-audio0`, `dp-audio1`: SIO channels `0x64` and `0x66`, with their own PMGR gates | Add a T8112 binding and remove the unused `reset-domains` properties | `drivers/gpu/drm/apple/audio.c` matches `apple,dpaudio`; DMA, power and graph routes are retained. |
| `/arm-io/aop-spmi0`, `aop-spmi1`: first windows at `0x24a894000`, `0x24a8d4000` | Describe both polled controller windows, disabled | Same driver; no AP bus ownership transfer, interrupt-controller mode, radio or NFC child is enabled. |

The peripheral descriptions are in `t8112.dtsi`; other T8112 boards inherit
disabled nodes. J413 is the only T8112 board whose evidence was inspected
here. Motion service children are added only on J413.
The AOP core registers its own service devices without an explicit child
availability check. `status = "disabled"` therefore cannot alone be claimed
as a tested runtime gate; the driver's missing-calibration refusal is an
independent prerequisite. Qualify availability handling before loader handoff.

## Remaining device and property gaps

| ADT device/property | Integration description | Disposition |
|---|---|---|
| AOP `accel-*`, `gyro-*` calibration, gyro temperature table, HID usage and service IDs | Motion service children were missing | Nodes added. The driver consumes three boot-time calibration containers per service. No calibration bytes belong in DTS. Loader handoff and sample qualification remain required. |
| AOP PDM microphone settings and decimator children | `aop/audio`, AOP ADMAC and J413 microphone identity exist | Firmware/calibration configuration, not extra AP MMIO nodes. Tuning is not ported. |
| `aop-spmi1/stockholm-spmi`, SID 12, and its `stockholm` GPIO child | No NFC/SE child | NFC/SE hardware is present. [Stockholm PR](https://github.com/aurora-silicon/linux/pull/1042) concerns other SoCs; no T8112 match is integrated. Requires IRQ/enable-line/ownership qualification. |
| `aop-spmi0/spmi-wifibt` | No radio SPMI child | Parent described disabled. Wi-Fi/Bluetooth use the existing PCIe host path; the SPMI function has no qualified consumer binding. |
| `nub-spmi1/pmu-aux` (`pmu,vale`) | Primary Stowe PMIC only at baseline | Add the read-only Vale NVMEM transport description below the disabled bus. Regulator functions, register-cell maps and ownership remain unqualified. Generic NVMEM transport is not a regulator implementation. |
| `error-handler`, `event-log-handler`: registers, interrupts and source/status metadata | Neither device is modeled | `apple_soc_edac.c` has only T6041/T8140 match data and status/source handling. No T8112-compatible alias can be justified by device name alone. |
| `mcc` and DRAM configuration | PMP/PMGR performance descriptions exist; no T8112 MCC device | Integration has no matching T8112 MCC driver. Do not copy memory configuration payloads. |
| `jpeg0`, `jpeg1`, their DARTs and mapper SID 0 | Engines and DARTs absent at the baseline | Integration JPEG matches T6041/T8140. Existing branch `jpeg/t8112-t6000-layouts` contains T8112 match data and disabled DT nodes; retain its recovery/qualification prerequisites instead of duplicating it. |
| `apr`, `dart-apr` | No ProRes engine/DART | Integration ProRes matches T8140 only; T8112 register/reset/descriptor compatibility is unqualified. |
| `scaler0`, `dart-scaler`, mapper children | No scaler/CSC node | No matching Apple scaler driver in integration. |
| `dart-scdc` | No corresponding IOMMU/consumer | No qualified SCDC consumer path; a standalone orphan DART is not functional codec support. |
| `ave`, `dart-ave` | AVE and two DART windows exist disabled | T8112 match data exists, but firmware/loader handoff is still required. Keep disabled. |
| `pmp` firmware/performance services | PMP, DART, mailbox and report nodes exist, but PMP remains disabled | Existing [PMP opt-in PR](https://github.com/aurora-silicon/linux/pull/1064) owns the driver/loader and AVD performance coordination. Do not add an isolated status flip. |
| AVD PIODMA/resource windows | AVD/DART present | [AVD PIODMA PR](https://github.com/aurora-silicon/linux/pull/710) owns the additional description. Do not duplicate it. |
| `spi4/dp855`; its `lcd-pmicwp`, `lcd-pmic`, `lcd-sswp`, `lcd-ss`, `tcon-registers`, `lcd-eeprom` | No bridge child | SPI4 described disabled. There is no DP855 bus/transport binding in integration; these are indirect display resources. |
| `i2c2/atcrt0` at `0x18`, `atcrt1` at `0x19`; their `AppleTypeCRetimer` children | I2C2 exists disabled, no retimer children | ADT compatible is only `atcrt`; no matching driver/binding. Generic Type-C retimer support does not identify this hardware. Graph insertion waits for a real binding. |
| `i2c4/avellino-pre` at `0x77`, `avellino-post` at `0x44` | I2C4 enabled, no children | No compatible or qualified driver is provided by the capture; do not invent one from the device names. |
| Plain `pwm` at `0x235040000` | FPWM1 at `0x235044000` exists for keyboard backlight | Distinct plain-PWM interface; no matching driver established. It must not be aliased to the FPWM driver. |
| `dwi`, `lcd0-sac`, `dcp0-expert`, `dcpext-expert`, `dcp-sac-controller`, backlight and `sacm` | DCP/panel and external display graphs exist | Firmware display services and feature/policy descriptions, with no separate AP driver. Their presence does not prove Linux display-feature parity. |
| `dp-audio0/1` resource properties | DTS carried `reset-domains` and legacy register windows; no T8112 schema existed | Add the binding and drop the ignored reset property. The capture has DMA/gate identities but no `reg`; existing register windows are retained as an optional legacy description and remain unqualified by this capture. The firmware audio driver does not map them. |
| `aes`, `dart-sio/mapper-aes` SID 1 | No AES consumer | No matching Apple AES driver. The existing SIO DART is not an AES implementation; storage/key ownership needs separate support. |
| `dart-aop/mapper-scm`, SID 6 | No corresponding consumer | The capture names this additional mapper but does not establish a matching Linux driver or ownership handoff. Do not attach it to the ADMAC SID-10 stream by analogy. |
| `nub-spmi0/btm`, sharing primary PMIC SID 14 | No dedicated BTM function | No matching consumer was established. Existing PMIC NVMEM and SMC battery/thermal services are separate interfaces. |
| PMU `info-rtc*`, alarm control/masks/events, fault and scratchpad functions | Stowe NVMEM cells plus SMC RTC/reboot/power services | Direct PMIC RTC/alarm is not modeled. Existing SMC RTC is a separate path; no T8112 direct-PMIC alarm driver is established. |
| `smctempsensor0`, firmware thermal descriptions | SMC hwmon and laptop sensor nodes; no J413 DT trip/cooling policy | More sensor channels and a policy require qualified keys/cooling mappings. Runtime SMC enumeration is needed for a complete sensor inventory. ADT does not justify invented trip values. |
| WLAN wake wiring | Wi-Fi/Bluetooth PCIe children present, no board `wakeup-source` | Existing `wifi/brcmf-wowlan-wake` branch supplies the associated driver and DT work. Integration lacks that retention/wake implementation. Physical wake is unqualified. |
| `smc-gpio` `wake-events`, `no-resume-restore`, supported interrupt groups | Physical SMC pin bank and virtual SMC GPIO service both exist | Resource description matches the capture. ADT wake/resume policy is not a literal `wakeup-source` conversion; AP/SMC ownership and wake routing remain unqualified. |
| `cpu-debug-interface`, `aft`, `timesync`, `apple-processor-trace` | No corresponding endpoint nodes | No qualified matching integration consumers. |

## Existing coverage that is not a new gap

The eight CPU nodes/topology, AIC/timebase, watchdog, GPIO banks, PMGR,
NCO, PMP/report transport, SEP, SPI2 fingerprint sensor, ANS/SART, SMC,
UART and dockchannel/MTP input stack, SPI1 NOR/NVRAM, I2C0–4, FPWM1,
SIO/ADMAC/MCA, ISP and ANE are described. J413 has no integrated SD-card
reader or HDMI connector in this capture; neither is a missing board node.
The physical SMC GPIO bank at `0x23e820000` already has the capture's
18 pins and AIC IRQs 490–496. It is distinct from `smc/gpio`, the virtual
SMC protocol provider.
The DART mapper identities match existing AOP SID 0, AOP ADMAC SID 10,
SIO SID 0, ADMAC SID 2, ISP SID 0 and ANE SID 0. AVD's additional
PIODMA/ADS-buffer mappers (SIDs 1 and 2) must not be conflated with the
existing SID-0 attachment; remaining consumer work is recorded above.

Both Type-C ports have ATC PHY, USB DRD/DART, ACIO processor/mailbox,
NHI, PCIe tunnel and display graph descriptions. The two unmodeled retimers
are a distinct gap from the existing USB/Thunderbolt host controllers.
The MagSafe HPM endpoint is also described on I2C0 at `0x3a`.

The four SN012776 controls map to I2C1 `0x38`/`0x39` and I2C3 `0x3b`/`0x3c`.
The CS42L84 jack codec is on I2C3 `0x4b`. DTS already describes the codec
children and MCA DAI links. Loopback and AOP microphone services are not
additional codec-control devices. Per-unit tuning and amplifier safety
configuration stay in their provisioning path.

Integration already includes the fingerprint data-ready GPIO 177 and power
GPIO 178. It would be incorrect to reopen the historical missing-IRQ gap
from the frozen source as a new integration gap.

## Frozen DTB set

The reference configuration is recorded as embedding the 7.1
`dtbs-04a63b117` set in `boot.bin`; newer installed kernels and `/boot` DTBs
do not automatically replace that set. An ANE status-only edit within the
old set is recorded separately. Therefore a baseline-source `disabled` value
must not be presented as a fresh observation of the executing J413 tree.

The queued appendix comparison will compare compiled source at
`04a63b11701363386e3691b7ffa7e64eef477fa7` and
`43c790fed6bb57a12d7ee3df474a15da790b56e6`, resolving phandles to paths.
The intended result is the exact source delta, with an installed-state limitation:
no current `boot.bin` or live DTB was read in this offline task.

Material changes missed by that baseline include five-cell display DART
specifiers/apertures; PCIe-C kernel initialization; USB4 loader aliases;
ANS DMA coordination; SEP DMA below 4 GiB; fingerprint data-ready IRQ;
the Wi-Fi country-code mapping property; trackpad click-strength description;
ANE performance-controller/PMP/DRAM metadata and ANE status changes;
new disabled AVE resources; and reserved-memory node naming. The proposed
motion/peripheral nodes are additional to the baseline comparison.

An embedded-DTB refresh requires an explicit boot-chain qualification that
accounts for every fallback kernel. No boot-chain change was made here.

## Verification and remaining acceptance work

Controller buildbox binding/DT checks and resource/frozen-delta appendices
are queued; no validation pass is claimed yet. Whole-board schema diagnostics
must be reported independently of a focused binding pass: Kbuild's successful
exit status does not establish a clean `dtbs_check` result.

No sensor sample, NFC, wake, display, audio or boot behavior was tested.
Complete acceptance still needs the installed frozen-DTB snapshot, full
property-level review of firmware-owned services, loader calibration handoff
and hardware qualification of enabled functionality.
