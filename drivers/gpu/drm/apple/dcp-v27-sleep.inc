/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Firmware 27 (T6050) display sleep: a reversible prepare at the system suspend, then the
 * firmware's polled stop from the sleep's last steps, after SEP's request.
 */
#if IS_ENABLED(CONFIG_APPLE_SOC_SLEEP)
#include "dcp-v27-sleep-contract.h"
#include <linux/soc/apple/dcp-v27-sleep.h>
static const u8 v27_sleep_endpoints[] = {
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x28,
	0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x31,
};
enum v27_phase { V27_LIVE, V27_PREPARED, V27_ARMED, V27_STOPPED, V27_FAILED };
static bool v27_two_stage = true;
module_param(v27_two_stage, bool, 0644);
MODULE_PARM_DESC(v27_two_stage, "V27 sleep: the panel's soft power state off before the hard one, on after it (macOS's display stage)");
static bool v27_soft_first = true;
module_param(v27_soft_first, bool, 0644);
MODULE_PARM_DESC(v27_soft_first, "V27 sleep: the display's soft off (display stage, plain A484) before the hard off, as macOS's display sleep");

/*
 * The calls made before A484's plain form in either direction, at the wake and at the display's
 * sleep: A414, A489, A489, A487, A489, A412. At the wake each is followed by A500's wake form, at
 * the display's sleep none is. Their arguments come from the DT; their replies are not needed.
 */
static void dcp_v27_display_stage(struct apple_dcp *dcp, bool a500)
{
	u8 args[IOMFB_V27_STAGE_ARGS];
	const struct {
		char tag[4] __nonstring;
		u8 in, out;
		const u8 *data;
	} seq[] = {
		{ "A414", 8, 8, args },
		{ "A489", 0, 4, NULL },
		{ "A489", 0, 4, NULL },
		{ "A487", 4, 0, args + 8 },
		{ "A489", 0, 4, NULL },
		{ "A412", 0, 4, NULL },
	};
	unsigned int i;
	int ret;

	if (iomfb_v27_display_args(dcp, true, args))
		return;
	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		ret = iomfb_v27_sleep_call(dcp, seq[i].tag, seq[i].data, seq[i].in, seq[i].out);
		if (ret)
			dev_warn(dcp->dev, "V27 display stage: %.4s: %d\n", seq[i].tag, ret);
		if (a500)
			iomfb_v27_sleep_rpc(dcp, true, true);
	}
}
struct dcp_v27_sleep {
	struct apple_dcp *dcp;
	struct device *scanout;
	struct apple_rtkit_v27_scope *rx;
	struct apple_dart_v27_lease *firmware, *display;
	enum v27_phase phase;
	unsigned long closed;
	bool workers_disabled, paused, power_off, registered, restore_on, soft_off;
	bool display_early;	/* the display DART's lease taken and stopped at the prepare */
	int stop_error, recovery_error;
	u64 attempt;
};
/* The display's own power state moves too, when the DT describes its calls. */
static bool dcp_v27_two_stage(struct dcp_v27_sleep *s)
{
	return s->restore_on && v27_two_stage && iomfb_v27_display_described(s->dcp);
}
static bool dcp_v27_sleep_selected(struct apple_dcp *dcp)
{
	return apple_sleep_v27_late() &&
		(of_device_is_compatible(dcp->dev->of_node, "apple,t6050-dcp") ||
		 of_device_is_compatible(dcp->dev->of_node, "apple,t6050-dcpext")) &&
		dcp->hw.adopt_live_session && dcp->fw_compat == DCP_FIRMWARE_V_27_0 &&
		dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_DEFAULT;
}
static bool dcp_v27_external(struct apple_dcp *dcp)
{
	/* dcp->external is a T6030 policy flag; a T6050 DCPEXT is known by its compatible. */
	return of_device_is_compatible(dcp->dev->of_node, "apple,t6050-dcpext");
}
static void dcp_v27_sleep_invalidate(struct apple_dcp *dcp)
{
	apple_sleep_display_quiesced(dcp->dev, false);
	apple_sleep_v27_prepared(dcp->dev, false);
}
/* The native close list is not the complete Linux application inventory.
 * IOMFB (0x37) drains through its serialized queue. Every other advertised,
 * started or instantiated endpoint must have a supported AFK close protocol.
 */
