// SPDX-License-Identifier: GPL-2.0-only
/*
 * cs42l83-i2c.c -- CS42L83 ALSA SoC audio driver for I2C
 *
 * Based on cs42l42-i2c.c:
 *   Copyright 2016, 2022 Cirrus Logic, Inc.
 */

#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <sound/soc.h>

#include "cs42l42.h"

/*
 * The register restore after a sleep is about 100 ms of I2C writes, and the PM core waits
 * for every device resume before user space thaws. ASoC holds the card from user space
 * until its own deferred resume, so the restore runs from there when ASoC suspended the
 * codec. 0 restores in the device's resume, as before.
 */
static bool defer_restore = true;
module_param(defer_restore, bool, 0644);
MODULE_PARM_DESC(defer_restore, "Restore the registers after a sleep from ASoC's card resume");

#define CS42L83_POWERED_WAIT_MS 1000

static const struct reg_default cs42l83_reg_defaults[] = {
	{ CS42L42_FRZ_CTL,			0x00 },
	{ CS42L42_SRC_CTL,			0x10 },
	{ CS42L42_MCLK_CTL,			0x00 }, /* <- only deviation from CS42L42 */
	{ CS42L42_SFTRAMP_RATE,			0xA4 },
	{ CS42L42_SLOW_START_ENABLE,		0x70 },
	{ CS42L42_I2C_DEBOUNCE,			0x88 },
	{ CS42L42_I2C_STRETCH,			0x03 },
	{ CS42L42_I2C_TIMEOUT,			0xB7 },
	{ CS42L42_PWR_CTL1,			0xFF },
	{ CS42L42_PWR_CTL2,			0x84 },
	{ CS42L42_PWR_CTL3,			0x20 },
	{ CS42L42_RSENSE_CTL1,			0x40 },
	{ CS42L42_RSENSE_CTL2,			0x00 },
	{ CS42L42_OSC_SWITCH,			0x00 },
	{ CS42L42_RSENSE_CTL3,			0x1B },
	{ CS42L42_TSENSE_CTL,			0x1B },
	{ CS42L42_TSRS_INT_DISABLE,		0x00 },
	{ CS42L42_HSDET_CTL1,			0x77 },
	{ CS42L42_HSDET_CTL2,			0x00 },
	{ CS42L42_HS_SWITCH_CTL,		0xF3 },
	{ CS42L42_HS_CLAMP_DISABLE,		0x00 },
	{ CS42L42_MCLK_SRC_SEL,			0x00 },
	{ CS42L42_SPDIF_CLK_CFG,		0x00 },
	{ CS42L42_FSYNC_PW_LOWER,		0x00 },
	{ CS42L42_FSYNC_PW_UPPER,		0x00 },
	{ CS42L42_FSYNC_P_LOWER,		0xF9 },
	{ CS42L42_FSYNC_P_UPPER,		0x00 },
	{ CS42L42_ASP_CLK_CFG,			0x00 },
	{ CS42L42_ASP_FRM_CFG,			0x10 },
	{ CS42L42_FS_RATE_EN,			0x00 },
	{ CS42L42_IN_ASRC_CLK,			0x00 },
	{ CS42L42_OUT_ASRC_CLK,			0x00 },
	{ CS42L42_PLL_DIV_CFG1,			0x00 },
	{ CS42L42_ADC_OVFL_INT_MASK,		0x01 },
	{ CS42L42_MIXER_INT_MASK,		0x0F },
	{ CS42L42_SRC_INT_MASK,			0x0F },
	{ CS42L42_ASP_RX_INT_MASK,		0x1F },
	{ CS42L42_ASP_TX_INT_MASK,		0x0F },
	{ CS42L42_CODEC_INT_MASK,		0x03 },
	{ CS42L42_SRCPL_INT_MASK,		0x7F },
	{ CS42L42_VPMON_INT_MASK,		0x01 },
	{ CS42L42_PLL_LOCK_INT_MASK,		0x01 },
	{ CS42L42_TSRS_PLUG_INT_MASK,		0x0F },
	{ CS42L42_PLL_CTL1,			0x00 },
	{ CS42L42_PLL_DIV_FRAC0,		0x00 },
	{ CS42L42_PLL_DIV_FRAC1,		0x00 },
	{ CS42L42_PLL_DIV_FRAC2,		0x00 },
	{ CS42L42_PLL_DIV_INT,			0x40 },
	{ CS42L42_PLL_CTL3,			0x10 },
	{ CS42L42_PLL_CAL_RATIO,		0x80 },
	{ CS42L42_PLL_CTL4,			0x03 },
	{ CS42L42_LOAD_DET_EN,			0x00 },
	{ CS42L42_HSBIAS_SC_AUTOCTL,		0x03 },
	{ CS42L42_WAKE_CTL,			0xC0 },
	{ CS42L42_ADC_DISABLE_MUTE,		0x00 },
	{ CS42L42_TIPSENSE_CTL,			0x02 },
	{ CS42L42_MISC_DET_CTL,			0x03 },
	{ CS42L42_MIC_DET_CTL1,			0x1F },
	{ CS42L42_MIC_DET_CTL2,			0x2F },
	{ CS42L42_DET_INT1_MASK,		0xE0 },
	{ CS42L42_DET_INT2_MASK,		0xFF },
	{ CS42L42_HS_BIAS_CTL,			0xC2 },
	{ CS42L42_ADC_CTL,			0x00 },
	{ CS42L42_ADC_VOLUME,			0x00 },
	{ CS42L42_ADC_WNF_HPF_CTL,		0x71 },
	{ CS42L42_DAC_CTL1,			0x00 },
	{ CS42L42_DAC_CTL2,			0x02 },
	{ CS42L42_HP_CTL,			0x0D },
	{ CS42L42_CLASSH_CTL,			0x07 },
	{ CS42L42_MIXER_CHA_VOL,		0x3F },
	{ CS42L42_MIXER_ADC_VOL,		0x3F },
	{ CS42L42_MIXER_CHB_VOL,		0x3F },
	{ CS42L42_EQ_COEF_IN0,			0x00 },
	{ CS42L42_EQ_COEF_IN1,			0x00 },
	{ CS42L42_EQ_COEF_IN2,			0x00 },
	{ CS42L42_EQ_COEF_IN3,			0x00 },
	{ CS42L42_EQ_COEF_RW,			0x00 },
	{ CS42L42_EQ_COEF_OUT0,			0x00 },
	{ CS42L42_EQ_COEF_OUT1,			0x00 },
	{ CS42L42_EQ_COEF_OUT2,			0x00 },
	{ CS42L42_EQ_COEF_OUT3,			0x00 },
	{ CS42L42_EQ_INIT_STAT,			0x00 },
	{ CS42L42_EQ_START_FILT,		0x00 },
	{ CS42L42_EQ_MUTE_CTL,			0x00 },
	{ CS42L42_SP_RX_CH_SEL,			0x04 },
	{ CS42L42_SP_RX_ISOC_CTL,		0x04 },
	{ CS42L42_SP_RX_FS,			0x8C },
	{ CS42l42_SPDIF_CH_SEL,			0x0E },
	{ CS42L42_SP_TX_ISOC_CTL,		0x04 },
	{ CS42L42_SP_TX_FS,			0xCC },
	{ CS42L42_SPDIF_SW_CTL1,		0x3F },
	{ CS42L42_SRC_SDIN_FS,			0x40 },
	{ CS42L42_SRC_SDOUT_FS,			0x40 },
	{ CS42L42_SPDIF_CTL1,			0x01 },
	{ CS42L42_SPDIF_CTL2,			0x00 },
	{ CS42L42_SPDIF_CTL3,			0x00 },
	{ CS42L42_SPDIF_CTL4,			0x42 },
	{ CS42L42_ASP_TX_SZ_EN,			0x00 },
	{ CS42L42_ASP_TX_CH_EN,			0x00 },
	{ CS42L42_ASP_TX_CH_AP_RES,		0x0F },
	{ CS42L42_ASP_TX_CH1_BIT_MSB,		0x00 },
	{ CS42L42_ASP_TX_CH1_BIT_LSB,		0x00 },
	{ CS42L42_ASP_TX_HIZ_DLY_CFG,		0x00 },
	{ CS42L42_ASP_TX_CH2_BIT_MSB,		0x00 },
	{ CS42L42_ASP_TX_CH2_BIT_LSB,		0x00 },
	{ CS42L42_ASP_RX_DAI0_EN,		0x00 },
	{ CS42L42_ASP_RX_DAI0_CH1_AP_RES,	0x03 },
	{ CS42L42_ASP_RX_DAI0_CH1_BIT_MSB,	0x00 },
	{ CS42L42_ASP_RX_DAI0_CH1_BIT_LSB,	0x00 },
	{ CS42L42_ASP_RX_DAI0_CH2_AP_RES,	0x03 },
	{ CS42L42_ASP_RX_DAI0_CH2_BIT_MSB,	0x00 },
	{ CS42L42_ASP_RX_DAI0_CH2_BIT_LSB,	0x00 },
	{ CS42L42_ASP_RX_DAI0_CH3_AP_RES,	0x03 },
	{ CS42L42_ASP_RX_DAI0_CH3_BIT_MSB,	0x00 },
	{ CS42L42_ASP_RX_DAI0_CH3_BIT_LSB,	0x00 },
	{ CS42L42_ASP_RX_DAI0_CH4_AP_RES,	0x03 },
	{ CS42L42_ASP_RX_DAI0_CH4_BIT_MSB,	0x00 },
	{ CS42L42_ASP_RX_DAI0_CH4_BIT_LSB,	0x00 },
	{ CS42L42_ASP_RX_DAI1_CH1_AP_RES,	0x03 },
	{ CS42L42_ASP_RX_DAI1_CH1_BIT_MSB,	0x00 },
	{ CS42L42_ASP_RX_DAI1_CH1_BIT_LSB,	0x00 },
	{ CS42L42_ASP_RX_DAI1_CH2_AP_RES,	0x03 },
	{ CS42L42_ASP_RX_DAI1_CH2_BIT_MSB,	0x00 },
	{ CS42L42_ASP_RX_DAI1_CH2_BIT_LSB,	0x00 },
};

