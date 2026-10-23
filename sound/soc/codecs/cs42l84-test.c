// SPDX-License-Identifier: GPL-2.0-only
//
// KUnit tests for the clock switching in cs42l84_mute_stream().
//
// This file is included at the end of cs42l84.c when
// CONFIG_SND_SOC_CS42L84_TEST is enabled, so the tests can reach the
// driver's static DAI ops and PLL configuration.
//
// The codec runs from its internal oscillator (RCO) while no stream is
// active. Unmuting the first stream moves it onto BCLK, through the PLL
// if the BCLK rate needs one. Once the codec has left the oscillator it
// needs a working clock to answer on I2C, so these tests check when that
// switch happens and when it must not happen.

#include <kunit/device.h>
#include <kunit/test.h>

#define CS42L84_TEST_MAX_WRITES		64
#define CS42L84_TEST_NUM_REGS		0x7400

#define CS42L84_TEST_CCM_CTL1(src, freq) \
	(FIELD_PREP_CONST(CS42L84_CCM_CTL1_MCLK_SRC, (src)) | \
	 FIELD_PREP_CONST(CS42L84_CCM_CTL1_MCLK_FREQ, (freq)))

struct cs42l84_test_write {
	unsigned int reg;
	unsigned int val;
};

struct cs42l84_test_priv {
	struct kunit *test;

	struct cs42l84_private cs42l84;
	struct snd_soc_component component;
	struct snd_soc_dai dai;
	struct snd_soc_card soc_card;
	struct snd_card card;

	/* Register contents seen by the driver */
	u8 *regs;

	/* Every register write made by the driver, in order */
	struct cs42l84_test_write writes[CS42L84_TEST_MAX_WRITES];
	unsigned int num_writes;

	unsigned int num_lock_reads;
	unsigned int lock_status;
};

struct cs42l84_test_clk_param {
	unsigned int bclk;
	unsigned int rate;
	unsigned int ccm_ctl1;
};

/* BCLK rates that run the codec from the PLL */
static const struct cs42l84_test_clk_param cs42l84_test_pll_clks[] = {
	{
		.bclk = 3072000, .rate = 48000,
		.ccm_ctl1 = CS42L84_TEST_CCM_CTL1(CS42L84_CCM_CTL1_MCLK_SRC_PLL,
						  CS42L84_CCM_CTL1_MCLK_F_12_288KHZ),
	},
	{
		.bclk = 2822400, .rate = 44100,
		.ccm_ctl1 = CS42L84_TEST_CCM_CTL1(CS42L84_CCM_CTL1_MCLK_SRC_PLL,
						  CS42L84_CCM_CTL1_MCLK_F_12_288KHZ),
	},
};

/* BCLK rates that are used directly as MCLK */
static const struct cs42l84_test_clk_param cs42l84_test_bclk_clks[] = {
	{
		.bclk = 12288000, .rate = 48000,
		.ccm_ctl1 = CS42L84_TEST_CCM_CTL1(CS42L84_CCM_CTL1_MCLK_SRC_BCLK,
						  CS42L84_CCM_CTL1_MCLK_F_12_288KHZ),
	},
	{
		.bclk = 11289600, .rate = 44100,
		.ccm_ctl1 = CS42L84_TEST_CCM_CTL1(CS42L84_CCM_CTL1_MCLK_SRC_BCLK,
						  CS42L84_CCM_CTL1_MCLK_F_12_288KHZ),
	},
};

static int cs42l84_test_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct cs42l84_test_priv *priv = context;

	if (reg >= CS42L84_TEST_NUM_REGS)
		return -EINVAL;

	if (reg == CS42L84_PLL_LOCK_STATUS) {
		priv->num_lock_reads++;
		*val = priv->lock_status;
		return 0;
	}

	*val = priv->regs[reg];

	return 0;
}