static int dcp_v27_sleep_inventory(struct apple_dcp *dcp)
{
	u32 supported = 0;
	unsigned int ep, i;

	if (READ_ONCE(dcp->sleep_v27_unhandled))
		return -EUCLEAN;
	for (i = 0; i < ARRAY_SIZE(v27_sleep_endpoints); i++)
		supported |= BIT(v27_sleep_endpoints[i] - 0x20);
	for (ep = 0x20; ep < 0x100; ep++) {
		bool tracked = ep < 0x40 && READ_ONCE(dcp->sleep_v27_ep[ep - 0x20]);

		if (ep == IOMFB_ENDPOINT) {
			if (tracked)
				return -EPROTO;
			continue;
		}
		if (!tracked && !apple_rtkit_endpoint_started(dcp->rtk, ep) &&
		    !apple_rtkit_has_endpoint(dcp->rtk, ep))
			continue;
		if (ep >= 0x40 || !(supported & BIT(ep - 0x20)))
			return -EOPNOTSUPP;
	}
	return 0;
}

static int dcp_v27_sleep_settle(struct apple_dcp *dcp)
{
	int i, ret, check;
	bool drained;

	ret = dcp_v27_sleep_inventory(dcp);
	if (ret)
		return ret;
	ret = read_poll_timeout(apple_rtkit_v27_rx_drained, drained, drained,
		1000, 250000, false, dcp->rtk);
	if (ret)
		return ret;
	ret = iomfb_queue_settle(dcp, msecs_to_jiffies(1000));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(v27_sleep_endpoints); i++) {
		struct apple_dcp_afkep *ep = dcp->sleep_v27_ep[v27_sleep_endpoints[i] - 0x20];

		ret = read_poll_timeout(afk_v27_sleep_check, check, check != -EBUSY,
			1000, 250000, false, ep);
		if (ret || check)
			return ret ?: check;
	}
	/* Before IDLE this is a snapshot; after mailbox_off RX is detached and
	 * synchronized. The same check then closes the accepted-callback lifetime.
	 */
	return apple_rtkit_v27_rx_drained(dcp->rtk) ? 0 : -EBUSY;
}

/* Do not replace a running endpoint or call afk_shutdown: both keep their
 * rings, service references and outstanding callback lifetimes across sleep.
 * New auxiliary endpoints get only the actual ring start and code4/5 handshake;
 * no invented service opens or speculative EPIC commands.
 */
static int dcp_v27_sleep_endpoints_open(struct apple_dcp *dcp)
{
	static const struct apple_epic_service_ops no_services[] = { {} };
	struct apple_dcp_afkep *ep;
	int i, ret, check;

	for (i = 0; i < ARRAY_SIZE(v27_sleep_endpoints); i++) {
		u8 id = v27_sleep_endpoints[i];

		if (!apple_rtkit_has_endpoint(dcp->rtk, id))
			return -ENODEV;
		ep = READ_ONCE(dcp->sleep_v27_ep[id - 0x20]);
		if (ep)
			continue;
		ep = afk_init(dcp, id, no_services);
		if (IS_ERR(ep))
			return PTR_ERR(ep);
		/* afk_init routes replies before afk_start publishes anything. */
		ep->sleep_closed = true;
		ret = afk_start(ep);
		if (!ret) {
			ret = read_poll_timeout(afk_v27_sleep_check, check, check != -EBUSY,
				1000, 100000, false, ep);
			if (!ret)
				ret = check;
		}
		if (!ret)
			ret = afk_v27_sleep_control(ep, true);
		if (ret) {
			ep->sleep_poisoned = true;
			return ret;
		}
	}
	return 0;
}

