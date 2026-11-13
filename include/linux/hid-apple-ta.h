/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Trusted-accessory channel of the Magic Keyboards with Touch ID.
 *
 * The keyboard's Touch ID interface is a vendor HID collection that carries
 * pairing, session and sensor traffic between the keyboard and the Secure
 * Enclave. The host only relays it: the bodies are opaque, and nothing here
 * hands them to user space. The transport (hid-apple-ta) owns the collection
 * and passes its input reports to one in-kernel client, the enclave's
 * trusted-accessory service.
 */
#ifndef _LINUX_HID_APPLE_TA_H
#define _LINUX_HID_APPLE_TA_H

#include <linux/types.h>

struct apple_ta_device;

/* The three input reports of the collection, by role. */
enum apple_ta_role {
	/* Command replies and state (107 bytes on USB). */
	APPLE_TA_CONTROL,
	/* Fragments of large objects and sensor frames (649 bytes on USB). */
	APPLE_TA_DATA,
	/* Short asynchronous events (62 bytes on USB). */
	APPLE_TA_EVENT,
	APPLE_TA_NR_ROLES,
};

enum apple_ta_bus {
	APPLE_TA_USB,
	APPLE_TA_BLUETOOTH,
};

struct apple_ta_client_ops {
	/* A keyboard's channel came up. May sleep. */
	void (*attach)(void *ctx, struct apple_ta_device *ta);
	/*
	 * The channel is going away. May sleep. After this returns no more
	 * reports arrive for @ta and @ta must not be used.
	 */
	void (*detach)(void *ctx, struct apple_ta_device *ta);
	/*
	 * One input report: its report ID and the @len bytes after it. Any ID
	 * and any length up to HID_MAX_BUFFER_SIZE - 1: besides the declared
	 * roles, the keyboard answers commands and sends sensor frames in
	 * reports its descriptor does not declare. Called in atomic context:
	 * copy what is needed and return.
	 */
	void (*report)(void *ctx, struct apple_ta_device *ta, u8 id,
		       const u8 *data, size_t len);
};

int apple_ta_register_client(const struct apple_ta_client_ops *ops, void *ctx);
void apple_ta_unregister_client(const struct apple_ta_client_ops *ops);

enum apple_ta_bus apple_ta_bus(const struct apple_ta_device *ta);
size_t apple_ta_report_len(const struct apple_ta_device *ta,
			   enum apple_ta_role role);
/*
 * Sends one feature or output report (@buf[0] is the report ID) on the
 * control path. May sleep. Returns the number of bytes sent.
 */
int apple_ta_send(struct apple_ta_device *ta, u8 report_type,
		  const u8 *buf, size_t len);
/* Reads one feature report (@buf[0] is the report ID). May sleep. */
int apple_ta_get(struct apple_ta_device *ta, u8 *buf, size_t len);

#endif /* _LINUX_HID_APPLE_TA_H */
