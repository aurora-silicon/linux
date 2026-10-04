// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Host-only protocol tests. Synthetic requests after the captured EPMAP;
 * no claim that external firmware has completed this conversation.
 */
#include "dcpext_session.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define MSG(t) ((u64)(t) << 52)
struct message { u8 ep; u64 word; };
struct harness {
	struct dcpext_session s;
	u64 now;
	struct message tx[1024];
	unsigned int n, buffers, fail_at, buffer_fault;
};

static u64 now_ms(void *cookie)
{
	return ((struct harness *)cookie)->now;
}

static int send_message(void *cookie, u8 ep, u64 msg)
{
	struct harness *h = cookie;
	assert(h->n < 1024);
	h->tx[h->n++] = (struct message){ep, msg};
	return h->fail_at && h->n == h->fail_at ? -EIO : 0;
}

static int prepare_buffer(void *cookie, u8 ep, u64 requested, u32 size, u64 *dva)
{
	struct harness *h = cookie;
	assert(ep == 1 || ep == 2 || ep == 4 || ep == 8);
	assert(size && size <= 0x100000);
	h->buffers++;
	if (h->buffer_fault == 1)
		return -ENOMEM;
	*dva = requested ? requested : 0x10040000000ULL + (u64)ep * 0x100000;
	if (h->buffer_fault == 2)
		*dva += 1;
	if (h->buffer_fault == 3)
		*dva = 0x11000000000ULL - 0x1000;
	if (h->buffer_fault == 4)
		h->now = h->s.deadline;
	if (h->buffer_fault == 5)
		*dva = 0x10041000000ULL; /* changed inherited address */
	if (h->buffer_fault == 6)
		*dva = 0x40000000; /* table address without the DCP wire bias */
	return 0;
}

static const struct dcpext_session_ops ops = {
	.now_ms = now_ms, .send = send_message, .buffer = prepare_buffer,
};

static void begin(struct harness *h)
{
	memset(h, 0, sizeof(*h));
	h->now = 100;
	assert(dcpext_session_begin(&h->s, &ops, h) == 0);
	assert(h->s.touched && h->s.phase == DCPEXT_HELLO);
	assert(h->n == 1 && h->tx[0].word == 0x60000000000220ULL);
}

static void rx(struct harness *h, u8 ep, u64 msg)
{
	assert(dcpext_session_receive(&h->s, ep, msg) == 0);
}

static void epmap(struct harness *h, bool early_iop)
{
	/* Actual J514S external hello and system map from session-1. */
	rx(h, 0, 0x100000000c000cULL);
	assert(h->tx[1].word == 0x200000000c000cULL);
	rx(h, 0, 0x8000000000011fULL);
	assert(h->tx[2].word == 0x80000000000001ULL);
	if (early_iop)
		rx(h, 0, MSG(7) | 0x20);
	assert(dcpext_session_quiesce(&h->s) == -EAGAIN);
	assert(h->n == 3); /* specifically no premature IOP-quiesce message */
	rx(h, 0, 0x88000100800fffULL);
	assert(h->tx[3].word == 0x88000100000000ULL);
	const unsigned int endpoints[] = {1, 2, 3, 4, 8};
	for (unsigned int i = 0; i < sizeof(endpoints) / sizeof(*endpoints); i++)
		assert(h->tx[4 + i].word == (MSG(5) | (u64)endpoints[i] << 32 | 2));
	assert(h->s.started == 0x11e);
	assert(h->s.endpoints[1] == 0x00800fff);
	assert(h->s.phase == (early_iop ? DCPEXT_AP_ON : DCPEXT_IOP_ON));
}

