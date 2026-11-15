// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests
 *
 * Copyright (C) 2020, Intel Corporation
 * Author: Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#include <linux/delay.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/idr.h>
#include <linux/suspend.h>

#include "tb.h"
#include "tunnel.h"

static int __ida_init(struct kunit_resource *res, void *context)
{
	struct ida *ida = context;

	ida_init(ida);
	res->data = ida;
	return 0;
}

static void __ida_destroy(struct kunit_resource *res)
{
	struct ida *ida = res->data;

	ida_destroy(ida);
}

static void kunit_ida_init(struct kunit *test, struct ida *ida)
{
	kunit_alloc_resource(test, __ida_init, __ida_destroy, GFP_KERNEL, ida);
}

static void tb_test_switch_release(struct device *dev)
{
	/* The memory is owned by KUnit, nothing to do here */
}

static void tb_test_switch_put(void *data)
{
	struct tb_switch *sw = data;

	put_device(&sw->dev);
}

static struct tb_switch *alloc_switch(struct kunit *test, u64 route,
				      u8 upstream_port, u8 max_port_number)
{
	struct tb_switch *sw;
	size_t size;
	int i;

	sw = kunit_kzalloc(test, sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return NULL;

	/*
	 * HopID allocations take a reference to their routers and those devices
	 * have to be initialized for that to work.
	 */
	sw->dev.release = tb_test_switch_release;
	device_initialize(&sw->dev);
	if (kunit_add_action_or_reset(test, tb_test_switch_put, sw))
		return NULL;

	sw->config.upstream_port_number = upstream_port;
	sw->config.depth = tb_route_length(route);
	sw->config.route_hi = upper_32_bits(route);
	sw->config.route_lo = lower_32_bits(route);
	sw->config.enabled = 0;
	sw->config.max_port_number = max_port_number;

	size = (sw->config.max_port_number + 1) * sizeof(*sw->ports);
	sw->ports = kunit_kzalloc(test, size, GFP_KERNEL);
	if (!sw->ports)
		return NULL;

	for (i = 0; i <= sw->config.max_port_number; i++) {
		sw->ports[i].sw = sw;
		sw->ports[i].port = i;
		sw->ports[i].config.port_number = i;
		if (i) {
			kunit_ida_init(test, &sw->ports[i].in_hopids);
			kunit_ida_init(test, &sw->ports[i].out_hopids);
		}
	}

	return sw;
}

static struct tb_switch *alloc_host(struct kunit *test)
{
	struct tb_switch *sw;

	sw = alloc_switch(test, 0, 7, 13);
	if (!sw)
		return NULL;

	sw->config.vendor_id = 0x8086;
	sw->config.device_id = 0x9a1b;

	sw->ports[0].config.type = TB_TYPE_PORT;
	sw->ports[0].config.max_in_hop_id = 7;
	sw->ports[0].config.max_out_hop_id = 7;

	sw->ports[1].config.type = TB_TYPE_PORT;
	sw->ports[1].config.max_in_hop_id = 19;
	sw->ports[1].config.max_out_hop_id = 19;
	sw->ports[1].total_credits = 60;
	sw->ports[1].ctl_credits = 2;
	sw->ports[1].dual_link_port = &sw->ports[2];

	sw->ports[2].config.type = TB_TYPE_PORT;
	sw->ports[2].config.max_in_hop_id = 19;
	sw->ports[2].config.max_out_hop_id = 19;
	sw->ports[2].total_credits = 60;
	sw->ports[2].ctl_credits = 2;
	sw->ports[2].dual_link_port = &sw->ports[1];
	sw->ports[2].link_nr = 1;

	sw->ports[3].config.type = TB_TYPE_PORT;
	sw->ports[3].config.max_in_hop_id = 19;
	sw->ports[3].config.max_out_hop_id = 19;
	sw->ports[3].total_credits = 60;
	sw->ports[3].ctl_credits = 2;
	sw->ports[3].dual_link_port = &sw->ports[4];

	sw->ports[4].config.type = TB_TYPE_PORT;
	sw->ports[4].config.max_in_hop_id = 19;
	sw->ports[4].config.max_out_hop_id = 19;
	sw->ports[4].total_credits = 60;
	sw->ports[4].ctl_credits = 2;
	sw->ports[4].dual_link_port = &sw->ports[3];
	sw->ports[4].link_nr = 1;

	sw->ports[5].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[5].config.max_in_hop_id = 9;
	sw->ports[5].config.max_out_hop_id = 9;
	sw->ports[5].cap_adap = -1;

	sw->ports[6].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[6].config.max_in_hop_id = 9;
	sw->ports[6].config.max_out_hop_id = 9;
	sw->ports[6].cap_adap = -1;

	sw->ports[7].config.type = TB_TYPE_NHI;
	sw->ports[7].config.max_in_hop_id = 11;
	sw->ports[7].config.max_out_hop_id = 11;
	sw->ports[7].config.nfc_credits = 0x41800000;

	sw->ports[8].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[8].config.max_in_hop_id = 8;
	sw->ports[8].config.max_out_hop_id = 8;

	sw->ports[9].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[9].config.max_in_hop_id = 8;
	sw->ports[9].config.max_out_hop_id = 8;

	sw->ports[10].disabled = true;
	sw->ports[11].disabled = true;

	sw->ports[12].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[12].config.max_in_hop_id = 8;
	sw->ports[12].config.max_out_hop_id = 8;

	sw->ports[13].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[13].config.max_in_hop_id = 8;
	sw->ports[13].config.max_out_hop_id = 8;

	return sw;
}

static struct tb_switch *alloc_host_usb4(struct kunit *test)
{
	struct tb_switch *sw;

	sw = alloc_host(test);
	if (!sw)
		return NULL;

	sw->generation = 4;
	sw->credit_allocation = true;
	sw->max_usb3_credits = 32;
	sw->min_dp_aux_credits = 1;
	sw->min_dp_main_credits = 0;
	sw->max_pcie_credits = 64;
	sw->max_dma_credits = 14;

	return sw;
}

static struct tb_switch *alloc_host_br(struct kunit *test)
{
	struct tb_switch *sw;

	sw = alloc_host_usb4(test);
	if (!sw)
		return NULL;

	sw->ports[10].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[10].config.max_in_hop_id = 9;
	sw->ports[10].config.max_out_hop_id = 9;
	sw->ports[10].cap_adap = -1;
	sw->ports[10].disabled = false;

	return sw;
}

static struct tb_switch *alloc_dev_default(struct kunit *test,
					   struct tb_switch *parent,
					   u64 route, bool bonded)
{
	struct tb_port *port, *upstream_port;
	struct tb_switch *sw;

	sw = alloc_switch(test, route, 1, 19);
	if (!sw)
		return NULL;

	sw->config.vendor_id = 0x8086;
	sw->config.device_id = 0x15ef;

	sw->ports[0].config.type = TB_TYPE_PORT;
	sw->ports[0].config.max_in_hop_id = 8;
	sw->ports[0].config.max_out_hop_id = 8;

	sw->ports[1].config.type = TB_TYPE_PORT;
	sw->ports[1].config.max_in_hop_id = 19;
	sw->ports[1].config.max_out_hop_id = 19;
	sw->ports[1].total_credits = 60;
	sw->ports[1].ctl_credits = 2;
	sw->ports[1].dual_link_port = &sw->ports[2];

	sw->ports[2].config.type = TB_TYPE_PORT;
	sw->ports[2].config.max_in_hop_id = 19;
	sw->ports[2].config.max_out_hop_id = 19;
	sw->ports[2].total_credits = 60;
	sw->ports[2].ctl_credits = 2;
	sw->ports[2].dual_link_port = &sw->ports[1];
	sw->ports[2].link_nr = 1;

	sw->ports[3].config.type = TB_TYPE_PORT;
	sw->ports[3].config.max_in_hop_id = 19;
	sw->ports[3].config.max_out_hop_id = 19;
	sw->ports[3].total_credits = 60;
	sw->ports[3].ctl_credits = 2;
	sw->ports[3].dual_link_port = &sw->ports[4];

	sw->ports[4].config.type = TB_TYPE_PORT;
	sw->ports[4].config.max_in_hop_id = 19;
	sw->ports[4].config.max_out_hop_id = 19;
	sw->ports[4].total_credits = 60;
	sw->ports[4].ctl_credits = 2;
	sw->ports[4].dual_link_port = &sw->ports[3];
	sw->ports[4].link_nr = 1;

	sw->ports[5].config.type = TB_TYPE_PORT;
	sw->ports[5].config.max_in_hop_id = 19;
	sw->ports[5].config.max_out_hop_id = 19;
	sw->ports[5].total_credits = 60;
	sw->ports[5].ctl_credits = 2;
	sw->ports[5].dual_link_port = &sw->ports[6];

	sw->ports[6].config.type = TB_TYPE_PORT;
	sw->ports[6].config.max_in_hop_id = 19;
	sw->ports[6].config.max_out_hop_id = 19;
	sw->ports[6].total_credits = 60;
	sw->ports[6].ctl_credits = 2;
	sw->ports[6].dual_link_port = &sw->ports[5];
	sw->ports[6].link_nr = 1;

	sw->ports[7].config.type = TB_TYPE_PORT;
	sw->ports[7].config.max_in_hop_id = 19;
	sw->ports[7].config.max_out_hop_id = 19;
	sw->ports[7].total_credits = 60;
	sw->ports[7].ctl_credits = 2;
	sw->ports[7].dual_link_port = &sw->ports[8];

	sw->ports[8].config.type = TB_TYPE_PORT;
	sw->ports[8].config.max_in_hop_id = 19;
	sw->ports[8].config.max_out_hop_id = 19;
	sw->ports[8].total_credits = 60;
	sw->ports[8].ctl_credits = 2;
	sw->ports[8].dual_link_port = &sw->ports[7];
	sw->ports[8].link_nr = 1;

	sw->ports[9].config.type = TB_TYPE_PCIE_UP;
	sw->ports[9].config.max_in_hop_id = 8;
	sw->ports[9].config.max_out_hop_id = 8;

	sw->ports[10].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[10].config.max_in_hop_id = 8;
	sw->ports[10].config.max_out_hop_id = 8;

	sw->ports[11].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[11].config.max_in_hop_id = 8;
	sw->ports[11].config.max_out_hop_id = 8;

	sw->ports[12].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[12].config.max_in_hop_id = 8;
	sw->ports[12].config.max_out_hop_id = 8;

	sw->ports[13].config.type = TB_TYPE_DP_HDMI_OUT;
	sw->ports[13].config.max_in_hop_id = 9;
	sw->ports[13].config.max_out_hop_id = 9;
	sw->ports[13].cap_adap = -1;

	sw->ports[14].config.type = TB_TYPE_DP_HDMI_OUT;
	sw->ports[14].config.max_in_hop_id = 9;
	sw->ports[14].config.max_out_hop_id = 9;
	sw->ports[14].cap_adap = -1;

	sw->ports[15].disabled = true;

	sw->ports[16].config.type = TB_TYPE_USB3_UP;
	sw->ports[16].config.max_in_hop_id = 8;
	sw->ports[16].config.max_out_hop_id = 8;

	sw->ports[17].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[17].config.max_in_hop_id = 8;
	sw->ports[17].config.max_out_hop_id = 8;

	sw->ports[18].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[18].config.max_in_hop_id = 8;
	sw->ports[18].config.max_out_hop_id = 8;

	sw->ports[19].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[19].config.max_in_hop_id = 8;
	sw->ports[19].config.max_out_hop_id = 8;

	if (!parent)
		return sw;

	/* Link them */
	upstream_port = tb_upstream_port(sw);
	port = tb_port_at(route, parent);
	port->remote = upstream_port;
	upstream_port->remote = port;
	if (port->dual_link_port && upstream_port->dual_link_port) {
		port->dual_link_port->remote = upstream_port->dual_link_port;
		upstream_port->dual_link_port->remote = port->dual_link_port;

		if (bonded) {
			/* Bonding is used */
			port->bonded = true;
			port->total_credits *= 2;
			port->dual_link_port->bonded = true;
			port->dual_link_port->total_credits = 0;
			upstream_port->bonded = true;
			upstream_port->total_credits *= 2;
			upstream_port->dual_link_port->bonded = true;
			upstream_port->dual_link_port->total_credits = 0;
		}
	}

	return sw;
}

static struct tb_switch *alloc_dev_with_dpin(struct kunit *test,
					     struct tb_switch *parent,
					     u64 route, bool bonded)
{
	struct tb_switch *sw;

	sw = alloc_dev_default(test, parent, route, bonded);
	if (!sw)
		return NULL;

	sw->ports[13].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[13].config.max_in_hop_id = 9;
	sw->ports[13].config.max_out_hop_id = 9;

	sw->ports[14].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[14].config.max_in_hop_id = 9;
	sw->ports[14].config.max_out_hop_id = 9;

	return sw;
}

static struct tb_switch *alloc_dev_without_dp(struct kunit *test,
					      struct tb_switch *parent,
					      u64 route, bool bonded)
{
	struct tb_switch *sw;
	int i;

	sw = alloc_dev_default(test, parent, route, bonded);
	if (!sw)
		return NULL;
	/*
	 * Device with:
	 * 2x USB4 Adapters (adapters 1,2 and 3,4),
	 * 1x PCIe Upstream (adapter 9),
	 * 1x PCIe Downstream (adapter 10),
	 * 1x USB3 Upstream (adapter 16),
	 * 1x USB3 Downstream (adapter 17)
	 */
	for (i = 5; i <= 8; i++)
		sw->ports[i].disabled = true;

	for (i = 11; i <= 14; i++)
		sw->ports[i].disabled = true;

	sw->ports[13].cap_adap = 0;
	sw->ports[14].cap_adap = 0;

	for (i = 18; i <= 19; i++)
		sw->ports[i].disabled = true;

	sw->generation = 4;
	sw->credit_allocation = true;
	sw->max_usb3_credits = 109;
	sw->min_dp_aux_credits = 0;
	sw->min_dp_main_credits = 0;
	sw->max_pcie_credits = 30;
	sw->max_dma_credits = 1;

	return sw;
}

static struct tb_switch *alloc_dev_usb4(struct kunit *test,
					struct tb_switch *parent,
					u64 route, bool bonded)
{
	struct tb_switch *sw;

	sw = alloc_dev_default(test, parent, route, bonded);
	if (!sw)
		return NULL;

	sw->generation = 4;
	sw->credit_allocation = true;
	sw->max_usb3_credits = 14;
	sw->min_dp_aux_credits = 1;
	sw->min_dp_main_credits = 18;
	sw->max_pcie_credits = 32;
	sw->max_dma_credits = 14;

	return sw;
}

static void tb_test_path_basic(struct kunit *test)
{
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host;

	host = alloc_host(test);

	src_port = &host->ports[5];
	dst_port = src_port;

	p = tb_next_port_on_path(src_port, dst_port, NULL);
	KUNIT_EXPECT_PTR_EQ(test, p, dst_port);

	p = tb_next_port_on_path(src_port, dst_port, p);
	KUNIT_EXPECT_TRUE(test, !p);
}

static void tb_test_path_not_connected_walk(struct kunit *test)
{
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev;

	host = alloc_host(test);
	/* No connection between host and dev */
	dev = alloc_dev_default(test, NULL, 3, true);

	src_port = &host->ports[12];
	dst_port = &dev->ports[16];

	p = tb_next_port_on_path(src_port, dst_port, NULL);
	KUNIT_EXPECT_PTR_EQ(test, p, src_port);

	p = tb_next_port_on_path(src_port, dst_port, p);
	KUNIT_EXPECT_PTR_EQ(test, p, &host->ports[3]);

	p = tb_next_port_on_path(src_port, dst_port, p);
	KUNIT_EXPECT_TRUE(test, !p);

	/* Other direction */

	p = tb_next_port_on_path(dst_port, src_port, NULL);
	KUNIT_EXPECT_PTR_EQ(test, p, dst_port);

	p = tb_next_port_on_path(dst_port, src_port, p);
	KUNIT_EXPECT_PTR_EQ(test, p, &dev->ports[1]);

	p = tb_next_port_on_path(dst_port, src_port, p);
	KUNIT_EXPECT_TRUE(test, !p);
}

struct port_expectation {
	u64 route;
	u8 port;
	enum tb_port_type type;
};

static void tb_test_path_single_hop_walk(struct kunit *test)
{
	/*
	 * Walks from Host PCIe downstream port to Device #1 PCIe
	 * upstream port.
	 *
	 *   [Host]
	 *   1 |
	 *   1 |
	 *  [Device]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x0, .port = 8, .type = TB_TYPE_PCIE_DOWN },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 9, .type = TB_TYPE_PCIE_UP },
	};
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev;
	int i;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 1, true);

	src_port = &host->ports[8];
	dst_port = &dev->ports[9];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_daisy_chain_walk(struct kunit *test)
{
	/*
	 * Walks from Host DP IN to Device #2 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /
	 *      1 /
	 * [Device #2]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x0, .port = 5, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 13, .type = TB_TYPE_DP_HDMI_OUT },
	};
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev1, *dev2;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);

	src_port = &host->ports[5];
	dst_port = &dev2->ports[13];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_simple_tree_walk(struct kunit *test)
{
	/*
	 * Walks from Host DP IN to Device #3 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x0, .port = 5, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 5, .type = TB_TYPE_PORT },
		{ .route = 0x501, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x501, .port = 13, .type = TB_TYPE_DP_HDMI_OUT },
	};
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev1, *dev3;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_default(test, dev1, 0x501, true);
	alloc_dev_default(test, dev1, 0x701, true);

	src_port = &host->ports[5];
	dst_port = &dev3->ports[13];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_complex_tree_walk(struct kunit *test)
{
	/*
	 * Walks from Device #3 DP IN to Device #9 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #5]
	 *    5 |      | 1         \ 7
	 *    1 |  [Device #4]      \ 1
	 * [Device #3]             [Device #6]
	 *                       3 /
	 *                      1 /
	 *                    [Device #7]
	 *                  3 /      | 5
	 *                 1 /       |
	 *               [Device #8] | 1
	 *                       [Device #9]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x50301, .port = 13, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x50301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 5, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 7, .type = TB_TYPE_PORT },
		{ .route = 0x701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x701, .port = 7, .type = TB_TYPE_PORT },
		{ .route = 0x70701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x70701, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3070701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3070701, .port = 5, .type = TB_TYPE_PORT },
		{ .route = 0x503070701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x503070701, .port = 14, .type = TB_TYPE_DP_HDMI_OUT },
	};
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev5, *dev6, *dev7, *dev9;
	struct tb_port *src_port, *dst_port, *p;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_with_dpin(test, dev2, 0x50301, true);
	alloc_dev_default(test, dev1, 0x501, true);
	dev5 = alloc_dev_default(test, dev1, 0x701, true);
	dev6 = alloc_dev_default(test, dev5, 0x70701, true);
	dev7 = alloc_dev_default(test, dev6, 0x3070701, true);
	alloc_dev_default(test, dev7, 0x303070701, true);
	dev9 = alloc_dev_default(test, dev7, 0x503070701, true);

	src_port = &dev3->ports[13];
	dst_port = &dev9->ports[14];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_max_length_walk(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5, *dev6;
	struct tb_switch *dev7, *dev8, *dev9, *dev10, *dev11, *dev12;
	struct tb_port *src_port, *dst_port, *p;
	int i;

	/*
	 * Walks from Device #6 DP IN to Device #12 DP OUT.
	 *
	 *          [Host]
	 *         1 /  \ 3
	 *        1 /    \ 1
	 * [Device #1]   [Device #7]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #2]   [Device #8]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #3]   [Device #9]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #4]   [Device #10]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #5]   [Device #11]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #6]   [Device #12]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x30303030301, .port = 13, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x30303030301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x303030301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x303030301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3030301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3030301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x30301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x30301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x0, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x30303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x30303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3030303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3030303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x303030303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x303030303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x30303030303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x30303030303, .port = 13, .type = TB_TYPE_DP_HDMI_OUT },
	};

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_default(test, dev2, 0x30301, true);
	dev4 = alloc_dev_default(test, dev3, 0x3030301, true);
	dev5 = alloc_dev_default(test, dev4, 0x303030301, true);
	dev6 = alloc_dev_with_dpin(test, dev5, 0x30303030301, true);
	dev7 = alloc_dev_default(test, host, 0x3, true);
	dev8 = alloc_dev_default(test, dev7, 0x303, true);
	dev9 = alloc_dev_default(test, dev8, 0x30303, true);
	dev10 = alloc_dev_default(test, dev9, 0x3030303, true);
	dev11 = alloc_dev_default(test, dev10, 0x303030303, true);
	dev12 = alloc_dev_default(test, dev11, 0x30303030303, true);

	src_port = &dev6->ports[13];
	dst_port = &dev12->ports[13];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_not_connected(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_port *down, *up;
	struct tb_path *path;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x3, false);
	/* Not connected to anything */
	dev2 = alloc_dev_default(test, NULL, 0x303, false);

	down = &dev1->ports[10];
	up = &dev2->ports[9];

	path = tb_path_alloc(NULL, down, 8, up, 8, 0, "PCIe Down");
	KUNIT_ASSERT_NULL(test, path);
	path = tb_path_alloc(NULL, down, 8, up, 8, 1, "PCIe Down");
	KUNIT_ASSERT_NULL(test, path);
}

struct hop_expectation {
	u64 route;
	u8 in_port;
	enum tb_port_type in_type;
	u8 out_port;
	enum tb_port_type out_type;
};

static void tb_test_path_not_bonded_lane0(struct kunit *test)
{
	/*
	 * PCIe path from host to device using lane 0.
	 *
	 *   [Host]
	 *   3 |: 4
	 *   1 |: 2
	 *  [Device]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 9,
			.in_type = TB_TYPE_PCIE_DOWN,
			.out_port = 3,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x3,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 9,
			.out_type = TB_TYPE_PCIE_UP,
		},
	};
	struct tb_switch *host, *dev;
	struct tb_port *down, *up;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x3, false);

	down = &host->ports[9];
	up = &dev->ports[9];

	path = tb_path_alloc(NULL, down, 8, up, 8, 0, "PCIe Down");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_not_bonded_lane1(struct kunit *test)
{
	/*
	 * DP Video path from host to device using lane 1. Paths like
	 * these are only used with Thunderbolt 1 devices where lane
	 * bonding is not possible. USB4 specifically does not allow
	 * paths like this (you either use lane 0 where lane 1 is
	 * disabled or both lanes are bonded).
	 *
	 *   [Host]
	 *   1 :| 2
	 *   1 :| 2
	 *  [Device]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 5,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 13,
			.out_type = TB_TYPE_DP_HDMI_OUT,
		},
	};
	struct tb_switch *host, *dev;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x1, false);

	in = &host->ports[5];
	out = &dev->ports[13];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_not_bonded_lane1_chain(struct kunit *test)
{
	/*
	 * DP Video path from host to device 3 using lane 1.
	 *
	 *    [Host]
	 *    1 :| 2
	 *    1 :| 2
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 5,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 8,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 6,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x50701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 13,
			.out_type = TB_TYPE_DP_HDMI_OUT,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, false);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_default(test, dev2, 0x50701, false);

	in = &host->ports[5];
	out = &dev3->ports[13];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_not_bonded_lane1_chain_reverse(struct kunit *test)
{
	/*
	 * DP Video path from device 3 to host using lane 1.
	 *
	 *    [Host]
	 *    1 :| 2
	 *    1 :| 2
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x50701,
			.in_port = 13,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 6,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 8,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x0,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 5,
			.out_type = TB_TYPE_DP_HDMI_IN,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, false);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_with_dpin(test, dev2, 0x50701, false);

	in = &dev3->ports[13];
	out = &host->ports[5];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_mixed_chain(struct kunit *test)
{
	/*
	 * DP Video path from host to device 4 where first and last link
	 * is bonded.
	 *
	 *    [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 *    3 |
	 *    1 |
	 *  [Device #4]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 5,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 1,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 8,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 6,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x50701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 3,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x3050701,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 13,
			.out_type = TB_TYPE_DP_HDMI_OUT,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_default(test, dev2, 0x50701, false);
	dev4 = alloc_dev_default(test, dev3, 0x3050701, true);

	in = &host->ports[5];
	out = &dev4->ports[13];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_mixed_chain_reverse(struct kunit *test)
{
	/*
	 * DP Video path from device 4 to host where first and last link
	 * is bonded.
	 *
	 *    [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 *    3 |
	 *    1 |
	 *  [Device #4]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x3050701,
			.in_port = 13,
			.in_type = TB_TYPE_DP_HDMI_OUT,
			.out_port = 1,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x50701,
			.in_port = 3,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 6,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 8,
			.in_type = TB_TYPE_PORT,
			.out_port = 1,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x0,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 5,
			.out_type = TB_TYPE_DP_HDMI_IN,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_default(test, dev2, 0x50701, false);
	dev4 = alloc_dev_default(test, dev3, 0x3050701, true);

	in = &dev4->ports[13];
	out = &host->ports[5];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_tunnel_pcie(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_tunnel *tunnel1, *tunnel2;
	struct tb_port *down, *up;

	/*
	 * Create PCIe tunnel between host and two devices.
	 *
	 *   [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *    5 |
	 *    1 |
	 *  [Device #2]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x501, true);

	down = &host->ports[8];
	up = &dev1->ports[9];
	tunnel1 = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel1);
	KUNIT_EXPECT_EQ(test, tunnel1->type, TB_TUNNEL_PCI);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[1].out_port, down);

	down = &dev1->ports[10];
	up = &dev2->ports[9];
	tunnel2 = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel2);
	KUNIT_EXPECT_EQ(test, tunnel2->type, TB_TUNNEL_PCI);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[1].out_port, down);

	tb_tunnel_put(tunnel2);
	tb_tunnel_put(tunnel1);
}

static void tb_test_dp_tunnel_active(struct tb_tunnel *tunnel, void *data)
{
}

static void tb_test_tunnel_dp(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Create DP tunnel between Host and Device
	 *
	 *   [Host]
	 *   1 |
	 *   1 |
	 *  [Device]
	 */
	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x3, true);

	in = &host->ports[5];
	out = &dev->ports[13];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0,
				    tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[1].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[1].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[1].out_port, in);
	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dp_chain(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev4;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Create DP tunnel from Host DP IN to Device #4 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	alloc_dev_default(test, dev1, 0x301, true);
	alloc_dev_default(test, dev1, 0x501, true);
	dev4 = alloc_dev_default(test, dev1, 0x701, true);

	in = &host->ports[5];
	out = &dev4->ports[14];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0,
				    tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[2].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[2].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[2].out_port, in);
	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dp_tree(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev5;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Create DP tunnel from Device #2 DP IN to Device #5 DP OUT.
	 *
	 *          [Host]
	 *           3 |
	 *           1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 *             | 5
	 *             | 1
	 *         [Device #5]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x3, true);
	dev2 = alloc_dev_with_dpin(test, dev1, 0x303, true);
	dev3 = alloc_dev_default(test, dev1, 0x503, true);
	alloc_dev_default(test, dev1, 0x703, true);
	dev5 = alloc_dev_default(test, dev3, 0x50503, true);

	in = &dev2->ports[13];
	out = &dev5->ports[13];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0,
				    tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 4);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[3].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 4);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[3].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 4);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[3].out_port, in);
	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dp_max_length(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5, *dev6;
	struct tb_switch *dev7, *dev8, *dev9, *dev10, *dev11, *dev12;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Creates DP tunnel from Device #6 to Device #12.
	 *
	 *          [Host]
	 *         1 /  \ 3
	 *        1 /    \ 1
	 * [Device #1]   [Device #7]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #2]   [Device #8]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #3]   [Device #9]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #4]   [Device #10]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #5]   [Device #11]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #6]   [Device #12]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_default(test, dev2, 0x30301, true);
	dev4 = alloc_dev_default(test, dev3, 0x3030301, true);
	dev5 = alloc_dev_default(test, dev4, 0x303030301, true);
	dev6 = alloc_dev_with_dpin(test, dev5, 0x30303030301, true);
	dev7 = alloc_dev_default(test, host, 0x3, true);
	dev8 = alloc_dev_default(test, dev7, 0x303, true);
	dev9 = alloc_dev_default(test, dev8, 0x30303, true);
	dev10 = alloc_dev_default(test, dev9, 0x3030303, true);
	dev11 = alloc_dev_default(test, dev10, 0x303030303, true);
	dev12 = alloc_dev_default(test, dev11, 0x30303030303, true);

	in = &dev6->ports[13];
	out = &dev12->ports[13];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0,
				    tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 13);
	/* First hop */
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	/* Middle */
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[6].in_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[6].out_port,
			    &host->ports[3]);
	/* Last */
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[12].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 13);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[6].in_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[6].out_port,
			    &host->ports[3]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[12].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 13);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[6].in_port,
			    &host->ports[3]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[6].out_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[12].out_port, in);
	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_3dp(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5;
	struct tb_port *in1, *in2, *in3, *out1, *out2, *out3;
	struct tb_tunnel *tunnel1, *tunnel2, *tunnel3;

	/*
	 * Create 3 DP tunnels from Host to Devices #2, #5 and #4.
	 *
	 *          [Host]
	 *           3 |
	 *           1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 *             | 5
	 *             | 1
	 *         [Device #5]
	 */
	host = alloc_host_br(test);
	dev1 = alloc_dev_default(test, host, 0x3, true);
	dev2 = alloc_dev_default(test, dev1, 0x303, true);
	dev3 = alloc_dev_default(test, dev1, 0x503, true);
	dev4 = alloc_dev_default(test, dev1, 0x703, true);
	dev5 = alloc_dev_default(test, dev3, 0x50503, true);

	in1 = &host->ports[5];
	in2 = &host->ports[6];
	in3 = &host->ports[10];

	out1 = &dev2->ports[13];
	out2 = &dev5->ports[13];
	out3 = &dev4->ports[14];

	tunnel1 = tb_tunnel_alloc_dp(NULL, in1, out1, 1, 0, 0,
				     tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_TRUE(test, tunnel1 != NULL);
	KUNIT_EXPECT_EQ(test, tunnel1->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->src_port, in1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->dst_port, out1);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[0]->path_length, 3);

	tunnel2 = tb_tunnel_alloc_dp(NULL, in2, out2, 1, 0, 0,
				     tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_TRUE(test, tunnel2 != NULL);
	KUNIT_EXPECT_EQ(test, tunnel2->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->src_port, in2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->dst_port, out2);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[0]->path_length, 4);

	tunnel3 = tb_tunnel_alloc_dp(NULL, in3, out3, 1, 0, 0,
				     tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_TRUE(test, tunnel3 != NULL);
	KUNIT_EXPECT_EQ(test, tunnel3->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel3->src_port, in3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel3->dst_port, out3);
	KUNIT_ASSERT_EQ(test, tunnel3->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel3->paths[0]->path_length, 3);

	tb_tunnel_put(tunnel3);
	tb_tunnel_put(tunnel2);
	tb_tunnel_put(tunnel1);
}

static void tb_test_tunnel_usb3(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_tunnel *tunnel1, *tunnel2;
	struct tb_port *down, *up;

	/*
	 * Create USB3 tunnel between host and two devices.
	 *
	 *   [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *          \ 7
	 *           \ 1
	 *         [Device #2]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, true);

	down = &host->ports[12];
	up = &dev1->ports[16];
	tunnel1 = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel1);
	KUNIT_EXPECT_EQ(test, tunnel1->type, TB_TUNNEL_USB3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[1].out_port, down);

	down = &dev1->ports[17];
	up = &dev2->ports[16];
	tunnel2 = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel2);
	KUNIT_EXPECT_EQ(test, tunnel2->type, TB_TUNNEL_USB3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[1].out_port, down);

	tb_tunnel_put(tunnel2);
	tb_tunnel_put(tunnel1);
}

static void tb_test_tunnel_port_on_path(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5;
	struct tb_port *in, *out, *port;
	struct tb_tunnel *dp_tunnel;

	/*
	 *          [Host]
	 *           3 |
	 *           1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 *             | 5
	 *             | 1
	 *         [Device #5]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x3, true);
	dev2 = alloc_dev_with_dpin(test, dev1, 0x303, true);
	dev3 = alloc_dev_default(test, dev1, 0x503, true);
	dev4 = alloc_dev_default(test, dev1, 0x703, true);
	dev5 = alloc_dev_default(test, dev3, 0x50503, true);

	in = &dev2->ports[13];
	out = &dev5->ports[13];

	dp_tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0,
				       tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_NOT_NULL(test, dp_tunnel);

	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, in));
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, out));

	port = &host->ports[8];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &host->ports[3];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[1];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[3];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[5];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[7];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev3->ports[1];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev5->ports[1];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev4->ports[1];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	tb_tunnel_put(dp_tunnel);
}

static void tb_test_tunnel_dma(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	/*
	 * Create DMA tunnel from NHI to port 1 and back.
	 *
	 *   [Host 1]
	 *    1 ^ In HopID 1 -> Out HopID 8
	 *      |
	 *      v In HopID 8 -> Out HopID 1
	 * ............ Domain border
	 *      |
	 *   [Host 2]
	 */
	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 2);
	/* RX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 8);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].next_hop_index, 1);
	/* TX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[0].in_hop_index, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].out_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[0].next_hop_index, 8);

	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dma_rx(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	/*
	 * Create DMA RX tunnel from port 1 to NHI.
	 *
	 *   [Host 1]
	 *    1 ^
	 *      |
	 *      | In HopID 15 -> Out HopID 2
	 * ............ Domain border
	 *      |
	 *   [Host 2]
	 */
	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, -1, -1, 15, 2);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 1);
	/* RX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 15);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].next_hop_index, 2);

	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dma_tx(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	/*
	 * Create DMA TX tunnel from NHI to port 1.
	 *
	 *   [Host 1]
	 *    1 | In HopID 2 -> Out HopID 15
	 *      |
	 *      v
	 * ............ Domain border
	 *      |
	 *   [Host 2]
	 */
	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 15, 2, -1, -1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 1);
	/* TX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].next_hop_index, 15);

	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dma_chain(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;

	/*
	 * Create DMA tunnel from NHI to Device #2 port 3 and back.
	 *
	 *   [Host 1]
	 *    1 ^ In HopID 1 -> Out HopID x
	 *      |
	 *    1 | In HopID x -> Out HopID 1
	 *  [Device #1]
	 *         7 \
	 *          1 \
	 *         [Device #2]
	 *           3 | In HopID x -> Out HopID 8
	 *             |
	 *             v In HopID 8 -> Out HopID x
	 * ............ Domain border
	 *             |
	 *          [Host 2]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, true);

	nhi = &host->ports[7];
	port = &dev2->ports[3];
	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 2);
	/* RX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 8);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port,
			    &dev2->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[1].in_port,
			    &dev1->ports[7]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[1].out_port,
			    &dev1->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[2].in_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[2].out_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[2].next_hop_index, 1);
	/* TX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[0].in_hop_index, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[1].in_port,
			    &dev1->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[1].out_port,
			    &dev1->ports[7]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[2].in_port,
			    &dev2->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[2].out_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[2].next_hop_index, 8);

	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dma_match(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 15, 1, 15, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);

	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, 1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 8, 1, 15, 1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, -1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, 1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, -1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 8, -1, 8, -1));

	tb_tunnel_put(tunnel);

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 15, 1, -1, -1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, -1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, -1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 15, 1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 15, 11, -1, -1));

	tb_tunnel_put(tunnel);

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, -1, -1, 15, 11);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 11));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, 11));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, -1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, -1, -1, 10, 11));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 15, 11, -1, -1));

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_legacy_not_bonded(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x1, false);

	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 16U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 16U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_legacy_bonded(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x1, true);

	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_pcie(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 64U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_without_dp(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_without_dp(test, host, 0x1, true);

	/*
	 * The device has no DP therefore baMinDPmain = baMinDPaux = 0
	 *
	 * Create PCIe path with buffers less than baMaxPCIe.
	 *
	 * For a device with buffers configurations:
	 * baMaxUSB3 = 109
	 * baMinDPaux = 0
	 * baMinDPmain = 0
	 * baMaxPCIe = 30
	 * baMaxHI = 1
	 * Remaining Buffers = Total - (CP + DP) = 120 - (2 + 0) = 118
	 * PCIe Credits = Max(6, Min(baMaxPCIe, Remaining Buffers - baMaxUSB3)
	 *		= Max(6, Min(30, 9) = 9
	 */
	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_TRUE(test, tunnel != NULL);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	/* PCIe downstream path */
	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 9U);

	/* PCIe upstream path */
	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 64U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_dp(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	in = &host->ports[5];
	out = &dev->ports[14];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0,
				    tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)3);

	/* Video (main) path */
	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 12U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 18U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 0U);

	/* AUX TX */
	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	/* AUX RX */
	path = tunnel->paths[2];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_usb3(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	down = &host->ports[12];
	up = &dev->ports[16];
	tunnel = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_dma(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	nhi = &host->ports[7];
	port = &dev->ports[3];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	/* DMA RX */
	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	/* DMA TX */
	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_dma_multiple(struct kunit *test)
{
	struct tb_tunnel *tunnel1, *tunnel2, *tunnel3;
	struct tb_switch *host, *dev;
	struct tb_port *nhi, *port;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	nhi = &host->ports[7];
	port = &dev->ports[3];

	/*
	 * Create three DMA tunnels through the same ports. With the
	 * default buffers we should be able to create two and the last
	 * one fails.
	 *
	 * For default host we have following buffers for DMA:
	 *
	 *   120 - (2 + 2 * (1 + 0) + 32 + 64 + spare) = 20
	 *
	 * For device we have following:
	 *
	 *  120 - (2 + 2 * (1 + 18) + 14 + 32 + spare) = 34
	 *
	 * spare = 14 + 1 = 15
	 *
	 * So on host the first tunnel gets 14 and the second gets the
	 * remaining 1 and then we run out of buffers.
	 */
	tunnel1 = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel1);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, (size_t)2);

	path = tunnel1->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = tunnel1->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	tunnel2 = tb_tunnel_alloc_dma(NULL, nhi, port, 9, 2, 9, 2);
	KUNIT_ASSERT_NOT_NULL(test, tunnel2);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, (size_t)2);

	path = tunnel2->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = tunnel2->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	tunnel3 = tb_tunnel_alloc_dma(NULL, nhi, port, 10, 3, 10, 3);
	KUNIT_ASSERT_NULL(test, tunnel3);

	/*
	 * Release the first DMA tunnel. That should make 14 buffers
	 * available for the next tunnel.
	 */
	tb_tunnel_put(tunnel1);

	tunnel3 = tb_tunnel_alloc_dma(NULL, nhi, port, 10, 3, 10, 3);
	KUNIT_ASSERT_NOT_NULL(test, tunnel3);

	path = tunnel3->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = tunnel3->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	tb_tunnel_put(tunnel3);
	tb_tunnel_put(tunnel2);
}