static void dcp_v27_sleep_unpublish(struct apple_dcp *dcp)
{
	struct dcp_v27_sleep *s = dcp->sleep_v27;

	/* Set with queue admission's mutex; callbacks/replies remain deliverable. */
	mutex_lock(&dcp->iomfb.lock);
	WRITE_ONCE(dcp->sleep_v27_closed, true);
	mutex_unlock(&dcp->iomfb.lock);
	iomfb_pause_presentation(dcp, true);
	s->paused = true;
	dcp_disable_typec_work(dcp, false);
	s->workers_disabled = true;
	/* Only H17P external DCPs have the 180 Hz retry work (dcp_180hz_init()). */
	if (dcp->rate_retry_initialized)
		disable_delayed_work_sync(&dcp->rate_retry_wq);
	disable_delayed_work_sync(&dcp->swap_watchdog_wq);
	disable_work_sync(&dcp->dimensions_wq);
	disable_work_sync(&dcp->bl_update_wq);
}


static int dcp_v27_fail(struct dcp_v27_sleep *s, int ret)
{
	/* Also reached with interrupts off in the final stop: no console wait there. */
	if (!irqs_disabled())
		dev_err(s->dcp->dev, "sleep step failed in phase %d: %d (%ps)\n",
			s->phase, ret, __builtin_return_address(0));
	s->phase = V27_FAILED;
	s->stop_error = ret;
	WRITE_ONCE(s->dcp->crashed, true);
	dcp_v27_sleep_invalidate(s->dcp);
	return ret;
}

/*
 * The internal display's DART stops at the display sleep, once the display is off and before
 * A500: here at that point of the prepare, with its clients quiesced: the CRTC off,
 * presentation paused, the queue settled and A484 answered. Its lease is then held to the
 * disarm.
 */
static int dcp_v27_disp_dart_stop_early(struct dcp_v27_sleep *s)
{
	struct apple_dcp *dcp = s->dcp;
	unsigned long flags;
	int ret, undo;

	ret = apple_dart_v27_claim(dcp->s2r_disp_dart, dcp->dev, s->scanout, true, false, &s->display);
	if (ret)
		return ret;
	local_irq_save(flags);
	ret = apple_dart_v27_stop_atomic(s->display);
	local_irq_restore(flags);
	if (ret) {
		undo = apple_dart_v27_finish(s->display, false);
		s->display = NULL;
		return undo ?: ret;
	}
	s->display_early = true;
	return 0;
}

/* The early lease back (its streams on again) before the prepare is undone. */
static int dcp_v27_disp_dart_release(struct dcp_v27_sleep *s)
{
	int ret = 0;

	if (s->display_early && s->display) {
		ret = apple_dart_v27_finish(s->display, false);
		if (!ret)
			s->display = NULL;
	}
	if (!ret)
		s->display_early = false;
	return ret;
}

