// SPDX-License-Identifier: GPL-2.0
/* Deterministic host fixture; scheduler, MMIO, PM and workqueues are mocks. */
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "avd-request.h"

#define __maybe_unused
#define __iomem
#define BIT(n) (1U << (n))
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v, x) ((v) = (x))
/* Deterministic ordering only; these mocks do not model SMP barriers. */
#define smp_store_release(p, x) (*(p) = (x))
#define smp_load_acquire(p) (*(p))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define spin_lock_irqsave(l, f) \
	do {                    \
		(void)(l);      \
		(f) = 0;        \
	} while (0)
#define spin_unlock_irqrestore(l, f) \
	do {                         \
		(void)(l);           \
		(void)(f);           \
	} while (0)
#define dev_err(...) ((void)0)
#define WARN_ON_ONCE(x) (x)
#define kvfree(p) free(p)
#define wait_event(q, cond)          \
	do {                         \
		(void)(q);           \
		if (!(cond))         \
			wait_hook(); \
		assert(cond);        \
	} while (0)
#define wake_up_all(q) ((void)(q))
#define msecs_to_jiffies(n) (n)
#define IRQ_HANDLED 1
#define VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF 1
#define AVD_QUIRK_V5_QUEUE_PACING BIT(3)
#define AVD_OP_EXEC 0x2b000000U
#define AVD_OP_EXEC_REV3_VP9_MASK 0x00fff000U
#define AVD_OP_EXEC_FIFO_IDX(n) ((n) << 4)
#define AVD_OP_EXEC_FLAG_START_REV3(n) ((n) << 8)
#define AVD_OP_EXEC_FLAG_START_REV4(n) ((n) << 9)
#define AVD_OP_EXEC_FLAG_END(n) ((n) << 10)
#include "avd-regs.h"

typedef int irqreturn_t;
typedef uintptr_t dma_addr_t;
enum vb2_buffer_state { VB2_BUF_STATE_DONE, VB2_BUF_STATE_ERROR };
enum avd_codec { AVD_CODEC_HEVC, AVD_CODEC_H264, AVD_CODEC_VP9, AVD_CODEC_AV1 };
struct work_struct {
	bool queued;
};
struct delayed_work {
	struct work_struct work;
};
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
struct vb2_v4l2_buffer {
	int id, done, result;
	bool held;
};
struct v4l2_m2m_ctx {
	struct {
		struct {
			unsigned int subsystem_flags;
		} q;
	} out_q_ctx;
	struct vb2_v4l2_buffer *src[4], *dst[4];
	int ns, nd, finishes;
};
struct avd_ctx;
struct avd_dev;
struct avd_variant {
	unsigned int revision, quirks, vp_slots[4], vp_slot_base[4];
	unsigned int vp_slot_offset;
	void (*configure_stream)(struct avd_dev *dev, dma_addr_t addr, u8 fifo,
				 u32 vp);
};
struct avd_dev {
	struct avd_ctx *job_ctx;
	u64 job_seq;
	int job_lock, hw_wait, irq;
	bool broken;
	void *dev, *m2m_dev, *rstc, *empty_domain;
	unsigned char *ctrl, *mbox;
	const struct avd_variant *variant;
};
struct device {
	struct avd_dev *data;
};
#define to_platform_device(d) (d)
#define platform_get_drvdata(d) ((d)->data)
struct avd_coded_fmt_ops {
	int (*run)(struct avd_ctx *ctx);
	void (*submit)(struct avd_ctx *ctx);
	void (*done)(struct avd_ctx *ctx, struct vb2_v4l2_buffer *src,
		     struct vb2_v4l2_buffer *dst, enum vb2_buffer_state state);
};
struct avd_coded_fmt_desc {
	const struct avd_coded_fmt_ops *ops;
};
struct avd_segment {
	size_t num;
	u32 instructions[512];
};
struct avd_job {
	enum avd_codec codec;
	size_t num;
	struct avd_segment *segments;
};
struct avd_ctx {
	struct {
		struct v4l2_m2m_ctx *m2m_ctx;
	} fh;
	struct avd_dev *dev;
	const struct avd_coded_fmt_desc *coded_fmt_desc;
	struct delayed_work watchdog_work;
	struct work_struct finish_work;
	int request_wait;
	bool active, scheduler_released, pm_ref;
	struct avd_request request;
	u64 wd_seq;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct avd_job job;
	int fifo_idx;
	struct {
		dma_addr_t addr;
	} inst;
	int kicks, callbacks, run_mode;
};
static struct avd_ctx *current, *waiting_owner, *draining;
static int pm_refs, pm_error, resets, reset_error, writes, configures;
static int queue_full, early_irq, seq_log[16], nlog, wait_calls, boots;
static u32 irq_status;
static unsigned char *mmio_base;
static int mailbox_words, pops;
#define readl_poll_timeout_atomic(reg, v, cond, delay, timeout) \
	({                                                      \
		int rc = -EIO;                                  \
		for (int j = 0; j < (timeout); j++) {           \
			(v) = readl(reg);                       \
			if (cond) {                             \
				rc = 0;                         \
				break;                          \
			}                                       \
		}                                               \
		rc;                                             \
	})
