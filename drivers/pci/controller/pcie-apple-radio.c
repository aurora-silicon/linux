// SPDX-License-Identifier: GPL-2.0-only
/* Included by pcie-apple.c. J713 BCM4388 firmware retention and host recovery. */

#define RADIO_PHY_RESET BIT(4)
#define RADIO_PORT_TIMEOUT_US 50000
#define RADIO_DBI_RO_WR 0x8bc
#define RADIO_DBI_LINK_CONTROL 0x710
#define RADIO_DBI_WIDTH_SPEED 0x80c

enum apple_radio_phase {
	APPLE_RADIO_IDLE,
	APPLE_RADIO_OPEN,
	APPLE_RADIO_SEALING,
	APPLE_RADIO_SEALED,
	APPLE_RADIO_VERIFIED,
	APPLE_RADIO_ABORTED,
	APPLE_RADIO_FAILED,
	APPLE_RADIO_STOPPING,
	APPLE_RADIO_STOPPED,
	APPLE_RADIO_RECOVERING,
};

struct apple_radio_function {
	struct pci_dev *pdev;
	u16 command;
	bool attempted, held;
	u64 suspended, resumed;
	int error;
	void (*finish)(struct device *dev);
};

struct apple_radio_sleep {
	struct apple_pcie *pcie;
	struct apple_pcie_port *port;
	struct apple_dart_sleep dart;
	struct apple_radio_function fn[2];
	struct task_struct *owner;
	u64 generation;
	enum apple_radio_phase phase;
	bool bound, host_suspended, snapshot;
	bool recovery_ready, config_ready;
	struct gpio_desc *perst, *clkreq;
	struct pci_dev *root;
	struct apple_tunable *tables[4];
	u32 intmask, appclk;
	u32 rid[16], msi[32], msicfg, msibase, msiaddr, msiaddr_hi;
	DECLARE_BITMAP(vectors, 32);
	DECLARE_BITMAP(sids, 16);
};

/* A single exact board/controller may own this publication during system PM. */
static struct apple_radio_sleep *apple_radio_active;

static void __noreturn apple_radio_terminal(void)
{
	panic("J713 radio recovery failed; DMA remains blocked");
}

static bool apple_radio_writer_closed(struct apple_pcie *pcie)
{
	return READ_ONCE(pcie->radio_closed);
}

static bool apple_radio_config_allowed(struct apple_pcie *pcie)
{
	struct apple_radio_sleep *s = pcie->radio;

	if (!s || READ_ONCE(apple_radio_active) != s)
		return true;
	/* Pair with publication after the complete port/DART/link recovery. */
	switch (smp_load_acquire(&s->phase)) {
	case APPLE_RADIO_STOPPING:
	case APPLE_RADIO_STOPPED:
	case APPLE_RADIO_FAILED:
		return false;
	case APPLE_RADIO_RECOVERING:
		return current == s->owner && READ_ONCE(s->config_ready);
	default:
		return true;
	}
}

static int apple_radio_bme_hold(struct apple_radio_function *fn)
{
	u16 value;

	if (pci_read_config_word(fn->pdev, PCI_COMMAND, &value) || value == 0xffff)
		return -EIO;
	fn->command = value;
	/* A failed config write may already have reached the function. */
	fn->attempted = true;
	if (pci_write_config_word(fn->pdev, PCI_COMMAND, value & ~PCI_COMMAND_MASTER) ||
	    pci_read_config_word(fn->pdev, PCI_COMMAND, &value) ||
	    value != (fn->command & ~PCI_COMMAND_MASTER))
		return -EIO;
	fn->held = true;
	return 0;
}

static int apple_radio_bme_restore(struct apple_radio_function *fn)
{
	u16 value;

	if (!fn->attempted)
		return 0;
	if (pci_read_config_word(fn->pdev, PCI_COMMAND, &value) || value == 0xffff)
		return -EUCLEAN;
	/* Restore only our own change, never overwrite a foreign config mutation. */
	if (value != fn->command && value != (fn->command & ~PCI_COMMAND_MASTER))
		return -EUCLEAN;
	if (value != fn->command && pci_write_config_word(fn->pdev, PCI_COMMAND, fn->command))
		return -EUCLEAN;
	if (pci_read_config_word(fn->pdev, PCI_COMMAND, &value) || value != fn->command)
		return -EUCLEAN;
	fn->held = false;
	fn->attempted = false;
	return 0;
}

static struct apple_radio_function *apple_radio_endpoint(struct device *dev)
{
	/* Acquire the initialized generation published by begin. */
	struct apple_radio_sleep *s = smp_load_acquire(&apple_radio_active);
	int i;

