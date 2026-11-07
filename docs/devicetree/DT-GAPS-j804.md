# DT gaps: J804 (Mac18,1)

Identity: Apple MacBook Pro (14-inch, M6); T8152 (M6/H18).

Audited against integration/aurora `43c790fed6bb57a12d7ee3df474a15da790b56e6`.
Public restore builds 27.2/26B7124 and 27.3 beta/26C5035r both contain this board.
This document contains hardware inventory and coverage facts; it contains no firmware blobs,
calibration values, tuning tables or raw property dumps.

## Coverage status

**Missing board support at the audited base.** There is no `t8152-j804.dts` and no common
`t8152.dtsi`. The existing standalone `t8152-j873g.dts` describes a minimal Mac mini
boot path; it is not a substitute for this board and does not establish peripheral coverage.

DTS and binding implementation must follow the shared T8152 work. Copying another
SoC's addresses or enabling devices with incomplete clock, power, interrupt or IOMMU
relationships would give a misleading coverage claim. A board-only include cannot pass
the required schema checks until that common description and its bindings are available.
No W=1/dtbs_check result or hardware qualification is claimed for this board.

Required implementation:

- Add `t8152-j804.dts`, its Makefile entry and the board compatible in the platform binding.
- Describe every hardware endpoint below, including devices whose drivers do not exist.
- Give each device a SoC compatible with a suitable generic fallback and a schema.
- Translate MMIO through its parent address ranges; resolve IRQ, clock, power and IOMMU
  references to Linux phandles. Firmware phandle numbers are not Linux phandle numbers.
- Keep unsafe or unimplemented devices disabled. Leave calibration and runtime firmware
  reservations to the loader; do not publish tuning values in DTS.
- Validate the resulting board DTB with W=1, dt_binding_check and dtbs_check, then qualify
  the boot handoff before claiming that the board works.

## Board-specific differences

The board exposes three USB-C, HDMI and MagSafe. Its peripheral graph must remain board-specific.
Both builds identify the same SoC and board; the beta adds two brightness-policy
containers without adding a new board identity. Those policy containers do not establish
new MMIO hardware or justify enabling a peripheral.

The laptop graph includes the keyboard/trackpad transport, backlight, battery/charging,
SD-reader helper and an HDMI path. Its internal panel, audio endpoints and power topology
must not be inherited from the Mac mini or iMac.

## Whole-tree accounting

The release template contains 585 nodes. The table accounts for every top-level
subtree; its node count includes the subtree root and all descendants. Counts are
inventory bookkeeping, not evidence that a Linux binding exists.

| Subtree | Nodes | Required disposition |
| --- | ---: | --- |
| `/chosen` | 18 | Loader handoff/runtime data; translate the relevant Linux representation |
| `/options` | 1 | Firmware policy/metadata container; do not copy as MMIO hardware |
| `/cpus` | 13 | Translate hardware and its relationships; binding and enable policy required |
| `/aliases` | 1 | Loader handoff/runtime data; translate the relevant Linux representation |
| `/memory` | 1 | Loader handoff/runtime data; translate the relevant Linux representation |
| `/pram` | 1 | Loader handoff/runtime data; translate the relevant Linux representation |
| `/vram` | 1 | Loader handoff/runtime data; translate the relevant Linux representation |
| `/socd-trace-ram` | 1 | Loader handoff/runtime data; translate the relevant Linux representation |
| `/hibernate` | 1 | Loader handoff/runtime data; translate the relevant Linux representation |
| `/hib-exclave-watchdog` | 1 | Translate hardware and its relationships; binding and enable policy required |
| `/pcie-sdreader-helper` | 1 | Translate hardware and its relationships; binding and enable policy required |
| `/arm-io` | 460 | Translate hardware and its relationships; binding and enable policy required |
| `/buttons` | 1 | Translate hardware and its relationships; binding and enable policy required |
| `/port-usb-c-1` | 1 | Translate hardware and its relationships; binding and enable policy required |
| `/port-usb-c-2` | 1 | Translate hardware and its relationships; binding and enable policy required |
| `/port-usb-c-4` | 1 | Translate hardware and its relationships; binding and enable policy required |
| `/port-hdmi-1` | 1 | Translate hardware and its relationships; binding and enable policy required |
| `/port-magsafe3-1` | 1 | Translate hardware and its relationships; binding and enable policy required |
| `/backlight` | 1 | Translate hardware and its relationships; binding and enable policy required |
| `/exbrightpil` | 1 | Separate light-sensor hardware references from firmware brightness policy |
| `/sacm` | 1 | Translate hardware and its relationships; binding and enable policy required |
| `/defaults` | 1 | Firmware policy/metadata container; do not copy as MMIO hardware |
| `/product` | 3 | Firmware policy/metadata container; do not copy as MMIO hardware |
| `/iboot-syscfg` | 2 | Firmware policy/metadata container; do not copy as MMIO hardware |
| `/filesystems-props` | 1 | Firmware policy/metadata container; do not copy as MMIO hardware |
| `/ced-ssw` | 3 | Firmware policy/metadata container; do not copy as MMIO hardware |
| `/osenvironments` | 64 | Firmware policy/metadata container; do not copy as MMIO hardware |
| `/exclaves-test` | 1 | Firmware policy/metadata container; do not copy as MMIO hardware |

The remaining one node is the tree root (board compatible/model and bus cell declarations).

## Peripheral work list

