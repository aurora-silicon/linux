/* SPDX-License-Identifier: GPL-2.0 */
/*
 * usb hub driver head file
 *
 * Copyright (C) 1999 Linus Torvalds
 * Copyright (C) 1999 Johannes Erdfelt
 * Copyright (C) 1999 Gregory P. Smith
 * Copyright (C) 2001 Brad Hards (bhards@bigpond.net.au)
 * Copyright (C) 2012 Intel Corp (tianyu.lan@intel.com)
 *
 *  move struct usb_hub to this file.
 */

#include <linux/usb.h>
#include <linux/usb/ch11.h>
#include <linux/usb/hcd.h>
#include <linux/usb/typec.h>
#include "usb.h"

/* Quirks that hub_id_table assigns through driver_info */
#define HUB_QUIRK_CHECK_PORT_AUTOSUSPEND	BIT(0)
#define HUB_QUIRK_DISABLE_AUTOSUSPEND		BIT(1)
#define HUB_QUIRK_REDUCE_FRAME_INTR_BINTERVAL	BIT(2)

struct usb_hub {
	struct device		*intfdev;	/* the "interface" device */
	struct usb_device	*hdev;
	struct kref		kref;
	struct urb		*urb;		/* for interrupt polling pipe */

	/* buffer for urb ... with extra space in case of babble */
	u8			(*buffer)[8];
	union {
		struct usb_hub_status	hub;
		struct usb_port_status	port;
	}			*status;	/* buffer for status reports */
	struct mutex		status_mutex;	/* for the status buffer */

	int			error;		/* last reported error */
	int			nerrors;	/* track consecutive errors */

	unsigned long		event_bits[1];	/* status change bitmask */
	unsigned long		change_bits[1];	/* ports with logical connect
							status change */
	unsigned long		removed_bits[1]; /* ports with a "removed"
							device present */
	unsigned long		wakeup_bits[1];	/* ports that have signaled
							remote wakeup */
	unsigned long		power_bits[1]; /* ports that are powered */
	unsigned long		child_usage_bits[1]; /* ports powered on for
							children */
	unsigned long		warm_reset_bits[1]; /* ports requesting warm
							reset recovery */
#if USB_MAXCHILDREN > 31 /* 8*sizeof(unsigned long) - 1 */
#error event_bits[] is too short!
#endif

	struct usb_hub_descriptor *descriptor;	/* class descriptor */
	struct usb_tt		tt;		/* Transaction Translator */

	unsigned		mA_per_port;	/* current for each child */
#ifdef	CONFIG_PM
	unsigned		wakeup_enabled_descendants;
#endif

	unsigned		limited_power:1;
	unsigned		quiescing:1;
	unsigned		disconnected:1;
	unsigned		in_reset:1;
	unsigned		quirk_disable_autosuspend:1;

	unsigned		quirk_check_port_auto_suspend:1;

	unsigned		has_indicators:1;
	u8			indicator[USB_MAXCHILDREN];
	struct delayed_work	leds;
	struct delayed_work	init_work;
	struct delayed_work	post_resume_work;
	struct work_struct      events;
	spinlock_t		irq_urb_lock;
	struct timer_list	irq_urb_retry;
	struct usb_port		**ports;
	struct list_head        onboard_devs;
};

/**
 * struct usb port - kernel's representation of a usb port
 * @child: usb device attached to the port
 * @dev: generic device interface
 * @port_owner: port's owner
 * @peer: related usb2 and usb3 ports (share the same connector)
 * @connector_node: firmware identity of the physical connector
 * @reconnect_hs: USB2 hub observed before its SuperSpeed connection
 * @reconnect_ss: SuperSpeed hub requesting that USB2 connection restart
 * @ss_reconnected: the current USB2 attachment already had its late-SS handover
 * @ss_reconnect_pending: consume the connection event from our own power cycle
 * @ss_handover_generation: explicit SS lane handovers, under usb_port_peer_mutex
 * @ss_reconnect_generation: handover serviced by the last USB2 reconnect
 * @connector: USB Type-C connector
 * @req: default pm qos request for hubs without port power control
 * @connect_type: port's connect type
 * @state: device state of the usb device attached to the port
 * @state_kn: kernfs_node of the sysfs attribute that accesses @state
 * @location: opaque representation of platform connector location
 * @status_lock: synchronize port_event() vs usb_port_{suspend|resume}
 * @power_lock: serialize administrative disable with runtime resume
 * @portnum: port index num based one
 * @is_superspeed cache super-speed status
 * @usb3_lpm_u1_permit: whether USB3 U1 LPM is permitted.
 * @usb3_lpm_u2_permit: whether USB3 U2 LPM is permitted.
 * @early_stop: whether port initialization will be stopped earlier.
 * @ignore_event: whether events of the port are ignored.
 * @disabled: switched off through the "disable" attribute
 * @held_off: switched off by usb_hub_port_power_off(), which holds the port
 *	runtime-active until usb_hub_port_power_on()
 */
