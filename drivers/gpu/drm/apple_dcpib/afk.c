// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AFKEPV2 ring-buffer endpoints of the T8152 DCP (port of aura src/afk.c).
 *
 * Each endpoint shares one DMA buffer with the firmware, which carves a TX
 * and an RX ring out of it. Messages carry an afkv2 header (interface, seq)
 * and a message header (token, type, category). The firmware publishes
 * services (REPORT/PUBLISH), which the AP opens; AP commands use out-of-band
 * TX/RX buffers, firmware callbacks are inline standard service calls.
 */

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "dcpib.h"

#define RBEP_TYPE		GENMASK_ULL(63, 48)
#define RBEP_INIT		0x80
#define RBEP_INIT_ACK		0xa0
#define RBEP_GETBUF		0x89
#define RBEP_GETBUF_ACK		0xa1
#define RBEP_INIT_TX		0x8a
#define RBEP_INIT_RX		0x8b
#define RBEP_START		0xa3
#define RBEP_START_ACK		0x86
#define RBEP_SEND		0xa2
#define RBEP_RECV		0x85
#define RBEP_SHUTDOWN		0xc0
#define RBEP_SHUTDOWN_ACK	0xc1

#define BLOCK_SHIFT		6
#define GETBUF_SIZE		GENMASK_ULL(31, 16)
#define GETBUF_TAG		GENMASK_ULL(15, 0)
#define GETBUF_ACK_DVA		GENMASK_ULL(47, 0)
#define INITRB_OFFSET		GENMASK_ULL(47, 32)
#define INITRB_SIZE		GENMASK_ULL(31, 16)
#define INITRB_TAG		GENMASK_ULL(15, 0)
#define SEND_WPTR		GENMASK_ULL(31, 0)

#define QE_MAGIC		0x20504f49 /* ' POI' */
#define SERVICE_CALL_MAGIC	0x69706378

#define AFKV2_REPORT		0
#define AFKV2_COMMAND		1
#define AFKV2_RESPONSE		2
#define AFKV2_PUBLISH		0x11
#define AFKV2_OPEN		0x12
#define AFKV2_STD_SERVICE	0xc0

#define DCPIB_BUF_SIZE		0x4000
#define DCPIB_TIMEOUT		msecs_to_jiffies(3000)

struct afk_qe {
	__le32 magic;
	__le32 size;
	__le32 channel;
	__le32 type;
	u8 data[];
} __packed;

struct afkv2_hdr {
	u8 seq;
	u8 reserved;
	__le16 interface;
	__le32 length;
} __packed;

struct afkv2_msg {
	__le64 token;
	u8 type;
	u8 category;
	u8 flags;
	u8 reserved[5];
} __packed;

struct afkv2_cmd {
	u8 flags;
	u8 id;
	__le16 reserved;
	__le32 value; /* command: response capacity; response: IOReturn */
} __packed;

struct afkv2_oob {
	__le64 rxbuf;
	__le64 txbuf;
	__le32 rxlen;
	__le32 txlen;
} __packed;

struct afkv2_service_call {
	__le16 reserved;
	__le16 group;
	__le32 command;
	__le32 length;
	__le32 magic;
	u8 padding[48];
} __packed;

struct afk_announce {
	char name[32];
	u8 props[];
} __packed;

int dcpib_dma_alloc(struct dcpib *dcp, struct dcpib_dma *dma, size_t size)
{
	dma->size = PAGE_ALIGN(size);
	dma->cpu = dma_alloc_coherent(dcp->dev, dma->size, &dma->iova,
				      GFP_KERNEL);
	return dma->cpu ? 0 : -ENOMEM;
}

void dcpib_dma_free(struct dcpib *dcp, struct dcpib_dma *dma)
{
	if (dma->cpu)
		dma_free_coherent(dcp->dev, dma->size, dma->cpu, dma->iova);
	dma->cpu = NULL;
}

static int dcpib_send(struct dcpib_ep *ep, u64 msg)
{
	return apple_rtkit_send_message(ep->dcp->rtk, ep->ep, msg, NULL, false);
}