static int cs42l84_test_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct cs42l84_test_priv *priv = context;

	if (reg >= CS42L84_TEST_NUM_REGS)
		return -EINVAL;

	if (priv->num_writes < ARRAY_SIZE(priv->writes)) {
		priv->writes[priv->num_writes].reg = reg;
		priv->writes[priv->num_writes].val = val;
		priv->num_writes++;
	} else {
		KUNIT_FAIL(priv->test, "write log full at reg %#x", reg);
	}

	priv->regs[reg] = val;

	return 0;
}

static const struct regmap_config cs42l84_test_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = CS42L84_TEST_NUM_REGS - 1,
	.reg_read = cs42l84_test_reg_read,
	.reg_write = cs42l84_test_reg_write,
	.cache_type = REGCACHE_NONE,
};

static void cs42l84_test_clear_log(struct cs42l84_test_priv *priv)
{
	priv->num_writes = 0;
	priv->num_lock_reads = 0;
}

/* Index of the first write to @reg at or after index @from, or -1 */
static int cs42l84_test_find_write(struct cs42l84_test_priv *priv,
				   unsigned int reg, int from)
{
	unsigned int i;

	for (i = max(from, 0); i < priv->num_writes; i++) {
		if (priv->writes[i].reg == reg)
			return i;
	}

	return -1;
}

/* Index of the last write to @reg, or -1 */
static int cs42l84_test_last_write(struct cs42l84_test_priv *priv, unsigned int reg)
{
	unsigned int i;

	for (i = priv->num_writes; i > 0; i--) {
		if (priv->writes[i - 1].reg == reg)
			return i - 1;
	}

	return -1;
}

static unsigned int cs42l84_test_count_writes(struct cs42l84_test_priv *priv,
					      unsigned int reg)
{
	unsigned int i, n = 0;

	for (i = 0; i < priv->num_writes; i++) {
		if (priv->writes[i].reg == reg)
			n++;
	}

	return n;
}

/* Select the clock configuration the way the machine driver and hw_params do */
static void cs42l84_test_set_clk(struct kunit *test, const struct cs42l84_test_clk_param *clk)
{
	struct cs42l84_test_priv *priv = test->priv;

	KUNIT_ASSERT_EQ(test, snd_soc_dai_set_sysclk(&priv->dai, 0, clk->bclk,
						     SND_SOC_CLOCK_IN), 0);
	priv->cs42l84.srate = clk->rate;
	KUNIT_ASSERT_EQ(test, cs42l84_pll_config(&priv->component), 0);
	cs42l84_test_clear_log(priv);
}

static void cs42l84_test_set_power(struct cs42l84_test_priv *priv, unsigned int state)
{
	snd_power_change_state(&priv->card, state);
}

/* Mute or unmute through the DAI ops, as the ASoC core does */
static int cs42l84_test_mute(struct cs42l84_test_priv *priv, int mute, int stream)
{
	return snd_soc_dai_digital_mute(&priv->dai, mute, stream);
}

/*
 * Check that the writes logged since the last clear are a complete switch
 * from the oscillator to the PLL output, with the DAC unmuted last.
 */
static void cs42l84_test_expect_switch_to_pll(struct kunit *test,
					      const struct cs42l84_test_clk_param *clk)
{
	struct cs42l84_test_priv *priv = test->priv;
	int refclk, pll_start, ccm, dac;

	refclk = cs42l84_test_find_write(priv, CS42L84_CCM_CTL4, 0);
	KUNIT_ASSERT_GE(test, refclk, 0);
	KUNIT_EXPECT_TRUE(test, priv->writes[refclk].val & CS42L84_CCM_CTL4_REFCLK_EN);

	pll_start = cs42l84_test_find_write(priv, CS42L84_PLL_CTL1, refclk);
	KUNIT_ASSERT_GT(test, pll_start, refclk);
	KUNIT_EXPECT_TRUE(test, priv->writes[pll_start].val & CS42L84_PLL_CTL1_EN);

	/* The PLL output must only be selected once the PLL has locked */
	KUNIT_EXPECT_GT(test, priv->num_lock_reads, 0);
	ccm = cs42l84_test_find_write(priv, CS42L84_CCM_CTL1, pll_start);
	KUNIT_ASSERT_GT(test, ccm, pll_start);
	KUNIT_EXPECT_EQ(test, priv->writes[ccm].val, clk->ccm_ctl1);

	dac = cs42l84_test_find_write(priv, CS42L84_DAC_CTL1, ccm);
	KUNIT_ASSERT_GT(test, dac, ccm);
	KUNIT_EXPECT_TRUE(test, priv->writes[dac].val & CS42L84_DAC_CTL1_UNMUTE);
}

