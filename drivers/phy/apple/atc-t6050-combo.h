/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* One Type-C owner reuses the established DP and USB backend sequences.
 * All calls below hold dp->lock; only PHY power_off authorizes PIPE teardown
 * while the controller reset is deasserted. Lane changes require reset/off.
 */
static u32 combo_read(void *ctx, unsigned int bank, unsigned int offset)
{
	struct t6050_dp *dp = ctx;
	return readl(dp->banks[bank] + offset);
}
static void combo_write(void *ctx, unsigned int bank, unsigned int offset, u32 value)
{
	struct t6050_dp *dp = ctx;
	writel(value, dp->banks[bank] + offset);
}
static int combo_poll(void *ctx, unsigned int bank, unsigned int offset,
		      unsigned int mask, unsigned int expected, unsigned int usec)
{
	struct t6050_dp *dp = ctx;
	u32 value;
	return readl_poll_timeout(dp->banks[bank] + offset, value,
		(value & mask) == expected,
		(bank == 44 || bank == 35 || usec <= 1000) ? 1 : 1000, usec);
}
static void combo_delay(void *ctx, unsigned int usec) { udelay(usec); }
static void combo_pause(void *ctx, unsigned int nsec) { ndelay(nsec); }
static void combo_sleep(void *ctx, unsigned int msec) { msleep(msec); }
static int combo_wrapper(void *ctx, unsigned int active)
{
	struct t6050_dp *dp = ctx;
	return atc_domain_set(&dp->domain[2], active);
}
static int combo_firmware(void *ctx, unsigned int active)
{
	return atc_firmware(ctx, active);
}
static int combo_tunable(void *ctx, unsigned int bank, const char *name)
{
	static const struct { u8 bank, index; const char *name; } common[] = {
		{41, 0, "tunable_ATC0AXI2AF"}, {40, 1, "tunable_ATC_FABRIC"},
		{43, 2, "tunable_ATC_COMMON_CFG"}, {42, 3, "tunable_ATC0AXI2AF_LIOA"},
		{20, 4, "tunable_LN0_RX_CFG_TX_OF_RXCLK"}, {29, 5, "tunable_LN1_RX_CFG_TX_OF_RXCLK"},
		{18, 6, "tunable_LN0_RX_EQ_DFLT"}, {27, 7, "tunable_LN1_RX_EQ_DFLT"},
		{8, 8, "tunable_AUSPLL_CORE"}, {5, 9, "tunable_AUS40CMN_SHM"},
		{10, 10, "tunable_CIO4PLL_CORE"},
	};
	struct t6050_dp *dp = ctx;

	if (bank == 0 && !strcmp(name, "tunable_USB2PHY_DFLT")) {
		apple_tunable_apply(dp->banks[0], dp->usb2_default);
		return 0;
	}
	if (bank == 0 && !strcmp(name, "tunable_USB2PHY_HOST")) {
		apple_tunable_apply(dp->banks[0], dp->usb2_host);
		return 0;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(common); i++)
		if (bank == common[i].bank && !strcmp(name, common[i].name)) {
			apple_tunable_apply(dp->banks[bank], dp->tunables[common[i].index]);
			return 0;
		}
	return -EINVAL;
}

static int combo_quiesce(void *ctx)
{
	struct t6050_dp *dp = ctx;
	int ret = 0;

	if (dp->usb_prepared) {
		ret = APPLE_T6050_USB_RUN(&dp->usb_platform, apple_t6050_usb3_shutdown);
		if (!ret)
			dp->usb_prepared = false;
	} else if (dp->enabled) {
		/* Multifunction has a distinct native shutdown; never run the
		 * USB-only analog sequence over live DisplayPort lane state. */
		ret = atc_stop(dp);
	}
	return ret;
}