static struct tb_tunnel *TB_TEST_PCIE_TUNNEL(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *up, *down;
	struct tb_tunnel *pcie_tunnel;
	struct tb_path *path;

	down = &host->ports[8];
	up = &dev->ports[9];
	pcie_tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, pcie_tunnel);
	KUNIT_ASSERT_EQ(test, pcie_tunnel->npaths, (size_t)2);

	path = pcie_tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	path = pcie_tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 64U);

	return pcie_tunnel;
}

static struct tb_tunnel *TB_TEST_DP_TUNNEL1(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *in, *out;
	struct tb_tunnel *dp_tunnel1;
	struct tb_path *path;

	in = &host->ports[5];
	out = &dev->ports[13];
	dp_tunnel1 = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0,
					tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_NOT_NULL(test, dp_tunnel1);
	KUNIT_ASSERT_EQ(test, dp_tunnel1->npaths, (size_t)3);

	path = dp_tunnel1->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 12U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 18U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 0U);

	path = dp_tunnel1->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = dp_tunnel1->paths[2];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	return dp_tunnel1;
}

static struct tb_tunnel *TB_TEST_DP_TUNNEL2(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *in, *out;
	struct tb_tunnel *dp_tunnel2;
	struct tb_path *path;

	in = &host->ports[6];
	out = &dev->ports[14];
	dp_tunnel2 = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0,
					tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_NOT_NULL(test, dp_tunnel2);
	KUNIT_ASSERT_EQ(test, dp_tunnel2->npaths, (size_t)3);

	path = dp_tunnel2->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 12U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 18U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 0U);

	path = dp_tunnel2->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = dp_tunnel2->paths[2];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	return dp_tunnel2;
}

