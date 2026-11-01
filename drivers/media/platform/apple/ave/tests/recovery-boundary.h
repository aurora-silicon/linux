/* SPDX-License-Identifier: GPL-2.0-only */
/* Fake PM, MMIO and vb2 boundary for unmodified production function bodies. */
#include <stddef.h>
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define dev_err_ratelimited(...) test_log(__VA_ARGS__)
#define lockdep_assert_held(p) assert((p)->held)
#define MEMREMAP_WC 0
/* Host memory is coherent; the production write barrier is simulated. */
#define wmb() ((void)0)
#define msecs_to_jiffies(x) (x)
#define ktime_add_ms(a, b) ((a) + (b))
#define ktime_after(a, b) ((a) > (b))
#define atomic_set(p, v) (*(p) = (v))
#define atomic_read(p) (*(p))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
struct work_struct { int unused; };

struct delayed_work { struct work_struct work; bool scheduled; };

struct mutex { bool held; };

static void mutex_lock(struct mutex *m)
{
	assert(!m->held);
	m->held = true;
}

static void mutex_unlock(struct mutex *m)
{
	assert(m->held);
	m->held = false;
}

#define spin_lock_irqsave(p, flags) do { (flags) = 0; mutex_lock(p); } while (0)
#define spin_unlock_irqrestore(p, flags) do { (void)(flags); mutex_unlock(p); } while (0)
struct ave_dev;
struct device { struct ave_dev *drvdata; unsigned int refs, dma_refs; };

static void *dev_get_drvdata(struct device *dev)
{
	return dev->drvdata;
}

struct vb2_queue { unsigned int type; bool error, streaming; void *priv; };

struct vb2_buffer {
	struct vb2_queue *vb2_queue;
	void *priv;
	struct { void *mem_priv; } planes[1];
};

struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; unsigned int completions; int state; };

struct v4l2_m2m_ctx {
	struct vb2_queue src, dst;
	struct vb2_v4l2_buffer *input[4], *output[4];
	unsigned int nsrc, ndst;
};

struct v4l2_fh { struct v4l2_m2m_ctx *m2m_ctx; };

struct ave_ctx {
	struct ave_dev *ave;
	struct v4l2_fh fh;
	struct ave_session *sess;
	struct delayed_work poll_work;
	struct mutex queue_lock;
	int ready;
	bool failed, draining, flush_pending, stopped;
	struct vb2_v4l2_buffer *drain_src;
};

struct ave_dev {
	struct device *dev;
	struct mutex hw_lock;
	enum ave_fw_state fw_state;
	struct ave_ctx *sessions[AVE_MAX_SESSIONS];
	struct ave_session *failed_sessions;
	struct ave_recovery_history recovery;
	unsigned int recovery_count;
	bool inject_wedge, data_dirty;
	void *wq, *m2m_dev, *coproc, *mbox;
	const struct ave_variant *variant;
	const struct ave_fw_layout *layout;
	u32 data_crc;
	struct ave_fw_segment text, data;
	void *data_pristine;
	struct ave_ring ring[2];
	u64 ipc_fw_base;
	unsigned long mctf_pool_flags;
	struct ave_buf mctf_pool[AVE_MCTF_BUFS];
	struct ave_buf mctf_pool_in[AVE_MCTF_IN_BUFS];
	struct ave_buf mctf_pool_out[AVE_MCTF_OUT_BUFS];
	u32 mctf_pool_crc;
	bool mctf_pool_crc_valid;
};

static void *test_ctrl_addr;
static void ave_ipc_dump(struct ave_dev *ave, const char *why) {}
static int halt_result, close_result;
static unsigned int halts, closes, freed, suspends, resumes, schedules, barriers;
static unsigned int polls, sleep_ms;
static bool map_fail, ctrl_sticky, stopped_for_sleep;
static int poll_result;
typedef unsigned long ktime_t;
static ktime_t ticks;
static ktime_t ktime_get(void)
{
	return ++ticks;
}

static void msleep(unsigned int ms)
{
	sleep_ms += ms;
}

static void usleep_range(unsigned int lo, unsigned int hi) { }

static u32 readl(const void *p)
{
	return *(const u32 *)p;
}

static u64 readq(const void *p)
{
	return *(const u64 *)p;
}

static void writel(u32 v, void *p)
{
	if (!ctrl_sticky) {
		*(u32 *)p = v;
		if (p == test_ctrl_addr)
			stop_seen = !v;
	}
}

static void *memremap(phys_addr_t p, size_t size, unsigned int mode)
{
	return map_fail ? NULL : (void *)p;
}

static void memunmap(void *p)
{
	barriers++;
}

static bool mod_delayed_work(void *wq, struct delayed_work *work, unsigned long delay)
{
	work->scheduled = true;
	return true;
}

static bool cancel_delayed_work(struct delayed_work *work)
{
	work->scheduled = false;
	return true;
}