/*
 * This is all the same as for CS42L42 but we
 * replace the on-reset register defaults.
 */
static const struct regmap_config cs42l83_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.readable_reg = cs42l42_readable_register,
	.volatile_reg = cs42l42_volatile_register,

	.ranges = &cs42l42_page_range,
	.num_ranges = 1,

	.max_register = CS42L42_MAX_REGISTER,
	.reg_defaults = cs42l83_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(cs42l83_reg_defaults),
	.cache_type = REGCACHE_MAPLE,

	.use_single_read = true,
	.use_single_write = true,
};

static void cs42l83_restore_locked(struct cs42l42_private *cs42l83)
{
	lockdep_assert_held(&cs42l83->restore_lock);
	if (!cs42l83->restore_pending)
		return;
	cs42l83->restore_pending = false;
	cs42l42_resume_restore(cs42l83->dev);
}

/* ASoC suspends the codec only while it's idle, and resumes it from its card resume. */
static int cs42l83_component_suspend(struct snd_soc_component *component)
{
	struct cs42l42_private *cs42l83 = snd_soc_component_get_drvdata(component);

	guard(mutex)(&cs42l83->restore_lock);
	cs42l83->asoc_suspended = true;
	return 0;
}

/*
 * From ASoC's deferred card resume, before DAPM and before user space may use the card. It
 * can come before the device's resume has powered the codec up: wait for that. Whichever
 * order the two take the lock in, the restore runs once, on a powered codec.
 */