	if (!s)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(s->fn); i++)
		if (&s->fn[i].pdev->dev == dev)
			return &s->fn[i];
	return NULL;
}

bool apple_radio_sleep_owned(struct device *dev)
{
	return apple_radio_endpoint(dev) != NULL;
}
EXPORT_SYMBOL_GPL(apple_radio_sleep_owned);

int apple_radio_sleep_endpoint_prepare(struct device *dev, void (*finish)(struct device *dev))
{
	struct apple_radio_function *fn = apple_radio_endpoint(dev);
	struct apple_radio_sleep *s;

	if (!fn)
		return 0;
	s = READ_ONCE(apple_radio_active);
	if (READ_ONCE(s->phase) != APPLE_RADIO_OPEN || pm_suspend_target_state != PM_SUSPEND_MEM)
		return -EPERM;
	fn->finish = finish;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_radio_sleep_endpoint_prepare);

int apple_radio_sleep_endpoint_suspended(struct device *dev)
{
	struct apple_radio_function *fn = apple_radio_endpoint(dev);
	struct apple_radio_sleep *s;

	if (!fn)
		return 0;
	s = READ_ONCE(apple_radio_active);
	if (READ_ONCE(s->phase) != APPLE_RADIO_OPEN || fn->suspended)
		return -ESTALE;
	/* Publish the endpoint stop before seal examines the receipt. */
	smp_store_release(&fn->suspended, s->generation);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_radio_sleep_endpoint_suspended);

int apple_radio_sleep_endpoint_resume(struct device *dev)
{
	struct apple_radio_function *fn = apple_radio_endpoint(dev);
	struct apple_radio_sleep *s;

	if (!fn)
		return 0;
	s = READ_ONCE(apple_radio_active);
	switch (READ_ONCE(s->phase)) {
	case APPLE_RADIO_OPEN: /* Device PM may undo a failed late/noirq phase. */
	case APPLE_RADIO_ABORTED:
	case APPLE_RADIO_VERIFIED:
		return 0;
	default:
		/* Returning an errno would still let PM resume other DMA consumers. */
		apple_radio_terminal();
	}
}
EXPORT_SYMBOL_GPL(apple_radio_sleep_endpoint_resume);

void apple_radio_sleep_endpoint_resumed(struct device *dev, int result)
{
	struct apple_radio_function *fn = apple_radio_endpoint(dev);
	struct apple_radio_sleep *s;

	if (!fn)
		return;
	s = READ_ONCE(apple_radio_active);
	WRITE_ONCE(fn->error, result);
	/* Final finish must observe the endpoint result with its receipt. */
	smp_store_release(&fn->resumed, s->generation);
}
EXPORT_SYMBOL_GPL(apple_radio_sleep_endpoint_resumed);

static int apple_radio_valid(struct apple_radio_sleep *s, u64 generation)
{
	if (!s->bound || !generation || s->generation != generation || s->owner != current ||
	    READ_ONCE(apple_radio_active) != s)
		return -ESTALE;
	return s->phase == APPLE_RADIO_FAILED ? -EUCLEAN : 0;
}

static int apple_radio_snapshot_error(struct apple_radio_sleep *s, const char *field,
				      unsigned int index, u32 value, u32 expected)
{
	/* Leave diagnostics silent if a caller has disabled interrupts. */
	if (!irqs_disabled())
		dev_err(s->pcie->dev, "radio sleep %s[%u]: observed=%#x expected=%#x\n", field,
			index, value, expected);
	return -EUCLEAN;
}

