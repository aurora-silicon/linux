// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T6000/T8112 memory-cache-controller bandwidth PMU
 *
 * The counter banks are shared with cache and DRAM management. Every enable
 * update is therefore an RMW limited to the allocated slot, and no hardware
 * state is touched until perf starts an event.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/soc/apple/mcc_pmu.h>

#include "apple_mcc_pmu.h"

#define APPLE_MCC_MAX_COUNTERS 32
#define T6000_AMCC_ENABLE 0x158000
#define T6000_AMCC_CONTROL(n) (0x158050 + 0x10 * (n))
#define T6000_AMCC_VALUE(n) (0x159000 + 0x8 * (n))
#define T6000_AMCC_COMMIT 0x152000
#define T6000_DCS_BASE 0x1c000
#define T6000_DCS_ENABLE 0x8000
#define T6000_DCS_CONTROL(n, wr) (0x8058 + 0x8 * (wr) + 0x10 * (n))
#define T6000_DCS_VALUE(n, wr) (0x9000 + 0x8 * (wr) + 0x10 * (n))

#define T8112_AMCC_ENABLE 0x58000
#define T8112_AMCC_CONTROL(n) (0x58010 + 0x10 * (n))
#define T8112_AMCC_RD_VALUE(n) (0x59000 + 0x8 * (n))
#define T8112_AMCC_WR_VALUE(n) (0x5a000 + 0x8 * (n))
#define T8112_DCS_BASE 0x1c000
#define T8112_DCS_ENABLE 0x8000
#define T8112_DCS_CONTROL(n) (0x8010 + 0x10 * (n))
#define T8112_DCS_VALUE(n) (0x9000 + 0x8 * (n))
#define T8112_WR_COMMIT 0x52000
#define T8112_WR_ENABLE 0x2004
#define T8112_WR_CONTROL(n) (0x3004 + 0x1c * (n))
#define T8112_WR_GLOBAL_ENABLE_REG 0x000c
#define T8112_WR_GLOBAL_EVENT_REG 0x0010

struct apple_mcc_pmu {
	struct pmu pmu;
	struct apple_mcc_pmu_aux *aux;
	raw_spinlock_t lock;
	DECLARE_BITMAP(used, APPLE_MCC_MAX_COUNTERS);
	struct perf_event *events[APPLE_MCC_MAX_COUNTERS];
	unsigned int t8112_writers;
};

static inline struct apple_mcc_pmu *to_apple_mcc_pmu(struct pmu *pmu)
{
	return container_of(pmu, struct apple_mcc_pmu, pmu);
}

static u64 apple_mcc_rmw64(void __iomem *addr, u64 mask, bool set)
{
	u64 value = readq_relaxed(addr);
	u64 next = set ? value | mask : value & ~mask;

	if (next != value)
		writeq_relaxed(next, addr);
	return next;
}

static void apple_mcc_rmw32(void __iomem *addr, u32 mask, bool set)
{
	u32 value = readl_relaxed(addr);
	u32 next = set ? value | mask : value & ~mask;

	if (next != value)
		writel_relaxed(next, addr);
}

static u64 apple_mcc_t8112_agent(unsigned int agent)
{
	switch (agent) {
	case APPLE_MCC_AGENT_PCPU0:
		return T8112_AGENT_PCPU;
	case APPLE_MCC_AGENT_ECPU:
		return T8112_AGENT_ECPU;
	case APPLE_MCC_AGENT_GFX:
		return T8112_AGENT_GFX;
	case APPLE_MCC_AGENT_ANE:
		return T8112_AGENT_ANE;
	case APPLE_MCC_AGENT_VDEC:
		return T8112_AGENT_VDEC;
	case APPLE_MCC_AGENT_VENC:
		return T8112_AGENT_VENC;
	case APPLE_MCC_AGENT_DISPLAY:
		return T8112_AGENT_DISPLAY;
	case APPLE_MCC_AGENT_EXT_DISPLAY:
		return T8112_AGENT_EXT_DISPLAY;
	case APPLE_MCC_AGENT_CODEC:
		return T8112_AGENT_CODEC;
	case APPLE_MCC_AGENT_ANS:
		return T8112_AGENT_ANS;
	case APPLE_MCC_AGENT_ISP:
		return T8112_AGENT_ISP;
	case APPLE_MCC_AGENT_JPEG:
		return T8112_AGENT_JPEG;
	case APPLE_MCC_AGENT_MSR:
		return T8112_AGENT_MSR;
	case APPLE_MCC_AGENT_PCIE:
		return T8112_AGENT_PCIE;
	case APPLE_MCC_AGENT_ATC0:
		return T8112_AGENT_ATC0;
	case APPLE_MCC_AGENT_ATC1:
		return T8112_AGENT_ATC1;
	case APPLE_MCC_AGENT_ATC2:
		return T8112_AGENT_ATC2;
	case APPLE_MCC_AGENT_ATC3:
		return T8112_AGENT_ATC3;
	default:
		return 0;
	}
}

