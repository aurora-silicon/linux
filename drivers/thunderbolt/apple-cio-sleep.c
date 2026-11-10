// SPDX-License-Identifier: GPL-2.0-only
/* Included by apple.c: one owner for the bound host's complete power cycle. */
static int apple_nhi_cio_router_sleep(struct tb_nhi *nhi)
{
	struct apple_nhi *anhi = nhi_to_anhi(nhi);
	struct tb_switch *sw = anhi->tb->root_switch;
	u32 command = 1, state, val;
	int cap, ret;

	cap = tb_switch_find_vse_cap(sw, TB_VSE_CAP_APPLE);
	if (cap < 0)
		return cap;
	/*
	 * The controller only accepts the vendor sleep request once the
	 * host router itself was told to sleep. Router-level wakes stay off.
	 */
	ret = tb_sw_read(sw, &val, TB_CFG_SWITCH, ROUTER_CS_5, 1);
	if (ret)
		return ret;
	val &= ~(ROUTER_CS_5_WOP | ROUTER_CS_5_WOU);
	val |= ROUTER_CS_5_SLP;
	ret = tb_sw_write(sw, &val, TB_CFG_SWITCH, ROUTER_CS_5, 1);
	if (ret)
		return ret;
	/* Sleep ready is advisory here; the controller state below decides. */
	for (unsigned int i = 0; i < 20; i++) {
		ret = tb_sw_read(sw, &val, TB_CFG_SWITCH, ROUTER_CS_6, 1);
		if (ret)
			return ret;
		if (val & ROUTER_CS_6_SLPR)
			break;
		usleep_range(10000, 11000);
	}
	dev_info(anhi->dev, "CIO host router sleep set, ready %s\n",
		 val & ROUTER_CS_6_SLPR ? "reported" : "not reported");
	ret = tb_sw_write(sw, &command, TB_CFG_SWITCH, cap + 0xb, 1);
	if (ret)
		return ret;
	ret = readl_poll_timeout(anhi->acio->rc_base + APPLE_CIO_M3_STAT, state,
				 state == 0x02000000, 10000, 1000000);
	dev_info(anhi->dev, "CIO controller sleep state %#x: %d\n", state, ret);
	return ret;
}

/* As at the first start: unlock the lane adapters, then the cable word starts the link. */
static int apple_nhi_cio_router_wake(struct tb_nhi *nhi)
{
	struct apple_nhi *anhi = nhi_to_anhi(nhi);
	struct tb_switch *sw = anhi->tb->root_switch;
	struct tb_port *port;
	int cap, ret;

	cap = tb_switch_find_vse_cap(sw, TB_VSE_CAP_APPLE);
	if (cap < 0)
		return cap;
	tb_switch_for_each_port(sw, port) {
		if (!tb_port_is_null(port))
			continue;
		ret = tb_port_unlock(port);
		if (ret)
			dev_warn(anhi->dev, "CIO port %u unlock failed: %d\n", port->port, ret);
	}
	ret = tb_sw_write(sw, &anhi->acio->current_cable_info, TB_CFG_SWITCH,
			  cap + TB_VSE_CAP_APPLE_CABLE_INFO, 1);
	dev_info(anhi->dev, "CIO cable word %#x written: %d\n", anhi->acio->current_cable_info,
		 ret);
	return ret;
}

static int apple_nhi_cio_bind_owner(struct apple_nhi *anhi)
{
	struct of_phandle_args args;
	struct platform_device *dart;
	struct phy *phy;
	int ret;

	if (anhi->acio->pd_list->num_pds != 4)
		return -EINVAL;
	phy = devm_of_phy_get(anhi->dev, anhi->acio->np, "cio");
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	ret = of_parse_phandle_with_args(anhi->np, "iommus", "#iommu-cells", 0, &args);
	if (ret)
		return ret;
	dart = of_find_device_by_node(args.np);
	of_node_put(args.np);
	if (!dart)
		return -EPROBE_DEFER;
	ret = apple_dart_cio_bind(anhi->dev, &dart->dev, &anhi->cio_dart);
	put_device(&dart->dev);
	if (ret)
		return ret;
	anhi->cio_phy = phy;
	anhi->ops.router_sleep = apple_nhi_cio_router_sleep;
	anhi->ops.router_wake = apple_nhi_cio_router_wake;
	return 0;
}