static struct tb_tunnel *TB_TEST_USB3_TUNNEL(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *up, *down;
	struct tb_tunnel *usb3_tunnel;
	struct tb_path *path;

	down = &host->ports[12];
	up = &dev->ports[16];
	usb3_tunnel = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, usb3_tunnel);
	KUNIT_ASSERT_EQ(test, usb3_tunnel->npaths, (size_t)2);

	path = usb3_tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = usb3_tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	return usb3_tunnel;
}

static struct tb_tunnel *TB_TEST_DMA_TUNNEL1(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *dma_tunnel1;
	struct tb_path *path;

	nhi = &host->ports[7];
	port = &dev->ports[3];
	dma_tunnel1 = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma_tunnel1);
	KUNIT_ASSERT_EQ(test, dma_tunnel1->npaths, (size_t)2);

	path = dma_tunnel1->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = dma_tunnel1->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	return dma_tunnel1;
}

static struct tb_tunnel *TB_TEST_DMA_TUNNEL2(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *dma_tunnel2;
	struct tb_path *path;

	nhi = &host->ports[7];
	port = &dev->ports[3];
	dma_tunnel2 = tb_tunnel_alloc_dma(NULL, nhi, port, 9, 2, 9, 2);
	KUNIT_ASSERT_NOT_NULL(test, dma_tunnel2);
	KUNIT_ASSERT_EQ(test, dma_tunnel2->npaths, (size_t)2);

	path = dma_tunnel2->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = dma_tunnel2->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	return dma_tunnel2;
}

static void tb_test_credit_alloc_all(struct kunit *test)
{
	struct tb_tunnel *pcie_tunnel, *dp_tunnel1, *dp_tunnel2, *usb3_tunnel;
	struct tb_tunnel *dma_tunnel1, *dma_tunnel2;
	struct tb_switch *host, *dev;

	/*
	 * Create PCIe, 2 x DP, USB 3.x and two DMA tunnels from host to
	 * device. Expectation is that all these can be established with
	 * the default credit allocation found in Intel hardware.
	 */

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	pcie_tunnel = TB_TEST_PCIE_TUNNEL(test, host, dev);
	dp_tunnel1 = TB_TEST_DP_TUNNEL1(test, host, dev);
	dp_tunnel2 = TB_TEST_DP_TUNNEL2(test, host, dev);
	usb3_tunnel = TB_TEST_USB3_TUNNEL(test, host, dev);
	dma_tunnel1 = TB_TEST_DMA_TUNNEL1(test, host, dev);
	dma_tunnel2 = TB_TEST_DMA_TUNNEL2(test, host, dev);

	tb_tunnel_put(dma_tunnel2);
	tb_tunnel_put(dma_tunnel1);
	tb_tunnel_put(usb3_tunnel);
	tb_tunnel_put(dp_tunnel2);
	tb_tunnel_put(dp_tunnel1);
	tb_tunnel_put(pcie_tunnel);
}

static const u32 root_directory[] = {
	0x55584401,	/* "UXD" v1 */
	0x00000018,	/* Root directory length */
	0x76656e64,	/* "vend" */
	0x6f726964,	/* "orid" */
	0x76000001,	/* "v" R 1 */
	0x00000a27,	/* Immediate value, ! Vendor ID */
	0x76656e64,	/* "vend" */
	0x6f726964,	/* "orid" */
	0x74000003,	/* "t" R 3 */
	0x0000001a,	/* Text leaf offset, (“Apple Inc.”) */
	0x64657669,	/* "devi" */
	0x63656964,	/* "ceid" */
	0x76000001,	/* "v" R 1 */
	0x0000000a,	/* Immediate value, ! Device ID */
	0x64657669,	/* "devi" */
	0x63656964,	/* "ceid" */
	0x74000003,	/* "t" R 3 */
	0x0000001d,	/* Text leaf offset, (“Macintosh”) */
	0x64657669,	/* "devi" */
	0x63657276,	/* "cerv" */
	0x76000001,	/* "v" R 1 */
	0x80000100,	/* Immediate value, Device Revision */
	0x6e657477,	/* "netw" */
	0x6f726b00,	/* "ork" */
	0x44000014,	/* "D" R 20 */
	0x00000021,	/* Directory data offset, (Network Directory) */
	0x4170706c,	/* "Appl" */
	0x6520496e,	/* "e In" */
	0x632e0000,	/* "c." ! */
	0x4d616369,	/* "Maci" */
	0x6e746f73,	/* "ntos" */
	0x68000000,	/* "h" */
	0x00000000,	/* padding */
	0xca8961c6,	/* Directory UUID, Network Directory */
	0x9541ce1c,	/* Directory UUID, Network Directory */
	0x5949b8bd,	/* Directory UUID, Network Directory */
	0x4f5a5f2e,	/* Directory UUID, Network Directory */
	0x70727463,	/* "prtc" */
	0x69640000,	/* "id" */
	0x76000001,	/* "v" R 1 */
	0x00000001,	/* Immediate value, Network Protocol ID */
	0x70727463,	/* "prtc" */
	0x76657273,	/* "vers" */
	0x76000001,	/* "v" R 1 */
	0x00000001,	/* Immediate value, Network Protocol Version */
	0x70727463,	/* "prtc" */
	0x72657673,	/* "revs" */
	0x76000001,	/* "v" R 1 */
	0x00000001,	/* Immediate value, Network Protocol Revision */
	0x70727463,	/* "prtc" */
	0x73746e73,	/* "stns" */
	0x76000001,	/* "v" R 1 */
	0x00000000,	/* Immediate value, Network Protocol Settings */
};

static const uuid_t network_dir_uuid =
	UUID_INIT(0xc66189ca, 0x1cce, 0x4195,
		  0xbd, 0xb8, 0x49, 0x59, 0x2e, 0x5f, 0x5a, 0x4f);

