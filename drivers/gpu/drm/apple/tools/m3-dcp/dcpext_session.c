// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* See dcpext_session.h for attribution and the hardware adapter contract. */
#include "dcpext_session.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#define MSG(type) ((u64)(type) << 52)
#define SYSTEM_PROFILE 0x11fU /* J514S 14.6 session-1 capture, 2026-09-21 */
#define SESSION_MS 8000
#define MAX_MESSAGES 256
#define DVA_SIZE (1ULL << 36) /* Two 11-bit levels plus 14-bit page offset */
#define DVA_BASE (1ULL << 40) /* M4 firmware wire address, not a table index */

static bool valid_buffer_address(unsigned int ep, bool inherited, u64 addr, u32 size)
{
	u64 base = DVA_BASE, limit = DVA_SIZE;

	if (ep == 8 && inherited) {
		/* OSLog can report a host physical address; the callback must
		 * qualify the exact OS-log reservation before accepting it.
		 */
		base = 0;
		limit = 1ULL << 42;
	}
	return addr && !(addr & 0xfff) && addr >= base &&
		addr - base < limit && size <= limit - (addr - base);
}

static int fail(struct dcpext_session *s, int error)
{
	if (!s->error)
		s->error = error < 0 ? error : -EIO;
	s->phase = DCPEXT_FAILED;
	return s->error;
}

int dcpext_session_poll(struct dcpext_session *s)
{
	u64 now;

	if (s->phase == DCPEXT_FAILED)
		return s->error;
	if (!s->ops || s->phase == DCPEXT_NEW)
		return -EINVAL;
	if (s->phase == DCPEXT_QUIESCED)
		return 0;
	now = s->ops->now_ms(s->cookie);
	if (now < s->last_time)
		return fail(s, -ERANGE);
	s->last_time = now;
	if (s->runtime) {
		if (s->phase != DCPEXT_RUNNING)
			return fail(s, -EPROTO);
		if (now - s->runtime_window >= 1000) {
			s->runtime_window = now;
			s->runtime_messages = 0;
		}
	} else if (now >= s->deadline)
		return fail(s, -ETIMEDOUT);
	return 0;
}

int dcpext_session_enter_runtime(struct dcpext_session *s)
{
	int ret = dcpext_session_poll(s);

	if (ret)
		return ret;
	if (s->phase != DCPEXT_RUNNING || s->runtime)
		return -EINVAL;
	s->runtime = true;
	s->runtime_window = s->last_time;
	s->runtime_messages = 0;
	return 0;
}

int dcpext_session_runtime_message(struct dcpext_session *s)
{
	int ret = dcpext_session_poll(s);

	if (ret)
		return ret;
	if (s->runtime && ++s->runtime_messages > 8192)
		return fail(s, -EOVERFLOW);
	return 0;
}

int dcpext_session_leave_runtime(struct dcpext_session *s)
{
	int ret = dcpext_session_poll(s);

	if (ret)
		return ret;
	if (!s->runtime || s->last_time > ~(u64)0 - SESSION_MS)
		return -EINVAL;
	s->runtime = false;
	s->deadline = s->last_time + SESSION_MS;
	s->received = 0;
	return 0;
}

static int send(struct dcpext_session *s, u8 ep, u64 msg)
{
	int ret = dcpext_session_poll(s);

	if (ret)
		return ret;
	/* A failed transport return does not establish that delivery failed. */
	s->touched = true;
	ret = s->ops->send(s->cookie, ep, msg);
	if (ret)
		return fail(s, ret);
	return dcpext_session_poll(s);
}