static int cs42l83_component_resume(struct snd_soc_component *component)
{
	struct cs42l42_private *cs42l83 = snd_soc_component_get_drvdata(component);

	if (!wait_for_completion_timeout(&cs42l83->powered,
					 msecs_to_jiffies(CS42L83_POWERED_WAIT_MS)))
		dev_dbg(cs42l83->dev, "card resumed before the codec\n");

	guard(mutex)(&cs42l83->restore_lock);
	cs42l83->asoc_suspended = false;
	cs42l83_restore_locked(cs42l83);
	return 0;
}

static int cs42l83_i2c_probe(struct i2c_client *i2c_client)
{
	struct device *dev = &i2c_client->dev;
	struct snd_soc_component_driver *component_drv;
	struct cs42l42_private *cs42l83;
	struct regmap *regmap;
	int ret;

	cs42l83 = devm_kzalloc(dev, sizeof(*cs42l83), GFP_KERNEL);
	if (!cs42l83)
		return -ENOMEM;

	regmap = devm_regmap_init_i2c(i2c_client, &cs42l83_regmap);
	if (IS_ERR(regmap))
		return dev_err_probe(&i2c_client->dev, PTR_ERR(regmap),
				     "regmap_init() failed\n");

	cs42l83->devid = CS42L83_CHIP_ID;
	cs42l83->dev = dev;
	cs42l83->regmap = regmap;
	cs42l83->irq = i2c_client->irq;
	mutex_init(&cs42l83->restore_lock);
	init_completion(&cs42l83->powered);
	complete_all(&cs42l83->powered);

	component_drv = devm_kmemdup(dev, &cs42l42_soc_component, sizeof(cs42l42_soc_component),
				     GFP_KERNEL);
	if (!component_drv)
		return -ENOMEM;
	component_drv->suspend = cs42l83_component_suspend;
	component_drv->resume = cs42l83_component_resume;

	ret = cs42l42_common_probe(cs42l83, component_drv, &cs42l42_dai);
	if (ret)
		return ret;

	return cs42l42_init(cs42l83);
}

