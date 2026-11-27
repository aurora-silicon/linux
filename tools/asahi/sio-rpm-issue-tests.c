// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host model of drivers/dma/apple-sio.c runtime PM + descriptor issue lifecycle.
 * Exercises the actual extracted functions from apple-sio.c against a runtime PM,
 * virt-dma, and RTKit firmware state model.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __maybe_unused
#define __force
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define BIT(nr) (1UL << (nr))
#define GENMASK(h, l) (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (64 - 1 - (h))))
#define FIELD_PREP(mask, val) (((uint64_t)(val) << __builtin_ctzll(mask)) & (mask))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define WARN_ON(cond) ({ \
	bool __c = !!(cond); \
	if (__c) \
		fprintf(stderr, "WARN_ON(%s)\n", #cond); \
	__c; \
})
static inline void dev_err(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; }
static inline void dev_err_ratelimited(const void *dev, const char *fmt, ...)
{
	(void)dev;
	(void)fmt;
}
static inline void dev_warn(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; }
static inline void dev_warn_ratelimited(const void *dev, const char *fmt, ...)
{
	(void)dev;
	(void)fmt;
}
static inline void dev_WARN_ONCE(const void *dev, bool cond, const char *fmt, ...)
{
	(void)dev;
	(void)cond;
	(void)fmt;
}
static inline void dev_info(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; }
static inline void dev_dbg(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; }
#define msecs_to_jiffies(m) (m)
#define dma_wmb() ((void)0)
#define spin_lock_irqsave(lock, flags) do { \
	bool *__l = (lock); \
	(void)(flags); \
	assert(!*__l); \
	*__l = true; \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
	bool *__l = (lock); \
	(void)(flags); \
	assert(*__l); \
	*__l = false; \
} while (0)

#define u8 uint8_t
#define u32 uint32_t
#define u64 uint64_t
#define dma_addr_t uintptr_t
#define dma_cookie_t int
#define gfp_t unsigned int
#define DMA_MIN_COOKIE 1
#define GFP_KERNEL 0
#define GFP_NOWAIT 0

enum dma_transfer_direction { DMA_MEM_TO_DEV, DMA_DEV_TO_MEM };
enum dma_slave_buswidth {
	DMA_SLAVE_BUSWIDTH_1_BYTE = 1,
	DMA_SLAVE_BUSWIDTH_2_BYTES = 2,
	DMA_SLAVE_BUSWIDTH_4_BYTES = 4,
};

struct list_head {
	struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

#define LIST_HEAD(name) struct list_head name = { &(name), &(name) }

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	struct list_head *prev = head->prev;

	new->next = head;
	new->prev = prev;
	prev->next = new;
	head->prev = new;
}

static inline void list_del(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	entry->next = entry->prev = NULL;
}

static inline void list_move_tail(struct list_head *list, struct list_head *head)
{
	list_del(list);
	list_add_tail(list, head);
}

static inline bool list_empty(const struct list_head *head)
{
	return head->next == head;
}

static inline void list_splice_init(struct list_head *list, struct list_head *head)
{
	if (!list_empty(list)) {
		struct list_head *first = list->next;
		struct list_head *last = list->prev;
		struct list_head *at = head->next;

		first->prev = head;
		head->next = first;
		last->next = at;
		at->prev = last;
		INIT_LIST_HEAD(list);
	}
}

static inline void list_splice_tail_init(struct list_head *list, struct list_head *head)
{
	if (!list_empty(list)) {
		struct list_head *first = list->next;
		struct list_head *last = list->prev;
		struct list_head *at = head->prev;

		first->prev = at;
		at->next = first;
		last->next = head;
		head->prev = last;
		INIT_LIST_HEAD(list);
	}
}

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

#define list_first_entry(ptr, type, member) \
	container_of((ptr)->next, type, member)