/*
 * Follow the codec callbacks from a running stream through card suspend and
 * resume. ASoC sends the only resume unmute at D2, then publishes D0. A PCM
 * with SNDRV_PCM_INFO_RESUME can trigger without another prepare/unmute.
 * A successful PLL lock represents clocks being available, independently of
 * card power state; the direct-BCLK case retains the host's clock contract.
 */
static void cs42l84_test_resume_without_prepare(struct kunit *test)
{
	const struct cs42l84_test_clk_param *clk = test->param_value;
	struct cs42l84_test_priv *priv = test->priv;
	unsigned int writes;

	if (!IS_ENABLED(CONFIG_PM))
		kunit_skip(test, "card power state needs CONFIG_PM");

	cs42l84_test_set_clk(test, clk);
	KUNIT_ASSERT_EQ(test, cs42l84_test_mute(priv, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);
	cs42l84_test_set_power(priv, SNDRV_CTL_POWER_D3hot);
	KUNIT_ASSERT_EQ(test, cs42l84_test_mute(priv, 1, SNDRV_PCM_STREAM_PLAYBACK), 0);
	KUNIT_ASSERT_EQ(test, priv->cs42l84.stream_use, 0);
	KUNIT_ASSERT_EQ(test, priv->regs[CS42L84_CCM_CTL1], CS42L84_CCM_CTL1_RCO);
	cs42l84_test_clear_log(priv);

	cs42l84_test_set_power(priv, SNDRV_CTL_POWER_D2);
	KUNIT_ASSERT_EQ(test, cs42l84_test_mute(priv, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L84_CCM_CTL1], clk->ccm_ctl1);
	KUNIT_EXPECT_TRUE(test, priv->regs[CS42L84_DAC_CTL1] & CS42L84_DAC_CTL1_UNMUTE);
	KUNIT_EXPECT_EQ(test, priv->cs42l84.stream_use, BIT(SNDRV_PCM_STREAM_PLAYBACK));
	writes = priv->num_writes;
	KUNIT_EXPECT_GT(test, writes, 0);

	/* D0 is a notification, not a second codec callback or clock proof. */
	cs42l84_test_set_power(priv, SNDRV_CTL_POWER_D0);
	KUNIT_EXPECT_EQ(test, priv->num_writes, writes);
	KUNIT_EXPECT_TRUE(test, priv->regs[CS42L84_DAC_CTL1] & CS42L84_DAC_CTL1_UNMUTE);
}

static void cs42l84_test_resume_without_prepare_bclk(struct kunit *test)
{
	cs42l84_test_resume_without_prepare(test);
}

/* At D0 with a locked PLL the codec moves onto the PLL output */
static void cs42l84_test_unmute_pll_locked(struct kunit *test)
{
	const struct cs42l84_test_clk_param *clk = test->param_value;
	struct cs42l84_test_priv *priv = test->priv;

	cs42l84_test_set_clk(test, clk);

	KUNIT_EXPECT_EQ(test, cs42l84_test_mute(priv, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);

	cs42l84_test_expect_switch_to_pll(test, clk);
	KUNIT_EXPECT_EQ(test, priv->cs42l84.stream_use, BIT(SNDRV_PCM_STREAM_PLAYBACK));
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L84_CCM_CTL1], clk->ccm_ctl1);
	KUNIT_EXPECT_TRUE(test, priv->regs[CS42L84_DAC_CTL1] & CS42L84_DAC_CTL1_UNMUTE);
}

/*
 * The PLL lock error bit is only reported: once the lock bit is set the
 * codec still moves onto the PLL output.
 */
