// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Bring-up only: PMP helpers for the J813 GPU.
 *
 * report: report the AGX device state to the T8142 PMP over the mcc dashboard
 * (SOC-DEV-PS-REQ idx 0x27, SOC-DEV-PS-ACK idx 0x2f), the way macOS ApplePMGR
 * does in native order right after the platform power-on write.
 *
 * power_cycle: force the ps_gfx power transition macOS performs from off.  The
 * M5's domain is inherited on (0x1f0023ff), so Linux's genpd never transitions
 * it; macOS writes target 0xf with auto-PM off, then auto-PM on, around the PMP
 * report.  A transition edge may be what makes the PMP enable the sgx node's
 * power/clock gates 515 (GFX-SGX) and 516 (GFX-BUSY).
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>

int asahi_j813_pmp_report_agx(bool on);
int asahi_j813_pmp_agx_power_cycle(void);

#define PTD_DB_BASE	0x3803c0000ULL
#define PTD_UPD_BASE	0x3803d0000ULL
#define PTD_WIN_SIZE	0x4000

#define PTD_PS_REQ	0x27
#define PTD_PS_ACK	0x2f
#define PTD_META_FRESH	BIT_ULL(1)

#define AGX_BIT		12

#define SGX_PS_BASE	0x580e80000ULL
#define SGX_PS_REG	0x110

#define PS_TARGET	GENMASK(3, 0)
#define PS_ACTUAL	GENMASK(7, 4)
#define PS_WAS_PWRGATED	BIT(8)
#define PS_WAS_CLKGATED	BIT(9)
#define PS_DEV_DISABLE	BIT(10)
#define PS_BUSY		BIT(11)
#define PS_RESET	BIT(12)
#define PS_AUTO_ENABLE	BIT(28)
#define PS_FLAGS	(PS_WAS_PWRGATED | PS_WAS_CLKGATED)

static u64 ptd_read(void __iomem *db, u32 idx, u64 *meta)
{
	u64 v = readq(db + 16 * idx);

	*meta = readq(db + 16 * idx + 8);
	return v;
}

int asahi_j813_pmp_report_agx(bool on)
{
	u64 req, ack, meta, pre_ack, pre_meta, want;
	ktime_t t0;
	void __iomem *db, *upd;
	bool fresh = false;
	int ret = 0;

	db = ioremap(PTD_DB_BASE, PTD_WIN_SIZE);
	upd = ioremap(PTD_UPD_BASE, PTD_WIN_SIZE);
	if (!db || !upd) {
		ret = -ENOMEM;
		goto out;
	}

	req = ptd_read(db, PTD_PS_REQ, &meta);
	pre_ack = ptd_read(db, PTD_PS_ACK, &pre_meta);
	want = on ? req | BIT_ULL(AGX_BIT) : req & ~BIT_ULL(AGX_BIT);
	pr_info("asahi: PMP AGX %s: PS-REQ %#llx -> %#llx (pre-ACK %#llx meta %#llx)\n",
		on ? "on" : "off", req, want, pre_ack, pre_meta);
	writeq(want, upd + 8 * PTD_PS_REQ);

	t0 = ktime_get();
	for (;;) {
		ack = ptd_read(db, PTD_PS_ACK, &meta);
		if (meta & PTD_META_FRESH)
			fresh = true;
		if (!((ack ^ want) & BIT_ULL(AGX_BIT)) &&
		    (fresh || ack != pre_ack)) {
			pr_info("asahi: PMP AGX %s ACKed in %lld us (ACK %#llx meta %#llx fresh=%u)\n",
				on ? "on" : "off",
				ktime_us_delta(ktime_get(), t0), ack, meta, fresh);
			break;
		}
		if (ktime_us_delta(ktime_get(), t0) > 15000000) {
			pr_warn("asahi: PMP AGX %s NOT acked in 15 s (ACK %#llx meta %#llx fresh=%u)\n",
				on ? "on" : "off", ack, meta, fresh);
			ret = -ETIMEDOUT;
			break;
		}
		usleep_range(100, 200);
	}

out:
	if (upd)
		iounmap(upd);
	if (db)
		iounmap(db);
	return ret;
}

static u32 ptd_ps_write(void __iomem *ps, u32 pstate, bool auto_enable)
{
	u32 cur = readl(ps);
	u32 val = cur & ~(PS_TARGET | PS_FLAGS | PS_BUSY | PS_DEV_DISABLE |
			  PS_RESET | PS_AUTO_ENABLE);

	val |= FIELD_PREP(PS_TARGET, pstate);
	if (auto_enable)
		val |= PS_AUTO_ENABLE;
	writel(val, ps);
	return val;
}

static int ptd_ps_wait(void __iomem *ps, u32 pstate, u32 *out)
{
	ktime_t t0 = ktime_get();
	u32 cur;

	for (;;) {
		cur = readl(ps);
		if (FIELD_GET(PS_ACTUAL, cur) == pstate)
			break;
		if (ktime_us_delta(ktime_get(), t0) > 20000) {
			*out = cur;
			return -ETIMEDOUT;
		}
		usleep_range(50, 100);
	}
	*out = cur;
	return 0;
}

int asahi_j813_pmp_agx_power_cycle(void)
{
	void __iomem *ps;
	u32 val, cur;
	int ret = 0;

	ps = ioremap(SGX_PS_BASE, 0x1000);
	if (!ps)
		return -ENOMEM;

	cur = readl(ps + SGX_PS_REG);
	pr_info("asahi: GFX ps before cycle: %#x\n", cur);

	val = ptd_ps_write(ps + SGX_PS_REG, 0, false);
	if (ptd_ps_wait(ps + SGX_PS_REG, 0, &cur)) {
		pr_warn("asahi: GFX ps did not power-gate (wrote %#x, now %#x)\n",
			val, cur);
		ret = -EIO;
	} else {
		pr_info("asahi: GFX ps powered off (wrote %#x, now %#x)\n", val, cur);
	}

	val = ptd_ps_write(ps + SGX_PS_REG, 0xf, false);
	if (ptd_ps_wait(ps + SGX_PS_REG, 0xf, &cur)) {
		pr_warn("asahi: GFX ps did not power on (wrote %#x, now %#x)\n",
			val, cur);
		ret = -EIO;
	} else {
		pr_info("asahi: GFX ps powered on, auto-PM off (wrote %#x, now %#x)\n",
			val, cur);
	}

	val = ptd_ps_write(ps + SGX_PS_REG, 0xf, true);
	pr_info("asahi: GFX ps auto-PM on (wrote %#x)\n", val);

	iounmap(ps);
	return ret;
}
