# Aurora unified-kernel launch roadmap

[Live roadmap and current status](https://github.com/omacom/linux-aurora/issues/56).

**Goal: one Aurora kernel and one installer for M1, M2, M3 and Neo.** Development lives on [`integration/aurora`](https://github.com/omacom/linux-aurora/tree/integration/aurora). Firmware, bootloader and Mesa profiles are selected for the actual Mac within that system.

Launch first with supported M3 Air/Pro and Neo profiles alongside M1/M2; add accelerated M3 Max through the same kernel next. The current Max path is kernel-only.

| Milestone | Deliverable | Work |
| --- | --- | --- |
| 1. Preserve M1/M2 | Existing boot, graphics and common-driver behavior remains working | Focused checks of changed paths in [#72](https://github.com/omacom/linux-aurora/issues/72); J493 recovery [#21](https://github.com/omacom/linux-aurora/issues/21) and M2 Max compute [#25](https://github.com/omacom/linux-aurora/issues/25) |
| 2. Finish Neo | Install, accelerated desktop and dependable Wi-Fi/DHCP | Association failure [#20](https://github.com/omacom/linux-aurora/issues/20) |
| 3. Finish M3 Air/Pro | One-line installation, responsive accelerated desktop and correct firmware/profile selection | Stock Pro stage1 [#47](https://github.com/omacom/linux-aurora/issues/47); matched native GL package [#36](https://github.com/omacom/linux-aurora/issues/36); required firmware setup [#46](https://github.com/omacom/linux-aurora/issues/46); missing Air profile acceptance [#26](https://github.com/omacom/linux-aurora/issues/26) |
| 4. Ship the unified release | Matched packages, recovery, concise instructions and working Omacom one-liner | **[#72](https://github.com/omacom/linux-aurora/issues/72) is the single launch acceptance/delivery tracker** |
| 5. Add M3 Max acceleration | Native display and G15C GPU in the same kernel/installer | Naeem’s end-to-end tracker [#61](https://github.com/omacom/linux-aurora/issues/61); does not hold up supported Air/Pro/Neo delivery |

**Implementation order now:** [#20](https://github.com/omacom/linux-aurora/issues/20) → [#47](https://github.com/omacom/linux-aurora/issues/47) → [#36](https://github.com/omacom/linux-aurora/issues/36) → required launch scope of [#46](https://github.com/omacom/linux-aurora/issues/46). Release checks are consolidated in [#72](https://github.com/omacom/linux-aurora/issues/72); reuse supplied passing evidence and retest changed or still-unverified paths. No requirement to repeat every dock, monitor or board permutation before each release.

**Already merged for the next release:** installer download/EFI fixes ([#18](https://github.com/omacom/linux-aurora/issues/18)/[#19](https://github.com/omacom/linux-aurora/issues/19)), Omacom report routing ([#63](https://github.com/omacom/linux-aurora/issues/63)), bounded J493 Bluetooth recovery ([#66](https://github.com/omacom/linux-aurora/issues/66)), M2 Max compute policy ([#68](https://github.com/omacom/linux-aurora/issues/68)) and DMA-buf cleanup ([#70](https://github.com/omacom/linux-aurora/issues/70)). The published [2026.10.11.13 release](https://github.com/omacom/linux-aurora/releases/tag/aurora-2026.10.11.13) remains unchanged.

**Launch scope:** record working capabilities per board/firmware. Native OpenGL and Vulkan are separate; Air’s exact26.6.2 native GL profile does not promise Vulkan. Keep current display/sleep limitations explicit. Fix newly introduced regressions before shipping.

**Detailed work is organized here:**

- [Initial unified launch](https://github.com/omacom/linux-aurora/milestone/1): core implementation and focused delivery checks.
- [M3 Max next stage](https://github.com/omacom/linux-aurora/milestone/2): accelerated Max bring-up.
- [Platform maturity and wider coverage](https://github.com/omacom/linux-aurora/milestone/3): broader retests, performance, display/sleep refinements, trusted-key persistence and advanced hardware. Original reports and priorities remain intact; these are not all initial-launch gates.

**Parallel work:** VA-API/m1n1 ownership migration [#71](https://github.com/omacom/linux-aurora/issues/71) waits for GitHub administrator access. Existing desktop lock-intent delivery [#17](https://github.com/omacom/linux-aurora/issues/17) remains **P0** with the shared desktop owner; a kernel release does not close it or require bundling custom Hyprland/Aquamarine.

[Historical issue destinations](AURORA_ISSUE_HISTORY.md) retain the original reports and migration links.
