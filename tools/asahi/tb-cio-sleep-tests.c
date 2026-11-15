// SPDX-License-Identifier: GPL-2.0-only
/* Software lifetime and replay model, not a hardware qualification. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GFP_KERNEL 0
#define TB_PATH_MAX_HOPS 16
#define TB_CFG_HOPS 1
#define TB_CFG_PORT 2
#define ADP_CS_4 4
static inline void tb_err(void *tb, const char *fmt, ...)
{
	(void)tb;
	(void)fmt;
}
static inline void tb_info(void *tb, const char *fmt, ...)
{
	(void)tb;
	(void)fmt;
}
static inline void tb_warn(void *tb, const char *fmt, ...)
{
	(void)tb;
	(void)fmt;
}
#define tb_tunnel_info(t, ...) ((void)(t))
struct tb_port;
static bool dock_lost;
static int link_waits, enumerated;
static int tb_wait_for_port(struct tb_port *p, bool w)
{
	(void)p;
	(void)w;
	link_waits++;
	return !dock_lost;
}
static void msleep(unsigned int ms)
{
	(void)ms;
}
#define ENOLINK 67
/* ACTUAL_REGS */
struct tb_switch;
struct tb_port {
	struct tb_switch *sw;
	struct tb_port *remote;
	void *xdomain;
	int port;
	bool null, host_dp;
	struct {
		u32 nfc_credits;
	} config;
	struct tb_regs_hop hw, expected;
	u32 credits;
	bool read_before_write;
};
struct tb_switch {
	bool is_unplugged, usb4;
	u64 uid;
	struct tb_port *ports;
};
struct tb_path_hop {
	struct tb_port *in_port, *out_port;
	int in_hop_index, next_hop_index, nfc_credits, in_counter_index;
};
struct tb_path {
	bool activated, sleep_valid;
	int path_length;
	struct tb_regs_hop *sleep_hops;
	struct tb_path_hop hops[2];
};
struct tb;
struct tb_tunnel {
	struct tb *tb;
	struct tb_port *src_port;
	bool active, dp, dma, pci;
	bool sleep_paused, sleep_dprx, sleep_replay, sleep_changed,
		dprx_started, dprx_canceled;
	unsigned long generation;
	int npaths;
	struct tb_path *paths[2];
	int (*pre_activate)(struct tb_tunnel *tunnel);
	int (*activate)(struct tb_tunnel *tunnel, bool activate);
	struct tb_tunnel *next;
};
struct work {
	bool pending;
};
struct group {
	struct work release_work;
};
struct apple_router_sleep {
	bool valid;
	u32 data[16];
};
struct tb_cm_ops {
	int unused;
};
struct tb_nhi;
struct tb_nhi_ops {
	int (*router_sleep)(struct tb_nhi *nhi);
	int (*router_wake)(struct tb_nhi *nhi);
};
struct tb_nhi {
	const struct tb_nhi_ops *ops;
};
struct tb {
	int lock;
	struct tb_nhi *nhi;
	struct tb_switch *root_switch;
	const struct tb_cm_ops *cm_ops;
	void *wq, *ctl;
};
struct tb_cm {
	bool cio_prepared, cio_ctl_stopped, cio_changed, cio_host_slept,
		hotplug_active, stopping, resume_cleanup_pending;
	unsigned long cio_bw_pending;
	struct apple_router_sleep cio_router;
	u64 cio_host_uid;
	bool cio_dp_released, dp_tunnel_pending;
	struct tb_tunnel *tunnel_list;
	struct group groups[2];
};
static struct tb tb;
static struct tb_cm cm;
static struct tb_switch root, child;
static struct tb_port link[2], ports[4], outs[4];
static struct tb_path paths[2];
static struct tb_tunnel tunnels[2];
static struct tb_nhi nhi;
static int fault, nth, ops, writes, drained, adapters, resumes, reads,
	allocated;
