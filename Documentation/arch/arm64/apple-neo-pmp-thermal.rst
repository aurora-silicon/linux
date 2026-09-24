J700 (MacBook Neo) PMP fast CPU thermal policy
==============================================

Status: production policy of the Aurora Silicon ``j700`` tree, ported from the
cleanroom-linux stopped-epoch PMP owner (Eryk Wieliczko, cleanroom commits
6bee911c2ff6, 49ff805e2f00, bbeeb69ccf29, ca74b2bfd7ec) and extended to the
firmware this machine runs. Keep this policy and the SMC backup active until a
replacement provides validated runtime thermal protection; a boot-time
frequency setting is not sufficient.

Architecture
------------

``CONFIG_APPLE_PMP_V2`` is a native passive owner for the stopped, iBoot
preloaded T8140 PMP (``drivers/soc/apple/pmp-v2.c``). It validates the
resident firmware in SRAM, applies the own-ADT patchbay values to resident
DATA while the processor is stopped, owns stream 0 of the PMP DART with a
Linux four-level DART2 page table inside the ADT DVA window, verifies and
programs the 26 SID0 address-filter rows of the own-ADT table, starts the
firmware exactly once (RTKit protocol 12) and answers its passive registry
endpoint. Shared-memory quiescence, restart and suspend are not qualified:
after any failure all published DMA is retained until platform reset, which
is also why kexec is disabled.

The PMP DART ownership lives in ``drivers/soc/apple/pmp-v2-dart.h`` as a
private translation instance of the owner instead of the cleanroom's profile
inside ``drivers/iommu/apple-dart.c``: the j700 tree's apple-dart driver
carries the DCP display handoff and is not modified for the PMP. The
semantics are those of the cleanroom profile: read-only admission that never
adopts an inherited valid SID0 root, every other stream and the TRAD
tunables retained as iBoot left them, exclusive fault interrupt that latches
the epoch failed, no unmap after failure.

``CONFIG_APPLE_PMP_V2_THERMAL`` adds the polled policy
(``drivers/soc/apple/pmp-v2-thermal.h``): once cpufreq has both clusters at
their minimum state and the SMC provider is healthy it starts the epoch,
waits for the registry inventory, registers the ``apple-pmp-selected``
``step_wise`` zone through the SMC provider's cpufreq cooling devices and
releases the cpufreq boot caps. The thermal core samples every 25 ms in both
active and passive modes. Frequency QoS constraints are the only actuator.

Firmware identity and the private sensor ABI
--------------------------------------------

Each observation validates the resident firmware identity, coherent provider
records, pointer bounds and a delivery timestamp no more than 5 ms old *at
observation*. The sample reader uses fixed private DATA addresses (timer
descriptor 0x104ffb8, cache line size 0x1050028, the two temperature
providers 0x1053ce0/0x10541f0 with their configuration objects
0x104abf8/0x104afb0 and heap caches in 0x105e3c0..0x10963c0), so it is only
enabled for an admitted firmware build. ``drivers/soc/apple/pmp-v2-profile.h``
admits a build by the RTKit UUID in the firmware info block at TEXT+0x204 and
the SHA-256 of the whole resident __TEXT segment:

* 25F84 (macOS 26.5.2, RTKit-3255.120.11.release), UUID
  f3b1bca0-7a39-313e-9f0b-adcbf1cfe366, the originally qualified build.
* 25G83 (macOS 26.6.2, RTKit-3255.160.4.release), UUID
  84a026de-8896-3cf4-a5b5-367879c8fbee, TEXT SHA-256
  8f944b29b8d6f6cebe645f9dc751084b3614963ff011c887d47031873d7cd84a.

The 25G83 identity was derived from ``Firmware/pmp/t8140pmp.im4p`` of the
installed IPSW (``UniversalMac_26.6.2_25G83_Restore.ipsw``, Mach-O preload
payload, TEXT at file offset 0x1000, 0x3c000 bytes). The same derivation on
the 25F84 payload reproduces the cleanroom's digest constant exactly, which
establishes that iBoot loads TEXT verbatim. The private DATA offsets carry
over because the two payloads are functionally identical: every load
command, section address and section size matches except that the RTKit
version string in __TEXT.__const is one byte shorter in 25G83; the only
__text differences are 404 ADD-immediate string references adjusted by one
and the four words of the UUID, and the only __DATA differences are 273
bytes of string pointers adjusted by one. ``_rtk_mtab``, ``__cstring``, every
``_rtk_*`` DATA section and the zerofill layout are byte-identical, and the
live ADT nub reports the 25G83 UUID.