int dcpext_session_begin(struct dcpext_session *s,
			 const struct dcpext_session_ops *ops, void *cookie)
{
	u64 now, duration = s->duration_ms ? s->duration_ms : SESSION_MS;

	if (duration > 60000 || duration < SESSION_MS)
		return -EINVAL;
	if (s->message_limit &&
	    !((s->message_limit == 512 && (duration == 16000 || duration == 60000)) ||
	      (s->message_limit == 8192 && duration == 16000)))
		return -EINVAL;
	if (s->phase != DCPEXT_NEW || s->touched || s->ops)
		return -EBUSY;
	if (!ops || !ops->send || !ops->buffer || !ops->now_ms)
		return -EINVAL;
	now = ops->now_ms(cookie);
	if (now > ~(u64)0 - duration)
		return -ERANGE;
	s->ops = ops;
	s->cookie = cookie;
	s->last_time = now;
	s->deadline = now + duration;
	s->phase = DCPEXT_HELLO;
	/* Linux apple_rtkit_wake and m1n1 rtkit_boot request INIT (0x220),
	 * including the reinitialization bit, and wait for ON (0x20).
	 * ON alone wakes an inherited session but does not request this reset
	 * of protocol state. Never issue CPU reset/run register writes here.
	 */
	return send(s, 0, MSG(6) | 0x220);
}

static int ap_on_if_ready(struct dcpext_session *s)
{
	if (!s->epmap_done || !s->iop_on)
		return 0;
	s->phase = DCPEXT_AP_ON;
	return send(s, 0, MSG(0xb) | 0x20);
}

static int management(struct dcpext_session *s, u64 msg)
{
	unsigned int type = (msg >> 52) & 0xff;
	unsigned int base, ep, state = msg & 0xffff;
	u32 bitmap;
	bool last;
	int ret;

	switch (type) {
	case 1:
		if (s->phase != DCPEXT_HELLO || s->hello ||
		    (msg & 0xffff) > 12 || ((msg >> 16) & 0xffff) < 12)
			return fail(s, -EPROTO);
		s->hello = true;
		s->phase = DCPEXT_EPMAP;
		return send(s, 0, MSG(2) | (12 << 16) | 12);
	case 8:
		base = (msg >> 32) & 7;
		bitmap = (u32)msg;
		last = (msg >> 51) & 1;
		if (s->phase != DCPEXT_EPMAP || !s->hello ||
		    (s->bases & (1U << base)))
			return fail(s, -EPROTO);
		s->bases |= 1U << base;
		s->endpoints[base] = bitmap;
		if ((!base && bitmap != SYSTEM_PROFILE) ||
		    (last && !(s->bases & 1)))
			return fail(s, -EOPNOTSUPP);
		ret = send(s, 0, MSG(8) | ((u64)base << 32) |
			   (last ? 1ULL << 51 : 1));
		if (ret || !last)
			return ret;
		s->epmap_done = true;
		s->phase = DCPEXT_IOP_ON;
		for (ep = 1; ep < 32; ep++) {
			if (!(SYSTEM_PROFILE & (1U << ep)))
				continue;
			ret = send(s, 0, MSG(5) | ((u64)ep << 32) | 2);
			if (ret)
				return ret;
			s->started |= 1U << ep;
		}
		return ap_on_if_ready(s);
	case 7:
		if (state == 0x20 && s->hello && !s->iop_on &&
		    (s->phase == DCPEXT_EPMAP || s->phase == DCPEXT_IOP_ON)) {
			s->iop_on = true;
			return ap_on_if_ready(s);
		}
		if (state == (s->sleep_iop ? 1 : 0x10) && s->phase == DCPEXT_IOP_QUIESCE) {
			s->phase = DCPEXT_QUIESCED;
			return 0;
		}
		return fail(s, -EPROTO);
	case 0xb:
		if (state == 0x20 && s->phase == DCPEXT_AP_ON) {
			s->phase = DCPEXT_RUNNING;
			return 0;
		}
		if (state == 0x10 && s->phase == DCPEXT_AP_QUIESCE && !s->ap_quiesced) {
			s->ap_quiesced = true;
			return s->defer_iop_quiesce ? 0 : dcpext_session_finish_quiesce(s);
		}
		return fail(s, -EPROTO);
	default:
		return fail(s, -EOPNOTSUPP);
	}
}

