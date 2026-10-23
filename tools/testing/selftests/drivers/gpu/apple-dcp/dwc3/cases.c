/* SPDX-License-Identifier: GPL-2.0-only */
static struct atcphy_hw hardware;
static int counted_off(struct phy *p)
{
	callback_calls++;
	return atcphy_usb3_power_off(p);
}
static int other_off(struct phy *p)
{
	(void)p;
	return other_off_error;
}
static const struct phy_ops usb3_ops = { .power_off = counted_off };
static const struct phy_ops usb2_ops = { .init = atcphy_usb2_init,
					 .power_off = other_off };
static const struct phy_ops plain_ops = { .power_off = other_off };
static void dwc3_host_exit(struct dwc3 *d)
{
	host_exits++;
	if (root_refs) {
		assert(phy_power_off(d->usb3_generic_phy[0]) == 0);
		assert(phy_exit(d->usb3_generic_phy[0]) == 0);
		root_refs = 0;
	}
}
/* Boundary mocks acquire references through the production generic PHY API. */
static int dwc3_core_init(struct dwc3 *d)
{
	starts++;
	if (core_init_error)
		return core_init_error;
	for (int i = 0; i < d->num_usb2_ports; i++)
		assert(phy_init(d->usb2_generic_phy[i]) == 0);
	for (int i = 0; i < d->num_usb3_ports; i++)
		assert(phy_init(d->usb3_generic_phy[i]) == 0);
	return dwc3_phy_power_on(d);
}
static int dwc3_apple_core_probe(struct dwc3_apple *a)
{
	int ret = dwc3_core_init(&a->dwc);
	if (!ret)
		a->state = DWC3_APPLE_NO_CABLE;
	return ret;
}
static void dwc3_apple_set_role(struct dwc3_apple *a, enum dwc3_apple_state s)
{
	assert(atcphy_usb3_set_mode(a->usb3_phy,
				    s == DWC3_APPLE_HOST ? PHY_MODE_USB_HOST :
							   PHY_MODE_USB_DEVICE,
				    0) == 0);
}
struct fixture {
	struct apple_atcphy atc;
	struct phy p2, p3, extra;
	struct dwc3_apple apple;
};
/* The post-tunable callback of each modelled SoC: T8103, T8122, T8140 */
static void (*const post_tunable[])(struct apple_atcphy *, enum atcphy_mode) = {
	atcphy_configure_post_tunable_t8103,
	atcphy_configure_post_tunable_t8122,
	atcphy_configure_post_tunable_all_modes,
};
static void initial(struct fixture *f, unsigned int gen, bool fixed, bool pm)
{
	memset(f, 0, sizeof(*f));
	memset(core_regs, 0, sizeof(core_regs));
	memset(pipe_regs, 0, sizeof(pipe_regs));
	memset(usb2_regs, 0, sizeof(usb2_regs));
	hardware = (struct atcphy_hw){
		.gen = gen == 0 ? ATCPHY_GENERATION_T8103 :
				  ATCPHY_GENERATION_T8122,
		.has_t8140_dp = gen == 2,
		.has_usb4 = gen != 2,
		.pipe_mux_clk = PIPEHANDLER_MUX_CTRL_CLK_T8103,
		.pipe_mux_data = PIPEHANDLER_MUX_CTRL_DATA_T8103,
	};
	hardware.configure_post_tunable = post_tunable[gen];
	f->atc = (struct apple_atcphy){
		/* Generation zero models an admitted base M1. */
		.t8103_usb3_workaround = gen == 0,
		.hw = &hardware,
		.regs.core = (unsigned char *)core_regs,
		.regs.lpdptx = (unsigned char *)core_regs,
		.regs.pipehandler = (unsigned char *)pipe_regs,
		.regs.usb2phy = (unsigned char *)usb2_regs,
		.mode = APPLE_ATCPHY_MODE_USB3,
		.typec_mode = APPLE_ATCPHY_MODE_USB3,
		.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3,
		.host_active = true,
		.fixed_usb2 = fixed,
		.ss_tunables = true,
		.dp_tunables = true,
	};
	f->p3 = (struct phy){ .data = &f->atc,
			      .ops = &usb3_ops,
			      .power_count = 1,
			      .init_count = 1,
			      .pm_enabled = pm,
			      .pm_refs = pm ? 1 : 0 };
	f->p2 = (struct phy){ .data = &f->atc,
			      .ops = &usb2_ops,
			      .power_count = 1,
			      .init_count = 1,
			      .pm_enabled = pm,
			      .pm_refs = pm ? 1 : 0 };
	f->extra = (struct phy){ .ops = &plain_ops,
				 .power_count = 1,
				 .init_count = 1,
				 .pm_enabled = pm,
				 .pm_refs = pm ? 1 : 0 };
	f->apple = (struct dwc3_apple){
		.state = DWC3_APPLE_HOST,
		.reset = &f->atc.rcdev,
		.usb2_phy = &f->p2,
		.usb3_phy = &f->p3,
		.core_active = true,
		.reset_pending = true,
		.usb3_power_off_pending = 1,
		.usb2_power_off_pending = 1,
		.dwc = { .num_usb3_ports = 1,
			 .num_usb2_ports = 1,
			 .usb3_generic_phy = { &f->p3 },
			 .usb2_generic_phy = { &f->p2 } },
	};
	core_regs[ATCPHY_POWER_CTRL / 4] =
		ATCPHY_POWER_PHY_RESET_N | ATCPHY_POWER_SLEEP_BIG |
		ATCPHY_POWER_SLEEP_SMALL | ATCPHY_POWER_APB_RESET_N;
	core_regs[ATCPHY_POWER_STAT / 4] = core_regs[ATCPHY_POWER_CTRL / 4];
	core_regs[AUS_COMMON_DIG_RCAL1 / 4] =
		AUS_COMMON_DIG_RCAL1_ALL_CODES_DONE;
	core_regs[ACIOPHY_TOP_PHY_STAT / 4] = ACIOPHY_TOP_PHY_STAT_LN0_UNK0;
	pipe_regs[PIPEHANDLER_AON_GEN / 4] = PIPEHANDLER_AON_GEN_DWC3_RESET_N;
	polls = fail_poll = reset_calls = reset_writes = core_writes =
		warnings = 0;
	generic_warnings = 0;
	host_exits = clocks_disabled = callback_calls = gadget_exits =
		event_cleanups = 0;
	legacy_suspends = core_mode_exits = free_buffers = starts = deasserts =
		setup_cio = root_refs = 0;
	host_init_error = gadget_init_error = core_init_error = reset_error =
		other_off_error = 0;
	init_cleanup_fault = false;
	syscore = true;
	stuck_ack = false;
}
static void recover(void)
{
	fail_poll = 0;
	stuck_ack = false;
	pipe_regs[PIPEHANDLER_LOCK_ACK / 4] = 0;
}
static int exit_apple(struct fixture *f)
{
	mutex_lock(&f->apple.lock);
	int ret = dwc3_apple_exit(&f->apple);
	mutex_unlock(&f->apple.lock);
	return ret;
}
static int role(struct fixture *f, enum usb_role r)
{
	struct usb_role_switch sw = { &f->apple };
	return dwc3_usb_role_switch_set(&sw, r);
}
static void clean(struct fixture *f)
{
	assert(!f->p3.power_count && !f->p2.power_count && !f->p3.init_count &&
	       !f->p2.init_count);
	assert(!f->p3.pm_refs && !f->p2.pm_refs && !f->apple.core_active &&
	       !f->apple.reset_pending);
	assert(!f->apple.usb3_power_off_pending &&
	       !f->apple.usb2_power_off_pending);
	assert(!f->atc.host_active && !f->atc.mode_dirty &&
	       f->atc.mode == APPLE_ATCPHY_MODE_OFF);
	assert(!syscore);
	assert(!f->atc.lock && !f->p3.mutex && !f->p2.mutex && !f->apple.lock);
	assert(!(pipe_regs[PIPEHANDLER_AON_GEN / 4] &
		 PIPEHANDLER_AON_GEN_DWC3_RESET_N));
}
static void cycles(void)
{
	unsigned int cases = 0;
	for (unsigned int gen = 0; gen < 3; gen++)
		for (unsigned int fixed = 0; fixed < 2; fixed++)
			for (unsigned int pm = 0; pm < 2; pm++)
				for (unsigned int fault = 0; fault < 4;
				     fault++) {
					struct fixture f;
					initial(&f, gen, fixed, pm);
					if (fault == 1) {
						stuck_ack = true;
						pipe_regs[PIPEHANDLER_LOCK_ACK /
							  4] =
							PIPEHANDLER_LOCK_EN;
					} else if (fault > 1) {
						f.atc.pipe_state =
							ATCPHY_PIPEHANDLER_STATE_DUMMY;
						fail_poll = fault - 1;
					}
					int ret = exit_apple(&f);
					printf("CYCLE gen=%u fixed=%u pm=%u fault=%u exit=%d refs=%d init=%d\n",
					       gen, fixed, pm, fault, ret,
					       f.p3.power_count,
					       f.p3.init_count);
					fflush(stdout);
					if (fault) {
						assert(ret == -ETIMEDOUT &&
						       f.p3.power_count == 1 &&
						       f.p3.init_count == 1);
						assert(f.apple.core_active &&
						       clocks_disabled == 0 &&
						       event_cleanups == 1 &&
						       host_exits == 1);
						assert(syscore);
						if (fault > 1)
							assert(f.atc.mode_dirty);
						if (fault == 1) {
							assert(role(&f,
								    USB_ROLE_HOST) ==
								       -ETIMEDOUT &&
							       starts == 0 &&
							       deasserts == 0);
							assert(host_exits ==
								       1 &&
							       event_cleanups ==
								       1 &&
							       f.p3.power_count ==
								       1);
						}
						recover();
						assert(exit_apple(&f) == 0);
					} else
						assert(ret == 0);
					clean(&f);
					assert(host_exits == 1 &&
					       clocks_disabled == 1 &&
					       event_cleanups == 1);
					assert(exit_apple(&f) == 0 &&
					       host_exits == 1 &&
					       clocks_disabled == 1);
					assert(role(&f, USB_ROLE_HOST) == 0 &&
					       f.p3.power_count == 1 &&
					       f.p3.init_count == 1);
					assert(f.atc.mode ==
						       APPLE_ATCPHY_MODE_USB3 &&
					       !f.atc.mode_dirty);
					assert(core_regs[ATCPHY_POWER_CTRL / 4] &
					       ATCPHY_POWER_PHY_RESET_N);
					assert(role(&f, USB_ROLE_NONE) == 0);
					clean(&f);
					assert(!generic_warnings);
					if (!fault)
						assert(!warnings);
					cases++;
				}
	printf("PASS: %u normal/parking/domain shutdown and restart scenarios\n",
	       cases);
}
static void pending_reset(void)
{
	for (unsigned int gen = 0; gen < 3; gen++) {
		struct fixture f;
		initial(&f, gen, false, true);
		reset_error = -ETIMEDOUT;
		assert(exit_apple(&f) == -ETIMEDOUT);
		assert(!f.apple.core_active && f.apple.reset_pending &&
		       !f.p3.power_count && !f.p3.init_count);
		assert(syscore);
		assert(role(&f, USB_ROLE_HOST) == -ETIMEDOUT && !starts &&
		       !deasserts);
		assert(host_exits == 1 && clocks_disabled == 1 &&
		       event_cleanups == 1 && callback_calls == 1);
		reset_error = 0;
		assert(exit_apple(&f) == 0);
		clean(&f);
		assert(host_exits == 1 && clocks_disabled == 1 &&
		       callback_calls == 1);
		assert(reset_calls == 3);
	}
	puts("PASS: pending reset retry does not repeat host, PHY or core teardown");
}
static void partial_release(void)
{
	struct fixture f;
	initial(&f, 1, false, true);
	other_off_error = -EIO;
	assert(exit_apple(&f) == -EIO);
	assert(!f.p3.power_count && f.p3.init_count == 1 &&
	       f.p2.power_count == 1);
	assert(!f.apple.usb3_power_off_pending &&
	       f.apple.usb2_power_off_pending == 1);
	assert(callback_calls == 1 && !clocks_disabled);
	other_off_error = 0;
	assert(exit_apple(&f) == 0);
	clean(&f);
	assert(callback_calls == 1 && event_cleanups == 1 &&
	       clocks_disabled == 1);
	initial(&f, 1, false, false);
	f.apple.dwc.num_usb3_ports = 2;
	f.apple.dwc.usb3_generic_phy[1] = &f.extra;
	f.apple.usb3_power_off_pending = 3;
	other_off_error = -EIO;
	assert(exit_apple(&f) == -EIO && !f.p3.power_count &&
	       f.extra.power_count == 1);
	other_off_error = 0;
	assert(exit_apple(&f) == 0 && !f.extra.power_count &&
	       !f.extra.init_count);
	clean(&f);
	assert(callback_calls == 1);
	puts("PASS: successful USB3 references are not released twice after another PHY fails");
}
static void init_failures(void)
{
	for (unsigned int device = 0; device < 2; device++) {
		struct fixture f;
		initial(&f, 2, false, true);
		assert(exit_apple(&f) == 0);
		if (device)
			gadget_init_error = -EIO;
		else
			host_init_error = -EIO;
		reset_error = -ETIMEDOUT;
		assert(role(&f, device ? USB_ROLE_DEVICE : USB_ROLE_HOST) ==
		       -EIO);
		assert(f.apple.reset_pending && !f.apple.core_active &&
		       !f.p3.power_count);
		assert(syscore);
		host_init_error = gadget_init_error = reset_error = 0;
		assert(role(&f, device ? USB_ROLE_DEVICE : USB_ROLE_HOST) == 0);
		assert(f.p3.power_count == 1 && f.p3.init_count == 1);
		assert(role(&f, USB_ROLE_NONE) == 0);
		clean(&f);
	}
	struct fixture f;
	initial(&f, 1, false, false);
	assert(exit_apple(&f) == 0);
	core_init_error = -EIO;
	reset_error = -ETIMEDOUT;
	assert(role(&f, USB_ROLE_HOST) == -EIO && f.apple.reset_pending &&
	       !f.apple.core_active);
	core_init_error = reset_error = 0;
	assert(role(&f, USB_ROLE_HOST) == 0);
	assert(role(&f, USB_ROLE_NONE) == 0);
	clean(&f);
	initial(&f, 1, false, true);
	assert(exit_apple(&f) == 0);
	host_init_error = -EIO;
	init_cleanup_fault = true;
	assert(role(&f, USB_ROLE_HOST) == -EIO && f.apple.core_active);
	assert(syscore);
	assert(f.p3.power_count == 1 && f.p3.init_count == 1);
	recover();
	host_init_error = 0;
	init_cleanup_fault = false;
	assert(role(&f, USB_ROLE_HOST) == 0 && f.p3.power_count == 1 &&
	       f.p3.init_count == 1);
	assert(role(&f, USB_ROLE_NONE) == 0);
	clean(&f);
	puts("PASS: host/gadget/core startup failure reset obligations drain before retry");
}
static void pending_restart(void)
{
	unsigned int cases = 0;
	for (unsigned int gen = 0; gen < 3; gen++)
		for (unsigned int fixed = 0; fixed < 2; fixed++)
			for (unsigned int pm = 0; pm < 2; pm++)
				for (unsigned int fault = 1; fault < 4;
				     fault++) {
					struct fixture f;
					initial(&f, gen, fixed, pm);
					if (fault == 1) {
						stuck_ack = true;
						pipe_regs[PIPEHANDLER_LOCK_ACK /
							  4] =
							PIPEHANDLER_LOCK_EN;
					} else {
						f.atc.pipe_state =
							ATCPHY_PIPEHANDLER_STATE_DUMMY;
						fail_poll = fault - 1;
					}
					assert(exit_apple(&f) == -ETIMEDOUT);
					recover();
					assert(role(&f, USB_ROLE_HOST) == 0 &&
					       f.p3.power_count == 1 &&
					       f.p3.init_count == 1);
					assert(f.atc.mode ==
						       APPLE_ATCPHY_MODE_USB3 &&
					       !f.atc.mode_dirty);
					assert(core_regs[ATCPHY_POWER_CTRL / 4] &
					       ATCPHY_POWER_PHY_RESET_N);
					assert(!generic_warnings);
					assert(role(&f, USB_ROLE_NONE) == 0);
					clean(&f);
					cases++;
				}
	struct fixture f;
	initial(&f, 2, true, true);
	f.apple.fixed_hub = true;
	assert(role(&f, USB_ROLE_NONE) == 0 &&
	       f.apple.state == DWC3_APPLE_HOST && f.p3.power_count == 1);
	assert(exit_apple(&f) == 0);
	mutex_lock(&f.apple.lock);
	assert(dwc3_apple_init(&f.apple, DWC3_APPLE_HOST) == 0);
	mutex_unlock(&f.apple.lock);
	assert(f.p3.power_count == 1 && exit_apple(&f) == 0);
	clean(&f);
	printf("PASS: %u pending-failure to HOST transitions and actual fixed-hub role no-op\n",
	       cases);
}
static void negative_power(void)
{
	struct fixture f;
	initial(&f, 0, false, false);
	f.atc.pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	fail_poll = 1;
	assert(exit_apple(&f) == -ETIMEDOUT && f.p3.power_count == 1 &&
	       f.p3.init_count == 1);
	recover();
	assert(role(&f, USB_ROLE_HOST) == 0 && f.p3.power_count == 1);
}
static void negative_reset(void)
{
	struct fixture f;
	initial(&f, 0, false, false);
	reset_error = -ETIMEDOUT;
	assert(exit_apple(&f) == -ETIMEDOUT && reset_calls == 1);
	reset_error = 0;
	assert(exit_apple(&f) == 0 && reset_calls == 2);
	assert(!(pipe_regs[PIPEHANDLER_AON_GEN / 4] &
		 PIPEHANDLER_AON_GEN_DWC3_RESET_N));
}
static void negative_legacy(void)
{
	struct fixture f;
	initial(&f, 0, false, false);
	f.atc.pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	fail_poll = 1;
	assert(phy_power_off(&f.p3) == -ETIMEDOUT && f.atc.mode_dirty);
	recover();
	struct typec_mux_dev mux = { &f.atc };
	struct typec_mux_state state = { .mode = TYPEC_STATE_USB };
	assert(atcphy_mux_set(&mux, &state) == 0);
	assert(core_regs[ATCPHY_POWER_CTRL / 4] & ATCPHY_POWER_PHY_RESET_N);
}
static void legacy_restore(void)
{
	for (unsigned int gen = 0; gen < 3; gen++) {
		struct fixture f;
		initial(&f, gen, false, false);
		f.atc.pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
		fail_poll = 1;
		assert(phy_power_off(&f.p3) == -ETIMEDOUT && f.atc.mode_dirty);
		unsigned int before = core_writes;
		assert(atcphy_usb3_set_mode(&f.p3, PHY_MODE_USB_HOST, 0) ==
			       -EBUSY &&
		       core_writes == before);
		if (gen < 2) {
			f.atc.dp_link_rate = ATCPHY_DP_LINK_RATE_RBR;
			assert(atcphy_dp_configure(&f.atc,
						   ATCPHY_DP_LINK_RATE_RBR) ==
				       -EBUSY &&
			       core_writes == before);
		}
		recover();
		struct typec_mux_dev mux = { &f.atc };
		struct typec_mux_state state = { .mode = TYPEC_STATE_USB };
		assert(atcphy_mux_set(&mux, &state) == 0 && !f.atc.mode_dirty &&
		       core_writes > before);
		assert(core_regs[ATCPHY_POWER_CTRL / 4] &
		       ATCPHY_POWER_PHY_RESET_N);
		assert(phy_power_off(&f.p3) == 0 && !f.p3.power_count);
	}
	puts("PASS: M1/M2/J700 aborted shutdown blocks PIPE and restores saved lane mode");
}
static void removal_and_generic(void)
{
	struct fixture f;
	initial(&f, 0, false, false);
	root_refs = 1;
	assert(phy_init(&f.p3) == 0 && phy_power_on(&f.p3) == 0);
	assert(exit_apple(&f) == 0);
	clean(&f);
	unsigned int callbacks = callback_calls, clocks = clocks_disabled,
		     events = event_cleanups;
	dwc3_core_remove_no_shutdown(&f.apple.dwc);
	assert(callbacks == callback_calls && clocks == clocks_disabled &&
	       events == event_cleanups);
	assert(!core_mode_exits && free_buffers == 1 && !f.p3.power_count &&
	       !f.p3.init_count);
	initial(&f, 1, false, false);
	dwc3_core_exit(&f.apple.dwc);
	assert(!f.p3.power_count && !f.p3.init_count && !f.p2.power_count &&
	       !f.p2.init_count);
	assert(event_cleanups == 1 && clocks_disabled == 1 &&
	       callback_calls == 1);
	puts("PASS: root-hub references, already-exited removal and ordinary generic core exit");
}
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "power")) {
		negative_power();
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "reset")) {
		negative_reset();
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "legacy")) {
		negative_legacy();
		return 0;
	}
	cycles();
	pending_restart();
	pending_reset();
	partial_release();
	init_failures();
	legacy_restore();
	removal_and_generic();
	puts("PASS: Apple teardown actual-function qualification");
	return 0;
}