/* Normal process-context cancellation: firmware was NEVER idled or stopped. */
static int dcp_v27_reopen_live(struct apple_dcp *dcp)
{
	struct dcp_v27_sleep *s = dcp->sleep_v27;
	unsigned int i;
	int ret;

	ret = dcp_v27_disp_dart_release(s);
	if (ret)
		return ret;
	if (!apple_rtkit_v27_live(dcp->rtk) ||
	    !(readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL) & APPLE_DCP_COPROC_CPU_CONTROL_RUN))
		return -EUCLEAN;
	for (i = 0; i < ARRAY_SIZE(v27_sleep_endpoints); i++) {
		if (!(s->closed & BIT(i)))
			continue;
		ret = afk_v27_sleep_control(dcp->sleep_v27_ep[v27_sleep_endpoints[i] - 0x20], true);
		if (ret)
			return ret;
		s->closed &= ~BIT(i);
	}
	if (s->power_off) {
		if (s->restore_on) {
			/* A500's wake form first; its failure does not stop the power-on. */
			iomfb_v27_sleep_rpc(dcp, true, true);
			if (s->soft_off)
				iomfb_v27_sleep_call(dcp, "A489", NULL, 0, 4);
			ret = iomfb_v27_sleep_rpc(dcp, false, true);
			if (ret)
				return ret;
			/* The hard state is on again; the display comes on with the soft one, as macOS's wake. */
			if (s->soft_off) {
				ret = iomfb_v27_sleep_a115(dcp, true);
				if (ret)
					dev_warn(dcp->dev, "V27 wake: A115(1): %d\n", ret);
				dcp_v27_display_stage(dcp, true);
				ret = iomfb_v27_sleep_plain(dcp, true);
				if (ret)
					return ret;
				s->soft_off = false;
			}
			WRITE_ONCE(dcp->v27_keep_mode, true);
		}
		/* An unused external pipeline stays OFF; its next actual activation
		 * uses the existing DEFAULT V27 normal power-on path. No fake sink.
		 */
		s->power_off = false;
	} else if (s->soft_off) {
		/* An entry refused between the display's soft off and the hard one. */
		dcp_v27_display_stage(dcp, true);
		ret = iomfb_v27_sleep_plain(dcp, true);
		if (ret)
			return ret;
		s->soft_off = false;
	}
	ret = iomfb_queue_settle(dcp, msecs_to_jiffies(1000));
	if (ret)
		return ret;
	mutex_lock(&dcp->iomfb.lock);
	WRITE_ONCE(dcp->sleep_v27_closed, false);
	mutex_unlock(&dcp->iomfb.lock);
	if (s->paused)
		iomfb_pause_presentation(dcp, false);
	if (READ_ONCE(dcp->v27_keep_mode))
		iomfb_v27_wake_present(dcp);
	if (s->workers_disabled) {
		if (dcp->rate_retry_initialized)
			enable_delayed_work(&dcp->rate_retry_wq);
		enable_delayed_work(&dcp->swap_watchdog_wq);
		enable_work(&dcp->dimensions_wq);
		enable_work(&dcp->bl_update_wq);
		dcp_enable_typec_work(dcp);
	}
	s->paused = s->workers_disabled = false;
	s->phase = V27_LIVE;
	WRITE_ONCE(dcp->s2r_fw_down, false);
	dcp_s2r_put_darts(dcp);
	dcp->brightness.update = true;
	dcp_v27_sleep_invalidate(dcp);
	return 0;
}

static int dcp_v27_late_arm(void *data)
{
	struct dcp_v27_sleep *s = data;
	struct apple_dcp *dcp = s->dcp;
	int ret, undo;

	if (s->phase != V27_PREPARED) {
		dev_info(dcp->dev, "V27 arm: phase %d\n", s->phase);
		return -EBUSY;
	}
	ret = apple_sleep_display_v27_power_ready(dcp->dev);
	if (ret) {
		dev_info(dcp->dev, "V27 arm: power ready %d\n", ret);
		return ret;
	}
	ret = apple_dart_v27_claim(dcp->s2r_dcp_dart, dcp->dev, NULL, false, false, &s->firmware);
	if (ret) {
		dev_info(dcp->dev, "V27 arm: firmware DART %d\n", ret);
		return ret;
	}
	if (!s->display_early)
		ret = apple_dart_v27_claim(dcp->s2r_disp_dart, dcp->dev, s->scanout, true,
			dcp_v27_external(dcp), &s->display);
	if (ret)
		dev_info(dcp->dev, "V27 arm: display DART %d\n", ret);
	if (!ret) {
		ret = apple_rtkit_v27_arm(dcp->rtk, &s->rx);
		if (ret)
			dev_info(dcp->dev, "V27 arm: RTKit %d\n", ret);
	}
	if (ret) {
		if (s->display && !s->display_early) {
			undo = apple_dart_v27_finish(s->display, false);
			if (undo)
				return dcp_v27_fail(s, undo);
			s->display = NULL;
		}
		undo = apple_dart_v27_finish(s->firmware, false);
		if (undo)
			return dcp_v27_fail(s, undo);
		s->firmware = NULL;
		return ret;
	}
	s->phase = V27_ARMED;
	return 0;
}

static int dcp_v27_late_check(void *data)
{
	struct dcp_v27_sleep *s = data;
	struct apple_dcp *dcp = s->dcp;
	int ret;

	if (s->phase != V27_ARMED || !s->power_off ||
	    s->closed != GENMASK(ARRAY_SIZE(v27_sleep_endpoints) - 1, 0) ||
	    !READ_ONCE(dcp->sleep_v27_closed) || READ_ONCE(dcp->crashed) || READ_ONCE(dcp->quiescing) ||
	    !(readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL) & APPLE_DCP_COPROC_CPU_CONTROL_RUN))
		return -EBUSY;
	ret = dcp_v27_sleep_inventory(dcp);
	if (!ret)
		ret = apple_dart_v27_check(s->display);
	if (!ret)
		ret = apple_dart_v27_check(s->firmware);
	if (!ret)
		ret = apple_rtkit_v27_check(s->rx);
	return ret;
}