static u64 apple_mcc_t6000_agent(unsigned int agent)
{
	switch (agent) {
	case APPLE_MCC_AGENT_PCPU0:
		return T6000_AGENT_PCPU0;
	case APPLE_MCC_AGENT_PCPU1:
		return T6000_AGENT_PCPU1;
	case APPLE_MCC_AGENT_ECPU:
		return T6000_AGENT_ECPU;
	case APPLE_MCC_AGENT_GFX:
		return T6000_AGENT_GFX;
	case APPLE_MCC_AGENT_ANE:
		return T6000_AGENT_ANE;
	case APPLE_MCC_AGENT_VDEC:
		return T6000_AGENT_VDEC;
	case APPLE_MCC_AGENT_CODEC:
		return T6000_AGENT_CODEC;
	case APPLE_MCC_AGENT_MSR:
		return T6000_AGENT_MSR;
	case APPLE_MCC_AGENT_JPEG:
		return T6000_AGENT_JPEG;
	case APPLE_MCC_AGENT_PCIE:
		return T6000_AGENT_PCIE;
	default:
		return 0;
	}
}

static u64 apple_mcc_event_agent(struct apple_mcc_pmu *mcc,
				 struct perf_event *event)
{
	unsigned int agent = FIELD_GET(APPLE_MCC_CFG_AGENT, event->attr.config);

	if (mcc->aux->type == APPLE_MCC_PMU_T8112)
		return apple_mcc_t8112_agent(agent);
	return apple_mcc_t6000_agent(agent);
}

static u64 apple_mcc_raw_read(struct apple_mcc_pmu *mcc,
			      struct perf_event *event)
{
	unsigned int slot = event->hw.idx;
	bool write = event->attr.config & APPLE_MCC_CFG_WRITE;
	bool dcs = event->attr.config & APPLE_MCC_CFG_DCS;
	void __iomem *base;
	u64 value;

	if (mcc->aux->type == APPLE_MCC_PMU_T6000) {
		base = mcc->aux->maps[0].base;
		if (dcs)
			value = readq_relaxed(base + T6000_DCS_BASE +
					      T6000_DCS_VALUE(slot, write));
		else
			value = readq_relaxed(base + T6000_AMCC_VALUE(slot));
		/* reg[0] represents each of the four equivalent controllers. */
		return value * mcc->aux->nr_maps;
	}

	if (dcs) {
		base = mcc->aux->maps[2].base;
		value = readq_relaxed(base + T8112_DCS_BASE +
				      T8112_DCS_VALUE(slot));
		/* T8112 has eight equivalent DCS channels. */
		return value * 8;
	}

	base = mcc->aux->maps[0].base;
	return readq_relaxed(base + (write ? T8112_AMCC_WR_VALUE(slot) :
					     T8112_AMCC_RD_VALUE(slot)));
}

static void apple_mcc_event_update(struct apple_mcc_pmu *mcc,
				   struct perf_event *event)
{
	u64 now = apple_mcc_raw_read(mcc, event);
	u64 prev = local64_xchg(&event->hw.prev_count, now);

	local64_add(now - prev, &event->count);
}

