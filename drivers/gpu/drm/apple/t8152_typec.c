// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The rear USB-C ports' DisplayPort alt mode, from the USB Type-C port
 * driver that owns their PD controllers. Each port has one Type-C mode
 * switch for the display path, shared by all DCPs: the port's usb-c node in
 * the shared ports node (apple,usb-c-ports), whose endpoint is at the display
 * port of the port's connector. The Type-C class looks at three mode switches
 * of a connector at most, and the port's PHY needs one as well, so there is
 * no room for one per DCP.
 *
 * The first DCP that probes registers the switches, and they stay until the
 * driver is unloaded: the port driver keeps using a switch it found, and
 * must not wait for a DCP that cannot start. A switch records what the
 * display path reads (DisplayPort alt mode with HPD high, the lanes of the
 * pin assignment, the plug orientation) and has every DCP that drives USB-C
 * look at its ports; t8152_atc.c claims a port for one of them.
 *
 * The switches are dcp-fabric.c's in the device tree only. Its route layer
 * switches a DP PHY and the display crossbar through their own drivers and
 * connects the sink through the DCP's IOMFB DPTX service; these DCPs program
 * the ATC PHY and the crossbar themselves and connect through the iBoot
 * interface's DPTX port service, so their switches record the state alone.
 */

#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/device/faux.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>

#include "iomfb_iboot.h"
#include "t8152_display.h"

/* The ATC instances, as the display crossbar's DFP endpoints number them */
#define T8152_TYPEC_PORTS	4

struct t8152_typec_port {
	struct typec_mux_dev *mux;
	/* below under t8152_typec_lock */
	bool sink;
	bool flipped;
	u32 lanes;
};

/* A DCP that drives USB-C, told about the ports' changes */
struct t8152_typec {
	struct list_head node;
	struct t8152_display *disp;
};

/* Registers the switches, once */
static DEFINE_MUTEX(t8152_typec_register_lock);
/* Below under t8152_typec_register_lock: the switches' parent, which no DCP binding owns */
static struct faux_device *t8152_typec_owner;
/* The status changesets of ports whose switch could not be registered */
struct t8152_typec_disabled {
	struct list_head node;
	struct of_changeset cs;
};

static LIST_HEAD(t8152_typec_disabled_ports);
/* The ports' state, written by the port driver, and the DCPs that read it */
static DEFINE_SPINLOCK(t8152_typec_lock);
static struct t8152_typec_port t8152_typec_ports[T8152_TYPEC_PORTS];
static LIST_HEAD(t8152_typec_displays);

static int t8152_typec_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct t8152_typec_port *port = typec_mux_get_drvdata(mux);
	struct typec_displayport_data *dp = NULL;
	struct t8152_typec *typec;
	bool flipped = false;
	u32 lanes = 4;

	if (state->alt && state->alt->svid == USB_TYPEC_DP_SID &&
	    state->mode >= TYPEC_DP_STATE_A && state->mode <= TYPEC_DP_STATE_F) {
		dp = state->data;
		flipped = typec_altmode_get_orientation(state->alt) ==
			  TYPEC_ORIENTATION_REVERSE;
		/* B, D and F share the cable with USB 3: two DisplayPort lanes */
		if (state->mode == TYPEC_DP_STATE_B || state->mode == TYPEC_DP_STATE_D ||
		    state->mode == TYPEC_DP_STATE_F)
			lanes = 2;
	}

	guard(spinlock)(&t8152_typec_lock);
	/* The sink answers AUX once it raised HPD, not at alt mode entry. */
	port->sink = dp && (dp->status & DP_STATUS_HPD_STATE);
	port->flipped = flipped;
	port->lanes = lanes;
	list_for_each_entry(typec, &t8152_typec_displays, node)
		dcp_iboot_hpd_event(container_of(typec->disp, struct dcp_iboot, disp));
	return 0;
}

/*
 * 1 with a DisplayPort sink on the port of ATC @index (DP alt mode, HPD
 * high), 0 without.
 *
 * A HPD pulse that ends before the DCP looks is not a new sink: an EV2780
 * without a video stream drops HPD for about 160 ms every few seconds, and
 * connecting it anew each time kept its link from coming up. A sink that is
 * really unplugged leaves DP alt mode with its partner.
 */
int t8152_typec_sink(struct t8152_display *disp, u32 index, bool *flipped,
		     u32 *lanes)
{
	struct t8152_typec_port *port;

	if (!disp->typec || index >= T8152_TYPEC_PORTS)
		return 0;
	port = &t8152_typec_ports[index];
	guard(spinlock)(&t8152_typec_lock);
	if (!port->mux || !port->sink)
		return 0;
	*flipped = port->flipped;
	*lanes = port->lanes;
	return 1;
}

