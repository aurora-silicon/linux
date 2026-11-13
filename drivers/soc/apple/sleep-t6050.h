/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T6050 (M5 Pro) suspend to RAM: what macOS does in its sleep entry and its wake, in its order,
 * on the M5 Pro's PMGR banks, clusters, PMU and coprocessors. Included by sleep.c after its
 * mapping and common power helpers.
 */
#define SLEEP_REGION_WORD(r, off) (((r) << 20) | (off))
#define SLEEP_REGION(w) (((w) >> 20) & 0x1f)
#define SLEEP_REGION_OFFSET(w) ((w) & GENMASK(19, 0))
#define SLEEP_REPLAY_PS BIT(31)
#define SLEEP_REPLAY_INDEX(w) ((w) & ~SLEEP_REPLAY_PS)

enum {
	T6050_PS0, T6050_PS1, T6050_PS_AON, T6050_PS_GPX,
	T6050_PS_MEDIA, T6050_PS_GFX_AON, T6050_CLK, T6050_CLK_SOC,
	T6050_CLK1, T6050_AFR_CLK, T6050_MEDIA_CLK, T6050_PERF,
	T6050_MEDIA_PERF, T6050_TABLE0, T6050_TABLE1, T6050_MEDIA_TABLE,
	T6050_REGIONS,
};

/* Exact table encoding retained; data only, no old driver implementation. */
#define T6050_W(r, off) SLEEP_REGION_WORD(r, off)
#define T6050_WAKE_PS(i) (SLEEP_REPLAY_PS | (i))
#include "sleep-t6050-tables.h"
#undef T6050_W
#undef T6050_WAKE_PS

/*
 * What a failed sleep leaves behind. PMU scratch survives the reset; the kernel log doesn't. 0x8300-0x867a is inside info-scrpad and outside every named
 * region (fault_shadow 0x867b, socd 0x8b00). The mark is written last at each step.
 */
#define T6050_PMU_MARK		0x8300
#define T6050_PMU_SNAP		0x8310
/* 0x85a8-0x8632 reads 0 after a reset, whatever was written. The rest goes here. */
#define T6050_PMU_SNAP_HEAD	0x298
#define T6050_PMU_SNAP_TAIL	0x8880
#define T6050_MARK_BEGIN	0x10
/* Both before the timer: after the timer cpu0 sends only macOS's PMU commands. */
#define T6050_MARK_COMMIT	0x50	/* the commit window's writes are done */
#define T6050_MARK_SNAP		0x51	/* the snapshot is in; the timer is written next */

/* apple,sleep-darts' order, then dart-apcie0 by name */
enum { T6050_DART_SEP, T6050_DART_PMP0, T6050_DART_SIO, T6050_DART_DCP, T6050_DART_APCIE0 };

/*
 * Register sequences the boot loader supplies for the radio's link and the shared PCIe PHY,
 * as <kind block offset value> each, run in order. Bits 7:0 of kind: write the value, read,
 * set the value's bits, clear them, write back what is read, wait for the value's bits to be
 * set, wait for them to be clear, a 16-bit write. Bits 31:16: a wait's timeout in ms (0: 10
 * ms); bit 8: its timeout is not an error.
 */
enum {
	T6050_OP_WRITE, T6050_OP_READ, T6050_OP_SET, T6050_OP_CLEAR, T6050_OP_REWRITE,
	T6050_OP_WAIT_SET, T6050_OP_WAIT_CLEAR, T6050_OP_WRITE16, T6050_OP_KINDS,
};
#define T6050_OP_KIND		GENMASK(7, 0)
#define T6050_OP_OPTIONAL	BIT(8)
#define T6050_OP_TIMEOUT	GENMASK(31, 16)

/* The blocks: the radio host's config, port0 (0x24000 from it), phy0, rc, phy0's four port words
 * (0x10000 from it) and the PHY's common page below them, then the sleep's storage-phy and
 * storage-phy-ip.
 */
enum {
	T6050_RBLK_CONFIG, T6050_RBLK_PORT, T6050_RBLK_PHY, T6050_RBLK_RC, T6050_RBLK_LANES,
	T6050_RBLK_PHY_CMN, T6050_RBLK_SHARED, T6050_RBLK_IP, T6050_RBLK_COUNT,
};

struct t6050_ops {
	const u32 *op;
	unsigned int n;
};

struct apple_sleep_region_state {
	void __iomem *base[T6050_REGIONS];
	struct resource resource[T6050_REGIONS];
	u32 cluster_saved[5], complex_observed, complex_request;
	u32 rails[6], floor[5], floor_changed;
	u64 ptd_saved;
	u32 park_saved, gate_saved[2];
	u32 display_saved[SLEEP_MAX_DISPLAYS][3];
	unsigned long display_lowered;
	bool staged, committed, pmc_saved, ptd_owned, attempt_active, boot_stage_written;
	unsigned int late_errors;
	struct device_node *ptd_entry[4];
	u32 *saved, *woke, *replay;
	bool *done, *late;
	/* Connected to shared domain APIs; owners clear exclusions after a successful raise. */
	unsigned long *driver_down;
	bool snapshot_valid;
	unsigned long manager_down[BITS_TO_LONGS(ARRAY_SIZE(t6050_ps_all))];
	u8 alarm_compare[4];
	bool alarm_valid;
	unsigned int pmu_errors;
	/* The DART GAPF bank, read only, if the node names it. */
	void __iomem *gapf;
	/* Words lowered or capped after the last coprocessor stop, and those that timed out. */
	u32 late_lowered, state4_set, late_timeouts;
	/* Type-C ports, by port index, whose always-on USB word the last entry left on. */
	unsigned long usb_aon_kept;
	/* apple,sleep-darts, then the radio port's DART, mapped unclaimed */
	void __iomem *dart[5];
	u32 dart_done, dart_timeouts;
	/* The radio's port, from apple,radio-pcie, for its link shutdown and the wake's restart */
	void __iomem *n1_ecam, *n1_port, *n1_phy, *n1_rc;
	struct platform_device *radio_host, *radio_dart;
	u32 n1_perst, n1_held, n1_gapf;
	int n1_state, n1_up_ret;
	bool n1_host;
	/* The GFX die's interrupt controller word over the sleep, and GFX at the wake */
	u32 gfx_aic[3];
	u32 gfx_wake[2];
	u32 pmc154[2];
	u8 pmu_rd[16];
	int after_ans_ret[4];
	u16 disp_children_lowered[5];
	u32 mcc_cycle[2][10];
	void __iomem *spmi_bus[5];
	void __iomem *aon_clk[2];
	void __iomem *ap_gpio;
	u32 pins_low[4];
	unsigned int npins_low;
	void __iomem *storage_phy, *storage_phy_ip;
	bool storage_phy_parked;
	/* The radio's and the shared PHY's register sequences from the DT, and their blocks */
	void __iomem *rblk[T6050_RBLK_COUNT];
	u32 rblk_size[T6050_RBLK_COUNT];
	struct t6050_ops link_down, link_down_port, port_wake, dart_wake, phy_park, phy_down,
			 phy_wake;
	/* ANS's power word and its parents', as indices into the power words */
	unsigned int ans_chain[4], nans_chain;
	/* Clock generator page 0 (mux words), its +0x78000 on both dies, MCC reg[22..26] */
	void __iomem *clkgen, *clkgen78, *media_clkgen78, *mcc[5];
	void __iomem *cpm[3];
	/* From the DT: XNU's boot writes to them, and their re-arm after a round */
	u64 *cpm_boot, *cpm_rearm, *cpm_rearm_boot;
	unsigned int ncpm_boot, ncpm_rearm, ncpm_rearm_boot;
	/* The AP wake-enable words, on this die and the GFX die */
	void __iomem *wake, *wake_gfx, *aic_gfx;
	u32 wake_mask[11], wake_saved[11];
	bool wake_armed, wake_sources;
	u32 spmi_mask[6];
	bool spmi_masked;
	bool spmi_post_timer;
	bool cycle_ran;
};

static const struct apple_sleep_region_desc t6050_regions[] = {
	{ "ps0", 0x4000, true }, { "ps1", 0x4000, true },
	{ "ps-aon", 0xc000, true }, { "ps-gpx", 0x4000, true },
	{ "ps-media", 0xc000, true }, { "ps-gfx-aon", 0x4000, true },
	{ "clk", 0x10000 }, { "clk-soc", 0x4000 },
	{ "clk1", 0x4000 }, { "afr-clk", 0x4000 },
	{ "media-clk", 0x10000 }, { "perf", 0x20000 },
	{ "media-perf", 0x14000 }, { "table0", 0x4000 },
	{ "table1", 0x4000 }, { "media-table", 0x4000 },
};

static const struct apple_sleep_region_table t6050_tables[] = {
	{ T6050_TABLE0, 0x44, 27 }, { T6050_TABLE1, 0x24, 25 },
	{ T6050_MEDIA_TABLE, 0x44, 14 },
};

static const struct apple_sleep_region_profile t6050_region_profile = {
	.region = t6050_regions,
	.count = ARRAY_SIZE(t6050_regions),
	.power = SLEEP_WORDS(t6050_ps_all),
	.replay = SLEEP_WORDS(t6050_wake_order),
	.entry_clk = SLEEP_WORDS(t6050_entry_clk),
	.entry_perf = SLEEP_WORDS(t6050_entry_perf),
	.table = t6050_tables,
	.ntables = ARRAY_SIZE(t6050_tables),
};

static bool sleep_region_word_valid(const struct apple_sleep_region_profile *p,
				    u32 w, bool power)
{
	unsigned int r = SLEEP_REGION(w);
	u32 off = SLEEP_REGION_OFFSET(w);

	return !(w & ~GENMASK(24, 0)) && r < p->count && !(off & 3) &&
	       p->region[r].size >= sizeof(u32) &&
	       off <= p->region[r].size - sizeof(u32) &&
	       p->region[r].power == power;
}

/* All indices/apertures checked before mapping or installing any power ops. */
static int sleep_region_validate(const struct apple_sleep_region_profile *p)
{
	unsigned int i, j;

	if (!p || !p->count || p->count > T6050_REGIONS ||
	    !p->power.count || !p->replay.count)
		return -EINVAL;
	for (i = 0; i < p->count; i++)
		if (!p->region[i].name || p->region[i].size < sizeof(u32))
			return -EINVAL;
	for (i = 0; i < p->power.count; i++) {
		if (!sleep_region_word_valid(p, p->power.offset[i], true))
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (p->power.offset[j] == p->power.offset[i])
				return -EINVAL;
	}
	for (i = 0; i < p->replay.count; i++) {
		u32 w = p->replay.offset[i];

		if (w & SLEEP_REPLAY_PS) {
			if (SLEEP_REPLAY_INDEX(w) >= p->power.count)
				return -EINVAL;
		} else if (!sleep_region_word_valid(p, w, false)) {
			return -EINVAL;
		}
	}
	for (i = 0; i < p->entry_clk.count; i++)
		if (!sleep_region_word_valid(p, p->entry_clk.offset[i], false))
			return -EINVAL;
	for (i = 0; i < p->entry_perf.count; i++)
		if (!sleep_region_word_valid(p, p->entry_perf.offset[i], false))
			return -EINVAL;
	for (i = 0; i < p->ntables; i++) {
		const struct apple_sleep_region_table *t = &p->table[i];

		if (t->region >= p->count || p->region[t->region].power ||
		    !t->count || t->stride < 4 || t->stride % 4 ||
		    (u64)(t->count - 1) * t->stride + 4 > p->region[t->region].size)
			return -EINVAL;
	}
	return 0;
}

static void __iomem *sleep_region_reg(struct apple_sleep *s, u32 w)
{
	return s->region->base[SLEEP_REGION(w)] + SLEEP_REGION_OFFSET(w);
}

static int sleep_region_map(struct apple_sleep *s)
{
	const struct apple_sleep_region_profile *p = s->soc->regions;
	struct apple_sleep_region_state *r;
	unsigned int i;
	int ret = sleep_region_validate(p);

	if (ret)
		return dev_err_probe(s->dev, ret, "invalid region/replay data\n");
	r = devm_kzalloc(s->dev, sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	for (i = 0; i < p->count; i++) {
		struct resource res;
		int idx = of_property_match_string(s->dev->of_node, "reg-names", p->region[i].name);

		if (idx < 0 || of_address_to_resource(s->dev->of_node, idx, &res) ||
		    resource_size(&res) != p->region[i].size)
			return dev_err_probe(s->dev, -EINVAL, "wrong %s aperture\n", p->region[i].name);
		r->resource[i] = res;
		r->base[i] = apple_sleep_map(s->dev, p->region[i].name);
		if (IS_ERR(r->base[i]))
			return PTR_ERR(r->base[i]);
	}
	r->saved = devm_kcalloc(s->dev, p->power.count, sizeof(u32), GFP_KERNEL);
	r->woke = devm_kcalloc(s->dev, p->power.count, sizeof(u32), GFP_KERNEL);
	r->done = devm_kcalloc(s->dev, p->power.count, sizeof(bool), GFP_KERNEL);
	r->late = devm_kcalloc(s->dev, p->power.count, sizeof(bool), GFP_KERNEL);
	r->driver_down = devm_bitmap_zalloc(s->dev, p->power.count, GFP_KERNEL);
	r->replay = devm_kcalloc(s->dev, p->replay.count, sizeof(u32), GFP_KERNEL);
	if (!r->saved || !r->woke || !r->done || !r->late || !r->replay || !r->driver_down)
		return -ENOMEM;
	s->region = r;
	return 0;
}

static void sleep_region_snapshot(struct apple_sleep *s)
{
	const struct apple_sleep_region_profile *p = s->soc->regions;
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;

	for (i = 0; i < p->power.count; i++)
		r->saved[i] = readl(sleep_region_reg(s, p->power.offset[i]));
	for (i = 0; i < p->replay.count; i++)
		if (!(p->replay.offset[i] & SLEEP_REPLAY_PS))
			r->replay[i] = readl(sleep_region_reg(s, p->replay.offset[i]));
	r->snapshot_valid = true;
}

/* The storage link's power words the sleep lowers after ANS's stop, as macOS. */
static const u32 t6050_ans_storage_ps[] = {
	0x268, /* APHY_ST0_AUS5 */
	0x240, /* APHY_ST0_AUS5_A */
	0x270, /* APHY_ST1_AUS5 */
	0x248, /* APHY_ST1_AUS5_A */
	/* macOS lowers these three as well, and leaves ANS and APCIE_SYS_ST0/1 on auto. */
	0x280, /* APHY_GP_AUS5 */
	0x258, /* APHY_GP_AUS5_A */
	0x1b0, /* APCIE_PHY_SW */
};
static const u8 t6050_ans_storage_on[] = { 6, 5, 4, 1, 3, 0, 2 };

static bool t6050_ans_storage_word(u32 word)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(t6050_ans_storage_ps); i++)
		if (word == SLEEP_REGION_WORD(T6050_PS1, t6050_ans_storage_ps[i]))
			return true;
	return false;
}

static int t6050_ans_storage_index(struct apple_sleep *s, u32 word)
{
	unsigned int i;

	for (i = 0; i < s->soc->regions->power.count; i++)
		if (s->soc->regions->power.offset[i] == word)
			return i;
	return -ENOENT;
}

/* The wake's order interleaves power and clock, perf and table words: never split by bank. */
static bool t6050_v27_retained_display_word(struct apple_sleep *s, u32 word)
{
	unsigned int d, j;

	if (!apple_sleep_v27_late())
		return false;
	for (d = 1; d < s->soc->display_count; d++)
		for (j = 1; j < 3; j++)
			if (word == s->soc->ps.disp[d][j])
				return true;
	return false;
}


static void sleep_region_replay(struct apple_sleep *s)
{
	const struct apple_sleep_region_profile *p = s->soc->regions;
	struct apple_sleep_region_state *r = s->region;
	unsigned int i, pass;

	if (!r->snapshot_valid)
		return;
	memset(r->done, 0, p->power.count * sizeof(*r->done));
	memset(r->late, 0, p->power.count * sizeof(*r->late));
	s->ps_rewoken = 0;
	s->ps_rewake_timeouts = 0;
	for (i = 0; i < p->power.count; i++)
		r->woke[i] = readl(sleep_region_reg(s, p->power.offset[i]));
	/*
	 * GFX comes back on from the sleep, and macOS writes it off at once:
	 * it stays off until the GPU's start raises it, so the firmware starts from a power cycle.
	 */
	r->gfx_wake[0] = r->gfx_wake[1] = 0;
	if (r->committed) {
		void __iomem *gfx = r->base[T6050_PS_GPX] + 0x110;
		u32 v;

		r->gfx_wake[0] = readl(gfx);
		writel(0x0f0003f0, gfx);
		readl_poll_timeout_atomic(gfx, v, !FIELD_GET(PS_ACTUAL, v), 1, PS_OFF_TIMEOUT_US);
		r->gfx_wake[1] = v;
	}
	for (i = 0; i < p->replay.count; i++) {
		u32 w = p->replay.offset[i];
		unsigned int n;

		if (!(w & SLEEP_REPLAY_PS)) {
			writel(r->replay[i], sleep_region_reg(s, w));
			continue;
		}
		n = SLEEP_REPLAY_INDEX(w);
		r->done[n] = true;
		if (r->committed &&
		    p->power.offset[n] == SLEEP_REGION_WORD(T6050_PS_GPX, 0x110))
			continue;
		if (t6050_v27_retained_display_word(s, p->power.offset[n])) {
			u32 value;

			/* Lowered by the display stop: its restore raises it, after this. */
			if (test_bit(n, r->manager_down))
				continue;
			value = readl(sleep_region_reg(s, p->power.offset[n]));

			if ((value & (PS_TARGET | PS_ACTUAL)) != (PS_TARGET | PS_ACTUAL) ||
			    (value & PS_DEV_DISABLE))
				r->late_errors++;
			continue; /* no write through an unknown display protection slice */
		}
		if (s->pcie_ans_stopped && t6050_ans_storage_word(p->power.offset[n]))
			continue;
		if ((test_bit(n, r->driver_down) || test_bit(n, r->manager_down)) || !FIELD_GET(PS_TARGET, r->saved[n]))
			continue;
		s->ps_rewoken++;
		r->late[n] = !!sleep_ps_restore(sleep_region_reg(s, p->power.offset[n]), r->saved[n]);
	}
	for (pass = 0; pass < PS_REWAKE_PASSES; pass++)
		for (i = 0; i < p->power.count; i++) {
			if (t6050_v27_retained_display_word(s, p->power.offset[i]))
				continue;
			if (s->pcie_ans_stopped && t6050_ans_storage_word(p->power.offset[i]))
				continue;
			if (r->committed &&
			    p->power.offset[i] == SLEEP_REGION_WORD(T6050_PS_GPX, 0x110))
				continue;
			if ((test_bit(i, r->driver_down) || test_bit(i, r->manager_down)) || (!r->late[i] &&
			    (r->done[i] || !sleep_ps_dropped(r->saved[i], r->woke[i]))))
				continue;
			if (!r->done[i])
				s->ps_rewoken++;
			r->done[i] = true;
			r->late[i] = !!sleep_ps_restore(sleep_region_reg(s, p->power.offset[i]), r->saved[i]);
		}
	for (i = 0; i < p->power.count; i++)
		s->ps_rewake_timeouts += r->late[i];
	r->snapshot_valid = false;
}