static void apple_mcc_program_t6000(struct apple_mcc_pmu *mcc,
				    struct perf_event *event, bool enable)
{
	void __iomem *base = mcc->aux->maps[0].base;
	unsigned int slot = event->hw.idx;
	u64 mask = BIT_ULL(slot);
	u64 agent = apple_mcc_event_agent(mcc, event);
	bool write = event->attr.config & APPLE_MCC_CFG_WRITE;
	bool dcs = event->attr.config & APPLE_MCC_CFG_DCS;

	if (!enable) {
		if (dcs) {
			apple_mcc_rmw64(base + T6000_DCS_BASE +
						T6000_DCS_ENABLE,
					mask, false);
		} else {
			apple_mcc_rmw64(base + T6000_AMCC_ENABLE, mask, false);
			writel_relaxed(t6000_amcc_commit_word(),
				       base + T6000_AMCC_COMMIT);
		}
		return;
	}

	if (dcs) {
		void __iomem *enable_reg =
			base + T6000_DCS_BASE + T6000_DCS_ENABLE;

		writeq_relaxed(t6000_dcs_control_word(agent, write),
			       base + T6000_DCS_BASE +
				       T6000_DCS_CONTROL(slot, write));
		apple_mcc_rmw64(enable_reg, mask, true);
	} else {
		writeq_relaxed(t6000_amcc_control_word(agent, write),
			       base + T6000_AMCC_CONTROL(slot));
		apple_mcc_rmw64(base + T6000_AMCC_ENABLE, mask, true);
		writel_relaxed(t6000_amcc_commit_word(),
			       base + T6000_AMCC_COMMIT);
	}
}

static void apple_mcc_program_t8112(struct apple_mcc_pmu *mcc,
				    struct perf_event *event, bool enable)
{
	unsigned int slot = event->hw.idx;
	u64 mask = BIT_ULL(slot);
	u64 control =
		t8112_control_word(apple_mcc_event_agent(mcc, event),
				   event->attr.config & APPLE_MCC_CFG_WRITE);
	bool dcs = event->attr.config & APPLE_MCC_CFG_DCS;
	bool write = event->attr.config & APPLE_MCC_CFG_WRITE;
	void __iomem *base;

	if (dcs) {
		base = mcc->aux->maps[2].base + T8112_DCS_BASE;
		if (enable)
			writeq_relaxed(control, base + T8112_DCS_CONTROL(slot));
		apple_mcc_rmw64(base + T8112_DCS_ENABLE, mask, enable);
	} else if (!write) {
		base = mcc->aux->maps[0].base;
		if (enable)
			writeq_relaxed(control,
				       base + T8112_AMCC_CONTROL(slot));
		apple_mcc_rmw64(base + T8112_AMCC_ENABLE, mask, enable);
	} else {
		void __iomem *amcc = mcc->aux->maps[0].base;
		void __iomem *dcs_base =
			mcc->aux->maps[2].base + T8112_DCS_BASE;
		u32 ctrl = T8112_WR_CONTROL(slot);

		/*
		 * The write family is a four-word descriptor. Its slot-enable
		 * word is shared with other counters, while the two global words
		 * and AMCC commit are command-only and reference counted here.
		 */
		if (enable) {
			writel_relaxed(t8112_write_desc0(), dcs_base + ctrl);
			writel_relaxed(t8112_write_desc1(),
				       dcs_base + ctrl + 4);
			writel_relaxed(lower_32_bits(control),
				       dcs_base + ctrl + 8);
			writel_relaxed(t8112_write_desc3(),
				       dcs_base + ctrl + 12);
			apple_mcc_rmw32(dcs_base + T8112_WR_ENABLE, BIT(slot),
					true);
			if (!mcc->t8112_writers++) {
				writel_relaxed(
					t8112_write_global_event_class(),
					dcs_base + T8112_WR_GLOBAL_EVENT_REG);
				writel_relaxed(
					T8112_WR_GLOBAL_ENABLE,
					dcs_base + T8112_WR_GLOBAL_ENABLE_REG);
				writel_relaxed(T8112_WR_AMCC_COMMIT,
					       amcc + T8112_WR_COMMIT);
			}
		} else {
			apple_mcc_rmw32(dcs_base + T8112_WR_ENABLE, BIT(slot),
					false);
			if (!--mcc->t8112_writers) {
				writel_relaxed(
					0,
					dcs_base + T8112_WR_GLOBAL_ENABLE_REG);
				writel_relaxed(0, amcc + T8112_WR_COMMIT);
			}
		}
	}
}