static bool ctl, physical, clients_awake, uid_wrong;
#define list_for_each_entry(t, h, m) for ((t) = *(h), (void)sizeof(#m); (t); (t) = (t)->next)
#define tb_switch_for_each_port(s, p) for ((p) = (s)->ports; (p); (p) = NULL)
static void mutex_lock(int *l)
{
	assert(!*l);
	*l = 1;
}
static void mutex_unlock(int *l)
{
	assert(*l);
	*l = 0;
}
static int *take(int *l)
{
	mutex_lock(l);
	return l;
}
static void drop(int **l)
{
	if (*l)
		mutex_unlock(*l);
}
#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define guard(type) \
	int *CAT(g_, __COUNTER__) __maybe_unused __cleanup(drop) = take
#define lockdep_assert_held(l) assert(*(l))
static void panic(const char *s, ...)
{
	assert(false);
}
static bool fail(int point)
{
	if (fault != point)
		return false;
	if (++ops != nth)
		return false;
	fault = 0;
	return true;
}
static void *kcalloc(int n, size_t s, int flags)
{
	if (fail(1))
		return NULL;
	allocated++;
	return calloc(n, s);
}
static void kfree(void *p)
{
	if (p) {
		allocated--;
		free(p);
	}
}
static bool tb_switch_is_usb4(struct tb_switch *s)
{
	return s->usb4;
}
static bool tb_port_is_null(struct tb_port *p)
{
	return p->null;
}
static bool tb_port_needs_host_dp_credits(struct tb_port *p)
{
	return p->host_dp;
}
static bool tb_path_is_invalid(struct tb_path *p)
{
	for (int i = 0; i < p->path_length; i++)
		if (p->hops[i].in_port->sw->is_unplugged)
			return true;
	return false;
}
static int tb_port_read(struct tb_port *p, void *value, int space, int off,
			int count)
{
	assert(physical && ctl);
	if (fail(2))
		return -EIO;
	assert(space == TB_CFG_HOPS && count == 2);
	memcpy(value, &p->hw, 8);
	p->read_before_write = true;
	reads++;
	return 0;
}
static int tb_port_write(struct tb_port *p, const void *value, int space,
			 int off, int count)
{
	assert(physical && ctl);
	if (fail(4))
		return -EIO;
	writes++;
	if (space == TB_CFG_PORT) {
		assert(off == ADP_CS_4 && count == 1);
		p->credits = *(u32 *)value;
	} else {
		assert(space == TB_CFG_HOPS && p->read_before_write &&
		       count == 2);
		memcpy(&p->hw, value, 8);
		p->read_before_write = false;
	}
	return 0;
}
static int __tb_path_deactivate_hop(struct tb_port *p, int index, bool clear_fc)
{
	assert(physical && ctl && !clear_fc);
	p->hw.enable = false;
	drained++;
	return fail(3) ? -ETIMEDOUT : 0;
}
static int tb_port_clear_counter(struct tb_port *p, int index)
{
	assert(physical && ctl);
	return 0;
}
/* ACTUAL_PATH */
static bool tb_tunnel_is_dma(struct tb_tunnel *t)
{
	return t->dma;
}
static bool tb_tunnel_is_dp(struct tb_tunnel *t)
{
	return t->dp;
}
static bool tb_tunnel_is_active(struct tb_tunnel *t)
{
	return t->active;
}
static bool tb_tunnel_is_pci(struct tb_tunnel *t)
{
	return t->pci;
}
static bool tb_tunnel_is_invalid(struct tb_tunnel *t)
{
	for (int i = 0; i < t->npaths; i++)
		if (tb_path_is_invalid(t->paths[i]))
			return true;
	return false;
}
static void tb_dp_dprx_stop(struct tb_tunnel *t)
{
	t->dprx_started = false;
	t->dprx_canceled = true;
}
static int tb_dp_dprx_start(struct tb_tunnel *t)
{
	assert(clients_awake);
	t->dprx_started = true;
	t->dprx_canceled = false;
	return 0;
}
/* ACTUAL_TUNNEL */
static struct tb_cm *tb_priv(struct tb *t)
{
	return &cm;
}
static bool tb_port_has_remote(struct tb_port *p)
{
	return p == &link[0] && p->remote;
}
static bool cancel_delayed_work_sync(struct work *w)
{
	bool pending = w->pending;

	w->pending = false;
	return pending;
}
static void *system_percpu_wq;
static void mod_delayed_work(void *q, struct work *w, int delay)
{
	assert(clients_awake);
	w->pending = true;
}
static void flush_workqueue(void *q)
{
	assert(!tb.lock && cm.cio_prepared);
}
static void tb_ctl_stop(void *c)
{
	assert(ctl);
	ctl = false;
}
static void tb_ctl_start(void *c)
{
	assert(physical && !ctl);
	ctl = true;
}
static int usb4_switch_read_uid(struct tb_switch *s, u64 *uid)
{
	assert(physical && ctl);
	*uid = s->uid + uid_wrong;
	return 0;
}
/* Display release and pairing are outside this model. */
static void tb_disconnect_and_release_dp(struct tb *t)
{
	(void)t;
}
static void tb_restore_dp_resources(struct tb_switch *s)
{
	(void)s;
}
static void tb_tunnel_dp(struct tb *t)
{
	(void)t;
}
static int tb_switch_configure(struct tb_switch *s)
{
	(void)s;
	assert(physical && ctl);
	enumerated++;
	return 0;
}
static unsigned long tb_route(struct tb_switch *s)
{
	return s != &root;
}
/* As the real one: a peer that did not come back is marked unplugged. */
static int tb_switch_resume(struct tb_switch *s, bool runtime)
{
	assert(physical && ctl);
	resumes++;
	if (fail(8))
		return -EIO;
	child.is_unplugged = dock_lost;
	return 0;
}
static void tb_restore_children(struct tb_switch *s)
{
	assert(physical && ctl);
}
static void tb_switch_enter_redrive(struct tb_switch *s)
{
	assert(physical && ctl);
}
static void tb_switch_exit_redrive(struct tb_switch *s)
{
	assert(physical && ctl);
}
static int tb_switch_suspend_cio(struct tb_switch *s)
{
	assert(drained && ctl);
	return fail(5) ? -EIO : 0;
}
static int router_sleep(struct tb_nhi *n)
{
	assert(ctl);
	return fail(6) ? -EIO : 0;
}
static int router_wakes;
static int router_wake(struct tb_nhi *n)
{
	assert(physical && ctl);
	router_wakes++;
	return 0;
}
static int apple_router_save_sleep(struct tb_switch *s,
				   struct apple_router_sleep *r)
{
	assert(ctl && !r->valid);
	if (fail(7))
		return -EIO;
	r->valid = true;
	r->data[0] = 0xcafe;
	return 0;
}
static int apple_router_restore_sleep(struct tb_switch *s,
				      struct apple_router_sleep *r)
{
	assert(physical && ctl && r->valid && r->data[0] == 0xcafe && enumerated);
	r->valid = false;
	return 0;
}
void tb_cio_sleep_complete(struct tb *t);
/* ACTUAL_CM */
static int pre_activate(struct tb_tunnel *t)
{
	assert(t->sleep_replay);
	return fail(9) ? -EIO : 0;
}
static int activate(struct tb_tunnel *t, bool on)
{
	assert(t->sleep_replay && on);
	adapters++;
	return fail(10) ? -EIO : 0;
}
static const struct tb_nhi_ops nhi_ops = { .router_sleep = router_sleep,
					  .router_wake = router_wake };