static void cs42l84_test_unmute_pll_lock_error_bit(struct kunit *test)
{
	const struct cs42l84_test_clk_param *clk = &cs42l84_test_pll_clks[0];
	struct cs42l84_test_priv *priv = test->priv;

	cs42l84_test_set_clk(test, clk);
	priv->lock_status = CS42L84_PLL_LOCK_STATUS_LOCKED | CS42L84_PLL_LOCK_STATUS_ERROR;

	KUNIT_EXPECT_EQ(test, cs42l84_test_mute(priv, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);

	cs42l84_test_expect_switch_to_pll(test, clk);
	KUNIT_EXPECT_EQ(test, priv->cs42l84.stream_use, BIT(SNDRV_PCM_STREAM_PLAYBACK));
}

/*
 * If the PLL does not lock the codec must stay on the oscillator with the
 * PLL and its reference clock stopped again and the DAC muted, and the
 * error must be returned.
 */
static void cs42l84_test_unmute_pll_lock_timeout(struct kunit *test)
{
	const struct cs42l84_test_clk_param *clk = test->param_value;
	struct cs42l84_test_priv *priv = test->priv;
	int pll_stop, refclk_stop;

	cs42l84_test_set_clk(test, clk);
	priv->lock_status = 0;

	KUNIT_EXPECT_EQ(test, cs42l84_test_mute(priv, 0, SNDRV_PCM_STREAM_PLAYBACK),
			-ETIMEDOUT);
	KUNIT_EXPECT_GT(test, priv->num_lock_reads, 0);

	/* Still on the oscillator */
	KUNIT_EXPECT_EQ(test, cs42l84_test_count_writes(priv, CS42L84_CCM_CTL1), 0);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L84_CCM_CTL1], CS42L84_CCM_CTL1_RCO);

	/* The PLL was started and stopped again, then its reference clock */
	pll_stop = cs42l84_test_last_write(priv, CS42L84_PLL_CTL1);
	KUNIT_ASSERT_GE(test, pll_stop, 0);
	KUNIT_EXPECT_EQ(test, priv->writes[pll_stop].val & CS42L84_PLL_CTL1_EN, 0);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L84_PLL_CTL1] & CS42L84_PLL_CTL1_EN, 0);

	refclk_stop = cs42l84_test_last_write(priv, CS42L84_CCM_CTL4);
	KUNIT_ASSERT_GT(test, refclk_stop, pll_stop);
	KUNIT_EXPECT_EQ(test, priv->writes[refclk_stop].val & CS42L84_CCM_CTL4_REFCLK_EN, 0);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L84_CCM_CTL4] & CS42L84_CCM_CTL4_REFCLK_EN, 0);

	/* DAC still muted, stream not counted as running */
	KUNIT_EXPECT_EQ(test, cs42l84_test_count_writes(priv, CS42L84_DAC_CTL1), 0);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L84_DAC_CTL1] & CS42L84_DAC_CTL1_UNMUTE, 0);
	KUNIT_EXPECT_EQ(test, priv->cs42l84.stream_use, 0);
}

/* No PLL lock at D2 must not be mistaken for a usable clock at resume. */
static void cs42l84_test_resume_without_pll_lock(struct kunit *test)
{
	struct cs42l84_test_priv *priv = test->priv;

	if (!IS_ENABLED(CONFIG_PM))
		kunit_skip(test, "card power state needs CONFIG_PM");

	cs42l84_test_set_power(priv, SNDRV_CTL_POWER_D2);
	cs42l84_test_unmute_pll_lock_timeout(test);
}

/*
 * A failed unmute leaves stream_use clear, so the next unmute retries the
 * whole switch instead of assuming the codec is already on the PLL.
 */
