// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/string.h>

#include "cd321x-dp.h"
#include "cd321x-pm.h"

/* A dock in pin assignment D: DisplayPort plus USB 3. */
#define DP_D	(TPS_DATA_STATUS_DATA_CONNECTION | TPS_DATA_STATUS_USB2_CONNECTION | \
		 TPS_DATA_STATUS_USB3_CONNECTION | TPS_DATA_STATUS_DP_CONNECTION | \
		 FIELD_PREP(TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK, 1))
#define HPD	CD321X_DATA_STATUS_HPD_LEVEL
/* The same dock giving the USB 3 lanes to DisplayPort: pin assignment C. */
#define DP_C	(DP_D & ~TPS_DATA_STATUS_USB3_CONNECTION)
/* A USB-C to DisplayPort cable: pin assignment E, or F with USB 3. */
#define DP_E	(TPS_DATA_STATUS_DATA_CONNECTION | TPS_DATA_STATUS_DP_CONNECTION)
#define DP_F	(DP_E | TPS_DATA_STATUS_USB2_CONNECTION | TPS_DATA_STATUS_USB3_CONNECTION)

static void cd321x_dp_first_hpd_connects_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = DP_D | HPD,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* Entering DP without HPD sets up the route but announces nothing. */
static void cd321x_dp_route_without_hpd_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D,
		.changed = DP_D,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_NONE);
}

static void cd321x_dp_unplug_tears_down_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.changed = DP_D | HPD,
		.link_reset = CD321X_DP_RESET_UNPLUG | CD321X_DP_RESET_PLUG,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, false), CD321X_DP_EVENT_NONE);
}

/* A flip or a new partner rebuilds the route even with HPD still high. */
static void cd321x_dp_link_reset_reconnects_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.link_reset = CD321X_DP_RESET_FLIP,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

static void cd321x_dp_hpd_low_tears_down_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D,
		.changed = HPD,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_NONE);
}

static void cd321x_dp_exit_tears_down_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = TPS_DATA_STATUS_DATA_CONNECTION |
			       TPS_DATA_STATUS_USB2_CONNECTION |
			       TPS_DATA_STATUS_USB3_CONNECTION,
		.changed = TPS_DATA_STATUS_DP_CONNECTION | HPD |
			   TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, false), CD321X_DP_EVENT_NONE);
}

/* A new pin assignment rebuilds the lanes under the stream. */
static void cd321x_dp_pin_change_reconnects_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/*
 * C <-> D and E <-> F change only USB3_CONNECTION, but they change the
 * DisplayPort lane count, so the lanes are rebuilt with HPD held high.
 */
static void cd321x_dp_usb3_pin_change_reconnects_test(struct kunit *test)
{
	const u32 pins[][2] = {
		{ DP_D, DP_C }, { DP_C, DP_D }, { DP_F, DP_E }, { DP_E, DP_F },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		struct cd321x_dp_update u = {
			.data_status = pins[i][1] | HPD,
			.changed = pins[i][0] ^ pins[i][1],
			.route_active = true,
			.sink_reported = true,
		};

		/* Neither DP_CONNECTION nor the pin assignment field moves. */
		KUNIT_EXPECT_FALSE(test, u.changed & CD321X_DP_MODE_BITS);
		KUNIT_EXPECT_TRUE(test, cd321x_dp_mode_changed(u.data_status, u.changed));
		KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
		KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
	}
}

/* Without DisplayPort, USB 3 coming or going is not a lane change. */
static void cd321x_dp_usb3_change_without_dp_test(struct kunit *test)
{
	u32 usb2 = TPS_DATA_STATUS_DATA_CONNECTION | TPS_DATA_STATUS_USB2_CONNECTION;
	u32 usb3 = TPS_DATA_STATUS_USB3_CONNECTION;
	u32 dp = TPS_DATA_STATUS_DP_CONNECTION;

	KUNIT_EXPECT_FALSE(test, cd321x_dp_mode_changed(usb2, usb3));
	KUNIT_EXPECT_FALSE(test, cd321x_dp_mode_changed(usb2 | usb3, usb3));
	/* Leaving DisplayPort is a mode change whatever happens to USB 3. */
	KUNIT_EXPECT_TRUE(test, cd321x_dp_mode_changed(usb2, dp | usb3));
}

