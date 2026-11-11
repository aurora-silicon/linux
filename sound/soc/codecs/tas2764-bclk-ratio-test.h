/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * KUnit tests of tas2764_parse_bclk_ratio() and tas2764_apply_bclk_ratio():
 * an amp whose node has no ti,bclk-ratio, as on every MCA machine (M1, M2,
 * M3, M4), keeps the ratio auto-detection and its probe writes neither
 * TAS2764_CLK_CFG nor TDM_CFG0's fixed-ratio bit; only the property sets
 * them. Uses the nodes of tas2764-kunit.dtso and tas2764_test_priv().
 */
#include <kunit/device.h>
#include <kunit/test.h>

/* Page 0, where both registers are */
#define TAS2764_TEST_NREGS	TAS2764_REG(0x1, 0x0)

/* Register values and every write, behind a regmap without a bus */
struct tas2764_test_regs {
	unsigned int val[TAS2764_TEST_NREGS];
	unsigned int writes;
};

static int tas2764_test_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct tas2764_test_regs *regs = context;

	if (reg >= TAS2764_TEST_NREGS)
		return -EINVAL;
	*val = regs->val[reg];
	return 0;
}

static int tas2764_test_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct tas2764_test_regs *regs = context;

	if (reg >= TAS2764_TEST_NREGS)
		return -EINVAL;
	regs->val[reg] = val;
	regs->writes++;
	return 0;
}

static const struct regmap_config tas2764_test_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TAS2764_TEST_NREGS - 1,
	.reg_read = tas2764_test_reg_read,
	.reg_write = tas2764_test_reg_write,
	.cache_type = REGCACHE_NONE,
};

/*
 * The driver state for test node @node, as tas2764_i2c_probe() leaves it
 * before the device tree is read, with a recording register map.
 */
static struct tas2764_priv *tas2764_test_bclk_priv(struct kunit *test, const char *node,
						   struct device **devp,
						   struct tas2764_test_regs **regsp)
{
	struct tas2764_test_regs *regs;
	struct tas2764_priv *tas2764;
	struct device *mapdev;

	tas2764 = tas2764_test_priv(test, node, DEVID_SN012776, devp);
	regs = kunit_kzalloc(test, sizeof(*regs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, regs);
	mapdev = kunit_device_register(test, "tas2764-bclk-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(mapdev));
	tas2764->regmap = devm_regmap_init(mapdev, NULL, regs, &tas2764_test_regmap_config);
	KUNIT_ASSERT_FALSE(test, IS_ERR(tas2764->regmap));

	tas2764->bclk_ratio_code = -1;
	/* Whatever TDM_CFG0 held: only the fixed-ratio bit may change */
	regs->val[TAS2764_TDM_CFG0] = TAS2764_TDM_CFG0_44_1_48KHZ | TAS2764_TDM_CFG0_FRAME_START;
	*regsp = regs;
	return tas2764;
}

/* Amp nodes without ti,bclk-ratio */
static const char *const tas2764_bclk_absent_nodes[] = {
	/* With the ADT blocks the boot loader copies on the MCA machines */
	"tas2764-kunit-bop-good",
	"tas2764-kunit-bop-none",
};

static void tas2764_bclk_absent_desc(const char *const *node, char *desc)
{
	strscpy(desc, *node, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(tas2764_bclk_absent, tas2764_bclk_absent_nodes, tas2764_bclk_absent_desc);

/* No ti,bclk-ratio: auto-detection, and not one register written */
static void tas2764_bclk_ratio_absent(struct kunit *test)
{
	const char *const *node = test->param_value;
	struct tas2764_test_regs *regs;
	struct tas2764_priv *tas2764;
	struct device *dev;

	tas2764 = tas2764_test_bclk_priv(test, *node, &dev, &regs);
	KUNIT_EXPECT_EQ(test, tas2764_parse_bclk_ratio(dev, tas2764), 0);
	KUNIT_EXPECT_EQ(test, tas2764->bclk_ratio_code, -1);
	KUNIT_EXPECT_EQ(test, tas2764_apply_bclk_ratio(tas2764), 0);
	KUNIT_EXPECT_EQ(test, regs->writes, 0);
	KUNIT_EXPECT_EQ(test, regs->val[TAS2764_CLK_CFG], 0);
	KUNIT_EXPECT_EQ(test, regs->val[TAS2764_TDM_CFG0],
			TAS2764_TDM_CFG0_44_1_48KHZ | TAS2764_TDM_CFG0_FRAME_START);
}

/* 250 BCLKs (J813, J873): its code, and the fixed-ratio bit beside the rest */
static void tas2764_bclk_ratio_250(struct kunit *test)
{
	struct tas2764_test_regs *regs;
	struct tas2764_priv *tas2764;
	struct device *dev;

	tas2764 = tas2764_test_bclk_priv(test, "tas2764-kunit-bclk-250", &dev, &regs);
	KUNIT_EXPECT_EQ(test, tas2764_parse_bclk_ratio(dev, tas2764), 0);
	KUNIT_EXPECT_EQ(test, tas2764->bclk_ratio_code, 0x30);
	KUNIT_EXPECT_EQ(test, tas2764_apply_bclk_ratio(tas2764), 0);
	KUNIT_EXPECT_EQ(test, regs->val[TAS2764_CLK_CFG], 0x30);
	KUNIT_EXPECT_EQ(test, regs->val[TAS2764_TDM_CFG0],
			TAS2764_TDM_CFG0_44_1_48KHZ | TAS2764_TDM_CFG0_FRAME_START |
			TAS2764_TDM_CFG0_FIXED_RATIO);
	KUNIT_EXPECT_EQ(test, regs->writes, 2);
	/* The register cache keeps both across a reset and a resume */
	KUNIT_EXPECT_TRUE(test, tas2764_writeable_register(NULL, TAS2764_CLK_CFG));
	KUNIT_EXPECT_TRUE(test, tas2764_writeable_register(NULL, TAS2764_TDM_CFG0));
}

/* A ratio without a known code fails the probe instead of guessing */
static void tas2764_bclk_ratio_unknown(struct kunit *test)
{
	struct tas2764_test_regs *regs;
	struct tas2764_priv *tas2764;
	struct device *dev;

	tas2764 = tas2764_test_bclk_priv(test, "tas2764-kunit-bclk-64", &dev, &regs);
	KUNIT_EXPECT_EQ(test, tas2764_parse_bclk_ratio(dev, tas2764), -EINVAL);
	KUNIT_EXPECT_EQ(test, tas2764->bclk_ratio_code, -1);
	KUNIT_EXPECT_EQ(test, tas2764_apply_bclk_ratio(tas2764), 0);
	KUNIT_EXPECT_EQ(test, regs->writes, 0);
}

static struct kunit_case tas2764_bclk_ratio_test_cases[] = {
	KUNIT_CASE_PARAM(tas2764_bclk_ratio_absent, tas2764_bclk_absent_gen_params),
	KUNIT_CASE(tas2764_bclk_ratio_250),
	KUNIT_CASE(tas2764_bclk_ratio_unknown),
	{}
};

static struct kunit_suite tas2764_bclk_ratio_test_suite = {
	.name = "tas2764-bclk-ratio",
	.init = tas2764_bop_test_init,
	.test_cases = tas2764_bclk_ratio_test_cases,
};

kunit_test_suite(tas2764_bclk_ratio_test_suite);