static int dcpib_ring_init(struct dcpib_ep *ep, struct dcpib_ring *rb, u64 base,
			   u64 size)
{
	u32 bufsz, block;
	void *hdr;

	if (!ep->ring.cpu || base > ep->ring.size ||
	    size > ep->ring.size - base || size < 4 * 0x40)
		return -EINVAL;

	hdr = ep->ring.cpu + base;
	bufsz = le32_to_cpu(READ_ONCE(*(__le32 *)hdr));
	/* Three blocks hold size, read pointer and write pointer. */
	if (!bufsz || bufsz >= size || (size - bufsz) % 3)
		return -EINVAL;
	block = (size - bufsz) / 3;
	if (block < 0x40 || !is_power_of_2(block) || bufsz % block)
		return -EINVAL;

	rb->rptr = hdr + block;
	rb->wptr = hdr + 2 * block;
	rb->buf = hdr + 3 * block;
	rb->bufsz = bufsz;
	rb->block = block;
	if (READ_ONCE(*rb->rptr) >= bufsz || READ_ONCE(*rb->wptr) >= bufsz)
		return -EINVAL;
	rb->ready = true;
	return 0;
}

/* Queue one message on the TX ring and tell the firmware. */
static int dcpib_tx(struct dcpib_ep *ep, const void *data, size_t size)
{
	struct dcpib_ring *rb = &ep->tx;
	struct afk_qe *qe;
	u32 rptr, wptr, advance;
	int ret;

	mutex_lock(&ep->tx_lock);
	if (!rb->ready || size > rb->bufsz - sizeof(*qe)) {
		ret = -EINVAL;
		goto out;
	}

	rptr = le32_to_cpu(READ_ONCE(*rb->rptr));
	wptr = le32_to_cpu(READ_ONCE(*rb->wptr));
	advance = ALIGN(sizeof(*qe) + size, rb->block);

	if (wptr < rptr && advance >= rptr - wptr)
		goto full;
	if (wptr >= rptr) {
		bool fits = advance < rb->bufsz - wptr ||
			    (advance == rb->bufsz - wptr && rptr);

		if (!fits && advance >= rptr)
			goto full;
	}

	qe = rb->buf + wptr;
	qe->magic = cpu_to_le32(QE_MAGIC);
	qe->size = cpu_to_le32(size);
	qe->channel = 0;
	qe->type = 0;
	wptr += sizeof(*qe);
	/* A payload that does not fit before the end starts at offset 0. */
	if (size > rb->bufsz - wptr) {
		memcpy(rb->buf, qe, sizeof(*qe));
		qe = rb->buf;
		wptr = sizeof(*qe);
	}
	memcpy(qe->data, data, size);
	wptr = ALIGN(wptr + size, rb->block);
	if (wptr >= rb->bufsz)
		wptr = 0;

	dma_mb();
	WRITE_ONCE(*rb->wptr, cpu_to_le32(wptr));
	dma_wmb();
	ret = dcpib_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_SEND) |
			     FIELD_PREP(SEND_WPTR, wptr));
	goto out;
full:
	dev_err(ep->dcp->dev, "AFK[%02x]: TX ring full\n", ep->ep);
	ret = -ENOSPC;
out:
	mutex_unlock(&ep->tx_lock);
	return ret;
}

static int dcpib_tx_v2(struct dcpib_ep *ep, u16 interface, void *packet,
		       size_t size)
{
	struct afkv2_hdr *hdr = packet;

	mutex_lock(&ep->tx_lock);
	hdr->seq = ep->seq++;
	mutex_unlock(&ep->tx_lock);
	hdr->interface = cpu_to_le16(interface);
	hdr->length = cpu_to_le32(size - sizeof(*hdr));
	return dcpib_tx(ep, packet, size);
}

static struct dcpib_service *dcpib_find_service(struct dcpib_ep *ep,
						u16 interface)
{
	for (unsigned int i = 0; i < ep->nr_services; i++)
		if (ep->services[i].interface == interface)
			return &ep->services[i];
	return NULL;
}