/* Without the owner, a deep sleep with a dock attached takes the stock path. */
static int apple_nhi_cio_bind(struct apple_nhi *anhi)
{
	int ret;

	if (!cio_s2r)
		return 0;
	if (anhi->acio->hw != &apple_cio_t8132_hw ||
	    !of_machine_is_compatible("apple,j713"))
		return 0;
	ret = apple_nhi_cio_bind_owner(anhi);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret)
		dev_warn(anhi->dev, "no CIO sleep owner: %d\n", ret);
	return 0;
}

static int apple_nhi_cio_prepare(struct apple_nhi *anhi)
{
	struct apple_cio *acio = anhi->acio;
	struct platform_device *pcie;
	int ret;

	if (!anhi->cio_dart || pm_suspend_target_state != PM_SUSPEND_MEM)
		return 0;
	scoped_guard(mutex, &acio->lock) {
		if (!acio->rtk || !acio->current_cable_info || acio->removing ||
		    acio->current_cable_info != acio->target_cable_info || acio->native_sleep)
			return -EBUSY;
		WRITE_ONCE(acio->native_sleep, true);
	}
	/* No new rings or topology may appear after the connection manager closes. */
	ret = tb_cio_sleep_prepare(anhi->tb);
	if (ret)
		goto release_cable;
	/* The released displays' routes go down on this queue: wait for them. */
	if (acio->dp_wq)
		flush_workqueue(acio->dp_wq);
	for (unsigned int i = 1; i < anhi->n_rings; i++) {
		if (anhi->nhi.tx_rings[i] || anhi->nhi.rx_rings[i]) {
			ret = -EBUSY;
			goto release_cm;
		}
	}
	cancel_delayed_work_sync(&anhi->dp_aux_work);
	ret = apple_atcphy_cio_prepare(anhi->cio_phy);
	if (ret)
		goto release_cm;
	ret = apple_dart_cio_prepare(anhi->cio_dart);
	if (ret)
		goto release_phy;
	scoped_guard(mutex, &acio->pcie_tunnel_lock)
		pcie = apple_cio_find_pcie_tunnel(acio);
	if (!pcie) {
		ret = -ENODEV;
		goto release_dart;
	}
	ret = apple_pcie_tunnel_cio_prepare(&pcie->dev);
	if (ret) {
		put_device(&pcie->dev);
		goto release_dart;
	}
	anhi->cio_pcie = &pcie->dev;
	anhi->cio_prepared = true;
	anhi->cio_pd_suspended = 0;
	anhi->cio_rtk_off = false;
	anhi->cio_cycled = false;
	return 0;
release_dart:
	if (apple_dart_cio_finish(anhi->cio_dart))
		panic("Apple CIO: failed to release unused DART transaction");
release_phy:
	if (apple_atcphy_cio_finish(anhi->cio_phy))
		panic("Apple CIO: failed to release unused PHY transaction");
release_cm:
	tb_cio_sleep_complete(anhi->tb);
release_cable:
	scoped_guard(mutex, &acio->lock)
		WRITE_ONCE(acio->native_sleep, false);
	if (anhi->dp_aux_armed)
		mod_delayed_work(system_dfl_wq, &anhi->dp_aux_work, 0);
	return ret;
}

static int apple_nhi_cio_start_cpu(void *data)
{
	struct apple_nhi *anhi = data;

	return reset_control_deassert(anhi->acio->reset);
}