static void tb_test_property_parse(struct kunit *test)
{
	struct tb_property_dir *dir, *network_dir;
	struct tb_property *p;

	dir = tb_property_parse_dir(root_directory, ARRAY_SIZE(root_directory));
	KUNIT_ASSERT_NOT_NULL(test, dir);

	p = tb_property_find(dir, "foo", TB_PROPERTY_TYPE_TEXT);
	KUNIT_ASSERT_NULL(test, p);

	p = tb_property_find(dir, "vendorid", TB_PROPERTY_TYPE_TEXT);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_STREQ(test, p->value.text, "Apple Inc.");

	p = tb_property_find(dir, "vendorid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0xa27);

	p = tb_property_find(dir, "deviceid", TB_PROPERTY_TYPE_TEXT);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_STREQ(test, p->value.text, "Macintosh");

	p = tb_property_find(dir, "deviceid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0xa);

	p = tb_property_find(dir, "missing", TB_PROPERTY_TYPE_DIRECTORY);
	KUNIT_ASSERT_NULL(test, p);

	p = tb_property_find(dir, "network", TB_PROPERTY_TYPE_DIRECTORY);
	KUNIT_ASSERT_NOT_NULL(test, p);

	network_dir = p->value.dir;
	KUNIT_EXPECT_TRUE(test, uuid_equal(network_dir->uuid, &network_dir_uuid));

	p = tb_property_find(network_dir, "prtcid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x1);

	p = tb_property_find(network_dir, "prtcvers", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x1);

	p = tb_property_find(network_dir, "prtcrevs", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x1);

	p = tb_property_find(network_dir, "prtcstns", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x0);

	p = tb_property_find(network_dir, "deviceid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_EXPECT_TRUE(test, !p);
	p = tb_property_find(network_dir, "deviceid", TB_PROPERTY_TYPE_TEXT);
	KUNIT_EXPECT_TRUE(test, !p);

	tb_property_free_dir(dir);
}

static void tb_test_property_format(struct kunit *test)
{
	struct tb_property_dir *dir;
	ssize_t block_len;
	u32 *block;
	int ret, i;

	dir = tb_property_parse_dir(root_directory, ARRAY_SIZE(root_directory));
	KUNIT_ASSERT_NOT_NULL(test, dir);

	ret = tb_property_format_dir(dir, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, ARRAY_SIZE(root_directory));

	block_len = ret;

	block = kunit_kzalloc(test, block_len * sizeof(u32), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, block);

	ret = tb_property_format_dir(dir, block, block_len);
	KUNIT_EXPECT_EQ(test, ret, 0);

	for (i = 0; i < ARRAY_SIZE(root_directory); i++)
		KUNIT_EXPECT_EQ(test, root_directory[i], block[i]);

	tb_property_free_dir(dir);
}

static void compare_dirs(struct kunit *test, struct tb_property_dir *d1,
			 struct tb_property_dir *d2)
{
	struct tb_property *p1, *p2, *tmp;
	int n1, n2, i;

	if (d1->uuid) {
		KUNIT_ASSERT_NOT_NULL(test, d2->uuid);
		KUNIT_ASSERT_TRUE(test, uuid_equal(d1->uuid, d2->uuid));
	} else {
		KUNIT_ASSERT_NULL(test, d2->uuid);
	}

	n1 = 0;
	tb_property_for_each(d1, tmp)
		n1++;
	KUNIT_ASSERT_NE(test, n1, 0);

	n2 = 0;
	tb_property_for_each(d2, tmp)
		n2++;
	KUNIT_ASSERT_NE(test, n2, 0);

	KUNIT_ASSERT_EQ(test, n1, n2);

	p1 = NULL;
	p2 = NULL;
	for (i = 0; i < n1; i++) {
		p1 = tb_property_get_next(d1, p1);
		KUNIT_ASSERT_NOT_NULL(test, p1);
		p2 = tb_property_get_next(d2, p2);
		KUNIT_ASSERT_NOT_NULL(test, p2);

		KUNIT_ASSERT_STREQ(test, &p1->key[0], &p2->key[0]);
		KUNIT_ASSERT_EQ(test, p1->type, p2->type);
		KUNIT_ASSERT_EQ(test, p1->length, p2->length);

		switch (p1->type) {
		case TB_PROPERTY_TYPE_DIRECTORY:
			KUNIT_ASSERT_NOT_NULL(test, p1->value.dir);
			KUNIT_ASSERT_NOT_NULL(test, p2->value.dir);
			compare_dirs(test, p1->value.dir, p2->value.dir);
			break;

		case TB_PROPERTY_TYPE_DATA:
			KUNIT_ASSERT_NOT_NULL(test, p1->value.data);
			KUNIT_ASSERT_NOT_NULL(test, p2->value.data);
			KUNIT_ASSERT_TRUE(test,
				!memcmp(p1->value.data, p2->value.data,
					p1->length * 4)
			);
			break;

		case TB_PROPERTY_TYPE_TEXT:
			KUNIT_ASSERT_NOT_NULL(test, p1->value.text);
			KUNIT_ASSERT_NOT_NULL(test, p2->value.text);
			KUNIT_ASSERT_STREQ(test, p1->value.text, p2->value.text);
			break;

		case TB_PROPERTY_TYPE_VALUE:
			KUNIT_ASSERT_EQ(test, p1->value.immediate,
					p2->value.immediate);
			break;
		default:
			KUNIT_FAIL(test, "unexpected property type");
			break;
		}
	}
}

static void tb_test_property_copy(struct kunit *test)
{
	struct tb_property_dir *src, *dst;
	u32 *block;
	int ret, i;

	src = tb_property_parse_dir(root_directory, ARRAY_SIZE(root_directory));
	KUNIT_ASSERT_NOT_NULL(test, src);

	dst = tb_property_copy_dir(src);
	KUNIT_ASSERT_NOT_NULL(test, dst);

	/* Compare the structures */
	compare_dirs(test, src, dst);

	/* Compare the resulting property block */
	ret = tb_property_format_dir(dst, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, ARRAY_SIZE(root_directory));

	block = kunit_kzalloc(test, sizeof(root_directory), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, block);

	ret = tb_property_format_dir(dst, block, ARRAY_SIZE(root_directory));
	KUNIT_EXPECT_TRUE(test, !ret);

	for (i = 0; i < ARRAY_SIZE(root_directory); i++)
		KUNIT_EXPECT_EQ(test, root_directory[i], block[i]);

	tb_property_free_dir(dst);
	tb_property_free_dir(src);
}

/*
 * Reproducers for three memory-safety defects in
 * drivers/thunderbolt/property.c reached from a crafted XDomain
 * PROPERTIES_RESPONSE payload.  Without the fix these trip KASAN or
 * smash the kernel stack; with the fix each returns NULL cleanly.
 *
 * The on-wire entry layout matches struct tb_property_entry in
 * property.c (private to that translation unit): u32 key_hi, u32
 * key_lo, then a packed u32 = (type << 24) | (reserved << 16) |
 * length, then u32 value.  Each entry is 4 dwords.
 */

static void tb_test_property_parse_u32_wrap(struct kunit *test)
{
	/*
	 * 0x102 dwords: enough for the entry's length field (0x100) to
	 * pass the "entry->length > block_len" gate so the wrap check
	 * is actually exercised.  parse_dwdata's downstream OOB read
	 * lands ~16 GiB past the allocation regardless.
	 */
	u32 *block = kunit_kzalloc(test, 0x102 * sizeof(u32), GFP_KERNEL);
	struct tb_property_dir *dir;

	KUNIT_ASSERT_NOT_NULL(test, block);

	block[0] = 0x55584401;	/* "UXD" v1 magic */
	block[1] = 0x00000004;	/* Root directory length: one entry */

	/*
	 * DATA entry whose value 0xffffff00 + length 0x100 wrap to 0
	 * in u32, passing the sum <= block_len guard even though the
	 * real offset is far past the allocation.
	 */
	block[2] = 0x61616161;	/* key_hi */
	block[3] = 0x61616161;	/* key_lo */
	block[4] = 0x64000100;	/* type=DATA, reserved=0, length=0x100 */
	block[5] = 0xffffff00;	/* value */

	dir = tb_property_parse_dir(block, 0x102);
	KUNIT_EXPECT_NULL(test, dir);
	tb_property_free_dir(dir);
}

static void tb_test_property_parse_recursion(struct kunit *test)
{
	/*
	 * 10 dwords: rootdir header (2) + parent DIRECTORY entry (4) +
	 * the child entry that lives at dir_offset(2) + UUID(4) = 6,
	 * occupying block[6..9].  Each recursive level re-reads the
	 * same block[6..9] as its first child entry, which is itself
	 * a DIRECTORY pointing at offset 2.
	 */
	u32 *block = kunit_kzalloc(test, 10 * sizeof(u32), GFP_KERNEL);
	struct tb_property_dir *dir;

	KUNIT_ASSERT_NOT_NULL(test, block);

	block[0] = 0x55584401;	/* "UXD" v1 magic */
	block[1] = 0x00000004;	/* Root directory length: one entry */

	/*
	 * DIRECTORY entry pointing at dir_offset = 2 with length = 8.
	 * Non-root parse derives content_offset = 6, content_len = 4,
	 * nentries = 1.  block[6..9] is read both as the parent's UUID
	 * (kmemdup'd into dir->uuid) and as the single child entry --
	 * which is itself a DIRECTORY pointing at offset 2, so the
	 * recursion never terminates and the kernel stack is exhausted.
	 */
	block[2] = 0x61616161;	/* key_hi */
	block[3] = 0x61616161;	/* key_lo */
	block[4] = 0x44000008;	/* type=DIRECTORY, reserved=0, length=8 */
	block[5] = 0x00000002;	/* value = dir_offset */

	block[6] = 0x62626262;	/* doubles as UUID dword 0 / child key_hi */
	block[7] = 0x62626262;	/* doubles as UUID dword 1 / child key_lo */
	block[8] = 0x44000008;	/* type=DIRECTORY, reserved=0, length=8 */
	block[9] = 0x00000002;	/* value = dir_offset (back at parent) */

	dir = tb_property_parse_dir(block, 10);
	KUNIT_EXPECT_NULL(test, dir);
	tb_property_free_dir(dir);
}

static void tb_test_property_parse_dir_len_underflow(struct kunit *test)
{
	/*
	 * Allocate exactly 7 dwords (28 bytes) so the kmalloc-32 chunk
	 * leaves a 4-byte slab redzone tail that KASAN-Generic can flag.
	 * With block_len = 7, dir_offset = 4, dir_len = 3, the non-root
	 * UUID kmemdup reads 16 bytes from byte 16, so bytes 28..31 fall
	 * in the redzone and trip a KASAN slab-out-of-bounds report on
	 * the pre-fix kernel.  Sizing the buffer at a power of two (32,
	 * 64, ...) puts the over-read into the slab cache tail where
	 * KASAN's generic shadow does not flag it, and the test reduces
	 * to the downstream content_len = dir_len - 4 underflow path
	 * which also returns NULL.
	 */
	u32 *block = kunit_kzalloc(test, 7 * sizeof(u32), GFP_KERNEL);
	struct tb_property_dir *dir;

	KUNIT_ASSERT_NOT_NULL(test, block);

	block[0] = 0x55584401;	/* "UXD" v1 magic */
	block[1] = 0x00000004;	/* Root directory length: one entry */

	/*
	 * DIRECTORY entry with length = 3 pointing at dir_offset = 4.
	 * tb_property_entry_valid() permits value(4) + length(3) <=
	 * block_len(7).  Non-root parse begins with a kmemdup of 4
	 * dwords from dir_offset for the UUID; that read runs past the
	 * 28-byte allocation before the dir_len < 4 reject would fire.
	 */
	block[2] = 0x61616161;	/* key_hi */
	block[3] = 0x61616161;	/* key_lo */
	block[4] = 0x44000003;	/* type=DIRECTORY, reserved=0, length=3 */
	block[5] = 0x00000004;	/* value = dir_offset */
	/* block[6] is the start of the four UUID dwords; block[7..] is OOB. */

	dir = tb_property_parse_dir(block, 7);
	KUNIT_EXPECT_NULL(test, dir);
	tb_property_free_dir(dir);
}

static void tb_test_property_parse_zero_length(struct kunit *test)
{
	u32 *block = kunit_kzalloc(test, 6 * sizeof(u32), GFP_KERNEL);
	struct tb_property_dir *dir;

	KUNIT_ASSERT_NOT_NULL(test, block);

	block[0] = 0x55584401;	/* rootdir magic */
	block[1] = 0x00000004;	/* root length: one entry */

	block[2] = 0x61616161;	/* key_hi */
	block[3] = 0x61616161;	/* key_lo */
	block[4] = 0x74000000;	/* type=TEXT, reserved=0, length=0 */
	block[5] = 0x00000000;	/* value */

	dir = tb_property_parse_dir(block, 6);
	KUNIT_EXPECT_NULL(test, dir);
	tb_property_free_dir(dir);
}

static void tb_test_property_parse_rootdir_overflow(struct kunit *test)
{
	u32 *block = kunit_kzalloc(test, 4 * sizeof(u32), GFP_KERNEL);
	struct tb_property_dir *dir;

	KUNIT_ASSERT_NOT_NULL(test, block);

	block[0] = 0x55584401;	/* rootdir magic */
	block[1] = 0x00000004;	/* root length claims 4 dwords of content */
	block[2] = 0x61616161;
	block[3] = 0x61616161;

	/* content_offset(2) + content_len(4) = 6 > block_len(4) */
	dir = tb_property_parse_dir(block, 4);
	KUNIT_EXPECT_NULL(test, dir);
	tb_property_free_dir(dir);
}

static void tb_test_property_merge(struct kunit *test)
{
	struct tb_property_dir *dir1, *dir2, *dir3;
	struct tb_property *p;
	uuid_t uuid;
	int ret;

	dir1 = tb_property_create_dir(&network_dir_uuid);
	KUNIT_ASSERT_NOT_NULL(test, dir1);
	ret = tb_property_add_immediate(dir1, "prtcid", 1);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = tb_property_add_immediate(dir1, "prtcvers", 1);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = tb_property_add_immediate(dir1, "prtcrevs", 0);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = tb_property_add_immediate(dir1, "prtcstns", 0);
	KUNIT_EXPECT_EQ(test, ret, 0);

	dir2 = tb_property_create_dir(&network_dir_uuid);
	KUNIT_ASSERT_NOT_NULL(test, dir2);
	ret = tb_property_add_text(dir2, "descr", "This is text");
	KUNIT_EXPECT_EQ(test, ret, 0);
	/* This replaces the value in dir1 */
	ret = tb_property_add_immediate(dir2, "prtcvers", 0x1234);
	KUNIT_EXPECT_EQ(test, ret, 0);

	uuid_gen(&uuid);
	dir3 = tb_property_create_dir(&uuid);
	KUNIT_ASSERT_NOT_NULL(test, dir3);
	ret = tb_property_add_immediate(dir3, "value0", 0);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = tb_property_add_text(dir3, "value1", "Text value");
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = tb_property_add_dir(dir2, "my", dir3);
	KUNIT_EXPECT_EQ(test, ret, 0);

	ret = tb_property_merge_dir(dir1, dir2, true);
	KUNIT_EXPECT_EQ(test, ret, 0);

	p = tb_property_get_next(dir1, NULL);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_STREQ(test, &p->key[0], "prtcid");
	KUNIT_ASSERT_EQ(test, p->type, TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_EQ(test, p->length, 1);
	KUNIT_ASSERT_EQ(test, p->value.immediate, 1);
	p = tb_property_get_next(dir1, p);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_STREQ(test, &p->key[0], "prtcvers");
	KUNIT_ASSERT_EQ(test, p->type, TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_EQ(test, p->length, 1);
	KUNIT_ASSERT_EQ(test, p->value.immediate, 0x1234);
	p = tb_property_get_next(dir1, p);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_STREQ(test, &p->key[0], "prtcrevs");
	KUNIT_ASSERT_EQ(test, p->type, TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_EQ(test, p->length, 1);
	KUNIT_ASSERT_EQ(test, p->value.immediate, 0);
	p = tb_property_get_next(dir1, p);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_STREQ(test, &p->key[0], "prtcstns");
	KUNIT_ASSERT_EQ(test, p->type, TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_EQ(test, p->length, 1);
	KUNIT_ASSERT_EQ(test, p->value.immediate, 0);
	p = tb_property_get_next(dir1, p);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_STREQ(test, &p->key[0], "descr");
	KUNIT_ASSERT_EQ(test, p->type, TB_PROPERTY_TYPE_TEXT);
	KUNIT_ASSERT_EQ(test, p->length, 4);
	KUNIT_ASSERT_STREQ(test, p->value.text, "This is text");
	p = tb_property_get_next(dir1, p);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_STREQ(test, &p->key[0], "my");
	KUNIT_ASSERT_EQ(test, p->type, TB_PROPERTY_TYPE_DIRECTORY);
	compare_dirs(test, p->value.dir, dir3);
	p = tb_property_get_next(dir1, p);
	KUNIT_ASSERT_NULL(test, p);

	tb_property_free_dir(dir2);
	tb_property_free_dir(dir1);
}

struct tb_test_pci_host {
	struct tb_nhi nhi;
	unsigned int prepares;
	unsigned int connects;
	unsigned int disconnects;
	int result;
};

static int tb_test_pci_host_prepare(struct tb_nhi *nhi)
{
	struct tb_test_pci_host *host = container_of(nhi, struct tb_test_pci_host, nhi);

	host->prepares++;
	return host->result;
}

static int tb_test_pci_host_connect(struct tb_nhi *nhi)
{
	struct tb_test_pci_host *host = container_of(nhi, struct tb_test_pci_host, nhi);

	host->connects++;
	return host->result;
}

static int tb_test_pci_host_disconnect(struct tb_nhi *nhi)
{
	struct tb_test_pci_host *host = container_of(nhi, struct tb_test_pci_host, nhi);

	host->disconnects++;
	return host->result;
}

static void tb_test_pci_host_teardown(struct kunit *test)
{
	static const struct tb_nhi_ops ops = {
		.pci_tunnel_deactivate = tb_test_pci_host_disconnect,
	};
	struct tb_test_pci_host *host;
	struct tb_tunnel *tunnel;
	struct tb *tb;

	host = kunit_kzalloc(test, sizeof(*host), GFP_KERNEL);
	tunnel = kunit_kzalloc(test, sizeof(*tunnel), GFP_KERNEL);
	tb = kunit_kzalloc(test, sizeof(*tb), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, host);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_NOT_NULL(test, tb);
	host->nhi.ops = &ops;
	tb->nhi = &host->nhi;
	tunnel->tb = tb;

	/* A discovered firmware tunnel does not own Linux's PCIe hierarchy. */
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_deactivate_host(tunnel), 0);
	KUNIT_EXPECT_EQ(test, host->disconnects, 0U);

	/* An activated tunnel must release the hierarchy exactly once. */
	tunnel->host_pci_activated = true;
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_deactivate_host(tunnel), 0);
	KUNIT_EXPECT_EQ(test, host->disconnects, 1U);
	KUNIT_EXPECT_FALSE(test, tunnel->host_pci_activated);
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_deactivate_host(tunnel), 0);
	KUNIT_EXPECT_EQ(test, host->disconnects, 1U);

	/* Failed teardown keeps ownership so a later attempt can retry. */
	tunnel->host_pci_activated = true;
	host->result = -EIO;
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_deactivate_host(tunnel), -EIO);
	KUNIT_EXPECT_TRUE(test, tunnel->host_pci_activated);
	KUNIT_EXPECT_EQ(test, host->disconnects, 2U);
	host->result = 0;
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_deactivate_host(tunnel), 0);
	KUNIT_EXPECT_FALSE(test, tunnel->host_pci_activated);
	KUNIT_EXPECT_EQ(test, host->disconnects, 3U);
}

static void tb_test_pci_host_daisy_chain(struct kunit *test)
{
	static const struct tb_nhi_ops ops = {
		.pci_tunnel_pre_activate = tb_test_pci_host_prepare,
		.pci_tunnel_post_activate = tb_test_pci_host_connect,
		.pci_tunnel_deactivate = tb_test_pci_host_disconnect,
	};
	struct tb_tunnel *root_tunnel, *downstream_tunnel;
	struct tb_switch *root, *dev1, *dev2;
	struct tb_test_pci_host *host;
	struct tb *tb;

	host = kunit_kzalloc(test, sizeof(*host), GFP_KERNEL);
	tb = kunit_kzalloc(test, sizeof(*tb), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, host);
	KUNIT_ASSERT_NOT_NULL(test, tb);
	host->nhi.ops = &ops;
	tb->nhi = &host->nhi;
	root = alloc_host(test);
	KUNIT_ASSERT_NOT_NULL(test, root);
	dev1 = alloc_dev_default(test, root, 0x1, true);
	KUNIT_ASSERT_NOT_NULL(test, dev1);
	dev2 = alloc_dev_default(test, dev1, 0x501, true);
	KUNIT_ASSERT_NOT_NULL(test, dev2);
	root_tunnel = tb_tunnel_alloc_pci(tb, &dev1->ports[9], &root->ports[8]);
	KUNIT_ASSERT_NOT_NULL(test, root_tunnel);
	downstream_tunnel = tb_tunnel_alloc_pci(tb, &dev2->ports[9], &dev1->ports[10]);
	KUNIT_ASSERT_NOT_NULL(test, downstream_tunnel);

	KUNIT_EXPECT_EQ(test, root_tunnel->pre_activate(root_tunnel), 0);
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_activate_host(root_tunnel), 0);
	KUNIT_EXPECT_TRUE(test, root_tunnel->host_pci_activated);
	KUNIT_EXPECT_EQ(test, host->prepares, 1U);
	KUNIT_EXPECT_EQ(test, host->connects, 1U);

	/* A second dock must not acquire or tear down the host's PCIe port. */
	KUNIT_EXPECT_EQ(test, downstream_tunnel->pre_activate(downstream_tunnel), 0);
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_activate_host(downstream_tunnel), 0);
	KUNIT_EXPECT_FALSE(test, downstream_tunnel->host_pci_activated);
	KUNIT_EXPECT_EQ(test, host->prepares, 1U);
	KUNIT_EXPECT_EQ(test, host->connects, 1U);
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_deactivate_host(downstream_tunnel), 0);
	KUNIT_EXPECT_EQ(test, host->disconnects, 0U);
	KUNIT_EXPECT_TRUE(test, root_tunnel->host_pci_activated);
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_deactivate_host(root_tunnel), 0);
	KUNIT_EXPECT_EQ(test, host->disconnects, 1U);

	/* A failed host activation may still need to unwind partial setup. */
	host->result = -EIO;
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_activate_host(root_tunnel), -EIO);
	KUNIT_EXPECT_TRUE(test, root_tunnel->host_pci_activated);
	host->result = 0;
	KUNIT_EXPECT_EQ(test, tb_pci_tunnel_deactivate_host(root_tunnel), 0);
	KUNIT_EXPECT_EQ(test, host->disconnects, 2U);

	tb_tunnel_put(downstream_tunnel);
	tb_tunnel_put(root_tunnel);
}