static int apple_radio_snapshot(struct apple_radio_sleep *s, bool verify)
{
	struct apple_pcie *pcie = s->pcie;
	struct apple_pcie_port *port = s->port;
	u32 value, expected;
	unsigned int i;

	value = apple_pcie_port_readl(port, PORT_STATUS);
	if (!(value & PORT_STATUS_READY))
		return apple_radio_snapshot_error(s, "port-ready", 0, value, PORT_STATUS_READY);
	value = apple_pcie_port_readl(port, PORT_LINKSTS);
	/*
	 * This is an operating link, not the stopped PCIe-C port checked by
	 * tunnel initialization. Bit 2 can remain set while link-up is valid.
	 * DMA quiescence is established separately by the endpoint receipts,
	 * checked BME fences and pending-transaction waits below.
	 */
	if (!(value & PORT_LINKSTS_UP))
		return apple_radio_snapshot_error(s, "link-state", 0, value, PORT_LINKSTS_UP);
	value = apple_pcie_port_readl(port, PORT_INTMSK);
	if (verify && value != s->intmask)
		return apple_radio_snapshot_error(s, "interrupt-mask", 0, value, s->intmask);
	if (!verify)
		s->intmask = value;
	if (!verify)
		s->appclk = apple_pcie_port_readl(port, PORT_APPCLK);
	for (i = 0; i < ARRAY_SIZE(s->rid); i++) {
		value = apple_pcie_port_readl(port, port_rid2sid_offset(port, i));
		if (verify && value != s->rid[i])
			return apple_radio_snapshot_error(s, "rid-compare", i, value, s->rid[i]);
		if (!verify)
			s->rid[i] = value;
	}
	for (i = 0; i < ARRAY_SIZE(s->fn); i++) {
		expected = 0;
		if (apple_pcie_managed_word(pcie->managed_sids, 0x100 + i, 1 + i, &expected) ||
		    s->rid[1 + i] != expected)
			return apple_radio_snapshot_error(s, "rid-admission", 1 + i, s->rid[1 + i],
							  expected);
	}
	for (i = 0; i < pcie->nvecs; i++) {
		value = apple_pcie_port_readl(port, pcie->hw->port_msimap + 4 * i);
		if (verify && value != s->msi[i])
			return apple_radio_snapshot_error(s, "msi-map", i, value, s->msi[i]);
		if (!verify)
			s->msi[i] = value;
	}
	value = apple_pcie_port_readl(port, PORT_MSICFG);
	if (verify && value != s->msicfg)
		return apple_radio_snapshot_error(s, "msi-config", 0, value, s->msicfg);
	if (!verify)
		s->msicfg = value;
	value = apple_pcie_port_readl(port, PORT_MSIBASE);
	if (verify && value != s->msibase)
		return apple_radio_snapshot_error(s, "msi-base", 0, value, s->msibase);
	if (!verify)
		s->msibase = value;
	value = apple_pcie_port_readl(port, pcie->hw->port_msiaddr);
	if (verify && value != s->msiaddr)
		return apple_radio_snapshot_error(s, "msi-address", 0, value, s->msiaddr);
	if (!verify)
		s->msiaddr = value;
	value = apple_pcie_port_readl(port, pcie->hw->port_msiaddr_hi);
	if (verify && value != s->msiaddr_hi)
		return apple_radio_snapshot_error(s, "msi-address-high", 0, value, s->msiaddr_hi);
	if (!verify)
		s->msiaddr_hi = value;
	if (!(s->msicfg & PORT_MSICFG_EN))
		return apple_radio_snapshot_error(s, "msi-enabled", 0, s->msicfg, PORT_MSICFG_EN);
	if (verify) {
		if (!bitmap_equal(s->vectors, pcie->bitmap, pcie->nvecs) ||
		    !bitmap_equal(s->sids, port->sid_map, port->sid_map_sz))
			return -EUCLEAN;
	} else {
		bitmap_copy(s->vectors, pcie->bitmap, pcie->nvecs);
		bitmap_copy(s->sids, port->sid_map, port->sid_map_sz);
	}
	return 0;
}

static int apple_radio_commands_held(struct apple_radio_sleep *s)
{
	u16 value;
	int i;

	for (i = 0; i < ARRAY_SIZE(s->fn); i++)
		if (!s->fn[i].held || pci_read_config_word(s->fn[i].pdev, PCI_COMMAND, &value) ||
		    value != (s->fn[i].command & ~PCI_COMMAND_MASTER))
			return -EUCLEAN;
	return 0;
}

static int apple_radio_begin(struct apple_radio_sleep *s, u64 generation)
{
	int i, ret;

	if (!s->bound || !generation || READ_ONCE(apple_radio_active) ||
	    s->phase != APPLE_RADIO_IDLE || irqs_disabled() || in_interrupt() ||
	    pm_suspend_target_state != PM_SUSPEND_MEM)
		return -EBUSY;
	/* QUIESCE is the only admitted BT path; do not silently change wake policy. */
	if (device_may_wakeup(&s->fn[1].pdev->dev))
		return -EBUSY;
	if (!s->recovery_ready)
		return -EOPNOTSUPP;
	dev_info(s->pcie->dev, "radio sleep begin: link=%#x\n",
		 apple_pcie_port_readl(s->port, PORT_LINKSTS));
	ret = apple_dart_sleep_begin(&s->dart, generation);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(s->fn); i++) {
		s->fn[i].attempted = s->fn[i].held = false;
		s->fn[i].suspended = s->fn[i].resumed = 0;
		s->fn[i].finish = NULL;
		s->fn[i].error = 0;
	}
	s->snapshot = s->host_suspended = false;
	s->config_ready = false;
	s->owner = current;
	s->generation = generation;
	s->phase = APPLE_RADIO_OPEN;
	/* Publish fully initialized endpoint and transaction records. */
	smp_store_release(&apple_radio_active, s);
	return 0;
}