/* SPMI extended register commands, sent to the PMU by hand in the sleep's last steps */
#define T6050_SPMI_EXT_WRITEL	0x30
#define T6050_SPMI_EXT_READL	0x38
#define M5_SPMI_STATUS 0x200
#define M5_SPMI_CMD 0x210
#define M5_SPMI_REPLY 0x220
#define M5_SPMI_RX_EMPTY BIT(30)
#define M5_SPMI_IRQ_SRC 0x400
#define M5_SPMI_IRQ_SRC1 0x404
#define M5_SPMI_IRQ_ACK1 0x604
#define M5_SPMI_IRQ_EN 0x420
#define M5_SPMI_IRQ_STATUS 0x620
#define M5_SPMI_IRQ_SLEEP 0x2000001
/* nub-spmi0's mask through each runtime PMU command, XNU's boot value */
#define M5_SPMI_IRQ_RUNTIME 0x3f833ff1
#define M5_PMU_SID 0x0e


/*
 * A PMU command on nub-spmi0 with XNU's accesses and nothing else: status, the interrupt mask
 * (0x2000000 after the timer, the sleep's or the runtime one before it), the command and its
 * data, the done bit polled and acked, then status and one reply word at a time, and status.
 */
static int t6050_sleep_pmu_xfer_post(struct apple_sleep *s, u32 mask, u8 command, u16 address,
				     const u8 *wr, u8 *rd, unsigned int len)
{
	u32 value, data;
	unsigned int i, n;
	int ret = 0;

	readl(s->spmi + M5_SPMI_STATUS);
	writel(mask, s->spmi + M5_SPMI_IRQ_EN);
	writel(command | M5_PMU_SID << 8 | (u32)address << 16 | BIT(15), s->spmi + M5_SPMI_CMD);
	for (i = 0; wr && i < len; i += 4) {
		data = 0;
		for (n = 0; n < min(4U, len - i); n++)
			data |= (u32)wr[i + n] << (8 * n);
		writel(data, s->spmi + M5_SPMI_CMD);
	}
	/* The done bit latches under the sleep's mask; if it doesn't, the reply still counts. */
	if (!readl_poll_timeout_atomic(s->spmi + M5_SPMI_IRQ_STATUS, value, value & BIT(0), 1, 1000))
		writel(value, s->spmi + M5_SPMI_IRQ_STATUS);
	else if (readl_poll_timeout_atomic(s->spmi + M5_SPMI_STATUS, value,
					   !(value & M5_SPMI_RX_EMPTY), 1, 1000))
		return -ETIMEDOUT;
	readl(s->spmi + M5_SPMI_STATUS);
	value = readl(s->spmi + M5_SPMI_REPLY);
	if ((value & 0xff) != command || ((value >> 8) & 0x7f) != M5_PMU_SID ||
	    (wr && !(value & BIT(15))))
		ret = -EIO;
	for (i = 0; !ret && rd && i < len; i += 4) {
		readl(s->spmi + M5_SPMI_STATUS);
		data = readl(s->spmi + M5_SPMI_REPLY);
		for (n = 0; n < min(4U, len - i); n++)
			rd[i + n] = data >> (8 * n);
	}
	readl(s->spmi + M5_SPMI_STATUS);
	return ret;
}

/*
 * Polled, length and reply checked. From the SPMI masks to the timer nothing but XNU's own
 * commands goes out, as on macOS: no PMU scratch write.
 */
static int t6050_sleep_pmu_xfer(struct apple_sleep *s, u8 opcode, u16 address,
				const u8 *wr, u8 *rd, unsigned int len)
{
	unsigned int i;
	u8 command;

	if (!len || len > 8 || (!!wr == !!rd))
		return -EINVAL;
	if (wr && s->region->spmi_masked && !s->region->spmi_post_timer &&
	    address >= 0x8000 && address < 0x9000)
		return 0;
	command = opcode | (len - 1);
	if (s->region->spmi_post_timer)
		return t6050_sleep_pmu_xfer_post(s, 0x2000000, command, address, wr, rd, len);
	/* XNU reads every reply, so its FIFO is empty here: a stale entry is read first. */
	for (i = 0; i < 8 && !(readl(s->spmi + M5_SPMI_STATUS) & M5_SPMI_RX_EMPTY); i++)
		readl(s->spmi + M5_SPMI_REPLY);
	return t6050_sleep_pmu_xfer_post(s, s->region->spmi_masked ? M5_SPMI_IRQ_SLEEP :
					 M5_SPMI_IRQ_RUNTIME, command, address, wr, rd, len);
}

/* Before the shared last-stop/enclave boundary: failure must leave ANS running. */
static int t6050_sleep_check_alarm(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	u8 control, mask;
	int ret;

	r->alarm_valid = false;
	ret = t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_READL, 0x2000, NULL, &control, 1);
	if (!ret)
		ret = t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_READL, 0x200e, NULL, &mask, 1);
	if (!ret && (!(control & BIT(6)) || (mask & BIT(0))))
		ret = -EBUSY;
	if (!ret)
		ret = t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_READL, 0x2008, NULL, r->alarm_compare, 4);
	r->alarm_valid = !ret;
	return ret;
}

#define M5_PTD_READ (16 * 0x1e0)
#define M5_PTD_ACK (16 * 0x1e8)
#define M5_PTD_WRITE (0x10000 + 8 * 0x1e0)

/*
 * The storage link's PHY is parked before its power words go down, in the commit window. Its
 * registers fault with its domains off: nothing is touched unless they are on.
 */
#define T6050_PS1_APHY_ST0_AUS5	0x268
#define T6050_PS1_APCIE_PHY_SW	0x1b0

static bool t6050_storage_phy_up(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;

	return r->storage_phy && r->storage_phy_ip &&
	       sleep_ps_on(s, SLEEP_REGION_WORD(T6050_PS1, T6050_PS1_APCIE_PHY_SW)) &&
	       sleep_ps_on(s, SLEEP_REGION_WORD(T6050_PS1, T6050_PS1_APHY_ST0_AUS5));
}

static unsigned int t6050_ops_run(struct apple_sleep *s, const struct t6050_ops *ops);

/* The DT's apple,phy-park */
static void t6050_storage_phy_park(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;

	r->storage_phy_parked = r->phy_park.n && t6050_storage_phy_up(s);
	if (r->storage_phy_parked)
		t6050_ops_run(s, &r->phy_park);
}

/* After APHY_GP_AUS5_A, before APCIE_PHY_SW: the DT's apple,phy-down */
static void t6050_storage_phy_down(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;

	if (r->storage_phy_parked)
		t6050_ops_run(s, &r->phy_down);
}




static void t6050_sleep_post_timer(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	u8 control, value;
	int ret;

	/* The boot core's PMU commands after the timer, as macOS sends them: the alarm rearmed. */
	r->spmi_post_timer = r->spmi_masked;
	r->pmu_errors = 0;
	if (!r->alarm_valid) {
		r->pmu_errors++;
		return;
	}
	ret = t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_READL, 0x2000, NULL, &control, 1);
	if (ret)
		goto failed; /* No fabricated status or conditional ack after a failed read. */
	if (control & BIT(1)) {
		value = 0x43;
		ret = t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_WRITEL, 0x2000, &value, NULL, 1);
		if (ret)
			goto failed;
	}
	ret = t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_WRITEL, 0x2008, r->alarm_compare, NULL, 4);
	if (ret)
		goto failed;
	value = 0;
	ret = t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_WRITEL, 0x200e, &value, NULL, 1);
	if (ret)
		goto failed;
	value = 0x41;
	ret = t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_WRITEL, 0x2000, &value, NULL, 1);
failed:
	r->pmu_errors += !!ret;
}

static bool t6050_sleep_cpu_valid(u64 mpidr)
{
	unsigned int c = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	unsigned int type = MPIDR_AFFINITY_LEVEL(mpidr, 2);

	if ((mpidr & ~MPIDR_HWID_BITMASK) || MPIDR_AFFINITY_LEVEL(mpidr, 3) ||
	    MPIDR_AFFINITY_LEVEL(mpidr, 0) >= 6)
		return false;
	return (c < 2 && type == 4) || (c == 2 && type == 1);
}

/*
 * The complex request the commit writes for the state it finds: 0xf releases to 0, 0x2f to 0x20,
 * as macOS does. Any other state refuses the sleep before anything irreversible.
 */
static int t6050_sleep_plan_complex_request(u32 observed, u32 *request)
{
	if (!request)
		return -EINVAL;
	switch (observed) {
	case 0xf:
		*request = 0;
		return 0;
	case 0x2f:
		*request = 0x20;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* The coprocessors a sleep needs bound and, where it stops them, stopped. */
#define M5_GATE(c, base, idx, stop, word, loaded) \
	{ .compatible = (c), .match_start = (base), .run_index = (idx), \
	  .explicit_run_index = true, .must_stop = (stop), .ps = (word), \
	  .allow_unstarted = (loaded), .strict_unbound = true }
static const struct apple_sleep_gate_coproc t6050_gate_coprocs[] = {
	{ .compatible = "apple,t6050-smc", .strict_unbound = true },
	M5_GATE("apple,t6050-mtp", 0, 0, false, 0, true),
	M5_GATE("apple,t6050-aop", 0, 1, false, 0, true),
	M5_GATE("apple,t6050-aop2", 0, 0, false, 0, true),
	M5_GATE("apple,t6050-pmp-v2", 0, 1, true, SLEEP_REGION_WORD(T6050_PS0, 0x128), true),
	M5_GATE("apple,t6050-sio", 0, 0, true, SLEEP_REGION_WORD(T6050_PS1, 0x2a0), true),
	{ .compatible = "apple,t6050-sep", .ps = SLEEP_REGION_WORD(T6050_PS1, 0x298),
	  .strict_unbound = true },
	{ .compatible = "apple,t6050-nvme-ans2", .strict_unbound = true },
	M5_GATE("apple,t6050-dcp", 0, 0, true, SLEEP_REGION_WORD(T6050_PS1, 0x1a8), false),
	M5_GATE("apple,t6050-dcpext", 0x2b2e00000ULL, 0, true, SLEEP_REGION_WORD(T6050_PS0, 0x278), false),
	M5_GATE("apple,t6050-dcpext", 0x392e00000ULL, 0, true, SLEEP_REGION_WORD(T6050_PS0, 0x2e0), false),
	M5_GATE("apple,t6050-dcpext", 0x302e00000ULL, 0, true, SLEEP_REGION_WORD(T6050_PS0, 0x2b0), false),
	M5_GATE("apple,t6050-dcpext", 0x306e00000ULL, 0, true, SLEEP_REGION_WORD(T6050_PS0, 0x2c8), false),
	/* The GPU driver owns its readiness for the sleep. */
	{ .compatible = "apple,agx-g17s", .strict_unbound = true },
};
#undef M5_GATE
static const struct apple_sleep_word t6050_gate_off[] = {
	{ "ACIO0", SLEEP_REGION_WORD(T6050_PS0, 0x280) },
	{ "ACIO1", SLEEP_REGION_WORD(T6050_PS0, 0x290) },
	{ "ACIO2", SLEEP_REGION_WORD(T6050_PS0, 0x2e8) },
};

/* The boot core's power-down control, set for the sleep's own cpu_suspend() and put back after */
#define M5_C1_6 sys_reg(3, 5, 15, 1, 6)
#define M5_C1_6_BITS GENMASK_ULL(28, 26)

struct t6050_cpu_control {
	u64 saved;
	bool changed;
};
static DEFINE_PER_CPU(struct t6050_cpu_control, t6050_cpu_control);

static int t6050_sleep_cpu_prepare(u64 mpidr)
{
	struct t6050_cpu_control *c = this_cpu_ptr(&t6050_cpu_control);
	u64 value;

	/* Only for a kernel running at EL2 with VHE */
	if (!is_kernel_in_hyp_mode() ||
	    (read_sysreg(hcr_el2) & (HCR_E2H | HCR_TGE)) != (HCR_E2H | HCR_TGE))
		return -EOPNOTSUPP;
	if (!c->changed) {
		c->saved = read_sysreg_s(M5_C1_6);
		c->changed = true;
	}
	write_sysreg_s(c->saved | M5_C1_6_BITS, M5_C1_6);
	isb();
	value = read_sysreg_s(M5_C1_6);
	return (value & M5_C1_6_BITS) == M5_C1_6_BITS ? 0 : -EIO;
}

static void t6050_sleep_cpu_restore(u64 mpidr)
{
	struct t6050_cpu_control *c = this_cpu_ptr(&t6050_cpu_control);

	if (!c->changed)
		return;
	write_sysreg_s(c->saved, M5_C1_6);
	isb();
	c->changed = false;
}

/* Resolve only the two actual M5 shapes: a PMGR word or one PMP report
 * entry whose sole dependency is that PMGR word. A report's reg is an ID,
 * never a byte offset. This does not acquire/withdraw/restore the PMP vote.
 */
static struct device_node *sleep_region_pmgr_node(struct device_node *pd)
{
	struct device_node *parent __free(device_node) = NULL;
	struct of_phandle_args args;
	u32 cells;
	int ret;

	if (of_property_read_u32(pd, "#power-domain-cells", &cells) || cells)
		return ERR_PTR(-EINVAL);
	if (of_device_is_compatible(pd, "apple,t6050-pmgr-pwrstate"))
		return of_node_get(pd);
	if (!of_device_is_compatible(pd, "apple,t6050-pmp-v2-report-entry"))
		return ERR_PTR(-EINVAL);
	parent = of_get_parent(pd);
	if (!parent || !of_device_is_compatible(parent, "apple,t6050-pmp-v2-report") ||
	    of_count_phandle_with_args(pd, "power-domains", "#power-domain-cells") != 1)
		return ERR_PTR(-EINVAL);
	ret = of_parse_phandle_with_args(pd, "power-domains", "#power-domain-cells", 0, &args);
	if (ret)
		return ERR_PTR(ret);
	if (args.args_count || !of_device_is_compatible(args.np, "apple,t6050-pmgr-pwrstate") ||
	    of_property_read_u32(args.np, "#power-domain-cells", &cells) || cells) {
		of_node_put(args.np);
		return ERR_PTR(-EINVAL);
	}
	return args.np;
}

/* The selected profile owns register encoding; exported driver APIs stay shared. */
static int sleep_region_domain_offset(struct apple_sleep *s, struct device_node *pd)
{
	struct device_node *word __free(device_node) = NULL;
	struct device_node *resolved = sleep_region_pmgr_node(pd);
	struct device_node *parent __free(device_node) = NULL;
	const struct apple_sleep_region_profile *p = s->soc->regions;
	struct resource res;
	u32 reg[2], off, encoded;
	unsigned int i;

	if (IS_ERR(resolved))
		return PTR_ERR(resolved);
	word = resolved;
	parent = of_get_parent(word);
	if (!parent || of_address_to_resource(parent, 0, &res) ||
	    of_property_count_u32_elems(word, "reg") != ARRAY_SIZE(reg) ||
	    of_property_read_u32_array(word, "reg", reg, ARRAY_SIZE(reg)))
		return -EINVAL;
	off = reg[0];
	if (reg[1] != sizeof(u32) || (off & 3) || resource_size(&res) < sizeof(u32) ||
	    off > resource_size(&res) - sizeof(u32))
		return -EINVAL;
	for (i = 0; i < p->count; i++) {
		if (!p->region[i].power || res.start != s->region->resource[i].start)
			continue;
		if (p->region[i].size < sizeof(u32) || off > p->region[i].size - sizeof(u32))
			return -EINVAL;
		encoded = SLEEP_REGION_WORD(i, off);
		if (!sleep_region_word_valid(p, encoded, true) ||
		    sleep_ps_find(p->power.offset, p->power.count, encoded) < 0)
			return -EINVAL;
		return encoded;
	}
	return -EINVAL;
}

static unsigned long *sleep_region_domain_bitmap(struct apple_sleep *s, u32 word,
						unsigned int *bit)
{
	const struct apple_sleep_words *p = &s->soc->regions->power;
	int index = sleep_ps_find(p->offset, p->count, word);

	if (index < 0)
		return NULL;
	*bit = index;
	return s->region->driver_down;
}

/* Shared mapping does not claim apertures also used by CIO/reset/PMGR owners. */
/*
 * The fabric and memory words XNU puts on auto once at boot, 0x0f0000ff then 0x1f0000ff, in
 * this order, and never writes again.
 * iBoot leaves them on with AUTO_ENABLE clear, so none can gate. The old driver's table.
 */
static const u32 t6050_boot_auto[] = {
	/*
	 * On auto in macOS, manual here. XNU writes ANS's pair;
	 * APCIE_SYS_GP has no XNU write, so it is iBoot's auto that Linux's power domain cleared.
	 */
	SLEEP_REGION_WORD(T6050_PS1, 0x140), SLEEP_REGION_WORD(T6050_PS1, 0x148),
	/* On auto in macOS, manual here */
	SLEEP_REGION_WORD(T6050_PS0, 0x110), SLEEP_REGION_WORD(T6050_PS0, 0x168),
	SLEEP_REGION_WORD(T6050_PS0, 0x178), SLEEP_REGION_WORD(T6050_PS0, 0x450),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x198), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x130),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x140), SLEEP_REGION_WORD(T6050_PS_AON, 0x60),
	SLEEP_REGION_WORD(T6050_PS_AON, 0x68), SLEEP_REGION_WORD(T6050_PS_AON, 0x128),
	SLEEP_REGION_WORD(T6050_PS_AON, 0x140), SLEEP_REGION_WORD(T6050_PS_AON, 0x58),
	SLEEP_REGION_WORD(T6050_PS_GFX_AON, 0x58),
	SLEEP_REGION_WORD(T6050_PS0, 0x148), SLEEP_REGION_WORD(T6050_PS0, 0x150),
	SLEEP_REGION_WORD(T6050_PS0, 0x158), SLEEP_REGION_WORD(T6050_PS0, 0x160),
	SLEEP_REGION_WORD(T6050_PS0, 0x1e8), SLEEP_REGION_WORD(T6050_PS0, 0x1f0),
	SLEEP_REGION_WORD(T6050_PS1, 0x138), SLEEP_REGION_WORD(T6050_PS1, 0x178),
	SLEEP_REGION_WORD(T6050_PS1, 0x288), SLEEP_REGION_WORD(T6050_PS1, 0x398),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x1a8), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x1b0),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x218), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x220),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x228), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x230),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x238), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x240),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x288), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x298),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x2a0), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x2a8),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x2b0), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x2b8),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x2c0), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x308),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x318), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x320),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x328), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x330),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x338), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x340),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x388), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x398),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x3a0), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x3a8),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x3b0),
	/* Under auto power in macOS through its sleep; iBoot leaves them manual. */
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x3b8), SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x3c0),
	SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x408),
};