struct tb_test_dp_host {
	struct tb_nhi nhi;
	unsigned int disconnects;
};

static void tb_test_dp_host_disconnect(struct tb_nhi *nhi, struct tb_port *in,
				       struct tb_port *out, u64 generation)
{
	struct tb_test_dp_host *host = container_of(nhi, struct tb_test_dp_host, nhi);

	host->disconnects++;
}

static void tb_test_dp_host_teardown_once(struct kunit *test)
{
	static const struct tb_nhi_ops ops = {
		.dp_tunnel_deactivate = tb_test_dp_host_disconnect,
	};
	struct tb_test_dp_host *host;
	struct tb_tunnel *tunnel;
	struct tb *tb;

	host = kunit_kzalloc(test, sizeof(*host), GFP_KERNEL);
	tunnel = kunit_kzalloc(test, sizeof(*tunnel), GFP_KERNEL);
	tb = kunit_kzalloc(test, sizeof(*tb), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, host);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_NOT_NULL(test, tb);
	host->nhi.ops = &ops;
	tb->nhi = &host->nhi;
	tunnel->tb = tb;
	tunnel->host_dp_activated = true;

	/* Domain removal must release the display even without adapter I/O. */
	tb_dp_tunnel_deactivate_host(tunnel);
	KUNIT_EXPECT_EQ(test, host->disconnects, 1U);
	KUNIT_EXPECT_FALSE(test, tunnel->host_dp_activated);

	/* A late tunnel cleanup must not access the already removed domain. */
	tunnel->tb = NULL;
	tb_dp_tunnel_deactivate_host(tunnel);
	KUNIT_EXPECT_EQ(test, host->disconnects, 1U);
}

static void tb_test_dp_host_teardown_unannounced(struct kunit *test)
{
	struct tb_tunnel *tunnel;

	tunnel = kunit_kzalloc(test, sizeof(*tunnel), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	/* A failed allocation or a domain-less test has no host state to undo. */
	tb_dp_tunnel_deactivate_host(tunnel);
	KUNIT_EXPECT_FALSE(test, tunnel->host_dp_activated);
}

static void tb_test_tunnel_dp_host_credits(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_tunnel *tunnel;
	struct tb_nhi *nhi;
	struct tb *tb;

	host = alloc_host(test);
	KUNIT_ASSERT_NOT_NULL(test, host);
	dev = alloc_dev_with_dpin(test, host, 0x3, true);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	tb = kunit_kzalloc(test, sizeof(*tb), GFP_KERNEL);
	nhi = kunit_kzalloc(test, sizeof(*nhi), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tb);
	KUNIT_ASSERT_NOT_NULL(test, nhi);
	tb->nhi = nhi;
	host->tb = tb;
	dev->tb = tb;

	KUNIT_EXPECT_FALSE(test, tb_port_needs_host_dp_credits(&host->ports[5]));
	/* j416s has no dp_tunnel_changed hook; the quirk must be sufficient. */
	nhi->quirks = QUIRK_HOST_DP_NFC_CREDITS;
	KUNIT_EXPECT_TRUE(test, tb_port_needs_host_dp_credits(&host->ports[5]));
	KUNIT_EXPECT_FALSE(test, tb_port_needs_host_dp_credits(&host->ports[1]));
	KUNIT_EXPECT_FALSE(test, tb_port_needs_host_dp_credits(&dev->ports[13]));

	dev->ports[14].config.type = TB_TYPE_DP_HDMI_OUT;
	tunnel = tb_tunnel_alloc_dp(NULL, &host->ports[5], &dev->ports[14],
				    1, 0, 0, tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].nfc_credits, 5U);
	tb_tunnel_put(tunnel);

	host->tb = NULL;
	KUNIT_EXPECT_FALSE(test, tb_port_needs_host_dp_credits(&host->ports[5]));
}

/* 7.2 owns the domain reference in DPRX, not in the callback. */
struct tb_test_dprx {
	struct tb tb;
	struct tb_nhi nhi;
	struct kunit *test;
	struct completion worker_passed;
	unsigned int completions;
};

static void tb_test_dprx_release(struct device *dev)
{
	/* The context is owned by KUnit and survives the final device put. */
}

static void tb_test_dprx_complete(struct tb_tunnel *tunnel, void *data)
{
	struct tb_test_dprx *ctx = data;

	KUNIT_EXPECT_TRUE(ctx->test, tunnel->dprx_canceled);
	ctx->completions++;
}

static void tb_test_dp_dprx_cancel_common(struct kunit *test, bool running)
{
	struct tb_test_dprx *ctx;
	struct tb_switch *host, *dev;
	struct tb_tunnel *tunnel;
	struct tb *tb;
	ktime_t deadline;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->test = test;
	tb = &ctx->tb;
	tb->nhi = &ctx->nhi;
	host = alloc_host(test);
	KUNIT_ASSERT_NOT_NULL(test, host);
	dev = alloc_dev_default(test, host, 0x1, true);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	tunnel = tb_tunnel_alloc_dp(tb, &host->ports[5], &dev->ports[13],
				    1, 0, 0, tb_test_dprx_complete, ctx);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	tb->wq = alloc_ordered_workqueue("tb-dprx-test", 0);
	if (!tb->wq) {
		tb_tunnel_put(tunnel);
		KUNIT_FAIL(test, "failed to allocate DPRX workqueue");
		return;
	}
	device_initialize(&tb->dev);
	tb->dev.release = tb_test_dprx_release;
	mutex_init(&tb->lock);
	mutex_lock(&tb->lock);
	kref_get(&tunnel->kref);
	tb_domain_get(tb);
	tunnel->dprx_started = true;
	queue_delayed_work(tb->wq, &tunnel->dprx_work, running ? 0 : 60 * HZ);
	if (running) {
		deadline = ktime_add_ms(ktime_get(), 5000);
		while (!(work_busy(&tunnel->dprx_work.work) & WORK_BUSY_RUNNING) &&
		       ktime_before(ktime_get(), deadline))
			usleep_range(100, 200);
		KUNIT_EXPECT_TRUE(test,
			work_busy(&tunnel->dprx_work.work) & WORK_BUSY_RUNNING);
	}
	tb_dp_tunnel_cancel_dprx(tunnel);
	KUNIT_EXPECT_FALSE(test, tunnel->dprx_started);
	KUNIT_EXPECT_TRUE(test, tunnel->dprx_canceled);
	/* 7.2 cancellation must not wait for a worker blocked on this lock. */
	mutex_unlock(&tb->lock);
	flush_workqueue(tb->wq);
	KUNIT_EXPECT_EQ(test, ctx->completions, running ? 1U : 0U);
	KUNIT_EXPECT_EQ(test, kref_read(&tunnel->kref), 1U);
	KUNIT_EXPECT_EQ(test, kref_read(&tb->dev.kobj.kref), 1U);
	mutex_lock(&tb->lock);
	tb_dp_tunnel_cancel_dprx(tunnel);
	mutex_unlock(&tb->lock);
	KUNIT_EXPECT_EQ(test, kref_read(&tunnel->kref), 1U);
	destroy_workqueue(tb->wq);
	tb_tunnel_put(tunnel);
	put_device(&tb->dev);
	mutex_destroy(&tb->lock);
}

static void tb_test_dp_dprx_cancel(struct kunit *test)
{
	tb_test_dp_dprx_cancel_common(test, false);
}

static void tb_test_dp_dprx_cancel_running(struct kunit *test)
{
	tb_test_dp_dprx_cancel_common(test, true);
}

struct tb_test_dprx_gate {
	struct tb_nhi nhi;
	bool deferred;
	struct completion *worker_passed;
	unsigned int samples;
};

static bool tb_test_dprx_awaits_display(struct tb_nhi *nhi, struct tb_port *in)
{
	struct tb_test_dprx_gate *gate = container_of(nhi, struct tb_test_dprx_gate, nhi);

	if (gate->deferred && !gate->samples++)
		complete(gate->worker_passed);
	return gate->deferred;
}

static void tb_test_dprx_finished(struct tb_tunnel *tunnel, void *data)
{
	struct tb_test_dprx *ctx = data;

	ctx->completions++;
	complete(&ctx->worker_passed);
}