static void cs42l83_i2c_remove(struct i2c_client *i2c_client)
{
	struct cs42l42_private *cs42l83 = dev_get_drvdata(&i2c_client->dev);

	cs42l42_common_remove(cs42l83);
}

/* A restore still due from the last wake goes first: the shutdown runs on the codec. */
static int cs42l83_i2c_suspend(struct device *dev)
{
	struct cs42l42_private *cs42l83 = dev_get_drvdata(dev);

	scoped_guard(mutex, &cs42l83->restore_lock)
		cs42l83_restore_locked(cs42l83);
	reinit_completion(&cs42l83->powered);

	return cs42l42_suspend(dev);
}

static int cs42l83_i2c_resume(struct device *dev)
{
	struct cs42l42_private *cs42l83 = dev_get_drvdata(dev);
	bool defer;
	int ret;

	ret = cs42l42_resume(dev);
	if (ret) {
		complete_all(&cs42l83->powered);
		return ret;
	}

	scoped_guard(mutex, &cs42l83->restore_lock) {
		defer = READ_ONCE(defer_restore) && cs42l83->init_done && cs42l83->asoc_suspended;
		cs42l83->restore_pending = defer;
	}
	complete_all(&cs42l83->powered);
	if (!defer)
		cs42l42_resume_restore(dev);

	return 0;
}

static const struct dev_pm_ops cs42l83_i2c_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(cs42l83_i2c_suspend, cs42l83_i2c_resume)
};

static const struct of_device_id __maybe_unused cs42l83_of_match[] = {
	{ .compatible = "cirrus,cs42l83", },
	{}
};
MODULE_DEVICE_TABLE(of, cs42l83_of_match);

static struct i2c_driver cs42l83_i2c_driver = {
	.driver = {
		.name = "cs42l83",
		.pm = pm_ptr(&cs42l83_i2c_pm_ops),
		.of_match_table = of_match_ptr(cs42l83_of_match),
		},
	.probe = cs42l83_i2c_probe,
	.remove = cs42l83_i2c_remove,
};

module_i2c_driver(cs42l83_i2c_driver);

MODULE_DESCRIPTION("ASoC CS42L83 I2C driver");
MODULE_AUTHOR("Martin Povišer <povik+lin@cutebit.org>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("SND_SOC_CS42L42_CORE");