/* A power or USB-only update leaves a live display alone. */
static void cd321x_dp_unrelated_update_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = TPS_DATA_STATUS_USB_DATA_ROLE,
		.route_active = true,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* A port that does not own the display never reports for it. */
static void cd321x_dp_display_taken_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = DP_D | HPD,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, false), CD321X_DP_EVENT_NONE);
	u.data_status = 0;
	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
}

/*
 * HPD went low and back within the debounce on the same dock and lanes.
 * That is not an IRQ_HPD: the sink may have been replaced behind the dock,
 * so it is reported gone and connected again.
 */
static void cd321x_dp_hpd_bounce_reconnects_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = HPD,
		.route_active = true,
		.sink_reported = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);

	/* An IRQ_HPD in the same window does not make the bounce harmless. */
	u.data_status |= CD321X_DATA_STATUS_HPD_IRQ;
	u.changed |= CD321X_DATA_STATUS_HPD_IRQ;
	u.irq_hpd = true;
	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* An IRQ_HPD attention with HPD held high asks for a link check. */
static void cd321x_dp_irq_hpd_retrains_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ,
		.changed = CD321X_DATA_STATUS_HPD_IRQ,
		.irq_hpd = true,
		.route_active = true,
		.sink_reported = true,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_RETRAIN);

	/* The sticky bit seen again, with nothing pending, is no new request. */
	u.irq_hpd = false;
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* Either IRQ_HPD indication becoming set is an attention. */
static void cd321x_dp_irq_hpd_sources_test(struct kunit *test)
{
	u32 hpd = DP_D | HPD, irq = hpd | CD321X_DATA_STATUS_HPD_IRQ;

	KUNIT_EXPECT_TRUE(test, cd321x_dp_irq_hpd(false, hpd, irq, 0, 0));
	KUNIT_EXPECT_TRUE(test, cd321x_dp_irq_hpd(false, hpd, hpd, DP_STATUS_HPD_STATE,
						  DP_STATUS_HPD_STATE | DP_STATUS_IRQ_HPD));
	/* Still set from an earlier attention, or clearing: nothing new. */
	KUNIT_EXPECT_FALSE(test, cd321x_dp_irq_hpd(false, irq, irq, DP_STATUS_IRQ_HPD,
						   DP_STATUS_IRQ_HPD));
	KUNIT_EXPECT_FALSE(test, cd321x_dp_irq_hpd(false, irq, hpd, DP_STATUS_IRQ_HPD, 0));
	/* IRQ_HPD is only valid with HPD high. */
	KUNIT_EXPECT_FALSE(test, cd321x_dp_irq_hpd(false, DP_D, DP_D | CD321X_DATA_STATUS_HPD_IRQ,
						   0, DP_STATUS_IRQ_HPD));
}

/* A pending attention survives later interrupts until HPD drops. */
static void cd321x_dp_irq_hpd_pending_test(struct kunit *test)
{
	u32 hpd = DP_D | HPD, irq = hpd | CD321X_DATA_STATUS_HPD_IRQ;
	bool pending;

	/* Set and cleared again within one debounce window. */
	pending = cd321x_dp_irq_hpd(false, hpd, irq, 0, 0);
	pending = cd321x_dp_irq_hpd(pending, irq, hpd, 0, 0);
	KUNIT_EXPECT_TRUE(test, pending);
	/* An unrelated interrupt rereads the same status. */
	pending = cd321x_dp_irq_hpd(pending, hpd, hpd, 0, 0);
	KUNIT_EXPECT_TRUE(test, pending);
	/* HPD low: the sink is reconnected, nothing is left to retrain. */
	pending = cd321x_dp_irq_hpd(pending, hpd, DP_D, 0, 0);
	KUNIT_EXPECT_FALSE(test, pending);
	pending = cd321x_dp_irq_hpd(pending, DP_D, hpd, 0, 0);
	KUNIT_EXPECT_FALSE(test, pending);
}

/* HPD rising after a reported low is a connect, as before. */
static void cd321x_dp_hpd_rise_connects_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = HPD,
		.route_active = true,
	};

	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
	/* Attention is no reason to skip announcing a sink that just came up. */
	u.data_status |= CD321X_DATA_STATUS_HPD_IRQ;
	u.changed |= CD321X_DATA_STATUS_HPD_IRQ;
	u.irq_hpd = true;
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* An HPD bounce with a C <-> D lane change: a reconnect either way. */
static void cd321x_dp_bounce_with_lane_change_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = HPD | TPS_DATA_STATUS_USB3_CONNECTION,
		.route_active = true,
		.sink_reported = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);

	u.changed = HPD | TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK;
	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* A replug, flip or new dock with an IRQ_HPD is still a reconnect. */