static void dcpib_announce(struct dcpib_ep *ep, u16 interface,
			   const void *payload, size_t size)
{
	const struct afk_announce *ann = payload;
	struct dcpib_service *service;
	struct {
		struct afkv2_hdr hdr;
		struct afkv2_msg msg;
	} __packed open = {
		.msg = { .type = AFKV2_OPEN, .category = AFKV2_REPORT, .flags = 1 },
	};

	if (!interface || size < sizeof(*ann) ||
	    strnlen(ann->name, sizeof(ann->name)) == sizeof(ann->name))
		return;
	if (dcpib_find_service(ep, interface))
		return;
	if (ep->nr_services >= DCPIB_MAX_SERVICES) {
		dev_warn(ep->dcp->dev, "AFK[%02x]: no room for service %s\n",
			 ep->ep, ann->name);
		return;
	}

	service = &ep->services[ep->nr_services];
	service->ep = ep;
	service->ops = ep->ops;
	service->interface = interface;
	service->unit = ep->nr_services;
	strscpy(service->name, ann->name, sizeof(service->name));
	ep->nr_services++;

	dev_info(ep->dcp->dev, "AFK[%02x]: opening %s on interface %u\n",
		 ep->ep, service->name, interface);
	if (dcpib_tx_v2(ep, interface, &open, sizeof(open))) {
		dev_err(ep->dcp->dev, "AFK[%02x]: failed to open %s\n", ep->ep,
			service->name);
	} else if (strstr(service->name, "dcpav-video-interface")) {
		/*
		 * The AV service powers the display on with this interface and
		 * starts its idle power off timer: SET_MODE again once it is up.
		 */
		dcpib_av_started(ep->dcp);
	}
	if (ep->nr_services == ep->expected)
		complete_all(&ep->services_done);
}

/* Answer a firmware callback we cannot handle: empty, kIOReturnUnsupported. */
static void dcpib_reject(struct dcpib_ep *ep, u16 interface,
			 const struct afkv2_msg *msg,
			 const struct afkv2_cmd *cmd, bool oob)
{
	struct {
		struct afkv2_hdr hdr;
		struct afkv2_msg msg;
		struct afkv2_cmd cmd;
		struct afkv2_oob oob;
	} __packed reply = {
		.msg = { .token = msg->token, .type = msg->type,
			 .category = AFKV2_RESPONSE },
		.cmd = { .flags = cmd->flags, .id = cmd->id,
			 .value = cpu_to_le32(KIO_RETURN_UNSUPPORTED) },
	};
	size_t len = sizeof(reply) - sizeof(reply.oob);

	if (oob) {
		const struct afkv2_oob *o = (const void *)(cmd + 1);

		/* Keep the descriptors, return no data; never touch them. */
		reply.oob.rxbuf = o->rxbuf;
		reply.oob.txbuf = o->txbuf;
		len += sizeof(reply.oob);
	}
	dev_dbg(ep->dcp->dev, "AFK[%02x]: rejecting callback type %#x\n",
		ep->ep, msg->type);
	dcpib_tx_v2(ep, interface, &reply, len);
}

