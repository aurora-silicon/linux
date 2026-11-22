// SPDX-License-Identifier: GPL-2.0-only
/* Included by pcie-apple.c. The NHI owns this system-sleep transaction. */
static void apple_pcie_cio_set_changed(struct apple_pcie *pcie, bool changed)
{
	/* Config cycles run under the lock, from map_bus() to the access. */
	scoped_guard(raw_spinlock_irqsave, &pcie->cio_cfg_lock)
		WRITE_ONCE(pcie->cio_changed, changed);
}

int apple_pcie_tunnel_cio_prepare(struct device *dev)
{
	struct apple_pcie *pcie = apple_pcie_lookup(dev);
	int ret;

	if (!pcie || !pcie->cio_dart || pm_suspend_target_state != PM_SUSPEND_MEM)
		return -EOPNOTSUPP;
	if (pcie->cio_prepared || !list_is_singular(&pcie->ports) ||
	    pcie->bus_stopped || pcie->resume_failed ||
	    !apple_pcie_tunnel_link_healthy(pcie))
		return -EBUSY;
	/* ACIO's syscore domain devices hold the supplies until the NHI stops. */
	ret = apple_dart_cio_prepare(pcie->cio_dart);
	if (ret)
		return ret;
	apple_pcie_cio_set_changed(pcie, false);
	pcie->cio_stopped = false;
	pcie->cio_port_reset = false;
	pcie->cio_lost = false;
	pcie->cio_prepared = true;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_pcie_tunnel_cio_prepare);

bool apple_pcie_tunnel_cio_stopped(struct device *dev)
{
	struct apple_pcie *pcie = apple_pcie_lookup(dev);

	return pcie && pcie->cio_prepared && pcie->cio_stopped;
}
EXPORT_SYMBOL_GPL(apple_pcie_tunnel_cio_stopped);

/* The NHI found the tunnel's peer gone: do not wait for this link. */
void apple_pcie_tunnel_cio_lost(struct device *dev)
{
	struct apple_pcie *pcie = apple_pcie_lookup(dev);

	if (pcie && pcie->cio_prepared)
		pcie->cio_lost = true;
}
EXPORT_SYMBOL_GPL(apple_pcie_tunnel_cio_lost);

int apple_pcie_tunnel_cio_finish(struct device *dev)
{
	struct apple_pcie *pcie = apple_pcie_lookup(dev);
	int ret;

	if (!pcie || !pcie->cio_prepared)
		return -EINVAL;
	/* Only a port that failed to resume may still be changed. */
	if (pcie->cio_changed && !pcie->resume_failed)
		return -EUCLEAN;
	/* This ends the DART transaction whatever it reports. */
	ret = apple_dart_cio_finish(pcie->cio_dart);
	pcie->cio_prepared = false;
	/* A failed port was retired like a surprise removal; its DART logged. */
	return pcie->resume_failed ? 0 : ret;
}
EXPORT_SYMBOL_GPL(apple_pcie_tunnel_cio_finish);