struct usb_port {
	struct usb_device *child;
	struct device dev;
	struct usb_dev_state *port_owner;
	struct usb_port *peer;
	struct typec_connector *connector;
	struct device_node *connector_node;
	struct usb_device *reconnect_hs;
	struct usb_device *reconnect_ss;
	struct dev_pm_qos_request *req;
	enum usb_port_connect_type connect_type;
	enum usb_device_state state;
	struct kernfs_node *state_kn;
	usb_port_location_t location;
	struct mutex status_lock;
	struct mutex power_lock; /* administrative disable vs runtime resume */
	u32 over_current_count;
	u8 portnum;
	u32 quirks;
	unsigned int early_stop:1;
	unsigned int ignore_event:1;
	unsigned int is_superspeed:1;
	unsigned int usb3_lpm_u1_permit:1;
	unsigned int usb3_lpm_u2_permit:1;
	bool disabled;
	bool held_off;
	bool ss_reconnected;
	bool ss_reconnect_pending;
	u32 ss_handover_generation;
	u32 ss_reconnect_generation;
};

/* usb_port_peer_mutex protects the peer and both handover counters. */
static inline bool usb_port_ss_reconnect_done(const struct usb_port *port)
{
	return port->ss_reconnected && port->peer &&
	       port->ss_reconnect_generation == port->peer->ss_handover_generation;
}

static inline bool usb_port_may_queue_ss_reconnect(const struct usb_port *port,
						   const struct usb_device *ss)
{
	return port->child && !usb_port_ss_reconnect_done(port) &&
	       (port->reconnect_hs != port->child || port->reconnect_ss != ss);
}

/* Caller holds the hub lock and usb_port_peer_mutex. */
static inline void usb_port_ss_reconnect_event(struct usb_port *port, u16 status, u16 change)
{
	if (port->held_off ||
	    (port->ss_reconnect_pending && !(status & USB_PORT_STAT_CONNECTION) &&
	     !(change & USB_PORT_STAT_C_CONNECTION)))
		return;
	if (!(status & USB_PORT_STAT_CONNECTION) ||
	    (!port->ss_reconnect_pending && (change & USB_PORT_STAT_C_CONNECTION)))
		port->ss_reconnected = false;
	port->ss_reconnect_pending = false;
}

/* A late SS hub may restart only the same, still-live USB3-capable USB2 hub. */
static inline bool usb_port_needs_ss_reconnect(const struct usb_port *port,
					       const struct usb_device *hs,
					       const struct usb_device *ss)
{
	return hs && ss && !usb_port_ss_reconnect_done(port) && !port->is_superspeed &&
	       port->child == hs && port->peer &&
	       port->peer->is_superspeed && port->connector_node &&
	       port->peer->connector_node == port->connector_node &&
	       READ_ONCE(port->peer->child) == ss && !port->disabled && !port->held_off &&
	       !port->port_owner && READ_ONCE(hs->state) == USB_STATE_CONFIGURED &&
	       READ_ONCE(ss->state) == USB_STATE_CONFIGURED && hs->speed == USB_SPEED_HIGH &&
	       ss->speed >= USB_SPEED_SUPER &&
	       hs->descriptor.bDeviceClass == USB_CLASS_HUB &&
	       ss->descriptor.bDeviceClass == USB_CLASS_HUB;
}

#define to_usb_port(_dev) \
	container_of(_dev, struct usb_port, dev)

extern int usb_hub_create_port_device(struct usb_hub *hub,
		int port1);
extern void usb_hub_remove_port_device(struct usb_hub *hub,
		int port1);
extern int usb_hub_set_port_power(struct usb_device *hdev, struct usb_hub *hub,
		int port1, bool set);
extern struct usb_hub *usb_hub_to_struct_hub(struct usb_device *hdev);
extern void hub_get(struct usb_hub *hub);
extern void hub_put(struct usb_hub *hub);
extern int hub_port_debounce(struct usb_hub *hub, int port1,
		bool must_be_connected);
extern int usb_clear_port_feature(struct usb_device *hdev,
		int port1, int feature);
extern int usb_hub_port_status(struct usb_hub *hub, int port1,
		u16 *status, u16 *change);
extern int usb_port_is_power_on(struct usb_hub *hub, unsigned int portstatus);

static inline bool hub_is_port_power_switchable(struct usb_hub *hub)
{
	__le16 hcs;

	if (!hub)
		return false;
	hcs = hub->descriptor->wHubCharacteristics;
	return (le16_to_cpu(hcs) & HUB_CHAR_LPSM) < HUB_CHAR_NO_LPSM;
}

static inline int hub_is_superspeed(struct usb_device *hdev)
{
	return hdev->descriptor.bDeviceProtocol == USB_HUB_PR_SS;
}

static inline int hub_is_superspeedplus(struct usb_device *hdev)
{
	return (hdev->descriptor.bDeviceProtocol == USB_HUB_PR_SS &&
		le16_to_cpu(hdev->descriptor.bcdUSB) >= 0x0310 &&
		hdev->bos && hdev->bos->ssp_cap);
}

static inline unsigned hub_power_on_good_delay(struct usb_hub *hub)
{
	unsigned delay = hub->descriptor->bPwrOn2PwrGood * 2;

	if (!hub->hdev->parent)	/* root hub */
		return delay;
	else /* Wait at least 100 msec for power to become stable */
		return max(delay, 100U);
}

static inline int hub_port_debounce_be_connected(struct usb_hub *hub,
		int port1)
{
	return hub_port_debounce(hub, port1, true);
}

static inline int hub_port_debounce_be_stable(struct usb_hub *hub,
		int port1)
{
	return hub_port_debounce(hub, port1, false);
}

#if IS_ENABLED(CONFIG_KUNIT)
unsigned long usb_hub_id_quirks(struct usb_interface *intf);
#endif