static void dcpib_callback(struct dcpib_ep *ep, u16 interface,
			   const struct afkv2_msg *msg, const void *payload,
			   size_t size)
{
	const struct afkv2_cmd *cmd = payload;
	const struct afkv2_service_call *call;
	struct dcpib_service *service;
	struct afkv2_hdr *rhdr;
	struct afkv2_msg *rmsg;
	struct afkv2_cmd *rcmd;
	struct afkv2_service_call *rcall;
	size_t body, len;
	__le32 status;
	u8 *reply;

	if (size < sizeof(*cmd) || cmd->flags & ~1)
		return;
	if ((cmd->flags & 1) && size < sizeof(*cmd) + sizeof(struct afkv2_oob))
		return;

	service = dcpib_find_service(ep, interface);
	body = size - sizeof(*cmd);
	call = (const void *)(cmd + 1);
	/* Only the inline standard service call form is supported. */
	if (msg->type != AFKV2_STD_SERVICE || cmd->flags || !service ||
	    !service->ops || !service->ops->call ||
	    body < sizeof(*call) + sizeof(u32) ||
	    le32_to_cpu(cmd->value) < body ||
	    le32_to_cpu(call->magic) != SERVICE_CALL_MAGIC ||
	    le32_to_cpu(call->length) != body - sizeof(*call)) {
		dcpib_reject(ep, interface, msg, cmd, cmd->flags & 1);
		return;
	}

	len = sizeof(*rhdr) + sizeof(*rmsg) + size;
	reply = kzalloc(len, GFP_KERNEL);
	if (!reply)
		return;
	rhdr = (void *)reply;
	rmsg = (void *)(rhdr + 1);
	rcmd = (void *)(rmsg + 1);
	rcall = (void *)(rcmd + 1);
	rmsg->token = msg->token;
	rmsg->type = msg->type;
	rmsg->category = AFKV2_RESPONSE;
	rcmd->id = cmd->id;
	memcpy(rcall, call, body);
	status = cpu_to_le32(service->ops->call(service,
				le16_to_cpu(call->group),
				le32_to_cpu(call->command), call + 1,
				le32_to_cpu(call->length), rcall + 1,
				le32_to_cpu(call->length)));
	memcpy(rcall + 1, &status, sizeof(status));
	if (status)
		dev_dbg(ep->dcp->dev,
			"AFK[%02x]: callback %s group %u command %u -> %#x\n",
			ep->ep, service->name, le16_to_cpu(call->group),
			le32_to_cpu(call->command), le32_to_cpu(status));
	dcpib_tx_v2(ep, interface, reply, len);
	kfree(reply);
}

static void dcpib_response(struct dcpib_ep *ep, u16 interface,
			   const struct afkv2_msg *msg, const void *payload,
			   size_t size)
{
	const struct afkv2_cmd *cmd = payload;
	const void *data;
	size_t len;
	int ret = 0;

	if (!ep->cmd_waiting || interface != ep->cmd_interface ||
	    msg->type != ep->cmd_type || size < sizeof(*cmd) ||
	    cmd->id != ep->cmd_id) {
		dev_warn(ep->dcp->dev,
			 "AFK[%02x]: unexpected response on interface %u\n",
			 ep->ep, interface);
		return;
	}

	data = cmd + 1;
	len = size - sizeof(*cmd);
	if (cmd->flags & ~1) {
		ret = -EPROTO;
	} else if (le32_to_cpu(cmd->value)) {
		dev_dbg(ep->dcp->dev, "AFK[%02x]: command %#x returned %#x\n",
			ep->ep, msg->type, le32_to_cpu(cmd->value));
		ret = -EIO;
	} else if (cmd->flags & 1) {
		const struct afkv2_oob *oob = data;

		if (len < sizeof(*oob)) {
			ret = -EPROTO;
		} else {
			data = oob + 1;
			len -= sizeof(*oob);
			if (le32_to_cpu(oob->rxlen)) {
				if (le64_to_cpu(oob->rxbuf) != ep->rxbuf.iova ||
				    le32_to_cpu(oob->rxlen) > ep->rxbuf.size) {
					ret = -EPROTO;
				} else {
					dma_rmb();
					data = ep->rxbuf.cpu;
					len = le32_to_cpu(oob->rxlen);
				}
			}
		}
	}
	if (!ret && len > ep->cmd_out_len)
		ret = -EOVERFLOW;
	if (!ret && len)
		memcpy(ep->cmd_out, data, len);
	ep->cmd_out_len = ret ? 0 : len;
	ep->cmd_ret = ret;
	ep->cmd_waiting = false;
	complete(&ep->cmd_done);
}