static void apple_mcc_program(struct apple_mcc_pmu *mcc,
			      struct perf_event *event, bool enable)
{
	if (mcc->aux->type == APPLE_MCC_PMU_T8112)
		apple_mcc_program_t8112(mcc, event, enable);
	else
		apple_mcc_program_t6000(mcc, event, enable);
}

static int apple_mcc_event_init(struct perf_event *event)
{
	struct apple_mcc_pmu *mcc = to_apple_mcc_pmu(event->pmu);
	u64 config_mask = APPLE_MCC_CFG_AGENT | APPLE_MCC_CFG_WRITE |
			  APPLE_MCC_CFG_DCS;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;
	if (event->attr.config & ~config_mask || event->attr.sample_period ||
	    event->attr.exclude_user || event->attr.exclude_kernel ||
	    event->attr.exclude_hv || event->attr.exclude_idle)
		return -EINVAL;
	if (!apple_mcc_event_agent(mcc, event))
		return -EOPNOTSUPP;

	event->hw.idx = -1;
	event->hw.state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	return 0;
}

static int apple_mcc_event_add(struct perf_event *event, int flags)
{
	struct apple_mcc_pmu *mcc = to_apple_mcc_pmu(event->pmu);
	unsigned long irqflags;
	unsigned int slot;

	raw_spin_lock_irqsave(&mcc->lock, irqflags);
	slot = find_first_zero_bit(mcc->used, APPLE_MCC_MAX_COUNTERS);
	if (slot == APPLE_MCC_MAX_COUNTERS) {
		raw_spin_unlock_irqrestore(&mcc->lock, irqflags);
		return -EAGAIN;
	}
	__set_bit(slot, mcc->used);
	mcc->events[slot] = event;
	event->hw.idx = slot;
	event->hw.state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	raw_spin_unlock_irqrestore(&mcc->lock, irqflags);

	if (flags & PERF_EF_START)
		event->pmu->start(event, flags);
	return 0;
}

static void apple_mcc_event_start(struct perf_event *event, int flags)
{
	struct apple_mcc_pmu *mcc = to_apple_mcc_pmu(event->pmu);
	unsigned long irqflags;

	raw_spin_lock_irqsave(&mcc->lock, irqflags);
	if (event->hw.state & PERF_HES_STOPPED) {
		apple_mcc_program(mcc, event, true);
		local64_set(&event->hw.prev_count,
			    apple_mcc_raw_read(mcc, event));
		event->hw.state = 0;
	}
	raw_spin_unlock_irqrestore(&mcc->lock, irqflags);
}

static void apple_mcc_event_stop(struct perf_event *event, int flags)
{
	struct apple_mcc_pmu *mcc = to_apple_mcc_pmu(event->pmu);
	unsigned long irqflags;

	raw_spin_lock_irqsave(&mcc->lock, irqflags);
	if (!(event->hw.state & PERF_HES_STOPPED)) {
		apple_mcc_event_update(mcc, event);
		apple_mcc_program(mcc, event, false);
		event->hw.state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	}
	raw_spin_unlock_irqrestore(&mcc->lock, irqflags);
}

static void apple_mcc_event_del(struct perf_event *event, int flags)
{
	struct apple_mcc_pmu *mcc = to_apple_mcc_pmu(event->pmu);
	unsigned long irqflags;
	int slot = event->hw.idx;

	apple_mcc_event_stop(event, PERF_EF_UPDATE);
	raw_spin_lock_irqsave(&mcc->lock, irqflags);
	if (slot >= 0) {
		mcc->events[slot] = NULL;
		__clear_bit(slot, mcc->used);
		event->hw.idx = -1;
	}
	raw_spin_unlock_irqrestore(&mcc->lock, irqflags);
}

