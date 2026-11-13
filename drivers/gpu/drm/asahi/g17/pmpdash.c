// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * GFX power for the J813 (T8142): hold the power domain and keep ps_gfx at its
 * top state for the firmware session.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/pm_runtime.h>

int asahi_j813_hold_power(struct device *dev);
int asahi_j813_power_on_gfx(void);

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

/*
 * hold_power: keep the GFX power domain up for the firmware session, as the G15
 * and G16 paths do for theirs (asahi_g15_hold_power).  Nothing held it on the
 * J813: the domain is inherited on from the boot loader, genpd gates it when it
 * finds no active consumer, and ps_gfx then reads target 0xf with actual 0 while
 * the coprocessors run out of their own always-on island.  Returns an error when
 * the device has no power domain to hold.
 */
int asahi_j813_hold_power(struct device *dev)
{
	int ret;

	pm_runtime_get_noresume(dev);
	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;
	pm_runtime_enable(dev);
	return dev->pm_domain ? 0 : -EINVAL;
}

/*
 * power_on: bring the GFX domain to its top power state and leave it there for
 * the firmware session.  The domain is gated by the time this driver probes
 * (genpd finds no active consumer, and marking the device active does not
 * request a transition), so ps_gfx reads target 0xf with actual 0 while the
 * coprocessors run out of their own always-on island and the GPU's cores can
 * never come up.  Auto power management is left off, so nothing gates the
 * domain again while the session holds it; macOS re-enables it because its own
 * stack keeps a demand on the domain.
 */
int asahi_j813_power_on_gfx(void)
{
	void __iomem *ps;
	u32 val, cur;
	int ret = 0;

	ps = ioremap(SGX_PS_BASE, 0x1000);
	if (!ps)
		return -ENOMEM;
	cur = readl(ps + SGX_PS_REG);
	if (FIELD_GET(PS_ACTUAL, cur) == 0xf && !(cur & PS_AUTO_ENABLE)) {
		pr_info("asahi: GFX ps already held on (%#x)\n", cur);
		iounmap(ps);
		return 0;
	}
	val = ptd_ps_write(ps + SGX_PS_REG, 0xf, false);
	if (ptd_ps_wait(ps + SGX_PS_REG, 0xf, &cur)) {
		pr_warn("asahi: GFX ps did not power on (wrote %#x, now %#x)\n", val, cur);
		ret = -EIO;
	} else {
		pr_info("asahi: GFX ps held on (wrote %#x, now %#x)\n", val, cur);
	}
	iounmap(ps);
	return ret;
}