static int apple_nhi_cio_power_on(struct apple_nhi *anhi)
{
	static const unsigned int order[] = { 3, 0, 2, 1 };
	struct apple_cio *acio = anhi->acio;
	u32 state;
	int ret;

	dev_info(anhi->dev, "CIO restore PHY\n");
	ret = apple_atcphy_cio_on(anhi->cio_phy);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < ARRAY_SIZE(order); i++) {
		unsigned int n = order[i];
		struct device *pd = acio->pd_list->pd_devs[n];

		if (!(anhi->cio_pd_suspended & BIT(n)))
			continue;
		/* Balance every attempted suspend, including one which stayed on. */
		dev_info(anhi->dev, "CIO restore domain %u\n", n);
		dev_pm_genpd_resume(pd);
		anhi->cio_pd_suspended &= ~BIT(n);
		ret = apple_pmgr_ps_verify_state(pd, true);
		if (ret)
			return ret;
	}
	if (anhi->cio_rtk_off) {
		/* The ACIO reset only deasserts: only its domains going off reset the CPU. */
		if (!anhi->cio_cycled)
			return -EUCLEAN;
		dev_info(anhi->dev, "CIO restart controller\n");
		ret = apple_rtkit_cold_restart(acio->rtk, apple_nhi_cio_start_cpu, anhi);
		if (ret)
			return ret;
		ret = apple_cio_lstx_release(acio);
		if (ret)
			return ret;
		ret = readl_poll_timeout(acio->rc_base + APPLE_CIO_M3_STAT, state,
					 (state & APPLE_CIO_M3_STAT_CODE_VALID) ||
					 FIELD_GET(APPLE_CIO_M3_STAT_STATE, state) ==
					 APPLE_CIO_M3_STAT_STATE_READY, 100, 500000);
		if (ret || (state & APPLE_CIO_M3_STAT_CODE_VALID))
			return ret ?: -EIO;
		apple_tunable_apply(acio->rc_base, acio->rc_tunable);
		if (acio->pcie_adapter_tunable)
			apple_tunable_apply(acio->pcie_adapter_base, acio->pcie_adapter_tunable);
		apple_tunable_apply(acio->router_ap_fw_base, acio->router_ap_fw_tunable);
		apple_tunable_apply(anhi->nhi_base, anhi->nhi_tunable);
		anhi->cio_rtk_off = false;
		anhi->cio_cycled = false;
	}
	/* The CPU and domains are back before the DART opens the NHI streams. */
	dev_info(anhi->dev, "CIO restore NHI DART\n");
	return apple_dart_cio_on(anhi->cio_dart);
}

/* A PCIe-C port whose tunnel went with a lost router does not wait for its link. */
static int apple_nhi_cio_wake(struct apple_nhi *anhi)
{
	bool pci_lost = false;
	int ret;

	ret = tb_cio_wake(anhi->tb, &pci_lost);
	if (!ret && pci_lost) {
		dev_warn(anhi->dev, "CIO dock lost during sleep; retiring its tunnels\n");
		apple_pcie_tunnel_cio_lost(anhi->cio_pcie);
	}
	return ret;
}

/* Take the four domains down in order; one already attempted is only checked. */
static int apple_nhi_cio_domains_off(struct apple_nhi *anhi)
{
	static const unsigned int order[] = { 1, 2, 0, 3 };
	int ret;

	for (unsigned int i = 0; i < ARRAY_SIZE(order); i++) {
		unsigned int n = order[i];
		struct device *pd = anhi->acio->pd_list->pd_devs[n];

		if (!(anhi->cio_pd_suspended & BIT(n))) {
			anhi->cio_pd_suspended |= BIT(n);
			dev_info(anhi->dev, "CIO stop domain %u\n", n);
			dev_pm_genpd_suspend(pd);
		}
		ret = dev_pm_genpd_is_on(pd) ? -EBUSY : apple_pmgr_ps_verify_state(pd, false);
		if (ret)
			return ret;
	}
	anhi->cio_cycled = true;
	return 0;
}

/* A map or unmap in flight closes the DART only briefly: wait for it. */
static int apple_nhi_cio_dart_off(struct apple_nhi *anhi)
{
	int ret;

	for (unsigned int i = 0; i < 20; i++) {
		ret = apple_dart_cio_off(anhi->cio_dart);
		if (ret != -EBUSY)
			break;
		usleep_range(500, 1000);
	}
	return ret;
}