static int apple_radio_seal(struct apple_radio_sleep *s, u64 generation)
{
	int i, ret = apple_radio_valid(s, generation);

	if (ret)
		return ret;
	if (s->phase != APPLE_RADIO_OPEN || irqs_disabled() || !s->host_suspended)
		return -EPERM;
	for (i = 0; i < ARRAY_SIZE(s->fn); i++) {
		/* Pair with the endpoint suspend receipt publication. */
		if (smp_load_acquire(&s->fn[i].suspended) != generation)
			return -EBUSY;
	}
	mutex_lock(&s->pcie->lock);
	if (s->pcie->radio_writers || s->pcie->radio_closed) {
		mutex_unlock(&s->pcie->lock);
		return -EBUSY;
	}
	s->pcie->radio_closed = true;
	mutex_unlock(&s->pcie->lock);
	s->phase = APPLE_RADIO_SEALING;
	/* Establish the host baseline before making even reversible BME changes. */
	ret = apple_radio_snapshot(s, false);
	if (ret) {
		dev_err(s->pcie->dev, "radio sleep host snapshot failed: %d\n", ret);
		return ret;
	}
	s->snapshot = true;
	for (i = 0; i < ARRAY_SIZE(s->fn); i++) {
		ret = apple_radio_bme_hold(&s->fn[i]);
		if (ret)
			return ret;
		if (!pci_wait_for_pending_transaction(s->fn[i].pdev))
			return -EBUSY;
	}
	ret = apple_dart_sleep_seal(&s->dart);
	if (ret) {
		dev_err(s->pcie->dev, "radio sleep DART seal failed: %d\n", ret);
		return ret;
	}
	ret = apple_radio_snapshot(s, true) ?: apple_radio_commands_held(s);
	if (ret)
		return ret;
	s->phase = APPLE_RADIO_SEALED;
	return 0;
}

static int apple_radio_port_restore(struct apple_radio_sleep *s)
{
	static const struct {
		u32 offset, value;
	} defaults[] = {
		{ 0x88, 0x110 }, { 0x100, ~0U },       { 0x148, ~0U },	    { 0x210, ~0U },
		{ 0x80, 0 },	 { 0x84, 0 },	       { 0x104, ~0U },	    { 0x124, 0x100 },
		{ 0x16c, 0 },	 { 0x13c, 0x10 },      { 0x800, 0x100100 }, { 0x808, 0x1000ff },
		{ 0x82c, 0 },	 { 0x130, 0x3000000 }, { 0x140, 0x10 },	    { 0x144, 0x253770 },
		{ 0x21c, 0 },	 { 0x834, 0 },	       { 0x83c, 0 },
	};
	struct apple_pcie_port *port = s->port;
	void __iomem *dbi = s->pcie->cfg->win;
	u32 value;
	int ret, speed = of_pci_get_max_link_speed(port->np);

	for (unsigned int i = 0; i < ARRAY_SIZE(defaults); i++)
		apple_pcie_port_writel(port, defaults[i].value, defaults[i].offset);
	apple_tunable_apply(port->base, s->tables[0]);
	apple_pcie_port_rmw_set(port, PORT_APPCLK_EN, PORT_APPCLK);
	writel(readl(port->phy) & ~(PHY_LANE_CFG_REFCLK0REQ | PHY_LANE_CFG_REFCLK1REQ), port->phy);
	writel(readl(port->phy) | PHY_LANE_CFG_REFCLK0REQ, port->phy);
	ret = readl_poll_timeout(port->phy, value, value & PHY_LANE_CFG_REFCLK0ACK, 100,
				 RADIO_PORT_TIMEOUT_US);
	if (ret)
		return ret;
	writel(readl(port->phy) | PHY_LANE_CFG_REFCLK1REQ, port->phy);
	ret = readl_poll_timeout(port->phy, value, value & PHY_LANE_CFG_REFCLK1ACK, 100,
				 RADIO_PORT_TIMEOUT_US);
	if (ret)
		return ret;
	writel(readl(port->phy) & ~RADIO_PHY_RESET, port->phy);
	writel(readl(port->phy) | BIT(9), port->phy);
	writel(readl(port->phy) | BIT(10), port->phy);
	apple_pcie_port_rmw_set(port, PORT_PERST_OFF, s->pcie->hw->port_perst);
	ret = readl_poll_timeout(port->base + PORT_STATUS, value, value & PORT_STATUS_READY, 100,
				 250000);
	if (ret)
		return ret;
	/* Keep the port clock policy that was qualified while the radio was live. */
	value = apple_pcie_port_readl(port, PORT_APPCLK);
	apple_pcie_port_writel(port, (value & ~PORT_APPCLK_CGDIS) | (s->appclk & PORT_APPCLK_CGDIS),
			       PORT_APPCLK);
	ret = readl_poll_timeout(port->base + PORT_LINKSTS, value, !(value & PORT_LINKSTS_BUSY),
				 100, 250000);
	if (ret)
		return ret;
	writel(readl(dbi + RADIO_DBI_RO_WR) | BIT(0), dbi + RADIO_DBI_RO_WR);
	for (unsigned int i = 1; i < ARRAY_SIZE(s->tables); i++)
		apple_tunable_apply(dbi, s->tables[i]);
	if (speed > 0 && speed <= 4) {
		value = readl(dbi + 0x7c);
		writel((value & ~PCI_EXP_LNKCAP_SLS) | speed, dbi + 0x7c);
		value = readl(dbi + 0x9c);
		writel((value & ~GENMASK(6, 1)) | FIELD_PREP(GENMASK(6, 1), (1 << speed) - 1),
		       dbi + 0x9c);
		writew((readw(dbi + 0xa0) & ~PCI_EXP_LNKCTL2_TLS) | speed, dbi + 0xa0);
		writel(readl(dbi + RADIO_DBI_WIDTH_SPEED) | BIT(17), dbi + RADIO_DBI_WIDTH_SPEED);
	}
	value = readl(dbi + RADIO_DBI_LINK_CONTROL);
	writel((value & ~GENMASK(21, 16)) | BIT(16), dbi + RADIO_DBI_LINK_CONTROL);
	value = readl(dbi + RADIO_DBI_WIDTH_SPEED);
	writel((value & ~GENMASK(12, 8)) | BIT(8), dbi + RADIO_DBI_WIDTH_SPEED);
	value = readl(dbi + 0x7c);
	writel((value & ~PCI_EXP_LNKCAP_MLW) | FIELD_PREP(PCI_EXP_LNKCAP_MLW, 1), dbi + 0x7c);
	writel(readl(dbi + RADIO_DBI_RO_WR) & ~BIT(0), dbi + RADIO_DBI_RO_WR);
	return 0;
}