static int combo_stop(struct t6050_dp *dp)
{
	int ret;

	lockdep_assert_held(&dp->lock);
	if (dp->faulted)
		return -EIO;
	if (dp->controller_active || dp->pipe_active)
		return -EBUSY;
	ret = combo_quiesce(dp);
	if (!ret && dp->usb2_active) {
		ret = apple_t6050_usb2_stop(&dp->usb_platform);
		if (!ret)
			dp->usb2_active = false;
	}
	if (!ret)
		ret = atc_domain_set(&dp->domain[1], false);
	if (!ret)
		ret = atc_domain_set(&dp->domain[0], false);
	if (ret) {
		dp->faulted = true;
		return ret < 0 ? ret : -EIO;
	}
	dp->combo_mode = COMBO_SAFE;
	return 0;
}

static int combo_start(struct t6050_dp *dp, enum combo_mode mode)
{
	int ret;

	lockdep_assert_held(&dp->lock);
	if (dp->faulted)
		return -EIO;
	if (dp->orientation == TYPEC_ORIENTATION_NONE)
		return -ENOLINK;
	ret = atc_domain_set(&dp->domain[0], true);
	if (!ret)
		ret = atc_domain_set(&dp->domain[1], true);
	if (!ret)
		ret = APPLE_T6050_USB_RUN(&dp->usb_platform, apple_t6050_usb2_host);
	if (!ret)
		dp->usb2_active = true;
	if (!ret && mode == COMBO_USB) {
		ret = dp->reverse ?
			APPLE_T6050_USB_RUN(&dp->usb_platform, apple_t6050_usb3_init_reverse) :
			APPLE_T6050_USB_RUN(&dp->usb_platform, apple_t6050_usb3_init_normal);
		if (!ret)
			dp->usb_prepared = true;
	} else if (!ret) {
		dp->multifunction = mode == COMBO_DP2;
		ret = atc_start(dp);
	}
	if (ret) {
		/* A partial shared transition has no cold-start ownership proof. */
		dp->faulted = true;
		return ret < 0 ? ret : -EIO;
	}
	dp->combo_mode = mode;
	return 0;
}

static int combo_orientation(struct t6050_dp *dp, enum typec_orientation orientation)
{
	int ret;

	guard(mutex)(&dp->lock);
	if (dp->faulted)
		return -EIO;
	if (orientation != TYPEC_ORIENTATION_NONE && orientation != TYPEC_ORIENTATION_NORMAL &&
	    orientation != TYPEC_ORIENTATION_REVERSE)
		return -EINVAL;
	if (dp->orientation == orientation)
		return 0;
	ret = combo_stop(dp);
	if (!ret) {
		dp->orientation = orientation;
		dp->reverse = orientation == TYPEC_ORIENTATION_REVERSE;
	}
	return ret;
}

static int combo_mux(struct t6050_dp *dp, struct typec_mux_state *state)
{
	enum combo_mode mode;
	int ret;

	if (state->mode == TYPEC_STATE_SAFE)
		mode = COMBO_SAFE;
	else if (state->mode == TYPEC_STATE_USB && !state->alt)
		mode = COMBO_USB;
	else if (state->alt && state->alt->svid == USB_TYPEC_DP_SID) {
		switch (state->mode) {
		case TYPEC_DP_STATE_C:
		case TYPEC_DP_STATE_E: mode = COMBO_DP4; break;
		case TYPEC_DP_STATE_D: mode = COMBO_DP2; break;
		default: return -EOPNOTSUPP;
		}
	} else {
		return -EOPNOTSUPP;
	}
	guard(mutex)(&dp->lock);
	if (dp->faulted)
		return -EIO;
	if (mode == dp->combo_mode &&
	    (mode == COMBO_SAFE || dp->usb_prepared || dp->enabled))
		return 0;
	if (mode != COMBO_SAFE && dp->orientation == TYPEC_ORIENTATION_NONE)
		return -ENOLINK;
	ret = combo_stop(dp);
	if (ret || mode == COMBO_SAFE)
		return ret;
	return combo_start(dp, mode);
}