/* All descendants are still suspended; the NHI has restored the router paths. */
static int apple_pcie_cio_resume(struct apple_pcie *pcie)
{
	struct apple_pcie_port *port;
	u32 stat;
	int i, ret;

	if (!pcie->cio_changed)
		return 0;
	port = list_first_entry(&pcie->ports, struct apple_pcie_port, entry);
	/* Until it is back, a later teardown must still stop the port. */
	port->needs_stop = true;
	if (pcie->cio_port_reset) {
		/*
		 * Rebuild after local PERST even if APPCLK never went off.
		 * Keep tunneled PERST asserted throughout the register replay.
		 */
		apple_pcie_port_rmw_set(port, PORT_TUNCTRL_PERST_ON,
					apple_pcie_port_tunctrl(port));
		ret = apple_pcie_tunnel_cold_init(port);
		if (ret)
			return ret;
	} else {
		apple_pcie_tunnel_pulse_intr2axi(pcie);
	}
	apple_pcie_tunnel_restore_irq_hw(port);
	for_each_set_bit(i, port->sid_map, port->sid_map_sz)
		apple_pcie_rid2sid_write(port, i, port->saved_rid2sid[i]);
	ret = apple_dart_cio_on(pcie->cio_dart);
	if (ret)
		return ret;
	/* Do not use tunnel_start(): its lost-power branch resets these maps. */
	apple_pcie_port_rmw_set(port, PORT_APPCLK_EN, PORT_APPCLK);
	apple_pcie_port_rmw_set(port, PORT_PERST_OFF, pcie->hw->port_perst);
	ret = read_poll_timeout(apple_pcie_port_readl, stat,
				stat & PORT_STATUS_READY,
				10, 250000, false, port, PORT_STATUS);
	if (ret) {
		dev_err(pcie->dev, "CIO PCIe-C READY timeout: status=%#x\n", stat);
		return ret;
	}
	/* The DART is back; the tunneled reset stays asserted with no peer. */
	if (pcie->cio_lost)
		return -ENOTCONN;
	ret = apple_pcie_tunnel_release_reset(port);
	if (ret)
		return ret;
	apple_pcie_port_writel(port, BIT(PORT_INT_LINK_UP) | BIT(PORT_INT_LINK_DOWN) |
			      BIT(PORT_INT_TUNNEL_ERR), PORT_INTSTAT);
	apple_pcie_port_writel(port, PORT_LTSSMCTL_START, PORT_LTSSMCTL);
	ret = read_poll_timeout(apple_pcie_port_readl, stat,
				stat & PORT_LINKSTS_UP,
				1000, 1000000, false, port, PORT_LINKSTS);
	if (ret) {
		dev_err(pcie->dev, "CIO PCIe-C link timeout: link=%#x events=%#x reset=%#x\n",
			stat, apple_pcie_port_readl(port, PORT_INTSTAT),
			apple_pcie_port_readl(port, apple_pcie_port_tunstat(port)));
		return ret;
	}
	apple_pcie_tunnel_link_up(pcie);
	apple_pcie_tunnel_wait_ready(pcie);
	if (!(apple_pcie_port_readl(port, PORT_LINKSTS) & PORT_LINKSTS_UP))
		return -ENOLINK;
	WRITE_ONCE(port->started, true);
	port->needs_stop = false;
	/* Publish the usable port before an interrupt can schedule link work. */
	wmb();
	apple_pcie_port_writel(port, port->saved_intmask, PORT_INTMSK);
	apple_pcie_port_enable_irq(port);
	apple_pcie_cio_set_changed(pcie, false);
	pcie->cio_stopped = false;
	pcie->cio_port_reset = false;
	dev_info(pcie->dev, "CIO PCIe-C link and DMA restored\n");
	return 0;
}

/*
 * T6050 PCIe-C, as traced from a macOS sleep and wake with a dock on this port (run mxt4, the
 * values as traced). The root port's express capability is at 0x70; the two fabric words and
 * the Intr2AXI word are written by the sleep with no name known for them.
 */
#define PCIEC_RP_SLTCTL			0x088
#define PCIEC_T6050_FABRIC_34		0x34	/* read 0x20, written 0 */
#define PCIEC_T6050_FABRIC_60		0x60	/* read 0x450045, written 0x45, then 0 */
#define PCIEC_T6050_I2A_00		0x00	/* read 0, written 0x78 */
#define   PCIEC_T6050_I2A_00_SLEEP	0x78
#define PCIEC_T6050_PME_US		100000	/* macOS: the send bit clears in 1.26 ms */
#define PCIEC_T6050_RESET_US		20000	/* macOS: the tunnelled reset reads back in 1.25 ms */

/*
 * The root port as macOS leaves it before the port goes down: the slot's hot-plug events
 * off, no decoding, no bus numbers. The PCI core saved the port before this and writes it
 * back at its resume, after a sleep and after one that was refused.
 */
static void apple_pcie_cio_t6050_root_down(struct apple_pcie *pcie)
{
	void __iomem *rp = pcie->cfg->win;

	guard(raw_spinlock_irqsave)(&pcie->cio_cfg_lock);
	writew(readw(rp + PCIEC_RP_SLTCTL) &
	       ~(PCI_EXP_SLTCTL_ABPE | PCI_EXP_SLTCTL_PDCE | PCI_EXP_SLTCTL_HPIE |
		 PCI_EXP_SLTCTL_DLLSCE), rp + PCIEC_RP_SLTCTL);
	writew(0, rp + PCI_COMMAND);
	writel(0, rp + PCI_PRIMARY_BUS);
}

