#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify compact AFK service start runs before publication and discards on failure.

Extracts the real compact-service admission/start/publish lifecycle from
afk.h, afk.c and dcp.c, compiles it with ASan/UBSan against a host stub of
afk_send_compact(), and checks:
- start (0x12) runs before publication/init/wakeup/debugfs so callers never
  observe an unstarted compact service;
- a failed start discards the service (teardown, disabled, torndown, ops/cookie
  cleared, zero users, retired channel, reusable slot) with and without
  ops->prepare or ops->teardown, and after singleton replacement;
- deferred starts (ops->defer_start or ops->prepare setting defer_start,
  including dcp_dpavctrl_service_prepare on interface 7) skip start during
  announcement and allow explicit afk_start_service() afterwards.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AFK_H = 'drivers/gpu/drm/apple/afk.h'
AFK_C = 'drivers/gpu/drm/apple/afk.c'
DCP_C = 'drivers/gpu/drm/apple/dcp.c'
DPAV_C = 'drivers/gpu/drm/apple/epic/dpavservep.c'
DPTX_C = 'drivers/gpu/drm/apple/dptxep.c'
DPTX_H = 'drivers/gpu/drm/apple/dptx-service.h'

MUTATIONS = {
    'no-discard-on-start-fail': (
        AFK_C,
        '\t\t\tafk_service_discard(service);\n\t\t\treturn;\n\t\t}\n\t}\n\n\tafk_service_publish(service);',
        '\t\t\t(void)ret;\n\t\t}\n\t}\n\n\tafk_service_publish(service);',
    ),
    'publish-before-start': (
        AFK_C,
        '\t/* Publish only once admission and compact start succeed. */\n\tservice->enabled = false;',
        '\tservice->enabled = true;',
    ),
    'skip-start': (
        AFK_C,
        '\tif (!service->defer_start) {',
        '\tif (false && !service->defer_start) {',
    ),
    'ignore-ops-defer': (
        AFK_C,
        '\tservice->defer_start = ops->defer_start;',
        '\tservice->defer_start = false;',
    ),
    'dpavctrl-no-defer-prepare': (
        DCP_C,
        '\tif ((service->channel & 7) == 7)\n\t\tservice->defer_start = true;',
        '\tif (false && (service->channel & 7) == 7)\n\t\tservice->defer_start = true;',
    ),
    'no-enabled-check-in-start': (
        AFK_C,
        '\tif (!service || !service->enabled)\n\t\treturn -EINVAL;',
        '\tif (!service)\n\t\treturn -EINVAL;',
    ),
    'reuse-pinned-slot': (
        AFK_C,
        '\t\tspin_lock_irqsave(&service->lock, flags);\n'
        '\t\tfree = !service->users &&\n'
        '\t\t       bitmap_empty(service->cmd_map, MAX_PENDING_CMDS);\n'
        '\t\tspin_unlock_irqrestore(&service->lock, flags);\n'
        '\t\tif (free) {\n'
        '\t\t\tservice->power_gen = gen;\n'
        '\t\t\treturn service;\n'
        '\t\t}\n'
        '\t\tbreak;',
        '\t\tservice->power_gen = gen;\n'
        '\t\treturn service;',
    ),
    'discard-clears-pinned-ops': (
        AFK_H,
        '\tif (!WARN_ON(service->users)) {\n'
        '\t\tservice->cookie = NULL;\n'
        '\t\tservice->ops = NULL;\n'
        '\t}',
        '\tservice->cookie = NULL;\n'
        '\tservice->ops = NULL;',
    ),
    'discard-keeps-defer-flag': (
        AFK_H,
        '\tservice->defer_start = false;\n'
        '\tservice->started = false;',
        '\tservice->started = false;',
    ),
    'dptxport-no-defer-unit0': (
        DPTX_C,
        '\t\tif (!unit)\n'
        '\t\t\tservice->defer_start = true;',
        '\t\tif (false && !unit)\n'
        '\t\t\tservice->defer_start = true;',
    ),
}


def block(text, start):
    opening = text.index('{', start)
    depth = 0
    for end in range(opening, len(text)):
        depth += text[end] == '{'
        depth -= text[end] == '}'
        if not depth:
            return text[start:end + 1]
    raise AssertionError('unbalanced production block')


