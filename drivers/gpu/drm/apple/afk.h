// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AFK (Apple Firmware Kit) EPIC (EndPoint Interface Client) support
 */
/* Copyright 2022 Sven Peter <sven@svenpeter.dev> */

#ifndef _DRM_APPLE_DCP_AFK_H
#define _DRM_APPLE_DCP_AFK_H

#include <linux/bitmap.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#include "dcp.h"

#define AFK_MAX_CHANNEL 16
#define MAX_PENDING_CMDS 16

struct apple_epic_service_ops;
struct apple_dcp_afkep;

struct epic_cmd_info {
	u16 tag;

	void *rxbuf;
	void *txbuf;
	dma_addr_t rxbuf_dma;
	dma_addr_t txbuf_dma;
	size_t rxlen;
	size_t txlen;

	u32 retcode;
	bool done;
	bool free_on_ack;
	struct completion *completion;
};

struct apple_epic_service {
	const struct apple_epic_service_ops *ops;
	struct apple_dcp_afkep *ep;

	struct epic_cmd_info cmds[MAX_PENDING_CMDS];
	DECLARE_BITMAP(cmd_map, MAX_PENDING_CMDS);
	u8 cmd_tag;
	spinlock_t lock;
	/* Pins held by owners and synchronous callers; protected by lock. */
	unsigned int users;

	u32 channel;
	bool enabled;
	bool torndown;

	void *cookie;

    struct {
        struct dentry *entry;
        u8 *scratch;
    } debugfs;
};

enum epic_subtype;

struct apple_epic_service_ops {
	const char name[32];
	/* All retained pointers must be pinned before enabling slot reuse. */
	bool reusable;

	void (*init)(struct apple_epic_service *service, const char *name,
			      const char *class, s64 unit);
	int (*call)(struct apple_epic_service *service, u32 idx,
		    const void *data, size_t data_size, void *reply,
		    size_t reply_size);
	int (*report)(struct apple_epic_service *service, enum epic_subtype type,
		      const void *data, size_t data_size);
	void (*teardown)(struct apple_epic_service *service);
};

static inline struct apple_epic_service *afk_service_get(struct apple_epic_service *service)
{
	unsigned long flags;
	bool available;

	if (!service)
		return NULL;

	spin_lock_irqsave(&service->lock, flags);
	available = service->enabled && !service->torndown;
	if (available)
		service->users++;
	spin_unlock_irqrestore(&service->lock, flags);

	return available ? service : NULL;
}

static inline void afk_service_put(struct apple_epic_service *service)
{
	unsigned long flags;

	spin_lock_irqsave(&service->lock, flags);
	if (!WARN_ON(!service->users))
		service->users--;
	spin_unlock_irqrestore(&service->lock, flags);
}

static inline void afk_service_disable(struct apple_epic_service *service)
{
	unsigned long flags;

	spin_lock_irqsave(&service->lock, flags);
	service->enabled = false;
	spin_unlock_irqrestore(&service->lock, flags);
}

static inline bool afk_service_matches(struct apple_epic_service *service,
				       u32 channel)
{
	unsigned long flags;
	bool found;

	spin_lock_irqsave(&service->lock, flags);
	/* Teardown does not cancel commands or relinquish their DMA. */
	found = service->channel == channel &&
		(service->enabled ||
		 !bitmap_empty(service->cmd_map, MAX_PENDING_CMDS));
	spin_unlock_irqrestore(&service->lock, flags);
	return found;
}

/* The caller holds service->lock. */
static inline bool afk_service_reclaimable(struct apple_epic_service *service)
{
	return !service->ops ||
		(service->ops->reusable && service->torndown &&
		 !service->enabled && !service->users &&
		 bitmap_empty(service->cmd_map, MAX_PENDING_CMDS));
}

static inline bool
afk_service_reinit(struct apple_epic_service *service,
		   struct apple_dcp_afkep *ep,
		   const struct apple_epic_service_ops *ops, u32 channel)
{
	unsigned long flags;
	bool reusable;

	spin_lock_irqsave(&service->lock, flags);
	reusable = afk_service_reclaimable(service);
	if (reusable) {
		service->enabled = true;
		service->torndown = false;
		service->ops = ops;
		service->ep = ep;
		service->channel = channel;
		service->cookie = NULL;
		/* Keep the command-tag sequence across service generations. */
	}
	spin_unlock_irqrestore(&service->lock, flags);
	return reusable;
}

