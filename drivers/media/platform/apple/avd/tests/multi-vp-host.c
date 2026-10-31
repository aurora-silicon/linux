// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Real host slot helpers and real MIT firmware scheduler, linked on the host. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../avd-queue-internal.h"
#include "../avd-open-protocol.h"
#include "queue.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "run the wire integration test on a little-endian host"
#endif

struct transport {
	struct avd_frame host[AVD_MAX_FRAMES];
	unsigned int vp[AVD_MAX_FRAMES], kicks, completed;
	u32 tags[AVD_MAX_FRAMES];
};
static int feed(void *cookie, struct avdq_frame *f)
{
	struct transport *t = cookie;

	t->vp[f->work.fifo] = f->vp;
	f->pushed = f->desc.program_words;
	return 1;
}
static void kick(void *cookie, uint32_t word)
{
	struct transport *t = cookie;

	assert((word & 0xff000000U) == AVDQ_EXEC);
	t->kicks++;
}
static int complete(void *cookie, unsigned int idx, const struct avdq_frame *f)
{
	struct transport *t = cookie;

	assert(avd_open_completion(avdq_notification(f->desc.tag, idx), f->desc.tag));
	assert(avd_slot_complete(&t->host[idx], f->desc.tag, f->status ? -EIO : 0));
	t->tags[t->completed++] = f->desc.tag;
	return 1;
}
static const struct avdq_ops ops = { .feed = feed, .kick = kick, .complete = complete };
static void submit(struct avdq *q, struct transport *t, unsigned int idx,
		   struct avd_ctx *ctx, u64 seq, unsigned int context, bool independent)
{
	struct avd_frame *h = &t->host[idx];
	struct avd_open_desc wire = {0};
	struct avdq_desc desc;

	h->idx = idx;
	assert(!avd_slot_reserve(h, ctx, seq));
	assert(h->dep == AVD_DEP_PREV_DONE);
	if (independent)
		h->dep = AVD_DEP_NONE;
	wire.magic = AVD_OPEN_MAGIC;
	wire.tag = h->tag;
	wire.codec = AVDQ_HEVC;
	wire.vp_slot = 0;
	wire.pp_kick = AVDQ_EXEC | (idx << 4);
	wire.program_lo = 0x4000;
	wire.program_words = wire.state = 1;
	wire.reserved[0] = AVD_OPEN_FLAG_MULTI_VP;
	wire.reserved[4] = 1;
	wire.reserved[1] = context |
		(h->dep == AVD_DEP_PREV_DONE ? AVD_OPEN_CONTEXT_DEP : 0);
	wire.reserved[2] = 0x1000000 + idx * 0x1000000;
	_Static_assert(sizeof(wire) == sizeof(desc), "host/firmware ABI mismatch");
	memcpy(&desc, &wire, sizeof(desc));
	h->state = AVD_FRAME_SUBMITTED;
	assert(!avdq_submit(q, idx, &desc));
}
int main(void)
{
	struct transport t = {0};
	struct avdq q;
	int context[2];
	unsigned int i;
	u32 chunk;
	u32 info[7] = { AVD_OPEN_MULTI_MAGIC, AVD_OPEN_MULTI_VERSION };

	assert(!avd_open_ring_chunk(4090, 4080, 4096, 20, &chunk) && chunk == 6);
	assert(!avd_open_ring_chunk(0xfffffff0U, 0xffffffe0U, 4096, 100, &chunk) && chunk == 16);
	assert(!avd_open_ring_chunk(0, 0xfffffff0U, 4096, 100, &chunk) && chunk == 100);
	assert(!avd_open_ring_chunk(4096, 0, 4096, 1, &chunk) && !chunk);
	assert(avd_open_ring_chunk(4097, 0, 4096, 1, &chunk) == -EIO);
	assert(avd_open_ring_chunk(0, 0, 6144, 1, &chunk) == -EIO);
	info[6] = AVD_OPEN_FLAG_MULTI_VP;
	assert(!avd_open_has_multi(15, info)); /* Old image, stale extension SRAM. */
	assert(avd_open_has_multi(15 | AVD_OPEN_CAP_MULTI_VP, info));
	info[1]++;
	assert(!avd_open_has_multi(15 | AVD_OPEN_CAP_MULTI_VP, info));

	assert(avd_inflight_limit(7, 16) == 7);
	assert(avd_inflight_limit(15, 16) == 15);
	assert(avd_inflight_limit(15, 1) == 1);
	for (i = 0; i < 7; i++)
		t.host[i].state = AVD_FRAME_SUBMITTED;
	assert(!avd_slot_available(t.host, avd_inflight_limit(7, 16)));
	assert(avd_slot_available(t.host, avd_inflight_limit(15, 16)) == &t.host[7]);
	for (; i < 15; i++)
		t.host[i].state = AVD_FRAME_SUBMITTED;
	assert(!avd_slot_available(t.host, avd_inflight_limit(15, 16)));
	for (i = 0; i < 8; i++) {
		memset(&t, 0, sizeof(t));
		avdq_init(&q, &avdq_variants[i]);
		submit(&q, &t, 0, (void *)&context[0], 1, 100, false);
		submit(&q, &t, 1, (void *)&context[1], 2, 200, false);
		avdq_step(&q, &ops, &t); avdq_step(&q, &ops, &t);
		assert(q.frame[0].phase == AVDQ_VP && q.frame[1].phase == AVDQ_VP);
		assert(t.vp[0] == 0 && t.vp[1] == 1);
		/* Two concurrent contexts on distinct VPs and distinct hardware FIFOs. */
		avdq_vp_done(&q, 1); avdq_step(&q, &ops, &t);
		avdq_pp_done(&q); avdq_step(&q, &ops, &t);
		assert(t.host[1].state == AVD_FRAME_DONE && t.host[0].state == AVD_FRAME_SUBMITTED);
		assert(avd_slot_first(t.host, &t.host[1]));
		avdq_vp_done(&q, 0); avdq_step(&q, &ops, &t);
		avdq_pp_done(&q); avdq_step(&q, &ops, &t);
		assert(t.completed == 2 && t.tags[0] == 33 && t.tags[1] == 16);
	}
	memset(&t, 0, sizeof(t));
	avdq_init(&q, &avdq_variants[1]);
	submit(&q, &t, 0, (void *)&context[0], 1, 100, true);
	submit(&q, &t, 1, (void *)&context[0], 2, 100, true);
	avdq_step(&q, &ops, &t); avdq_step(&q, &ops, &t);
	avdq_vp_done(&q, 1); avdq_step(&q, &ops, &t);
	avdq_pp_done(&q); avdq_step(&q, &ops, &t);
	assert(!t.completed && !avd_slot_first(t.host, &t.host[1]));
	avdq_vp_done(&q, 0); avdq_step(&q, &ops, &t);
	avdq_pp_done(&q); avdq_step(&q, &ops, &t); avdq_step(&q, &ops, &t);
	assert(t.completed == 2 && t.tags[0] == 16 && t.tags[1] == 33);
	assert(avd_slot_first(t.host, &t.host[0]));
	t.host[0].state = AVD_FRAME_FREE;
	assert(avd_slot_first(t.host, &t.host[1]));
	puts("PASS: two contexts on VP0/VP1, eight images, FIFO caps and ordered tags");
	return 0;
}