static void cd321x_dp_irq_hpd_with_link_reset_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ,
		.changed = CD321X_DATA_STATUS_HPD_IRQ,
		.link_reset = CD321X_DP_RESET_PARTNER,
		.irq_hpd = true,
		.route_active = true,
		.sink_reported = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* A route without a delivered HPD has nothing to retrain yet. */
static void cd321x_dp_retrain_needs_live_sink_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ,
		.changed = CD321X_DATA_STATUS_HPD_IRQ,
		.irq_hpd = true,
		.route_active = true,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);

	/* A port that just entered DP connects rather than retrains. */
	u.route_active = false;
	u.sink_reported = true;
	u.changed = HPD;
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/*
 * A pass that reported the sink gone and then failed leaves the route
 * marked up.  When the retry finds HPD high again the sink must be
 * announced, not retrained: the connector has no link left to retrain.
 */
static void cd321x_dp_retry_after_teardown_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = HPD,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

static struct kunit_case cd321x_dp_cases[] = {
	KUNIT_CASE(cd321x_dp_first_hpd_connects_test),
	KUNIT_CASE(cd321x_dp_route_without_hpd_test),
	KUNIT_CASE(cd321x_dp_unplug_tears_down_test),
	KUNIT_CASE(cd321x_dp_link_reset_reconnects_test),
	KUNIT_CASE(cd321x_dp_hpd_low_tears_down_test),
	KUNIT_CASE(cd321x_dp_exit_tears_down_test),
	KUNIT_CASE(cd321x_dp_pin_change_reconnects_test),
	KUNIT_CASE(cd321x_dp_usb3_pin_change_reconnects_test),
	KUNIT_CASE(cd321x_dp_usb3_change_without_dp_test),
	KUNIT_CASE(cd321x_dp_unrelated_update_test),
	KUNIT_CASE(cd321x_dp_display_taken_test),
	KUNIT_CASE(cd321x_dp_hpd_bounce_reconnects_test),
	KUNIT_CASE(cd321x_dp_irq_hpd_retrains_test),
	KUNIT_CASE(cd321x_dp_irq_hpd_sources_test),
	KUNIT_CASE(cd321x_dp_irq_hpd_pending_test),
	KUNIT_CASE(cd321x_dp_hpd_rise_connects_test),
	KUNIT_CASE(cd321x_dp_bounce_with_lane_change_test),
	KUNIT_CASE(cd321x_dp_irq_hpd_with_link_reset_test),
	KUNIT_CASE(cd321x_dp_retrain_needs_live_sink_test),
	KUNIT_CASE(cd321x_dp_retry_after_teardown_test),
	{}
};

static struct kunit_suite cd321x_dp_suite = {
	.name = "cd321x-dp",
	.test_cases = cd321x_dp_cases,
};

/*
 * One port as cd321x_queue_status() and cd321x_update_work() drive it:
 * every interrupt adds a sample, and a debounced update reports the display
 * around a mux update that can fail.  @log records the connector events in
 * order: D(isconnected), C(onnected) and U(nknown, i.e. retrain).
 */
struct cd321x_dp_port {
	struct cd321x_pm_state pm;
	u32 status, data_status, rx;
	u32 status_changed, data_changed;
	bool irq_hpd, route_active, sink_reported;
	char log[16];
};

static void port_notify(struct cd321x_dp_port *p, char event)
{
	size_t n = strlen(p->log);

	if (n + 1 < sizeof(p->log))
		p->log[n] = event;
	p->sink_reported = event != 'D';
}

/* An interrupt that reads @status, @data_status and the DP status @rx. */
static void port_irq(struct cd321x_dp_port *p, u32 status, u32 data_status, u32 rx)
{
	p->irq_hpd = cd321x_dp_irq_hpd(p->irq_hpd, p->data_status, data_status, p->rx, rx);
	p->status_changed |= p->status ^ status;
	p->data_changed |= p->data_status ^ data_status;
	p->status = status;
	p->data_status = data_status;
	p->rx = rx;
}