static void cs42l84_test_unmute_after_pll_lock_timeout(struct kunit *test)
{
	const struct cs42l84_test_clk_param *clk = test->param_value;
	struct cs42l84_test_priv *priv = test->priv;

	cs42l84_test_set_clk(test, clk);

	priv->lock_status = 0;
	KUNIT_EXPECT_LT(test, cs42l84_test_mute(priv, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);

	cs42l84_test_clear_log(priv);
	priv->lock_status = CS42L84_PLL_LOCK_STATUS_LOCKED;
	KUNIT_EXPECT_EQ(test, cs42l84_test_mute(priv, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);

	cs42l84_test_expect_switch_to_pll(test, clk);
	KUNIT_EXPECT_EQ(test, priv->cs42l84.stream_use, BIT(SNDRV_PCM_STREAM_PLAYBACK));
}

/*
 * Muting the last stream goes back to the oscillator before stopping the
 * PLL and then its reference clock.
 */
static void cs42l84_test_mute_returns_to_rco(struct kunit *test)
{
	const struct cs42l84_test_clk_param *clk = test->param_value;
	struct cs42l84_test_priv *priv = test->priv;
	int dac, ccm, pll_stop, refclk_stop;

	cs42l84_test_set_clk(test, clk);
	KUNIT_ASSERT_EQ(test, cs42l84_test_mute(priv, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);
	cs42l84_test_clear_log(priv);

	KUNIT_EXPECT_EQ(test, cs42l84_test_mute(priv, 1, SNDRV_PCM_STREAM_PLAYBACK), 0);

	dac = cs42l84_test_find_write(priv, CS42L84_DAC_CTL1, 0);
	KUNIT_ASSERT_GE(test, dac, 0);
	KUNIT_EXPECT_EQ(test, priv->writes[dac].val & CS42L84_DAC_CTL1_UNMUTE, 0);

	ccm = cs42l84_test_find_write(priv, CS42L84_CCM_CTL1, dac);
	KUNIT_ASSERT_GT(test, ccm, dac);
	KUNIT_EXPECT_EQ(test, priv->writes[ccm].val, CS42L84_CCM_CTL1_RCO);

	pll_stop = cs42l84_test_find_write(priv, CS42L84_PLL_CTL1, ccm);
	KUNIT_ASSERT_GT(test, pll_stop, ccm);
	KUNIT_EXPECT_EQ(test, priv->writes[pll_stop].val & CS42L84_PLL_CTL1_EN, 0);

	refclk_stop = cs42l84_test_find_write(priv, CS42L84_CCM_CTL4, pll_stop);
	KUNIT_ASSERT_GT(test, refclk_stop, pll_stop);
	KUNIT_EXPECT_EQ(test, priv->writes[refclk_stop].val & CS42L84_CCM_CTL4_REFCLK_EN, 0);

	KUNIT_EXPECT_EQ(test, priv->cs42l84.stream_use, 0);
}

/* Without the PLL the codec goes straight onto BCLK, no lock poll */
static void cs42l84_test_unmute_bclk(struct kunit *test)
{
	const struct cs42l84_test_clk_param *clk = test->param_value;
	struct cs42l84_test_priv *priv = test->priv;
	int refclk, ccm, dac;

	cs42l84_test_set_clk(test, clk);
	priv->lock_status = 0;

	KUNIT_EXPECT_EQ(test, cs42l84_test_mute(priv, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);

	KUNIT_EXPECT_EQ(test, priv->num_lock_reads, 0);
	KUNIT_EXPECT_EQ(test, cs42l84_test_count_writes(priv, CS42L84_PLL_CTL1), 0);

	refclk = cs42l84_test_find_write(priv, CS42L84_CCM_CTL4, 0);
	KUNIT_ASSERT_GE(test, refclk, 0);
	KUNIT_EXPECT_TRUE(test, priv->writes[refclk].val & CS42L84_CCM_CTL4_REFCLK_EN);

	ccm = cs42l84_test_find_write(priv, CS42L84_CCM_CTL1, refclk);
	KUNIT_ASSERT_GT(test, ccm, refclk);
	KUNIT_EXPECT_EQ(test, priv->writes[ccm].val, clk->ccm_ctl1);

	dac = cs42l84_test_find_write(priv, CS42L84_DAC_CTL1, ccm);
	KUNIT_ASSERT_GT(test, dac, ccm);
	KUNIT_EXPECT_TRUE(test, priv->writes[dac].val & CS42L84_DAC_CTL1_UNMUTE);

	KUNIT_EXPECT_EQ(test, priv->cs42l84.stream_use, BIT(SNDRV_PCM_STREAM_PLAYBACK));
}

static int cs42l84_test_case_init(struct kunit *test)
{
	struct cs42l84_test_priv *priv;
	struct regmap *regmap;
	struct device *dev;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv);
	priv->test = test;
	test->priv = priv;

	priv->regs = kunit_kzalloc(test, CS42L84_TEST_NUM_REGS, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->regs);

	dev = kunit_device_register(test, "cs42l84_test_drv");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	regmap = devm_regmap_init(dev, NULL, priv, &cs42l84_test_regmap_config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, regmap);

	/* Construct the minimal set of driver structs */
	priv->cs42l84.dev = dev;
	priv->cs42l84.regmap = regmap;

	priv->component.dev = dev;
	priv->component.card = &priv->soc_card;
	snd_soc_component_init_regmap(&priv->component, regmap);
	snd_soc_component_set_drvdata(&priv->component, &priv->cs42l84);

	priv->dai.dev = dev;
	priv->dai.name = cs42l84_dai.name;
	priv->dai.driver = &cs42l84_dai;
	priv->dai.component = &priv->component;

	priv->soc_card.snd_card = &priv->card;
#ifdef CONFIG_PM
	init_waitqueue_head(&priv->card.power_sleep);
	snd_refcount_init(&priv->card.power_ref);
#endif
	cs42l84_test_set_power(priv, SNDRV_CTL_POWER_D0);

	/* Idle codec: on the oscillator, PLL and REFCLK off, DAC muted */
	priv->regs[CS42L84_CCM_CTL1] = CS42L84_CCM_CTL1_RCO;
	priv->lock_status = CS42L84_PLL_LOCK_STATUS_LOCKED;

	return 0;
}

static void cs42l84_test_clk_desc(const struct cs42l84_test_clk_param *param, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "BCLK %u Hz, %u Hz", param->bclk, param->rate);
}

KUNIT_ARRAY_PARAM(cs42l84_test_pll_clks, cs42l84_test_pll_clks, cs42l84_test_clk_desc);
KUNIT_ARRAY_PARAM(cs42l84_test_bclk_clks, cs42l84_test_bclk_clks, cs42l84_test_clk_desc);

static struct kunit_case cs42l84_test_cases[] = {
	KUNIT_CASE_PARAM(cs42l84_test_resume_without_prepare, cs42l84_test_pll_clks_gen_params),
	KUNIT_CASE_PARAM(cs42l84_test_resume_without_prepare_bclk, cs42l84_test_bclk_clks_gen_params),
	KUNIT_CASE_PARAM(cs42l84_test_unmute_pll_locked, cs42l84_test_pll_clks_gen_params),
	KUNIT_CASE(cs42l84_test_unmute_pll_lock_error_bit),
	KUNIT_CASE_PARAM(cs42l84_test_unmute_pll_lock_timeout, cs42l84_test_pll_clks_gen_params),
	KUNIT_CASE_PARAM(cs42l84_test_resume_without_pll_lock, cs42l84_test_pll_clks_gen_params),
	KUNIT_CASE_PARAM(cs42l84_test_unmute_after_pll_lock_timeout,
			 cs42l84_test_pll_clks_gen_params),
	KUNIT_CASE_PARAM(cs42l84_test_mute_returns_to_rco, cs42l84_test_pll_clks_gen_params),
	KUNIT_CASE_PARAM(cs42l84_test_unmute_bclk, cs42l84_test_bclk_clks_gen_params),
	{ } /* terminator */
};

static struct kunit_suite cs42l84_test_suite = {
	.name = "snd-soc-cs42l84-test",
	.init = cs42l84_test_case_init,
	.test_cases = cs42l84_test_cases,
};

kunit_test_suite(cs42l84_test_suite);