struct afk_ringbuffer_header {
	__le32 bufsz;
	u32 unk;
	u32 _pad1[14];
	__le32 rptr;
	u32 _pad2[15];
	__le32 wptr;
	u32 _pad3[15];
};

struct afk_qe {
#define QE_MAGIC 0x20504f49 // ' POI'
	__le32 magic;
	__le32 size;
	__le32 channel;
	__le32 type;
	u8 data[];
};

struct epic_hdr {
	u8 version;
	__le16 seq;
	u8 _pad;
	__le32 unk;
	__le64 timestamp;
} __attribute__((packed));

struct epic_sub_hdr {
	__le32 length;
	u8 version;
	u8 category;
	__le16 type;
	__le64 timestamp;
	__le16 tag;
	__le16 unk;
	__le32 inline_len;
} __attribute__((packed));

struct epic_cmd {
	__le32 retcode;
	__le64 rxbuf;
	__le64 txbuf;
	__le32 rxlen;
	__le32 txlen;
	u8 rxcookie;
	u8 txcookie;
} __attribute__((packed));

struct epic_service_call {
	u8 _pad0[2];
	__le16 group;
	__le32 command;
	__le32 data_len;
#define EPIC_SERVICE_CALL_MAGIC 0x69706378
	__le32 magic;
	u8 _pad1[48];
} __attribute__((packed));
static_assert(sizeof(struct epic_service_call) == 64);

enum epic_type {
	EPIC_TYPE_NOTIFY = 0,
	EPIC_TYPE_COMMAND = 3,
	EPIC_TYPE_REPLY = 4,
	EPIC_TYPE_NOTIFY_ACK = 8,
};

enum epic_category {
	EPIC_CAT_REPORT = 0x00,
	EPIC_CAT_NOTIFY = 0x10,
	EPIC_CAT_REPLY = 0x20,
	EPIC_CAT_COMMAND = 0x30,
};

enum epic_subtype {
	EPIC_SUBTYPE_ANNOUNCE = 0x30,
	EPIC_SUBTYPE_TEARDOWN = 0x32,
	EPIC_SUBTYPE_STD_SERVICE = 0xc0,
};

struct afk_ringbuffer {
	bool ready;
	struct afk_ringbuffer_header *hdr;
	u32 rptr;
	void *buf;
	size_t bufsz;
};

struct apple_dcp_afkep {
	struct apple_dcp *dcp;

	u32 endpoint;
	struct workqueue_struct *wq;

	struct completion started;
	struct completion stopped;

	void *bfr;
	u16 bfr_tag;
	size_t bfr_size;
	dma_addr_t bfr_dma;

	struct afk_ringbuffer txbfr;
	struct afk_ringbuffer rxbfr;
	/* Private receive entry, owned by the ordered endpoint worker. */
	void *rx_scratch;

	spinlock_t lock;
	u16 qe_seq;
	bool stopping; /* lock: no new receive work after shutdown. */

	const struct apple_epic_service_ops *ops;
	struct apple_epic_service services[AFK_MAX_CHANNEL];
	u32 num_channels;

	struct dentry *debugfs_entry;

	bool match_epic_name;
};

struct apple_dcp_afkep *afk_init(struct apple_dcp *dcp, u32 endpoint,
				 const struct apple_epic_service_ops *ops);
int afk_start(struct apple_dcp_afkep *ep);
void afk_shutdown(struct apple_dcp_afkep *ep);
int afk_receive_message(struct apple_dcp_afkep *ep, u64 message);
int afk_send_epic(struct apple_dcp_afkep *ep, u32 channel, u16 tag,
		  enum epic_type etype, enum epic_category ecat, u8 stype,
		  const void *payload, size_t payload_len);
int afk_send_command(struct apple_epic_service *service, u8 type,
		     const void *payload, size_t payload_len, void *output,
		     size_t output_len, u32 *retcode);
int afk_send_command_timeout(struct apple_epic_service *service, u8 type,
			     const void *payload, size_t payload_len,
			     void *output, size_t output_len, u32 *retcode,
			     unsigned int timeout_ms);
int afk_service_call(struct apple_epic_service *service, u16 group, u32 command,
		     const void *data, size_t data_len, size_t data_pad,
		     void *output, size_t output_len, size_t output_pad);
int afk_service_call_timeout(struct apple_epic_service *service, u16 group,
			     u32 command, const void *data, size_t data_len,
			     size_t data_pad, void *output, size_t output_len,
			     size_t output_pad, unsigned int timeout_ms);
#endif