/* A debounced update whose mux update returns @ret. */
static void port_update(struct cd321x_dp_port *p, int ret)
{
	bool connected = p->status & TPS_STATUS_PLUG_PRESENT;
	struct cd321x_dp_update u = {
		.data_status = p->data_status,
		.changed = p->data_changed,
		.irq_hpd = p->irq_hpd,
		.route_active = p->route_active,
		.sink_reported = p->sink_reported,
	};

	if (p->pm.force_reconnect)
		u.link_reset |= CD321X_DP_RESET_FORCED;
	if (p->status_changed & TPS_STATUS_PLUG_PRESENT)
		u.link_reset |= CD321X_DP_RESET_PLUG;
	if (!connected)
		u.link_reset |= CD321X_DP_RESET_UNPLUG;
	p->status_changed = 0;
	p->data_changed = 0;

	if (cd321x_dp_teardown(&u))
		port_notify(p, 'D');
	if (u.link_reset)
		p->route_active = false;
	if (!connected) {
		p->irq_hpd = false;
		cd321x_pm_complete(&p->pm);
		return;
	}
	if (ret) {
		cd321x_pm_retry(&p->pm, ret != -EAGAIN);
		return;
	}

	p->route_active = u.data_status & TPS_DATA_STATUS_DP_CONNECTION;
	p->irq_hpd = false;
	switch (cd321x_dp_event(&u, p->route_active)) {
	case CD321X_DP_EVENT_NONE:
		break;
	case CD321X_DP_EVENT_CONNECTED:
		port_notify(p, 'C');
		break;
	case CD321X_DP_EVENT_RETRAIN:
		port_notify(p, 'C');
		port_notify(p, 'U');
		break;
	}
	cd321x_pm_complete(&p->pm);
}

/* The fresh read that a retried update starts from. */
static void port_reread(struct kunit *test, struct cd321x_dp_port *p)
{
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&p->pm));
	cd321x_pm_snapshot_ready(&p->pm);
	port_irq(p, p->status, p->data_status, p->rx);
}

#define PLUG	TPS_STATUS_PLUG_PRESENT
#define HPD_RX	DP_STATUS_HPD_STATE

/* A dock in pin assignment D with its sink announced and nothing pending. */
static void port_docked(struct kunit *test, struct cd321x_dp_port *p)
{
	*p = (struct cd321x_dp_port){ .pm.phase = CD321X_PM_RUNNING };
	port_irq(p, PLUG, DP_D | HPD, HPD_RX);
	port_update(p, 0);
	KUNIT_ASSERT_STREQ(test, p->log, "C");
	memset(p->log, 0, sizeof(p->log));
}

/*
 * The cases below are the nine sequence invariants of the tipd HPD pulse
 * review, in its order, run through the helpers the driver uses.
 */

/* 1, 2: a Type-C unplug and replug within the debounce. */
static void cd321x_dp_seq_typec_replug_test(struct kunit *test)
{
	struct cd321x_dp_port p;

	port_docked(test, &p);
	port_irq(&p, 0, 0, HPD_RX);
	port_irq(&p, PLUG, DP_D | HPD, HPD_RX);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "DC");
}

/* 3: the monitor behind the dock is replaced; the dock stays. */
static void cd321x_dp_seq_downstream_replug_test(struct kunit *test)
{
	struct cd321x_dp_port p;

	port_docked(test, &p);
	port_irq(&p, PLUG, DP_D, 0);
	port_irq(&p, PLUG, DP_D | HPD, HPD_RX);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "DC");
}

/* 4: the sink's IRQ_HPD retrains the link and keeps the sink. */
static void cd321x_dp_seq_irq_hpd_retrains_test(struct kunit *test)
{
	struct cd321x_dp_port p;

	port_docked(test, &p);
	port_irq(&p, PLUG, DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ, HPD_RX);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "CU");

	/* Reported by the partner's DP status alone, set and cleared again. */
	memset(p.log, 0, sizeof(p.log));
	port_irq(&p, PLUG, DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ, HPD_RX | DP_STATUS_IRQ_HPD);
	port_irq(&p, PLUG, DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ, HPD_RX);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "CU");
}

/* 5: -EAGAIN retries from a fresh read and keeps the session. */
static void cd321x_dp_seq_eagain_keeps_session_test(struct kunit *test)
{
	struct cd321x_pm_state pm = { .phase = CD321X_PM_RUNNING };

	KUNIT_EXPECT_TRUE(test, cd321x_pm_retry(&pm, false));
	KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
	KUNIT_EXPECT_TRUE(test, cd321x_pm_begin_read(&pm));
}

