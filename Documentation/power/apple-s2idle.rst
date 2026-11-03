.. SPDX-License-Identifier: GPL-2.0

Apple Silicon suspend-to-idle wake qualification
===============================================

A missing wake IRQ number does not establish that an Apple Silicon system
has no wake source. There are two interrupt paths to distinguish:

* An ordinary wake interrupt is enabled with ``enable_irq_wake()``. The IRQ
  core keeps it enabled during device IRQ suspension and reports a system
  wakeup when it arrives. AIC and AIC2/AIC3 use ``IRQCHIP_SKIP_SET_WAKE``:
  retained-power suspend-to-idle needs normal interrupt delivery rather
  than a second hardware wake routing configuration.
* An ``IRQF_NO_SUSPEND`` interrupt continues to run its handler. The handler
  or its consumer must identify a genuine wake event and report it to the
  PM core. The interrupt alone does not end system suspend. This permits
  transports with unrelated traffic to filter their wake events.

SMC mailbox interrupts use the second path. The SMC input driver makes
power-button presses and lid-open events hard wake events during its sleep
window. The RTKit notification worker must remain runnable. Do not arm the
whole SMC mailbox as an ordinary wake IRQ: unrelated notifications and
command replies are not user wake requests.

Dockchannel HID also uses the second path. Its RX child interrupt stays
live below a chained AIC parent. The HID transport classifies keyboard
reports and trackpad clicks before reporting a hard wake event; pointer
motion, touch and other interface traffic do not wake the system. On
systems using a receive DMA ring, both the coprocessor and its translations
must remain usable while the system sleeps.

Apple platform xHCI can keep running during suspend-to-idle and report
filtered USB wake events. Peripheral remote-wake support, hub policy and
controller retention all matter. An unarmed xHCI IRQ by itself does not
prove that this path is broken.

Bluetooth depends on the fitted radio and its transport driver. In
particular, the MT7932 driver is not used by a board with a Broadcom radio.
Check the bound driver, device wake policy, HCI event filters, PCI/MSI wake
propagation and the return value of wake IRQ arming. A driver must undo its
own suspend state if it returns an error after suspending HCI: the PM core
will not invoke its normal resume callback for that failed suspend.

An SMC RTC that provides timekeeping is not necessarily an alarm wake
source. Enumerate RTC names, alarm support and wake policy rather than
assuming that ``rtc0`` or a PMU driver enabled in the kernel configuration
provides a working recovery alarm. A PMIC's compatible string, register
layout, offset cell and interrupt wiring must all match its RTC driver.

Establish the failing phase
--------------------------

The ``PM: suspend entry (s2idle)`` message is printed before process
freezing and device suspend. It does not prove entry into the idle loop.
Likewise, loss of SSH can follow network suspension while another device
is still suspending. A black display does not establish whether CPUs have
resumed.

Capture the console independently of the display and network under test.
Use the existing ``power:suspend_resume`` and
``power:device_pm_callback_start``/``power:device_pm_callback_end`` trace
points, PM debug messages and callback timing. A ``machine_suspend`` entry
for suspend-to-idle establishes that the idle path was reached; callback
start/end pairs locate a device suspend or resume stall. See
:doc:`basic-pm-debugging` for staged PM tests. For suspend-to-idle, use
``freezer``, ``devices`` and ``platform``. The idle loop runs before the
``processors`` and ``core`` test checkpoints, so those levels do not
provide an automatic return from suspend-to-idle.

Before actual sleep, record:

* Board compatible strings, kernel release, source commit, configuration
  and the DTB used by the boot loader.
* ``/sys/power/mem_sleep`` and ``/sys/power/pm_test``.
* The wake policy of the SMC input, dockchannel HID, Bluetooth PCI function
  and USB devices; the bound drivers; relevant IRQ names and counts.
* ``/sys/kernel/debug/wakeup_sources`` and
  ``/sys/power/suspend_stats`` where available.
* Each RTC's name, alarm support and device wake policy.

Use ``/sys/power/pm_wakeup_irq`` for ordinary armed-IRQ attribution. For a
filtered transport, also compare its wakeup-source counters and interrupt
trace. An empty IRQ result is compatible with a valid filtered wake.

Qualification matrix
--------------------

Test one wake stimulus at a time, first after a short sleep and then after
repeated cycles:

* A short power-button press, and a lid close/open cycle.
* The built-in keyboard and a trackpad click. Separately verify that
  motion and touch do not cause an unwanted wake.
* A wake-enabled, paired Bluetooth keyboard with the fitted radio driver.
  Repeat with the PCI function's wake policy disabled.
* A directly connected USB keyboard, followed by the intended hub/dock,
  with the necessary device and hub wake policies enabled.
* A timed RTC alarm only on a device known to provide that functionality.

Also test a wake during entry, an aborted suspend, Bluetooth wake IRQ
arming failure, and suspend with Bluetooth wake disabled. A wake-arming
failure must abort suspend and leave the running transport recoverable;
successful cycles must balance wake IRQ enable/disable calls.

For every cycle record the stimulus, idle entry, wake event, resume
completion and post-resume input, storage and network health separately.
Preserve a failed cycle's console and trace before recovery. Keep a known
bootable fallback and an independently available recovery transport, and
restore any temporary debug settings and wake policies after testing.