/* The tunnelled reset, the port's own reset and its clock: the end of a stop, and of an undo. */
static void apple_pcie_cio_t6050_reset(struct apple_pcie *pcie, struct apple_pcie_port *port,
				       int *perst, int *ready)
{
	u32 stat;

	apple_pcie_port_rmw_set(port, PORT_TUNCTRL_PERST_ON, apple_pcie_port_tunctrl(port));
	*perst = read_poll_timeout(apple_pcie_port_readl, stat,
				   stat & PORT_TUNSTAT_PERST_ON,
				   100, PCIEC_T6050_RESET_US, false, port,
				   apple_pcie_port_tunstat(port));
	pcie->cio_port_reset = true;
	apple_pcie_port_rmw_clear(port, PORT_PERST_OFF, pcie->hw->port_perst);
	apple_pcie_port_rmw_clear(port, PORT_APPCLK_EN, PORT_APPCLK);
	*ready = read_poll_timeout(apple_pcie_port_readl, stat,
				   !(stat & PORT_STATUS_READY),
				   10, PCIEC_T6050_RESET_US, false, port, PORT_STATUS);
}

/*
 * J714s: the tunnel's root port takes PME_Turn_Off at its own register. Through the other
 * one nothing is sent: the send bit reads clear at once, no PME_TO_Ack is waited for, the
 * link never reaches L2, and it is stopped and reset from L0 before its routers are told
 * to sleep. Other boards keep the register they had; no trace of theirs has been read.
 */
static u32 apple_pcie_cio_t6050_pme_reg(void)
{
	return (of_machine_is_compatible("apple,j714s") ||
		of_machine_is_compatible("apple,j716c")) ? PORT_T6050_PCIEC_PME_TURN_OFF :
							 PORT_PME_TURN_OFF;
}

/*
 * T6050: macOS takes this port DOWN for a sleep and waits for no link (run mxt4, 6.6 ms
 * before the ATC2_PCIE power word goes off, the router still awake): the slot's events off;
 * the port's error interrupts masked and acknowledged; the root port's command and bus
 * numbers 0; the clock gate off and PME_Turn_Off sent (the send bit clears, the link reads
 * as in L2); link-down masked and the link's state machine stopped; the DART taken down
 * with its clock protection slice; two fabric words and an Intr2AXI word (no step that can
 * fail follows them, and the domain's power-off gives them back); every interrupt masked and
 * acknowledged; the tunnelled reset set and read back; the port's reset; its clock. At the wake the port is set up from cold, as at its first start, and the link
 * comes up through the tunnel the router kept (macOS: 70 us after the state machine starts).
 *
 * Every step that can fail comes before the router is told to sleep, so each one refuses
 * the sleep with the port put back: reset as for the sleep and started again from cold. A
 * port that does not come back is retired as after an unplug; the sleep is refused all the
 * same and nothing panics.
 */