static void setup(void)
{
	assert(!allocated);
	memset(&cm, 0, sizeof(cm));
	memset(paths, 0, sizeof(paths));
	memset(tunnels, 0, sizeof(tunnels));
	memset(ports, 0, sizeof(ports));
	memset(link, 0, sizeof(link));
	root = (struct tb_switch){ .usb4 = true, .uid = 1, .ports = &link[0] };
	child = (struct tb_switch){ .usb4 = true, .uid = 2 };
	link[0].remote = &link[1];
	link[1].sw = &child;
	nhi.ops = &nhi_ops;
	tb = (struct tb){ .cm_ops = &tb_cm_ops,
			  .nhi = &nhi,
			  .root_switch = &root };
	for (int i = 0; i < 4; i++) {
		ports[i].sw = i % 2 ? &child : &root;
		ports[i].host_dp = true;
		ports[i].config.nfc_credits = 39;
		ports[i].credits = 39;
		outs[i].port = i + 1;
		ports[i].hw = (struct tb_regs_hop){ .enable = true,
						    .next_hop = 9 + i,
						    .out_port = i + 1,
						    .initial_credits = 17,
						    .weight = 4,
						    .priority = 3,
						    .ingress_fc = true,
						    .pending = true };
		ports[i].expected = ports[i].hw;
		ports[i].expected.pending = false;
		paths[i / 2].hops[i % 2] =
			(struct tb_path_hop){ .in_port = &ports[i],
					      .out_port = &outs[i],
					      .in_hop_index = 8 + i,
					      .next_hop_index = 9 + i,
					      .nfc_credits = 10,
					      .in_counter_index = 0 };
	}
	for (int i = 0; i < 2; i++) {
		paths[i].activated = true;
		paths[i].path_length = 2;
		tunnels[i] = (struct tb_tunnel){ .tb = &tb,
						 .src_port = &ports[2 * i],
						 .active = true,
						 .pci = !i,
						 .generation = 123 + i,
						 .npaths = 1,
						 .paths = { &paths[i] },
						 .pre_activate = pre_activate,
						 .activate = activate };
	}
	tunnels[0].next = &tunnels[1];
	cm.tunnel_list = tunnels;
	cm.hotplug_active = true;
	cm.groups[0].release_work.pending = true;
	fault = ops = writes = drained = adapters = resumes = reads = 0;
	nth = 1;
	physical = ctl = clients_awake = true;
	uid_wrong = dock_lost = false;
	link_waits = router_wakes = enumerated = 0;
}
static void inject(int point, int count)
{
	fault = point;
	nth = count;
	ops = 0;
}
static void poweroff(void)
{
	assert(cm.cio_ctl_stopped && !ctl &&
	       !cm.groups[0].release_work.pending);
	physical = false;
	for (int i = 0; i < 4; i++) {
		memset(&ports[i].hw, 0, 8);
		ports[i].credits = 0;
	}
}
static void check_restored(void)
{
	assert(!cm.cio_changed && !cm.cio_ctl_stopped && !cm.cio_router.valid &&
	       ctl);
	for (int i = 0; i < 4; i++) {
		assert(!memcmp(&ports[i].hw, &ports[i].expected, 8));
		assert(ports[i].credits == 39);
	}
	for (int i = 0; i < 2; i++)
		assert(tunnels[i].generation == 123UL + i &&
		       tunnels[i].active && !tunnels[i].sleep_changed);
}
static void complete(void)
{
	clients_awake = true;
	tb_cio_sleep_complete(&tb);
	assert(!allocated && !cm.cio_prepared && cm.hotplug_active &&
	       !cm.cio_bw_pending);
	for (int i = 0; i < 2; i++)
		assert(!paths[i].sleep_hops && !tunnels[i].sleep_paused);
}
int main(void)
{
	unsigned int cases = 0;

	setup();
	for (int n = 0; n < 3; n++) {
		assert(!tb_cio_sleep_prepare(&tb));
		clients_awake = false;
		assert(!tb_cio_sleep(&tb));
		poweroff();
		physical = true;
		assert(!tb_cio_wake(&tb, NULL));
		check_restored();
		assert(!cm.hotplug_active);
		complete();
		cases++;
	}
	for (int n = 1; n <= 2; n++) {
		setup();
		inject(1, n);
		assert(tb_cio_sleep_prepare(&tb) == -ENOMEM);
		assert(!allocated && !cm.cio_prepared && cm.hotplug_active);
		cases++;
	}
	for (int point = 2; point <= 6; point++) {
		if (point == 4)
			continue;
		setup();
		assert(!tb_cio_sleep_prepare(&tb));
		clients_awake = false;
		inject(point, point == 2 ? 3 : 1);
		assert(tb_cio_sleep(&tb) < 0);
		assert(!cm.cio_changed && ctl);
		for (int i = 0; i < 4; i++)
			assert(ports[i].hw.enable);
		complete();
		cases++;
	}
	for (int point = 4; point <= 10; point++) {
		if (point >= 5 && point <= 7)
			continue;
		setup();
		assert(!tb_cio_sleep_prepare(&tb));
		clients_awake = false;
		assert(!tb_cio_sleep(&tb));
		poweroff();
		physical = true;
		inject(point, 1);
		assert(tb_cio_wake(&tb, NULL) < 0);
		assert(cm.cio_changed && !cm.hotplug_active);
		assert(!tb_cio_wake(&tb, NULL));
		check_restored();
		complete();
		cases++;
	}
	setup();
	assert(!tb_cio_sleep_prepare(&tb));
	clients_awake = false;
	assert(!tb_cio_sleep(&tb));
	poweroff();
	physical = true;
	uid_wrong = true;
	assert(tb_cio_wake(&tb, NULL) == -ENODEV);
	assert(cm.cio_changed);
	uid_wrong = false;
	assert(!tb_cio_wake(&tb, NULL));
	check_restored();
	complete();
	cases++;
	/* A state save that fails after the vendor sleep: wake as from a cold start. */
	setup();
	assert(!tb_cio_sleep_prepare(&tb));
	clients_awake = false;
	inject(7, 1);
	assert(!tb_cio_sleep(&tb));
	assert(cm.cio_changed && !cm.cio_router.valid);
	poweroff();
	physical = true;
	assert(!tb_cio_wake(&tb, NULL));
	assert(enumerated == 1 && router_wakes == 1);
	check_restored();
	complete();
	cases++;
	/* A dock gone during the sleep is an unplug: its tunnels wait for complete. */
	for (int n = 0; n < 2; n++) {
		bool pci_lost = false;

		setup();
		assert(!tb_cio_sleep_prepare(&tb));
		clients_awake = false;
		assert(!tb_cio_sleep(&tb));
		poweroff();
		physical = true;
		dock_lost = true;
		tunnels[0].pci = !n;
		assert(!tb_cio_wake(&tb, &pci_lost));
		assert(pci_lost == !n && child.is_unplugged && router_wakes == 1);
		assert(!cm.cio_changed && !cm.cio_ctl_stopped && !cm.cio_router.valid && ctl);
		for (int i = 0; i < 2; i++)
			assert(tunnels[i].sleep_changed && !adapters);
		complete();
		assert(cm.resume_cleanup_pending);
		cases++;
	}
	/* Before the sleep a lost router refuses it. */
	setup();
	child.is_unplugged = true;
	assert(tb_cio_sleep_prepare(&tb) == -ENODEV);
	assert(!cm.cio_prepared && cm.hotplug_active);
	cases++;
	/* Display tunnels are released before this; one left over refuses it. */
	setup();
	tunnels[1].dp = true;
	assert(tb_cio_sleep_prepare(&tb) == -EBUSY);
	assert(!allocated && !cm.cio_prepared);
	cases++;
	setup();
	tunnels[1].dma = true;
	assert(tb_cio_sleep_prepare(&tb) == -EBUSY);
	assert(!allocated && !cm.cio_prepared);
	cases++;
	setup();
	ports[2].hw.out_port = 63;
	assert(!tb_cio_sleep_prepare(&tb));
	clients_awake = false;
	assert(tb_cio_sleep(&tb) == -EUCLEAN);
	assert(ports[0].hw.enable && ports[1].hw.enable);
	complete();
	cases++;
	setup();
	assert(!tb_cio_sleep_prepare(&tb));
	complete();
	cases++;
	printf("PASS %u native tunnel replay cases\n", cases);
}