static int get_buffer(struct dcpext_session *s, unsigned int ep, u64 msg)
{
	struct dcpext_buffer *b = &s->buffers[ep];
	u64 requested, dva = 0, reply;
	u32 size;
	int ret;

	if (b->ready)
		return fail(s, ep == 1 ? -EIO : -EPROTO); /* EP1 repeat = crash */
	if (ep == 8) {
		size = (msg >> 36) & 0xfffff;
		requested = (msg & ((1ULL << 36) - 1)) << 12;
	} else {
		size = ((msg >> 44) & 0xff) << 12;
		requested = msg & ((1ULL << 44) - 1);
	}
	if (!size || size > 0x100000 ||
	    (requested && !valid_buffer_address(ep, true, requested, size)))
		return fail(s, -EINVAL);
	ret = s->ops->buffer(s->cookie, ep, requested, size, &dva);
	if (ret)
		return fail(s, ret);
	/* Record ownership before any further operation can fail. */
	b->dva = dva;
	b->size = size;
	b->inherited = requested != 0;
	b->ready = true;
	ret = dcpext_session_poll(s);
	if (ret)
		return ret;
	if (!valid_buffer_address(ep, requested != 0, dva, size) ||
	    (requested && dva != requested))
		return fail(s, -EINVAL);
	/* M4 firmware-owned buffers are validated without a host reply. */
	if (requested)
		return 0;
	if (ep == 8)
		reply = (1ULL << 56) | ((u64)size << 36) | (dva >> 12);
	else
		reply = MSG(1) | ((u64)(size >> 12) << 44) | dva;
	return send(s, ep, reply);
}

int dcpext_session_receive(struct dcpext_session *s, unsigned int ep, u64 msg)
{
	unsigned int type;
	int ret = dcpext_session_poll(s);

	if (ret)
		return ret;
	if (s->phase == DCPEXT_QUIESCED || (!s->runtime &&
	    ++s->received > (s->message_limit ? s->message_limit : MAX_MESSAGES)))
		return fail(s, -EPROTO);
	if (!ep)
		return management(s, msg);
	/* This diagnostic never starts any application endpoint. */
	if (ep >= 32 || !(s->started & (1U << ep)))
		return fail(s, -EPROTO);
	type = ep == 8 ? msg >> 56 : (msg >> 52) & 0xff;
	if ((ep == 1 || ep == 2 || ep == 4 || ep == 8) && type == 1)
		return get_buffer(s, ep, msg);
	/* AppleDCP 1041.120.7 transport at image 0x34c94 consumes a type-2
	 * reply as the log ring's read counter and clears its outstanding bit.
	 * Without this reply the AP power task waits forever in 0x34708.
	 * Discard the log contents, acknowledging precisely the notified counter;
	 * it is not an address, byte extent, or proof of display/DMA retirement.
	 * Only channel zero has a qualified buffer in this session engine.
	 */
	if (ep == 8) {
		s->oslog_notices++;
		if (type == 2) {
			if (!s->buffers[8].ready || (msg & 0x00ffffff00000000ULL))
				return fail(s, -EPROTO);
			return send(s, ep, msg);
		}
		/* Types 3/4/5 describe log metadata and require no reply. */
		return 0;
	}
	/* System messages are serviced during both boot and shutdown. Syslog
	 * payload is deliberately not dereferenced by this diagnostic.
	 */
	if (ep == 2 && type == 8)
		return 0;
	if ((ep == 2 && type == 5) || (ep == 4 && (type == 8 || type == 0xc)))
		return send(s, ep, msg);
	return fail(s, -EOPNOTSUPP);
}

int dcpext_session_quiesce(struct dcpext_session *s)
{
	int ret = dcpext_session_poll(s);

	if (ret)
		return ret;
	/* Reject the old endpoint-only attempt without sending IOP quiesce. */
	if (s->phase != DCPEXT_RUNNING)
		return -EAGAIN;
	if (s->runtime) {
		ret = dcpext_session_leave_runtime(s);
		if (ret)
			return ret;
	}
	s->phase = DCPEXT_AP_QUIESCE;
	return send(s, 0, MSG(0xb) | 0x10);
}

int dcpext_session_finish_quiesce(struct dcpext_session *s)
{
	int ret = dcpext_session_poll(s);

	if (ret)
		return ret;
	if (s->phase != DCPEXT_AP_QUIESCE || !s->ap_quiesced)
		return -EAGAIN;
	s->phase = DCPEXT_IOP_QUIESCE;
	return send(s, 0, MSG(6) | (s->sleep_iop ? 1 : 0x10));
}