static int apple_radio_routes_restore(struct apple_radio_sleep *s)
{
	struct apple_pcie_port *port = s->port;
	const struct hw_info *hw = s->pcie->hw;

	apple_pcie_port_writel(port, s->msicfg & ~PORT_MSICFG_EN, PORT_MSICFG);
	for (unsigned int i = 0; i < ARRAY_SIZE(s->rid); i++) {
		apple_pcie_port_writel(port, s->rid[i], port_rid2sid_offset(port, i));
		if (apple_pcie_port_readl(port, port_rid2sid_offset(port, i)) != s->rid[i])
			return -EIO;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(s->msi); i++) {
		apple_pcie_port_writel(port, s->msi[i], hw->port_msimap + 4 * i);
		if (apple_pcie_port_readl(port, hw->port_msimap + 4 * i) != s->msi[i])
			return -EIO;
	}
	apple_pcie_port_writel(port, s->msibase, PORT_MSIBASE);
	apple_pcie_port_writel(port, s->msiaddr, hw->port_msiaddr);
	apple_pcie_port_writel(port, s->msiaddr_hi, hw->port_msiaddr_hi);
	apple_pcie_port_writel(port, s->msicfg, PORT_MSICFG);
	if (apple_pcie_port_readl(port, PORT_MSIBASE) != s->msibase ||
	    apple_pcie_port_readl(port, hw->port_msiaddr) != s->msiaddr ||
	    apple_pcie_port_readl(port, hw->port_msiaddr_hi) != s->msiaddr_hi ||
	    apple_pcie_port_readl(port, PORT_MSICFG) != s->msicfg)
		return -EIO;
	return 0;
}