static int apple_pcie_cio_suspend_t6050(struct apple_pcie *pcie)
{
	struct apple_pcie_port *port;
	ktime_t start = ktime_get(), sent;
	const char *step;
	bool idle;
	u32 stat, link, fabric[2] = {}, i2a = 0, pme_reg = apple_pcie_cio_t6050_pme_reg();
	int i, ret, recover, perst, ready, l2 = -ENODATA;
	s64 pme_us = 0;

	port = list_first_entry(&pcie->ports, struct apple_pcie_port, entry);
	/* Nothing of the port has been touched: a DART that cannot be saved only refuses. */
	ret = apple_dart_cio_check(pcie->cio_dart, true);
	if (ret) {
		dev_err(pcie->dev, "T6050 sleep: PCIe-C not stopped, its DART cannot be saved (%d): sleep refused, the port untouched\n",
			ret);
		return ret;
	}
	dev_info(pcie->dev, "CIO PCIe-C stop (T6050 order)\n");
	apple_pcie_port_disable_irq(port);
	port->saved_intmask = apple_pcie_port_readl(port, PORT_INTMSK);
	for_each_set_bit(i, port->sid_map, port->sid_map_sz)
		port->saved_rid2sid[i] = apple_pcie_port_readl(port, port_rid2sid_offset(port, i));
	apple_pcie_port_writel(port, PORT_INT_AER_MASK, PORT_INTMSKSET);
	apple_pcie_port_writel(port, PORT_INT_AER_MASK, PORT_INTSTAT);
	apple_pcie_cio_t6050_root_down(pcie);
	/* From here the functions behind the tunnel lose their state: the PCI core restores them. */
	apple_pcie_tunnel_hierarchy_lost(pcie->dev);
	apple_pcie_cio_set_changed(pcie, true);

	step = "PME_Turn_Off";
	apple_pcie_port_rmw_set(port, PORT_APPCLK_CGDIS, PORT_APPCLK);
	sent = ktime_get();
	apple_pcie_port_writel(port, PORT_PME_TURN_OFF_SEND | PORT_PME_TURN_OFF_BIT4, pme_reg);
	/* macOS reads the register once at once: keep its access order. */
	apple_pcie_port_readl(port, pme_reg);
	ret = read_poll_timeout(apple_pcie_port_readl, stat,
				!(stat & PORT_PME_TURN_OFF_SEND),
				50, PCIEC_T6050_PME_US, false, port, pme_reg);
	pme_us = ktime_us_delta(ktime_get(), sent);
	if (ret)
		goto unwind;
	/* macOS reads the link once here and goes on: L2 is reported, not required. */
	l2 = read_poll_timeout(apple_pcie_port_readl, link, link & PORT_LINKSTS_L2,
			       50, PCIEC_T6050_PME_US, false, port, PORT_LINKSTS);

	step = "transactions idle";
	apple_pcie_port_writel(port, BIT(PORT_INT_LINK_DOWN), PORT_INTMSKSET);
	apple_pcie_port_rmw_clear(port, PORT_LTSSMCTL_START, PORT_LTSSMCTL);
	ret = read_poll_timeout(apple_pcie_tunnel_transactions_idle, idle,
				idle, 10, 20000, false, port);
	if (ret)
		goto unwind;

	step = "DART off";
	ret = apple_dart_cio_off(pcie->cio_dart);
	if (ret)
		goto unwind;

	if (pcie->fabric_base) {
		fabric[0] = readl(pcie->fabric_base + PCIEC_T6050_FABRIC_34);
		fabric[1] = readl(pcie->fabric_base + PCIEC_T6050_FABRIC_60);
		writel(0, pcie->fabric_base + PCIEC_T6050_FABRIC_34);
		writel(fabric[1] & 0xffff, pcie->fabric_base + PCIEC_T6050_FABRIC_60);
		writel(0, pcie->fabric_base + PCIEC_T6050_FABRIC_60);
	}
	apple_pcie_port_writel(port, ~0, PORT_INTMSKSET);
	if (pcie->intr2axi_base) {
		i2a = readl(pcie->intr2axi_base + PCIEC_T6050_I2A_00);
		writel(PCIEC_T6050_I2A_00_SLEEP, pcie->intr2axi_base + PCIEC_T6050_I2A_00);
	}
	apple_pcie_port_writel(port, ~0, PORT_INTSTAT);
	apple_pcie_cio_t6050_reset(pcie, port, &perst, &ready);
	WRITE_ONCE(port->started, false);
	pcie->cio_stopped = true;
	dev_info(pcie->dev, "T6050 sleep: PCIe-C port down in %lld us: PME_Turn_Off sent in %lld us, link %#x (L2 %s), fabric %#x/%#x, Intr2AXI %#x, tunnelled reset %s, port %s\n",
		 ktime_us_delta(ktime_get(), start), pme_us, link,
		 l2 ? "not reported" : "reported", fabric[0], fabric[1], i2a,
		 perst ? "not read back" : "read back", ready ? "still ready" : "not ready");
	return 0;
unwind:
	/*
	 * PM does not resume a device whose own suspend callback failed. A link that took
	 * PME_Turn_Off, or whose state machine was stopped, trains again only behind a
	 * reset: the port goes down the rest of the way and comes back as after a sleep.
	 */
	dev_err(pcie->dev,"T6050 sleep: PCIe-C step \"%s\" failed (%d) after %lld us, status %#x, link %#x, events %#x: the port is reset and started again from cold, sleep refused\n",
		step, ret, ktime_us_delta(ktime_get(), start),
		apple_pcie_port_readl(port, PORT_STATUS),
		apple_pcie_port_readl(port, PORT_LINKSTS),
		apple_pcie_port_readl(port, PORT_INTSTAT));
	apple_pcie_port_writel(port, ~0, PORT_INTMSKSET);
	apple_pcie_port_writel(port, ~0, PORT_INTSTAT);
	apple_pcie_cio_t6050_reset(pcie, port, &perst, &ready);
	recover = apple_pcie_cio_resume(pcie);
	if (recover) {
		/* As a port that is not back after a sleep: closed, its functions gone. */
		dev_err(pcie->dev, "T6050 sleep: PCIe-C port not back after the refused stop (%d; tunnelled reset %s, port %s): retired as after an unplug, its functions return when the dock is plugged in again\n",
			recover, perst ? "not read back" : "read back",
			ready ? "still ready" : "not ready");
		pcie->resume_failed = true;
		apple_pcie_tunnel_hierarchy_gone(pcie->dev);
	} else {
		dev_warn(pcie->dev, "T6050 sleep: PCIe-C port back from cold after the refused stop: sleep refused (%d)\n",
			 ret);
	}
	return ret;
}