static int apple_nhi_cio_suspend(struct apple_nhi *anhi)
{
	struct apple_cio *acio = anhi->acio;
	int ret, recover;

	if (!apple_pcie_tunnel_cio_stopped(anhi->cio_pcie))
		return -EBUSY;
	dev_info(anhi->dev, "CIO save paths and routers\n");
	ret = tb_cio_sleep(anhi->tb);
	if (ret)
		return ret;
	dev_info(anhi->dev, "CIO stop NHI DART\n");
	ret = apple_nhi_cio_dart_off(anhi);
	/* Asleep, the controller wakes only through a restart, which loses this DART. */
	if (ret)
		panic("Apple CIO: NHI DART not saved (%d) with the router asleep", ret);
	dev_info(anhi->dev, "CIO stop controller\n");
	ret = apple_rtkit_poweroff(acio->rtk);
	if (ret)
		panic("Apple CIO: CPU shutdown failed %d; DMA stays closed", ret);
	anhi->cio_rtk_off = true;
	apple_rtkit_mailbox_off(acio->rtk);
	ret = apple_cio_lstx_hold(acio, !!(readl(acio->rc_base + APPLE_CIO_M3_CTRL) &
						 APPLE_CIO_LSTX_CTRL_LEVEL));
	if (ret)
		goto unwind;
	ret = apple_nhi_cio_domains_off(anhi);
	if (ret)
		goto unwind;
	dev_info(anhi->dev, "CIO stop PHY\n");
	ret = apple_atcphy_cio_off(anhi->cio_phy);
	if (!ret)
		return 0;
unwind:
	dev_warn(anhi->dev, "CIO suspend failed %d; restarting the controller\n", ret);
	/*
	 * The host router took the vendor sleep: only a controller restart wakes
	 * it, and only its domains going off reset the CPU for that restart.
	 */
	if (!anhi->cio_cycled) {
		recover = apple_nhi_cio_domains_off(anhi);
		if (recover)
			panic("Apple CIO: suspend %d; controller not reset: %d", ret, recover);
	}
	recover = apple_nhi_cio_power_on(anhi) ?: apple_nhi_cio_wake(anhi);
	if (recover)
		panic("Apple CIO: suspend %d, recovery %d", ret, recover);
	return ret;
}

static int apple_nhi_cio_resume(struct apple_nhi *anhi)
{
	/* A lost dock is an unplug; only the host side must come back. */
	int ret = apple_nhi_cio_power_on(anhi) ?: apple_nhi_cio_wake(anhi);

	/* Child resume would otherwise access a dead tunnel or missing translations. */
	if (ret)
		panic("Apple CIO: resume failed %d; children remain suspended", ret);
	return 0;
}

static void apple_nhi_cio_complete(struct apple_nhi *anhi)
{
	struct apple_cio *acio = anhi->acio;
	int ret;

	if (!anhi->cio_prepared)
		return;
	ret = apple_pcie_tunnel_cio_finish(anhi->cio_pcie) ?:
	      apple_dart_cio_finish(anhi->cio_dart) ?:
	      apple_atcphy_cio_finish(anhi->cio_phy);
	if (ret)
		panic("Apple CIO: incomplete recovery at complete: %d", ret);
	tb_cio_sleep_complete(anhi->tb);
	put_device(anhi->cio_pcie);
	anhi->cio_pcie = NULL;
	anhi->cio_prepared = false;
	scoped_guard(mutex, &acio->lock) {
		WRITE_ONCE(acio->native_sleep, false);
		if (acio->target_cable_info != acio->current_cable_info)
			dev_warn(acio->dev, "cable changed during native sleep; revalidation required\n");
	}
	if (anhi->dp_aux_armed)
		mod_delayed_work(system_dfl_wq, &anhi->dp_aux_work, 0);
}
