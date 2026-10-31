.. SPDX-License-Identifier: GPL-2.0

Aura PV-KVM discovery client
============================

``CONFIG_AURA_PV`` builds a small, opt-in diagnostic client for Aura's
physical-EL1 paravirtualization ABI.  The client only probes the interface
when the device tree contains ``aurora,aura-kvm-pv-0``; ordinary boots do not
issue any Aura HVC calls.

The module creates ``/dev/aura-pv`` and provides two ioctls from
``include/uapi/linux/aura_pv.h``:

* ``AURA_PV_IOC_DISCOVER`` returns the negotiated ABI and feature bitmap.
* ``AURA_PV_IOC_RUN`` submits a bounded integer program (up to eight unsigned
  additions) and returns the result plus host-register preservation sentinels.

This is a bring-up ABI probe, not a KVM implementation.  It does not claim
guest EL1 entry, stage-2 memory translation, virtual interrupt delivery,
timer preemption, or compatibility with ``/dev/kvm``.  Those capabilities
remain disabled until Aura and Linux have a complete state-switch protocol.
The shared control block is exactly 4096 bytes so it works with both 4 KiB
and 16 KiB Linux page configurations.

The module is intentionally not installed or enabled by default.  A kernel
must advertise ``CONFIG_AURA_PV=m`` (or ``y``) and Aura must have been booted
with its PV mode enabled.