static int apple_radio_recover(struct apple_radio_sleep *s)
{
	static const u32 identities[] = { 0x443414e4, 0x5f7214e4 };
	struct apple_pcie_port *port = s->port;
	u32 value;
	u16 command;
	int ret;

	/* These native GPIO operations restore mux and direction as well as level. */
	ret = gpiod_direction_output(s->perst, 1) ?: gpiod_direction_input(s->clkreq);
	if (ret)
		return ret;
	ret = apple_radio_port_restore(s) ?: apple_radio_routes_restore(s);
	if (ret)
		return ret;
	ret = apple_dart_sleep_power_restore(&s->dart);
	if (ret)
		return ret;
	/* Only this transaction's task may access config until the link is checked. */
	s->config_ready = true;
	pci_restore_state(s->root);
	/* Keep the rail untouched and allow reference clocks to settle. */
	msleep(100);
	ret = gpiod_set_value(s->perst, 0);
	if (ret)
		return ret;
	msleep(100);
	apple_pcie_port_writel(port, PORT_LTSSMCTL_START, PORT_LTSSMCTL);
	ret = readl_poll_timeout(port->base + PORT_LINKSTS, value, value & PORT_LINKSTS_UP, 1000,
				 500000);
	if (ret)
		return ret;
	msleep(100);
	for (unsigned int i = 0; i < ARRAY_SIZE(s->fn); i++) {
		if (pci_read_config_dword(s->fn[i].pdev, PCI_VENDOR_ID, &value) ||
		    value != identities[i])
			return -ENODEV;
		if (pci_read_config_word(s->fn[i].pdev, PCI_COMMAND, &command) ||
		    (command & PCI_COMMAND_MASTER))
			return -EUCLEAN;
	}
	apple_pcie_port_writel(port, ~0U, PORT_INTSTAT);
	apple_pcie_port_writel(port, s->intmask, PORT_INTMSK);
	/* Child noirq callbacks now restore config, including BME, before firmware. */
	return 0;
}

static int apple_radio_abort(struct apple_radio_sleep *s, u64 generation)
{
	int i, ret = apple_radio_valid(s, generation);

	if (ret)
		return ret;
	if (irqs_disabled() || s->phase == APPLE_RADIO_VERIFIED ||
	    s->phase == APPLE_RADIO_STOPPING || s->phase == APPLE_RADIO_STOPPED ||
	    s->phase == APPLE_RADIO_RECOVERING)
		return -EPERM;
	if (s->phase == APPLE_RADIO_ABORTED)
		return 0;
	if (s->snapshot && apple_radio_snapshot(s, true))
		goto failed;
	if (apple_dart_sleep_abort(&s->dart))
		goto failed;
	for (i = ARRAY_SIZE(s->fn) - 1; i >= 0; i--)
		if (apple_radio_bme_restore(&s->fn[i]))
			goto failed;
	s->phase = APPLE_RADIO_ABORTED;
	/* Publish restored/verified state before admitting host writers. */
	smp_store_release(&s->pcie->radio_closed, false);
	return 0;
failed:
	s->phase = APPLE_RADIO_FAILED;
	return -EUCLEAN;
}

static int apple_radio_finish(struct apple_radio_sleep *s, u64 generation)
{
	int i, ret = apple_radio_valid(s, generation);

	if (ret)
		return ret;
	if (s->phase != APPLE_RADIO_VERIFIED && s->phase != APPLE_RADIO_ABORTED)
		return -EPERM;
	for (i = 0; i < ARRAY_SIZE(s->fn); i++) {
		/* Acquire the receipt before reading its published error result. */
		u64 resumed = smp_load_acquire(&s->fn[i].resumed);

		if (READ_ONCE(s->fn[i].error) ||
		    (s->fn[i].suspended == generation && resumed != generation))
			return -EUCLEAN;
	}
	ret = apple_dart_sleep_finish(&s->dart);
	if (ret)
		return ret;
	/* End ownership before the endpoint finish hooks reopen normal work. */
	smp_store_release(&apple_radio_active, NULL);
	for (i = 0; i < ARRAY_SIZE(s->fn); i++)
		if (s->fn[i].finish)
			s->fn[i].finish(&s->fn[i].pdev->dev);
	s->phase = APPLE_RADIO_IDLE;
	s->owner = NULL;
	return 0;
}

static int apple_radio_recovery_gpio(struct apple_radio_sleep *s)
{
	static const char *const names[] = { "reset-gpios", "clkreq-gpios" };
	static const unsigned int pins[] = { 163, 160 };
	struct of_phandle_args spec;
	struct resource resource;
	int ret;

	/* Claim only the two board-defined pins; leave the SMC-owned rail alone. */
	for (unsigned int i = 0; i < ARRAY_SIZE(names); i++) {
		ret = of_parse_phandle_with_fixed_args(s->port->np, names[i], 2, 0, &spec);
		if (ret)
			return ret;
		ret = of_address_to_resource(spec.np, 0, &resource);
		if (!ret && (!of_device_is_compatible(spec.np, "apple,t8132-pinctrl") ||
			     resource.start != 0x39a000000ULL || spec.args[0] != pins[i] ||
			     spec.args[1] != 1))
			ret = -EINVAL;
		of_node_put(spec.np);
		if (ret)
			return ret;
	}
	s->perst = s->port->perst_gpio;
	if (!s->perst)
		s->perst = devm_fwnode_gpiod_get(s->pcie->dev, of_fwnode_handle(s->port->np),
						 "reset", GPIOD_ASIS, "radio PERST#");
	if (IS_ERR(s->perst))
		return PTR_ERR(s->perst);
	s->clkreq = devm_fwnode_gpiod_get(s->pcie->dev, of_fwnode_handle(s->port->np), "clkreq",
					  GPIOD_ASIS, "radio CLKREQ#");
	if (IS_ERR(s->clkreq))
		return PTR_ERR(s->clkreq);
	if (gpiod_cansleep(s->perst))
		return -EOPNOTSUPP;
	return 0;
}