#define list_for_each_entry(pos, head, member) \
	for (pos = container_of((head)->next, typeof(*pos), member); \
	     &pos->member != (head); \
	     pos = container_of(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = container_of((head)->next, typeof(*pos), member), \
	     n = container_of(pos->member.next, typeof(*pos), member); \
	     &pos->member != (head); \
	     pos = n, n = container_of(n->member.next, typeof(*pos), member))

struct completion {
	bool done;
};

static inline void init_completion(struct completion *c)
{
	c->done = false;
}

static inline void complete(struct completion *c)
{
	c->done = true;
}
static inline unsigned long wait_for_completion_timeout(struct completion *c, unsigned long t)
{
	(void)t;
	return c->done ? 1 : 0;
}

struct wait_queue_head {
	int wakes;
};
#define wait_queue_head_t struct wait_queue_head
static inline void wake_up(wait_queue_head_t *q) { q->wakes++; }
#define wait_event_timeout(wq, cond, t) ((void)(wq), (void)(t), (cond) ? 1 : 0)

struct work_struct {
	void (*func)(struct work_struct *work);
	bool pending;
};
#define INIT_WORK(w, fn) do { (w)->func = (fn); (w)->pending = false; } while (0)
static inline bool schedule_work(struct work_struct *w)
{
	if (w->pending)
		return false;
	w->pending = true;
	return true;
}
static inline bool cancel_work_sync(struct work_struct *w)
{
	bool was_pending = w->pending;

	w->pending = false;
	return was_pending;
}
static inline void flush_work(struct work_struct *w)
{
	while (w->pending) {
		w->pending = false;
		w->func(w);
	}
}

struct tasklet_struct {
	int kills;
	bool scheduled;
};
static inline void tasklet_kill(struct tasklet_struct *t);

enum rpm_status {
	RPM_ACTIVE,
	RPM_SUSPENDING,
	RPM_SUSPENDED,
	RPM_RESUMING,
};

struct device_node {
	int unused;
};

struct device {
	void *drvdata;
	struct device_node *of_node;
	enum rpm_status rpm_status;
	int usage_count;
	int disable_depth;
	bool autosuspend_pending;
};

struct platform_device {
	struct device dev;
};

static inline void *dev_get_drvdata(const struct device *dev) { return dev->drvdata; }
static inline void *platform_get_drvdata(const struct platform_device *pdev)
{
	return dev_get_drvdata(&pdev->dev);
}

enum dma_status {
	DMA_COMPLETE,
	DMA_IN_PROGRESS,
	DMA_PAUSED,
	DMA_ERROR,
	DMA_OUT_OF_ORDER,
};

enum dmaengine_tx_result {
	DMA_TRANS_NOERROR = 0,
	DMA_TRANS_ABORTED,
};

struct dmaengine_result {
	enum dmaengine_tx_result result;
	u32 residue;
};

struct dma_tx_state {
	dma_cookie_t last;
	dma_cookie_t used;
	u32 residue;
	u32 in_flight_bytes;
};

struct dma_device {
	int unused;
};

struct dma_chan {
	dma_cookie_t cookie;
	dma_cookie_t completed_cookie;
};

struct dma_async_tx_descriptor {
	struct dma_chan *chan;
	dma_cookie_t cookie;
};

struct dma_slave_config {
	enum dma_transfer_direction direction;
	enum dma_slave_buswidth src_addr_width;
	enum dma_slave_buswidth dst_addr_width;
	void *peripheral_config;
	size_t peripheral_size;
};

struct apple_sio_idle {
	dma_addr_t addr;
	size_t len;
	size_t period_len;
};

struct virt_dma_desc {
	struct dma_async_tx_descriptor tx;
	struct dmaengine_result tx_result;
	struct list_head node;
};

struct virt_dma_chan {
	struct dma_chan chan;
	bool lock;
	struct tasklet_struct task;
	struct list_head desc_submitted;
	struct list_head desc_issued;
	struct list_head desc_completed;
	struct virt_dma_desc *cyclic;
	void (*desc_free)(struct virt_dma_desc *vd);
};

static inline void dma_cookie_complete(struct dma_async_tx_descriptor *tx)
{
	tx->chan->completed_cookie = tx->cookie;
	tx->cookie = 0;
}

static inline enum dma_status dma_cookie_status(struct dma_chan *chan,
						dma_cookie_t cookie,
						struct dma_tx_state *state)
{
	if (state) {
		state->last = chan->completed_cookie;
		state->used = chan->cookie;
		state->residue = 0;
		state->in_flight_bytes = 0;
	}
	return cookie <= chan->completed_cookie ? DMA_COMPLETE : DMA_IN_PROGRESS;
}

static inline void dma_set_residue(struct dma_tx_state *state, u32 residue)
{
	if (state)
		state->residue = residue;
}

static inline struct virt_dma_desc *vchan_find_desc(struct virt_dma_chan *vc,
						    dma_cookie_t cookie)
{
	struct virt_dma_desc *vd;

	list_for_each_entry(vd, &vc->desc_issued, node)
		if (vd->tx.cookie == cookie)
			return vd;
	return NULL;
}

static inline void vchan_cookie_complete(struct virt_dma_desc *vd)
{
	struct virt_dma_chan *vc = container_of(vd->tx.chan, struct virt_dma_chan, chan);

	dma_cookie_complete(&vd->tx);
	list_add_tail(&vd->node, &vc->desc_completed);
	vc->task.scheduled = true;
}

static inline bool vchan_issue_pending(struct virt_dma_chan *vc)
{
	bool pending = !list_empty(&vc->desc_submitted);

	list_splice_tail_init(&vc->desc_submitted, &vc->desc_issued);
	return pending;
}

static inline struct virt_dma_desc *vchan_next_desc(struct virt_dma_chan *vc)
{
	if (list_empty(&vc->desc_issued))
		return NULL;
	return list_first_entry(&vc->desc_issued, struct virt_dma_desc, node);
}

static inline void vchan_get_all_descriptors(struct virt_dma_chan *vc, struct list_head *head)
{
	list_splice_tail_init(&vc->desc_submitted, head);
	list_splice_tail_init(&vc->desc_issued, head);
	list_splice_tail_init(&vc->desc_completed, head);
}

static inline void vchan_dma_desc_free_list(struct virt_dma_chan *vc, struct list_head *head)
{
	struct virt_dma_desc *vd, *_vd;

	list_for_each_entry_safe(vd, _vd, head, node) {
		list_del(&vd->node);
		vc->desc_free(vd);
	}
}

static inline void vchan_cyclic_callback(struct virt_dma_desc *vd) { (void)vd; }

static inline bool bitmap_empty(const unsigned long *src, unsigned int nbits)
{
	(void)nbits;
	return *src == 0;
}

#define REG_CPU_CONTROL 0x44
#define CPU_CONTROL_RUN BIT(4)
#define SIOMSG_DATA GENMASK(63, 32)
#define SIOMSG_TYPE GENMASK(23, 16)
#define SIOMSG_EP GENMASK(7, 0)
#define MSG_CONFIGURE 0x5
#define MSG_ISSUE 0x6
#define MSG_TERMINATE 0x8
#define SIO_MAX_NINFLIGHT 4
#define SIO_NTAGS 16
#define SIO_MAX_ERROR_COOKIES 16

struct sio_error_cookie {
	dma_cookie_t cookie;
	u32 residue;
};

struct sio_coproc_desc {
	u32 pad1;
	u32 flag;
	u64 unk;
	u64 iova;
	u64 size;
	u64 pad2;
	u64 pad3;
} __packed;

struct sio_shmem_chan_config {
	u32 datashape;
	u32 timeout;
	u32 fifo;
	u32 threshold;
	u32 limit;
} __packed;

struct sio_data;
struct sio_chan;
struct sio_tx;
typedef void (*sio_ack_callback)(struct sio_chan *, void *, bool);

struct sio_hw_data {
	u32 fifo_threshold;
	u32 cfg_word4;
	bool cfg_six_words;
	bool stop_at_commit;
	bool commit_stop;
};

struct sio_chan {
	unsigned int no;
	struct sio_data *host;
	struct virt_dma_chan vc;
	struct work_struct terminate_wq;
/* EXTRA_CHAN_FIELDS */
	bool configured;
	struct sio_shmem_chan_config cfg;
	struct sio_tx *current_tx;
	unsigned int pending;
	bool ring;
	struct sio_tx *idle;
	struct list_head retired;
	wait_queue_head_t drained;
	struct sio_tx *issued[SIO_MAX_NINFLIGHT];
	unsigned int issued_head;
	unsigned int nissued;
	bool issuing;
	bool stopping;
	struct list_head dead;
};

struct sio_data {
	const struct sio_hw_data *hw;
	u8 base[0x50];
	struct dma_device dma;
	struct device *dev;
	void *rtk;
	bool asleep;
	bool rpm_stopped;
	bool removing;
	bool rtk_stopped;
	bool rtk_shutdown_failed;
	bool firmware_only;
	bool commit_registered;
	bool tables_mismatch;
	const void *arrays;
	u32 fifo_threshold;
	u32 cfg_word4;
	struct sio_shmem_chan_config shmem[2];
	struct {
		unsigned long allocated[1];
	} tags;
	int nchannels;
	struct sio_chan channels[2];
};

struct sio_tx {
	struct virt_dma_desc vd;
	struct completion done;
	bool terminated;
	bool retired;
	size_t period_len;
	int nperiods;
	int ninflight;
	int next;
	struct sio_coproc_desc *siodesc[];
};

static inline u32 readl(const void *addr)
{
	return *(const u32 *)addr;
}

static inline void writel(u32 val, void *addr)
{
	*(u32 *)addr = val;
}

static struct sio_chan *to_sio_chan(struct dma_chan *chan)
{
	return container_of(chan, struct sio_chan, vc.chan);
}

static struct sio_tx *to_sio_tx(struct dma_async_tx_descriptor *tx)
{
	return container_of(tx, struct sio_tx, vd.tx);
}

static enum dma_transfer_direction sio_chan_direction(int channo)
{
	return (channo & 1) ? DMA_DEV_TO_MEM : DMA_MEM_TO_DEV;
}

static int sio_coproc_desc_slot(struct sio_data *sio, struct sio_coproc_desc *d)
{
	(void)sio;
	return (int)(uintptr_t)d;
}

/* Global mock hardware/firmware state */
static int g_issue_calls;
static int g_config_calls;
static int g_terminate_calls;
static int g_stop_calls;
static int g_resume_calls;
static int g_reset_calls;
static int g_tx_freed;
static int g_completed_callbacks;
static enum dmaengine_tx_result g_last_tx_result;
static u32 g_last_tx_residue;
static bool g_fail_resume;
static bool g_unregistered;
static bool g_rpm_barrier;
static int g_mailbox_off_calls;
static int g_shutdown_calls;
static int g_shutdown_at_mailbox_off;
static int g_total_tasklet_kills;
static int g_kills_at_mailbox_off;
static struct sio_chan *g_sleep_stop_ch;
static void (*g_sleep_stop_hook)(struct sio_chan *ch);

static inline void tasklet_kill(struct tasklet_struct *t)
{
	struct virt_dma_chan *vc = container_of(t, struct virt_dma_chan, task);
	struct virt_dma_desc *vd, *tmp;

	t->kills++;
	g_total_tasklet_kills++;
	if (!t->scheduled)
		return;
	t->scheduled = false;
	list_for_each_entry_safe(vd, tmp, &vc->desc_completed, node) {
		g_last_tx_result = vd->tx_result.result;
		g_last_tx_residue = vd->tx_result.residue;
		g_completed_callbacks++;
		list_del(&vd->node);
		vc->desc_free(vd);
	}
}

static int sio_suspend(struct device *dev);
static int sio_resume(struct device *dev);

static void apple_sleep_coproc_unregister(struct device *dev) { (void)dev; }

static void of_dma_controller_free(struct device_node *np) { (void)np; }

static void dma_async_device_unregister(struct dma_device *dma)
{
	struct sio_data *sio = container_of(dma, struct sio_data, dma);

	assert(sio->removing);
	for (int i = 0; i < sio->nchannels; i++) {
#ifdef HAVE_ISSUE_WORK
		assert(!sio->channels[i].issue_wq.pending);
#endif
		assert(!sio->channels[i].terminate_wq.pending);
		assert(!sio->channels[i].vc.task.scheduled);
	}
	g_unregistered = true;
}

static void pm_runtime_dont_use_autosuspend(struct device *dev)
{
	dev->autosuspend_pending = false;
}

static void pm_runtime_barrier(struct device *dev)
{
	struct sio_data *sio = dev_get_drvdata(dev);

	for (int i = 0; i < sio->nchannels; i++) {
#ifdef HAVE_ISSUE_WORK
		assert(!sio->channels[i].issue_wq.pending);
#endif
		assert(!sio->channels[i].terminate_wq.pending);
	}
	g_rpm_barrier = true;
}

static void pm_runtime_put_noidle(struct device *dev)
{
	assert(dev->usage_count > 0);
	dev->usage_count--;
}

static int pm_runtime_get_if_active(struct device *dev)
{
	if (dev->disable_depth > 0)
		return -EINVAL;
	if (dev->rpm_status != RPM_ACTIVE)
		return 0;
	dev->usage_count++;
	dev->autosuspend_pending = false;
	return 1;
}

static int pm_runtime_resume_and_get(struct device *dev)
{
	int ret;

	if (dev->disable_depth > 0)
		return -EACCES;
	if (dev->rpm_status == RPM_SUSPENDING) {
		ret = sio_suspend(dev);
		dev->rpm_status = ret ? RPM_ACTIVE : RPM_SUSPENDED;
	}
	if (dev->rpm_status == RPM_SUSPENDED) {
		dev->rpm_status = RPM_RESUMING;
		ret = sio_resume(dev);
		if (ret) {
			dev->rpm_status = RPM_SUSPENDED;
			return ret;
		}
		dev->rpm_status = RPM_ACTIVE;
	}
	dev->usage_count++;
	dev->autosuspend_pending = false;
	return 0;
}

static void pm_runtime_mark_last_busy(struct device *dev) { (void)dev; }

static int pm_runtime_put_autosuspend(struct device *dev)
{
	assert(dev->usage_count > 0);
	dev->usage_count--;
	if (dev->usage_count == 0)
		dev->autosuspend_pending = true;
	return 0;
}

static void apple_rtkit_mailbox_off(void *rtk)
{
	(void)rtk;
	g_shutdown_at_mailbox_off = g_shutdown_calls;
	g_mailbox_off_calls++;
	g_kills_at_mailbox_off = g_total_tasklet_kills;
}

static int apple_rtkit_shutdown(void *rtk)
{
	(void)rtk;
	assert(g_mailbox_off_calls == 0);
	g_shutdown_calls++;
	return 0;
}

static int apple_rtkit_sleep_stop(void *rtk, void *cpu_ctrl)
{
	(void)rtk;
	(void)cpu_ctrl;
	g_stop_calls++;
	if (g_sleep_stop_hook) {
		void (*hook)(struct sio_chan *ch) = g_sleep_stop_hook;
		struct sio_chan *ch = g_sleep_stop_ch;

		g_sleep_stop_hook = NULL;
		g_sleep_stop_ch = NULL;
		hook(ch);
	}
	return 0;
}

static int apple_rtkit_sleep_return(void *rtk, void *cpu_ctrl)
{
	(void)rtk;
	(void)cpu_ctrl;
	return 0;
}

static int apple_sleep_engine_reset(struct device *dev)
{
	(void)dev;
	g_reset_calls++;
	return 0;
}

static int apple_rtkit_power_cycled_return(void *rtk, void *cpu_ctrl)
{
	(void)rtk;
	(void)cpu_ctrl;
	g_resume_calls++;
	if (g_fail_resume)
		return -ETIMEDOUT;
	return 0;
}

static int sio_send_siomsg_atomic(struct sio_data *sio, u64 msg,
				  sio_ack_callback ack_callback, void *cookie)
{
	u8 type = (msg & SIOMSG_TYPE) >> 16;

	(void)ack_callback;
	(void)cookie;
	/* Firmware MUST be live and active with a held runtime PM reference. */
	assert(sio->dev->rpm_status == RPM_ACTIVE);
	assert(!sio->rpm_stopped && !sio->asleep);
	assert(sio->dev->usage_count > 0);
	if (type == MSG_ISSUE)
		g_issue_calls++;
	return 0;
}

static int sio_call(struct sio_data *sio, u64 msg)
{
	u8 type = (msg & SIOMSG_TYPE) >> 16;

	assert(sio->dev->rpm_status == RPM_ACTIVE);
	assert(!sio->rpm_stopped && !sio->asleep);
	assert(sio->dev->usage_count > 0);
	if (type == MSG_CONFIGURE)
		g_config_calls++;
	else if (type == MSG_TERMINATE)
		g_terminate_calls++;
	return 1;
}

static struct sio_tx *sio_alloc_tx(struct sio_chan *siochan, dma_addr_t addr,
				   size_t len, size_t period_len, gfp_t gfp)
{
	struct sio_tx *tx;
	int nperiods = (int)(len / period_len);
	int i;

	(void)addr;
	(void)gfp;
	tx = calloc(1, sizeof(*tx) + nperiods * sizeof(tx->siodesc[0]));
	assert(tx);
	init_completion(&tx->done);
	tx->vd.tx.chan = &siochan->vc.chan;
	tx->period_len = period_len;
	tx->nperiods = nperiods;
	for (i = 0; i < nperiods; i++)
		tx->siodesc[i] = (struct sio_coproc_desc *)(uintptr_t)(0x100 + i * 4);
	return tx;
}

static void sio_tx_free(struct virt_dma_desc *vd)
{
	g_tx_freed++;
	free(to_sio_tx(&vd->tx));
}

static void sio_write_config(const struct sio_hw_data *hw, void *shmem,
			     const struct sio_shmem_chan_config *cfg)
{
	(void)hw;
	memcpy(shmem, cfg, sizeof(*cfg));
}

static bool sio_chan_busy(struct sio_chan *siochan);
static int sio_stop_quiet(struct sio_chan *siochan)
{
	(void)siochan;
	return 0;
}
static void sio_free_dead(struct sio_chan *siochan)
{
	struct sio_tx *tx, *n;

	list_for_each_entry_safe(tx, n, &siochan->dead, vd.node) {
		list_del(&tx->vd.node);
		sio_tx_free(&tx->vd);
	}
}

/* ACTUAL_SIO_FUNCTIONS */

static void run_autosuspend(struct device *dev)
{
	int ret;

	assert(dev->autosuspend_pending);
	assert(dev->usage_count == 0);
	assert(dev->rpm_status == RPM_ACTIVE);
	dev->autosuspend_pending = false;
	dev->rpm_status = RPM_SUSPENDING;
	ret = sio_suspend(dev);
	dev->rpm_status = ret ? RPM_ACTIVE : RPM_SUSPENDED;
}

static void reset_env(struct sio_data *sio, struct device *dev, struct sio_hw_data *hw)
{
	struct sio_chan *ch = &sio->channels[0];

	memset(sio, 0, sizeof(*sio));
	memset(dev, 0, sizeof(*dev));
	memset(hw, 0, sizeof(*hw));
	hw->fifo_threshold = 0x800;
	hw->cfg_word4 = 0x800;
	dev->drvdata = sio;
	dev->rpm_status = RPM_ACTIVE;
	sio->hw = hw;
	sio->dev = dev;
	sio->nchannels = 1;
	sio->fifo_threshold = hw->fifo_threshold;
	sio->cfg_word4 = hw->cfg_word4;

	ch->host = sio;
	ch->no = 0;
	ch->vc.desc_free = sio_tx_free;
	INIT_LIST_HEAD(&ch->vc.desc_submitted);
	INIT_LIST_HEAD(&ch->vc.desc_issued);
	INIT_LIST_HEAD(&ch->vc.desc_completed);
	INIT_LIST_HEAD(&ch->retired);
	INIT_LIST_HEAD(&ch->dead);
	INIT_WORK(&ch->terminate_wq, sio_terminate_work);
#ifdef HAVE_ISSUE_WORK
	INIT_WORK(&ch->issue_wq, sio_issue_work);
#endif
	g_issue_calls = g_config_calls = g_terminate_calls = 0;
	g_stop_calls = g_resume_calls = g_reset_calls = g_tx_freed = 0;
	g_completed_callbacks = 0;
	g_last_tx_result = DMA_TRANS_NOERROR;
	g_last_tx_residue = 0;
	g_fail_resume = false;
	g_unregistered = false;
	g_rpm_barrier = false;
	g_mailbox_off_calls = 0;
	g_shutdown_calls = 0;
	g_shutdown_at_mailbox_off = 0;
	g_total_tasklet_kills = 0;
	g_kills_at_mailbox_off = 0;
	g_sleep_stop_ch = NULL;
	g_sleep_stop_hook = NULL;
}

static struct sio_tx *submit_cyclic_len(struct sio_chan *ch, dma_cookie_t cookie,
					size_t len)
{
	struct sio_tx *tx = sio_alloc_tx(ch, 0x10000, len, 0x1000, GFP_NOWAIT);

	tx->vd.tx.cookie = cookie;
	ch->vc.chan.cookie = cookie;
	list_add_tail(&tx->vd.node, &ch->vc.desc_submitted);
	return tx;
}

static struct sio_tx *submit_cyclic(struct sio_chan *ch, dma_cookie_t cookie)
{
	return submit_cyclic_len(ch, cookie, 0x2000);
}

static struct sio_tx *g_mid_suspend_tx;

static void mid_suspend_issue_hook(struct sio_chan *ch)
{
	g_mid_suspend_tx = submit_cyclic(ch, 20);
	sio_issue_pending(&ch->vc.chan);
}

int main(void)
{
	struct sio_data sio;
	struct device dev;
	struct sio_hw_data hw;
	struct sio_chan *ch = &sio.channels[0];
	struct dma_slave_config cfg = {
		.direction = DMA_MEM_TO_DEV,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
	};

	/* 1. Configure channel, let autosuspend expire, then issue descriptor. */
	reset_env(&sio, &dev, &hw);
	assert(sio_device_config(&ch->vc.chan, &cfg) == 0);
	assert(g_config_calls == 1);
	assert(dev.usage_count == 0);
	assert(dev.autosuspend_pending);

	run_autosuspend(&dev);
	assert(dev.rpm_status == RPM_SUSPENDED);
	assert(sio.rpm_stopped);
	assert(g_stop_calls == 1);

	struct sio_tx *tx1 = submit_cyclic(ch, 1);

	sio_issue_pending(&ch->vc.chan);
	/* Must NOT have published while suspended! */
	assert(g_issue_calls == 0);
	assert(ch->current_tx == NULL);
#ifdef HAVE_ISSUE_WORK
	assert(ch->issue_wq.pending);
	flush_work(&ch->issue_wq);
#endif
	assert(dev.rpm_status == RPM_ACTIVE);
	assert(!sio.rpm_stopped);
	assert(dev.usage_count == 1);
	assert(g_resume_calls == 1);
	assert(g_issue_calls == 1);
	assert(ch->current_tx == tx1);
	/* Autosuspend must refuse while channel has an active transfer. */
	assert(sio_suspend(&dev) == -EBUSY);

	/* Ack the 1st issued descriptor (queues 2nd), then terminate + synchronize. */
	sio_handle_issue_ack(ch, (void *)(uintptr_t)1, true);
	assert(dev.usage_count == 1);
	sio_terminate_all(&ch->vc.chan);
	complete(&tx1->done);
	sio_synchronize(&ch->vc.chan);
	assert(ch->current_tx == NULL);
	/* 2nd ISSUE is still pending its ACK, so PM ref stays held until ACK arrives. */
	assert(ch->pending == 1);
	assert(dev.usage_count == 1);
	sio_handle_issue_ack(ch, (void *)(uintptr_t)1, true);
	assert(ch->pending == 0);
	assert(dev.usage_count == 0);
	assert(dev.autosuspend_pending);
	assert(g_tx_freed == 1);
	run_autosuspend(&dev);
	assert(sio.rpm_stopped);

	/* 2. Issue while RPM_SUSPENDING (mid-suspend): must defer, not publish. */
	reset_env(&sio, &dev, &hw);
	assert(sio_device_config(&ch->vc.chan, &cfg) == 0);
	dev.autosuspend_pending = false;
	dev.rpm_status = RPM_SUSPENDING;
	struct sio_tx *tx2 = submit_cyclic(ch, 2);

	sio_issue_pending(&ch->vc.chan);
	assert(g_issue_calls == 0);
	assert(ch->current_tx == NULL);
	/* Pending desc_issued refuses suspend; issue_wq then starts tx2. */
	assert(sio_suspend(&dev) == -EBUSY);
	dev.rpm_status = RPM_ACTIVE;
	assert(!sio.rpm_stopped);
#ifdef HAVE_ISSUE_WORK
	flush_work(&ch->issue_wq);
#endif
	assert(dev.rpm_status == RPM_ACTIVE);
	assert(!sio.rpm_stopped);
	assert(g_issue_calls == 1);
	assert(ch->current_tx == tx2);
	assert(dev.usage_count == 1);
	sio_handle_issue_ack(ch, (void *)(uintptr_t)2, true);
	sio_terminate_all(&ch->vc.chan);
	complete(&tx2->done);
	sio_synchronize(&ch->vc.chan);
	sio_handle_issue_ack(ch, (void *)(uintptr_t)2, true);
	assert(dev.usage_count == 0);

	/* 2b. Issue during apple_rtkit_sleep_stop(): suspend completes, worker wakes. */
	reset_env(&sio, &dev, &hw);
	assert(sio_device_config(&ch->vc.chan, &cfg) == 0);
	dev.autosuspend_pending = false;
	dev.rpm_status = RPM_SUSPENDING;
	g_sleep_stop_ch = ch;
	g_sleep_stop_hook = mid_suspend_issue_hook;
	g_mid_suspend_tx = NULL;
	assert(sio_suspend(&dev) == 0);
	dev.rpm_status = RPM_SUSPENDED;
	assert(sio.rpm_stopped);
	assert(g_issue_calls == 0);
	assert(ch->current_tx == NULL);
#ifdef HAVE_ISSUE_WORK
	flush_work(&ch->issue_wq);
#endif
	assert(dev.rpm_status == RPM_ACTIVE);
	assert(!sio.rpm_stopped);
	assert(g_issue_calls == 1);
	assert(ch->current_tx == g_mid_suspend_tx);
	assert(dev.usage_count == 1);
	sio_handle_issue_ack(ch, (void *)(uintptr_t)20, true);
	sio_terminate_all(&ch->vc.chan);
	complete(&g_mid_suspend_tx->done);
	sio_synchronize(&ch->vc.chan);
	sio_handle_issue_ack(ch, (void *)(uintptr_t)20, true);
	assert(dev.usage_count == 0);

	/* 3. Cancel before issue_wq runs: no publish, no leaked PM ref. */
	reset_env(&sio, &dev, &hw);
	assert(sio_device_config(&ch->vc.chan, &cfg) == 0);
	run_autosuspend(&dev);
	submit_cyclic(ch, 3);
	sio_issue_pending(&ch->vc.chan);
	assert(g_issue_calls == 0);
	sio_terminate_all(&ch->vc.chan);
	sio_synchronize(&ch->vc.chan);
#ifdef HAVE_ISSUE_WORK
	assert(!ch->issue_wq.pending);
#endif
	assert(g_issue_calls == 0);
	assert(ch->current_tx == NULL);
	assert(dev.usage_count == 0);
	assert(g_tx_freed == 1);

	/* 4. Active window issue + ring/idle handover + drain after clear_idle. */
	reset_env(&sio, &dev, &hw);
	assert(sio_device_config(&ch->vc.chan, &cfg) == 0);
	struct apple_sio_idle idle = { .addr = 0x20000, .len = 0x2000, .period_len = 0x1000 };

	assert(sio_set_idle(ch, &idle) == 0); /* issues idle #0 */
	assert(dev.usage_count == 1);
	sio_handle_issue_ack(ch, NULL, true); /* acks idle #0, issues idle #1 */
	struct sio_tx *tx4 = submit_cyclic(ch, 4);

	sio_issue_pending(&ch->vc.chan);
	assert(ch->current_tx == tx4);
	assert(dev.usage_count == 2); /* idle + tx_pm */
	sio_handle_issue_ack(ch, NULL, true); /* acks idle #1, issues tx4 #0 */
	sio_handle_issue_ack(ch, NULL, true); /* acks tx4 #0, issues tx4 #1 */
	/* acks tx4 #1; issued=[idle#0, idle#1, tx4#0, tx4#1] */
	sio_handle_issue_ack(ch, NULL, true);
	/* Handover to retired while idle is active. */
	sio_terminate_all(&ch->vc.chan);
	assert(ch->current_tx == NULL);
	assert(!list_empty(&ch->retired));
	assert(dev.usage_count == 2);
	/* Drain the two idle descriptors; tx4 #0 and #1 remain in retired. */
	sio_process_report(ch); /* idle #0 -> issues idle */
	sio_handle_issue_ack(ch, NULL, true);
	sio_process_report(ch); /* idle #1 -> issues idle */
	sio_handle_issue_ack(ch, NULL, true);
	assert(!list_empty(&ch->retired));
	/* Drain tx4 #0 -> still 1 tx4 left in retired. */
	sio_process_report(ch);
	assert(!list_empty(&ch->retired));
	/* Clear idle while tx4 #1 is still in retired: idle PM ref drops, tx_pm stays! */
	struct apple_sio_idle idle_off = { .len = 0 };

	complete(&ch->idle->done);
	assert(sio_set_idle(ch, &idle_off) == 0);
	assert(dev.usage_count == 1);
	assert(ch->ring);
	/* Drain tx4 #1 + remaining idle descriptors: ring leaves and tx_pm drops. */
	for (int k = 0; k < 8 && ch->nissued; k++) {
		if (ch->issuing)
			sio_handle_issue_ack(ch, NULL, true);
		sio_process_report(ch);
	}
	sio_free_dead(ch);
	assert(list_empty(&ch->retired));
	assert(!ch->ring);
	assert(dev.usage_count == 0);
	assert(dev.autosuspend_pending);

	/* 5. Commit-stop controller stays active (usage_count >= 1 from probe). */
	reset_env(&sio, &dev, &hw);
	hw.commit_stop = true;
	dev.usage_count = 1;
	struct sio_tx *tx5 = submit_cyclic(ch, 5);

	sio_issue_pending(&ch->vc.chan);
	assert(ch->current_tx == tx5);
	assert(dev.usage_count == 2);
	sio_handle_issue_ack(ch, (void *)(uintptr_t)5, true);
	sio_terminate_all(&ch->vc.chan);
	complete(&tx5->done);
	sio_synchronize(&ch->vc.chan);
	sio_handle_issue_ack(ch, (void *)(uintptr_t)5, true);
	assert(dev.usage_count == 1);

	/* 6. System-sleep stopped firmware (asleep=true) must not publish descriptors. */
	reset_env(&sio, &dev, &hw);
	hw.commit_stop = true;
	dev.usage_count = 1;
	sio.asleep = true;
	submit_cyclic(ch, 6);
	sio_issue_pending(&ch->vc.chan);
	assert(g_issue_calls == 0);
	assert(ch->current_tx == NULL);
	sio_terminate_all(&ch->vc.chan);
	sio_synchronize(&ch->vc.chan);
	assert(dev.usage_count == 1);

#ifdef HAVE_ISSUE_WORK
	/*
	 * 7. Failed runtime resume aborts multiple differently sized descriptors
	 *    with per-cookie residues preserved across subsequent healthy work.
	 */
	reset_env(&sio, &dev, &hw);
	assert(sio_device_config(&ch->vc.chan, &cfg) == 0);
	run_autosuspend(&dev);
	g_fail_resume = true;
	submit_cyclic_len(ch, 7, 0x2000);
	submit_cyclic_len(ch, 8, 0x3000);
	sio_issue_pending(&ch->vc.chan);
	assert(ch->issue_wq.pending);
	flush_work(&ch->issue_wq);
	assert(g_resume_calls == 1);
	assert(g_issue_calls == 0);
	assert(!ch->current_tx);
	assert(!ch->tx_rpm_held);
	assert(dev.usage_count == 0);
	assert(!sio_chan_busy(ch));
	assert(list_empty(&ch->vc.desc_issued));
	assert(ch->vc.task.scheduled);
	{
		struct dma_tx_state txstate = { 0 };

		assert(sio_tx_status(&ch->vc.chan, 7, &txstate) == DMA_ERROR);
		assert(txstate.residue == 0x2000);
		assert(sio_tx_status(&ch->vc.chan, 8, &txstate) == DMA_ERROR);
		assert(txstate.residue == 0x3000);
		sio_synchronize(&ch->vc.chan);
		assert(g_completed_callbacks == 2);
		assert(g_last_tx_result == DMA_TRANS_ABORTED);
		assert(g_last_tx_residue == 0x3000);
		assert(g_tx_freed == 2);

		/* Starting and completing a healthy transfer must retain both errors. */
		g_fail_resume = false;
		struct sio_tx *tx9 = submit_cyclic_len(ch, 9, 0x4000);

		sio_issue_pending(&ch->vc.chan);
		flush_work(&ch->issue_wq);
		assert(ch->current_tx == tx9);
		assert(g_issue_calls == 1);
		assert(sio_tx_status(&ch->vc.chan, 9, &txstate) == DMA_IN_PROGRESS);
		assert(sio_tx_status(&ch->vc.chan, 7, &txstate) == DMA_ERROR);
		assert(txstate.residue == 0x2000);
		assert(sio_tx_status(&ch->vc.chan, 8, &txstate) == DMA_ERROR);
		assert(txstate.residue == 0x3000);
		sio_handle_issue_ack(ch, (void *)(uintptr_t)9, true);
		sio_terminate_all(&ch->vc.chan);
		complete(&tx9->done);
		sio_synchronize(&ch->vc.chan);
		sio_handle_issue_ack(ch, (void *)(uintptr_t)9, true);
		assert(sio_tx_status(&ch->vc.chan, 9, &txstate) == DMA_COMPLETE);
		assert(sio_tx_status(&ch->vc.chan, 7, &txstate) == DMA_ERROR);
		assert(txstate.residue == 0x2000);
		assert(sio_tx_status(&ch->vc.chan, 8, &txstate) == DMA_ERROR);
		assert(txstate.residue == 0x3000);
		assert(dev.usage_count == 0);
	}

	/* 8. Platform remove closes admission and cancels issue_wq before unregister. */
	reset_env(&sio, &dev, &hw);
	assert(sio_device_config(&ch->vc.chan, &cfg) == 0);
	run_autosuspend(&dev);
	submit_cyclic(ch, 9);
	sio_issue_pending(&ch->vc.chan);
	assert(ch->issue_wq.pending);
	{
		struct platform_device pdev = { .dev = dev };

		pdev.dev.drvdata = &sio;
		sio.dev = &pdev.dev;
		sio_remove(&pdev);
		assert(sio.removing);
		assert(!ch->issue_wq.pending);
		assert(g_unregistered);
		assert(g_rpm_barrier);
		assert(g_resume_calls == 0);
		assert(g_issue_calls == 0);
		assert(list_empty(&ch->vc.desc_issued));
		assert(!ch->vc.task.scheduled);
		assert(g_completed_callbacks == 1);
		assert(g_last_tx_result == DMA_TRANS_ABORTED);
		assert(g_tx_freed == 1);
		assert(pdev.dev.usage_count == 0);
	}
	/* Late issue after admission closes must not move descriptors or rearm vc.task. */
	submit_cyclic(ch, 10);
	sio_issue_pending(&ch->vc.chan);
	assert(!ch->issue_wq.pending);
	assert(g_issue_calls == 0);
	assert(list_empty(&ch->vc.desc_issued));
	assert(!list_empty(&ch->vc.desc_submitted));
	assert(list_empty(&ch->vc.desc_completed));
	assert(!ch->vc.task.scheduled);
	assert(g_completed_callbacks == 1);
	sio_terminate_all(&ch->vc.chan);
	assert(!ch->vc.task.scheduled);
	assert(g_completed_callbacks == 1);
	assert(g_tx_freed == 2);

	/* 8b. Active remove quiesces RTKit report producers before final tasklet kill. */
	reset_env(&sio, &dev, &hw);
	assert(sio_device_config(&ch->vc.chan, &cfg) == 0);
	{
		static const int dummy_arrays;
		struct platform_device pdev = { .dev = dev };

		sio.arrays = &dummy_arrays;
		pdev.dev.drvdata = &sio;
		sio.dev = &pdev.dev;
		sio_remove(&pdev);
		assert(sio.removing);
		assert(sio.rtk_stopped);
		assert(g_shutdown_calls == 1);
		assert(g_mailbox_off_calls == 1);
		assert(g_shutdown_at_mailbox_off == 1);
		assert(g_kills_at_mailbox_off < ch->vc.task.kills);
		assert(!ch->vc.task.scheduled);
	}
	printf("all 8 SIO runtime PM issue lifecycle scenarios passed\n");
#else
	printf("all 6 SIO runtime PM issue lifecycle scenarios passed\n");
#endif
	return 0;
}
