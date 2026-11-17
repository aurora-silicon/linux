// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Host fixture: the slot allocator is extracted from afk.c at build time. */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef int64_t s64;
#define MAX_PENDING_CMDS 8
#define READ_ONCE(x) (x)
#define dev_info(...) do { } while (0)
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
#define bitmap_empty(map, bits) ((void)(bits), *(map) == 0)

struct apple_epic_service;
struct apple_epic_service_ops {
	void (*teardown)(struct apple_epic_service *service);
	/* One instance ever live; a re-announcement supersedes it in place. */
	bool singleton;
};

struct apple_epic_service {
	bool enabled, torndown, debugfs;
	unsigned int users, channel, lock;
	unsigned long cmd_map[1];
	const struct apple_epic_service_ops *ops;
	char name[33];
	s64 unit;
	u32 power_gen;
};

struct apple_dcp {
	void *dev;
	u32 afk_power_gen;
};

struct apple_dcp_afkep {
	struct apple_dcp *dcp;
	u32 endpoint, num_channels;
	struct apple_epic_service services[AFK_MAX_CHANNEL];
};

static unsigned int teardowns;

static void afk_remove_service_debugfs(struct apple_epic_service *service)
{
	assert(service->enabled && service->debugfs);
	service->debugfs = false;
}

static void afk_service_retire(struct apple_epic_service *service)
{
	assert(service->enabled && !service->debugfs);
	service->enabled = false;
	service->torndown = true;
}

static void teardown(struct apple_epic_service *service)
{
	assert(!service->enabled && service->torndown && !service->debugfs);
	teardowns++;
}

/* PRODUCTION_SLOT */

static const struct apple_epic_service_ops ops = { .teardown = teardown };
static const struct apple_epic_service_ops other_ops;
static const struct apple_epic_service_ops singleton_ops = {
	.teardown = teardown,
	.singleton = true,
};

/* Model publication after allocation; poweroff performs no AFK teardown. */
static struct apple_epic_service *
announce_ops(struct apple_dcp_afkep *ep,
	     const struct apple_epic_service_ops *service_ops, const char *name,
	     s64 unit, unsigned int interface)
{
	struct apple_epic_service *service =
		afk_compact_slot(ep, service_ops, name, unit);

	assert(service);
	service->ops = service_ops;
	strcpy(service->name, name);
	service->unit = unit;
	service->channel = interface;
	service->enabled = true;
	service->torndown = false;
	service->debugfs = true;
	return service;
}

static struct apple_epic_service *
announce(struct apple_dcp_afkep *ep, const char *name, s64 unit,
	 unsigned int interface)
{
	return announce_ops(ep, &ops, name, unit, interface);
}

/* What one DCP poweron announces on an endpoint. */
#define SIBLINGS 4
static unsigned int poweron_announce(struct apple_dcp_afkep *ep,
				     unsigned int interface)
{
	bool hotplug = ep->endpoint >= 0x27 && ep->endpoint <= 0x29;

	announce(ep, "dcpav-service-epic", 0, interface++);
	announce(ep, "dcpdp-service-epic", 0, interface++);
	announce(ep, "dcpav-video-interface-epi", -1, interface++);
	/* DPAVCTRL-style siblings: same name, no unit, one generation. */
	if (!hotplug)
		for (unsigned int i = 0; i < SIBLINGS; i++)
			announce(ep, "dcpav-controller-epic:", -1, interface++);
	return interface;
}

/* Every endpoint, both DCPs, more power cycles than there are slots. */
static void power_cycle_test(u32 endpoint)
{
	struct apple_dcp dcps[2] = { 0 };
	struct apple_dcp_afkep eps[2] = { { .dcp = &dcps[0], .endpoint = endpoint },
					  { .dcp = &dcps[1], .endpoint = endpoint } };
	bool hotplug = endpoint >= 0x27 && endpoint <= 0x29;
	unsigned int per_gen = hotplug ? 3 : 3 + SIBLINGS;
	unsigned int interface[2] = { 1, 1 }, cycles = 64;
	unsigned int before = teardowns;

	for (unsigned int cycle = 0; cycle < cycles; cycle++) {
		for (unsigned int d = 0; d < 2; d++) {
			struct apple_dcp_afkep *ep = &eps[d];
			unsigned int first = interface[d], enabled = 0;

			dcps[d].afk_power_gen++;	/* iomfb_poweron() */
			interface[d] = poweron_announce(ep, first);
			assert(ep->num_channels == per_gen);
			for (unsigned int i = 0; i < ep->num_channels; i++) {
				struct apple_epic_service *s = &ep->services[i];

				if (!s->enabled)
					continue;
				enabled++;
				/* Only this generation's interfaces are bound. */
				assert(s->channel >= first && s->channel < interface[d]);
				assert(s->power_gen == dcps[d].afk_power_gen);
			}
			assert(enabled == per_gen);
		}
	}
	assert(teardowns - before == 2 * per_gen * (cycles - 1));
}