/*
 * The one-time clock-gating set-up of the PMGR's clock pages, where the boot loader puts it
 * (apple,clk-gating-boot: reg index, offset, value). Without it the pages keep iBoot's values.
 */
static void t6050_boot_clk_gating(struct apple_sleep *s)
{
	struct device_node *np = s->dev->of_node;
	int n = of_property_count_u32_elems(np, "apple,clk-gating-boot");
	void __iomem *base[48] = {};
	u32 size[48] = {};
	unsigned int i, bad = 0;
	u32 *w;

	if (n <= 0)
		return;
	if (n % 3) {
		dev_warn(s->dev, "apple,clk-gating-boot is not in threes\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(base); i++) {
		const char *name;
		struct resource res;

		if (of_property_read_string_index(np, "reg-names", i, &name) ||
		    of_address_to_resource(np, i, &res))
			break;
		/* Only the clock pages and tables: the sleep's mapped regions and the AON pages */
		if (strstr(name, "clk") || strstr(name, "table")) {
			base[i] = of_iomap(np, i);
			size[i] = resource_size(&res);
		}
	}
	w = kcalloc(n, sizeof(*w), GFP_KERNEL);
	if (w && !of_property_read_u32_array(np, "apple,clk-gating-boot", w, n)) {
		for (i = 0; i < n; i += 3) {
			if (w[i] >= ARRAY_SIZE(base) || !base[w[i]] || w[i + 1] % 4 ||
			    w[i + 1] + 4 > size[w[i]]) {
				bad++;
				continue;
			}
			writel(w[i + 2], base[w[i]] + w[i + 1]);
		}
	}
	kfree(w);
	for (i = 0; i < ARRAY_SIZE(base); i++)
		if (base[i])
			iounmap(base[i]);
	if (bad)
		dev_warn(s->dev, "%u apple,clk-gating-boot writes outside the clock pages\n", bad);
}

static int t6050_cpm_prop(struct apple_sleep *s, const char *name, u64 **ops, unsigned int *n);

static void t6050_sleep_boot_auto(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;

	/* The clock-gating set-up comes first, before the fabric words go on auto. */
	t6050_boot_clk_gating(s);
	for (i = 0; i < r->ncpm_boot; i++) {
		const u64 *w = &r->cpm_boot[3 * i];

		if (w[0] < ARRAY_SIZE(r->cpm) && r->cpm[w[0]])
			writeq(w[2], r->cpm[w[0]] + w[1]);
	}
	for (i = 0; i < ARRAY_SIZE(t6050_boot_auto); i++) {
		void __iomem *reg = sleep_region_reg(s, t6050_boot_auto[i]);

		writel(0x0f0000ff, reg);
		writel(0x1f0000ff, reg);
	}
	/* The two XNU turns on from off: SPI_P and ATC3_USB_AON. */
	writel(0xff, sleep_region_reg(s, SLEEP_REGION_WORD(T6050_PS1, 0x308)));
	writel(0x100000ff, sleep_region_reg(s, SLEEP_REGION_WORD(T6050_PS1, 0x308)));
	writel(0xff, sleep_region_reg(s, SLEEP_REGION_WORD(T6050_PS_AON, 0x190)));
	writel(0x100000ff, sleep_region_reg(s, SLEEP_REGION_WORD(T6050_PS_AON, 0x190)));
	/* Each SPMI bus's interrupt mask as XNU leaves it at boot, aop-spmi0 last with its +0x41c. */
	for (i = 0; i < ARRAY_SIZE(r->spmi_bus) + 1; i++) {
		void __iomem *bus = i ? r->spmi_bus[i - 1] : s->spmi;

		if (!bus)
			continue;
		writel(M5_SPMI_IRQ_RUNTIME, bus + M5_SPMI_IRQ_EN);
		if (i == ARRAY_SIZE(r->spmi_bus)) {
			writel(0x20, bus + 0x41c);
			writel(0x28, bus + 0x41c);
		}
	}
	/* nub-spmi0's event sources, as XNU sets them at boot */
	writel(0x40000000, s->spmi + M5_SPMI_IRQ_SRC1);
	writel(0x40000000, s->spmi + M5_SPMI_IRQ_ACK1);
	writel(4, s->spmi + M5_SPMI_IRQ_SRC);
}

static int t6050_ops_get(struct apple_sleep *s, const char *name, struct t6050_ops *ops)
{
	struct apple_sleep_region_state *r = s->region;
	int len = of_property_count_u32_elems(s->dev->of_node, name);
	unsigned int i;
	u32 *op;

	ops->n = 0;
	if (len <= 0)
		return 0;
	if (len % 4)
		return dev_err_probe(s->dev, -EINVAL, "%s: not in fours\n", name);
	op = devm_kcalloc(s->dev, len, sizeof(*op), GFP_KERNEL);
	if (!op)
		return -ENOMEM;
	if (of_property_read_u32_array(s->dev->of_node, name, op, len))
		return -EINVAL;
	for (i = 0; i < len; i += 4)
		if (FIELD_GET(T6050_OP_KIND, op[i]) >= T6050_OP_KINDS ||
		    op[i + 1] >= T6050_RBLK_COUNT || !r->rblk[op[i + 1]] || op[i + 2] % 2 ||
		    op[i + 2] + 4 > r->rblk_size[op[i + 1]])
			return dev_err_probe(s->dev, -EINVAL, "%s: entry %u outside its block\n",
					     name, i / 4);
	ops->op = op;
	ops->n = len / 4;
	return 0;
}

/* The number of waits that timed out, other than the optional ones */
static unsigned int t6050_ops_run(struct apple_sleep *s, const struct t6050_ops *ops)
{
	unsigned int i, timeouts = 0;
	u32 v;

	for (i = 0; i < ops->n; i++) {
		const u32 *op = &ops->op[4 * i];
		void __iomem *reg = s->region->rblk[op[1]] + op[2];
		u32 ms = FIELD_GET(T6050_OP_TIMEOUT, op[0]) ?: 10;
		int ret = 0;

		switch (FIELD_GET(T6050_OP_KIND, op[0])) {
		case T6050_OP_WRITE:
			writel(op[3], reg);
			break;
		case T6050_OP_READ:
			readl(reg);
			break;
		case T6050_OP_SET:
			writel(readl(reg) | op[3], reg);
			break;
		case T6050_OP_CLEAR:
			writel(readl(reg) & ~op[3], reg);
			break;
		case T6050_OP_REWRITE:
			writel(readl(reg), reg);
			break;
		case T6050_OP_WAIT_SET:
			ret = readl_poll_timeout_atomic(reg, v, (v & op[3]) == op[3], 1, ms * 1000);
			break;
		case T6050_OP_WAIT_CLEAR:
			ret = readl_poll_timeout_atomic(reg, v, !(v & op[3]), 1, ms * 1000);
			break;
		case T6050_OP_WRITE16:
			writew(op[3], reg);
			break;
		}
		if (ret && !(op[0] & T6050_OP_OPTIONAL))
			timeouts++;
	}
	return timeouts;
}

/* The shared PHY's blocks and every sequence; one naming a block that isn't there is refused. */
static int t6050_sleep_map_ops(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	static const char *const names[] = { "storage-phy", "storage-phy-ip" };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct resource res;
		int idx = of_property_match_string(s->dev->of_node, "reg-names", names[i]);

		if (idx >= 0 && !of_address_to_resource(s->dev->of_node, idx, &res)) {
			r->rblk[T6050_RBLK_SHARED + i] = i ? r->storage_phy_ip : r->storage_phy;
			r->rblk_size[T6050_RBLK_SHARED + i] = resource_size(&res);
		}
	}
	return t6050_ops_get(s, "apple,radio-link-down", &r->link_down) ?:
	       t6050_ops_get(s, "apple,radio-link-down-port", &r->link_down_port) ?:
	       t6050_ops_get(s, "apple,radio-port-wake", &r->port_wake) ?:
	       t6050_ops_get(s, "apple,radio-dart-wake", &r->dart_wake) ?:
	       t6050_ops_get(s, "apple,radio-phy-wake", &r->phy_wake) ?:
	       t6050_ops_get(s, "apple,phy-park", &r->phy_park) ?:
	       t6050_ops_get(s, "apple,phy-down", &r->phy_down);
}

static void __iomem *t6050_radio_block(struct apple_sleep *s, struct device_node *np,
				       const char *name, unsigned int blk, u32 size)
{
	struct apple_sleep_region_state *r = s->region;
	struct resource res;
	int idx = of_property_match_string(np, "reg-names", name);

	if (idx < 0 || of_address_to_resource(np, idx, &res))
		return NULL;
	r->rblk_size[blk] = size ?: resource_size(&res);
	r->rblk[blk] = ioremap(res.start, r->rblk_size[blk]);
	return r->rblk[blk];
}

/*
 * The radio's PCIe port, for its link shutdown: the host's root port config page, port and PHY
 * registers by name, its PERST# pin, and the DART its functions are behind.
 */
static int t6050_sleep_map_radio(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	struct device_node *np, *port;
	struct of_phandle_args args, dart = {};
	struct platform_device *pdev;
	struct resource res;
	int ret = 0;

	np = of_parse_phandle(s->dev->of_node, "apple,radio-pcie", 0);
	if (!np)
		return 0;
	if (!of_device_is_available(np))
		goto out;
	r->n1_ecam = t6050_radio_block(s, np, "config", T6050_RBLK_CONFIG, SZ_4K);
	r->n1_port = t6050_radio_block(s, np, "port0", T6050_RBLK_PORT, 0x24000);
	r->n1_phy = t6050_radio_block(s, np, "phy0", T6050_RBLK_PHY, 0);
	r->n1_rc = t6050_radio_block(s, np, "rc", T6050_RBLK_RC, 0);
	/* The four ports' PHY words from phy0, and the PHY's common page below them */
	if (!of_address_to_resource(np, of_property_match_string(np, "reg-names", "phy0"), &res) &&
	    (res.start & 0xfffff) == 0x10000) {
		r->rblk[T6050_RBLK_LANES] = ioremap(res.start, 0x10000);
		r->rblk_size[T6050_RBLK_LANES] = 0x10000;
		r->rblk[T6050_RBLK_PHY_CMN] = ioremap(res.start - 0xc000, 0x4000);
		r->rblk_size[T6050_RBLK_PHY_CMN] = 0x4000;
	}
	r->radio_host = of_find_device_by_node(np);
	port = of_get_next_child(np, NULL);
	if (port && !of_parse_phandle_with_args(port, "reset-gpios", "#gpio-cells", 0, &args)) {
		r->n1_perst = args.args[0];
		of_node_put(args.np);
	}
	of_node_put(port);
	if (!of_map_iommu_id(np, 0x100, &dart)) {
		pdev = of_find_device_by_node(dart.np);
		r->dart[T6050_DART_APCIE0] = of_iomap(dart.np, 0);
		of_node_put(dart.np);
		if (!pdev) {
			ret = -EPROBE_DEFER;
			goto out;
		}
		r->radio_dart = pdev;
	}
	if (!r->n1_ecam || !r->n1_port || !r->n1_phy || !r->n1_perst)
		ret = -EINVAL;
out:
	of_node_put(np);
	return ret;
}

static void t6050_wake_masks(struct apple_sleep *s);

/* ANS's power word and the parents the DT gives it, which its wake depends on */
static void t6050_ans_chain(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	const struct apple_sleep_region_profile *p = s->soc->regions;
	struct device_node *np, *found = NULL;
	int word, n;

	r->nans_chain = 0;
	for_each_compatible_node(np, NULL, "apple,t6050-pmgr-pwrstate")
		if (sleep_region_domain_offset(s, np) == s->soc->ps.ans) {
			found = of_node_get(np);
			of_node_put(np);
			break;
		}
	np = found;
	while (np && r->nans_chain < ARRAY_SIZE(r->ans_chain)) {
		struct device_node *parent;

		word = sleep_region_domain_offset(s, np);
		n = word < 0 ? -1 : sleep_ps_find(p->power.offset, p->power.count, word);
		if (n >= 0)
			r->ans_chain[r->nans_chain++] = n;
		parent = of_parse_phandle(np, "power-domains", 0);
		of_node_put(np);
		np = parent;
	}
	of_node_put(np);
}

static int t6050_sleep_map_regs(struct apple_sleep *s)
{
	static const struct { const char *name; size_t offset; u32 minimum; } regs[] = {
		{ "cluster", offsetof(struct apple_sleep, cluster), 0x20 },
		{ "pwrgate", offsetof(struct apple_sleep, pwrgate), 0x134 },
		{ "cpu-start", offsetof(struct apple_sleep, cpu_start), 0x14 },
		{ "commit", offsetof(struct apple_sleep, commit), 4 },
		{ "wdt", offsetof(struct apple_sleep, wdt), 0x100 },
		{ "wdt-aux", offsetof(struct apple_sleep, wdt_aux), 0x30 },
		{ "disp-gate", offsetof(struct apple_sleep, disp_gate), 0x544 },
		{ "spmi", offsetof(struct apple_sleep, spmi), 0x224 },
		{ "pmc-voters", offsetof(struct apple_sleep, pmc_voters), 0x10008 },
		{ "pmc-rails", offsetof(struct apple_sleep, pmc_rails), 0x202c },
		{ "ptd", offsetof(struct apple_sleep, ptd), 0x10f08 },
	};
	unsigned int i;
	int ret = sleep_region_map(s);

	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		struct resource resource;
		void __iomem **field = (void __iomem **)((u8 *)s + regs[i].offset);
		int index = of_property_match_string(s->dev->of_node, "reg-names", regs[i].name);

		if (index < 0 || of_address_to_resource(s->dev->of_node, index, &resource) ||
		    resource_size(&resource) < regs[i].minimum)
			return dev_err_probe(s->dev, -EINVAL, "missing/short %s aperture\n", regs[i].name);
		*field = apple_sleep_map(s->dev, regs[i].name);
		if (IS_ERR(*field))
			return PTR_ERR(*field);
	}
	if (of_property_match_string(s->dev->of_node, "reg-names", "gapf") >= 0) {
		s->region->gapf = apple_sleep_map(s->dev, "gapf");
		if (IS_ERR(s->region->gapf))
			s->region->gapf = NULL;
	}
	for (i = 0; i < ARRAY_SIZE(s->region->dart); i++) {
		struct device_node *np = of_parse_phandle(s->dev->of_node, "apple,sleep-darts", i);

		if (!np)
			break;
		/* Shared with the DART driver, so not claimed. */
		s->region->dart[i] = of_iomap(np, 0);
		of_node_put(np);
	}
	for (i = 0; i < ARRAY_SIZE(s->region->spmi_bus); i++) {
		struct device_node *np = of_parse_phandle(s->dev->of_node, "apple,spmi-buses", i);

		if (!np)
			break;
		/* Each has its own driver, or none: not claimed. */
		s->region->spmi_bus[i] = of_iomap(np, 0);
		of_node_put(np);
	}
	{
		/* Blocks other drivers also map, or none does: not claimed. */
		static const struct { const char *name; size_t off; } opt[] = {
			{ "aon-clk", offsetof(struct apple_sleep_region_state, aon_clk[0]) },
			{ "media-aon-clk", offsetof(struct apple_sleep_region_state, aon_clk[1]) },
			{ "cpm0", offsetof(struct apple_sleep_region_state, cpm[0]) },
			{ "cpm1", offsetof(struct apple_sleep_region_state, cpm[1]) },
			{ "cpm2", offsetof(struct apple_sleep_region_state, cpm[2]) },
			{ "storage-phy", offsetof(struct apple_sleep_region_state, storage_phy) },
			{ "storage-phy-ip", offsetof(struct apple_sleep_region_state, storage_phy_ip) },
			{ "clkgen", offsetof(struct apple_sleep_region_state, clkgen) },
			{ "clkgen-78", offsetof(struct apple_sleep_region_state, clkgen78) },
			{ "media-clkgen-78", offsetof(struct apple_sleep_region_state, media_clkgen78) },
			{ "mcc22", offsetof(struct apple_sleep_region_state, mcc[0]) },
			{ "mcc23", offsetof(struct apple_sleep_region_state, mcc[1]) },
			{ "mcc24", offsetof(struct apple_sleep_region_state, mcc[2]) },
			{ "mcc25", offsetof(struct apple_sleep_region_state, mcc[3]) },
			{ "mcc26", offsetof(struct apple_sleep_region_state, mcc[4]) },
			{ "wake", offsetof(struct apple_sleep_region_state, wake) },
			{ "wake-gfx", offsetof(struct apple_sleep_region_state, wake_gfx) },
			{ "aic-gfx", offsetof(struct apple_sleep_region_state, aic_gfx) },
		};
		int idx;

		for (i = 0; i < ARRAY_SIZE(opt); i++) {
			idx = of_property_match_string(s->dev->of_node, "reg-names", opt[i].name);
			if (idx >= 0)
				*(void __iomem **)((u8 *)s->region + opt[i].off) =
					of_iomap(s->dev->of_node, idx);
		}
	}
	{
		struct device_node *np = of_parse_phandle(s->dev->of_node, "apple,pinctrl", 0);

		if (np) {
			s->region->ap_gpio = of_iomap(np, 0);
			of_node_put(np);
		}
	}
	ret = of_property_read_variable_u32_array(s->dev->of_node, "apple,sleep-low-pins",
						  s->region->pins_low, 0,
						  ARRAY_SIZE(s->region->pins_low));
	s->region->npins_low = max(ret, 0);
	t6050_wake_masks(s);
	t6050_ans_chain(s);
	/* The clusters' CPM set-up, where the boot loader puts it: no entry, no write */
	ret = t6050_cpm_prop(s, "apple,cpm-boot", &s->region->cpm_boot, &s->region->ncpm_boot) ?:
	      t6050_cpm_prop(s, "apple,cpm-rearm", &s->region->cpm_rearm, &s->region->ncpm_rearm) ?:
	      t6050_cpm_prop(s, "apple,cpm-rearm-boot-cluster", &s->region->cpm_rearm_boot,
			     &s->region->ncpm_rearm_boot);
	if (ret)
		return ret;
	ret = t6050_sleep_map_radio(s) ?: t6050_sleep_map_ops(s);
	if (ret)
		return ret;
	t6050_sleep_boot_auto(s);
	return 0;
}