static int t8152_typec_port_register(struct device *dev, struct device_node *np)
{
	struct typec_mux_desc desc = {};
	struct t8152_typec_port *port;
	struct typec_mux_dev *mux;
	u32 index;
	int ret;

	ret = of_property_read_u32(np, "apple,dfp-endpoint", &index);
	if (ret || index >= T8152_TYPEC_PORTS)
		return dev_err_probe(dev, ret ?: -EINVAL, "%pOF: invalid port\n", np);
	port = &t8152_typec_ports[index];
	if (port->mux)
		return 0;
	desc.fwnode = of_fwnode_handle(np);
	desc.set = t8152_typec_set;
	desc.drvdata = port;
	desc.name = kasprintf(GFP_KERNEL, "dcp-usb-c%u", index);
	if (!desc.name)
		return -ENOMEM;
	port->lanes = 4;
	/* Under the shared owner, not @dev: the switch outlives any DCP's binding. */
	mux = typec_mux_register(&t8152_typec_owner->dev, &desc);
	kfree(desc.name);
	if (IS_ERR(mux))
		return dev_err_probe(dev, PTR_ERR(mux), "%pOF: switch not registered\n", np);
	scoped_guard(spinlock, &t8152_typec_lock)
		port->mux = mux;
	return 0;
}

/*
 * A switch that cannot be registered: the port driver goes on without it.
 * The changeset stays applied until the driver unloads, which reverts it.
 */
static void t8152_typec_port_disable(struct device *dev, struct device_node *np)
{
	struct t8152_typec_disabled *d = kzalloc(sizeof(*d), GFP_KERNEL);

	if (!d)
		return;
	of_changeset_init(&d->cs);
	if (!of_changeset_update_prop_string(&d->cs, np, "status", "disabled") &&
	    !of_changeset_apply(&d->cs)) {
		list_add(&d->node, &t8152_typec_disabled_ports);
		return;
	}
	dev_err(dev, "%pOF: could not disable the display switch\n", np);
	of_changeset_destroy(&d->cs);
	kfree(d);
}

/*
 * Register the display switches of the USB-C ports the DCP @dev can drive,
 * unless another DCP did. Called first in the DCP's probe, so that the port
 * driver, which waits for them, never waits for a DCP that cannot start.
 */
void t8152_typec_register(struct device *dev)
{
	struct device_node *ports __free(device_node) =
		of_parse_phandle(dev->of_node, "apple,usb-c-ports", 0);
	struct device_node *np = ports ?: dev->of_node;

	guard(mutex)(&t8152_typec_register_lock);
	if (!t8152_typec_owner) {
		t8152_typec_owner = faux_device_create("t8152-typec", NULL, NULL);
		if (!t8152_typec_owner) {
			dev_err(dev, "no owner for the USB-C display switches\n");
			return;
		}
	}
	for_each_available_child_of_node_scoped(np, child) {
		if (of_property_present(child, "mode-switch") &&
		    t8152_typec_port_register(dev, child))
			t8152_typec_port_disable(dev, child);
	}
}

/* At driver unload, once no DCP is bound */
void t8152_typec_unregister(void)
{
	guard(mutex)(&t8152_typec_register_lock);
	for (unsigned int i = 0; i < T8152_TYPEC_PORTS; i++) {
		struct typec_mux_dev *mux = t8152_typec_ports[i].mux;

		if (!mux)
			continue;
		scoped_guard(spinlock, &t8152_typec_lock)
			t8152_typec_ports[i].mux = NULL;
		typec_mux_unregister(mux);
	}
	while (!list_empty(&t8152_typec_disabled_ports)) {
		struct t8152_typec_disabled *d =
			list_first_entry(&t8152_typec_disabled_ports, typeof(*d), node);

		list_del(&d->node);
		/* A changeset that cannot be reverted is still part of the tree: keep it */
		if (of_changeset_revert(&d->cs))
			continue;
		of_changeset_destroy(&d->cs);
		kfree(d);
	}
	if (t8152_typec_owner) {
		faux_device_destroy(t8152_typec_owner);
		t8152_typec_owner = NULL;
	}
}

static void t8152_typec_leave(void *data)
{
	struct t8152_typec *typec = data;

	scoped_guard(spinlock, &t8152_typec_lock)
		list_del(&typec->node);
}

/*
 * Have the DCP see the USB-C ports' display state from now on: call this
 * once the DCP stays bound. Without it, the DCP sees no USB-C sink.
 */
int t8152_typec_init(struct t8152_display *disp)
{
	struct t8152_typec *typec;
	int ret;

	typec = devm_kzalloc(disp->dev, sizeof(*typec), GFP_KERNEL);
	if (!typec)
		return -ENOMEM;
	typec->disp = disp;
	scoped_guard(spinlock, &t8152_typec_lock)
		list_add_tail(&typec->node, &t8152_typec_displays);
	ret = devm_add_action_or_reset(disp->dev, t8152_typec_leave, typec);
	if (ret)
		return ret;
	disp->typec = typec;
	return 0;
}