static void dcpib_rx_work(struct work_struct *work)
{
	struct dcpib_ep *ep = container_of(work, struct dcpib_ep, rx_work);
	struct dcpib_ring *rb = &ep->rx;

	if (!rb->ready)
		return;

	for (;;) {
		const struct afkv2_hdr *hdr;
		const struct afkv2_msg *msg;
		struct afk_qe *qe;
		u32 rptr, wptr, qsize;

		dma_rmb();
		rptr = le32_to_cpu(READ_ONCE(*rb->rptr));
		wptr = le32_to_cpu(READ_ONCE(*rb->wptr));
		if (rptr == wptr)
			break;
		if (rptr >= rb->bufsz || rptr % rb->block)
			goto corrupt;

		qe = rb->buf + rptr;
		qsize = le32_to_cpu(qe->size);
		if (le32_to_cpu(qe->magic) != QE_MAGIC ||
		    qsize > rb->bufsz - sizeof(*qe))
			goto corrupt;
		/* An entry that does not fit before the end is at offset 0. */
		if (sizeof(*qe) + qsize > rb->bufsz - rptr) {
			rptr = 0;
			qe = rb->buf;
			qsize = le32_to_cpu(qe->size);
			if (le32_to_cpu(qe->magic) != QE_MAGIC ||
			    qsize > rb->bufsz - sizeof(*qe))
				goto corrupt;
		}

		hdr = (const void *)qe->data;
		msg = (const void *)(hdr + 1);
		if (!qe->channel && !qe->type &&
		    qsize >= sizeof(*hdr) + sizeof(*msg) &&
		    le32_to_cpu(hdr->length) == qsize - sizeof(*hdr)) {
			u16 interface = le16_to_cpu(hdr->interface);
			const void *payload = msg + 1;
			size_t size = qsize - sizeof(*hdr) - sizeof(*msg);

			if (msg->category == AFKV2_REPORT &&
			    msg->type == AFKV2_PUBLISH)
				dcpib_announce(ep, interface, payload, size);
			else if (msg->category == AFKV2_RESPONSE)
				dcpib_response(ep, interface, msg, payload,
					       size);
			else if (msg->category == AFKV2_COMMAND)
				dcpib_callback(ep, interface, msg, payload,
					       size);
			else
				dev_dbg(ep->dcp->dev, "AFK[%02x]: if %u cat %u type %#x: %*ph\n",
					 ep->ep, interface, msg->category, msg->type,
					 (int)min_t(size_t, size, 96), payload);
		} else {
			dev_dbg(ep->dcp->dev, "AFK[%02x]: ignoring message\n",
				ep->ep);
		}

		dma_mb();
		rptr = ALIGN(rptr + sizeof(*qe) + qsize, rb->block);
		if (rptr >= rb->bufsz)
			rptr = 0;
		WRITE_ONCE(*rb->rptr, cpu_to_le32(rptr));
	}
	return;

corrupt:
	dev_err(ep->dcp->dev, "AFK[%02x]: corrupt RX ring, stopping it\n",
		ep->ep);
	rb->ready = false;
}

void dcpib_ep_recv(struct dcpib_ep *ep, u64 message)
{
	u32 type = FIELD_GET(RBEP_TYPE, message);
	u64 base, size;
	int ret;

	switch (type) {
	case RBEP_INIT_ACK:
		break;
	case RBEP_GETBUF:
		size = FIELD_GET(GETBUF_SIZE, message) << BLOCK_SHIFT;
		ep->tag = FIELD_GET(GETBUF_TAG, message);
		if (ep->ring.cpu || dcpib_dma_alloc(ep->dcp, &ep->ring, size)) {
			dev_err(ep->dcp->dev, "AFK[%02x]: no ring buffer\n",
				ep->ep);
			break;
		}
		dcpib_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_GETBUF_ACK) |
			       FIELD_PREP(GETBUF_ACK_DVA, ep->ring.iova));
		break;
	case RBEP_INIT_TX:
	case RBEP_INIT_RX:
		base = FIELD_GET(INITRB_OFFSET, message) << BLOCK_SHIFT;
		size = FIELD_GET(INITRB_SIZE, message) << BLOCK_SHIFT;
		if (FIELD_GET(INITRB_TAG, message) != ep->tag) {
			dev_err(ep->dcp->dev, "AFK[%02x]: wrong ring tag\n",
				ep->ep);
			break;
		}
		ret = dcpib_ring_init(ep, type == RBEP_INIT_RX ? &ep->rx : &ep->tx,
				      base, size);
		if (ret) {
			dev_err(ep->dcp->dev, "AFK[%02x]: bad ring %#llx+%#llx\n",
				ep->ep, base, size);
			break;
		}
		if (ep->rx.ready && ep->tx.ready)
			dcpib_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_START));
		break;
	case RBEP_START_ACK:
		complete_all(&ep->started);
		break;
	case RBEP_RECV:
		queue_work(ep->dcp->wq, &ep->rx_work);
		break;
	case RBEP_SHUTDOWN_ACK:
		complete_all(&ep->stopped);
		break;
	default:
		dev_warn(ep->dcp->dev, "AFK[%02x]: unknown message %#llx\n",
			 ep->ep, message);
		break;
	}
}