static const struct apple_sleep_smc_policy t6050_smc_policy = {
	.pmvc = 0x101,
	/* Spacings between the SMC's sleep messages, as measured from macOS, used as floors.
	 */
	.warn_to_pmvc_us = 42000,
	.pmvc_to_slpa_us = 35000,
	.slpa_to_idle_us = 10000,
	.idle_to_timer_us = 60000,
	.waka_to_pmvc_us = 58000,
};

#define M5_PMC_STRIDE 0x4000

static int t6050_sleep_ptd_ownership(struct apple_sleep *s, bool own)
{
	static const u32 ids[] = { 12, 13, 14, 36 };
	struct apple_sleep_region_state *r = s->region;
	struct device_node *report, *entry;
	unsigned int i;
	int ret = 0;

	if (!own) {
		for (i = 0; i < ARRAY_SIZE(ids); i++)
			if (r->ptd_entry[i]) {
				apple_pmp_report_entry_commit_owned(r->ptd_entry[i], false);
				of_node_put(r->ptd_entry[i]);
				r->ptd_entry[i] = NULL;
			}
		r->ptd_owned = false;
		return 0;
	}
	report = of_find_compatible_node(NULL, NULL, "apple,t6050-pmp-v2-report");
	if (!report)
		return -ENODEV;
	for_each_child_of_node(report, entry) {
		u32 id;

		if (of_property_read_u32(entry, "reg", &id))
			continue;
		for (i = 0; i < ARRAY_SIZE(ids); i++)
			if (id == ids[i]) {
				ret = apple_pmp_report_entry_commit_owned(entry, true);
				if (ret) {
					of_node_put(entry);
					goto out;
				}
				r->ptd_entry[i] = of_node_get(entry);
			}
	}
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		if (!r->ptd_entry[i])
			ret = -ENODEV;
out:
	of_node_put(report);
	if (ret)
		t6050_sleep_ptd_ownership(s, false);
	else
		r->ptd_owned = true;
	return ret;
}

#define SYS_APL_AGT_CTL_EL02		sys_reg(3, 4, 15, 2, 7)
#define SYS_APL_AGTCNTP_CTL_EL02	sys_reg(3, 4, 15, 4, 3)

/*
 * XNU turns EL1's Apple timers off on the boot core before its sleep; under Linux they keep
 * iBoot's state. Done on the core itself, in a bound worker.
 */
static struct {
	struct work_struct work;
	struct completion done;
} t6050_agt_off;

static void t6050_agt_off_work(struct work_struct *w)
{
	write_sysreg_s(0, SYS_APL_AGT_CTL_EL02);
	write_sysreg_s(0, SYS_APL_AGTCNTP_CTL_EL02);
	isb();
	complete(&t6050_agt_off.done);
}

static void t6050_sleep_boot_timer_off(struct apple_sleep *s)
{
	int cpu;

	if (!is_kernel_in_hyp_mode())
		return;
	for_each_online_cpu(cpu) {
		if (cpu_logical_map(cpu) != s->mbox->boot_mpidr)
			continue;
		INIT_WORK(&t6050_agt_off.work, t6050_agt_off_work);
		init_completion(&t6050_agt_off.done);
		queue_work_on(cpu, system_highpri_wq, &t6050_agt_off.work);
		if (!wait_for_completion_timeout(&t6050_agt_off.done, 2 * HZ))
			dev_warn(s->dev, "boot core's timers not turned off\n");
		return;
	}
}

/* WD2 from the commit: a net until the sleep engine arms it, long enough for the resume. */
#define T6050_WDT_COMMIT_SECS	13

static void t6050_sleep_n1_down(struct apple_sleep *s, bool perst_alone);
static bool t6050_n1_host_bound(struct apple_sleep *s);

#define T6050_GPIO_DATA		BIT(0)
#define T6050_GPIO_MODE		GENMASK(3, 1)
#define T6050_GPIO_MODE_OUT	1

/* A pin driven high goes low; one in any other state is left alone. */
static void t6050_pin_low(void __iomem *pin)
{
	u32 v = readl(pin);

	if (FIELD_GET(T6050_GPIO_MODE, v) == T6050_GPIO_MODE_OUT && (v & T6050_GPIO_DATA))
		writel(v & ~T6050_GPIO_DATA, pin);
}

/* The AP pins macOS drives low in its sleep entry. */
static void t6050_sleep_pins_low(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;

	for (i = 0; r->ap_gpio && i < r->npins_low; i++)
		t6050_pin_low(r->ap_gpio + 4 * r->pins_low[i]);
}

static int t6050_sleep_begin(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;
	int ret;

	t6050_sleep_pins_low(s);
	r->n1_up_ret = 0;
	r->n1_host = t6050_n1_host_bound(s);
	if (r->n1_host) {
		/* Its functions are in use until their own suspend: the host's noirq suspend runs it. */
		r->n1_held = 0;
		r->n1_state = 5;
	} else {
		t6050_sleep_n1_down(s, true);
		/* The port's DART is clocked by the port: its restore waits for a host. */
		if (r->radio_dart && (!r->n1_state || r->n1_state == 4))
			apple_dart_set_host_resumes(&r->radio_dart->dev);
	}

	/* The GFX die's interrupt controller configuration, written back at the wake. */
	if (r->aic_gfx)
		r->gfx_aic[0] = readl(r->aic_gfx + 0x14);
	/* Wake writes these back: iBoot's requests. */
	for (i = 0; i < 5; i++)
		r->cluster_saved[i] = readl(s->cluster + 4 * i);
	s->ans_m5_wake_error = -EAGAIN;
	s->ans_off_ret = 0;
	r->staged = false;
	r->committed = false;
	r->pmc_saved = false;
	r->floor_changed = 0;
	r->display_lowered = 0;
	bitmap_zero(r->manager_down, ARRAY_SIZE(t6050_ps_all));
	r->late_errors = 0;
	r->pmu_errors = 0;
	r->alarm_valid = false;
	r->boot_stage_written = false;
	s->regs_staged = s->entry_prepared = s->wake_masks_saved = false;
	s->smc_idle_sent = s->smc_idle_ack = 0;
	for (i = 0; i < s->soc->display_count; i++)
		s->display[i].quiesced = false;
	r->cycle_ran = false;
	t6050_sleep_boot_timer_off(s);
	/* Preserve actual driver ownership bitmap, including a prior incomplete recovery. */
	ret = t6050_sleep_ptd_ownership(s, true);
	r->attempt_active = !ret;
	return ret;
}

static void t6050_pmc_floor(struct apple_sleep *s, unsigned int agent)
{
	s->region->floor_changed |= BIT(agent);
	writel(0, s->pmc_voters + M5_PMC_STRIDE * agent);
	readl(s->pmc_voters + M5_PMC_STRIDE * agent + 4);
}

/* The ADT's no_ps PMGR devices with real PS-bank offsets (coproc/inv/pmgr-devices.txt): 154 words */
/* Not in a mapped bank, so not read: NUB_GFX_SRAM 0x2388304000 */
static const struct { u32 word; const char *name; } t6050_no_ps[] = {
	{ SLEEP_REGION_WORD(T6050_PS0, 0x100), "PMS" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x188), "FAB0_AFI_0" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x190), "FAB0_AFI_1" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x198), "FAB0_AFI_2" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x1a0), "FAB0_AFI_3" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x1a8), "FAB_B2BC_AFI_0" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x1b0), "FAB_B2BC_AFI_1" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x1b8), "B2BC_AFI_0" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x1c0), "B2BC_AFI_0_DFX" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x1c8), "B2BC_AFI_1" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x1d0), "B2BC_AFI_1_DFX" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x1d8), "FAB0_PIOGW" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x1e0), "FAB3_SOC" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x1f8), "SBR" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x308), "DISPEXT0_DBE" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x310), "DPEXT0" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x318), "DISPEXT0_GP0" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x320), "DISPEXT0_GP1" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x328), "DISPEXT0_GPL" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x330), "DISPEXT0_PPP" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x368), "DISPEXT2_DBE" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x370), "DPEXT2" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x378), "DISPEXT2_GP0" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x380), "DISPEXT2_GP1" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x388), "DISPEXT2_GPL" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x390), "DISPEXT2_PPP" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x398), "DISPEXT3_DBE" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3a0), "DPEXT3" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3a8), "DISPEXT3_GP0" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3b0), "DISPEXT3_GP1" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3b8), "DISPEXT3_GPL" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3c0), "DISPEXT3_PPP" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3c8), "DISPEXT1_DBE" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3d0), "DPEXT1" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3d8), "DISPEXT1_GP0" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3e0), "DISPEXT1_GP1" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3e8), "DISPEXT1_GPL" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x3f0), "DISPEXT1_PPP" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x428), "DISPEXT0_DSC" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x430), "DISPEXT2_DSC" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x438), "DISPEXT3_DSC" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x440), "DISPEXT1_DSC" },
	{ SLEEP_REGION_WORD(T6050_PS0, 0x448), "FAB0_AFIGW" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x120), "APCIE_GE" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x188), "APCIE_SYS_GE" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x1b8), "APCIE_SYS_GE_OL" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x1c8), "DISP_SPI" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x1d0), "DISP_SPMI" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x1d8), "DISP_SMB0" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x1e0), "DISP_SMB1" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x1e8), "DISP_DBE" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x1f0), "DP" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x1f8), "DISP_GP0" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x200), "DISP_GP1" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x208), "DISP_GPL" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x210), "DISP_CMB" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x218), "DISP_PPP" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x220), "DISP_BRC" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x228), "DISP_BLC" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x230), "DISP_HILO" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x238), "DISP_RCA" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x250), "APHY_GE_AUS5_A" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x260), "DISP_DSC" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x278), "APHY_GE_AUS5" },
	{ SLEEP_REGION_WORD(T6050_PS1, 0x2b0), "FPWM1" },
	{ SLEEP_REGION_WORD(T6050_PS_AON, 0x78), "NUB_AON" },
	{ SLEEP_REGION_WORD(T6050_PS_AON, 0x90), "NUB_OCLA" },
	{ SLEEP_REGION_WORD(T6050_PS_AON, 0x1a8), "NUB_FABRIC" },
	{ SLEEP_REGION_WORD(T6050_PS_AON, 0x1b0), "NUB_SRAM" },
	{ SLEEP_REGION_WORD(T6050_PS_AON, 0x1b8), "DEBUG_SWITCH" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x100), "PMS_GFX" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x150), "FAB1_AFI_0" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x158), "FAB1_AFI_1" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x160), "FAB_B2BG_AFI_0" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x168), "FAB_B2BG_AFI_1" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x170), "B2BG_AFI_0" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x178), "B2BG_AFI_0_DFX" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x180), "B2BG_AFI_1" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x190), "FAB1_AFIGW" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x1a0), "FAB1_PIOGW" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x1b8), "FAB2_SOC" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x1e8), "SBR_GFX" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x1f0), "VENC1_SYS" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x210), "FAB_AFI_AMCC_0" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x248), "DCS_00" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x258), "DCS_01" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x268), "DCS_02" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x278), "DCS_03" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x290), "FAB_AFI_AMCC_1" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x2c8), "DCS_04" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x2d8), "DCS_05" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x2e8), "DCS_06" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x2f8), "DCS_07" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x310), "FAB_AFI_AMCC_2" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x348), "DCS_08" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x358), "DCS_09" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x368), "DCS_10" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x378), "DCS_11" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x390), "FAB_AFI_AMCC_3" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x3c8), "DCS_12" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x3d8), "DCS_13" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x3e8), "DCS_14" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x3f8), "DCS_15" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x410), "FAB_AFI_AMCC_4" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x418), "AMCC16" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x420), "AMCC17" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x428), "AMCC18" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x430), "AMCC19" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x448), "DCS_16" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x458), "DCS_17" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x468), "DCS_18" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x478), "DCS_19" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x490), "FAB_AFI_AMCC_5" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x498), "AMCC20" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x4a0), "AMCC21" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x4a8), "AMCC22" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x4b0), "AMCC23" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x4c8), "DCS_20" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x4d8), "DCS_21" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x4e8), "DCS_22" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x4f8), "DCS_23" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x510), "FAB_AFI_AMCC_6" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x518), "AMCC24" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x520), "AMCC25" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x528), "AMCC26" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x530), "AMCC27" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x548), "DCS_24" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x558), "DCS_25" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x568), "DCS_26" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x578), "DCS_27" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x590), "FAB_AFI_AMCC_7" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x598), "AMCC28" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x5a0), "AMCC29" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x5a8), "AMCC30" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x5b0), "AMCC31" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x5d0), "DCS_28" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x5e0), "DCS_29" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x5f0), "DCS_30" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x600), "DCS_31" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x618), "FAB_B2BD_AFI_0" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x620), "FAB_B2BD_AFI_1" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x628), "B2BD_AFI_0" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x630), "B2BD_AFI_0_DFX" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x638), "B2BD_AFI_1" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x640), "B2BD_AFI_1_DFX" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x8028), "VENC1_DMA" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x8030), "VENC1_PIPE4" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x8038), "VENC1_PIPE5" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x8040), "VENC1_ME0" },
	{ SLEEP_REGION_WORD(T6050_PS_MEDIA, 0x8048), "VENC1_ME1" },
	{ SLEEP_REGION_WORD(T6050_PS_GFX_AON, 0x60), "NUB_GFX_AON" },
	{ SLEEP_REGION_WORD(T6050_PS_GFX_AON, 0x70), "NUB_GFX_OCLA" },
	{ SLEEP_REGION_WORD(T6050_PS_GFX_AON, 0x78), "NUB_GFX_FABRIC" },
	{ SLEEP_REGION_WORD(T6050_PS_GFX_AON, 0x80), "DEBUG_GFX_SWITCH" },
};
/* /arm-io/pmgr device-check-ps-b4-sleep: each display set's children, as indices into t6050_no_ps; ps.disp[] order */
static const u8 t6050_disp_children[5][16] = {
	{ 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 62 },
	{ 14, 15, 16, 17, 18, 19, 38 },
	{ 32, 33, 34, 35, 36, 37, 41 },
	{ 20, 21, 22, 23, 24, 25, 39 },
	{ 26, 27, 28, 29, 30, 31, 40 },
};
static const u8 t6050_disp_nchildren[5] = { 16, 7, 7, 7, 7 };

static void t6050_sleep_entry_lower(struct apple_sleep *s);

/*
 * Driver-provided readiness is required; never clear a running DCP's RUN bit. One set's words,
 * right after its own firmware stop, as macOS lowers each set's after its idle.
 */