static void tb_test_dp_dprx_deferred_first(struct kunit *test)
{
	static const struct tb_nhi_ops ops = {
		.dp_tunnel_awaits_display = tb_test_dprx_awaits_display,
	};
	struct tb_test_dprx_gate gate = { .nhi.ops = &ops, .deferred = true };
	struct tb_test_dprx *ctx;
	struct tb_switch *host, *dev;
	struct tb_tunnel *tunnel;
	struct tb *tb;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->test = test;
	tb = &ctx->tb;
	tb->nhi = &gate.nhi;
	host = alloc_host(test);
	KUNIT_ASSERT_NOT_NULL(test, host);
	dev = alloc_dev_default(test, host, 0x1, true);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	tunnel = tb_tunnel_alloc_dp(tb, &host->ports[5], &dev->ports[13],
				    1, 0, 0, tb_test_dprx_finished, ctx);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	tb->wq = alloc_ordered_workqueue("tb-dprx-gate-test", 0);
	if (!tb->wq) {
		tb_tunnel_put(tunnel);
		KUNIT_FAIL(test, "failed to allocate DPRX workqueue");
		return;
	}
	device_initialize(&tb->dev);
	tb->dev.release = tb_test_dprx_release;
	mutex_init(&tb->lock);
	init_completion(&ctx->worker_passed);
	gate.worker_passed = &ctx->worker_passed;
	mutex_lock(&tb->lock);
	kref_get(&tunnel->kref);
	tb_domain_get(tb);
	tunnel->dprx_started = true;
	tunnel->dprx_timeout = ktime_sub_ms(ktime_get(), 30000);
	queue_delayed_work(tb->wq, &tunnel->dprx_work, 0);
	mutex_unlock(&tb->lock);
	if (!wait_for_completion_timeout(&ctx->worker_passed, 5 * HZ)) {
		KUNIT_FAIL(test, "DPRX worker did not reach deferred gate");
		goto out;
	}
	mutex_lock(&tb->lock);
	KUNIT_EXPECT_EQ(test, ctx->completions, 0U);
	KUNIT_EXPECT_TRUE(test, tunnel->dprx_started);
	KUNIT_EXPECT_TRUE(test, ktime_after(tunnel->dprx_timeout, ktime_get()));
	KUNIT_EXPECT_EQ(test, kref_read(&tunnel->kref), 2U);
	gate.deferred = false;
	host->generation = 3;
	reinit_completion(&ctx->worker_passed);
	mod_delayed_work(tb->wq, &tunnel->dprx_work, 0);
	mutex_unlock(&tb->lock);
	if (!wait_for_completion_timeout(&ctx->worker_passed, 5 * HZ)) {
		KUNIT_FAIL(test, "released DPRX worker did not finish");
		goto out;
	}
	/* Completion precedes 7.2's final tunnel/domain puts. Drain before inspection. */
	flush_workqueue(tb->wq);
	mutex_lock(&tb->lock);
	KUNIT_EXPECT_EQ(test, ctx->completions, 1U);
	KUNIT_EXPECT_EQ(test, kref_read(&tunnel->kref), 1U);
	KUNIT_EXPECT_EQ(test, kref_read(&tb->dev.kobj.kref), 1U);
	mutex_unlock(&tb->lock);
out:
	mutex_lock(&tb->lock);
	tb_dp_tunnel_cancel_dprx(tunnel);
	mutex_unlock(&tb->lock);
	flush_workqueue(tb->wq);
	destroy_workqueue(tb->wq);
	tb_tunnel_put(tunnel);
	put_device(&tb->dev);
	mutex_destroy(&tb->lock);
}

static void tb_test_dp_generation(struct kunit *test)
{
	atomic64_t counter = ATOMIC64_INIT(0);

	KUNIT_EXPECT_EQ(test, tb_dp_generation_alloc(&counter), 1ULL);
	KUNIT_EXPECT_EQ(test, tb_dp_generation_alloc(&counter), 2ULL);
	/* Unsigned cookies continue across the signed atomic64 boundary. */
	atomic64_set(&counter, S64_MAX);
	KUNIT_EXPECT_EQ(test, tb_dp_generation_alloc(&counter), (u64)S64_MAX + 1);
	atomic64_set(&counter, (s64)(U64_MAX - 1));
	KUNIT_EXPECT_EQ(test, tb_dp_generation_alloc(&counter), U64_MAX);
	/* Overflow refuses new handoffs permanently; never reuse zero or one. */
	KUNIT_EXPECT_EQ(test, tb_dp_generation_alloc(&counter), 0ULL);
	KUNIT_EXPECT_EQ(test, tb_dp_generation_alloc(&counter), 0ULL);
	KUNIT_EXPECT_EQ(test, (u64)atomic64_read(&counter), U64_MAX);
}

static void tb_test_apple_right_dpin_wiring(struct kunit *test)
{
	const struct {
		const char *name;
		bool wired;
		unsigned long policy;
		bool expected;
	} cases[] = {
		{ "701f00000.nhi", true, TB_HOST_DP_INITIAL_BW_GRANT, false },
		{ "b01f00000.nhi", true, TB_HOST_DP_INITIAL_BW_GRANT, false },
		{ "f01f00000.nhi", true, TB_HOST_DP_INITIAL_BW_GRANT, true },
		{ "f01f00000.nhi", false, TB_HOST_DP_INITIAL_BW_GRANT, false },
		{ "f01f00000.nhi", true, TB_HOST_DP_NOTIFY, false },
		{ "f01f00000.nhi", true, 0, false },
		{ "pci-nhi", true, TB_HOST_DP_INITIAL_BW_GRANT, false },
	};
	struct tb_nhi missing = {
		.host_dp_policy = TB_HOST_DP_INITIAL_BW_GRANT,
		.host_dp_dual_stream = true,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct device dev = { .init_name = cases[i].name };
		struct tb_nhi nhi = {
			.dev = &dev, .host_dp_policy = cases[i].policy,
			.host_dp_dual_stream = cases[i].wired,
		};

		KUNIT_EXPECT_EQ(test, tb_apple_nhi_needs_bw_grant(&nhi), cases[i].expected);
	}
	KUNIT_EXPECT_FALSE(test, tb_apple_nhi_needs_bw_grant(NULL));
	KUNIT_EXPECT_FALSE(test, tb_apple_nhi_needs_bw_grant(&missing));
}

/*
 * TB_HOST_DP_SHARED_RATE: a second DP tunnel keeps the rate of the first and
 * gives up lanes instead; alone at HBR2, lanes go before the rate.
 */
static void tb_test_dp_reduce_bandwidth_shared(struct kunit *test)
{
	u32 rate, lanes;

	/* Room for HBR2 x4: nothing reduced. */
	KUNIT_EXPECT_EQ(test, tb_dp_reduce_bandwidth_shared(20000, 5400, 4, 5400, 4, 0,
							    &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, lanes, 4);
	KUNIT_EXPECT_EQ(test, tb_dp_reduce_bandwidth_shared(20000, 5400, 4, 5400, 4, 5400,
							    &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, lanes, 4);
	/* 10000 Mb/s: HBR2 x2, never HBR x4. */
	KUNIT_EXPECT_EQ(test, tb_dp_reduce_bandwidth_shared(10000, 5400, 4, 5400, 4, 5400,
							    &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, lanes, 2);
	KUNIT_EXPECT_EQ(test, tb_dp_reduce_bandwidth_shared(10000, 5400, 4, 5400, 4, 0,
							    &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, lanes, 2);
	/* 4000 Mb/s: alone, a lower rate; next to an HBR2 tunnel, nothing. */
	KUNIT_EXPECT_EQ(test, tb_dp_reduce_bandwidth_shared(4000, 5400, 4, 5400, 4, 0,
							    &rate, &lanes), 0);
	KUNIT_EXPECT_LT(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, tb_dp_reduce_bandwidth_shared(4000, 5400, 4, 5400, 4, 5400,
							    &rate, &lanes), -ENOSR);
	/* The other tunnel's rate, lower than this one's: taken, at x4. */
	KUNIT_EXPECT_EQ(test, tb_dp_reduce_bandwidth_shared(20000, 5400, 4, 5400, 4, 2700,
							    &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 2700);
	KUNIT_EXPECT_EQ(test, lanes, 4);
	/* A rate this sink cannot run: refused. */
	KUNIT_EXPECT_EQ(test, tb_dp_reduce_bandwidth_shared(20000, 5400, 4, 2700, 4, 5400,
							    &rate, &lanes), -ENOSR);
}

/*
 * TB_HOST_DP_SHARED_RATE: one DP tunnel on the host router may offer HBR3,
 * more offer HBR2. Alone, a reduction keeps the most bandwidth; held at
 * HBR2 or next to another tunnel, the lanes go first.
 */
static void tb_test_dp_shared_rate_single(struct kunit *test)
{
	u32 rate, lanes;

	KUNIT_EXPECT_EQ(test, tb_dp_host_rate_for(0), 8100);
	KUNIT_EXPECT_EQ(test, tb_dp_host_rate_for(1), 8100);
	KUNIT_EXPECT_EQ(test, tb_dp_host_rate_for(2), 5400);
	KUNIT_EXPECT_EQ(test, tb_dp_host_rate_for(3), 5400);

	/* Alone, the dock's 34650 Mb/s: HBR3 x4 (25920 Mb/s) */
	KUNIT_EXPECT_EQ(test, tb_dp_host_reduce_bandwidth(34650, 8100, 4, 8100, 4, 0, 8100,
							  &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 8100);
	KUNIT_EXPECT_EQ(test, lanes, 4);
	/* Alone, 20000 Mb/s: HBR2 x4 (17280), not HBR3 x2 (12960, too little for 4K60) */
	KUNIT_EXPECT_EQ(test, tb_dp_host_reduce_bandwidth(20000, 8100, 4, 8100, 4, 0, 8100,
							  &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, lanes, 4);
	/* Alone, a sink side that takes HBR2 at most: HBR2 x4 */
	KUNIT_EXPECT_EQ(test, tb_dp_host_reduce_bandwidth(34650, 8100, 4, 5400, 4, 0, 8100,
							  &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, lanes, 4);
	/* Held at HBR2 for a second tunnel, 10000 Mb/s: HBR2 x2, lanes first */
	KUNIT_EXPECT_EQ(test, tb_dp_host_reduce_bandwidth(10000, 5400, 4, 5400, 4, 0, 5400,
							  &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, lanes, 2);
}

/*
 * TB_HOST_DP_SHARED_RATE: with two DP tunnels the second takes the first
 * one's rate, lanes first; a first one up above HBR2 is set up again at HBR2.
 */
static void tb_test_dp_shared_rate_second(struct kunit *test)
{
	u32 rate, lanes;

	/* A second one wanted next to one at HBR3: re-link it now */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(8100, 2, false), TB_DP_RELINK_NOW);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(8100, 3, false), TB_DP_RELINK_NOW);
	/* Alone: kept */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(8100, 1, false), TB_DP_RELINK_NONE);
	/* At HBR2 or below: kept, the second one takes its rate */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(5400, 2, false), TB_DP_RELINK_NONE);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(2700, 2, false), TB_DP_RELINK_NONE);

	/*
	 * The first one, held at HBR2 between adapters that take HBR3, is
	 * counted at HBR2 whatever its DP IN adapter reports: 34650 - 17280
	 * left for the second, not 34650 - 25920 (HBR2 x2 at most)
	 */
	KUNIT_EXPECT_EQ(test, tb_dp_host_counted_bandwidth(5400, 8100, 4), 17280);
	KUNIT_EXPECT_EQ(test, tb_dp_host_counted_bandwidth(5400, 5400, 2), 8640);
	KUNIT_EXPECT_EQ(test, tb_dp_host_counted_bandwidth(8100, 8100, 4), 25920);
	KUNIT_EXPECT_EQ(test, tb_dp_host_counted_bandwidth(0, 8100, 4), 25920);

	/* Two HBR2 x4 tunnels fit the dock: 34650 - 17280 left for the second */
	KUNIT_EXPECT_EQ(test, tb_dp_host_reduce_bandwidth(17370, 5400, 4, 5400, 4, 5400, 5400,
							  &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, lanes, 4);
	/* Less room: the second gives up lanes, never its rate */
	KUNIT_EXPECT_EQ(test, tb_dp_host_reduce_bandwidth(12000, 5400, 4, 8100, 4, 5400, 5400,
							  &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, lanes, 2);
	KUNIT_EXPECT_EQ(test, tb_dp_host_reduce_bandwidth(4000, 5400, 4, 5400, 4, 5400, 5400,
							  &rate, &lanes), -ENOSR);
}

/*
 * TB_HOST_DP_SHARED_RATE: when the other DP tunnel goes, the one left at
 * HBR2 is re-linked at HBR3 once the debounce is over, unless another one
 * comes by then; once per unplug, and the debounce backs off while it flaps.
 */
static void tb_test_dp_shared_rate_relink_up(struct kunit *test)
{
	const unsigned long due = 1000;

	/* Alone at HBR2, HBR3 possible: marked */
	KUNIT_EXPECT_TRUE(test, tb_dp_relink_up_wanted(1, 5400, 8100, false));
	/* Still two, none left, already at its best, or an HBR2-only sink side: not */
	KUNIT_EXPECT_FALSE(test, tb_dp_relink_up_wanted(2, 5400, 8100, false));
	KUNIT_EXPECT_FALSE(test, tb_dp_relink_up_wanted(0, 0, 0, false));
	KUNIT_EXPECT_FALSE(test, tb_dp_relink_up_wanted(1, 8100, 8100, false));
	KUNIT_EXPECT_FALSE(test, tb_dp_relink_up_wanted(1, 5400, 5400, false));

	/* Within the debounce: wait; after it: re-link */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(true, 1, 5400, 8100, false, false, due - 1, due),
			TB_DP_RELINK_WAIT);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(true, 1, 5400, 8100, false, false, due, due),
			TB_DP_RELINK_NOW);
	/* Not marked (the re-linked tunnel itself): once only */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(false, 1, 5400, 8100, false, false, due, due),
			TB_DP_RELINK_NONE);
	/* Another one came, or could be paired: dropped */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(true, 2, 5400, 8100, false, false, due, due),
			TB_DP_RELINK_NONE);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(true, 1, 5400, 8100, true, false, due, due),
			TB_DP_RELINK_NONE);
	/* At HBR3 by now: nothing to do */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(true, 1, 8100, 8100, false, false, due, due),
			TB_DP_RELINK_NONE);

	/* Back-off: doubled while it flaps, at most 40 s; reset when calm */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_backoff(2500, false), 2500);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_backoff(2500, true), 5000);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_backoff(20000, true), 40000);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_backoff(40000, true), 40000);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_backoff(40000, false), 2500);
}

/*
 * TB_HOST_DP_SHARED_RATE: a tunnel re-linked at HBR3 that does not come up
 * is set up again at HBR2, once and without a new re-link up; any other
 * tunnel that does not come up is torn down as before.
 */
static void tb_test_dp_shared_rate_recover(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tb_dp_relink_activation(true, false), TB_DP_RELINK_NOW);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_activation(true, true), TB_DP_RELINK_NONE);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_activation(false, false), TB_DP_RELINK_NONE);
	/* The recovery: HBR2 at most even alone; afterwards HBR3 alone again */
	KUNIT_EXPECT_EQ(test, tb_dp_host_rate_after(1, true), 5400);
	KUNIT_EXPECT_EQ(test, tb_dp_host_rate_after(1, false), 8100);
	KUNIT_EXPECT_EQ(test, tb_dp_host_rate_after(2, false), 5400);
	/* The tunnel set up so (no re-link mark) is never re-linked up again */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(false, 1, 5400, 8100, false, false, 1000, 1000),
			TB_DP_RELINK_NONE);
}

/*
 * TB_HOST_DP_SHARED_RATE: no re-link while system sleep is in progress, from
 * the PREPARE event until the transition is over, also when it is refused.
 */