struct dcpib_ep *dcpib_ep_start(struct dcpib *dcp, u8 epnum,
				const struct dcpib_service_ops *ops,
				unsigned int expected)
{
	struct dcpib_ep *ep;
	int ret;

	if (epnum < DCPIB_FIRST_EP || epnum >= DCPIB_FIRST_EP + DCPIB_NR_EPS)
		return ERR_PTR(-EINVAL);

	ep = devm_kzalloc(dcp->dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return ERR_PTR(-ENOMEM);
	ep->dcp = dcp;
	ep->ep = epnum;
	ep->ops = ops;
	ep->expected = expected;
	init_completion(&ep->started);
	init_completion(&ep->stopped);
	init_completion(&ep->services_done);
	init_completion(&ep->cmd_done);
	mutex_init(&ep->tx_lock);
	mutex_init(&ep->cmd_lock);
	INIT_WORK(&ep->rx_work, dcpib_rx_work);

	ret = dcpib_dma_alloc(dcp, &ep->txbuf, DCPIB_BUF_SIZE);
	if (!ret)
		ret = dcpib_dma_alloc(dcp, &ep->rxbuf, DCPIB_BUF_SIZE);
	if (ret)
		goto err;

	dcp->eps[epnum - DCPIB_FIRST_EP] = ep;
	/*
	 * START_EP for an application endpoint of the adopted session; the
	 * bootloader consumed the endpoint map, so apple_rtkit_start_ep()
	 * does not know it.
	 */
	ret = apple_rtkit_send_message(dcp->rtk, 0,
				       FIELD_PREP(GENMASK_ULL(59, 52), 5) |
				       FIELD_PREP(GENMASK_ULL(39, 32), epnum) |
				       BIT_ULL(1), NULL, false);
	if (ret) {
		dev_err(dcp->dev, "failed to start endpoint %#x: %d\n", epnum, ret);
		goto err_unregister;
	}
	apple_rtkit_adopt_ep(dcp->rtk, epnum);
	ret = dcpib_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_INIT));
	if (ret)
		goto err_unregister;
	if (!wait_for_completion_timeout(&ep->started, DCPIB_TIMEOUT)) {
		dev_err(dcp->dev, "endpoint %#x did not start\n", epnum);
		ret = -ETIMEDOUT;
		goto err_unregister;
	}
	if (!wait_for_completion_timeout(&ep->services_done, DCPIB_TIMEOUT) &&
	    !ep->nr_services) {
		dev_err(dcp->dev, "endpoint %#x announced no service\n", epnum);
		ret = -ETIMEDOUT;
		goto err_unregister;
	}
	if (ep->nr_services < expected)
		dev_warn(dcp->dev, "endpoint %#x: %u of %u services\n", epnum,
			 ep->nr_services, expected);
	return ep;

err_unregister:
	/* The firmware may still own the rings; keep them allocated. */
	return ERR_PTR(ret);
err:
	dcpib_dma_free(dcp, &ep->txbuf);
	dcpib_dma_free(dcp, &ep->rxbuf);
	return ERR_PTR(ret);
}

void dcpib_ep_stop(struct dcpib_ep *ep)
{
	reinit_completion(&ep->stopped);
	if (dcpib_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_SHUTDOWN)) ||
	    !wait_for_completion_timeout(&ep->stopped, DCPIB_TIMEOUT)) {
		dev_err(ep->dcp->dev, "endpoint %#x did not stop\n", ep->ep);
		return;
	}
	ep->rx.ready = ep->tx.ready = false;
	flush_work(&ep->rx_work);
}