static int t6050_sleep_display_off_one(struct apple_sleep *s, unsigned int d)
{
	struct apple_sleep_region_state *r = s->region;
	u32 cpu = s->soc->ps.disp[d][0];
	u32 val = readl(sleep_region_reg(s, cpu));
	unsigned int j;
	int ret;

	if (!FIELD_GET(PS_TARGET, val) && !FIELD_GET(PS_ACTUAL, val))
		return 0;
	if (!s->display[d].bound || !READ_ONCE(s->display[d].quiesced) || !s->display[d].asc ||
	    (readl(s->display[d].asc + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_RUN))
		return -EBUSY;
	if (!s->entry_committed)
		return -EPERM;
	/*
	 * The ADT's device-check-ps-b4-sleep children of this set: any that reads on goes down first.
	 */
	for (j = 0; j < t6050_disp_nchildren[d]; j++) {
		u32 child = t6050_no_ps[t6050_disp_children[d][j]].word;
		u32 cv = readl(sleep_region_reg(s, child));

		if (!FIELD_GET(PS_TARGET, cv) && !FIELD_GET(PS_ACTUAL, cv))
			continue;
		sleep_ps_lower(s, child);
		r->disp_children_lowered[d] |= BIT(j);
		r->late_timeouts += !!sleep_ps_wait_off(s, child);
	}
	/* Snapshot belongs to the returning pre-SEP phase. */
	r->display_lowered |= BIT(d);
	for (j = 0; j < 3; j++) {
		int index = sleep_ps_find(t6050_ps_all, ARRAY_SIZE(t6050_ps_all), s->soc->ps.disp[d][j]);

		if (index >= 0)
			set_bit(index, r->manager_down);
	}
	val &= ~(PS_AUTO_ENABLE | PS_FLAGS);
	writel(val, sleep_region_reg(s, cpu));
	if (!d) {
		/* Internal gate snapshot was taken before SEP. */
		writel(3, s->disp_gate + 0x51c);
		writel(0, s->disp_gate + 0x540);
	}
	writel((val & ~PS_TARGET) | PS_FLAGS, sleep_region_reg(s, cpu));
	ret = sleep_ps_wait_off(s, cpu);
	/*
	 * FE, then SYS, to 0x3f0 on every set: macOS lowers each DISPEXT's within 35 us of its CPU
	 * word, as the internal DISP's.
	 */
	for (j = 1; !ret && j < 3; j++) {
		sleep_ps_lower(s, s->soc->ps.disp[d][j]);
		ret = sleep_ps_wait_off(s, s->soc->ps.disp[d][j]);
	}
	return ret;
}

/*
 * A display word ready for the V27 stop: target 0xf, not disabled, and actually on. An external
 * set (d > 0) with no display attached idles under automatic power: target 0xf, AUTO_ENABLE and a
 * lower actual state. macOS's own stop of those words starts by
 * clearing AUTO_ENABLE, so that state is ready too.
 */
static bool t6050_display_word_ready(unsigned int d, u32 value)
{
	if (FIELD_GET(PS_TARGET, value) != 0xf || (value & PS_DEV_DISABLE))
		return false;
	if (FIELD_GET(PS_ACTUAL, value) == 0xf)
		return true;
	return d && (value & PS_AUTO_ENABLE) && FIELD_GET(PS_ACTUAL, value);
}

static int t6050_sleep_display_prepare(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int d, j;
	int ret;

	if (!apple_sleep_v27_late() || s->entry_committed)
		return -EOPNOTSUPP;
	for (d = 0; d < s->soc->display_count; d++) {
		ret = sleep_v27_final_check(d);
		if (ret) {
			dev_info(s->dev, "display %u: final check %d\n", d, ret);
			return ret;
		}
		for (j = 0; j < 3; j++) {
			u32 value = readl(sleep_region_reg(s, s->soc->ps.disp[d][j]));

			if (!t6050_display_word_ready(d, value)) {
				dev_info(s->dev, "display %u: word %u reads %#x\n", d, j, value);
				return -EAGAIN;
			}
			r->display_saved[d][j] = value;
		}
	}
	r->gate_saved[0] = readl(s->disp_gate + 0x51c);
	r->gate_saved[1] = readl(s->disp_gate + 0x540);
	memset(r->disp_children_lowered, 0, sizeof(r->disp_children_lowered));
	sleep_region_snapshot(s);
	t6050_sleep_entry_lower(s);
	return 0;
}

/*
 * Row 52: PMC +0x154's channels 2 and 3 down in macOS's
 * form, channel 2 by W (v & ~4) | 0x400, channel 3 by W (v & ~8) | 0x800, +0x158 polled to 0xf
 * after each. Linux never writes it; only what reads on is taken down.
 */
static void t6050_pmc_154(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	void __iomem *req = s->pmc_rails + 0x154;
	u32 v = readl(req), status;

	r->pmc154[0] = v;
	r->pmc154[1] = readl(req + 4);
	if (v & BIT(2)) {
		writel((v & ~BIT(2)) | 0x400, req);
		readl_poll_timeout_atomic(req + 4, status, status == 0xf, 1, 2000);
		v = readl(req);
	}
	if (v & BIT(3)) {
		writel((v & ~BIT(3)) | 0x800, req);
		readl_poll_timeout_atomic(req + 4, status, status == 0xf, 1, 2000);
	}
}

/* Row 8: the CLPC floor stepped 0x2, 0x1, then 0. */
static void t6050_clpc_steps(struct apple_sleep *s)
{
	void __iomem *reg = s->pmc_voters + M5_PMC_STRIDE * 1;

	/*
	 * Stepped from any floor, 0 included: the memory controller follows the writes, not the
	 * value read back (which stays 0), and only 1 then 0 leaves it where macOS commits.
	 */
	s->region->floor_changed |= BIT(1);
	writel(2, reg);
	mdelay(9);
	writel(1, reg);
	mdelay(9);
	t6050_pmc_floor(s, 1);
}

/*
 * The AP wake-enable words (1 = masked), ten on this die and one on the GFX die. iBoot leaves
 * them 0, every source enabled, and a pending source then turns the entry round at once. At
 * the stage every source but the DT's apple,wake-enable (and apple,wake-enable-gfx) is masked;
 * the saved values go back at the wake. The sources come from the boot loader: without them
 * the words are left alone.
 */
#define T6050_WAKE_ENABLE	0x0c
#define T6050_WAKE_WORDS	10
/* Sources in the last word, and on the GFX die */
#define T6050_WAKE_LAST		GENMASK(4, 0)
#define T6050_WAKE_GFX		GENMASK(24, 0)

static void t6050_wake_masks(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	u32 ids[64];
	int n, i;

	for (i = 0; i < T6050_WAKE_WORDS; i++)
		r->wake_mask[i] = i == T6050_WAKE_WORDS - 1 ? T6050_WAKE_LAST : U32_MAX;
	r->wake_mask[T6050_WAKE_WORDS] = T6050_WAKE_GFX;
	n = of_property_read_variable_u32_array(s->dev->of_node, "apple,wake-enable", ids, 0,
						ARRAY_SIZE(ids));
	r->wake_sources = n > 0;
	if (!r->wake_sources)
		dev_warn(s->dev, "no wake sources in the DT: the AP wake masks stay as iBoot left them\n");
	for (i = 0; i < n; i++)
		if (ids[i] < 32 * T6050_WAKE_WORDS)
			r->wake_mask[ids[i] / 32] &= ~BIT(ids[i] % 32);
	n = of_property_read_variable_u32_array(s->dev->of_node, "apple,wake-enable-gfx", ids, 0,
						ARRAY_SIZE(ids));
	for (i = 0; i < n; i++)
		if (ids[i] < 32)
			r->wake_mask[T6050_WAKE_WORDS] &= ~BIT(ids[i]);
}

static void t6050_wake_arm(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;

	if (!r->wake || !r->wake_gfx || !r->wake_sources)
		return;
	for (i = 0; i < T6050_WAKE_WORDS; i++) {
		r->wake_saved[i] = readl(r->wake + T6050_WAKE_ENABLE + 4 * i);
		writel(r->wake_mask[i], r->wake + T6050_WAKE_ENABLE + 4 * i);
	}
	r->wake_saved[T6050_WAKE_WORDS] = readl(r->wake_gfx + T6050_WAKE_ENABLE);
	writel(r->wake_mask[T6050_WAKE_WORDS], r->wake_gfx + T6050_WAKE_ENABLE);
	r->wake_armed = true;
}

static void t6050_wake_disarm(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;

	if (!r->wake_armed)
		return;
	for (i = 0; i < T6050_WAKE_WORDS; i++)
		writel(r->wake_saved[i], r->wake + T6050_WAKE_ENABLE + 4 * i);
	writel(r->wake_saved[T6050_WAKE_WORDS], r->wake_gfx + T6050_WAKE_ENABLE);
	r->wake_armed = false;
}

static int t6050_sleep_stage(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;
	u32 park;

	r->staged = true;
	for (i = 0; i < 5; i++)
		r->floor[i] = readl(s->pmc_voters + M5_PMC_STRIDE * i);
	for (i = 0; i < 4; i++)
		r->rails[i] = readl(s->pmc_rails + 0x2000 + 4 * i);
	r->rails[4] = readl(s->pmc_rails + 0x2028);
	r->rails[5] = readl(s->pmc_rails + 0x2020);
	r->pmc_saved = true;
	/* macOS's order: PMC +0x154, then the CLPC floor's steps. */
	t6050_pmc_154(s);
	t6050_clpc_steps(s);
	r->park_saved = readl(r->base[T6050_PERF] + 0x18000);
	writel(0, r->base[T6050_PERF] + 0x18000);
	readl_poll_timeout_atomic(r->base[T6050_PERF] + 0x182a0, park,
		!(park & BIT(1)) && FIELD_GET(GENMASK(15, 8), park) ==
		FIELD_GET(GENMASK(23, 16), park), 1, 1000);
	for (i = 0; i < 4; i++)
		writel(0xb, s->pmc_rails + 0x2000 + 4 * i);
	writel(0x40a, s->pmc_rails + 0x2028);
	writel(0x10000, s->pmc_rails + 0x2020);
	writel(0, s->pmc_rails + 0x2020);
	r->ptd_saved = readq(s->ptd + M5_PTD_READ);
	t6050_wake_arm(s);
	return 0;
}

#define T6050_LATE(r, off, name) { SLEEP_REGION_WORD(r, off), name }

/*
 * Power words macOS has off by its commit that Linux can still have on then, each at macOS's own
 * last write to it. The first nine at the start of .enter, in the order of macOS's runtime
 * power-downs. The rest after ANS, in macOS's order, with the pmVC writes, the PMU's write and the
 * SPMI masks between them (t6050_sleep_after_ans). Lowered only if on, and polled off.
 */
enum {
	T6050_LATE_ENTRY_END = 9,
	T6050_LATE_ATC2_USB = 9, T6050_LATE_ATC1_USB, T6050_LATE_I2C3, T6050_LATE_I2C1,
	T6050_LATE_I2C2, T6050_LATE_I2C6, T6050_LATE_SPI4, T6050_LATE_SPI2,
};
static const struct { u32 word; const char *name; } t6050_late_off[] = {
	T6050_LATE(T6050_PS1, 0x110, "global_aux_hpd_1"),
	T6050_LATE(T6050_PS1, 0x108, "global_aux_hpd_0"),
	T6050_LATE(T6050_PS1, 0x2c0, "i2c0"),
	T6050_LATE(T6050_PS1, 0x2e8, "i2c5"),
	T6050_LATE(T6050_PS1, 0x3a0, "isp_sys"),
	T6050_LATE(T6050_PS1, 0x318, "aes"),
	T6050_LATE(T6050_PS1, 0x100, "ispsens0"),
	T6050_LATE(T6050_PS1, 0x2a8, "fpwm0"),
	T6050_LATE(T6050_PS_AON, 0x0d8, "atc2_common_dp_0"),
	T6050_LATE(T6050_PS_AON, 0x178, "atc2_usb_aon"),
	T6050_LATE(T6050_PS_AON, 0x160, "atc1_usb_aon"),
	T6050_LATE(T6050_PS1, 0x2d8, "i2c3"),
	T6050_LATE(T6050_PS1, 0x2c8, "i2c1"),
	T6050_LATE(T6050_PS1, 0x2d0, "i2c2"),
	T6050_LATE(T6050_PS1, 0x2f0, "i2c6"),
	T6050_LATE(T6050_PS1, 0x340, "spi4"),
	T6050_LATE(T6050_PS1, 0x330, "spi2"),
};

/*
 * No Linux driver touches these. macOS has them at state 4 up to its commit, and writes them
 * only in its wake restore. Capped at 4 if higher at the start of .enter, as the old driver did. ATC2_COMMON_DP_0 isn't one:
 * macOS turns it off (above).
 */
#define T6050_STATE4	4
static const struct { u32 word; const char *name; } t6050_state4[] = {
	T6050_LATE(T6050_PS0, 0x138, "pmgr_soc_ocla"),
	T6050_LATE(T6050_PS_MEDIA, 0x120, "pmgr_soc_ocla_g"),
	T6050_LATE(T6050_PS1, 0x2b8, "fpwm2"),
	T6050_LATE(T6050_PS1, 0x2f8, "i2c7"),
	T6050_LATE(T6050_PS_AON, 0x8058, "smc_i2cm6"),
	T6050_LATE(T6050_PS_AON, 0x0b8, "atc1_common_dp_0"),
	T6050_LATE(T6050_PS_AON, 0x0c0, "atc1_common_dp_1"),
	T6050_LATE(T6050_PS_AON, 0x0c8, "atc1_common_dp_2"),
	T6050_LATE(T6050_PS_AON, 0x0d0, "atc1_common_dp_3"),
	T6050_LATE(T6050_PS_AON, 0x0e0, "atc2_common_dp_1"),
	T6050_LATE(T6050_PS_AON, 0x0e8, "atc2_common_dp_2"),
	T6050_LATE(T6050_PS_AON, 0x0f0, "atc2_common_dp_3"),
	T6050_LATE(T6050_PS_AON, 0x0f8, "atc3_common_dp_0"),
	T6050_LATE(T6050_PS_AON, 0x100, "atc3_common_dp_1"),
	T6050_LATE(T6050_PS_AON, 0x108, "atc3_common_dp_2"),
	T6050_LATE(T6050_PS_AON, 0x110, "atc3_common_dp_3"),
};

static bool t6050_late_word_on(struct apple_sleep *s, u32 word)
{
	u32 val = readl(sleep_region_reg(s, word));

	return FIELD_GET(PS_TARGET, val) || FIELD_GET(PS_ACTUAL, val);
}

static void t6050_late_lower(struct apple_sleep *s, unsigned int i)
{
	struct apple_sleep_region_state *r = s->region;
	u32 word = t6050_late_off[i].word;

	if (!t6050_late_word_on(s, word))
		return;
	sleep_ps_lower(s, word);
	r->late_lowered |= BIT(i);
	r->late_timeouts += !!sleep_ps_wait_off(s, word);
}

/* At the start of .enter, after the snapshot a refusal restores: rows 1 and the state-4 caps. */
static void t6050_sleep_entry_lower(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;

	r->late_lowered = r->state4_set = r->late_timeouts = 0;
	for (i = 0; i < T6050_LATE_ENTRY_END; i++)
		t6050_late_lower(s, i);
	for (i = 0; i < ARRAY_SIZE(t6050_state4); i++) {
		void __iomem *reg = sleep_region_reg(s, t6050_state4[i].word);
		u32 val = readl(reg);

		if (FIELD_GET(PS_TARGET, val) <= T6050_STATE4 && !(val & PS_AUTO_ENABLE))
			continue;
		writel((val & ~(PS_AUTO_ENABLE | PS_TARGET)) | T6050_STATE4, reg);
		r->state4_set |= BIT(i);
	}
	dev_info(s->dev, "entry: idle words lowered %#x (%u timed out), capped at 4 %#x\n",
		 r->late_lowered, r->late_timeouts, r->state4_set);
}

/*
 * Past the point of no return, between ANS's stop and SIO's, in macOS's order: ATC2_USB_AON,
 * pmVC, ATC1_USB_AON, pmVC, the watchdog service, PMU 0xf801 = 0x50 and its seven reads, I2C3/I2C1/I2C2/I2C6, the five buses'
 * SPMI masks (+0x420 = 0x2000001, nub-spmi0's isn't one), SPI4, SPI2.
 */
static void t6050_sleep_after_ans(struct apple_sleep *s)
{
	static const u8 order[] = { 1, 0, 4, 2, 3 };	/* nub-spmi2, -a0, aop-spmi0, -a1, nub-spmi3 */
	static const struct { u16 address; u8 len; } reads[] = {
		{ 0xf801, 1 }, { 0xf808, 4 }, { 0xf802, 1 }, { 0xf818, 1 }, { 0xf819, 2 },
		{ 0xf800, 1 }, { 0xa7dc, 4 },
	};
	struct apple_sleep_region_state *r = s->region;
	unsigned int i, n, failed = 0;
	u8 stage = 0x50;

	/* A port in use keeps its word, as macOS with both ports in use. */
	r->usb_aon_kept = READ_ONCE(sleep_usb_aon_kept) & (BIT(1) | BIT(2));
	if (!(r->usb_aon_kept & BIT(2)))
		t6050_late_lower(s, T6050_LATE_ATC2_USB);
	r->after_ans_ret[0] = apple_sleep_smc_pmvc(s);
	if (!(r->usb_aon_kept & BIT(1)))
		t6050_late_lower(s, T6050_LATE_ATC1_USB);
	r->after_ans_ret[1] = apple_sleep_smc_pmvc(s);
	writel(0, s->wdt);
	writel(0, s->wdt + 0x10);
	r->boot_stage_written = true; /* Restored at the wake even if the reply is lost. */
	r->after_ans_ret[2] = t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_WRITEL, 0xf801, &stage, NULL, 1);
	memset(r->pmu_rd, 0, sizeof(r->pmu_rd));
	for (i = 0, n = 0; i < ARRAY_SIZE(reads); n += reads[i].len, i++)
		if (t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_READL, reads[i].address, NULL,
					 r->pmu_rd + n, reads[i].len))
			failed |= BIT(i);
	r->after_ans_ret[3] = failed;
	for (i = T6050_LATE_I2C3; i <= T6050_LATE_I2C6; i++)
		t6050_late_lower(s, i);
	r->spmi_mask[0] = readl(s->spmi + M5_SPMI_IRQ_EN);
	/* nub-spmi0 first: its interrupt outputs are AP wake sources. */
	writel(readl(s->spmi + M5_SPMI_IRQ_STATUS), s->spmi + M5_SPMI_IRQ_STATUS);
	writel(M5_SPMI_IRQ_SLEEP, s->spmi + M5_SPMI_IRQ_EN);
	for (i = 0; i < ARRAY_SIZE(order); i++) {
		void __iomem *bus = r->spmi_bus[order[i]];

		if (!bus)
			continue;
		udelay(480);
		/* What polled transactions latched since boot, acked as XNU's handler would have. */
		writel(readl(bus + M5_SPMI_IRQ_STATUS), bus + M5_SPMI_IRQ_STATUS);
		r->spmi_mask[1 + order[i]] = readl(bus + M5_SPMI_IRQ_EN);
		writel(M5_SPMI_IRQ_SLEEP, bus + M5_SPMI_IRQ_EN);
	}
	r->spmi_masked = true;
	for (i = T6050_LATE_SPI4; i <= T6050_LATE_SPI2; i++)
		t6050_late_lower(s, i);
}

/*
 * Non-display DART stops, as SPTM makes them on macOS: mask and clear the DART's error interrupts, disable each enabled stream with its own
 * write, flush the TLB, then write the block's GAPF slice 0x2. SEP's DART gets the interrupt
 * words and the slice only, 11 us before the SEP request.
 */
#define T6050_DART_ERROR_MASK	0x4060
#define T6050_DART_EXCEPTION	0x4000
#define T6050_DART_ERROR_STATUS	0x4204
#define T6050_DART_PROTECT	0x200
#define T6050_DART_ENABLE	0xc00
#define T6050_DART_DISABLE	0xc20
#define T6050_DART_TLB_CMD	0x80
#define T6050_DART_TLB_FLUSH	0xa000
#define T6050_DART_TLB_BUSY	BIT(31)

static const struct {
	u32 slice;
	bool streams;
	u32 ps;	/* Its domain's word; touched only while that reads on. 0: always on. */
	bool all16;	/* Streams 1-15 each written whether enabled or not, then any enabled above */
} t6050_darts[] = {
	[T6050_DART_SEP] = { 0xb00, false, 0 },
	[T6050_DART_PMP0] = { 0x1c0, true, 0 },
	[T6050_DART_SIO] = { 0xac0, true, SLEEP_REGION_WORD(T6050_PS1, 0x290) },
	[T6050_DART_APCIE0] = { 0x980, true, SLEEP_REGION_WORD(T6050_PS1, 0x118), true },
};