static int dcp_v27_late_stop(void *data)
{
	struct dcp_v27_sleep *s = data;
	struct apple_dcp *dcp = s->dcp;
	u32 status;
	int ret;

	ret = apple_rtkit_v27_idle_atomic(s->rx);
	if (ret)
		return dcp_v27_fail(s, ret);
	ret = readl_poll_timeout_atomic(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_STATUS,
		status, (status & (APPLE_DCP_COPROC_CPU_STATUS_IDLE | APPLE_DCP_COPROC_CPU_STATUS_STOPPED)) ==
		(APPLE_DCP_COPROC_CPU_STATUS_IDLE | APPLE_DCP_COPROC_CPU_STATUS_STOPPED), 1, 5000);
	if (ret)
		return dcp_v27_fail(s, ret);
	dcp_s2r_cpu_stop(dcp);
	if (readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL) & APPLE_DCP_COPROC_CPU_CONTROL_RUN)
		return dcp_v27_fail(s, -EIO);
	WRITE_ONCE(dcp->s2r_fw_down, true);
	ret = s->display_early ? 0 : apple_dart_v27_stop_atomic(s->display);
	if (!ret)
		ret = apple_dart_v27_stop_atomic(s->firmware);
	if (ret)
		return dcp_v27_fail(s, ret);
	s->phase = V27_STOPPED;
	return 0;
}

static int dcp_v27_late_disarm(void *data, bool stopped)
{
	struct dcp_v27_sleep *s = data;
	int ret;

	if (s->phase == V27_FAILED || (stopped && s->phase != V27_STOPPED))
		return dcp_v27_fail(s, -EUCLEAN);
	ret = apple_sleep_display_v27_power_ready(s->dcp->dev);
	if (!ret)
		ret = apple_dart_v27_finish(s->firmware, stopped);
	if (ret)
		return dcp_v27_fail(s, ret);
	s->firmware = NULL;
	ret = apple_dart_v27_finish(s->display, stopped);
	if (ret)
		return dcp_v27_fail(s, ret);
	s->display = NULL;
	s->display_early = false;
	ret = apple_rtkit_v27_disarm(s->rx, stopped);
	if (ret)
		return dcp_v27_fail(s, ret);
	s->rx = NULL;
	if (!stopped)
		s->phase = V27_PREPARED;
	return 0;
}

static const struct apple_sleep_v27_ops dcp_v27_late_ops = {
	.arm = dcp_v27_late_arm, .check = dcp_v27_late_check,
	.stop = dcp_v27_late_stop, .disarm = dcp_v27_late_disarm,
};