static void wait_hook(void);
static void inject(struct avd_dev *, u32);
static void pump(struct avd_ctx *);
static void writel(u32 value, void *reg)
{
	(void)reg;
	writes++;
	if (early_irq && (value & 0xff000000U) == AVD_OP_EXEC &&
	    (value & BIT(10))) {
		early_irq = 0;
		inject(current->dev, 0x100 | current->request.vp);
		inject(current->dev, 0x1000);
	}
}
static u32 readl(void *reg)
{
	if (reg == mmio_base + AVD_REG_MBOX1_STATUS)
		return mailbox_words ? 0 : AVD_MBOX1_STATUS_EMPTY;
	assert(reg == mmio_base + AVD_REG_MBOX1_RETRIEVE && mailbox_words > 0);
	mailbox_words--;
	pops++;
	return irq_status;
}
static int schedule_work(struct work_struct *w)
{
	assert(!w->queued);
	w->queued = true;
	return 1;
}
static int schedule_delayed_work(struct delayed_work *w, int delay)
{
	(void)delay;
	assert(!w->work.queued);
	w->work.queued = true;
	return 1;
}
static void cancel_delayed_work_sync(struct delayed_work *w)
{
	w->work.queued = false;
}
static void synchronize_irq(int irq)
{
	(void)irq;
}
static int reset_control_reset(void *r)
{
	(void)r;
	resets++;
	return reset_error;
}
static int iommu_attach_device(void *domain, void *dev)
{
	(void)domain;
	(void)dev;
	return 0;
}
static void iommu_detach_device(void *domain, void *dev)
{
	(void)domain;
	(void)dev;
}
static void avd_shutdown(struct avd_dev *d)
{
	(void)d;
}
static int avd_boot(struct avd_dev *d)
{
	(void)d;
	boots++;
	return 0;
}
static int pm_runtime_resume_and_get(void *d)
{
	(void)d;
	if (pm_error)
		return -EIO;
	return ++pm_refs;
}
static void pm_runtime_put_autosuspend(void *d)
{
	(void)d;
	assert(pm_refs > 0);
	pm_refs--;
}
static struct vb2_v4l2_buffer *v4l2_m2m_next_src_buf(struct v4l2_m2m_ctx *m)
{
	return m->ns ? m->src[0] : NULL;
}
static struct vb2_v4l2_buffer *v4l2_m2m_next_dst_buf(struct v4l2_m2m_ctx *m)
{
	return m->nd ? m->dst[0] : NULL;
}
static struct vb2_v4l2_buffer *remove_buf(struct vb2_v4l2_buffer **a, int *n)
{
	assert(*n > 0);
	struct vb2_v4l2_buffer *b = a[0];
	memmove(a, a + 1, --*n * sizeof(*a));
	return b;
}
static struct vb2_v4l2_buffer *v4l2_m2m_src_buf_remove(struct v4l2_m2m_ctx *m)
{
	return remove_buf(m->src, &m->ns);
}
static struct vb2_v4l2_buffer *v4l2_m2m_dst_buf_remove(struct v4l2_m2m_ctx *m)
{
	return remove_buf(m->dst, &m->nd);
}
static void v4l2_m2m_buf_done(struct vb2_v4l2_buffer *b,
			      enum vb2_buffer_state r)
{
	assert(!b->done);
	b->done++;
	b->result = r;
	seq_log[nlog++] = b->id;
}
static void v4l2_m2m_job_finish(void *d, struct v4l2_m2m_ctx *m)
{
	(void)d;
	assert(current && current->fh.m2m_ctx == m);
	m->finishes++;
	current = NULL;
}
static void v4l2_m2m_buf_done_and_job_finish(void *d, struct v4l2_m2m_ctx *m,
					     enum vb2_buffer_state r)
{
	struct vb2_v4l2_buffer *src = v4l2_m2m_src_buf_remove(m);
	if (!src->held)
		v4l2_m2m_buf_done(v4l2_m2m_dst_buf_remove(m), r);
	v4l2_m2m_buf_done(src, r);
	v4l2_m2m_job_finish(d, m);
}
static void v4l2_m2m_try_schedule(struct v4l2_m2m_ctx *m)
{
	(void)m;
}
static int avd_wait_submission_queue(struct avd_ctx *c, int vp)
{
	(void)c;
	(void)vp;
	return queue_full;
}
static void flush_work(struct work_struct *w)
{
	if (w->queued)
		pump(container_of(w, struct avd_ctx, finish_work));
}
#include "actual-driver.h"