static void tb_test_dp_shared_rate_sleep(struct kunit *test)
{
	struct tb_dp_pm_state s = {};

	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(8100, 2, true), TB_DP_RELINK_WAIT);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(5400, 2, true), TB_DP_RELINK_NONE);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(8100, 1, true), TB_DP_RELINK_NONE);

	/* Suspend: closed at PREPARE, open only once the transition is over */
	KUNIT_EXPECT_FALSE(test, tb_dp_pm_event(&s, PM_SUSPEND_PREPARE));
	KUNIT_EXPECT_TRUE(test, s.sleeping);
	KUNIT_EXPECT_TRUE(test, tb_dp_pm_event(&s, PM_POST_SUSPEND));
	KUNIT_EXPECT_TRUE(test, s.sleeping);
	tb_dp_pm_reopen(&s);
	KUNIT_EXPECT_FALSE(test, s.sleeping);
	/* Refused: still closed during the POST replay, until the reopen */
	tb_dp_pm_event(&s, PM_SUSPEND_PREPARE);
	KUNIT_EXPECT_TRUE(test, tb_dp_pm_event(&s, PM_POST_SUSPEND));
	KUNIT_EXPECT_TRUE(test, s.sleeping);
	tb_dp_pm_reopen(&s);
	KUNIT_EXPECT_FALSE(test, s.sleeping);
	/* No reopen without a POST event: closed until it comes */
	tb_dp_pm_event(&s, PM_SUSPEND_PREPARE);
	tb_dp_pm_reopen(&s);
	KUNIT_EXPECT_TRUE(test, s.sleeping);
	tb_dp_pm_event(&s, PM_POST_SUSPEND);
	tb_dp_pm_reopen(&s);
	KUNIT_EXPECT_FALSE(test, s.sleeping);
	/*
	 * The reopen asked for by one transition runs after the next one began
	 * (the snapshot device drops the lock in between): closed until that
	 * transition's POST and the reopen after it
	 */
	tb_dp_pm_event(&s, PM_HIBERNATION_PREPARE);
	tb_dp_pm_event(&s, PM_POST_HIBERNATION);
	tb_dp_pm_event(&s, PM_HIBERNATION_PREPARE);
	tb_dp_pm_reopen(&s);
	KUNIT_EXPECT_TRUE(test, s.sleeping);
	KUNIT_EXPECT_TRUE(test, tb_dp_pm_event(&s, PM_POST_HIBERNATION));
	KUNIT_EXPECT_TRUE(test, s.sleeping);
	tb_dp_pm_reopen(&s);
	KUNIT_EXPECT_FALSE(test, s.sleeping);
	/* Two transitions before the reopen runs: one reopen, at the end */
	tb_dp_pm_event(&s, PM_HIBERNATION_PREPARE);
	tb_dp_pm_event(&s, PM_POST_HIBERNATION);
	tb_dp_pm_event(&s, PM_RESTORE_PREPARE);
	tb_dp_pm_event(&s, PM_POST_RESTORE);
	KUNIT_EXPECT_TRUE(test, s.sleeping);
	tb_dp_pm_reopen(&s);
	KUNIT_EXPECT_FALSE(test, s.sleeping);
	tb_dp_pm_reopen(&s);
	KUNIT_EXPECT_FALSE(test, s.sleeping);
	/* Other events change nothing */
	tb_dp_pm_event(&s, PM_SUSPEND_PREPARE);
	KUNIT_EXPECT_FALSE(test, tb_dp_pm_event(&s, 0));
	KUNIT_EXPECT_TRUE(test, s.sleeping);

	/* Tunnels going during the sleep mark nothing */
	KUNIT_EXPECT_FALSE(test, tb_dp_relink_up_wanted(1, 5400, 8100, true));
	/* A re-link to HBR3 due during the sleep waits for its end */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(true, 1, 5400, 8100, false, true, 1001, 1000),
			TB_DP_RELINK_WAIT);
}

/*
 * TB_HOST_DP_SHARED_RATE: re-link work that cannot runtime resume the domain
 * tries again, a few times and later each time; then the request waits for
 * the next resume, thaw or plug event.
 */
static void tb_test_dp_shared_rate_retry(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tb_dp_relink_retry_delay(0), 2500);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_retry_delay(1), 5000);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_retry_delay(4), 40000);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_retry_delay(5), 0);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_retry_delay(100), 0);
}

/* The dock's link: what is left there for DP tunnels (Mb/s) */
#define TB_TEST_DOCK_DP_BW	34650

/*
 * Set @t up as pairing and the capabilities exchange do under
 * TB_HOST_DP_SHARED_RATE, between adapters that take HBR3 x4: capped at
 * @rate_limit, next to a tunnel at @shared_rate (0: alone), with @room Mb/s
 * left on the link. The lanes it takes in @lanes.
 */
static void tb_test_host_dp_up(struct kunit *test, struct tb_tunnel *t, u32 rate_limit,
			       u32 shared_rate, int room, u32 *lanes)
{
	u32 rate;
	int ret;

	memset(t, 0, sizeof(*t));
	t->host_rate_limit = rate_limit;
	t->max_rate = rate_limit;
	t->host_max_rate = TB_DP_HOST_RATE_ALONE;
	ret = tb_dp_host_reduce_bandwidth(room, rate_limit, 4, rate_limit, 4, shared_rate,
					  rate_limit, &rate, lanes);
	KUNIT_ASSERT_EQ(test, ret, 0);
	t->max_rate = min(t->max_rate, rate);
	t->host_rate = rate;
}

/*
 * What @t, up at @lanes, leaves on the dock's link for another DP tunnel,
 * its DP IN adapter reporting the rate both adapters take (HBR3).
 */
static int tb_test_host_dp_room(const struct tb_tunnel *t, u32 lanes)
{
	return TB_TEST_DOCK_DP_BW -
	       tb_dp_host_counted_bandwidth(t->max_rate, TB_DP_HOST_RATE_ALONE, lanes);
}

/*
 * A at HBR2 x4 next to B: B is wanted while A is up at HBR3, so A is set up
 * again at HBR2 first, alone on the link, then B takes its rate.
 */
static void tb_test_host_dp_two_up(struct kunit *test, struct tb_tunnel *a, u32 *a_lanes,
				   struct tb_tunnel *b, u32 *b_lanes)
{
	tb_test_host_dp_up(test, a, tb_dp_host_rate_for(1), 0, TB_TEST_DOCK_DP_BW, a_lanes);
	KUNIT_ASSERT_EQ(test, tb_dp_relink_down(a->host_rate, 2, false), TB_DP_RELINK_NOW);
	tb_test_host_dp_up(test, a, tb_dp_host_rate_for(2), 0, TB_TEST_DOCK_DP_BW, a_lanes);
	tb_test_host_dp_up(test, b, tb_dp_host_rate_for(2), a->host_rate,
			   tb_test_host_dp_room(a, *a_lanes), b_lanes);
}

/* TB_HOST_DP_SHARED_RATE, one display: HBR3 x4, counted so, nothing to re-link. */
static void tb_test_dp_shared_rate_lone(struct kunit *test)
{
	struct tb_tunnel *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	u32 lanes;

	KUNIT_ASSERT_NOT_NULL(test, a);
	tb_test_host_dp_up(test, a, tb_dp_host_rate_for(1), 0, TB_TEST_DOCK_DP_BW, &lanes);
	KUNIT_EXPECT_EQ(test, a->host_rate, 8100);
	KUNIT_EXPECT_EQ(test, lanes, 4);
	KUNIT_EXPECT_EQ(test, tb_test_host_dp_room(a, lanes), 34650 - 25920);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(a->host_rate, 1, false), TB_DP_RELINK_NONE);
	KUNIT_EXPECT_FALSE(test, tb_dp_relink_up_wanted(1, a->host_rate, a->host_max_rate, false));
}

/* TB_HOST_DP_SHARED_RATE, a second display comes: both HBR2 x4. */
static void tb_test_dp_shared_rate_two(struct kunit *test)
{
	struct tb_tunnel *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	struct tb_tunnel *b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	u32 a_lanes, b_lanes;

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	tb_test_host_dp_two_up(test, a, &a_lanes, b, &b_lanes);
	KUNIT_EXPECT_EQ(test, a->host_rate, 5400);
	KUNIT_EXPECT_EQ(test, a_lanes, 4);
	KUNIT_EXPECT_EQ(test, b->host_rate, 5400);
	KUNIT_EXPECT_EQ(test, b_lanes, 4);
	/* At HBR2 both: neither is set up again */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(a->host_rate, 2, false), TB_DP_RELINK_NONE);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(b->host_rate, 2, false), TB_DP_RELINK_NONE);
}

/*
 * TB_HOST_DP_SHARED_RATE, the second display goes: the one left is marked,
 * stays capped and counted at HBR2 through the debounce, and is set up again
 * at HBR3 x4 once it is over.
 */
static void tb_test_dp_shared_rate_leave(struct kunit *test)
{
	struct tb_tunnel *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	struct tb_tunnel *b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	const unsigned long due = 1000 + TB_DP_RELINK_DELAY_MS;
	u32 a_lanes, b_lanes;

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	tb_test_host_dp_two_up(test, a, &a_lanes, b, &b_lanes);

	/* B goes at 1000 */
	KUNIT_ASSERT_TRUE(test, tb_dp_relink_up_wanted(1, a->host_rate, a->host_max_rate, false));
	a->host_relink = true;
	/* Waiting: still capped at HBR2 and counted so */
	KUNIT_EXPECT_EQ(test, a->max_rate, 5400);
	KUNIT_EXPECT_EQ(test, tb_test_host_dp_room(a, a_lanes), 34650 - 17280);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(a->host_relink, 1, a->host_rate, a->host_max_rate,
					      false, false, due - 1, due), TB_DP_RELINK_WAIT);

	/* Debounce over: set up again, alone, at HBR3 x4 and counted so */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(a->host_relink, 1, a->host_rate, a->host_max_rate,
					      false, false, due, due), TB_DP_RELINK_NOW);
	tb_test_host_dp_up(test, a, TB_DP_HOST_RATE_ALONE, 0, TB_TEST_DOCK_DP_BW, &a_lanes);
	KUNIT_EXPECT_EQ(test, a->host_rate, 8100);
	KUNIT_EXPECT_EQ(test, a_lanes, 4);
	KUNIT_EXPECT_EQ(test, tb_test_host_dp_room(a, a_lanes), 34650 - 25920);
	/* Once: the tunnel set up again is not marked */
	KUNIT_EXPECT_FALSE(test, a->host_relink);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(a->host_relink, 1, a->host_rate, a->host_max_rate,
					      false, false, due + 1, due), TB_DP_RELINK_NONE);
	KUNIT_EXPECT_FALSE(test, tb_dp_host_drop_relink_up(a));
}

/*
 * TB_HOST_DP_SHARED_RATE, a second display that comes and goes quickly: no
 * re-link while it flaps within the debounce, the debounce starts again at
 * each unplug, and a second display soon after a re-link up doubles it.
 */
static void tb_test_dp_shared_rate_flap(struct kunit *test)
{
	struct tb_tunnel *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	struct tb_tunnel *b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	unsigned int delay = TB_DP_RELINK_DELAY_MS;
	u32 a_lanes, b_lanes;
	unsigned long due;

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	tb_test_host_dp_two_up(test, a, &a_lanes, b, &b_lanes);

	/* B goes at 1000 */
	a->host_relink = tb_dp_relink_up_wanted(1, a->host_rate, a->host_max_rate, false);
	KUNIT_ASSERT_TRUE(test, a->host_relink);
	due = 1000 + delay;
	/* ... is back at 1500: A held at HBR2, B HBR2 x4 */
	KUNIT_EXPECT_TRUE(test, tb_dp_host_drop_relink_up(a));
	tb_test_host_dp_up(test, b, tb_dp_host_rate_for(2), a->host_rate,
			   tb_test_host_dp_room(a, a_lanes), &b_lanes);
	KUNIT_EXPECT_EQ(test, b->host_rate, 5400);
	KUNIT_EXPECT_EQ(test, b_lanes, 4);
	/* ... goes again at 2000: a new debounce, the first one's end changes nothing */
	a->host_relink = tb_dp_relink_up_wanted(1, a->host_rate, a->host_max_rate, false);
	KUNIT_ASSERT_TRUE(test, a->host_relink);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(a->host_relink, 1, a->host_rate, a->host_max_rate,
					      false, false, due, 2000 + delay), TB_DP_RELINK_WAIT);
	due = 2000 + delay;
	/* ... is back at 3000, gone again at 3200 */
	KUNIT_EXPECT_TRUE(test, tb_dp_host_drop_relink_up(a));
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(a->host_relink, 2, a->host_rate, a->host_max_rate,
					      false, false, due, due), TB_DP_RELINK_NONE);
	a->host_relink = tb_dp_relink_up_wanted(1, a->host_rate, a->host_max_rate, false);
	due = 3200 + delay;
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(a->host_relink, 1, a->host_rate, a->host_max_rate,
					      false, false, due - 1, due), TB_DP_RELINK_WAIT);

	/* Calm until the debounce is over: A set up again at HBR3 x4 */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(a->host_relink, 1, a->host_rate, a->host_max_rate,
					      false, false, due, due), TB_DP_RELINK_NOW);
	tb_test_host_dp_up(test, a, TB_DP_HOST_RATE_ALONE, 0, TB_TEST_DOCK_DP_BW, &a_lanes);
	KUNIT_EXPECT_EQ(test, a->host_rate, 8100);

	/* B is back within a minute: A set up again at HBR2, the next debounce doubled */
	KUNIT_EXPECT_EQ(test, tb_dp_relink_down(a->host_rate, 2, false), TB_DP_RELINK_NOW);
	delay = tb_dp_relink_backoff(delay, true);
	KUNIT_EXPECT_EQ(test, delay, 5000);
	tb_test_host_dp_up(test, a, tb_dp_host_rate_for(2), 0, TB_TEST_DOCK_DP_BW, &a_lanes);
	tb_test_host_dp_up(test, b, tb_dp_host_rate_for(2), a->host_rate,
			   tb_test_host_dp_room(a, a_lanes), &b_lanes);
	KUNIT_EXPECT_EQ(test, a->host_rate, 5400);
	KUNIT_EXPECT_EQ(test, a_lanes, 4);
	KUNIT_EXPECT_EQ(test, b->host_rate, 5400);
	KUNIT_EXPECT_EQ(test, b_lanes, 4);

	/* B goes once more at 10000: HBR3 again only after the doubled debounce */
	a->host_relink = tb_dp_relink_up_wanted(1, a->host_rate, a->host_max_rate, false);
	due = 10000 + delay;
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(a->host_relink, 1, a->host_rate, a->host_max_rate,
					      false, false, 10000 + TB_DP_RELINK_DELAY_MS, due),
			TB_DP_RELINK_WAIT);
	KUNIT_EXPECT_EQ(test, tb_dp_relink_up(a->host_relink, 1, a->host_rate, a->host_max_rate,
					      false, false, due, due), TB_DP_RELINK_NOW);
}

/*
 * A domain with the software connection manager, a host router whose DP IN
 * adapters share one link rate (TB_HOST_DP_SHARED_RATE) and a USB4 device
 * router with two DP OUT adapters. Adapter registers (TB_CFG_PORT, from the
 * adapter capability) are read from @regs: [0] host, [1] device router.
 */
struct tb_test_cm {
	struct tb_nhi nhi;
	struct tb *tb;
	struct tb_switch *host, *dev;
	u32 regs[2][20][16];
};

#define TB_TEST_CM_CAP		0x10
#define TB_TEST_CM_HBR3_X4	(DP_COMMON_CAP_RATE_HBR3 << DP_COMMON_CAP_RATE_SHIFT | \
				 DP_COMMON_CAP_4_LANES << DP_COMMON_CAP_LANES_SHIFT)

static int tb_test_cm_cfg_read(struct tb_ctl *ctl, void *buffer, u64 route, u32 port,
			       enum tb_cfg_space space, u32 offset, u32 length)
{
	struct tb_test_cm *cm = kunit_get_current_test()->priv;
	u32 (*regs)[16] = cm->regs[route ? 1 : 0];

	if (space != TB_CFG_PORT || port >= ARRAY_SIZE(cm->regs[0]) ||
	    offset < TB_TEST_CM_CAP || offset - TB_TEST_CM_CAP + length > ARRAY_SIZE(regs[0]))
		return -EIO;
	memcpy(buffer, &regs[port][offset - TB_TEST_CM_CAP], length * sizeof(u32));
	return 0;
}

static struct tb_test_cm *tb_test_cm_alloc(struct kunit *test)
{
	struct tb_test_cm *cm;
	struct tb *tb;

	cm = kunit_kzalloc(test, sizeof(*cm), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, cm);
	tb = kunit_kzalloc(test, sizeof(*tb) + tb_test_cm_size(), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tb);
	cm->nhi.host_dp_policy = TB_HOST_DP_NOTIFY | TB_HOST_DP_SHARED_RATE;
	tb->nhi = &cm->nhi;
	device_initialize(&tb->dev);
	tb->dev.release = tb_test_dprx_release;
	mutex_init(&tb->lock);
	tb_test_cm_init(tb);
	cm->tb = tb;