static void system_buffers(struct harness *h)
{
	unsigned int n = h->n;
	/* Synthetic conversation based on Linux's system endpoint definitions.
	 * The firmware-owned DVA values below are test inputs, not hardware reads.
	 */
	rx(h, 1, MSG(1) | (16ULL << 44) | 0x10006631000ULL);
	assert(h->n == n && h->s.buffers[1].inherited);
	rx(h, 2, MSG(1) | (4ULL << 44));
	assert(h->tx[n].ep == 2);
	assert(h->tx[n++].word == (MSG(1) | (4ULL << 44) | 0x10040200000ULL));
	rx(h, 4, MSG(1) | (3ULL << 44));
	assert(h->tx[n].ep == 4);
	assert(h->tx[n++].word == (MSG(1) | (3ULL << 44) | 0x10040400000ULL));
	rx(h, 8, (1ULL << 56) | (0x24000ULL << 36) | (0x103e1b32000ULL >> 12));
	assert(h->n == n && h->s.buffers[8].size == 0x24000);
	assert(h->s.buffers[8].dva == 0x103e1b32000ULL && h->s.buffers[8].inherited);
	rx(h, 2, MSG(8) | (128ULL << 24) | 16);
	rx(h, 2, MSG(5) | 3);
	assert(h->tx[n].ep == 2 && h->tx[n++].word == (MSG(5) | 3));
	rx(h, 4, MSG(8) | 123);
	assert(h->tx[n].ep == 4 && h->tx[n++].word == (MSG(8) | 123));
	rx(h, 4, MSG(0xc) | 321);
	assert(h->tx[n].ep == 4 && h->tx[n++].word == (MSG(0xc) | 321));
	assert(h->n == n && h->buffers == 4);
}

static void running(struct harness *h)
{
	begin(h);
	epmap(h, false);
	system_buffers(h);
	rx(h, 0, MSG(7) | 0x20);
	assert(h->s.phase == DCPEXT_AP_ON);
	assert(h->tx[h->n - 1].word == (MSG(0xb) | 0x20));
	rx(h, 0, MSG(0xb) | 0x20);
	assert(h->s.phase == DCPEXT_RUNNING);
}

static void quiesce(struct harness *h)
{
	unsigned int n = h->n;
	assert(dcpext_session_quiesce(&h->s) == 0);
	assert(h->s.phase == DCPEXT_AP_QUIESCE);
	assert(h->tx[n++].word == (MSG(0xb) | 0x10));
	/* Logging can still arrive while AP quiescence is pending. */
	rx(h, 4, MSG(0xc) | 0x55);
	assert(h->tx[n++].word == (MSG(0xc) | 0x55));
	rx(h, 0, MSG(0xb) | 0x10);
	assert(h->s.phase == DCPEXT_IOP_QUIESCE);
	assert(h->tx[n++].word == (MSG(6) | 0x10));
	rx(h, 0, MSG(7) | 0x10);
	assert(h->s.phase == DCPEXT_QUIESCED && h->n == n);
	assert(h->buffers == 4); /* engine never discards buffer ownership */
}

static void sticky_failure(struct harness *h, int error)
{
	unsigned int n = h->n;
	assert(h->s.phase == DCPEXT_FAILED && h->s.error == error);
	assert(dcpext_session_receive(&h->s, 0, MSG(7) | 0x10) == error);
	assert(dcpext_session_quiesce(&h->s) == error);
	assert(dcpext_session_poll(&h->s) == error);
	assert(h->n == n && h->s.phase == DCPEXT_FAILED);
}