/* 6: an IRQ_HPD whose update is deferred is still delivered, once. */
static void cd321x_dp_seq_irq_hpd_after_eagain_test(struct kunit *test)
{
	struct cd321x_dp_port p;

	port_docked(test, &p);
	port_irq(&p, PLUG, DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ, HPD_RX);
	port_update(&p, -EAGAIN);
	KUNIT_EXPECT_STREQ(test, p.log, "");
	port_reread(test, &p);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "CU");

	/* Delivered: the sticky bit seen again is not another request. */
	port_irq(&p, PLUG, DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ |
		 TPS_DATA_STATUS_USB_DATA_ROLE, HPD_RX);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "CUC");
}

/* 7: HPD low at the end of the debounce. */
static void cd321x_dp_seq_hpd_low_test(struct kunit *test)
{
	struct cd321x_dp_port p;

	port_docked(test, &p);
	port_irq(&p, PLUG, DP_D, 0);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "D");
}

/* 8: D to C with HPD held high: the lane count changes. */
static void cd321x_dp_seq_usb3_pin_change_test(struct kunit *test)
{
	struct cd321x_dp_port p;

	port_docked(test, &p);
	port_irq(&p, PLUG, DP_C | HPD, HPD_RX);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "DC");
}

/* 9: D to C with an HPD bounce. */
static void cd321x_dp_seq_pin_change_with_bounce_test(struct kunit *test)
{
	struct cd321x_dp_port p;

	port_docked(test, &p);
	port_irq(&p, PLUG, DP_D, 0);
	port_irq(&p, PLUG, DP_C | HPD, HPD_RX);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "DC");
}

/* A bounce whose reconnect is deferred announces the sink on the retry. */
static void cd321x_dp_seq_bounce_after_eagain_test(struct kunit *test)
{
	struct cd321x_dp_port p;

	port_docked(test, &p);
	port_irq(&p, PLUG, DP_D, 0);
	port_irq(&p, PLUG, DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ, HPD_RX);
	port_update(&p, -EAGAIN);
	KUNIT_EXPECT_STREQ(test, p.log, "D");
	port_reread(test, &p);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "DC");
}

/* An IRQ_HPD still pending when the dock leaves is dropped with it. */
static void cd321x_dp_seq_irq_hpd_then_unplug_test(struct kunit *test)
{
	struct cd321x_dp_port p;

	port_docked(test, &p);
	port_irq(&p, PLUG, DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ, HPD_RX);
	port_update(&p, -EAGAIN);
	port_irq(&p, 0, 0, HPD_RX);
	port_reread(test, &p);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "D");
	KUNIT_EXPECT_FALSE(test, p.irq_hpd);

	port_irq(&p, PLUG, DP_D | HPD, HPD_RX);
	port_update(&p, 0);
	KUNIT_EXPECT_STREQ(test, p.log, "DC");
}

static struct kunit_case cd321x_dp_seq_cases[] = {
	KUNIT_CASE(cd321x_dp_seq_typec_replug_test),
	KUNIT_CASE(cd321x_dp_seq_downstream_replug_test),
	KUNIT_CASE(cd321x_dp_seq_irq_hpd_retrains_test),
	KUNIT_CASE(cd321x_dp_seq_eagain_keeps_session_test),
	KUNIT_CASE(cd321x_dp_seq_irq_hpd_after_eagain_test),
	KUNIT_CASE(cd321x_dp_seq_hpd_low_test),
	KUNIT_CASE(cd321x_dp_seq_usb3_pin_change_test),
	KUNIT_CASE(cd321x_dp_seq_pin_change_with_bounce_test),
	KUNIT_CASE(cd321x_dp_seq_bounce_after_eagain_test),
	KUNIT_CASE(cd321x_dp_seq_irq_hpd_then_unplug_test),
	{}
};

static struct kunit_suite cd321x_dp_seq_suite = {
	.name = "cd321x-dp-sequence",
	.test_cases = cd321x_dp_seq_cases,
};

kunit_test_suites(&cd321x_dp_suite, &cd321x_dp_seq_suite);
MODULE_DESCRIPTION("KUnit tests for the CD321x DisplayPort hotplug decisions");
MODULE_LICENSE("GPL");