int dcpib_command(struct dcpib_service *service, u8 type, const void *tx,
		  size_t txlen, void *rx, size_t *rxlen)
{
	struct dcpib_ep *ep = service->ep;
	size_t cap = rxlen ? *rxlen : 0;
	struct {
		struct afkv2_hdr hdr;
		struct afkv2_msg msg;
		struct afkv2_cmd cmd;
		struct afkv2_oob oob;
	} __packed packet = {
		.msg = { .type = type, .category = AFKV2_COMMAND },
	};
	int ret;

	if (txlen > ep->txbuf.size || cap > ep->rxbuf.size)
		return -EINVAL;

	mutex_lock(&ep->cmd_lock);
	memcpy(ep->txbuf.cpu, tx, txlen);
	packet.cmd.flags = 1;
	packet.cmd.id = ep->next_cmd_id++;
	packet.cmd.value = cpu_to_le32(cap);
	packet.oob.rxbuf = cpu_to_le64(cap ? ep->rxbuf.iova : 0);
	packet.oob.txbuf = cpu_to_le64(ep->txbuf.iova);
	packet.oob.rxlen = cpu_to_le32(cap);
	packet.oob.txlen = cpu_to_le32(txlen);

	reinit_completion(&ep->cmd_done);
	ep->cmd_interface = service->interface;
	ep->cmd_type = type;
	ep->cmd_id = packet.cmd.id;
	ep->cmd_out = rx;
	ep->cmd_out_len = cap;
	ep->cmd_ret = -ETIMEDOUT;
	WRITE_ONCE(ep->cmd_waiting, true);

	ret = dcpib_tx_v2(ep, service->interface, &packet, sizeof(packet));
	if (!ret && !wait_for_completion_timeout(&ep->cmd_done, DCPIB_TIMEOUT))
		ret = -ETIMEDOUT;
	if (!ret)
		ret = ep->cmd_ret;
	if (!ret && rxlen)
		*rxlen = ep->cmd_out_len;
	WRITE_ONCE(ep->cmd_waiting, false);
	/* A late response must not land in the caller's buffer. */
	flush_work(&ep->rx_work);
	mutex_unlock(&ep->cmd_lock);

	if (ret)
		dev_dbg(ep->dcp->dev, "AFK[%02x]: command %#x on %s: %d\n",
			ep->ep, type, service->name, ret);
	return ret;
}

/* An AP-initiated standard service call (aura afk_service_call). */
int dcpib_service_call(struct dcpib_service *service, u16 group, u32 command,
		       const void *data, size_t data_len, size_t data_pad,
		       void *output, size_t output_len, size_t output_pad)
{
	struct afkv2_service_call *call;
	size_t len = max(data_len + data_pad, output_len + output_pad) +
		     sizeof(*call);
	size_t rxlen = len;
	u8 *buf;
	int ret;

	buf = kzalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	call = (void *)buf;
	call->group = cpu_to_le16(group);
	call->command = cpu_to_le32(command);
	call->length = cpu_to_le32(data_len + data_pad);
	call->magic = cpu_to_le32(SERVICE_CALL_MAGIC);
	if (data_len)
		memcpy(call + 1, data, data_len);

	ret = dcpib_command(service, AFKV2_STD_SERVICE, buf, len, buf, &rxlen);
	if (ret)
		goto out;
	if (rxlen < sizeof(*call) ||
	    le32_to_cpu(call->magic) != SERVICE_CALL_MAGIC ||
	    le16_to_cpu(call->group) != group ||
	    le32_to_cpu(call->command) != command ||
	    le32_to_cpu(call->length) > rxlen - sizeof(*call) ||
	    le32_to_cpu(call->length) < output_len) {
		ret = -EPROTO;
		goto out;
	}
	if (output && output_len)
		memcpy(output, call + 1, output_len);
out:
	kfree(buf);
	return ret;
}