static int apple_pcie_cio_suspend(struct apple_pcie *pcie)
{
	struct apple_pcie_port *port;
	bool idle;
	u32 stat;
	int i, ret, recover;

	if (pcie->hw == &t6050_pciec_hw && (of_machine_is_compatible("apple,j714s") ||
					    of_machine_is_compatible("apple,j716c")))
		return apple_pcie_cio_suspend_t6050(pcie);
	port = list_first_entry(&pcie->ports, struct apple_pcie_port, entry);
	dev_info(pcie->dev, "CIO PCIe-C drain and reset\n");
	apple_pcie_port_disable_irq(port);
	port->saved_intmask = apple_pcie_port_readl(port, PORT_INTMSK);
	for_each_set_bit(i, port->sid_map, port->sid_map_sz)
		port->saved_rid2sid[i] = apple_pcie_port_readl(port, port_rid2sid_offset(port, i));
	apple_pcie_cio_set_changed(pcie, true);
	apple_pcie_port_writel(port, ~0, PORT_INTMSK);
	apple_pcie_port_rmw_clear(port, PORT_LTSSMCTL_START, PORT_LTSSMCTL);
	ret = read_poll_timeout(apple_pcie_tunnel_transactions_idle, idle,
				idle, 10, 20000, false, port);
	if (ret)
		goto unwind;
	apple_pcie_port_rmw_set(port, PORT_TUNCTRL_PERST_ON, apple_pcie_port_tunctrl(port));
	/* PERST may reset descendants even when the acknowledgment times out. */
	apple_pcie_tunnel_hierarchy_lost(pcie->dev);
	ret = read_poll_timeout(apple_pcie_port_readl, stat,
				stat & PORT_TUNSTAT_PERST_ON,
				1000, 100000, false, port, apple_pcie_port_tunstat(port));
	if (ret)
		goto unwind;
	pcie->cio_port_reset = true;
	apple_pcie_port_rmw_clear(port, PORT_PERST_OFF, pcie->hw->port_perst);
	/* The DART window is still clocked here. GAPF goes down before APPCLK. */
	ret = apple_dart_cio_off(pcie->cio_dart);
	if (ret)
		goto unwind;
	apple_pcie_port_rmw_clear(port, PORT_APPCLK_EN, PORT_APPCLK);
	ret = read_poll_timeout(apple_pcie_port_readl, stat,
				!(stat & PORT_STATUS_READY),
				10, 100000, false, port, PORT_STATUS);
	if (ret)
		goto unwind;
	apple_pcie_port_rmw_set(port, PORT_TUNCTRL_PERST_ACK_REQ,
				apple_pcie_port_tunctrl(port));
	/* T8132 reports the completed acknowledgment with bit 1 set. */
	ret = read_poll_timeout(apple_pcie_port_readl, stat,
				stat & PORT_TUNSTAT_PERST_ACK_PEND,
				1000, 1000000, false, port, apple_pcie_port_tunstat(port));
	apple_pcie_port_rmw_clear(port, PORT_TUNCTRL_PERST_ACK_REQ,
				  apple_pcie_port_tunctrl(port));
	if (ret)
		goto unwind;
	WRITE_ONCE(port->started, false);
	pcie->cio_stopped = true;
	dev_info(pcie->dev, "CIO PCIe-C stopped\n");
	return 0;
unwind:
	/* PM does not resume a device whose own suspend callback failed. */
	recover = apple_pcie_cio_resume(pcie);
	if (recover)
		panic("Apple PCIe-C: suspend %d, recovery %d; DMA remains closed", ret, recover);
	return ret;
}