static void t6050_sleep_dart_stop(struct apple_sleep *s, unsigned int i)
{
	struct apple_sleep_region_state *r = s->region;
	void __iomem *dart = r->dart[i];
	u32 enabled, val;
	unsigned int sid;

	if (!dart || (t6050_darts[i].ps && !sleep_ps_on(s, t6050_darts[i].ps)))
		return;
	writel(U32_MAX, dart + T6050_DART_ERROR_MASK);
	writel(0, dart + T6050_DART_EXCEPTION);
	writel(U32_MAX, dart + T6050_DART_ERROR_STATUS);
	if (t6050_darts[i].streams) {
		enabled = readl(dart + T6050_DART_ENABLE);
		for (sid = 0; sid < 32; sid++)
			if ((t6050_darts[i].all16 && sid && sid < 16) || (enabled & BIT(sid))) {
				writel(BIT(sid), dart + T6050_DART_DISABLE);
				if (t6050_darts[i].all16)
					readl(dart + T6050_DART_DISABLE);
			}
		writel(T6050_DART_TLB_FLUSH, dart + T6050_DART_TLB_CMD);
		r->dart_timeouts += !!readl_poll_timeout_atomic(dart + T6050_DART_TLB_CMD, val,
								!(val & T6050_DART_TLB_BUSY), 1, 1000);
	}
	if (r->gapf)
		writel(0x2, r->gapf + t6050_darts[i].slice);
	r->dart_done |= BIT(i);
}

/*
 * The radio's link shutdown before the sleep: the DT's apple,radio-link-down (the link's own
 * shutdown, through PME_Turn_Off and its ack), PERST# low, the PHY's reference clock gate off,
 * the port's DART stopped and its GAPF slice, then apple,radio-link-down-port. The port's SYS
 * words idle off under automatic power: they are held on for the sequence and given back
 * after it. Only with the link up. With a host driver bound it runs from the host's noirq
 * suspend instead.
 * n1_state: 0 done, 1 no registers or no sequence, 2 a domain off, 3 link down, 4 no PME_TO
 * ack, 5 the host's.
 */
#define T6050_PORT_LINKSTS		0x208
#define   T6050_PORT_LINKSTS_UP		BIT(0)
#define T6050_PHY_LANE_CFG_REFCLKCG	BIT(31)

static void t6050_sleep_n1_down(struct apple_sleep *s, bool perst_alone)
{
	static const u32 ps[] = { 0x118, 0x128, 0x148, 0x150 };	/* APCIE_GP, _ST0, _SYS_GP, _SYS_ST0 */
	struct apple_sleep_region_state *r = s->region;
	void __iomem *port = r->n1_port, *phy = r->n1_phy;
	void __iomem *perst = r->ap_gpio && r->n1_perst ? r->ap_gpio + 4 * r->n1_perst : NULL;
	u32 v, held[ARRAY_SIZE(ps)] = {};
	unsigned int i;

	r->n1_held = 0;
	r->n1_state = 1;
	if (!perst)
		return;
	if (!r->n1_ecam || !port || !phy || !r->dart[T6050_DART_APCIE0] || !r->gapf ||
	    !r->link_down.n)
		goto perst_only;
	r->n1_state = 2;
	for (i = 0; i < ARRAY_SIZE(ps); i++) {
		void __iomem *reg = sleep_power_reg(s, SLEEP_REGION_WORD(T6050_PS1, ps[i]));

		v = readl(reg);
		if (FIELD_GET(PS_ACTUAL, v))
			continue;
		if (!(v & PS_AUTO_ENABLE) || !FIELD_GET(PS_TARGET, v))
			goto perst_only;
		held[i] = v;
		writel((v & ~PS_AUTO_ENABLE) | PS_TARGET, reg);
		if (readl_poll_timeout_atomic(reg, v, FIELD_GET(PS_ACTUAL, v) == 0xf, 10, 5000))
			goto perst_only;
	}
	r->n1_state = 3;
	if (!(readl(port + T6050_PORT_LINKSTS) & T6050_PORT_LINKSTS_UP))
		goto perst_only;
	r->n1_state = t6050_ops_run(s, &r->link_down) ? 4 : 0;
	t6050_pin_low(perst);
	writel(readl(phy) & ~T6050_PHY_LANE_CFG_REFCLKCG, phy);
	r->n1_gapf = readl(r->gapf + t6050_darts[T6050_DART_APCIE0].slice);
	t6050_sleep_dart_stop(s, T6050_DART_APCIE0);
	readl(r->dart[T6050_DART_APCIE0] + T6050_DART_EXCEPTION);
	t6050_ops_run(s, &r->link_down_port);
	goto unhold;

perst_only:
	if (perst_alone)
		t6050_pin_low(perst);
unhold:
	for (i = 0; i < ARRAY_SIZE(ps); i++)
		if (held[i]) {
			writel(held[i], sleep_power_reg(s, SLEEP_REGION_WORD(T6050_PS1, ps[i])));
			r->n1_held |= BIT(i);
		}
}

static bool t6050_n1_host_bound(struct apple_sleep *s)
{
	struct platform_device *pdev = s->region->radio_host;

	return pdev && device_is_bound(&pdev->dev);
}

/*
 * From the radio host driver's noirq suspend, once its functions are quiet and its DART is saved
 * and gated: the same link shutdown, before every step of the commit. An error means nothing
 * was written.
 */
int apple_sleep_radio_port_down(void)
{
	struct apple_sleep *s = apple_sleep;
	struct apple_sleep_region_state *r = s ? s->region : NULL;

	if (!r || !r->n1_host || !r->ap_gpio)
		return -ENODEV;
	t6050_sleep_n1_down(s, false);
	if (r->n1_state && r->n1_state != 4)
		return -EIO;
	/* The wake puts PERST# back as the shutdown left it, low, as macOS restores it */
	if (s->pins_saved_valid && s->npins > r->n1_perst && !test_bit(r->n1_perst, s->pins_skip))
		s->pins_saved[r->n1_perst] = readl(r->ap_gpio + 4 * r->n1_perst);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sleep_radio_port_down);

/*
 * For the radio host's noirq resume: 0 once the root complex runs again (or never stopped),
 * else nothing behind it may be read. A sleep abandoned after the shutdown lost no power: the
 * DART's slice goes back, and the host resets the DART itself.
 */
int apple_sleep_radio_rc_ready(void)
{
	struct apple_sleep *s = apple_sleep;
	struct apple_sleep_region_state *r = s ? s->region : NULL;

	if (!r || !r->n1_host)
		return 0;
	if (!r->committed && r->gapf && (!r->n1_state || r->n1_state == 4))
		writel(r->n1_gapf, r->gapf + t6050_darts[T6050_DART_APCIE0].slice);
	return r->n1_up_ret;
}
EXPORT_SYMBOL_GPL(apple_sleep_radio_rc_ready);

/*
 * The shared PCIe PHY and the root complex after a real sleep, as the boot loader brings them
 * up: each block's power word on, held if automatic, before any read; the root complex's +0x4
 * cleared; the DT's apple,radio-phy-wake (the PHY's tunables and its start); then the root
 * complex's pre-run word, its run bit and ready.
 */
#define T6050_RC_CTL			0x50
#define   T6050_RC_CTL_RUN		BIT(0)
#define T6050_RC_PRE_RUN		0x54
#define T6050_RC_STAT			0x58
#define   T6050_RC_STAT_READY		BIT(0)

static int t6050_n1_unpark(struct apple_sleep *s)
{
	/* APCIE_GP, APCIE_SYS_GP, APCIE_PHY_SW, APHY_GP_AUS5_A, APHY_GP_AUS5 */
	static const u32 ps[] = { 0x118, 0x148, 0x1b0, 0x258, 0x280 };
	struct apple_sleep_region_state *r = s->region;
	void __iomem *rc = r->n1_rc;
	u32 v;
	unsigned int i;
	bool running;

	if (!rc || !r->phy_wake.n)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(ps); i++) {
		void __iomem *reg = sleep_power_reg(s, SLEEP_REGION_WORD(T6050_PS1, ps[i]));

		v = readl(reg);
		if (FIELD_GET(PS_ACTUAL, v) == 0xf)
			continue;
		writel((v & ~PS_AUTO_ENABLE) | PS_TARGET, reg);
		if (readl_poll_timeout_atomic(reg, v, FIELD_GET(PS_ACTUAL, v) == 0xf, 10, 5000))
			return -EIO;
	}
	/* Still running after a commit that lost no power: only the park is undone then */
	running = readl(rc + T6050_RC_CTL) & T6050_RC_CTL_RUN;
	if (!running)
		writel(0, rc + 0x4);
	if (t6050_ops_run(s, &r->phy_wake) && !running)
		return -ETIMEDOUT;
	if (!running) {
		writel(0x140, rc + T6050_RC_PRE_RUN);
		writel(readl(rc + T6050_RC_CTL) | T6050_RC_CTL_RUN, rc + T6050_RC_CTL);
	}
	return readl_poll_timeout_atomic(rc + T6050_RC_STAT, v, v & T6050_RC_STAT_READY, 10,
					 250000);
}

/* The radio host's port wake steps from the DT: 0 after the port's set-up, 1 after its DART's. */
void apple_sleep_radio_port_wake(unsigned int step)
{
	struct apple_sleep *s = apple_sleep;
	struct apple_sleep_region_state *r = s ? s->region : NULL;

	if (r && r->n1_host)
		t6050_ops_run(s, step ? &r->dart_wake : &r->port_wake);
}
EXPORT_SYMBOL_GPL(apple_sleep_radio_port_wake);

/* Right before SEP's sleep request: SEP's DART, interrupts and slice only. */
/*
 * After GFX/GFX1's commit stop, before SEP. MCC +0x10: macOS releases it 1.2 ms after its GPU
 * stop, from 0xf, and polls 0x10 -> 0. With the GPU's
 * firmware still up the release doesn't complete. Past the point of no return: reported, never refused.
 */
static void t6050_sleep_mcc_release_10(struct apple_sleep *s)
{
	void __iomem *reg = s->cluster + 0x10;
	u64 start = __arch_counter_get_cntpct();
	u32 before = readl(reg), value;
	int ret;

	/* Released at the stage: only polled here, and written only if it isn't pending. */
	if (before && before != 0x10)
		writel(0, reg);
	ret = readl_poll_timeout_atomic(reg, value, !value, 1, 5000);
	dev_dbg(s->dev, "MCC +0x10 after the GPU stop: %x -> %x%s in %llu us\n", before, value,
		 ret ? " (NOT down)" : "",
		 (__arch_counter_get_cntpct() - start) * USEC_PER_SEC / arch_timer_get_cntfrq());
}

static void t6050_sleep_before_enclave(struct apple_sleep *s)
{
	t6050_sleep_mcc_release_10(s);
	s->region->dart_done = s->region->dart_timeouts = 0;
	t6050_sleep_dart_stop(s, T6050_DART_SEP);
}

/* Final returning checks: no last_stop/enclave action has happened yet. */
static int t6050_sleep_final_check(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	u8 ticks[8] = {};
	u64 now, delay, counter;
	u32 compare;
	int ret;

	if (!r->ptd_owned || !s->smc.held || !s->smc.saved_ops ||
	    !s->smc.saved_ops->check || !s->smc.saved_ops->commit_notice) {
		dev_info(s->dev, "final check: PTD owned %d, SMC held %d\n", r->ptd_owned, s->smc.held);
		return -EBUSY;
	}
	ret = s->smc.saved_ops->check(s->smc.data);
	if (ret) {
		dev_info(s->dev, "final check: SMC check %d\n", ret);
		return ret;
	}
	r->complex_observed = readl(s->cluster + 0xc);
	ret = t6050_sleep_plan_complex_request(r->complex_observed, &r->complex_request);
	if (ret)
		return ret;
	/* Returning RTC admission precedes SEP. The real AP notice follows peer
	 * stops; schedule is anchored again to its actual request/ACK afterward.
	 */
	now = __arch_counter_get_cntpct();
	s->timer_at = now + arch_timer_get_cntfrq() *
		max(48000U, s->soc->smc_policy->idle_to_timer_us) / USEC_PER_SEC;
	ret = t6050_sleep_check_alarm(s);
	if (ret)
		return ret;
	ret = t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_READL, 0x2002, NULL, ticks, 6);
	if (ret)
		return ret;
	counter = get_unaligned_le64(ticks) >> 16; /* Pinned Abbey RTC counter units. */
	compare = get_unaligned_le32(r->alarm_compare);
	delay = div_u64((s->timer_at - now) * USEC_PER_SEC, arch_timer_get_cntfrq());
	if (compare <= counter + DIV_ROUND_UP_ULL(delay, USEC_PER_SEC) + 2)
		return -ETIME;
	/* PMU 0xf801 = 0x50 is macOS's after ANS: t6050_sleep_after_ans. */
	return 0;
}

static void t6050_sleep_prepare_regs(struct apple_sleep *s)
{
	/* Measured watchdog counts before the irreversible complex sequence. */
	writel(0, s->wdt + WDT_WD0_COUNT);
	writel(0, s->wdt + WDT_WD1_COUNT);
}

static void t6050_sleep_schedule(struct apple_sleep *s)
{
	u64 now = __arch_counter_get_cntpct();

	/*
	 * Last-stop/enclave latency cannot consume the runway: the cluster step needs about 45 ms
	 * before the timer, the two raises before it included.
	 */
	s->timer_at = max(now + arch_timer_get_cntfrq() * 48000ULL / USEC_PER_SEC,
		s->smc_idle_sent + arch_timer_get_cntfrq() *
		s->soc->smc_policy->idle_to_timer_us / USEC_PER_SEC);
}

/* This callback may return an error only to the common fatal path.
 * No newly pending wake/alarm can become a returning post-SEP veto here.
 */
static int t6050_sleep_commit_notice(struct apple_sleep *s)
{
	int ret;

	/* PMP0's DART right after the PMP's run bit, the last stop of the second pass. */
	t6050_sleep_dart_stop(s, T6050_DART_PMP0);
	ret = apple_sleep_smc_commit_notice(s);

	if (ret)
		return ret;
	if (!s->smc_idle_sent || s->smc_idle_ack < s->smc_idle_sent)
		return -EPROTO;
	/* SIO's DART 2.3 ms after the SMC's AP IDLE ack, the last write before the window. */
	t6050_sleep_dart_stop(s, T6050_DART_SIO);
	t6050_sleep_schedule(s);
	return 0;
}

static void t6050_sleep_ptd_drop(struct apple_sleep *s, unsigned int bit, unsigned int times)
{
	u64 request = readq(s->ptd + M5_PTD_READ) & ~BIT_ULL(bit);

	while (times--)
		writeq(request, s->ptd + M5_PTD_WRITE);
}

static void t6050_sleep_wait_before(struct apple_sleep *s, u32 before_us)
{
	u64 target = s->timer_at - arch_timer_get_cntfrq() * before_us / USEC_PER_SEC;

	while (__arch_counter_get_cntpct() < target)
		cpu_relax();
}

/*
 * A cluster's CPM after its power-up, as XNU's runtime rounds: the kept counters back, then
 * the table, the reads, and +0x4180 = 3.
 */
#define T6050_CPM_COUNTER0	0x8000
#define T6050_CPM_COUNTER1	0x8008
enum { T6050_CPM_WRITE, T6050_CPM_READ, T6050_CPM_REWRITE };

static void t6050_cpm_ops(void __iomem *cpm, const u64 *ops, unsigned int n)
{
	for (; n; n--, ops += 3) {
		void __iomem *reg = cpm + ops[1];

		if (ops[0] == T6050_CPM_WRITE)
			writeq(ops[2], reg);
		else if (ops[0] == T6050_CPM_READ)
			readq(reg);
		else
			writeq(readq(reg), reg);
	}
}

/* A cluster's CPM after its power-up: its kept counters back, then the DT's re-arm. */
static void t6050_cpm_rearm(struct apple_sleep *s, void __iomem *cpm, const u64 *saved,
			    unsigned int cl)
{
	struct apple_sleep_region_state *r = s->region;

	writeq(saved[0], cpm + T6050_CPM_COUNTER0);
	writeq(saved[1], cpm + T6050_CPM_COUNTER1);
	t6050_cpm_ops(cpm, r->cpm_rearm, r->ncpm_rearm);
	if (!cl)
		t6050_cpm_ops(cpm, r->cpm_rearm_boot, r->ncpm_rearm_boot);
}

static int t6050_cpm_prop(struct apple_sleep *s, const char *name, u64 **ops, unsigned int *n)
{
	int len = of_property_count_u64_elems(s->dev->of_node, name);

	*n = 0;
	if (len <= 0)
		return 0;
	if (len % 3)
		return -EINVAL;
	*ops = devm_kcalloc(s->dev, len, sizeof(u64), GFP_KERNEL);
	if (!*ops)
		return -ENOMEM;
	if (of_property_read_u64_array(s->dev->of_node, name, *ops, len))
		return -EINVAL;
	*n = len / 3;
	return 0;
}


/*
 * The clusters' runtime rounds before the commit, as macOS makes them: a cluster's cores go
 * down, its request is released and raised again, its CPM re-armed and its cores restarted
 * into the holding pen, where the commit's stop bits find them. The boot cluster's round runs
 * with the boot core in cpu_suspend(), driven by a helper core in cluster 2.
 */
#define T6050_HELPER_MPIDR	0x10201

static u32 t6050_c0_sync;
static atomic_t t6050_c0_active;
static struct cpumask t6050_c0_off;
static u64 t6050_c0_deep_saved;
/* Cores a round restarted into the holding pen */
static u32 t6050_restarted;

#define SYS_APL_S3_7_C15_C5_0	sys_reg(3, 7, 15, 5, 0)
#define SYS_APL_S3_1_C15_C0_3	sys_reg(3, 1, 15, 0, 3)
#define SYS_APL_S3_1_C15_C1_3	sys_reg(3, 1, 15, 1, 3)
#define SYS_APL_S3_1_C15_C2_3	sys_reg(3, 1, 15, 2, 3)
#define SYS_APL_S3_4_C15_C10_4	sys_reg(3, 4, 15, 10, 4)
#define SYS_APL_S3_1_C15_C7_4	sys_reg(3, 1, 15, 7, 4)

/* A round's cores back into the pen, the deep-sleep WFI armed for after the commit */
static void t6050_restart_cores(struct apple_sleep *s, unsigned int cl, u32 cores)
{
	unsigned int core, c;

	spin_table_pen_close();
	spin_table_pen_deep_arm(true);
	for (core = 0; core < 6; core++) {
		if (!(cores & BIT(core)))
			continue;
		writel(BIT(6 * cl + core), s->cpu_start + CPU_START_SYS);
		for (c = 0; c < 3; c++)
			writel(c == cl ? BIT(core) : 0, s->cpu_start + CPU_START_CLUSTER(c));
	}
	t6050_restarted |= cores << (6 * cl);
}

