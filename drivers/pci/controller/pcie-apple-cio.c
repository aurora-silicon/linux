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

static int apple_pcie_cio_suspend(struct apple_pcie *pcie)
{
	struct apple_pcie_port *port;
	bool idle;
	u32 stat;
	int i, ret, recover;

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