/* Hotplug endpoints also replace within one poweron generation. */
static void hotplug_test(void)
{
	struct apple_dcp dcp = { .afk_power_gen = 1 };
	struct apple_dcp_afkep ep = { .dcp = &dcp, .endpoint = 0x28 };
	struct apple_epic_service *av = announce(&ep, "dcpav-service-epic", 0, 1);

	for (unsigned int i = 0; i < 2 * AFK_MAX_CHANNEL; i++)
		assert(announce(&ep, "dcpav-service-epic", 0, 3 + i) == av);
	assert(ep.num_channels == 1 && av->channel == 2 + 2 * AFK_MAX_CHANNEL);
}

/*
 * A singleton AV interface (dcpav-audio/cec/video on AV_AUDIO_ENDPOINT_DESKTOP
 * 0x2b, which is not a hotplug endpoint) is re-announced on every DP relink
 * without a poweron. Only one is ever live, so each re-announcement must
 * reclaim the one slot in place instead of consuming a fresh channel until the
 * endpoint runs out at AFK_MAX_CHANNEL.
 */
static void singleton_test(void)
{
	struct apple_dcp dcp = { .afk_power_gen = 3 };
	struct apple_dcp_afkep ep = { .dcp = &dcp, .endpoint = 0x2b };
	struct apple_epic_service *first;
	unsigned int before;

	first = announce_ops(&ep, &singleton_ops, "dcpav-audio-interface-epic",
			     0, 1);
	before = teardowns;

	/* Many relinks within one generation: one slot, one live instance. */
	for (unsigned int i = 0; i < 4 * AFK_MAX_CHANNEL; i++)
		assert(announce_ops(&ep, &singleton_ops,
				    "dcpav-audio-interface-epic", 0, 3 + i) ==
		       first);
	assert(ep.num_channels == 1 && first->channel == 2 + 4 * AFK_MAX_CHANNEL);
	assert(teardowns - before == 4 * AFK_MAX_CHANNEL);

	/* A different name on the same endpoint still takes its own slot. */
	assert(announce_ops(&ep, &singleton_ops, "dcpav-cec-interface-epic", 0,
			    1000) == &ep.services[1]);
	assert(ep.num_channels == 2);
}

/* Same-generation siblings are not replacements on non-hotplug endpoints. */
static void controller_test(void)
{
	struct apple_dcp dcp = { .afk_power_gen = 7 };
	struct apple_dcp_afkep ep = { .dcp = &dcp, .endpoint = 0x24 };
	unsigned int before = teardowns;

	for (unsigned int i = 0; i < 4; i++)
		assert(announce(&ep, "dcpav-controller-epic:", -1, 2 * i + 1) ==
		       &ep.services[i]);
	assert(ep.num_channels == 4 && teardowns == before);
	for (unsigned int i = 0; i < 4; i++)
		assert(ep.services[i].enabled && ep.services[i].channel == 2 * i + 1);
	/* After the next poweron each sibling replaces one from before. */
	dcp.afk_power_gen++;
	for (unsigned int i = 0; i < 4; i++)
		assert(announce(&ep, "dcpav-controller-epic:", -1, 20 + i) ==
		       &ep.services[i]);
	assert(ep.num_channels == 4 && teardowns == before + 4);
}

static void identity_test(void)
{
	struct apple_dcp dcp = { .afk_power_gen = 1 };
	struct apple_dcp_afkep ep = { .dcp = &dcp, .endpoint = 0x2a };
	struct apple_epic_service *first, *second, *third;

	first = announce(&ep, "dcpav-service-epic", 0, 1);
	second = announce(&ep, "dcpav-service-epic", 1, 3);
	third = announce(&ep, "dcpdp-service-epic", 0, 5);
	assert(first != second && first != third && second != third);
	dcp.afk_power_gen++;
	assert(announce(&ep, "dcpav-service-epic", 0, 7) == first);
	assert(second->channel == 3 && third->channel == 5);
	/* A different binding must not take over the same name and unit. */
	assert(afk_compact_slot(&ep, &other_ops, "dcpav-service-epic", 0) ==
	       &ep.services[3]);
	/* No-unit names remain distinct from unit zero. */
	assert(announce(&ep, "dcpav-service-epic", -1, 9) == &ep.services[3]);
}

static void free_slot_test(void)
{
	struct apple_dcp dcp = { 0 };
	struct apple_dcp_afkep ep = { .dcp = &dcp, .endpoint = 0x2a };

	ep.num_channels = AFK_MAX_CHANNEL;
	for (unsigned int i = 0; i < AFK_MAX_CHANNEL; i++)
		ep.services[i].enabled = true;
	assert(!afk_compact_slot(&ep, &ops, "new", 0));
	ep.services[2].enabled = false;
	ep.services[2].users = 1;
	assert(!afk_compact_slot(&ep, &ops, "new", 0));
	ep.services[2].users = 0;
	ep.services[2].cmd_map[0] = 1;
	assert(!afk_compact_slot(&ep, &ops, "new", 0));
	ep.services[2].cmd_map[0] = 0;
	assert(afk_compact_slot(&ep, &ops, "new", 0) == &ep.services[2]);
}

int main(void)
{
	for (u32 endpoint = 0x20; endpoint <= 0x2f; endpoint++)
		power_cycle_test(endpoint);
	hotplug_test();
	singleton_test();
	controller_test();
	identity_test();
	free_slot_test();
	puts("PASS: compact slots: 64 poweron cycles, 16 endpoints, 2 DCPs");
	return 0;
}