def function(text, name):
    match = re.search(
        r'\n((?:static\s+)?(?:inline\s+)?[^\n;{}]*\b' + name + r'\([^;{}]*\)\n)\{',
        text,
    )
    assert match, name
    start = match.start(1)
    if text.startswith(name + '(', start):
        start = text.rindex('\n', 0, start - 1) + 1
    return block(text, start) + '\n'


HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;

#define AFK_MAX_CHANNEL 16
#define MAX_PENDING_CMDS 16
#define AFK_COMPACT_NAME_LEN 32
#define AFK_COMPACT_CUT_MIN 20
#define BIT(n) (1U << (n))

#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define smp_store_release(p, v) (*(p) = (v))
#define smp_load_acquire(p) (*(p))
static unsigned int warn_count;
#define WARN_ON(cond) ({ bool _c = !!(cond); if (_c) warn_count++; _c; })
#define trace_dptxport_init(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)

typedef int spinlock_t;
#define spin_lock_irqsave(l, f) do { assert(!*(l)); *(l) = 1; (f) = 0; } while (0)
#define spin_unlock_irqrestore(l, f) do { assert(*(l)); *(l) = 0; (void)(f); } while (0)

static bool bitmap_empty(const unsigned long *map, unsigned int bits)
{
	(void)bits;
	return *map == 0;
}

static size_t strscpy(char *dst, const char *src, size_t size)
{
	size_t n = strlen(src);

	if (!size)
		return 0;
	if (n >= size)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
	return n;
}

static int kstrtos64(const char *s, unsigned int base, s64 *res)
{
	char *end;
	long long v;

	if (!s || !*s)
		return -EINVAL;
	errno = 0;
	v = strtoll(s, &end, base);
	if (errno || *end)
		return -EINVAL;
	*res = v;
	return 0;
}

struct apple_epic_service;
struct apple_dcp_afkep;

struct apple_epic_service_ops {
	const char name[32];
	const char *compact_full;
	bool reusable;
	bool optional;
	bool singleton;
	bool open_later;
	bool defer_start;
	int (*prepare)(struct apple_epic_service *service, const char *name,
		       const char *class, s64 unit);
	void (*init)(struct apple_epic_service *service, const char *name,
		     const char *class, s64 unit);
	void (*teardown)(struct apple_epic_service *service);
};

struct apple_epic_service {
	const struct apple_epic_service_ops *ops;
	struct apple_dcp_afkep *ep;
	unsigned long cmd_map[1];
	u8 cmd_tag;
	spinlock_t lock;
	unsigned int users;
	u32 channel;
	bool enabled;
	bool torndown;
	void *cookie;
	char name[33];
	s64 unit;
	u32 power_gen;
	bool defer_start;
	bool started;
	bool debugfs;
};

struct dcp_desktop_state {
	struct apple_epic_service *dpavctrl_av_controller[2];
	struct apple_epic_service *dpavctrl_late_service;
	unsigned int dpavctrl_ready;
};

struct dptx_port {
	struct apple_epic_service *service;
	s64 unit;
	bool enabled;
	bool connected;
	bool closed;
	unsigned int enable_completion;
};

struct apple_dcp {
	void *dev;
	u32 afk_power_gen;
	u32 index;
	struct {
		u32 num_dptx_ports;
	} hw;
	struct dptx_port dptxport[2];
	struct dcp_desktop_state *desktop;
	bool has_panel;
};

struct apple_dcp_afkep {
	struct apple_dcp *dcp;
	u32 endpoint;
	bool compact;
	const struct apple_epic_service_ops *ops;
	struct apple_epic_service services[AFK_MAX_CHANNEL];
	u32 num_channels;
	unsigned int service_wq;
};

static bool dcp_has_panel(struct apple_dcp *dcp)
{
	return dcp->has_panel;
}

static bool dcp_fw_is_14_7(const struct apple_dcp *dcp)
{
	(void)dcp;
	return false;
}

static bool dcp_is_ext26(const struct apple_dcp *dcp)
{
	(void)dcp;
	return false;
}