static int combo_usb2_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct t6050_dp *dp = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(&dp->lock);
	if (dp->faulted)
		return -EIO;
	if (mode != PHY_MODE_USB_HOST)
		return -EOPNOTSUPP;
	if (!dp->usb_prepared && !dp->enabled)
		return -ENOLINK;
	if (dp->usb2_active)
		return 0;
	ret = APPLE_T6050_USB_RUN(&dp->usb_platform, apple_t6050_usb2_host);
	if (ret)
		dp->faulted = true;
	else
		dp->usb2_active = true;
	return ret;
}

static int combo_usb2_reset(struct phy *phy)
{
	struct t6050_dp *dp = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(&dp->lock);
	if (dp->faulted || !dp->usb2_active)
		return -ENOLINK;
	ret = apple_t6050_usb2_port_reset(&dp->usb_io, dp, combo_sleep);
	if (ret)
		dp->faulted = true;
	return ret ? -EIO : 0;
}

static int combo_usb3_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct t6050_dp *dp = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(&dp->lock);
	if (dp->faulted)
		return -EIO;
	if (mode != PHY_MODE_USB_HOST)
		return -EOPNOTSUPP;
	if (!dp->controller_active || !dp->usb2_active)
		return -ENOLINK;
	/* Four DP lanes leave DWC3 on its established dummy PIPE; USB2 still
	 * uses its independent D+/D- PHY. Do not switch USB3 onto DP lanes. */
	if (dp->combo_mode == COMBO_DP4 && dp->enabled)
		return 0;
	if (!dp->usb_prepared && !(dp->combo_mode == COMBO_DP2 && dp->enabled))
		return -ENOLINK;
	if (dp->pipe_active)
		return (readl(dp->banks[44] + 0xc) & 0x73) == 0x10 ? 0 : -EIO;
	apple_tunable_apply(dp->banks[44], dp->pipe_default);
	ret = apple_t6050_usb3_pipe_switch(&dp->usb_io);
	if (ret)
		dp->faulted = true;
	else
		dp->pipe_active = true;
	return ret ? -EIO : 0;
}

static int combo_usb3_poweroff(struct phy *phy)
{
	struct t6050_dp *dp = phy_get_drvdata(phy);
	int ret = 0;

	guard(mutex)(&dp->lock);
	if (dp->faulted)
		return -EIO;
	/* DWC3 calls power_off only after host/gadget shutdown and SUSPHY.
	 * PIPE must select its dummy producer before the controller is reset. */
	if (dp->pipe_active) {
		ret = apple_t6050_usb3_pipe_stop(&dp->usb_io, combo_quiesce, dp);
		if (!ret)
			dp->pipe_active = false;
	}
	if (!ret && dp->usb2_active) {
		ret = apple_t6050_usb2_stop(&dp->usb_platform);
		if (!ret)
			dp->usb2_active = false;
	}
	if (ret)
		dp->faulted = true;
	return ret ? -EIO : 0;
}
static const struct phy_ops combo_usb2_ops = {
	.set_mode = combo_usb2_mode, .reset = combo_usb2_reset, .owner = THIS_MODULE,
};
static const struct phy_ops combo_usb3_ops = {
	.set_mode = combo_usb3_mode, .power_off = combo_usb3_poweroff, .owner = THIS_MODULE,
};

static int combo_reset_assert(struct reset_controller_dev *reset, unsigned long id)
{
	struct t6050_dp *dp = container_of(reset, struct t6050_dp, reset);
	int ret;

	guard(mutex)(&dp->lock);
	if (id || dp->pipe_active)
		return -EBUSY;
	if (dp->faulted)
		return -EIO;
	apple_t6050_usb_mask(&dp->usb_io, 44, 0x1c, 1, 0);
	apple_t6050_usb_mask(&dp->usb_io, 44, 0x1c, 0x10, 0x10);
	ret = apple_t6050_usb3_pipe_cold_dummy(&dp->usb_io);
	if (ret)
		dp->faulted = true;
	else
		dp->controller_active = false;
	return ret ? -EIO : 0;
}
static int combo_reset_deassert(struct reset_controller_dev *reset, unsigned long id)
{
	struct t6050_dp *dp = container_of(reset, struct t6050_dp, reset);

	guard(mutex)(&dp->lock);
	if (id)
		return -EINVAL;
	if (dp->faulted || !dp->usb2_active)
		return -ENOLINK;
	apple_t6050_usb_mask(&dp->usb_io, 44, 0x1c, 0x10, 0);
	apple_t6050_usb_mask(&dp->usb_io, 44, 0x1c, 1, 1);
	dp->controller_active = true;
	return 0;
}
static const struct reset_control_ops combo_reset_ops = {
	.assert = combo_reset_assert, .deassert = combo_reset_deassert,
};