static bool cancel_delayed_work_sync(struct delayed_work *work)
{
	return cancel_delayed_work(work);
}

static bool queue_delayed_work(void *wq, struct delayed_work *work, unsigned long delay)
{
	return mod_delayed_work(wq, work, delay);
}

static int ave_cmd_halt(struct ave_dev *ave)
{
	halts++;
	return halt_result;
}

static int ave_cmd_close(struct ave_ctx *ctx)
{
	closes++;
	if (!close_result)
		ctx->sess->fw_open = false;
	return close_result;
}

static int ave_cmd_complete(struct ave_ctx *ctx, unsigned int n)
{
	return 0;
}

static int ave_ipc_settle(struct ave_dev *ave, unsigned int ms)
{
	return 0;
}

static int ave_ipc_poll_once(struct ave_dev *ave)
{
	/* A deadline bug must fail, rather than leave the test spinning. */
	assert(++polls < 100);
	return poll_result;
}

static bool ave_sess_starved(struct ave_session *sess)
{
	return false;
}

static bool ave_sess_stalled(struct ave_session *sess)
{
	return false;
}

static void ave_free(struct ave_dev *ave, struct ave_buf *buf)
{
	if (buf->cpu) {
		assert(stop_seen && power_seen);
		freed++;
		buf->cpu = NULL;
	}
}

static void pm_runtime_mark_last_busy(struct device *dev) { }

static void pm_runtime_put_autosuspend(struct device *dev)
{
	assert(dev->refs);
	dev->refs--;
}

static void v4l2_m2m_suspend(void *dev)
{
	suspends++;
}

static void v4l2_m2m_resume(void *dev)
{
	resumes++;
}

static void ave_v4l2_suspend(struct ave_dev *ave)
{
	stopped_for_sleep = true;
}

static void v4l2_m2m_try_schedule(struct v4l2_m2m_ctx *ctx)
{
	schedules++;
}

static struct vb2_queue *v4l2_m2m_get_src_vq(struct v4l2_m2m_ctx *ctx)
{
	return &ctx->src;
}

static struct vb2_queue *v4l2_m2m_get_dst_vq(struct v4l2_m2m_ctx *ctx)
{
	return &ctx->dst;
}

#define V4L2_TYPE_IS_OUTPUT(t) ((t) == 1)
enum vb2_buffer_state { VB2_BUF_STATE_ERROR = 1 };

static struct vb2_v4l2_buffer *v4l2_m2m_src_buf_remove(struct v4l2_m2m_ctx *ctx)
{
	return ctx->nsrc ? ctx->input[--ctx->nsrc] : NULL;
}

static struct vb2_v4l2_buffer *v4l2_m2m_dst_buf_remove(struct v4l2_m2m_ctx *ctx)
{
	return ctx->ndst ? ctx->output[--ctx->ndst] : NULL;
}

static unsigned int v4l2_m2m_num_src_bufs_ready(struct v4l2_m2m_ctx *ctx)
{
	return ctx->nsrc;
}

static void v4l2_m2m_buf_done(struct vb2_v4l2_buffer *buf, enum vb2_buffer_state state)
{
	assert(++buf->completions == 1);
	buf->state = state;
}

static bool vb2_is_streaming(struct vb2_queue *q)
{
	return q->streaming;
}

static void vb2_queue_error(struct vb2_queue *q)
{
	q->error = true;
}

static void *vb2_get_drv_priv(struct vb2_queue *q)
{
	return q->priv;
}
#define to_vb2_v4l2_buffer(p) container_of(p, struct vb2_v4l2_buffer, vb2_buf)
static void v4l2_m2m_buf_queue(struct v4l2_m2m_ctx *ctx, struct vb2_v4l2_buffer *buf)
{
	assert(!ctx->src.error && ctx->nsrc < 3);
	ctx->input[ctx->nsrc++] = buf;
}
/* Forward declarations between the extracted, otherwise unchanged callbacks. */
void ave_fw_mark_broken(struct ave_dev *ave, int err, const char *what);
void ave_sess_reap(struct ave_dev *ave);
static void ave_reset_drain(struct ave_ctx *ctx);
int ave_ipc_pump(struct ave_dev *ave, unsigned int wait_ms,
		 bool (*cond)(struct ave_dev *, void *), void *arg);

/* ABI-selected helpers are unreachable in the 9003 recovery fixtures. */
#define readl_poll_timeout(addr, value, condition, delay, timeout) \
 ((value) = readl(addr), (condition) ? 0 : -ETIMEDOUT)
static int ave_fw_crc(const struct ave_fw_segment *segment, u32 *crc, void *copy)
{
 assert(!"6070 CRC readback reached a 9003 recovery fixture");
 return -EIO;
}
static void ave_fw_release_boot(struct ave_dev *ave)
{
 assert(!"6070 boot teardown reached a 9003 recovery fixture");
}