static bool dcp_is_external_h17p(const struct apple_dcp *dcp)
{
	(void)dcp;
	return false;
}

static void complete_all(unsigned int *c)
{
	(*c)++;
}

static void reinit_completion(unsigned int *c)
{
	*c = 0;
}

static void afk_service_retry(struct apple_dcp_afkep *ep)
{
	(void)ep;
}

enum ev_kind {
	EV_PREPARE = 1,
	EV_SEND_START,
	EV_INIT,
	EV_WAKE,
	EV_DEBUGFS_ADD,
	EV_DEBUGFS_DEL,
	EV_TEARDOWN,
};

static enum ev_kind trace[512];
static unsigned int trace_n;
static void record(enum ev_kind ev)
{
	if (trace_n < sizeof(trace) / sizeof(trace[0]))
		trace[trace_n++] = ev;
}

static int send_compact_ret;
static unsigned int send_compact_calls;
static u16 last_start_intf;
static bool send_saw_enabled;

static int afk_send_compact(struct apple_dcp_afkep *ep, u16 intf_id, u8 type,
			    u8 category, const void *payload,
			    size_t payload_size, size_t wire_size)
{
	(void)payload;
	(void)payload_size;
	(void)wire_size;
	assert(type == 0x12 && category == 0);
	send_compact_calls++;
	last_start_intf = intf_id;
	for (u32 i = 0; i < ep->num_channels; i++)
		if (ep->services[i].channel == intf_id)
			send_saw_enabled = ep->services[i].enabled;
	record(EV_SEND_START);
	return send_compact_ret;
}

static void wake_up_all(unsigned int *wq)
{
	(*wq)++;
	record(EV_WAKE);
}

static void afk_populate_service_debugfs(struct apple_epic_service *srv)
{
	assert(srv->enabled && !srv->debugfs);
	srv->debugfs = true;
	record(EV_DEBUGFS_ADD);
}

static void afk_remove_service_debugfs(struct apple_epic_service *srv)
{
	if (srv->debugfs) {
		srv->debugfs = false;
		record(EV_DEBUGFS_DEL);
	}
}
'''

CHECKS = r'''
static int failures;
#define CHECK(c) do { if (!(c)) { \
	printf("FAIL line %d (%s: %s)\n", __LINE__, case_name, #c); \
	failures++; } } while (0)

static int prepare_ret;
static bool prepare_defer;
static bool prepare_saw_enabled;
static bool init_saw_enabled;
static bool init_saw_started;
static bool teardown_saw_cookie;
static unsigned int prepare_calls, init_calls, teardown_calls;
static int cookie_sentinel;

static int test_prepare(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
	(void)name;
	(void)class;
	(void)unit;
	prepare_calls++;
	prepare_saw_enabled = service->enabled;
	service->cookie = &cookie_sentinel;
	if (prepare_defer)
		service->defer_start = true;
	record(EV_PREPARE);
	return prepare_ret;
}

static void test_init(struct apple_epic_service *service, const char *name,
		      const char *class, s64 unit)
{
	(void)name;
	(void)class;
	(void)unit;
	init_calls++;
	init_saw_enabled = service->enabled;
	init_saw_started = service->started;
	record(EV_INIT);
}

static void test_teardown(struct apple_epic_service *service)
{
	teardown_calls++;
	teardown_saw_cookie = service->cookie == &cookie_sentinel;
	record(EV_TEARDOWN);
}

static void test_dptx_init(struct apple_epic_service *service, const char *name,
			   const char *class, s64 unit)
{
	(void)name;
	(void)class;
	(void)unit;
	init_calls++;
	init_saw_enabled = service->enabled;
	init_saw_started = service->started;
	dptx_service_publish(service);
	record(EV_INIT);
}

static void test_dptx_teardown(struct apple_epic_service *service)
{
	teardown_calls++;
	dptx_service_unbind(service);
	record(EV_TEARDOWN);
}

static struct apple_epic_service *owner_srv;

static void owner_init(struct apple_epic_service *service, const char *name,
		       const char *class, s64 unit)
{
	struct apple_epic_service *old = owner_srv;

	(void)name;
	(void)class;
	(void)unit;
	init_calls++;
	owner_srv = afk_service_get(service);
	if (old)
		afk_service_put(old);
	record(EV_INIT);
}