static struct phy *combo_xlate(struct device *dev, const struct of_phandle_args *args)
{
	struct t6050_dp *dp = dev_get_drvdata(dev);
	if (args->args_count != 1)
		return ERR_PTR(-EINVAL);
	switch (args->args[0]) {
	case PHY_TYPE_USB2: return dp->combo_phy[0];
	case PHY_TYPE_USB3: return dp->combo_phy[1];
	case PHY_TYPE_DP: return dp->combo_phy[2];
	default: return ERR_PTR(-EINVAL);
	}
}

static int combo_setup(struct platform_device *pdev, struct t6050_dp *dp)
{
	static const struct { u8 bank; u32 minimum; const char *name; } extra[] = {
		{0, 0x20, "usb2phy"}, {1, 0x24, "usb2-digital"}, {44, 0x48, "pipehandler"},
	};
	struct device *dev = dp->dev;
	int ret;

	for (unsigned int i = 0; i < ARRAY_SIZE(extra); i++) {
		struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, extra[i].name);
		if (!res || resource_size(res) < extra[i].minimum)
			return -EINVAL;
		dp->banks[extra[i].bank] = devm_ioremap_resource(dev, res);
		if (IS_ERR(dp->banks[extra[i].bank]))
			return PTR_ERR(dp->banks[extra[i].bank]);
		dp->resources[extra[i].bank] = *res;
	}
	dp->usb2_default = devm_apple_tunable_parse(dev, dev->of_node,
		"apple,tunable-usb2-default", &dp->resources[0]);
	if (IS_ERR(dp->usb2_default))
		return PTR_ERR(dp->usb2_default);
	dp->usb2_host = devm_apple_tunable_parse(dev, dev->of_node,
		"apple,tunable-usb2-host", &dp->resources[0]);
	if (IS_ERR(dp->usb2_host))
		return PTR_ERR(dp->usb2_host);
	dp->pipe_default = devm_apple_tunable_parse(dev, dev->of_node,
		"apple,tunable-pipe-default", &dp->resources[44]);
	if (IS_ERR(dp->pipe_default))
		return PTR_ERR(dp->pipe_default);
	dp->usb_io = (struct apple_t6050_usb_io) {
		.context = dp, .read = combo_read, .write = combo_write,
		.poll = combo_poll, .delay = combo_delay,
	};
	dp->usb_platform = (struct apple_t6050_usb_platform) {
		.context = dp, .io = &dp->usb_io, .wrapper_power = combo_wrapper,
		.firmware = combo_firmware, .tunable = combo_tunable, .pause_ns = combo_pause,
	};
	dp->reset = (struct reset_controller_dev) {
		.owner = THIS_MODULE, .ops = &combo_reset_ops, .dev = dev,
		.of_node = dev->of_node, .nr_resets = 1,
	};
	ret = devm_reset_controller_register(dev, &dp->reset);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < 2; i++) {
		dp->combo_phy[i] = devm_phy_create(dev, NULL, i ? &combo_usb3_ops : &combo_usb2_ops);
		if (IS_ERR(dp->combo_phy[i]))
			return PTR_ERR(dp->combo_phy[i]);
		phy_set_drvdata(dp->combo_phy[i], dp);
	}
	return 0;
}