/* The retained hierarchy cannot be removed while these references are held. */
static void apple_radio_put_devices(void *data)
{
	struct apple_radio_sleep *s = data;

	if (READ_ONCE(apple_radio_active) == s)
		apple_radio_terminal();
	for (unsigned int i = 0; i < ARRAY_SIZE(s->fn); i++)
		pci_dev_put(s->fn[i].pdev);
	pci_dev_put(s->root);
}

static int apple_radio_bind(struct platform_device *pdev, struct apple_radio_sleep *s)
{
	static const u16 ids[] = { 0x4434, 0x5f72 };
	static const char *const names[] = {
		"apple,tunable-port",
		"apple,tunable-rc",
		"apple,tunable-rc-gen3",
		"apple,tunable-rc-gen4",
	};
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(s->pcie);
	struct of_phandle_args map[2] = {};
	struct resource *res;
	int ret;

	ret = devm_add_action_or_reset(&pdev->dev, apple_radio_put_devices, s);
	if (ret)
		return ret;
	s->root = pci_get_slot(bridge->bus, PCI_DEVFN(0, 0));
	if (!s->root || !s->root->subordinate)
		return -ENODEV;
	for (unsigned int i = 0; i < ARRAY_SIZE(s->fn); i++) {
		struct pci_dev *ep = pci_get_slot(s->root->subordinate, PCI_DEVFN(0, i));

		s->fn[i].pdev = ep;
		if (!ep || ep->vendor != 0x14e4 || ep->device != ids[i])
			return -ENODEV;
	}
	ret = apple_radio_recovery_gpio(s);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < ARRAY_SIZE(names); i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, i ? "config" : "port0");
		if (!res)
			return -ENOENT;
		s->tables[i] = devm_apple_tunable_parse(&pdev->dev, s->port->np, names[i], res);
		if (IS_ERR(s->tables[i]))
			return PTR_ERR(s->tables[i]);
	}
	ret = apple_dart_sleep_bind(&pdev->dev, &s->fn[0].pdev->dev, &s->dart);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < ARRAY_SIZE(map); i++) {
		ret = of_map_iommu_id(pdev->dev.of_node, 0x100 + i, &map[i]);
		if (ret)
			break;
		if (map[i].np != s->dart.provider->of_node || map[i].args_count != 1 ||
		    map[i].args[0] != 1 + i) {
			ret = -EINVAL;
			break;
		}
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(map); i++)
		of_node_put(map[i].np);
	if (ret)
		return ret;
	s->bound = s->recovery_ready = true;
	return 0;
}