static void owner_teardown(struct apple_epic_service *service)
{
	teardown_calls++;
	if (owner_srv == service) {
		owner_srv = NULL;
		afk_service_put(service);
	}
	record(EV_TEARDOWN);
}

static void reset_ep(struct apple_dcp *dcp, struct dcp_desktop_state *desk,
		     struct apple_dcp_afkep *ep, u32 endpoint,
		     const struct apple_epic_service_ops *ops)
{
	memset(dcp, 0, sizeof(*dcp));
	memset(desk, 0, sizeof(*desk));
	memset(ep, 0, sizeof(*ep));
	dcp->desktop = desk;
	dcp->hw.num_dptx_ports = 2;
	ep->dcp = dcp;
	ep->endpoint = endpoint;
	ep->compact = true;
	ep->ops = ops;
	trace_n = 0;
	warn_count = 0;
	owner_srv = NULL;
	send_compact_ret = 0;
	send_compact_calls = 0;
	last_start_intf = 0;
	send_saw_enabled = false;
	prepare_ret = 0;
	prepare_defer = false;
	prepare_saw_enabled = false;
	init_saw_enabled = false;
	init_saw_started = false;
	teardown_saw_cookie = false;
	prepare_calls = init_calls = teardown_calls = 0;
}

static void announce(struct apple_dcp_afkep *ep, u16 ch, const char *wire)
{
	char buf[AFK_COMPACT_NAME_LEN] = { 0 };

	strscpy(buf, wire, sizeof(buf));
	afk_recv_handle_compact_init(ep, ch, buf, sizeof(buf));
}