Sampling and failures
---------------------

Repeated or backwards timestamps, pending/dropped deliveries, failed reads
and unhealthy SMC telemetry reset recovery and request both minimum states.
Three advancing valid pairs permit automatic recovery without reboot. If the
temperature is already above the trip, both cooling devices must first have
engaged before the independent error guard is released. A zone disable holds
minimum; re-enable restarts the three-read recovery. An actuator failure or
a fault after admission remains latched at minimum.

Startup failure is different on this tree: with ``pmp_v2.startup_fallback``
(default on) a policy that never started, for example because the firmware
identity is unknown or the DMA epoch could not be established, releases its
own constraints and the cpufreq boot caps and logs the reason; the
independent SMC policy over the full OPP range, the regime this
tree qualified before the PMP policy, is then the throttle.
``pmp_v2.startup_fallback=0`` keeps the cleanroom fail-closed behaviour
(both clusters pinned to their minimum state until reboot).

Qualification settings
----------------------

The first passive trip is 95 C with 10 C hysteresis. Its minimum cooling
state immediately limits the P cluster to at most 2.472 GHz (P6) and reduces
the E cluster by at least one state; ``step_wise`` can impose stronger
cooling if temperature continues rising. A second passive trip at 100 C binds
both clusters to maximum cooling (minimum frequency) with 10 C hysteresis.
State indices are derived from each cooling device's cpufreq table.

While this policy runs, the SMC provider's own ``apple-smc-selected`` zone
is retuned into the backup: passive trip 105 C (hysteresis 10 C) and the
hard QoS clamp from 105 C (release 100 C). Without the PMP policy the SMC
zone uses a 95 C passive trip (release 85 C) and a 103 C hard QoS clamp
(release 98 C). Its passive binding requests maximum cooling immediately;
it does not walk down the frequency table one state at a time. These are
Linux operating-policy thresholds, not independently qualified silicon
safety limits.

The default ceiling is E6/P17 (2.424/4.044 GHz). The read-only
``pmp_v2.max_pstate`` and ``pmp_v2.max_estate`` parameters may request lower
ceilings for testing. The PMP policy requires ``idle=nop arm64.nowfxt``. Alternatively,
``pmp_v2.thermal_policy=0 idle=wfi arm64.nowfxt`` selects the SMC-only
policy and the T8140 state-preserving WFI path. The architecture NOP idle
path spins continuously and can substantially increase idle power and
temperature. Neither mode establishes a sustained all-core peak-frequency
guarantee; the thermal constraints still apply.

The driver permits s2idle while retaining the firmware epoch; this is not
proof of platform suspend/resume qualification. Deeper suspend states and
runtime suspend are rejected. CPU hot-unplug is vetoed after PMP policy
attachment, and kexec remains unsupported.

Board description
-----------------

The bootloader must forward the machine's own PMP metadata into the device
node at boot: patchbay inputs, resident segments, the firmware nub properties,
and the dart-pmp address-filter table and tunables. The kernel's board DTS
leaves this optional owner disabled until that handoff is complete. It embeds
neither a captured nub payload nor a per-device calibration/policy table.
See ``Documentation/devicetree/bindings/soc/apple/apple,t8140-pmp-v2.yaml``.

The source DT therefore remains usable with a loader that cannot supply this
metadata: the selected SMC thermal policy governs CPU frequency, and the PMP
startup fallback releases its boot caps only through the documented path.
Generated runtime metadata belongs to the individual boot; it is not part of
the distributable source or unpatched DTB.

Host regression tests
---------------------

``tools/testing/selftests/cpufreq/apple-pmp-*.py`` and
``apple-soc-command-test.py`` compile the real reader, policy and command
writer against mocked samples and QoS operations with ASan/UBSan. The KUnit
suite ``CONFIG_APPLE_PMP_V2_KUNIT_TEST`` covers the passive registry,
allocation ownership and the identity matcher. None of these emulate
hardware timing, thermal-core scheduling or the physical temperature curve.

Run the source-backed host fixtures with::

  make -C tools/testing/selftests/cpufreq apple-host-tests

Requested and delivered frequency
--------------------------------

The T8140 driver deliberately has no ``get`` callback: command-register
readback identifies the nominal requested state, not the delivered core
clock. ``scaling_cur_freq`` alone therefore cannot qualify full-frequency
operation. Record both policy tables and limits, all six online CPUs,
thermal constraints and an independent per-core clock measurement under
bounded load. Full supported states are E6 (2.424 GHz) and P17 (4.044 GHz);
sustained all-core performance remains subject to thermal throttling.