static int dcp_v27_sleep_suspend(struct apple_dcp *dcp)
{
	struct dcp_v27_sleep *s;
	unsigned int i;
	int ret;

	dcp_v27_sleep_invalidate(dcp);
	/* Refused before anything changes: the sleep RPCs' arguments come from the DT. */
	if (!iomfb_v27_sleep_described(dcp)) {
		dev_warn(dcp->dev, "V27 sleep: no sleep RPC arguments in the DT\n");
		return -EOPNOTSUPP;
	}
	if (!dcp->retain_dma || IS_ERR_OR_NULL(dcp->rtk) || !dcp->active || dcp->crashed || dcp->quiescing ||
	    dcp->peer_owner || dcp->peer_reserved || dcp->secure_rtk)
		return -EBUSY;
	for (i = 0; i < dcp->hw.num_dptx_ports; i++)
		if (READ_ONCE(dcp->dptxport[i].connected))
			return -EBUSY;
	if (dcp_v27_external(dcp) && (READ_ONCE(dcp->active_typec_route) ||
	    READ_ONCE(dcp->typec_cable_connected) ||
	    (dcp->hdmi_hpd && gpiod_get_value_cansleep(dcp->hdmi_hpd) > 0)))
		return -EBUSY;
	/* Mode-config suspend is the actual DRM scanout-owner boundary. No HPD
	 * shortcut: external firmware still executes A484/A500/AFK and late IDLE.
	 */
	if (dcp->crtc && !dcp->crtc->base.dev->mode_config.suspend_state)
		return -EBUSY;
	if (dcp_v27_external(dcp) && dcp->crtc) {
		/* The saved copy is the duplicated state's new state; its old
		 * state is stale once disable_all has committed.
		 */
		struct drm_crtc_state *saved = drm_atomic_get_new_crtc_state(
			dcp->crtc->base.dev->mode_config.suspend_state, &dcp->crtc->base);

		if (!saved || saved->active)
			return -EBUSY;
	}
	if (!dcp_v27_external(dcp) && !dcp->crtc)
		return -ENODEV;
	if (!dcp_v27_external(dcp) && !READ_ONCE(dcp->pipe_enabled_h17p))
		return -EAGAIN; /* require the actual normal driver pipe-on ACK */
	if (!dcp->sleep_v27) {
		dcp->sleep_v27 = devm_kzalloc(dcp->dev, sizeof(*s), GFP_KERNEL);
		if (!dcp->sleep_v27)
			return -ENOMEM;
	}
	s = dcp->sleep_v27;
	if (s->phase != V27_LIVE)
		return -EUCLEAN;
	s->dcp = dcp;
	s->scanout = dcp->crtc ? dcp->crtc->base.dev->dev : NULL;
	s->restore_on = !dcp_v27_external(dcp);
	s->attempt++;
	ret = apple_sleep_v27_register(dcp->dev, &dcp_v27_late_ops, s, THIS_MODULE, dcp_v27_external(dcp));
	if (ret)
		return ret;
	s->registered = true;
	ret = dcp_v27_sleep_inventory(dcp);
	if (!ret)
		ret = dcp_v27_sleep_endpoints_open(dcp);
	if (ret)
		return ret;
	dcp_s2r_find_darts(dcp);
	if (!dcp->s2r_dcp_dart || !dcp->s2r_disp_dart) {
		dcp_s2r_put_darts(dcp);
		return -ENODEV;
	}
	dcp_v27_sleep_unpublish(dcp);
	ret = dcp_v27_sleep_settle(dcp);
	if (ret)
		goto undo;
	/*
	 * macOS's display sleep first (the display stage, the plain A484(0), answered when the
	 * display is off, then A500's wake form), and the system's hard off after it.
	 */
	if (dcp_v27_two_stage(s) && v27_soft_first) {
		dcp_v27_display_stage(dcp, false);
		ret = iomfb_v27_sleep_plain(dcp, false);
		if (ret)
			goto undo;
		s->soft_off = true;
		iomfb_v27_sleep_rpc(dcp, true, true);
	}
	if (dcp_v27_two_stage(s))
		iomfb_v27_sleep_call(dcp, "A489", NULL, 0, 4);
	ret = iomfb_v27_sleep_rpc(dcp, false, false); /* A484, actual state/status */
	if (ret)
		goto uncertain;
	s->power_off = true;
	/* The AP's state word for the sleep, right after the hard off, as macOS. */
	if (dcp_v27_two_stage(s)) {
		ret = iomfb_v27_sleep_a115(dcp, false);
		if (ret)
			dev_warn(dcp->dev, "V27 sleep: A115(0): %d\n", ret);
	}
	/*
	 * Without the display's sleep first, the soft state goes off after the hard one, so that the
	 * wake's hard power-on still leaves the display off and the plain form turns it on.
	 */
	if (dcp_v27_two_stage(s) && !s->soft_off) {
		ret = iomfb_v27_sleep_plain(dcp, false);
		if (ret)
			goto uncertain;
		s->soft_off = true;
	}
	/*
	 * The display's DART stops once the pipe has stopped fetching: after the A484s, answered
	 * when the display is off, and before A500. Stopped before A484, a pipe that is still lit
	 * can read through it and the firmware dies of a bus error.
	 */
	if (!dcp_v27_external(dcp)) {
		ret = dcp_v27_disp_dart_stop_early(s);
		if (ret)
			goto undo; /* nothing stopped: the display back on, the sleep refused */
	}
	/* A500's sleep form right after A484, on every DCP and DCPEXT. */
	ret = iomfb_v27_sleep_rpc(dcp, true, false);
	if (ret)
		goto uncertain;
	for (i = ARRAY_SIZE(v27_sleep_endpoints); i-- > 0;) {
		ret = afk_v27_sleep_control(dcp->sleep_v27_ep[v27_sleep_endpoints[i] - 0x20], false);
		if (ret)
			goto uncertain;
		s->closed |= BIT(i);
	}
	ret = dcp_v27_sleep_settle(dcp);
	if (ret)
		goto uncertain;
	/* Firmware remains AP/IOP ON + RUN. No provider/domain mutation here. */
	if (!apple_rtkit_v27_live(dcp->rtk) ||
	    !(readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL) & APPLE_DCP_COPROC_CPU_CONTROL_RUN)) {
		ret = -EUCLEAN;
		goto uncertain;
	}
	s->phase = V27_PREPARED;
	apple_sleep_v27_prepared(dcp->dev, true);
	return 0;