static void apple_radio_register(struct platform_device *pdev, struct apple_pcie *pcie)
{
	struct apple_radio_sleep *s;
	int ret;

	if (!pcie->managed_sids)
		return;
	if (!of_machine_is_compatible("apple,j713") ||
	    !of_device_is_compatible(pcie->dev->of_node, "apple,t8132-pcie") ||
	    pcie->managed_sids != (BIT(1) | BIT(2)) || pcie->piodma_supplier ||
	    !list_is_singular(&pcie->ports) || pcie->nvecs != 32 || !pcie->hw->port_msiaddr_hi ||
	    !pcie->hw->port_msimap)
		return;
	s = devm_kzalloc(pcie->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return;
	s->pcie = pcie;
	s->port = list_first_entry(&pcie->ports, struct apple_pcie_port, entry);
	if (s->port->idx || !s->port->phy || s->port->sid_map_sz != 16)
		return;
	ret = apple_radio_bind(pdev, s);
	if (ret) {
		dev_warn(pcie->dev, "native radio sleep unavailable: %d\n", ret);
		return;
	}
	/* All host callbacks must execute in the task that owns the transaction. */
	device_disable_async_suspend(pcie->dev);
	pcie->radio = s;
	dev_info(pcie->dev, "native radio sleep recovery registered\n");
}

static int apple_radio_prepare(struct device *dev)
{
	static const char *const drivers[] = { "brcmfmac", "hci_bcm4377" };
	struct apple_pcie *pcie = apple_pcie_lookup(dev);
	struct apple_radio_sleep *s = pcie->radio;

	if (!pcie->managed_sids || !pm_suspend_via_firmware())
		return 0;
	if (!s || !s->bound || s->generation == U64_MAX)
		return -EBUSY;
	for (unsigned int i = 0; i < ARRAY_SIZE(s->fn); i++) {
		struct device *ep = &s->fn[i].pdev->dev;

		if (!ep->driver || strcmp(ep->driver->name, drivers[i]))
			return -ENODEV;
	}
	return apple_radio_begin(s, s->generation + 1);
}

static int apple_radio_suspend_noirq(struct apple_pcie *pcie)
{
	struct apple_radio_sleep *s = pcie->radio;
	int ret;

	if (!pm_suspend_via_firmware())
		return 0;
	if (!s || READ_ONCE(apple_radio_active) != s || s->phase != APPLE_RADIO_OPEN)
		return -EBUSY;
	apple_pcie_port_disable_irq(s->port);
	s->host_suspended = true;
	ret = apple_radio_seal(s, s->generation);
	if (!ret &&
	    (!s->root->state_saved || !s->fn[0].pdev->state_saved || !s->fn[1].pdev->state_saved))
		ret = -EUCLEAN;
	if (!ret)
		ret = apple_dart_sleep_power_off(&s->dart);
	if (ret) {
		dev_err(pcie->dev, "native radio suspend refused: %d\n", ret);
		if (apple_radio_abort(s, s->generation))
			apple_radio_terminal();
		apple_pcie_port_enable_irq(s->port);
		return ret;
	}
	/* The final SoC commit owns PERST and the port clocks. A refusal leaves them on. */
	s->phase = APPLE_RADIO_STOPPED;
	device_set_awake_path(pcie->dev);
	dev_info(pcie->dev, "native radio DMA fenced, DART off\n");
	return 0;
}

static int apple_radio_resume_noirq(struct apple_pcie *pcie)
{
	struct apple_radio_sleep *s = pcie->radio;
	int ret;

	if (!s || READ_ONCE(apple_radio_active) != s)
		return 0;
	if (s->phase == APPLE_RADIO_OPEN || s->phase == APPLE_RADIO_ABORTED) {
		apple_pcie_port_enable_irq(s->port);
		return 0;
	}
	if (s->phase != APPLE_RADIO_STOPPED || current != s->owner)
		apple_radio_terminal();
	s->phase = APPLE_RADIO_RECOVERING;
	if (pm_resume_via_firmware()) {
		ret = apple_radio_recover(s);
	} else {
		/* pm_test or a refused commit: the link stayed up, only the DART was stopped. */
		ret = apple_dart_sleep_power_restore(&s->dart);
		s->config_ready = !ret;
		for (unsigned int i = 0; !ret && i < ARRAY_SIZE(s->fn); i++)
			ret = apple_radio_bme_restore(&s->fn[i]);
	}
	if (ret) {
		dev_emerg(pcie->dev, "native radio recovery failed: %d\n", ret);
		s->config_ready = false;
		/* Config readers must see the closed gate before the failure state. */
		smp_store_release(&s->phase, APPLE_RADIO_FAILED);
		apple_radio_terminal();
	}
	/* Publish the complete recovery before config access resumes. */
	smp_store_release(&s->phase, APPLE_RADIO_VERIFIED);
	/* Host writers must observe that same restored state. */
	smp_store_release(&pcie->radio_closed, false);
	apple_pcie_port_enable_irq(s->port);
	if (pm_resume_via_firmware())
		apple_sleep_radio_port_up();
	dev_info(pcie->dev, "native radio %s recovery complete\n",
		 pm_resume_via_firmware() ? "power-loss" : "retained");
	return 0;
}

static void apple_radio_complete(struct device *dev)
{
	struct apple_radio_sleep *s = apple_pcie_lookup(dev)->radio;
	int ret;

	if (!s || READ_ONCE(apple_radio_active) != s)
		return;
	if (s->phase == APPLE_RADIO_OPEN || s->phase == APPLE_RADIO_SEALING) {
		if (apple_radio_abort(s, s->generation))
			apple_radio_terminal();
	}
	ret = apple_radio_finish(s, s->generation);
	if (ret) {
		dev_emerg(dev, "native radio endpoints failed to resume: %d\n", ret);
		apple_radio_terminal();
	}
	dev_info(dev, "native radio transaction complete\n");
}