static void configure(struct avd_dev *d, dma_addr_t addr, u8 fifo, u32 vp)
{
	(void)addr;
	(void)vp;
	assert(d->job_ctx == current && fifo == 0);
	configures++;
}
static void pp_submit(struct avd_ctx *ctx)
{
	ctx->kicks++;
}
static void codec_done(struct avd_ctx *ctx, struct vb2_v4l2_buffer *src,
		       struct vb2_v4l2_buffer *dst,
		       enum vb2_buffer_state result)
{
	(void)result;
	assert(src == ctx->src_buf && dst == ctx->dst_buf);
	assert(!src->done && !dst->done);
	ctx->callbacks++;
}
static int codec_run(struct avd_ctx *ctx)
{
	if (ctx->run_mode == 1)
		return -EINVAL;
	if (ctx->run_mode == 2) {
		avd_job_finish(ctx, VB2_BUF_STATE_DONE);
		return 0;
	}
	ctx->job.codec = AVD_CODEC_HEVC;
	ctx->job.num = 1;
	ctx->job.segments = calloc(2, sizeof(*ctx->job.segments));
	assert(ctx->job.segments);
	return avd_submit_job(ctx);
}
static void pump(struct avd_ctx *ctx)
{
	assert(ctx->finish_work.queued);
	ctx->finish_work.queued = false;
	avd_finish_work(&ctx->finish_work);
}
static void inject(struct avd_dev *d, u32 status)
{
	irq_status = status;
	mailbox_words = 1;
	avd_irq_handler(0, d);
}
static void complete(struct avd_ctx *ctx)
{
	inject(ctx->dev, 0x100 | ctx->request.vp);
	inject(ctx->dev, 0x1000);
	pump(ctx);
}
static void wait_hook(void)
{
	if (draining) {
		pump(draining);
		draining = NULL;
		return;
	}
	assert(waiting_owner && current && current != waiting_owner);
	assert(waiting_owner->active && current->active);
	assert(!waiting_owner->src_buf->done && !waiting_owner->dst_buf->done);
	assert(configures ==
	       1); /* CPU prepared the second job, no second MMIO. */
	wait_calls++;
	complete(waiting_owner);
	waiting_owner = NULL;
}
static void init_ctx(struct avd_ctx *ctx, struct avd_dev *d,
		     struct v4l2_m2m_ctx *m, struct vb2_v4l2_buffer *b)
{
	static const struct avd_coded_fmt_ops ops = { codec_run, pp_submit,
						      codec_done };
	static const struct avd_coded_fmt_desc desc = { &ops };
	nlog = 0;
	memset(ctx, 0, sizeof(*ctx));
	memset(m, 0, sizeof(*m));
	memset(b, 0, 4 * sizeof(*b));
	ctx->dev = d;
	ctx->fh.m2m_ctx = m;
	ctx->coded_fmt_desc = &desc;
	for (int i = 0; i < 4; i++)
		b[i].id = i;
	m->src[0] = b;
	m->src[1] = b + 2;
	m->ns = 2;
	m->dst[0] = b + 1;
	m->dst[1] = b + 3;
	m->nd = 2;
}
static void start(struct avd_ctx *ctx)
{
	assert(!current && avd_job_ready(ctx));
	current = ctx;
	avd_device_run(ctx);
}
int main(void)
{
	unsigned char regs[256] = { 0 };
	mmio_base = regs;
	struct avd_variant variant = { .revision = 4,
				       .configure_stream = configure };
	struct avd_dev dev = { .ctrl = regs,
			       .mbox = regs,
			       .variant = &variant };
	struct avd_ctx a, b;
	struct v4l2_m2m_ctx ma, mb;
	struct vb2_v4l2_buffer ba[4], bb[4];

	init_ctx(&a, &dev, &ma, ba);
	init_ctx(&b, &dev, &mb, bb);
	start(&a);
	assert(a.active && !avd_job_ready(&a) && a.scheduler_released);
	assert(!ba[0].done && !ba[1].done && ma.ns == 1 && ma.nd == 1);
	assert(dev.job_ctx == &a && !current && pm_refs == 1);
	waiting_owner = &a;
	start(&b); /* Run B's real submission path while A owns the hardware. */
	assert(wait_calls == 1 && !a.active && dev.job_ctx == &b &&
	       pm_refs == 1);
	assert(a.kicks == 1 && b.kicks == 0 && a.callbacks == 1);
	assert(ba[0].done == 1 && ba[1].done == 1 && !ba[2].done &&
	       !ba[3].done);
	assert(!bb[0].done && !bb[1].done && seq_log[0] == 1 &&
	       seq_log[1] == 0);
	complete(&b);
	assert(!dev.job_ctx && pm_refs == 0 && !resets && b.callbacks == 1);
	int prior_pops = pops;
	avd_irq_handler(0, &dev);
	assert(pops == prior_pops);
	inject(&dev, 0x1000); /* No owner: still acknowledge, no completion. */
	assert(!a.finish_work.queued && !b.finish_work.queued);
	puts("PASS: two-context CPU pipeline, owner routing, retained buffers and PM");

	init_ctx(&a, &dev, &ma, ba);
	early_irq = 1;
	start(&a);
	assert(a.request.state == AVD_JOB_FINISHING && !a.scheduler_released);
	assert(!ba[0].done && a.finish_work.queued && current == &a);
	pump(&a);
	assert(!current && pm_refs == 0);
	puts("PASS: IRQ during submission is deferred until run returns");

	init_ctx(&a, &dev, &ma, ba);
	start(&a);
	inject(&dev, 0x101);
	assert(!a.kicks);
	inject(&dev, 0x100);
	inject(&dev, 0x100);
	assert(a.kicks == 1);
	avd_watchdog_func(&a.watchdog_work.work);
	inject(&dev, 0x1000);
	pump(&a);
	assert(resets == 1 && ba[0].result == VB2_BUF_STATE_ERROR &&
	       pm_refs == 0);
	assert(!a.job.segments);
	init_ctx(&a, &dev, &ma, ba);
	start(&a);
	complete(&a);
	assert(ba[0].result == VB2_BUF_STATE_DONE && pm_refs == 0);
	puts("PASS: wrong/duplicate VP, watchdog wins PP race, next decode recovers");

	init_ctx(&a, &dev, &ma, ba);
	a.run_mode = 1;
	a.job.segments = calloc(2, sizeof(*a.job.segments));
	start(&a);
	pump(&a);
	assert(resets == 1 && ba[0].result == VB2_BUF_STATE_ERROR &&
	       pm_refs == 0);
	assert(!a.job.segments);
	init_ctx(&a, &dev, &ma, ba);
	pm_error = 1;
	start(&a);
	pump(&a);
	pm_error = 0;
	assert(resets == 1 && pm_refs == 0);
	puts("PASS: pre-submit and PM failures do not reset another owner or put PM");

	init_ctx(&a, &dev, &ma, ba);
	ma.out_q_ctx.q.subsystem_flags = 1;
	ba[0].held = true;
	a.run_mode = 2;
	a.job.segments = calloc(2, sizeof(*a.job.segments));
	start(&a);
	pump(&a);
	assert(ba[0].done == 1 && !ba[1].done && ma.nd == 2 &&
	       !a.scheduler_released);
	assert(a.job.segments);
	free(a.job.segments);
	a.job.segments = NULL;
	init_ctx(&a, &dev, &ma, ba);
	ma.out_q_ctx.q.subsystem_flags = 1;
	start(&a);
	assert(current == &a && !a.scheduler_released);
	complete(&a);
	assert(!current && ba[1].done && pm_refs == 0);
	puts("PASS: held-capture path retains m2m scheduler and capture buffer");

	init_ctx(&a, &dev, &ma, ba);
	start(&a);
	inject(&dev, 0x100);
	inject(&dev, 0x1000);
	draining = &a;
	avd_drain_context(&a);
	assert(!a.active && !a.watchdog_work.work.queued);
	puts("PASS: streamoff drains released ownership and watchdog");

	init_ctx(&a, &dev, &ma, ba);
	queue_full = 1;
	start(&a);
	pump(&a);
	queue_full = 0;
	assert(resets == 2 && ba[0].result == VB2_BUF_STATE_ERROR);
	assert(!a.job.segments);
	init_ctx(&a, &dev, &ma, ba);
	reset_error = -EIO;
	start(&a);
	avd_watchdog_func(&a.watchdog_work.work);
	pump(&a);
	assert(dev.broken);
	int before = configures;
	init_ctx(&a, &dev, &ma, ba);
	start(&a);
	pump(&a);
	assert(configures == before && ba[0].result == VB2_BUF_STATE_ERROR &&
	       pm_refs == 0);
	struct device device = { .data = &dev };
	int before_boots = boots;

	assert(avd_runtime_resume(&device) == -EIO && boots == before_boots);
	dev.broken = false;
	assert(avd_runtime_resume(&device) == 0 && boots == before_boots + 1);
	puts("PASS: partial submission resets; failed reset prevents FIFO reuse and firmware resume");
	return 0;
}