	cm->host = alloc_host_usb4(test);
	KUNIT_ASSERT_NOT_NULL(test, cm->host);
	cm->dev = alloc_dev_usb4(test, cm->host, 0x1, true);
	KUNIT_ASSERT_NOT_NULL(test, cm->dev);
	cm->host->tb = tb;
	cm->dev->tb = tb;
	cm->host->config.thunderbolt_version = FIELD_PREP(USB4_VERSION_MAJOR_MASK, 1);
	cm->dev->config.thunderbolt_version = FIELD_PREP(USB4_VERSION_MAJOR_MASK, 1);
	cm->host->ports[5].cap_adap = TB_TEST_CM_CAP;
	cm->host->ports[6].cap_adap = TB_TEST_CM_CAP;
	cm->dev->ports[13].cap_adap = TB_TEST_CM_CAP;
	cm->dev->ports[14].cap_adap = TB_TEST_CM_CAP;

	test->priv = cm;
	kunit_activate_static_stub(test, tb_cfg_read, tb_test_cm_cfg_read);
	return cm;
}

/*
 * What the DP IN adapter @in reports once its DPRX is done: HBR3 x4, the
 * rate both adapters take, in its caps or (@bw_mode) as its allocated
 * bandwidth (26000 Mb/s at a granularity of 250 Mb/s).
 */
static void tb_test_cm_reports(struct tb_test_cm *cm, struct tb_port *in, bool bw_mode)
{
	u32 *regs = cm->regs[0][in->port];

	regs[DP_LOCAL_CAP] = TB_TEST_CM_HBR3_X4;
	regs[DP_COMMON_CAP] = TB_TEST_CM_HBR3_X4 | DP_COMMON_CAP_DPRX_DONE;
	regs[ADP_DP_CS_2] = 0;
	regs[ADP_DP_CS_8] = bw_mode ? ADP_DP_CS_8_DPME : 0;
	regs[DP_STATUS] = bw_mode ? 104 << DP_STATUS_ALLOCATED_BW_SHIFT : 0;
}

/*
 * A host DP tunnel from @in to @out as the capabilities exchange leaves it:
 * capped at @rate_limit (Mb/s per lane), up at @rate. Listed by the
 * connection manager, its adapters enabled.
 */
static struct tb_tunnel *tb_test_cm_dp_tunnel(struct kunit *test, struct tb_test_cm *cm,
					      struct tb_port *in, struct tb_port *out,
					      u32 rate_limit, u32 rate)
{
	struct tb_tunnel *tunnel;
	int i;

	tunnel = tb_tunnel_alloc_dp(cm->tb, in, out, 1, 0, 0, tb_test_dp_tunnel_active, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	tunnel->host_rate_limit = rate_limit;
	tunnel->max_rate = min(rate_limit, rate);
	tunnel->host_rate = rate;
	tunnel->state = TB_TUNNEL_ACTIVE;
	for (i = 0; i < tunnel->npaths; i++)
		tunnel->paths[i]->activated = true;
	list_add_tail(&tunnel->list, tb_test_cm_tunnels(cm->tb));
	cm->regs[0][in->port][ADP_DP_CS_0] = ADP_DP_CS_0_VE | ADP_DP_CS_0_AE;
	cm->regs[1][out->port][ADP_DP_CS_0] = ADP_DP_CS_0_VE | ADP_DP_CS_0_AE;
	return tunnel;
}

static void tb_test_cm_dp_tunnel_put(struct tb_tunnel *tunnel)
{
	int i;

	list_del(&tunnel->list);
	for (i = 0; i < tunnel->npaths; i++)
		tunnel->paths[i]->activated = false;
	tunnel->state = TB_TUNNEL_INACTIVE;
	tb_tunnel_put(tunnel);
}

/*
 * The DP bandwidth the connection manager counts on the host router's lane
 * adapter for a new tunnel from host DP IN 6 to device DP OUT 14.
 */
static int tb_test_cm_counted(struct kunit *test, struct tb_test_cm *cm)
{
	int up, down;

	KUNIT_EXPECT_EQ(test, tb_consumed_dp_bandwidth(cm->tb, &cm->host->ports[6],
						       &cm->dev->ports[14], &cm->host->ports[1],
						       &up, &down), 0);
	KUNIT_EXPECT_EQ(test, up, 0);
	return down;
}

/*
 * TB_HOST_DP_SHARED_RATE, through the connection manager's DP bandwidth
 * accounting: a tunnel capped at HBR2 whose DP IN adapter reports HBR3 x4
 * once its DPRX is done is counted at HBR2 x4, from its caps and from its
 * allocated bandwidth; capped at HBR3, at HBR3 x4.
 */
static void tb_test_dp_shared_rate_counted(struct kunit *test)
{
	struct tb_test_cm *cm = tb_test_cm_alloc(test);
	struct tb_port *in = &cm->host->ports[5];
	struct tb_tunnel *a;

	a = tb_test_cm_dp_tunnel(test, cm, in, &cm->dev->ports[13], 5400, 5400);
	KUNIT_ASSERT_TRUE(test, tb_tunnel_port_on_path(a, &cm->host->ports[1]));

	/* Caps: HBR2 x4, not the HBR3 x4 reported */
	tb_test_cm_reports(cm, in, false);
	KUNIT_EXPECT_EQ(test, tb_test_cm_counted(test, cm), 17280);
	a->max_rate = 8100;
	KUNIT_EXPECT_EQ(test, tb_test_cm_counted(test, cm), 25920);

	/* Allocated bandwidth: HBR2 x4, not the 26000 Mb/s reported */
	tb_test_cm_reports(cm, in, true);
	a->bw_mode = true;
	a->max_rate = 5400;
	KUNIT_EXPECT_EQ(test, tb_test_cm_counted(test, cm), 17280);
	a->max_rate = 8100;
	KUNIT_EXPECT_EQ(test, tb_test_cm_counted(test, cm), 25920);

	tb_test_cm_dp_tunnel_put(a);
	put_device(&cm->tb->dev);
}

/*
 * TB_HOST_DP_SHARED_RATE, through the connection manager: A (host DP IN 5 to
 * device DP OUT 13) is left alone at HBR2 when B (DP IN 6 to DP OUT 14) goes,
 * and C is plugged into DP OUT 14 during the debounce. Pairing drops A's
 * re-link up and holds A at HBR2 before C's bandwidth is worked out, and the
 * debounce timer is stopped unless deferred pairing (@deferred) needs the
 * re-link work. A's DP IN adapter reports HBR3 x4, in its caps or (@bw_mode)
 * as its allocated bandwidth; it is counted at HBR2 x4 and C gets HBR2 x4.
 */
static void tb_test_dp_shared_rate_arrival(struct kunit *test, bool bw_mode, bool deferred)
{
	struct tb_test_cm *cm = tb_test_cm_alloc(test);
	struct list_head *resources = tb_test_cm_dp_resources(cm->tb);
	struct tb_port *in = &cm->host->ports[5];
	struct tb_tunnel *a;
	u32 rate, lanes;
	int counted;

	/*
	 * The debounce timer is real: should it expire before the end, its
	 * work runs on this queue and stops at the missing root switch.
	 */
	cm->tb->wq = alloc_ordered_workqueue("tb-cm-test", 0);
	KUNIT_ASSERT_NOT_NULL(test, cm->tb->wq);

	a = tb_test_cm_dp_tunnel(test, cm, in, &cm->dev->ports[13], 5400, 5400);
	a->host_max_rate = 8100;
	a->bw_mode = bw_mode;
	tb_test_cm_reports(cm, in, bw_mode);
	list_add_tail(&cm->host->ports[5].list, resources);
	list_add_tail(&cm->host->ports[6].list, resources);
	list_add_tail(&cm->dev->ports[13].list, resources);

	mutex_lock(&cm->tb->lock);
	/* B went (two were up): A is marked, the debounce armed */
	tb_dp_host_tunnel_gone(cm->tb, 2);
	KUNIT_EXPECT_TRUE(test, a->host_relink);
	KUNIT_EXPECT_TRUE(test, tb_test_cm_relink_armed(cm->tb));
	KUNIT_EXPECT_EQ(test, kref_read(&cm->tb->dev.kobj.kref), 2U);
	tb_test_cm_set_relink_deferred(cm->tb, deferred);

	/* C comes on DP OUT 14: pairing takes HBR2 for it, A's re-link dropped */
	list_add_tail(&cm->dev->ports[14].list, resources);
	KUNIT_EXPECT_EQ(test, tb_dp_host_rate_limit(cm->tb), 5400U);
	KUNIT_EXPECT_FALSE(test, a->host_relink);
	KUNIT_EXPECT_EQ(test, a->host_rate_limit, 5400U);
	KUNIT_EXPECT_EQ(test, a->max_rate, 5400U);
	KUNIT_EXPECT_EQ(test, tb_test_cm_relink_armed(cm->tb), deferred);
	KUNIT_EXPECT_EQ(test, kref_read(&cm->tb->dev.kobj.kref), deferred ? 2U : 1U);

	/* C's bandwidth: A counted at HBR2 x4, C at A's rate gets x4 */
	counted = tb_test_cm_counted(test, cm);
	KUNIT_EXPECT_EQ(test, counted, 17280);
	KUNIT_EXPECT_EQ(test, tb_dp_host_reduce_bandwidth(TB_TEST_DOCK_DP_BW - counted, 5400, 4,
							  5400, 4, a->host_rate, 5400,
							  &rate, &lanes), 0);
	KUNIT_EXPECT_EQ(test, rate, 5400);
	KUNIT_EXPECT_EQ(test, lanes, 4);
	mutex_unlock(&cm->tb->lock);

	/* Outside the domain lock, which the work takes: drain it, then the queue */
	tb_test_cm_stop(cm->tb);
	destroy_workqueue(cm->tb->wq);
	KUNIT_EXPECT_EQ(test, kref_read(&cm->tb->dev.kobj.kref), 1U);
	INIT_LIST_HEAD(resources);
	tb_test_cm_dp_tunnel_put(a);
	put_device(&cm->tb->dev);
}

/*
 * TB_HOST_DP_SHARED_RATE, a second display comes during the debounce: it
 * gets HBR2 x4 and the debounce timer is stopped, whether the tunnel left
 * alone is counted from its caps or from its allocated bandwidth.
 */
static void tb_test_dp_shared_rate_debounce(struct kunit *test)
{
	tb_test_dp_shared_rate_arrival(test, false, false);
	tb_test_dp_shared_rate_arrival(test, true, false);
}

/*
 * As tb_test_dp_shared_rate_debounce(), with pairing deferred for a system
 * sleep transition: the re-link up is dropped, the re-link work kept armed.
 */
static void tb_test_dp_shared_rate_debounce_deferred(struct kunit *test)
{
	tb_test_dp_shared_rate_arrival(test, false, true);
}

static struct kunit_case tb_test_cases[] = {
	KUNIT_CASE(tb_test_apple_right_dpin_wiring),
	KUNIT_CASE(tb_test_dp_reduce_bandwidth_shared),
	KUNIT_CASE(tb_test_dp_shared_rate_single),
	KUNIT_CASE(tb_test_dp_shared_rate_second),
	KUNIT_CASE(tb_test_dp_shared_rate_relink_up),
	KUNIT_CASE(tb_test_dp_shared_rate_recover),
	KUNIT_CASE(tb_test_dp_shared_rate_lone),
	KUNIT_CASE(tb_test_dp_shared_rate_two),
	KUNIT_CASE(tb_test_dp_shared_rate_leave),
	KUNIT_CASE(tb_test_dp_shared_rate_debounce),
	KUNIT_CASE(tb_test_dp_shared_rate_debounce_deferred),
	KUNIT_CASE(tb_test_dp_shared_rate_flap),
	KUNIT_CASE(tb_test_dp_shared_rate_sleep),
	KUNIT_CASE(tb_test_dp_shared_rate_retry),
	KUNIT_CASE(tb_test_dp_shared_rate_counted),
	KUNIT_CASE(tb_test_dp_generation),
	KUNIT_CASE(tb_test_dp_dprx_deferred_first),
	KUNIT_CASE(tb_test_dp_dprx_cancel),
	KUNIT_CASE(tb_test_dp_dprx_cancel_running),
	KUNIT_CASE(tb_test_property_parse_u32_wrap),
	KUNIT_CASE(tb_test_property_parse_recursion),
	KUNIT_CASE(tb_test_property_parse_dir_len_underflow),
	KUNIT_CASE(tb_test_pci_host_teardown),
	KUNIT_CASE(tb_test_pci_host_daisy_chain),
	KUNIT_CASE(tb_test_tunnel_dp_host_credits),
	KUNIT_CASE(tb_test_dp_host_teardown_once),
	KUNIT_CASE(tb_test_dp_host_teardown_unannounced),
	KUNIT_CASE(tb_test_path_basic),
	KUNIT_CASE(tb_test_path_not_connected_walk),
	KUNIT_CASE(tb_test_path_single_hop_walk),
	KUNIT_CASE(tb_test_path_daisy_chain_walk),
	KUNIT_CASE(tb_test_path_simple_tree_walk),
	KUNIT_CASE(tb_test_path_complex_tree_walk),
	KUNIT_CASE(tb_test_path_max_length_walk),
	KUNIT_CASE(tb_test_path_not_connected),
	KUNIT_CASE(tb_test_path_not_bonded_lane0),
	KUNIT_CASE(tb_test_path_not_bonded_lane1),
	KUNIT_CASE(tb_test_path_not_bonded_lane1_chain),
	KUNIT_CASE(tb_test_path_not_bonded_lane1_chain_reverse),
	KUNIT_CASE(tb_test_path_mixed_chain),
	KUNIT_CASE(tb_test_path_mixed_chain_reverse),
	KUNIT_CASE(tb_test_tunnel_pcie),
	KUNIT_CASE(tb_test_tunnel_dp),
	KUNIT_CASE(tb_test_tunnel_dp_chain),
	KUNIT_CASE(tb_test_tunnel_dp_tree),
	KUNIT_CASE(tb_test_tunnel_dp_max_length),
	KUNIT_CASE(tb_test_tunnel_3dp),
	KUNIT_CASE(tb_test_tunnel_port_on_path),
	KUNIT_CASE(tb_test_tunnel_usb3),
	KUNIT_CASE(tb_test_tunnel_dma),
	KUNIT_CASE(tb_test_tunnel_dma_rx),
	KUNIT_CASE(tb_test_tunnel_dma_tx),
	KUNIT_CASE(tb_test_tunnel_dma_chain),
	KUNIT_CASE(tb_test_tunnel_dma_match),
	KUNIT_CASE(tb_test_credit_alloc_legacy_not_bonded),
	KUNIT_CASE(tb_test_credit_alloc_legacy_bonded),
	KUNIT_CASE(tb_test_credit_alloc_pcie),
	KUNIT_CASE(tb_test_credit_alloc_without_dp),
	KUNIT_CASE(tb_test_credit_alloc_dp),
	KUNIT_CASE(tb_test_credit_alloc_usb3),
	KUNIT_CASE(tb_test_credit_alloc_dma),
	KUNIT_CASE(tb_test_credit_alloc_dma_multiple),
	KUNIT_CASE(tb_test_credit_alloc_all),
	KUNIT_CASE(tb_test_property_parse),
	KUNIT_CASE(tb_test_property_format),
	KUNIT_CASE(tb_test_property_copy),
	KUNIT_CASE(tb_test_property_parse_zero_length),
	KUNIT_CASE(tb_test_property_parse_rootdir_overflow),
	KUNIT_CASE(tb_test_property_merge),
	{ }
};

static struct kunit_suite tb_test_suite = {
	.name = "thunderbolt",
	.test_cases = tb_test_cases,
};

kunit_test_suite(tb_test_suite);