int main(void)
{
	struct harness h;
	unsigned int n;

	/* AP acknowledgement permits channel shutdown, never DMA release. */
	running(&h);
	h.s.defer_iop_quiesce = true;
	assert(dcpext_session_finish_quiesce(&h.s) == -EAGAIN);
	assert(!dcpext_session_quiesce(&h.s));
	n = h.n;
	assert(dcpext_session_finish_quiesce(&h.s) == -EAGAIN);
	rx(&h, 0, MSG(0xb) | 0x10);
	assert(h.s.ap_quiesced && h.s.phase == DCPEXT_AP_QUIESCE && h.n == n);
	rx(&h, 4, MSG(0xc) | 0x55);
	assert(!dcpext_session_finish_quiesce(&h.s));
	assert(h.s.phase == DCPEXT_IOP_QUIESCE);
	rx(&h, 0, MSG(7) | 0x10);
	assert(h.s.phase == DCPEXT_QUIESCED);
	running(&h);
	h.s.defer_iop_quiesce = true;
	assert(!dcpext_session_quiesce(&h.s));
	rx(&h, 0, MSG(0xb) | 0x10);
	assert(dcpext_session_receive(&h.s, 0, MSG(0xb) | 0x10) == -EPROTO);
	sticky_failure(&h, -EPROTO);
	assert(dcpext_session_finish_quiesce(&h.s) == -EPROTO);
	puts("PASS: deferred IOP waits for AP ACK; duplicate ACK fails with retained ownership");

	running(&h);
	h.s.sleep_iop = true;
	assert(!dcpext_session_quiesce(&h.s));
	rx(&h, 0, MSG(0xb) | 0x10);
	assert(h.tx[h.n - 1].word == (MSG(6) | 1));
	rx(&h, 0, MSG(7) | 1);
	assert(h.s.phase == DCPEXT_QUIESCED);
	running(&h);
	h.s.sleep_iop = true;
	assert(!dcpext_session_quiesce(&h.s));
	rx(&h, 0, MSG(0xb) | 0x10);
	assert(dcpext_session_receive(&h.s, 0, MSG(7) | 0x10) == -EPROTO);
	sticky_failure(&h, -EPROTO);
	puts("PASS: restartable sleep requires AP quiescence then exact IOP sleep ACK; wrong ACK retains buffers");

	running(&h);
	for (unsigned int type = 0; type < 256; type++)
		if (type != 1) {
			running(&h);
			n = h.n;
			rx(&h, 8, (u64)type << 56 | 0x3060);
			assert(h.n == n + (type == 2) && h.buffers == 4 && h.s.oslog_notices == 1);
			if (type == 2)
				assert(h.tx[n].ep == 8 && h.tx[n].word == 0x0200000000003060ULL);
		}
	/* The absolute message bound must still terminate an OSLog flood. */
	running(&h);
	int flood_result = 0;
	for (unsigned int i = 0; i < 300 && !flood_result; i++)
		flood_result = dcpext_session_receive(&h.s, 8, 0x0200000000003060ULL);
	assert(flood_result == -EPROTO);
	sticky_failure(&h, -EPROTO);
	/* Exact captured notification must advance the firmware's read counter. */
	running(&h);
	n = h.n;
	rx(&h, 8, 0x0200000000003060ULL);
	assert(h.n == n + 1 && h.buffers == 4 && h.s.oslog_notices == 1);
	assert(h.tx[n].ep == 8 && h.tx[n].word == 0x0200000000003060ULL);
	puts("PASS: captured OSLog type 2 acknowledged; metadata opaque, no dereference, flood bound preserved");
	quiesce(&h);
	puts("PASS: captured hello/map, system starts, buffer layouts/ACKs, AP then IOP quiescence");
	assert(dcpext_session_receive(&h.s, 2, MSG(5)) == -EPROTO);
	sticky_failure(&h, -EPROTO); /* unexpected traffic invalidates completion */

	/* Read counters wrap and are cumulative, not offsets into the buffer. */
	running(&h);
	assert(!dcpext_session_quiesce(&h.s));
	const u64 counters[] = {0x31e1, 0x379b, 0xffffffff, 0};
	for (unsigned int i = 0; i < sizeof(counters) / sizeof(*counters); i++) {
		u64 notice = (2ULL << 56) | counters[i];
		n = h.n;
		rx(&h, 8, notice);
		assert(h.n == n + 1 && h.tx[n].ep == 8 && h.tx[n].word == notice);
		assert(h.s.phase == DCPEXT_AP_QUIESCE && !h.s.ap_quiesced);
	}
	running(&h);
	h.fail_at = h.n + 1;
	assert(dcpext_session_receive(&h.s, 8, 0x02000000000031e1ULL) == -EIO);
	assert(h.s.buffers[8].ready);
	sticky_failure(&h, -EIO);
	begin(&h);
	epmap(&h, false);
	n = h.n;
	assert(dcpext_session_receive(&h.s, 8, 0x02000000000031e1ULL) == -EPROTO);
	assert(h.n == n);
	for (unsigned int bit = 32; bit < 56; bit++) {
		running(&h);
		n = h.n;
		assert(dcpext_session_receive(&h.s, 8, (2ULL << 56) | (1ULL << bit)) == -EPROTO);
		assert(h.n == n);
		sticky_failure(&h, -EPROTO);
	}
	puts("PASS: OSLog drain during AP shutdown, counter wrap, missing buffer, malformed fields and uncertain ACK failure");

	begin(&h);
	epmap(&h, true);
	system_buffers(&h);
	rx(&h, 0, MSG(0xb) | 0x20);
	quiesce(&h);
	puts("PASS: IOP acknowledgement before last map, with AP start deferred until system starts");

	begin(&h);
	epmap(&h, false);
	rx(&h, 8, (1ULL << 56) | (0x23456ULL << 36));
	assert(h.s.buffers[8].size == 0x23456 && !h.s.buffers[8].inherited);
	assert(h.tx[h.n - 1].word == ((1ULL << 56) | (0x23456ULL << 36) | (0x10040800000ULL >> 12)));
	puts("PASS: OSLog byte size and page-shifted address, including non-page-multiple size");
	begin(&h);
	epmap(&h, false);
	/* Buffer size/address and notification words from the retained INTERNAL
	 * J713 kernel-client log. External firmware has not supplied these yet.
	 */
	n = h.n;
	rx(&h, 8, (1ULL << 56) | (0x5ff8ULL << 36) | (0x103e1b32000ULL >> 12));
	rx(&h, 8, 0x3010042100000a1ULL);
	rx(&h, 8, 0x4000020d51e4021ULL);
	rx(&h, 8, 0x4000040d28078a1ULL);
	rx(&h, 8, 0x4000060d51e4001ULL);
	assert(h.n == n && h.s.oslog_notices == 4);
	assert(h.s.buffers[8].dva == 0x103e1b32000ULL && h.s.buffers[8].size == 0x5ff8);
	puts("PASS: saved internal-DCP OSLog buffer fields/notices; no dereference, reply or false protocol failure");

	/* Fault every TX in the complete conversation. Delivery is uncertain;
	 * the engine must never report quiescence after any failed send.
	 */
	running(&h);
	quiesce(&h);
	n = h.n;
	for (unsigned int failed = 1; failed <= n; failed++) {
		struct harness recorded = h, f = {0};
		f.fail_at = failed;
		int ret = dcpext_session_begin(&f.s, &ops, &f);
		const struct message input[] = {
			{0, 0x100000000c000cULL}, {0, 0x8000000000011fULL},
			{0, 0x88000100800fffULL},
			{1, MSG(1) | (16ULL << 44) | 0x10006631000ULL},
			{2, MSG(1) | (4ULL << 44)}, {4, MSG(1) | (3ULL << 44)},
			{8, (1ULL << 56) | (0x24000ULL << 36) | (0x103e1b32000ULL >> 12)},
			{2, MSG(8) | (128ULL << 24) | 16}, {2, MSG(5) | 3},
			{4, MSG(8) | 123}, {4, MSG(0xc) | 321},
			{0, MSG(7) | 0x20}, {0, MSG(0xb) | 0x20},
			{4, MSG(0xc) | 0x55}, {0, MSG(0xb) | 0x10}, {0, MSG(7) | 0x10},
		};
		for (unsigned int i = 0; !ret && i < sizeof(input) / sizeof(*input); i++) {
			if (i == 13)
				ret = dcpext_session_quiesce(&f.s);
			if (!ret)
				ret = dcpext_session_receive(&f.s, input[i].ep, input[i].word);
		}
		assert(ret == -EIO && f.n == failed && f.s.touched);
		for (unsigned int i = 0; i < f.n; i++)
			assert(f.tx[i].ep == recorded.tx[i].ep && f.tx[i].word == recorded.tx[i].word);
		sticky_failure(&f, -EIO);
	}
	printf("PASS: delivery failure injected at all %u sends; failures remain sticky\n", n);

	for (unsigned int fault = 1; fault <= 6; fault++) {
		begin(&h);
		epmap(&h, false);
		h.buffer_fault = fault;
		n = h.n;
		int ret = dcpext_session_receive(&h.s, 1, MSG(1) | (16ULL << 44) | 0x10006631000ULL);
		assert(ret == (fault == 1 ? -ENOMEM : fault == 4 ? -ETIMEDOUT : -EINVAL));
		assert(h.n == n && h.buffers == 1);
		if (fault != 1)
			assert(h.s.buffers[1].ready);
		sticky_failure(&h, ret);
	}
	puts("PASS: buffer allocation/validation/timeouts retain failure state and suppress replies");

	const u64 bad_requests[] = {MSG(1), MSG(1) | (1ULL << 44) | (1ULL << 36),
		MSG(1) | (1ULL << 44) | 0x40000001, MSG(1) | (2ULL << 44) | 0x10ffffff000ULL};
	for (unsigned int i = 0; i < sizeof(bad_requests) / sizeof(*bad_requests); i++) {
		begin(&h);
		epmap(&h, false);
		assert(dcpext_session_receive(&h.s, 2, bad_requests[i]) == -EINVAL);
		assert(!h.buffers);
		sticky_failure(&h, -EINVAL);
	}
	for (unsigned int ep = 1; ep <= 8; ep++) {
		if (ep != 1 && ep != 2 && ep != 4 && ep != 8)
			continue;
		running(&h);
		int ret = dcpext_session_receive(&h.s, ep, ep == 8 ? 1ULL << 56 : MSG(1));
		assert(ret == (ep == 1 ? -EIO : -EPROTO) && h.buffers == 4);
		sticky_failure(&h, ret);
	}
	puts("PASS: zero/out-of-aperture/unaligned/cross-boundary requests; crash and duplicate buffers");

	/* Expire every phase where firmware may still own shared memory. */
	for (unsigned int phase = DCPEXT_HELLO; phase <= DCPEXT_IOP_QUIESCE; phase++) {
		begin(&h);
		if (phase >= DCPEXT_EPMAP)
			rx(&h, 0, 0x100000000c000cULL);
		if (phase >= DCPEXT_IOP_ON)
			rx(&h, 0, 0x8800000000011fULL);
		if (phase >= DCPEXT_AP_ON)
			rx(&h, 0, MSG(7) | 0x20);
		if (phase >= DCPEXT_RUNNING)
			rx(&h, 0, MSG(0xb) | 0x20);
		if (phase >= DCPEXT_AP_QUIESCE)
			assert(dcpext_session_quiesce(&h.s) == 0);
		if (phase >= DCPEXT_IOP_QUIESCE)
			rx(&h, 0, MSG(0xb) | 0x10);
		assert(h.s.phase == phase);
		h.now = h.s.deadline;
		assert(dcpext_session_poll(&h.s) == -ETIMEDOUT);
		sticky_failure(&h, -ETIMEDOUT);
	}
	puts("PASS: fixed deadline covers every live phase; late quiesce ACK never permits cleanup");
	/* Explicit observation budget stays finite and cannot be renewed. */
	begin(&h);
	h.s = (struct dcpext_session){.duration_ms = 60001};
	assert(dcpext_session_begin(&h.s, &ops, &h) == -EINVAL && !h.s.touched);
	h.s = (struct dcpext_session){.duration_ms = 60000};
	assert(!dcpext_session_begin(&h.s, &ops, &h));
	assert(h.s.deadline == h.now + 60000);
	h.now = h.s.deadline;
	assert(dcpext_session_poll(&h.s) == -ETIMEDOUT);
	sticky_failure(&h, -ETIMEDOUT);

	begin(&h);
	h.now--;
	assert(dcpext_session_poll(&h.s) == -ERANGE);
	sticky_failure(&h, -ERANGE);
	begin(&h);
	assert(dcpext_session_receive(&h.s, 0, 0x100000000d000dULL) == -EPROTO);
	begin(&h);
	rx(&h, 0, 0x100000000c000cULL);
	assert(dcpext_session_receive(&h.s, 0, 0x8800000000059fULL) == -EOPNOTSUPP);
	begin(&h);
	rx(&h, 0, 0x100000000c000cULL);
	rx(&h, 0, 0x8000000000011fULL);
	assert(dcpext_session_receive(&h.s, 0, 0x8800000000011fULL) == -EPROTO);
	running(&h);
	assert(dcpext_session_quiesce(&h.s) == 0);
	assert(dcpext_session_receive(&h.s, 0, MSG(7) | 0x10) == -EPROTO);
	sticky_failure(&h, -EPROTO); /* IOP quiescence before AP is not sufficient */
	running(&h);
	assert(dcpext_session_receive(&h.s, 0x28, 0) == -EPROTO);
	assert(dcpext_session_begin(&h.s, &ops, &h) == -EBUSY);
	running(&h);
	while (h.s.received < 256)
		rx(&h, 2, MSG(5));
	assert(dcpext_session_receive(&h.s, 2, MSG(5)) == -EPROTO);
	sticky_failure(&h, -EPROTO);
	puts("PASS: backwards clock, version/profile/map mismatch, wrong ACK order, unsolicited app EP, reuse and message flood");
	/* The retained 40-surface run reached message 257 during power-off.
	 * Its explicit larger budget still terminates a flood and cannot extend
	 * the absolute deadline. Native display plus AV startup can also use
	 * this budget, without extending its 16-second deadline. */
	begin(&h);
	h.s = (struct dcpext_session){.message_limit=512};
	assert(dcpext_session_begin(&h.s,&ops,&h)==-EINVAL && !h.s.touched);
	h.s = (struct dcpext_session){.duration_ms=16000,.message_limit=512};
	assert(!dcpext_session_begin(&h.s,&ops,&h));
	assert(h.s.deadline == h.now + 16000);
	h.now += 16000;
	assert(dcpext_session_poll(&h.s) == -ETIMEDOUT);
	h.s = (struct dcpext_session){.duration_ms=16000,.message_limit=8192};
	assert(!dcpext_session_begin(&h.s,&ops,&h));
	assert(h.s.deadline == h.now + 16000);
	h.s = (struct dcpext_session){.duration_ms=60000,.message_limit=8192};
	assert(dcpext_session_begin(&h.s,&ops,&h)==-EINVAL && !h.s.touched);
	h.s = (struct dcpext_session){.duration_ms=15000,.message_limit=512};
	assert(dcpext_session_begin(&h.s,&ops,&h)==-EINVAL && !h.s.touched);
	h.s = (struct dcpext_session){.duration_ms=16000};
	assert(!dcpext_session_begin(&h.s,&ops,&h));
	h.s = (struct dcpext_session){.duration_ms=60000,.message_limit=513};
	assert(dcpext_session_begin(&h.s,&ops,&h)==-EINVAL && !h.s.touched);
	h.n = 0;
	h.s = (struct dcpext_session){.duration_ms=60000,.message_limit=512};
	assert(!dcpext_session_begin(&h.s,&ops,&h));
	epmap(&h,false);
	system_buffers(&h);
	rx(&h,0,MSG(7)|0x20);rx(&h,0,MSG(0xb)|0x20);
	while(h.s.received<512) rx(&h,8,0x0200000000003060ULL);
	assert(dcpext_session_receive(&h.s,8,0x0200000000003060ULL)==-EPROTO);
	sticky_failure(&h,-EPROTO);
	puts("PASS: explicit 60s surface-trial message budget accepts message 257, rejects 513 and preserves sticky failure");
	/* A desktop becomes persistent only after qualified startup; leaving it
	 * reestablishes one bounded teardown, and floods still fail permanently. */
	running(&h);
	assert(!dcpext_session_enter_runtime(&h.s));
	assert(dcpext_session_enter_runtime(&h.s) == -EINVAL);
	for (unsigned int second = 0; second < 120; second++) {
		h.now += 1000;
		assert(!dcpext_session_poll(&h.s));
		for (unsigned int m = 0; m < 512; m++)
			assert(!dcpext_session_runtime_message(&h.s));
	}
	assert(!dcpext_session_leave_runtime(&h.s));
	assert(!h.s.runtime && h.s.deadline == h.now + 8000);
	assert(dcpext_session_leave_runtime(&h.s) == -EINVAL);
	h.now += 8000;
	assert(dcpext_session_poll(&h.s) == -ETIMEDOUT);
	assert(dcpext_session_enter_runtime(&h.s) == -ETIMEDOUT);
	running(&h);assert(!dcpext_session_enter_runtime(&h.s));
	for (unsigned int m = 0; m < 8192; m++)
		assert(!dcpext_session_runtime_message(&h.s));
	assert(dcpext_session_runtime_message(&h.s) == -EOVERFLOW);
	h.now += 2000;assert(dcpext_session_poll(&h.s) == -EOVERFLOW);
	running(&h);assert(!dcpext_session_enter_runtime(&h.s));
	assert(!dcpext_session_quiesce(&h.s));
	assert(!h.s.runtime && h.s.phase == DCPEXT_AP_QUIESCE);
	h.now += 8000;assert(dcpext_session_poll(&h.s) == -ETIMEDOUT);
	begin(&h);assert(dcpext_session_enter_runtime(&h.s) == -EINVAL);
	puts("PASS: explicit desktop admission, sustained runtime, all-endpoint rate bound and bounded teardown");
	puts("OFFLINE ONLY: no mailbox, DMA, external firmware completion, EDID or image was tested.");
	return 0;
}
