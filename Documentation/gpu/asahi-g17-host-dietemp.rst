.. SPDX-License-Identifier: GPL-2.0-only OR MIT

===============================
G17 host die-temperature ceiling
===============================

The J700 PMP closes its fast die-temperature loops on firmware-owned sensors
and publishes one byte of effort per admitted control lane.  The G17 host
selects the board lane from ``apple,fast-die-effort`` and converts effort
``e`` to the AGX integer-percent ceiling::

        ceiling = ((255 - e) * 100) / 255

The driver samples at the PMP controller's 5 ms period while GPU work is in
flight and once more on the idle transition.  Effort zero, a stopped PMP, an
invalid lane, or provider removal withdraws the host limit and restores the
three native AGX words captured after firmware initialization.

Firmware-data access contract
=============================

The complete read/write list is deliberately small:

* The PMP report provider reads the documented 64-bit running status at
  report offset ``0x10``.  This rejects samples from a stopped PMP.
* The provider reads the documented 32-bit fast die-temperature word at
  report offset ``0x1c10``.  The selected lane is one byte of this word.
* The GPU reads its host-built AGX power-state words at offsets ``0x128``
  (valid), ``0x12c`` (target milliwatts), and ``0x144`` (timescale).  These
  are captured before the first host limit so native policy can be restored.
* To publish a ceiling, the GPU writes target milliwatts, then timescale, then
  release-stores valid.  The firmware polls this coherent object; no command,
  doorbell, cache operation, or undocumented firmware address is involved.

The controller does not read the dormant J700 lifetime-target entry, the old
``+0xc000`` report area, the report write alias, a high qword, or any sparse
firmware word.  It does not write the AGX fast-die target or release fields;
the independent firmware limiter keeps those native values.

