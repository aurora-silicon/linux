.. SPDX-License-Identifier: GPL-2.0-only

Preinitialized PCIe guests
=========================

``apple,apif-pcie`` admits an ECAM host whose producer has already configured
its clocks, PHY, link, address windows and RID-to-SID routes. The producer
must stop endpoint DMA and MSI before transferring ownership. Linux allocates
MSI vectors from the single native four-cell AIC edge range in ``msi-ranges``;
the doorbell is supplied by ``apple,msi-address``. Vector counts and addresses
are checked before the routing tables are touched. No polling interrupt path
or fixed IRQ assignment per function is introduced.

A second compatible, ``apple,t6050-pcie-apif``, selects the known port-0
register layout and supplies port, PHY, PERST and interrupt-to-AXI resources.
It restores explicit ``apple,rid-to-sid`` tuples and the same declared MSI
range after a controlled preboot cycle. Tunables come from the boot producer.
Unknown layouts use only ECAM/MSI; they cannot enter the T6050 reset sequence.

``apple_apif_pcie_cycle()`` is for the isolated N1 ROM/preboot function before
radio functions are enumerated or have live DMA. The caller must quiesce all
activity on that unit. PCI topology locking excludes sibling functions, and
DART transitions refuse other Linux groups or retained firmware streams.
Timeouts are bounded. A partial transition leaves bus mastering and bridge
decoding stopped; it does not attempt a speculative DMA restart. This is not
a general PCI bus reset API and does not add N1 Wi-Fi/Bluetooth drivers.

The generic host and its matched port sequence require hardware qualification
with the matching Aura monitor and boot description. Host sequence tests and
native builds do not establish link recovery or radio firmware behavior.