/* On the helper core, interrupts off, from its hold loop: the boot cluster's round. */
static void t6050_c0_helper(void *data)
{
	struct apple_sleep *s = data;
	struct apple_sleep_region_state *r = s->region;
	void __iomem *reg = s->cluster, *cpm = r->cpm[0];
	u64 saved[2] = {}, siq, t0, rel;
	unsigned int core;
	u32 v = 0;

	/* This core takes the AIC's interrupts while cluster 0's cores have opted out. */
	siq = read_sysreg_s(SYS_APL_SIQ_CFG_EL1);
	write_sysreg_s(siq & ~3ULL, SYS_APL_SIQ_CFG_EL1);
	isb();
	t0 = __arch_counter_get_cntpct();
	while (atomic_read(&t6050_c0_active) > 0 &&
	       __arch_counter_get_cntpct() - t0 < arch_timer_get_cntfrq() / 10)
		cpu_relax();
	if (cpm) {
		readq(cpm + 0x4180);
		writeq(0, cpm + 0x4180);
		saved[0] = readq(cpm + 0x8000);
		saved[1] = readq(cpm + 0x8008);
	}
	writel(readl(s->pwrgate + 0x120), s->pwrgate + 0x120);
	writel(readl(s->pwrgate + 0x128), s->pwrgate + 0x128);
	writel(readl(s->pwrgate + 0x130), s->pwrgate + 0x130);
	t6050_sleep_ptd_drop(s, 12, 1);
	writel(0, reg);
	rel = __arch_counter_get_cntpct();
	udelay(17);
	readl_poll_timeout_atomic(reg, v, !v || v == 0x10, 1, 200);
	WRITE_ONCE(t6050_c0_sync, 2);
	dsb(ish);
	for (core = 0; core < 6; core++)
		readl_poll_timeout_atomic(sleep_region_reg(s, SLEEP_REGION_WORD(T6050_PS0, 8 * core)),
					  v, !FIELD_GET(PS_ACTUAL, v), 10, 30000);
	while (__arch_counter_get_cntpct() - rel < arch_timer_get_cntfrq() * 32 / MSEC_PER_SEC)
		cpu_relax();
	v = readl(reg);
	writel(v ? 0x1f : 0xf, reg);
	if (!readl_poll_timeout_atomic(reg, v, v == 0x2f, 10, 3000)) {
		writel(0x2f, reg);
		readl_poll_timeout_atomic(reg, v, v == 0xf, 1, 500);
		writeq(readq(s->ptd + M5_PTD_READ) | BIT_ULL(12), s->ptd + M5_PTD_WRITE);
	}
	if (cpm) {
		t6050_cpm_rearm(s, cpm, saved, 0);
	}
	/* Core 0's start writes, then cores 1-5 back into the pen */
	writel(BIT(0), s->cpu_start + CPU_START_SYS);
	writel(BIT(0), s->cpu_start + CPU_START_CLUSTER(0));
	writel(0, s->cpu_start + CPU_START_CLUSTER(1));
	writel(0, s->cpu_start + CPU_START_CLUSTER(2));
	t6050_restart_cores(s, 0, GENMASK(5, 1));
	write_sysreg_s(siq, SYS_APL_SIQ_CFG_EL1);
	isb();
}

/* A cluster-0 core's power-down in the round, as XNU's: the last one leaves its SIQ alone. */
static void __noreturn t6050_c0_down(bool last, bool boot)
{
	u64 v, t0;

	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntv_ctl_el0);
	write_sysreg_s(0, SYS_APL_S3_7_C15_C5_0);
	v = read_sysreg_s(SYS_APL_S3_1_C15_C0_3);
	write_sysreg_s(v, SYS_APL_S3_1_C15_C0_3);
	read_sysreg_s(SYS_APL_S3_1_C15_C0_3);
	write_sysreg_s(0, SYS_APL_S3_1_C15_C0_3);
	v = read_sysreg_s(SYS_APL_S3_1_C15_C1_3);
	write_sysreg_s(v, SYS_APL_S3_1_C15_C1_3);
	read_sysreg_s(SYS_APL_S3_1_C15_C1_3);
	write_sysreg_s(0, SYS_APL_S3_1_C15_C1_3);
	v = read_sysreg_s(SYS_APL_S3_1_C15_C2_3);
	write_sysreg_s(v, SYS_APL_S3_1_C15_C2_3);
	read_sysreg_s(SYS_APL_S3_1_C15_C2_3);
	write_sysreg_s(0, SYS_APL_S3_1_C15_C2_3);
	dsb(ish);
	dsb(sy);
	v = read_sysreg_s(SYS_APL_S3_4_C15_C10_4);
	if (!last) {
		write_sysreg_s((v & ~3ULL) | 3, SYS_APL_S3_4_C15_C10_4);
		isb();
		write_sysreg_s(0, SYS_APL_S3_1_C15_C7_4);
	}
	t0 = __arch_counter_get_cntpct();
	while (READ_ONCE(t6050_c0_sync) < 2 &&
	       __arch_counter_get_cntpct() - t0 < arch_timer_get_cntfrq() / 50)
		cpu_relax();
	if (last) {
		v = read_sysreg_s(SYS_APL_DEEP_SLEEP);
		if (boot) {
			t6050_c0_deep_saved = v;
			dcache_clean_poc((unsigned long)&t6050_c0_deep_saved,
					 (unsigned long)(&t6050_c0_deep_saved + 1));
		}
		write_sysreg_s(v | DEEP_SLEEP_ENABLE, SYS_APL_DEEP_SLEEP);
	}
	while (true) {
		if (boot)
			write_sysreg_s(IPI_SR_PENDING, SYS_APL_IPI_SR_EL1);
		dsb(sy);
		isb();
		wfi();
	}
}

/*
 * A stopped core after the commit, as macOS's parked cores: its interrupts opted out, the
 * deep-sleep bits set, then WFI, powered until the sleep engine takes the cluster down.
 */
static void __noreturn t6050_cpu_down(struct apple_sleep *s, u64 mpidr)
{
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntp_ctl_el0);
	write_sysreg(ARCH_TIMER_CTRL_IT_MASK, cntv_ctl_el0);
	isb();
	write_sysreg_s(read_sysreg_s(SYS_APL_S3_4_C15_C10_4) & ~3ULL, SYS_APL_S3_4_C15_C10_4);
	isb();
	write_sysreg_s(1, SYS_APL_S3_7_C15_C5_0);
	isb();
	write_sysreg_s(read_sysreg_s(SYS_APL_DEEP_SLEEP) | DEEP_SLEEP_ENABLE, SYS_APL_DEEP_SLEEP);
	isb();
	for (;;) {
		write_sysreg_s(IPI_SR_PENDING, SYS_APL_IPI_SR_EL1);
		dsb(sy);
		isb();
		wfi();
	}
}

/* Cores 1-5, parked after cpu0's stop bit: their stop bit is written, then XNU's path */
static void __noreturn t6050_idle_wfi(void)
{
	t6050_c0_down(atomic_dec_return(&t6050_c0_active) == 0, false);
}

static int t6050_c0_finisher(unsigned long data)
{
	struct apple_sleep *s = (struct apple_sleep *)data;
	bool last;

	sleep_cpu_stop(s->mbox->boot_mpidr);
	last = atomic_dec_return(&t6050_c0_active) == 0;
	spin_table_park_off(&t6050_c0_off);
	t6050_c0_down(last, true);
	return 0;
}

/* The boot core's registers as XNU's reset path leaves them, after its round */
static void t6050_reset_path_restore(void)
{
	write_sysreg_s(0, sys_reg(3, 1, 15, 1, 5));
	write_sysreg_s(3, sys_reg(3, 4, 15, 14, 6));
	isb();
	write_sysreg_s(0x100, sys_reg(3, 4, 15, 1, 4));
	isb();
	write_sysreg(0x2000, actlr_el1);
	isb();
	write_sysreg_s(0, SYS_APL_S3_4_C15_C10_4);
	write_sysreg_s(read_sysreg_s(sys_reg(3, 4, 15, 9, 6)), sys_reg(3, 4, 15, 9, 6));
	write_sysreg_s(0x1c000000, sys_reg(3, 5, 15, 1, 6));
	write_sysreg_s(2, sys_reg(3, 1, 15, 13, 4));
	write_sysreg_s(0xa, sys_reg(3, 1, 15, 15, 4));
	write_sysreg_s(1, SYS_APL_S3_7_C15_C5_0);
	isb();
}

static void t6050_cl_round(struct apple_sleep *s, unsigned int cl);

/* Once, with the other CPUs held: the boot cluster's round, then clusters 1 and 2. */
static void t6050_boot_cluster_cycle(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	u64 boot = s->mbox->boot_mpidr, siq, cntkctl;
	unsigned int cpu, helper = nr_cpu_ids;
	struct cpumask off;
	int ret, us;

	ret = spin_table_hold_parked();
	if (ret) {
		dev_warn(s->dev, "boot cluster round: hold %d, not run\n", ret);
		return;
	}
	cpumask_clear(&off);
	for_each_present_cpu(cpu) {
		u64 m = cpu_logical_map(cpu);

		if (m != boot && (m & ~0xffULL) == (boot & ~0xffULL))
			cpumask_set_cpu(cpu, &off);
		if (m == T6050_HELPER_MPIDR)
			helper = cpu;
	}
	if (helper >= nr_cpu_ids || cpumask_weight(&off) != 5) {
		dev_warn(s->dev, "boot cluster round: no helper or not five cores\n");
		spin_table_unhold_parked();
		return;
	}
	/* The boot core takes the held cores' power-down control, put back after the round */
	ret = s->soc->ops->cpu_prepare(boot);
	if (ret) {
		dev_warn(s->dev, "boot cluster round: core control %d, not run\n", ret);
		spin_table_unhold_parked();
		return;
	}
	r->cycle_ran = true;
	t6050_restarted = 0;
	cpumask_copy(&t6050_c0_off, &off);
	atomic_set(&t6050_c0_active, 6);
	t6050_c0_deep_saved = read_sysreg_s(SYS_APL_DEEP_SLEEP);
	WRITE_ONCE(s->mbox->resume_pa, __pa_symbol(cpu_resume));
	dcache_clean_poc((unsigned long)s->mbox, (unsigned long)(s->mbox + 1));
	dsb(sy);
	cntkctl = read_sysreg(cntkctl_el1);
	siq = read_sysreg_s(SYS_APL_SIQ_CFG_EL1);
	WRITE_ONCE(t6050_c0_sync, 0);
	spin_table_call(helper, t6050_c0_helper, s);
	ret = cpu_suspend((unsigned long)s, t6050_c0_finisher);
	write_sysreg_s(t6050_c0_deep_saved, SYS_APL_DEEP_SLEEP);
	t6050_reset_path_restore();
	write_sysreg(cntkctl, cntkctl_el1);
	isb();
	write_sysreg_s(siq, SYS_APL_SIQ_CFG_EL1);
	s->soc->ops->cpu_restore(boot);
	for (us = 0; us < 100000 && !spin_table_call_done(); us++)
		udelay(1);
	dev_dbg(s->dev, "boot cluster round: %d\n", ret);
	t6050_cl_round(s, 1);
	t6050_cl_round(s, 2);
}

/* A round on cluster 1 or 2, from the boot core: its six cores parked off by the hold. */
static void t6050_cl_round(struct apple_sleep *s, unsigned int cl)
{
	struct apple_sleep_region_state *r = s->region;
	void __iomem *reg = s->cluster + 4 * cl, *cpm = r->cpm[cl];
	static struct cpumask off;
	u64 saved[2] = {}, t0, rel, f = arch_timer_get_cntfrq();
	unsigned int cpu, core;
	u32 v = 0;

	cpumask_clear(&off);
	for_each_present_cpu(cpu)
		if (cpu_logical_map(cpu) != s->mbox->boot_mpidr &&
		    sleep_cpu_cluster(s, cpu_logical_map(cpu)) == cl)
			cpumask_set_cpu(cpu, &off);
	if (cpumask_weight(&off) != 6) {
		dev_warn(s->dev, "cluster %u round: %u cores, not run\n", cl, cpumask_weight(&off));
		return;
	}
	atomic_set(&t6050_c0_active, 6);
	WRITE_ONCE(t6050_c0_sync, 0);
	dsb(ish);
	spin_table_park_off(&off);
	t0 = __arch_counter_get_cntpct();
	while (atomic_read(&t6050_c0_active) > 0 && __arch_counter_get_cntpct() - t0 < f / 10)
		cpu_relax();
	if (cpm) {
		readq(cpm + 0x4180);
		writeq(0, cpm + 0x4180);
		saved[0] = readq(cpm + 0x8000);
		saved[1] = readq(cpm + 0x8008);
	}
	writel(readl(s->pwrgate + 0x120), s->pwrgate + 0x120);
	writel(readl(s->pwrgate + 0x128), s->pwrgate + 0x128);
	writel(readl(s->pwrgate + 0x130), s->pwrgate + 0x130);
	t6050_sleep_ptd_drop(s, 12 + cl, 1);
	writel(0, reg);
	rel = __arch_counter_get_cntpct();
	udelay(17);
	readl_poll_timeout_atomic(reg, v, !v || v == 0x10, 1, 200);
	WRITE_ONCE(t6050_c0_sync, 2);
	dsb(ish);
	for (core = 0; core < 6; core++)
		readl_poll_timeout_atomic(sleep_region_reg(s, SLEEP_REGION_WORD(T6050_PS0, 8 * (6 * cl + core))),
					  v, !FIELD_GET(PS_ACTUAL, v), 10, 30000);
	while (__arch_counter_get_cntpct() - rel < f * 20 / MSEC_PER_SEC)
		cpu_relax();
	v = readl(reg);
	writel(v ? 0x1f : 0xf, reg);
	if (!readl_poll_timeout_atomic(reg, v, v == 0x2f, 10, 3000)) {
		writel(0x2f, reg);
		readl_poll_timeout_atomic(reg, v, v == 0xf, 1, 500);
		writeq(readq(s->ptd + M5_PTD_READ) | BIT_ULL(12 + cl), s->ptd + M5_PTD_WRITE);
	}
	if (cpm)
		t6050_cpm_rearm(s, cpm, saved, cl);
	t6050_restart_cores(s, cl, GENMASK(5, 0));
}

/* The cores the rounds restarted wait in the pen: their stop bits at the commit, one each. */
static void t6050_sleep_stop_restarted(struct apple_sleep *s)
{
	unsigned int core;

	if (!s->region->cycle_ran)
		return;
	for (core = 1; core < 18; core++)
		if (t6050_restarted & BIT(core))
			writel(BIT(core), s->cpu_start + CPU_START_STOP);
	dsb(sy);
}

static void t6050_mcc_release(struct apple_sleep *s, unsigned int i)
{
	struct apple_sleep_region_state *r = s->region;
	void __iomem *reg = s->cluster + (i ? 8 : 4);
	void __iomem *ack = s->cluster + (i ? 0x1c : 0x18);
	u32 *c = r->mcc_cycle[i], v;

	t6050_sleep_ptd_drop(s, i ? 14 : 13, 1);
	writel(0, reg);
	readl_poll_timeout_atomic(reg, v, !v || v == 0x10, 1, 200);
	c[8] = v;
	c[9] = readl(ack);
}

/*
 * PTD's three drops come before the three pwrgate rounds. The Neo's commit writes its CPM
 * gates around its PTD writes; the M5's order is its own.
 */
static const u32 t6050_ptd_drop_us[] = { 2491, 2296, 2202 };

/*
 * The MCC channel stop, after the cluster words, each access at its width:
 * - mcc reg[22..25]: for each set bit of +0x584, lowest first, the 64-bit counter at +0x4000 + 8n
 *   (n = the step), then +0x584 read and written with that bit cleared, down to 0. reg[26]'s +0x584
 *   isn't touched.
 * - Then lanes 0-1 on every channel, then lanes 2-3: the 64-bit counter at +0x4050 + 8n, +0x58c
 *   (lanes 0-1) or +0x590 (2-3) rewritten, +0x588 strobe bit 16 + 4n set, then bit 4n cleared. reg[26]
 *   has lanes 0 and 2 only.
 * The counters fault on 32-bit reads: only 64-bit ones, as macOS's.
 */
static const u8 t6050_mcc_lanes[5] = { 0xf, 0xf, 0xf, 0xf, 0x5 };

static void t6050_mcc_stop(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int ch, pass, n, k;
	u32 v;

	for (ch = 0; ch < 4; ch++) {
		void __iomem *m = r->mcc[ch];

		if (!m)
			continue;
		for (k = 0; k < 32; k++) {
			readq(m + 0x4000 + 8 * k);
			v = readl(m + 0x584);
			if (!v)
				break;
			v &= v - 1;
			writel(v, m + 0x584);
			if (!v)
				break;
		}
	}
	for (pass = 0; pass < 2; pass++)
		for (ch = 0; ch < ARRAY_SIZE(t6050_mcc_lanes); ch++) {
			void __iomem *m = r->mcc[ch];
			u32 mask = pass ? 0x590 : 0x58c;

			if (!m)
				continue;
			for (n = 2 * pass; n < 2 * pass + 2; n++) {
				if (!(t6050_mcc_lanes[ch] & BIT(n)))
					continue;
				readq(m + 0x4050 + 8 * n);
				writel(readl(m + mask), m + mask);
				writel(readl(m + 0x588) | BIT(16 + 4 * n), m + 0x588);
				writel(readl(m + 0x588) & ~BIT(4 * n), m + 0x588);
			}
		}
}

/* clkgen page 0's four mux words, parked right before the commit: [26:24] cleared */
static const u16 t6050_mux_park[] = { 0xa0, 0xa4, 0x94, 0x90 };
static const u32 t6050_pwrgate_us[] = { 1552, 1468, 1382 };