undo:
	s->recovery_error = dcp_v27_reopen_live(dcp);
	if (s->recovery_error)
		return dcp_v27_fail(s, s->recovery_error);
	return ret;
uncertain:
	/* Late/failed wire transactions retain their durable DMA/callback state. */
	return dcp_v27_fail(s, ret);
}

static int dcp_v27_sleep_resume(struct apple_dcp *dcp)
{
	struct dcp_v27_sleep *s = dcp->sleep_v27;
	int ret;

	if (!s || s->phase == V27_FAILED) {
		dev_err(dcp->dev, "no restart after the sleep: an earlier step failed: %d (recovery %d)\n",
			s ? s->stop_error : 0, s ? s->recovery_error : 0);
		return -EUCLEAN;
	}
	dev_dbg(dcp->dev, "restart after the sleep from phase %d\n", s->phase);
	if (READ_ONCE(dcp->quiescing))
		return dcp_v27_fail(s, -ENODEV);
	/* A refusal before the arm: the early display lease goes back first. */
	if (s->phase == V27_PREPARED) {
		ret = dcp_v27_disp_dart_release(s);
		if (ret)
			return dcp_v27_fail(s, ret);
	}
	if (s->phase == V27_ARMED || s->rx || s->firmware || s->display)
		return dcp_v27_fail(s, -EBUSY);
	if (s->phase == V27_STOPPED) {
		ret = apple_sleep_display_v27_power_ready(dcp->dev);
		if (!ret)
			ret = apple_rtkit_warm_restart_strict(dcp->rtk);
		/*
		 * The final step stopped this CPU and it is still stopped: the firmware boots
		 * from its reset vector next, so the state tracked before the stop does not
		 * decide. Start it.
		 */
		if (ret == -EUCLEAN && READ_ONCE(dcp->s2r_fw_down) &&
		    !(readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL) &
		      APPLE_DCP_COPROC_CPU_CONTROL_RUN))
			ret = apple_rtkit_warm_restart_stopped(dcp->rtk);
		if (ret)
			return dcp_v27_fail(s, ret);
		writel(readl(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL) |
			APPLE_DCP_COPROC_CPU_CONTROL_RUN, dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
		ret = apple_rtkit_boot(dcp->rtk);
		if (ret)
			return dcp_v27_fail(s, ret);
	}
	ret = dcp_v27_reopen_live(dcp);
	if (ret)
		return dcp_v27_fail(s, ret);
	return 0;
}
#else
static bool dcp_v27_sleep_selected(struct apple_dcp *dcp) { return false; }
static void dcp_v27_sleep_invalidate(struct apple_dcp *dcp) { }
static int dcp_v27_sleep_suspend(struct apple_dcp *dcp) { return -EOPNOTSUPP; }
static int dcp_v27_sleep_resume(struct apple_dcp *dcp) { return -EOPNOTSUPP; }
#endif