static void apple_mcc_event_read(struct perf_event *event)
{
	struct apple_mcc_pmu *mcc = to_apple_mcc_pmu(event->pmu);
	unsigned long irqflags;

	raw_spin_lock_irqsave(&mcc->lock, irqflags);
	if (!(event->hw.state & PERF_HES_STOPPED))
		apple_mcc_event_update(mcc, event);
	raw_spin_unlock_irqrestore(&mcc->lock, irqflags);
}

PMU_FORMAT_ATTR(event, "config:0-7");
PMU_FORMAT_ATTR(direction, "config:8");
PMU_FORMAT_ATTR(family, "config:9");

static struct attribute *apple_mcc_format_attrs[] = {
	&format_attr_event.attr,
	&format_attr_direction.attr,
	&format_attr_family.attr,
	NULL,
};

static const struct attribute_group apple_mcc_format_group = {
	.name = "format",
	.attrs = apple_mcc_format_attrs,
};

#define MCC_EVENT(_name, _agent, _write, _dcs)                          \
	PMU_EVENT_ATTR_STRING(                                          \
		_name, apple_mcc_event_attr_##_name,                    \
		"event=" __stringify(_agent) ",direction=" __stringify( \
			_write) ",family=" __stringify(_dcs))
#define MCC_AGENT_EVENTS(_name, _agent)          \
	MCC_EVENT(_name##_rd, _agent, 0, 0);     \
	MCC_EVENT(_name##_wr, _agent, 1, 0);     \
	MCC_EVENT(_name##_dcs_rd, _agent, 0, 1); \
	MCC_EVENT(_name##_dcs_wr, _agent, 1, 1)

MCC_AGENT_EVENTS(pcpu0, APPLE_MCC_AGENT_PCPU0);
MCC_AGENT_EVENTS(pcpu1, APPLE_MCC_AGENT_PCPU1);
MCC_AGENT_EVENTS(ecpu, APPLE_MCC_AGENT_ECPU);
MCC_AGENT_EVENTS(gfx, APPLE_MCC_AGENT_GFX);
MCC_AGENT_EVENTS(ane, APPLE_MCC_AGENT_ANE);
MCC_AGENT_EVENTS(vdec, APPLE_MCC_AGENT_VDEC);
MCC_AGENT_EVENTS(venc, APPLE_MCC_AGENT_VENC);
MCC_AGENT_EVENTS(display, APPLE_MCC_AGENT_DISPLAY);
MCC_AGENT_EVENTS(ext_display, APPLE_MCC_AGENT_EXT_DISPLAY);
MCC_AGENT_EVENTS(codec, APPLE_MCC_AGENT_CODEC);
MCC_AGENT_EVENTS(ans, APPLE_MCC_AGENT_ANS);
MCC_AGENT_EVENTS(isp, APPLE_MCC_AGENT_ISP);
MCC_AGENT_EVENTS(jpeg, APPLE_MCC_AGENT_JPEG);
MCC_AGENT_EVENTS(msr, APPLE_MCC_AGENT_MSR);
MCC_AGENT_EVENTS(pcie, APPLE_MCC_AGENT_PCIE);
MCC_AGENT_EVENTS(atc0, APPLE_MCC_AGENT_ATC0);
MCC_AGENT_EVENTS(atc1, APPLE_MCC_AGENT_ATC1);
MCC_AGENT_EVENTS(atc2, APPLE_MCC_AGENT_ATC2);
MCC_AGENT_EVENTS(atc3, APPLE_MCC_AGENT_ATC3);

#define MCC_EVENT_ATTR(_name) (&apple_mcc_event_attr_##_name.attr.attr)

static struct attribute *apple_mcc_event_attrs[] = {
	MCC_EVENT_ATTR(pcpu0_rd),
	MCC_EVENT_ATTR(pcpu0_wr),
	MCC_EVENT_ATTR(pcpu0_dcs_rd),
	MCC_EVENT_ATTR(pcpu0_dcs_wr),
	MCC_EVENT_ATTR(pcpu1_rd),
	MCC_EVENT_ATTR(pcpu1_wr),
	MCC_EVENT_ATTR(pcpu1_dcs_rd),
	MCC_EVENT_ATTR(pcpu1_dcs_wr),
	MCC_EVENT_ATTR(ecpu_rd),
	MCC_EVENT_ATTR(ecpu_wr),
	MCC_EVENT_ATTR(ecpu_dcs_rd),
	MCC_EVENT_ATTR(ecpu_dcs_wr),
	MCC_EVENT_ATTR(gfx_rd),
	MCC_EVENT_ATTR(gfx_wr),
	MCC_EVENT_ATTR(gfx_dcs_rd),
	MCC_EVENT_ATTR(gfx_dcs_wr),
	MCC_EVENT_ATTR(ane_rd),
	MCC_EVENT_ATTR(ane_wr),
	MCC_EVENT_ATTR(ane_dcs_rd),
	MCC_EVENT_ATTR(ane_dcs_wr),
	MCC_EVENT_ATTR(vdec_rd),
	MCC_EVENT_ATTR(vdec_wr),
	MCC_EVENT_ATTR(vdec_dcs_rd),
	MCC_EVENT_ATTR(vdec_dcs_wr),
	MCC_EVENT_ATTR(venc_rd),
	MCC_EVENT_ATTR(venc_wr),
	MCC_EVENT_ATTR(venc_dcs_rd),
	MCC_EVENT_ATTR(venc_dcs_wr),
	MCC_EVENT_ATTR(display_rd),
	MCC_EVENT_ATTR(display_wr),
	MCC_EVENT_ATTR(display_dcs_rd),
	MCC_EVENT_ATTR(display_dcs_wr),
	MCC_EVENT_ATTR(ext_display_rd),
	MCC_EVENT_ATTR(ext_display_wr),
	MCC_EVENT_ATTR(ext_display_dcs_rd),
	MCC_EVENT_ATTR(ext_display_dcs_wr),
	MCC_EVENT_ATTR(codec_rd),
	MCC_EVENT_ATTR(codec_wr),
	MCC_EVENT_ATTR(codec_dcs_rd),
	MCC_EVENT_ATTR(codec_dcs_wr),
	MCC_EVENT_ATTR(ans_rd),
	MCC_EVENT_ATTR(ans_wr),
	MCC_EVENT_ATTR(ans_dcs_rd),
	MCC_EVENT_ATTR(ans_dcs_wr),
	MCC_EVENT_ATTR(isp_rd),
	MCC_EVENT_ATTR(isp_wr),
	MCC_EVENT_ATTR(isp_dcs_rd),
	MCC_EVENT_ATTR(isp_dcs_wr),
	MCC_EVENT_ATTR(jpeg_rd),
	MCC_EVENT_ATTR(jpeg_wr),
	MCC_EVENT_ATTR(jpeg_dcs_rd),
	MCC_EVENT_ATTR(jpeg_dcs_wr),
	MCC_EVENT_ATTR(msr_rd),
	MCC_EVENT_ATTR(msr_wr),
	MCC_EVENT_ATTR(msr_dcs_rd),
	MCC_EVENT_ATTR(msr_dcs_wr),
	MCC_EVENT_ATTR(pcie_rd),
	MCC_EVENT_ATTR(pcie_wr),
	MCC_EVENT_ATTR(pcie_dcs_rd),
	MCC_EVENT_ATTR(pcie_dcs_wr),
	MCC_EVENT_ATTR(atc0_rd),
	MCC_EVENT_ATTR(atc0_wr),
	MCC_EVENT_ATTR(atc0_dcs_rd),
	MCC_EVENT_ATTR(atc0_dcs_wr),
	MCC_EVENT_ATTR(atc1_rd),
	MCC_EVENT_ATTR(atc1_wr),
	MCC_EVENT_ATTR(atc1_dcs_rd),
	MCC_EVENT_ATTR(atc1_dcs_wr),
	MCC_EVENT_ATTR(atc2_rd),
	MCC_EVENT_ATTR(atc2_wr),
	MCC_EVENT_ATTR(atc2_dcs_rd),
	MCC_EVENT_ATTR(atc2_dcs_wr),
	MCC_EVENT_ATTR(atc3_rd),
	MCC_EVENT_ATTR(atc3_wr),
	MCC_EVENT_ATTR(atc3_dcs_rd),
	MCC_EVENT_ATTR(atc3_dcs_wr),
	NULL,
};

static const struct attribute_group apple_mcc_event_group = {
	.name = "events",
	.attrs = apple_mcc_event_attrs,
};

static const struct attribute_group *apple_mcc_attr_groups[] = {
	&apple_mcc_format_group,
	&apple_mcc_event_group,
	NULL,
};

static int apple_mcc_suspend(struct device *dev)
{
	struct apple_mcc_pmu *mcc = dev_get_drvdata(dev);
	unsigned long flags;
	unsigned int i;

	raw_spin_lock_irqsave(&mcc->lock, flags);
	for (i = 0; i < APPLE_MCC_MAX_COUNTERS; i++) {
		struct perf_event *event = mcc->events[i];

		if (event && !(event->hw.state & PERF_HES_STOPPED)) {
			apple_mcc_event_update(mcc, event);
			apple_mcc_program(mcc, event, false);
		}
	}
	raw_spin_unlock_irqrestore(&mcc->lock, flags);
	return 0;
}

static int apple_mcc_resume(struct device *dev)
{
	struct apple_mcc_pmu *mcc = dev_get_drvdata(dev);
	unsigned long flags;
	unsigned int i;

	raw_spin_lock_irqsave(&mcc->lock, flags);
	for (i = 0; i < APPLE_MCC_MAX_COUNTERS; i++) {
		struct perf_event *event = mcc->events[i];

		if (event && !(event->hw.state & PERF_HES_STOPPED)) {
			apple_mcc_program(mcc, event, true);
			local64_set(&event->hw.prev_count,
				    apple_mcc_raw_read(mcc, event));
		}
	}
	raw_spin_unlock_irqrestore(&mcc->lock, flags);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(apple_mcc_pm_ops, apple_mcc_suspend,
				apple_mcc_resume);

static int apple_mcc_probe(struct auxiliary_device *auxdev,
			   const struct auxiliary_device_id *id)
{
	struct apple_mcc_pmu_aux *aux;
	struct apple_mcc_pmu *mcc;
	const char *name;
	int ret;

	aux = container_of(auxdev, struct apple_mcc_pmu_aux, auxdev);
	mcc = devm_kzalloc(&auxdev->dev, sizeof(*mcc), GFP_KERNEL);
	if (!mcc)
		return -ENOMEM;

	mcc->aux = aux;
	raw_spin_lock_init(&mcc->lock);
	mcc->pmu = (struct pmu){
		.module = THIS_MODULE,
		.attr_groups = apple_mcc_attr_groups,
		.task_ctx_nr = perf_invalid_context,
		.event_init = apple_mcc_event_init,
		.add = apple_mcc_event_add,
		.del = apple_mcc_event_del,
		.start = apple_mcc_event_start,
		.stop = apple_mcc_event_stop,
		.read = apple_mcc_event_read,
	};
	dev_set_drvdata(&auxdev->dev, mcc);

	name = aux->type == APPLE_MCC_PMU_T8112 ? "apple_mcc_t8112" :
						  "apple_mcc_t6000";
	ret = perf_pmu_register(&mcc->pmu, name, -1);
	if (ret)
		return dev_err_probe(&auxdev->dev, ret,
				     "failed to register PMU\n");

	return 0;
}

static void apple_mcc_remove(struct auxiliary_device *auxdev)
{
	struct apple_mcc_pmu *mcc = dev_get_drvdata(&auxdev->dev);

	perf_pmu_unregister(&mcc->pmu);
}

static const struct auxiliary_device_id apple_mcc_ids[] = {
	{ .name = "apple_regview.mcc_pmu" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, apple_mcc_ids);

static struct auxiliary_driver apple_mcc_driver = {
	.name = "apple_mcc_pmu",
	.probe = apple_mcc_probe,
	.remove = apple_mcc_remove,
	.id_table = apple_mcc_ids,
	.driver.pm = pm_sleep_ptr(&apple_mcc_pm_ops),
};
module_auxiliary_driver(apple_mcc_driver);

MODULE_DESCRIPTION("Apple T6000/T8112 MCC DRAM bandwidth PMU");
MODULE_LICENSE("Dual MIT/GPL");