static void t6050_sleep_commit_regs(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	const struct apple_sleep_region_profile *p = s->soc->regions;
	unsigned int i, j;
	bool media78 = false;
	u32 value;

	t6050_sleep_stop_restarted(s);
	r->committed = true;
	/*
	 * In the sleep entry macOS only releases +0x4 and +0x8, each after pwrgate's three R/W: no
	 * raise, CPM disarm or re-arm, and its cores stay powered.
	 */
	for (i = 0; i < 2; i++) {
		t6050_sleep_wait_before(s, i ? 40900 : 41700);
		readl(s->pwrgate + 0x120);
		writel(PWRGATE_ON, s->pwrgate + 0x120);
		readl(s->pwrgate + 0x128);
		writel(PWRGATE_ON, s->pwrgate + 0x128);
		readl(s->pwrgate + 0x130);
		writel(PWRGATE_ON, s->pwrgate + 0x130);
		t6050_mcc_release(s, i);
	}
	/* Only after ANS's registered driver stop. Never stop ANS here. */
	t6050_sleep_wait_before(s, 39800);
	if (s->pcie_ans_stopped)
		t6050_storage_phy_park(s);
	t6050_sleep_wait_before(s, 24700);
	if (s->pcie_ans_stopped) {
		for (i = 0; i < ARRAY_SIZE(t6050_ans_storage_ps); i++) {
			u32 word = SLEEP_REGION_WORD(T6050_PS1, t6050_ans_storage_ps[i]);

			if (t6050_ans_storage_ps[i] == T6050_PS1_APCIE_PHY_SW)
				t6050_storage_phy_down(s);
			sleep_ps_lower(s, word);
			/* Diagnostic only: a final-stop error cannot return to callers. */
			if (sleep_ps_wait_off(s, word))
				s->ans_off_ret = -ETIMEDOUT;
		}
	}
	t6050_sleep_wait_before(s, 23300);
	writel(0x20, s->wdt_aux + 0x2c);
	writel(0, s->wdt + WDT_WD0_CTL);
	apple_sleep_set_wd2(s, T6050_WDT_COMMIT_SECS);
	writel(0, s->wdt_aux + 8);
	/* the CLPC voter W 0 (no read) 1.6 ms after the watchdog writes */
	t6050_sleep_wait_before(s, 22300);
	s->region->floor_changed |= BIT(1);
	writel(0, s->pmc_voters + M5_PMC_STRIDE * 1);
	/* the MCC stop, ending about 1 ms before the clock words */
	t6050_sleep_wait_before(s, 20300);
	t6050_mcc_stop(s);
	/* clkgen +0x78000 R 0x1, W 0, 8 us before the clock words */
	t6050_sleep_wait_before(s, 18010);
	if (r->clkgen78) {
		readl(r->clkgen78);
		writel(0, r->clkgen78);
	}
	t6050_sleep_wait_before(s, 18000);
	for (i = 0; i < p->entry_clk.count; i++) {
		void __iomem *reg = sleep_region_reg(s, p->entry_clk.offset[i]);

		/* and the media die's, just before its own clock words */
		if (!media78 && (p->entry_clk.offset[i] >> 20) == T6050_MEDIA_CLK) {
			media78 = true;
			if (r->media_clkgen78) {
				readl(r->media_clkgen78);
				writel(0, r->media_clkgen78);
			}
		}
		writel(readl(reg) & ~BIT(0), reg);
	}
	/* PTD's three drops, then the three pwrgate rounds: see t6050_ptd_drop_us */
	for (i = 0; i < 3; i++) {
		t6050_sleep_wait_before(s, t6050_ptd_drop_us[i]);
		t6050_sleep_ptd_drop(s, 12, 1);
	}
	for (i = 0; i < 3; i++) {
		t6050_sleep_wait_before(s, t6050_pwrgate_us[i]);
		/* CPMn's counters read, then pwrgate's three R/W */
		if (r->cpm[i]) {
			readq(r->cpm[i] + 0x8000);
			readq(r->cpm[i] + 0x8008);
		}
		readl(s->pwrgate + 0x120);
		writel(PWRGATE_ON, s->pwrgate + 0x120);
		readl(s->pwrgate + 0x128);
		writel(PWRGATE_ON, s->pwrgate + 0x128);
		readl(s->pwrgate + 0x130);
		writel(PWRGATE_ON, s->pwrgate + 0x130);
	}
	t6050_sleep_wait_before(s, 950);
	for (i = 0; i < p->entry_perf.count; i++)
		writel(0, sleep_region_reg(s, p->entry_perf.offset[i]));
	/* the AP voter between the perf words and AFR: R, W 0, R +4 */
	s->region->floor_changed |= BIT(3);
	readl(s->pmc_voters + M5_PMC_STRIDE * 3);
	writel(0, s->pmc_voters + M5_PMC_STRIDE * 3);
	readl(s->pmc_voters + M5_PMC_STRIDE * 3 + 4);
	t6050_sleep_wait_before(s, 730);
	/* AFR: macOS reads 0x1000030f and writes 0x1004030f, then 0. */
	value = readl(r->base[T6050_PS_GPX] + 0x100);
	if (FIELD_GET(PS_TARGET, value)) {
		writel(0x1004030f, r->base[T6050_PS_GPX] + 0x100);
		writel(0, r->base[T6050_PS_GPX] + 0x100);
	}
	t6050_sleep_wait_before(s, 650);
	/* +0x0 = 0 (ack 0x100 -> 0x101), +0x4 = +0x8 = 0x10. */
	for (i = 0; i < 3; i++) {
		readl(s->cluster + 4 * i);
		writel(i ? 0x10 : 0, s->cluster + 4 * i);
	}
	/* The complex request the plan checked before anything irreversible */
	writel(r->complex_request, s->cluster + 0xc);
	r->late_errors += !!readl_poll_timeout_atomic(s->cluster + 0xc, value, !value, 1, 1000);
	writel(0, r->base[T6050_PERF] + 0x18000);
	readl(s->cluster + 0x10);
	writel(0, s->cluster + 0x10);
	t6050_sleep_wait_before(s, 550);
	writel(0x0400024f, r->base[T6050_PS1] + 0x290);
	writel(0x0400024f, r->base[T6050_PS1] + 0x2a0);
	/* right after SIO/SIO_CPU, the four mux parks, each R, W 0x80100400, R */
	if (r->clkgen) {
		for (i = 0; i < ARRAY_SIZE(t6050_mux_park); i++) {
			readl(r->clkgen + t6050_mux_park[i]);
			writel(0x80100400, r->clkgen + t6050_mux_park[i]);
			readl(r->clkgen + t6050_mux_park[i]);
		}
	}
	t6050_sleep_wait_before(s, 500);
	for (i = 0; i < p->ntables; i++)
		for (j = 0; j < p->table[i].count; j++) {
			void __iomem *reg = r->base[p->table[i].region] + j * p->table[i].stride;
			readl(reg);
			writel(0, reg);
		}
	t6050_sleep_wait_before(s, 0);
}

static void t6050_sleep_pre_commit(struct apple_sleep *s)
{
	t6050_sleep_schedule(s);
}

static void t6050_sleep_restore_display(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	int d, j;
	u32 saved, keep, now;
	void __iomem *cpu;

	for (d = s->soc->display_count - 1; d >= 0; d--) {
		if (!(r->display_lowered & BIT(d)))
			continue;
		for (j = 2; j >= 1; j--)
			r->late_errors += !!sleep_ps_restore(sleep_region_reg(s,
				s->soc->ps.disp[d][j]), r->display_saved[d][j]);
		cpu = sleep_region_reg(s, s->soc->ps.disp[d][0]);
		saved = r->display_saved[d][0];
		keep = (saved & ~(PS_AUTO_ENABLE | PS_AUTO_STATE | PS_PARENT_MISSING |
			PS_DEV_DISABLE | PS_FLAGS | PS_ACTUAL)) | (readl(cpu) & PS_AUTO_STATE);
		/* SYS, FE, CPU target, saved gate A/B, actual-state poll, AUTO_ENABLE. */
		writel(keep, cpu);
		if (!d) {
			writel(r->gate_saved[0], s->disp_gate + 0x51c);
			writel(r->gate_saved[1], s->disp_gate + 0x540);
			/* Restore result must reach the DCP's warm-start gate. */
			if (readl(s->disp_gate + 0x51c) != r->gate_saved[0] ||
			    readl(s->disp_gate + 0x540) != r->gate_saved[1])
				r->late_errors++;
		}
		r->late_errors += !!readl_poll_timeout_atomic(cpu, now,
			FIELD_GET(PS_ACTUAL, now) == FIELD_GET(PS_TARGET, saved), 1, 5000);
		if (saved & PS_AUTO_ENABLE)
			writel(keep | PS_AUTO_ENABLE, cpu);
	}
	r->display_lowered = 0;
	bitmap_zero(r->manager_down, ARRAY_SIZE(t6050_ps_all));
}

static void t6050_sleep_restore_pmc(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;

	if (!r->pmc_saved)
		return;
	for (i = 0; i < 5; i++)
		if (r->floor_changed & BIT(i))
			writel(r->floor[i], s->pmc_voters + M5_PMC_STRIDE * i);
	r->floor_changed = 0;
	for (i = 0; i < 4; i++)
		writel(r->rails[i], s->pmc_rails + 0x2000 + 4 * i);
	writel(r->rails[4], s->pmc_rails + 0x2028);
	writel(r->rails[5], s->pmc_rails + 0x2020);
	r->pmc_saved = false;
}

static void t6050_sleep_undo_regs(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;

	/* Never write +c on abort: the requests keep iBoot's values. */
	t6050_wake_disarm(s);
	if (r->staged && !r->committed)
		writel(r->park_saved, r->base[T6050_PERF] + 0x18000);
	t6050_sleep_restore_pmc(s);
	t6050_sleep_restore_display(s);
	r->staged = false;
	r->snapshot_valid = false;
}

static int t6050_ans_storage_restore(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(t6050_ans_storage_on); i++) {
		u32 word = SLEEP_REGION_WORD(T6050_PS1,
			t6050_ans_storage_ps[t6050_ans_storage_on[i]]);
		int n = t6050_ans_storage_index(s, word);
		void __iomem *reg;
		u32 value, keep;

		if (n < 0 || test_bit(n, r->driver_down) || test_bit(n, r->manager_down) ||
		    (r->saved[n] & PS_TARGET) != PS_TARGET)
			return -EIO;
		/* As macOS at the wake: forced on, polled, then automatic again. */
		reg = sleep_region_reg(s, word);
		keep = (r->saved[n] & ~(PS_AUTO_ENABLE | PS_AUTO_STATE | PS_PARENT_MISSING |
					PS_DEV_DISABLE | PS_FLAGS | PS_ACTUAL)) |
		       (readl(reg) & PS_AUTO_STATE);
		writel(keep, reg);
		if (readl_poll_timeout_atomic(reg, value, FIELD_GET(PS_ACTUAL, value) == 0xf, 1,
					      PS_OFF_TIMEOUT_US))
			ret = -ETIMEDOUT;
		if (r->saved[n] & PS_AUTO_ENABLE)
			writel(keep | PS_AUTO_ENABLE, reg);
	}
	return ret;
}

static void t6050_sleep_wake_regs(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	unsigned int i;
	u32 value;

	if (r->spmi_masked) {
		writel(r->spmi_mask[0], s->spmi + M5_SPMI_IRQ_EN);
		for (i = 0; i < ARRAY_SIZE(r->spmi_bus); i++)
			if (r->spmi_bus[i])
				writel(r->spmi_mask[1 + i], r->spmi_bus[i] + M5_SPMI_IRQ_EN);
		r->spmi_masked = false;
		r->spmi_post_timer = false;
	}

	if (r->aic_gfx) {
		r->gfx_aic[1] = readl(r->aic_gfx + 0x14);
		writel(r->gfx_aic[0], r->aic_gfx + 0x14);
		r->gfx_aic[2] = readl(r->aic_gfx + 0x14);
	}
	/* The complexes' requests back, in macOS's early wake order */
	writel(0, s->wdt + WDT_WD2_CTL);
	apple_sleep_set_wd2(s, WDT_SUSPEND_SECS);
	for (i = 0; i < 3; i++)
		writel(r->cluster_saved[i], s->cluster + 4 * i);
	writel(PWRGATE_ON, s->pwrgate);
	writel(PWRGATE_ON, s->pwrgate + 0xc0);
	value = readl(r->base[T6050_PS1] + 0x298);
	if (value & PS_DEV_DISABLE)
		writel(value & ~PS_DEV_DISABLE, r->base[T6050_PS1] + 0x298);
	writel(r->cluster_saved[4], s->cluster + 0x10);
	/* Replay only Linux snapshots; driver_down excludes owners that resume later. */
	sleep_region_replay(s);
	if (s->pcie_ans_stopped) {
		int ret = t6050_ans_storage_restore(s);

		/* A final-stop failure stays sticky even if domains returned. */
		if (s->ans_m5_wake_error != -EIO)
			s->ans_m5_wake_error = ret ?: s->ans_off_ret;
		/* Only ANS's own word and its parents fence it; another word is only reported. */
		for (i = 0; i < r->nans_chain; i++)
			if (r->late[r->ans_chain[i]])
				s->ans_m5_wake_error = -EIO;
	}
	if (s->ps_rewake_timeouts)
		dev_warn(s->dev, "%u power words not back at the wake\n", s->ps_rewake_timeouts);
	t6050_sleep_restore_pmc(s);
	/* Request word is Linux's saved state; report ownership remains until finish. */
	{
		u64 owned = BIT_ULL(12) | BIT_ULL(13) | BIT_ULL(14) | BIT_ULL(36);
		u64 request = readq(s->ptd + M5_PTD_READ);

		writeq((request & ~owned) | (r->ptd_saved & owned), s->ptd + M5_PTD_WRITE);
	}
	t6050_sleep_restore_display(s);
	s->pins_lost = s->pins_saved_valid;
	if (s->pcie_ans_stopped && s->pins_saved_valid)
		apple_sleep_restore_pins();
}

static void t6050_sleep_finish(struct apple_sleep *s)
{
	struct apple_sleep_region_state *r = s->region;
	u8 value = 0xfe;
	int ret;

	if (!r->attempt_active)
		return;
	r->attempt_active = false;
	t6050_sleep_undo_regs(s);
	/* Before any device resumes: the radio host's noirq resume asks for the result. */
	if (r->committed && r->n1_host) {
		r->n1_up_ret = t6050_n1_unpark(s);
		if (r->n1_up_ret)
			dev_err(s->dev, "radio's PCIe PHY and root complex not back: %d\n", r->n1_up_ret);
	}
	/* Also reached after a reversible refusal. No PMU sleep/wake command. */
	ret = r->boot_stage_written ?
		t6050_sleep_pmu_xfer(s, T6050_SPMI_EXT_WRITEL, 0xf801, &value, NULL, 1) : 0;
	r->boot_stage_written = false;
	t6050_sleep_ptd_ownership(s, false);
	r->usb_aon_kept = 0;
	dev_dbg(s->dev, "entry %s: replay %u, timeouts %u, late %u, PMU %u/%d, DART stops %#x\n",
		r->committed ? "returned" : "refused", s->ps_rewoken, s->ps_rewake_timeouts,
		r->late_errors, r->pmu_errors, ret, r->dart_done);
}

static const struct apple_sleep_soc_ops t6050_sleep_ops = {
	.plan_complex_request = t6050_sleep_plan_complex_request,
	.map_regs = t6050_sleep_map_regs,
	.boot_cluster_cycle = t6050_boot_cluster_cycle,
	.idle_wfi = t6050_idle_wfi,
	.begin = t6050_sleep_begin,
	.check = t6050_sleep_check_alarm,
	.display_off = t6050_sleep_display_prepare,
	.display_stop = sleep_v27_final_stop,
	.before_enclave = t6050_sleep_before_enclave,
	.stage = t6050_sleep_stage,
	.final_check = t6050_sleep_final_check,
	.commit_notice = t6050_sleep_commit_notice,
	.after_ans = t6050_sleep_after_ans,
	.prepare_regs = t6050_sleep_prepare_regs,
	.undo_regs = t6050_sleep_undo_regs,
	.pre_commit = t6050_sleep_pre_commit,
	.commit_regs = t6050_sleep_commit_regs,
	.post_timer = t6050_sleep_post_timer,
	.cpu_down = t6050_cpu_down,
	.wake_regs = t6050_sleep_wake_regs,
	.finish = t6050_sleep_finish,
	.cpu_prepare = t6050_sleep_cpu_prepare,
	.cpu_restore = t6050_sleep_cpu_restore,
};

static const struct apple_sleep_soc t6050_sleep_soc = {
	.strict_gate_inventory = true,
	.gate_coprocs = t6050_gate_coprocs,
	.ngate_coprocs = ARRAY_SIZE(t6050_gate_coprocs),
	.gate_off = t6050_gate_off,
	.ngate_off = ARRAY_SIZE(t6050_gate_off),
	.ops = &t6050_sleep_ops,
	.smc_policy = &t6050_smc_policy,
	.early_context = true,
	.machine = "apple,j714s",
	.boot_mpidr = 0x040000,
	.cpu_count = 18,
	.cpu_cluster_affinity = 1,
	.cpu_clusters = 3,
	.cpu_cores_per_cluster = 6,
	.cpu_start_stride = 6,
	.cpu_valid = t6050_sleep_cpu_valid,
	.display_count = 5,
	/* Resource inventory only; M5 DVFM sequencing is not T8140's. */
	.dvfm_names = { "dvfm-m0", "dvfm-m1", "dvfm-p" },
	.regions = &t6050_region_profile,
	.ps = {
		.ans = SLEEP_REGION_WORD(T6050_PS1, 0x140),
		.apcie_sys_gp = SLEEP_REGION_WORD(T6050_PS1, 0x280),
		.sep = SLEEP_REGION_WORD(T6050_PS1, 0x298),
		.sio_cpu = SLEEP_REGION_WORD(T6050_PS1, 0x2a0),
		.disp = {
			{ SLEEP_REGION_WORD(T6050_PS1, 0x1a8), SLEEP_REGION_WORD(T6050_PS1, 0x198),
			  SLEEP_REGION_WORD(T6050_PS1, 0x180) },
			{ SLEEP_REGION_WORD(T6050_PS0, 0x278), SLEEP_REGION_WORD(T6050_PS0, 0x268),
			  SLEEP_REGION_WORD(T6050_PS0, 0x200) },
			{ SLEEP_REGION_WORD(T6050_PS0, 0x2e0), SLEEP_REGION_WORD(T6050_PS0, 0x2d0),
			  SLEEP_REGION_WORD(T6050_PS0, 0x230) },
			{ SLEEP_REGION_WORD(T6050_PS0, 0x2b0), SLEEP_REGION_WORD(T6050_PS0, 0x2a0),
			  SLEEP_REGION_WORD(T6050_PS0, 0x220) },
			{ SLEEP_REGION_WORD(T6050_PS0, 0x2c8), SLEEP_REGION_WORD(T6050_PS0, 0x2b8),
			  SLEEP_REGION_WORD(T6050_PS0, 0x228) },
		},
	},
};