`/arm-io` has 179 immediate children. Every child and its descendants are
assigned below to a work item. A row with no MMIO can be a bus consumer, interrupt-only
endpoint, firmware service or a container; it needs an explicit mapping or a reasoned
waiver, rather than a fabricated register range. Nested nodes include bus endpoints and
firmware-owned resources and must be reviewed before this work item can be closed.

| Peripheral work item | Nodes in subtree | MMIO property | IRQ property | Status at audited base |
| --- | ---: | --- | --- | --- |
| `/arm-io/spi2` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/spi4` | 8 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/i2c0` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/i2c1` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/i2c3` | 7 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/i2c4` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/fpwm0` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-apcie2` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apcie2-piodma` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/displaymanager` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/nub-spmi-a0` | 4 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/nub-spmi-a1` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aop-spmi0` | 4 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dynamic-pil` | 3 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/centauri` | 1 | absent | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/bluetooth` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apcie` | 7 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-apcie0` | 5 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apcie0-piodma` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/acio-cpu0` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apciec0` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-apciec0` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apciec0-piodma` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/acio0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-acio0` | 25 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc0-dpxbar` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc0-dpphy` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc0-dpin0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc0-dpin1` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/acio-cpu1` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apciec1` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-apciec1` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apciec1-piodma` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/acio1` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-acio1` | 25 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc1-dpxbar` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc1-dpphy` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc1-dpin0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc1-dpin1` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc2-dpxbar` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc2-dpphy` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/acio-cpu3` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apciec3` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-apciec3` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apciec3-piodma` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/acio3` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-acio3` | 25 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc3-dpxbar` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc3-dpphy` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc3-dpin0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc3-dpin1` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/display-crossbar0` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/mcc` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aic` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aic-timebase` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/wdt` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/error-handler` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/soc-misc` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/soc-reporter` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aes` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/gpio0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aop-gpio` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/nub-gpio` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aop` | 11 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aop-exclave-mailbox` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aop-exclave-ioreporting` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aop2` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/pmgr-child` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/pmgr` | 4 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/event-log-handler` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/pmp` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/sep` | 5 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-sep` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/esm` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/sio` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-sio` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-sbr` | 13 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/ans` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/sart-ans` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/smc` | 7 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aon-ptd` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/rsm` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/uart0` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dockchannel-uart` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/mtp` | 7 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/mtp-aop-mux` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/qspi` | 5 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/i2c2` | 4 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dp2hdmi-gpio0` | 1 | absent | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc-phy2` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc-phy0` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-usb0` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/usb-drd0` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc-phy1` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-usb1` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/usb-drd1` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/atc-phy3` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-usb3` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/usb-drd3` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/nub-spmi0` | 4 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/nub-spmi1` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/disp0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dcp0-expert` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dcp` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-dcp` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-disp0` | 5 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dcp-sac-controller` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-disp0-isoc` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-disp0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dp-audio0` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-disp0-ced-ssw` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dispext0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dcpext0-expert` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dcpext0` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-dcpext0` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-dispext0` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-dispext0-isoc` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-dispext0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dp-audio1` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-dispext0-ced-ssw` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dispext1` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dcpext1-expert` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dcpext1` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-dcpext1` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-dispext1` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-dispext1-isoc` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-dispext1` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dp-audio2` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-dispext1-ced-ssw` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dptx0-aux` | 3 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dptx1-aux` | 3 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/scaler0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-scaler` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/jpeg0` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/jpeg1` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-jpeg` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/ave` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-ave` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/avd` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-avd` | 4 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apr` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-apr` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/ispRtb` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/isp` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-isp` | 6 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/isp-exclave-proxy` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/isp-exclave-s-proxy` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/ane` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-ane` | 10 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/ane1` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-ane1` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/ane-exclave-proxy` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/sgx` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/gfx-asc` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/gfx1-asc` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/mapper-gfx-asc` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/iop-audio-controller` | 20 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/iop-voicetrigger-controller` | 2 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-base-ns` | 9 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-leap-ns` | 2 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-aop-ced-ssw` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-leap-s` | 3 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/audio-shared-dart-proxy` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-hp-mic-ec-proxy` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/audio-hp-mic-proxy` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/admac-sllb-ec-proxy` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/audio-sllb-proxy` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/smctempsensor0` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/cpu-debug-interface` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dbgCPU` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/aft` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/timesync` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/fillmore` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/sapt` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/dart-tlbi-aperture` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/secure-repair` | 1 | present | present | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/eic` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/apple-processor-trace` | 1 | present | absent | Unmapped on this board; shared SoC/board translation required |
| `/arm-io/device-supports-gf2` | 1 | absent | absent | Unmapped on this board; shared SoC/board translation required |

A service or policy node is not automatically a Linux device. Waivers must name the
specific node and explain the software/container role. Conversely, lack of a driver
does not waive an actual hardware resource: describe it with a binding and keep it
disabled where necessary.

## Public provenance

- [Apple restore catalog](https://mesu.apple.com/assets/macos/com_apple_macOSIPSW/com_apple_macOSIPSW.xml)
- [Apple software lookup service](https://gdmf.apple.com/v2/pmv)
- [Public model/board metadata](https://api.ipsw.me/v4/device/Mac18,1?type=ipsw)

DeviceTrees were selected from remote ZIP members and parsed without downloading a
complete restore archive. The firmware extracts and detailed private mapping ledger
are intentionally excluded from this repository.