int main(void)
{
	const char *case_name;
	struct apple_dcp dcp;
	struct dcp_desktop_state desk;
	struct apple_dcp_afkep ep;

	static const struct apple_epic_service_ops test_ops[] = {
		{
			.name = "prep-svc",
			.prepare = test_prepare,
			.init = test_init,
			.teardown = test_teardown,
		},
		{
			.name = "noprep-svc",
			.init = test_init,
			.teardown = test_teardown,
		},
		{
			.name = "aux-svc",
			.init = test_init,
		},
		{
			.name = "defer-ops-svc",
			.defer_start = true,
			.prepare = test_prepare,
			.init = test_init,
			.teardown = test_teardown,
		},
		{
			.name = "singleton-svc",
			.singleton = true,
			.prepare = test_prepare,
			.init = test_init,
			.teardown = test_teardown,
		},
		{
			.name = "pinned-singleton",
			.singleton = true,
			.prepare = test_prepare,
			.init = owner_init,
			.teardown = owner_teardown,
		},
		{},
	};

	static const struct apple_epic_service_ops dptx_ops[] = {
		{
			.name = "dcpdptx-port-epic",
			.prepare = dptxport_prepare,
			.init = test_dptx_init,
			.teardown = test_dptx_teardown,
		},
		{},
	};

	static const struct apple_epic_service_ops dpavctrl_ops[] = {
		{
			.name = "dcpav-controller-epic",
			.prepare = dcp_dpavctrl_service_prepare,
			.init = dcp_dpavctrl_service_init,
		},
		{
			.name = "dcpdp-controller-epic",
			.prepare = dcp_dpavctrl_service_prepare,
			.init = dcp_dpavctrl_service_init,
		},
		{},
	};

	/* 1. Success path: start runs before publication/init/wakeup/debugfs. */
	case_name = "start-ok";
	reset_ep(&dcp, &desk, &ep, 0x2b, test_ops);
	announce(&ep, 3, "dispext0:prep-svc:0");
	CHECK(prepare_calls == 1 && !prepare_saw_enabled);
	CHECK(send_compact_calls == 1 && last_start_intf == 3 && !send_saw_enabled);
	CHECK(init_calls == 1 && init_saw_enabled && init_saw_started);
	CHECK(teardown_calls == 0);
	CHECK(ep.services[0].enabled && ep.services[0].started && !ep.services[0].torndown);
	CHECK(afk_epic_find_service(&ep, 3) == &ep.services[0]);
	CHECK(trace_n == 5 && trace[0] == EV_PREPARE && trace[1] == EV_SEND_START &&
	      trace[2] == EV_INIT && trace[3] == EV_WAKE && trace[4] == EV_DEBUGFS_ADD);
	/* Idempotent start call after announcement sends no second 0x12. */
	CHECK(afk_start_service(&ep.services[0]) == 0 && send_compact_calls == 1);

	/* 2. Start failure with ops->prepare: discarded, never published/inited. */
	case_name = "start-fail-prepared";
	reset_ep(&dcp, &desk, &ep, 0x2c, test_ops);
	send_compact_ret = -EIO;
	announce(&ep, 5, "dispext0:prep-svc:1");
	CHECK(prepare_calls == 1 && !prepare_saw_enabled);
	CHECK(send_compact_calls == 1 && !send_saw_enabled);
	CHECK(teardown_calls == 1 && teardown_saw_cookie);
	CHECK(init_calls == 0 && ep.service_wq == 0 && !ep.services[0].debugfs);
	CHECK(!ep.services[0].enabled && ep.services[0].torndown && !ep.services[0].started);
	CHECK(!ep.services[0].ops && !ep.services[0].cookie && !ep.services[0].users);
	CHECK(!afk_epic_find_service(&ep, 5) && afk_service_retired(&ep.services[0], 5));
	CHECK(!afk_service_get(&ep.services[0]));
	CHECK(afk_start_service(&ep.services[0]) == -EINVAL);

	/* 3. Start failure without ops->prepare: never published, teardown called. */
	case_name = "start-fail-unprepared";
	reset_ep(&dcp, &desk, &ep, 0x2b, test_ops);
	send_compact_ret = -ETIMEDOUT;
	announce(&ep, 9, "dispext0:noprep-svc:0");
	CHECK(send_compact_calls == 1 && !send_saw_enabled);
	CHECK(teardown_calls == 1 && init_calls == 0 && ep.service_wq == 0);
	CHECK(!ep.services[0].enabled && ep.services[0].torndown && !ep.services[0].ops);
	CHECK(!afk_epic_find_service(&ep, 9) && afk_service_retired(&ep.services[0], 9));

	/* 4. Start failure without ops->prepare or ops->teardown: safe discard. */
	case_name = "start-fail-no-teardown";
	reset_ep(&dcp, &desk, &ep, 0x25, test_ops);
	send_compact_ret = -EPROTO;
	announce(&ep, 11, "dispext0:aux-svc:0");
	CHECK(send_compact_calls == 1 && init_calls == 0 && ep.service_wq == 0);
	CHECK(!ep.services[0].enabled && ep.services[0].torndown && !ep.services[0].ops);
	CHECK(!afk_epic_find_service(&ep, 11));

	/* 5. Repeated failed starts reclaim the same slot and allow retry of same channel. */
	case_name = "start-fail-slot-reuse";
	reset_ep(&dcp, &desk, &ep, 0x2b, test_ops);
	send_compact_ret = -EIO;
	for (unsigned int i = 0; i < 4 * AFK_MAX_CHANNEL; i++)
		announce(&ep, 5, "dispext0:prep-svc:0");
	CHECK(ep.num_channels == 1 && init_calls == 0 && teardown_calls == 4 * AFK_MAX_CHANNEL);
	send_compact_ret = 0;
	announce(&ep, 5, "dispext0:prep-svc:0");
	CHECK(ep.num_channels == 1 && init_calls == 1 && ep.services[0].enabled &&
	      ep.services[0].started);

	/* 6. Failed start on a singleton replacement leaves the slot free, not stuck. */
	case_name = "start-fail-singleton-replace";
	reset_ep(&dcp, &desk, &ep, 0x2b, test_ops);
	announce(&ep, 10, "dispext0:singleton-svc:0");
	CHECK(ep.num_channels == 1 && ep.services[0].enabled && ep.services[0].channel == 10);
	send_compact_ret = -EIO;
	announce(&ep, 12, "dispext0:singleton-svc:0");
	CHECK(ep.num_channels == 1 && init_calls == 1 && teardown_calls == 2);
	CHECK(!ep.services[0].enabled && ep.services[0].torndown && !ep.services[0].ops);
	CHECK(!afk_epic_find_service(&ep, 10) && !afk_epic_find_service(&ep, 12));
	send_compact_ret = 0;
	announce(&ep, 14, "dispext0:singleton-svc:0");
	CHECK(ep.num_channels == 1 && init_calls == 2 && ep.services[0].enabled &&
	      ep.services[0].started && ep.services[0].channel == 14);

	/* 7. ops->defer_start skips 0x12 at announcement and publishes unstarted. */
	case_name = "defer-ops-flag";
	reset_ep(&dcp, &desk, &ep, 0x28, test_ops);
	send_compact_ret = -EIO;
	announce(&ep, 4, "dispext0:defer-ops-svc:0");
	CHECK(send_compact_calls == 0 && init_calls == 1 && init_saw_enabled && !init_saw_started);
	CHECK(ep.services[0].enabled && ep.services[0].defer_start && !ep.services[0].started);

	/* 8. ops->prepare defer_start skips 0x12 and later afk_start_service() sends it. */
	case_name = "defer-in-prepare";
	reset_ep(&dcp, &desk, &ep, 0x2c, test_ops);
	prepare_defer = true;
	announce(&ep, 2, "dispext0:prep-svc:0");
	CHECK(send_compact_calls == 0 && init_calls == 1 && ep.services[0].enabled &&
	      !ep.services[0].started);
	CHECK(afk_start_service(&ep.services[0]) == 0);
	CHECK(send_compact_calls == 1 && ep.services[0].started);

	/* 9. Real dcp_dpavctrl_service_prepare defers interface 7, starts interface 5. */
	case_name = "dpavctrl-defer-and-fail";
	reset_ep(&dcp, &desk, &ep, 0x24, dpavctrl_ops);
	dcp.has_panel = true;
	send_compact_ret = -EIO;
	announce(&ep, 5, "disp0:dcpav-controller-epic");
	CHECK(send_compact_calls == 1 && !desk.dpavctrl_av_controller[1] && !desk.dpavctrl_ready);
	CHECK(!ep.services[0].enabled && ep.services[0].torndown);
	/* Interface 7 is deferred in prepare, so it publishes without sending 0x12. */
	announce(&ep, 7, "disp0:dcpdp-controller-epic");
	CHECK(send_compact_calls == 1 && ep.num_channels == 1);
	CHECK(ep.services[0].enabled && ep.services[0].defer_start && !ep.services[0].started);
	CHECK(desk.dpavctrl_late_service == &ep.services[0] && !desk.dpavctrl_ready);
	/* Once TX recovers, interface 5 starts before init and completes dpavctrl_ready. */
	send_compact_ret = 0;
	announce(&ep, 5, "disp0:dcpav-controller-epic");
	CHECK(send_compact_calls == 2 && ep.num_channels == 2);
	CHECK(desk.dpavctrl_av_controller[1] == &ep.services[1] && desk.dpavctrl_ready == 1);

	/* 10. Admission failure discards and resets defer_start/started cleanly. */
	case_name = "admit-fail";
	reset_ep(&dcp, &desk, &ep, 0x2b, test_ops);
	prepare_defer = true;
	prepare_ret = -EINVAL;
	announce(&ep, 8, "dispext0:prep-svc:0");
	CHECK(prepare_calls == 1 && send_compact_calls == 0 && teardown_calls == 1 &&
	      init_calls == 0 && !ep.services[0].enabled && ep.services[0].torndown);
	CHECK(!ep.services[0].defer_start && !ep.services[0].started && !warn_count);

	/*
	 * 11. Singleton replacement start failure while a caller still pins
	 * the predecessor: old slot keeps ops->name and cookie intact, and the
	 * failed replacement uses and discards a separate slot.
	 */
	case_name = "start-fail-singleton-pinned";
	reset_ep(&dcp, &desk, &ep, 0x2b, test_ops);
	announce(&ep, 10, "dispext0:pinned-singleton:0");
	CHECK(ep.num_channels == 1 && owner_srv == &ep.services[0] &&
	      ep.services[0].users == 1);
	{
		struct apple_epic_service *caller = afk_service_get(&ep.services[0]);

		CHECK(caller == &ep.services[0] && caller->users == 2);
		send_compact_ret = -EIO;
		announce(&ep, 12, "dispext0:pinned-singleton:0");
		CHECK(warn_count == 0);
		CHECK(ep.num_channels == 2 && !owner_srv && teardown_calls == 2);
		CHECK(!caller->enabled && caller->torndown && caller->channel == 10);
		CHECK(caller->users == 1 && caller->ops &&
		      !strcmp(caller->ops->name, "pinned-singleton") &&
		      caller->cookie == &cookie_sentinel);
		CHECK(!ep.services[1].enabled && ep.services[1].torndown &&
		      !ep.services[1].ops && !ep.services[1].cookie &&
		      !ep.services[1].defer_start && !ep.services[1].started &&
		      !ep.services[1].users);
		CHECK(!afk_epic_find_service(&ep, 10) && !afk_epic_find_service(&ep, 12));
		CHECK(afk_service_retired(&ep.services[1], 12));
		/* Releasing the caller pin lets the next announcement reclaim slot 0. */
		afk_service_put(caller);
		CHECK(caller->users == 0);
		send_compact_ret = 0;
		announce(&ep, 14, "dispext0:pinned-singleton:0");
		CHECK(ep.num_channels == 2 && owner_srv == &ep.services[0] &&
		      ep.services[0].enabled && ep.services[0].channel == 14 &&
		      ep.services[0].started && warn_count == 0);
	}

	/* 12. Pending cmd_map on predecessor also prevents in-place replacement. */
	case_name = "start-fail-singleton-cmd-pending";
	reset_ep(&dcp, &desk, &ep, 0x2b, test_ops);
	announce(&ep, 10, "dispext0:singleton-svc:0");
	ep.services[0].cmd_map[0] = 1;
	send_compact_ret = -EIO;
	announce(&ep, 12, "dispext0:singleton-svc:0");
	CHECK(warn_count == 0 && ep.num_channels == 2);
	CHECK(!ep.services[0].enabled && ep.services[0].torndown &&
	      ep.services[0].channel == 10 && ep.services[0].ops &&
	      !strcmp(ep.services[0].ops->name, "singleton-svc"));
	CHECK(!ep.services[1].enabled && ep.services[1].torndown && !ep.services[1].ops);

	/* 13. Defense-in-depth: afk_service_discard() preserves ops/cookie if pinned. */
	case_name = "discard-guard-pinned";
	reset_ep(&dcp, &desk, &ep, 0x2b, test_ops);
	ep.services[0].ops = &test_ops[0];
	ep.services[0].ep = &ep;
	ep.services[0].cookie = &cookie_sentinel;
	ep.services[0].enabled = true;
	ep.services[0].defer_start = true;
	ep.services[0].started = true;
	ep.services[0].users = 1;
	afk_service_discard(&ep.services[0]);
	CHECK(warn_count == 1);
	CHECK(!ep.services[0].enabled && ep.services[0].torndown);
	CHECK(!ep.services[0].defer_start && !ep.services[0].started);
	CHECK(ep.services[0].ops == &test_ops[0] &&
	      ep.services[0].cookie == &cookie_sentinel);

	/* 14. Real dptxport_prepare(): unit 0 deferred, unit 1 fails & discards cleanly. */
	case_name = "dptxport-compact-prepare-and-fail";
	reset_ep(&dcp, &desk, &ep, 0x2c, dptx_ops);
	send_compact_ret = -EIO;
	announce(&ep, 1, "dispext0:dcpdptx-port-epic:0");
	CHECK(send_compact_calls == 0 && init_calls == 1 && !warn_count);
	CHECK(ep.services[0].enabled && ep.services[0].defer_start && !ep.services[0].started);
	CHECK(dcp.dptxport[0].service == &ep.services[0] && dcp.dptxport[0].enabled);
	announce(&ep, 3, "dispext0:dcpdptx-port-epic:1");
	CHECK(send_compact_calls == 1 && init_calls == 1 && teardown_calls == 1 && !warn_count);
	CHECK(!dcp.dptxport[1].service && !dcp.dptxport[1].enabled && !dcp.dptxport[1].closed);
	CHECK(!ep.services[1].enabled && ep.services[1].torndown && !ep.services[1].ops);
	send_compact_ret = 0;
	announce(&ep, 3, "dispext0:dcpdptx-port-epic:1");
	CHECK(ep.num_channels == 2 && send_compact_calls == 2 && init_calls == 2);
	CHECK(dcp.dptxport[1].service == &ep.services[1] && dcp.dptxport[1].enabled &&
	      ep.services[1].started);
	CHECK(afk_start_service(&ep.services[0]) == 0 && send_compact_calls == 3 &&
	      ep.services[0].started && !warn_count);

	if (failures)
		return 1;
	puts("PASS compact AFK start: pre-publish start, discard on failure, pinned replace, deferred ops/prepare, dpavctrl, dptxport");
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()

    sources = {
        AFK_H: (ROOT / AFK_H).read_text(),
        AFK_C: (ROOT / AFK_C).read_text(),
        DCP_C: (ROOT / DCP_C).read_text(),
        DPAV_C: (ROOT / DPAV_C).read_text(),
        DPTX_C: (ROOT / DPTX_C).read_text(),
        DPTX_H: (ROOT / DPTX_H).read_text(),
    }

    if args.mutation:
        path, old, new = MUTATIONS[args.mutation]
        assert sources[path].count(old) == 1, args.mutation
        sources[path] = sources[path].replace(old, new)

    # Verify wiring in production tables so defer-before-start holds on real ops.
    assert '.prepare = dcp_dpavctrl_service_prepare,' in sources[DCP_C]
    dpav_ops = sources[DPAV_C][sources[DPAV_C].index('dpavservep_ops[]'):]
    dp_entry = dpav_ops[dpav_ops.index('"dcpdp-service-epic"'):]
    dp_entry = dp_entry[:dp_entry.index('}')]
    assert '.defer_start = true,' in dp_entry
    dptx_ops = sources[DPTX_C][sources[DPTX_C].index('dptxep_compact_ops[]'):]
    dptx_entry = dptx_ops[dptx_ops.index('"dcpdptx-port-epic"'):]
    dptx_entry = dptx_entry[:dptx_entry.index('}')]
    assert '.prepare = dptxport_prepare,' in dptx_entry
    assert '.teardown = dptxport_teardown,' in dptx_entry
    assert '.init = dptxport_init,' in dptx_entry

    parts = [
        function(sources[AFK_H], 'afk_service_get'),
        function(sources[AFK_H], 'afk_service_put'),
        function(sources[AFK_H], 'afk_service_retire'),
        function(sources[AFK_H], 'afk_service_matches'),
        function(sources[AFK_H], 'afk_service_retired'),
        function(sources[AFK_H], 'afk_service_discard'),
        function(sources[AFK_H], 'afk_service_publish'),
        function(sources[DPTX_H], 'dptx_service_port'),
        function(sources[DPTX_H], 'dptx_service_bind'),
        function(sources[DPTX_H], 'dptx_service_publish'),
        function(sources[DPTX_H], 'dptx_service_unbind'),
        function(sources[AFK_C], 'afk_epic_find_service'),
        function(sources[AFK_C], 'afk_start_compact_service'),
        function(sources[AFK_C], 'afk_start_service'),
        function(sources[AFK_C], 'afk_compact_cut_of'),
        function(sources[AFK_C], 'afk_compact_match_service'),
        function(sources[AFK_C], 'afk_compact_slot'),
        function(sources[AFK_C], 'afk_recv_handle_compact_init'),
        function(sources[DCP_C], 'dcp_dpavctrl_service_prepare'),
        function(sources[DCP_C], 'dcp_dpavctrl_service_init'),
        function(sources[DPTX_C], 'dptxport_prepare'),
    ]

    code = HARNESS + '\n'.join(parts) + CHECKS
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'check.c'
        path.write_text(code)
        subprocess.run(
            ['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-parameter',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             str(path), '-o', str(path.with_suffix(''))],
            check=True,
        )
        sys.exit(subprocess.run([str(path.with_suffix(''))]).returncode)


if __name__ == '__main__':
    main()
