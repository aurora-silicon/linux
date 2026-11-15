// SPDX-License-Identifier: GPL-2.0-only
//
// KUnit tests for the clock switching in cs42l42_mute_stream().
//
// The codec runs from its internal oscillator while no stream is active.
// Unmuting the first stream moves it onto SCLK, through the PLL if the
// selected clock configuration needs one. Once the codec has left the
// oscillator it needs a working clock to answer on I2C, so these tests
// check when that switch happens and when it must not happen.

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/wait.h>
#include <sound/core.h>
#include <sound/cs42l42.h>
#include <sound/pcm.h>
#include <sound/soc.h>

#include "cs42l42.h"

#define CS42L42_TEST_MAX_WRITES		64

#define CS42L42_TEST_HP_MUTE_MASK	(CS42L42_HP_ANA_AMUTE_MASK | \
					 CS42L42_HP_ANA_BMUTE_MASK)

/* PLL_LOCK_STATUS bit polled by cs42l42_mute_stream() */
#define CS42L42_TEST_PLL_LOCKED		BIT(0)

struct cs42l42_test_write {
	unsigned int reg;
	unsigned int val;
};

struct cs42l42_test_priv {
	struct kunit *test;

	struct cs42l42_private cs42l42;
	struct snd_soc_component component;
	struct snd_soc_dai dai;
	struct snd_soc_card soc_card;
	struct snd_card card;

	/* Register contents seen by the driver */
	u8 *regs;

	/* Every register write made by the driver, in order */
	struct cs42l42_test_write writes[CS42L42_TEST_MAX_WRITES];
	unsigned int num_writes;

	unsigned int num_lock_reads;
	bool pll_locks;
};

struct cs42l42_test_clk_param {
	unsigned int sclk;
	unsigned int rate;
	bool pll_divout_step;
};

struct cs42l42_test_power_param {
	unsigned int state;
	const char *name;
};

/* Clock configurations that run the codec from the PLL */
static const struct cs42l42_test_clk_param cs42l42_test_pll_clks[] = {
	{ .sclk = 1536000, .rate = 48000, .pll_divout_step = true },
	{ .sclk = 3072000, .rate = 48000, .pll_divout_step = false },
};

static const struct cs42l42_test_power_param cs42l42_test_low_power[] = {
	{ .state = SNDRV_CTL_POWER_D1, .name = "D1" },
	{ .state = SNDRV_CTL_POWER_D2, .name = "D2" },
	{ .state = SNDRV_CTL_POWER_D3hot, .name = "D3hot" },
	{ .state = SNDRV_CTL_POWER_D3cold, .name = "D3cold" },
};

static int cs42l42_test_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct cs42l42_test_priv *priv = context;

	if (reg > CS42L42_MAX_REGISTER)
		return -EINVAL;

	if (reg == CS42L42_PLL_LOCK_STATUS) {
		priv->num_lock_reads++;
		*val = priv->pll_locks ? CS42L42_TEST_PLL_LOCKED : 0;
		return 0;
	}

	*val = priv->regs[reg];

	return 0;
}

static int cs42l42_test_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct cs42l42_test_priv *priv = context;

	if (reg > CS42L42_MAX_REGISTER)
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

static const struct regmap_config cs42l42_test_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = CS42L42_MAX_REGISTER,
	.reg_read = cs42l42_test_reg_read,
	.reg_write = cs42l42_test_reg_write,
	.cache_type = REGCACHE_NONE,
};

static void cs42l42_test_clear_log(struct cs42l42_test_priv *priv)
{
	priv->num_writes = 0;
	priv->num_lock_reads = 0;
}

/* Index of the first write to @reg at or after index @from, or -1 */
static int cs42l42_test_find_write(struct cs42l42_test_priv *priv,
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
static int cs42l42_test_last_write(struct cs42l42_test_priv *priv, unsigned int reg)
{
	unsigned int i;

	for (i = priv->num_writes; i > 0; i--) {
		if (priv->writes[i - 1].reg == reg)
			return i - 1;
	}

	return -1;
}

static unsigned int cs42l42_test_count_writes(struct cs42l42_test_priv *priv,
					      unsigned int reg)
{
	unsigned int i, n = 0;

	for (i = 0; i < priv->num_writes; i++) {
		if (priv->writes[i].reg == reg)
			n++;
	}

	return n;
}

/* Select the clock configuration the way hw_params does */
static void cs42l42_test_set_clk(struct kunit *test, unsigned int sclk,
				 unsigned int rate)
{
	struct cs42l42_test_priv *priv = test->priv;

	KUNIT_ASSERT_EQ(test, cs42l42_pll_config(&priv->component, sclk, rate), 0);
	cs42l42_test_clear_log(priv);
}

static void cs42l42_test_set_power(struct cs42l42_test_priv *priv, unsigned int state)
{
	snd_power_change_state(priv->soc_card.snd_card, state);
}

/*
 * Check that the writes logged since the last clear are a complete switch
 * from the oscillator to SCLK through the PLL, with the headphone unmuted
 * last. The PLL output divider is only rewritten if it still holds the
 * startup value from cs42l42_pll_config(), so @check_divout must be false
 * when an earlier unmute has already set the final value.
 */
static void cs42l42_test_expect_switch_to_pll(struct kunit *test,
					      const struct cs42l42_test_clk_param *param,
					      bool check_divout)
{
	struct cs42l42_test_priv *priv = test->priv;
	int pll_start, divout, mclk_sel, sclk, hp;

	pll_start = cs42l42_test_find_write(priv, CS42L42_PLL_CTL1, 0);
	KUNIT_ASSERT_GE(test, pll_start, 0);
	KUNIT_EXPECT_TRUE(test, priv->writes[pll_start].val & CS42L42_PLL_START_MASK);

	divout = cs42l42_test_find_write(priv, CS42L42_PLL_CTL3, pll_start);
	if (check_divout && param->pll_divout_step)
		KUNIT_EXPECT_GT(test, divout, pll_start);
	else if (check_divout)
		KUNIT_EXPECT_LT(test, divout, 0);

	/* The PLL output must only be selected once the PLL has locked */
	KUNIT_EXPECT_GT(test, priv->num_lock_reads, 0);
	mclk_sel = cs42l42_test_find_write(priv, CS42L42_MCLK_SRC_SEL, pll_start);
	KUNIT_ASSERT_GT(test, mclk_sel, pll_start);
	KUNIT_EXPECT_EQ(test, priv->writes[mclk_sel].val & CS42L42_MCLK_SRC_SEL_MASK,
			CS42L42_MCLK_SRC_SEL_MASK);

	sclk = cs42l42_test_find_write(priv, CS42L42_OSC_SWITCH, mclk_sel);
	KUNIT_ASSERT_GT(test, sclk, mclk_sel);
	KUNIT_EXPECT_EQ(test, priv->writes[sclk].val, CS42L42_SCLK_PRESENT_MASK);

	hp = cs42l42_test_find_write(priv, CS42L42_HP_CTL, sclk);
	KUNIT_ASSERT_GT(test, hp, sclk);
	KUNIT_EXPECT_EQ(test, priv->writes[hp].val & CS42L42_TEST_HP_MUTE_MASK, 0);
}

/*
 * ASoC resume sets the card to D2, unmutes the playback DAIs that were
 * active at suspend and only then sets D0. That early unmute must not move
 * the codec off the oscillator: the stream is still suspended and SCLK may
 * not be running yet.
 */
static void cs42l42_test_unmute_below_d0_does_nothing(struct kunit *test)
{
	const struct cs42l42_test_power_param *param = test->param_value;
	const struct cs42l42_test_clk_param *clk = &cs42l42_test_pll_clks[0];
	struct cs42l42_test_priv *priv = test->priv;

	if (!IS_ENABLED(CONFIG_PM))
		kunit_skip(test, "card power state needs CONFIG_PM");

	/* A PLL configuration whose PLL would lock if it were started */
	cs42l42_test_set_clk(test, clk->sclk, clk->rate);
	cs42l42_test_set_power(priv, param->state);

	KUNIT_EXPECT_EQ(test, cs42l42_mute_stream(&priv->dai, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);

	KUNIT_EXPECT_EQ(test, priv->num_writes, 0);
	KUNIT_EXPECT_EQ(test, priv->num_lock_reads, 0);
	KUNIT_EXPECT_EQ(test, priv->cs42l42.stream_use, 0);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L42_HP_CTL] & CS42L42_TEST_HP_MUTE_MASK,
			CS42L42_TEST_HP_MUTE_MASK);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L42_OSC_SWITCH], 0);
}

/*
 * Use real ALSA RESUME/PREPARE ioctls and the ASoC trigger installed by
 * soc_new_pcm(). Only the host DAI and prepare callback are synthetic: the
 * host prepare supplies a working clock before calling the codec unmute.
 */
struct cs42l42_test_pcm {
	struct snd_soc_pcm_runtime rtd;
	struct snd_soc_dai_link link;
	struct snd_soc_dai_link_ch_map map;
	struct snd_soc_dai cpu;
	struct snd_soc_dai_driver cpu_driver;
	struct snd_soc_dai *dais[2];
	struct snd_pcm_runtime runtime;
	struct snd_pcm_mmap_status status;
	struct snd_pcm_mmap_control control;
	struct snd_pcm_runtime capture_runtime;
	struct snd_pcm_mmap_status capture_status;
	struct snd_pcm_group linked_group;
	struct snd_pcm_file pcm_file;
	struct file file;
	struct cs42l42_test_priv *priv;
	unsigned int resumes;
	unsigned int suspends;
	unsigned int prepares;
};

static int cs42l42_test_cpu_trigger(struct snd_pcm_substream *substream,
				    int cmd, struct snd_soc_dai *dai)
{
	struct cs42l42_test_pcm *pcm = container_of(snd_soc_substream_to_rtd(substream),
						 struct cs42l42_test_pcm, rtd);

	if (cmd == SNDRV_PCM_TRIGGER_RESUME)
		pcm->resumes++;
	else if (cmd == SNDRV_PCM_TRIGGER_SUSPEND)
		pcm->suspends++;
	return 0;
}

static const struct snd_soc_dai_ops cs42l42_test_cpu_ops = {
	.trigger = cs42l42_test_cpu_trigger,
};

static int cs42l42_test_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct cs42l42_test_pcm *pcm = container_of(snd_soc_substream_to_rtd(substream),
						 struct cs42l42_test_pcm, rtd);
	int ret;

	pcm->prepares++;
	pcm->priv->pll_locks = true;
	ret = snd_soc_pcm_dai_prepare(substream);
	if (ret)
		return ret;
	return cs42l42_mute_stream(&pcm->priv->dai, 0, substream->stream);
}

static int cs42l42_test_pcm_ioctl(struct snd_pcm_substream *substream,
				  unsigned int cmd, void *arg)
{
	return cmd == SNDRV_PCM_IOCTL1_RESET ? 0 : -EINVAL;
}

static void cs42l42_test_pcm_free(void *data)
{
	struct cs42l42_test_pcm *pcm = data;
	int stream;

	if (pcm->rtd.pcm) {
		for_each_pcm_streams(stream) {
			struct snd_pcm_substream *substream;

			substream = pcm->rtd.pcm->streams[stream].substream;
			if (!substream)
				continue;
			substream->runtime = NULL;
			if (snd_pcm_stream_linked(substream)) {
				list_move(&substream->link_list, &substream->self_group.substreams);
				substream->group = &substream->self_group;
			}
		}
	}
	snd_card_free(pcm->rtd.card->snd_card);
}

static struct cs42l42_test_pcm *cs42l42_test_pcm_init(struct kunit *test)
{
	struct cs42l42_test_priv *priv = test->priv;
	struct cs42l42_test_pcm *pcm;
	struct snd_pcm_substream *substream;
	struct snd_card *card;

	pcm = kunit_kzalloc(test, sizeof(*pcm), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pcm);
	KUNIT_ASSERT_EQ(test, snd_card_new(priv->component.dev, -1, NULL,
					   THIS_MODULE, 0, &card), 0);
	priv->soc_card.snd_card = card;
	priv->soc_card.dev = priv->component.dev;
	pcm->priv = priv;
	pcm->rtd.dev = priv->component.dev;
	pcm->rtd.card = &priv->soc_card;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, cs42l42_test_pcm_free, pcm), 0);
	pcm->rtd.dai_link = &pcm->link;
	pcm->rtd.dais = pcm->dais;
	pcm->link.name = "cs42l42-resume-test";
	pcm->link.stream_name = pcm->link.name;
	pcm->link.num_cpus = 1;
	pcm->link.num_codecs = 1;
	pcm->link.ch_maps = &pcm->map;
	pcm->cpu_driver = cs42l42_dai;
	pcm->cpu_driver.ops = &cs42l42_test_cpu_ops;
	pcm->cpu.driver = &pcm->cpu_driver;
	pcm->cpu.name = "clock-provider";
	pcm->cpu.dev = priv->component.dev;
	pcm->cpu.component = &priv->component;
	pcm->dais[0] = &pcm->cpu;
	pcm->dais[1] = &priv->dai;
	KUNIT_ASSERT_EQ(test, soc_new_pcm(&pcm->rtd), 0);
	pcm->rtd.ops.prepare = cs42l42_test_pcm_prepare;
	pcm->rtd.ops.ioctl = cs42l42_test_pcm_ioctl;
	substream = pcm->rtd.pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
	substream->private_data = &pcm->rtd;
	substream->runtime = &pcm->runtime;
	pcm->runtime.status = &pcm->status;
	pcm->runtime.control = &pcm->control;
	pcm->runtime.info = SNDRV_PCM_INFO_RESUME;
	pcm->runtime.suspended_state = SNDRV_PCM_STATE_RUNNING;
	pcm->runtime.period_size = 128;
	mutex_init(&pcm->runtime.buffer_mutex);
	init_waitqueue_head(&pcm->runtime.sleep);
	init_waitqueue_head(&pcm->runtime.tsleep);
	pcm->pcm_file.substream = substream;
	pcm->file.private_data = &pcm->pcm_file;
	return pcm;
}

static long cs42l42_test_pcm_command(struct cs42l42_test_pcm *pcm, unsigned int cmd)
{
	return snd_pcm_f_ops[SNDRV_PCM_STREAM_PLAYBACK].unlocked_ioctl(&pcm->file, cmd, 0);
}

static void cs42l42_test_resume_requires_prepare(struct kunit *test)
{
	const struct cs42l42_test_clk_param *clk = &cs42l42_test_pll_clks[0];
	struct cs42l42_test_priv *priv = test->priv;
	struct cs42l42_test_pcm *pcm;

	if (!IS_ENABLED(CONFIG_PM))
		kunit_skip(test, "card power state needs CONFIG_PM");
	pcm = cs42l42_test_pcm_init(test);
	KUNIT_ASSERT_NOT_NULL(test, pcm);
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D0);
	cs42l42_test_set_clk(test, clk->sclk, clk->rate);
	__snd_pcm_set_state(&pcm->runtime, SNDRV_PCM_STATE_SETUP);
	KUNIT_ASSERT_EQ(test, cs42l42_test_pcm_command(pcm, SNDRV_PCM_IOCTL_PREPARE), 0);
	KUNIT_EXPECT_FALSE(test, pcm->runtime.info & SNDRV_PCM_INFO_RESUME);
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D3hot);
	KUNIT_ASSERT_EQ(test, cs42l42_mute_stream(&priv->dai, 1, SNDRV_PCM_STREAM_PLAYBACK), 0);
	__snd_pcm_set_state(&pcm->runtime, SNDRV_PCM_STATE_SUSPENDED);
	cs42l42_test_clear_log(priv);
	priv->pll_locks = false;
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D2);
	KUNIT_ASSERT_EQ(test, cs42l42_mute_stream(&priv->dai, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);
	KUNIT_EXPECT_EQ(test, priv->num_writes, 0);
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D0);

	/* ALSA rejects before the real ASoC trigger can start any DAI. */
	KUNIT_EXPECT_EQ(test, cs42l42_test_pcm_command(pcm, SNDRV_PCM_IOCTL_RESUME), -ENOSYS);
	KUNIT_EXPECT_EQ(test, pcm->resumes, 0);
	KUNIT_EXPECT_EQ(test, pcm->suspends, 0);
	KUNIT_EXPECT_EQ(test, pcm->runtime.state, SNDRV_PCM_STATE_SUSPENDED);
	KUNIT_EXPECT_EQ(test, pcm->prepares, 1);
	KUNIT_EXPECT_EQ(test, priv->num_writes, 0);

	/* ALSA's documented -ENOSYS recovery is PREPARE, not a D0 callback. */
	KUNIT_ASSERT_EQ(test, cs42l42_test_pcm_command(pcm, SNDRV_PCM_IOCTL_PREPARE), 0);
	KUNIT_EXPECT_EQ(test, pcm->prepares, 2);
	KUNIT_EXPECT_EQ(test, pcm->runtime.state, SNDRV_PCM_STATE_PREPARED);
	cs42l42_test_expect_switch_to_pll(test, clk, false);
	KUNIT_EXPECT_EQ(test, priv->cs42l42.stream_use, BIT(SNDRV_PCM_STREAM_PLAYBACK));
}

static void cs42l42_test_linked_resume_preflight(struct kunit *test)
{
	struct cs42l42_test_priv *priv = test->priv;
	struct cs42l42_test_pcm *pcm;
	struct snd_pcm_substream *playback, *capture;

	if (!IS_ENABLED(CONFIG_PM))
		kunit_skip(test, "card power state needs CONFIG_PM");
	pcm = cs42l42_test_pcm_init(test);
	KUNIT_ASSERT_NOT_NULL(test, pcm);
	playback = pcm->pcm_file.substream;
	capture = pcm->rtd.pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
	capture->private_data = &pcm->rtd;
	capture->runtime = &pcm->capture_runtime;
	pcm->capture_runtime.info = SNDRV_PCM_INFO_RESUME;
	pcm->capture_runtime.status = &pcm->capture_status;
	pcm->capture_runtime.suspended_state = SNDRV_PCM_STATE_RUNNING;
	__snd_pcm_set_state(&pcm->capture_runtime, SNDRV_PCM_STATE_SUSPENDED);
	__snd_pcm_set_state(&pcm->runtime, SNDRV_PCM_STATE_SUSPENDED);
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D0);
	KUNIT_ASSERT_EQ(test, snd_soc_pcm_dai_prepare(playback), 0);

	/* Same group layout as linked ALSA streams, with capture visited first. */
	spin_lock_init(&pcm->linked_group.lock);
	mutex_init(&pcm->linked_group.mutex);
	INIT_LIST_HEAD(&pcm->linked_group.substreams);
	refcount_set(&pcm->linked_group.refs, 2);
	list_move_tail(&capture->link_list, &pcm->linked_group.substreams);
	list_move_tail(&playback->link_list, &pcm->linked_group.substreams);
	capture->group = &pcm->linked_group;
	playback->group = &pcm->linked_group;

	KUNIT_EXPECT_EQ(test, cs42l42_test_pcm_command(pcm, SNDRV_PCM_IOCTL_RESUME), -ENOSYS);
	KUNIT_EXPECT_EQ(test, pcm->resumes, 0);
	KUNIT_EXPECT_EQ(test, pcm->suspends, 0);
	KUNIT_EXPECT_EQ(test, pcm->runtime.state, SNDRV_PCM_STATE_SUSPENDED);
	KUNIT_EXPECT_EQ(test, pcm->capture_runtime.state, SNDRV_PCM_STATE_SUSPENDED);
	KUNIT_EXPECT_TRUE(test, pcm->capture_runtime.info & SNDRV_PCM_INFO_RESUME);
	KUNIT_EXPECT_EQ(test, priv->num_writes, 0);
}

static void cs42l42_test_capture_resume_remains_supported(struct kunit *test)
{
	struct cs42l42_test_priv *priv = test->priv;
	struct cs42l42_test_pcm *pcm;
	struct snd_pcm_substream *capture;

	if (!IS_ENABLED(CONFIG_PM))
		kunit_skip(test, "card power state needs CONFIG_PM");
	pcm = cs42l42_test_pcm_init(test);
	KUNIT_ASSERT_NOT_NULL(test, pcm);
	capture = pcm->rtd.pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
	capture->private_data = &pcm->rtd;
	capture->runtime = &pcm->capture_runtime;
	pcm->capture_runtime.info = SNDRV_PCM_INFO_RESUME;
	pcm->capture_runtime.status = &pcm->capture_status;
	pcm->capture_runtime.suspended_state = SNDRV_PCM_STATE_RUNNING;
	__snd_pcm_set_state(&pcm->capture_runtime, SNDRV_PCM_STATE_SUSPENDED);
	pcm->pcm_file.substream = capture;
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D0);
	KUNIT_ASSERT_EQ(test, snd_soc_pcm_dai_prepare(capture), 0);
	KUNIT_EXPECT_EQ(test, cs42l42_test_pcm_command(pcm, SNDRV_PCM_IOCTL_RESUME), 0);
	KUNIT_EXPECT_EQ(test, pcm->resumes, 1);
	KUNIT_EXPECT_EQ(test, pcm->suspends, 0);
	KUNIT_EXPECT_EQ(test, pcm->capture_runtime.state, SNDRV_PCM_STATE_RUNNING);
	KUNIT_EXPECT_EQ(test, priv->num_writes, 0);
}

static void cs42l42_test_prepare_resume_capability_is_per_stream(struct kunit *test)
{
	struct cs42l42_test_priv *priv = test->priv;
	struct snd_pcm_runtime runtime = {
		.info = SNDRV_PCM_INFO_RESUME | SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_MMAP,
	};
	struct snd_pcm_substream substream = {
		.stream = SNDRV_PCM_STREAM_CAPTURE,
		.runtime = &runtime,
	};

	KUNIT_ASSERT_EQ(test, snd_soc_dai_prepare(&priv->dai, &substream), 0);
	KUNIT_EXPECT_EQ(test, runtime.info,
			SNDRV_PCM_INFO_RESUME | SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_MMAP);
	substream.stream = SNDRV_PCM_STREAM_PLAYBACK;
	KUNIT_ASSERT_EQ(test, snd_soc_dai_prepare(&priv->dai, &substream), 0);
	KUNIT_EXPECT_EQ(test, runtime.info, SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_MMAP);
	KUNIT_EXPECT_EQ(test, priv->num_writes, 0);
}

struct cs42l42_test_admission_param {
	const char *name;
	const char *root;
	size_t root_len;
	const char *codec;
	size_t codec_len;
	bool soundwire;
	bool expected;
};

#define CS42L42_TEST_M1_ROOT(board) "apple," board "\0apple,t8103\0apple,arm-platform"
#define CS42L42_TEST_ADMISSION(label, machine, device, sdw, admitted) \
	{ label, machine, sizeof(machine), device, sizeof(device), sdw, admitted }

static const struct cs42l42_test_admission_param cs42l42_test_admissions[] = {
	CS42L42_TEST_ADMISSION("J274 CS42L83", CS42L42_TEST_M1_ROOT("j274"),
			       "cirrus,cs42l83", false, true),
	CS42L42_TEST_ADMISSION("J293 CS42L83", CS42L42_TEST_M1_ROOT("j293"),
			       "cirrus,cs42l83", false, true),
	CS42L42_TEST_ADMISSION("J313 CS42L83", CS42L42_TEST_M1_ROOT("j313"),
			       "cirrus,cs42l83", false, true),
	CS42L42_TEST_ADMISSION("J456 CS42L83", CS42L42_TEST_M1_ROOT("j456"),
			       "cirrus,cs42l83", false, true),
	CS42L42_TEST_ADMISSION("J457 CS42L83", CS42L42_TEST_M1_ROOT("j457"),
			       "cirrus,cs42l83", false, true),
	CS42L42_TEST_ADMISSION("base-M1 CS42L42", "apple,t8103",
			       "cirrus,cs42l42", false, true),
	CS42L42_TEST_ADMISSION("J700 CS42L83", "apple,j700\0apple,t8140",
			       "cirrus,cs42l83", false, false),
	CS42L42_TEST_ADMISSION("M1 Pro", "apple,j314s\0apple,t6000",
			       "cirrus,cs42l83", false, false),
	CS42L42_TEST_ADMISSION("M2 Air", "apple,j413\0apple,t8112",
			       "cirrus,cs42l83", false, false),
	CS42L42_TEST_ADMISSION("other codec", "apple,t8103",
			       "cirrus,cs42l84", false, false),
	CS42L42_TEST_ADMISSION("absent codec OF node", "apple,t8103",
			       NULL, false, false),
	CS42L42_TEST_ADMISSION("ACPI/non-OF host", NULL,
			       "cirrus,cs42l42", false, false),
	CS42L42_TEST_ADMISSION("CS42L42 SoundWire", "apple,t8103",
			       "cirrus,cs42l42", true, false),
	CS42L42_TEST_ADMISSION("CS42L83 SoundWire", "apple,t8103",
			       "cirrus,cs42l83", true, false),
	CS42L42_TEST_ADMISSION("generic shared-BE host", "test,shared-be",
			       "cirrus,cs42l42", false, false),
	CS42L42_TEST_ADMISSION("root lookalike", "apple,t8103-other",
			       "cirrus,cs42l83", false, false),
};

static void cs42l42_test_resume_admission(struct kunit *test)
{
	const struct cs42l42_test_admission_param *param = test->param_value;
	struct property root_prop = {
		.name = "compatible", .value = (void *)param->root, .length = param->root_len,
	};
	struct property codec_prop = {
		.name = "compatible", .value = (void *)param->codec, .length = param->codec_len,
	};
	struct device_node root = { .full_name = "/", .properties = &root_prop };
	struct device_node codec = { .full_name = "/codec", .properties = &codec_prop };
	bool required;

	if (!IS_ENABLED(CONFIG_OF))
		kunit_skip(test, "admission matching needs CONFIG_OF");
	required = cs42l42_resume_workaround_required(param->root ? &root : NULL,
						      param->codec ? &codec : NULL,
						      param->soundwire);
	KUNIT_EXPECT_EQ(test, required, param->expected);
}

/* Excluded hosts keep their sole D2 unmute and transparent PCM resume. */
static void cs42l42_test_unadmitted_resume_without_prepare(struct kunit *test)
{
	struct cs42l42_test_priv *priv = test->priv;
	struct cs42l42_test_pcm *pcm;

	if (!IS_ENABLED(CONFIG_PM))
		kunit_skip(test, "card power state needs CONFIG_PM");
	priv->cs42l42.resume_reprepare = false;
	pcm = cs42l42_test_pcm_init(test);
	KUNIT_ASSERT_NOT_NULL(test, pcm);
	cs42l42_test_set_clk(test, 3072000, 48000);
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D0);
	__snd_pcm_set_state(&pcm->runtime, SNDRV_PCM_STATE_SETUP);
	KUNIT_ASSERT_EQ(test, cs42l42_test_pcm_command(pcm, SNDRV_PCM_IOCTL_PREPARE), 0);
	KUNIT_EXPECT_TRUE(test, pcm->runtime.info & SNDRV_PCM_INFO_RESUME);
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D3hot);
	KUNIT_ASSERT_EQ(test, cs42l42_mute_stream(&priv->dai, 1, SNDRV_PCM_STREAM_PLAYBACK), 0);
	__snd_pcm_set_state(&pcm->runtime, SNDRV_PCM_STATE_SUSPENDED);
	cs42l42_test_clear_log(priv);
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D2);
	KUNIT_ASSERT_EQ(test, cs42l42_mute_stream(&priv->dai, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);
	cs42l42_test_expect_switch_to_pll(test, &cs42l42_test_pll_clks[1], false);
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D0);
	cs42l42_test_clear_log(priv);
	KUNIT_EXPECT_EQ(test, cs42l42_test_pcm_command(pcm, SNDRV_PCM_IOCTL_RESUME), 0);
	KUNIT_EXPECT_EQ(test, pcm->runtime.state, SNDRV_PCM_STATE_RUNNING);
	KUNIT_EXPECT_EQ(test, pcm->prepares, 1);
	KUNIT_EXPECT_EQ(test, pcm->resumes, 1);
	KUNIT_EXPECT_EQ(test, priv->cs42l42.stream_use, BIT(SNDRV_PCM_STREAM_PLAYBACK));
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L42_HP_CTL] & CS42L42_TEST_HP_MUTE_MASK, 0);
	KUNIT_EXPECT_EQ(test, priv->num_writes, 0);
}

static void cs42l42_test_unadmitted_prepare_preserves_capabilities(struct kunit *test)
{
	struct cs42l42_test_priv *priv = test->priv;
	struct snd_pcm_runtime runtime = {
		.info = SNDRV_PCM_INFO_RESUME | SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_MMAP,
	};
	struct snd_pcm_substream substream = {
		.stream = SNDRV_PCM_STREAM_PLAYBACK, .runtime = &runtime,
	};

	priv->cs42l42.resume_reprepare = false;
	KUNIT_ASSERT_EQ(test, snd_soc_dai_prepare(&priv->dai, &substream), 0);
	KUNIT_EXPECT_EQ(test, runtime.info,
			SNDRV_PCM_INFO_RESUME | SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_MMAP);
	KUNIT_EXPECT_EQ(test, priv->num_writes, 0);
}

/* At D0 with a locked PLL the codec moves onto the PLL output and SCLK */
static void cs42l42_test_unmute_pll_locked(struct kunit *test)
{
	const struct cs42l42_test_clk_param *param = test->param_value;
	struct cs42l42_test_priv *priv = test->priv;

	cs42l42_test_set_clk(test, param->sclk, param->rate);

	KUNIT_EXPECT_EQ(test, cs42l42_mute_stream(&priv->dai, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);

	cs42l42_test_expect_switch_to_pll(test, param, true);
	KUNIT_EXPECT_EQ(test, priv->cs42l42.stream_use, BIT(SNDRV_PCM_STREAM_PLAYBACK));
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L42_HP_CTL] & CS42L42_TEST_HP_MUTE_MASK, 0);
}

/*
 * If the PLL does not lock the codec must stay on the oscillator with the
 * PLL stopped and the headphone muted, and the error must be returned.
 */
static void cs42l42_test_unmute_pll_lock_timeout(struct kunit *test)
{
	const struct cs42l42_test_clk_param *param = test->param_value;
	struct cs42l42_test_priv *priv = test->priv;
	int pll_stop;

	cs42l42_test_set_clk(test, param->sclk, param->rate);
	priv->pll_locks = false;

	KUNIT_EXPECT_EQ(test, cs42l42_mute_stream(&priv->dai, 0, SNDRV_PCM_STREAM_PLAYBACK),
			-ETIMEDOUT);
	KUNIT_EXPECT_GT(test, priv->num_lock_reads, 0);

	/* The PLL was started, then stopped again */
	pll_stop = cs42l42_test_last_write(priv, CS42L42_PLL_CTL1);
	KUNIT_ASSERT_GE(test, pll_stop, 0);
	KUNIT_EXPECT_EQ(test, priv->writes[pll_stop].val & CS42L42_PLL_START_MASK, 0);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L42_PLL_CTL1] & CS42L42_PLL_START_MASK, 0);

	/* Still on the oscillator */
	KUNIT_EXPECT_EQ(test, cs42l42_test_count_writes(priv, CS42L42_MCLK_SRC_SEL), 0);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L42_MCLK_SRC_SEL] & CS42L42_MCLK_SRC_SEL_MASK, 0);
	KUNIT_EXPECT_EQ(test, cs42l42_test_count_writes(priv, CS42L42_OSC_SWITCH), 0);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L42_OSC_SWITCH], 0);

	/* Headphone still muted, stream not counted as running */
	KUNIT_EXPECT_EQ(test, cs42l42_test_count_writes(priv, CS42L42_HP_CTL), 0);
	KUNIT_EXPECT_EQ(test, priv->regs[CS42L42_HP_CTL] & CS42L42_TEST_HP_MUTE_MASK,
			CS42L42_TEST_HP_MUTE_MASK);
	KUNIT_EXPECT_EQ(test, priv->cs42l42.stream_use, 0);
}

/*
 * A failed unmute leaves stream_use clear, so the next unmute retries the
 * whole switch instead of assuming the codec is already on SCLK.
 */
static void cs42l42_test_unmute_after_pll_lock_timeout(struct kunit *test)
{
	const struct cs42l42_test_clk_param *param = test->param_value;
	struct cs42l42_test_priv *priv = test->priv;

	cs42l42_test_set_clk(test, param->sclk, param->rate);

	priv->pll_locks = false;
	KUNIT_EXPECT_LT(test, cs42l42_mute_stream(&priv->dai, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);

	cs42l42_test_clear_log(priv);
	priv->pll_locks = true;
	KUNIT_EXPECT_EQ(test, cs42l42_mute_stream(&priv->dai, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);

	cs42l42_test_expect_switch_to_pll(test, param, false);
	KUNIT_EXPECT_EQ(test, priv->cs42l42.stream_use, BIT(SNDRV_PCM_STREAM_PLAYBACK));
}

/* Muting the last stream goes back to the oscillator, then stops the PLL */
static void cs42l42_test_mute_returns_to_osc(struct kunit *test)
{
	const struct cs42l42_test_clk_param *param = test->param_value;
	struct cs42l42_test_priv *priv = test->priv;
	int hp, osc, mclk_sel, pll_stop;

	cs42l42_test_set_clk(test, param->sclk, param->rate);
	KUNIT_ASSERT_EQ(test, cs42l42_mute_stream(&priv->dai, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);
	cs42l42_test_clear_log(priv);

	KUNIT_EXPECT_EQ(test, cs42l42_mute_stream(&priv->dai, 1, SNDRV_PCM_STREAM_PLAYBACK), 0);

	hp = cs42l42_test_find_write(priv, CS42L42_HP_CTL, 0);
	KUNIT_ASSERT_GE(test, hp, 0);
	KUNIT_EXPECT_EQ(test, priv->writes[hp].val & CS42L42_TEST_HP_MUTE_MASK,
			CS42L42_TEST_HP_MUTE_MASK);

	osc = cs42l42_test_find_write(priv, CS42L42_OSC_SWITCH, hp);
	KUNIT_ASSERT_GT(test, osc, hp);
	KUNIT_EXPECT_EQ(test, priv->writes[osc].val, 0);

	mclk_sel = cs42l42_test_find_write(priv, CS42L42_MCLK_SRC_SEL, osc);
	KUNIT_ASSERT_GT(test, mclk_sel, osc);
	KUNIT_EXPECT_EQ(test, priv->writes[mclk_sel].val & CS42L42_MCLK_SRC_SEL_MASK, 0);

	pll_stop = cs42l42_test_find_write(priv, CS42L42_PLL_CTL1, mclk_sel);
	KUNIT_ASSERT_GT(test, pll_stop, mclk_sel);
	KUNIT_EXPECT_EQ(test, priv->writes[pll_stop].val & CS42L42_PLL_START_MASK, 0);

	KUNIT_EXPECT_EQ(test, priv->cs42l42.stream_use, 0);
}

/* Without the PLL the codec goes straight onto SCLK, no lock poll */
static void cs42l42_test_unmute_no_pll(struct kunit *test)
{
	struct cs42l42_test_priv *priv = test->priv;
	int sclk, hp;

	/* SCLK is used directly as MCLK */
	cs42l42_test_set_clk(test, 12288000, 48000);
	priv->pll_locks = false;

	KUNIT_EXPECT_EQ(test, cs42l42_mute_stream(&priv->dai, 0, SNDRV_PCM_STREAM_PLAYBACK), 0);

	KUNIT_EXPECT_EQ(test, priv->num_lock_reads, 0);
	KUNIT_EXPECT_EQ(test, cs42l42_test_count_writes(priv, CS42L42_PLL_CTL1), 0);
	KUNIT_EXPECT_EQ(test, cs42l42_test_count_writes(priv, CS42L42_MCLK_SRC_SEL), 0);

	sclk = cs42l42_test_find_write(priv, CS42L42_OSC_SWITCH, 0);
	KUNIT_ASSERT_GE(test, sclk, 0);
	KUNIT_EXPECT_EQ(test, priv->writes[sclk].val, CS42L42_SCLK_PRESENT_MASK);

	hp = cs42l42_test_find_write(priv, CS42L42_HP_CTL, sclk);
	KUNIT_ASSERT_GT(test, hp, sclk);
	KUNIT_EXPECT_EQ(test, priv->writes[hp].val & CS42L42_TEST_HP_MUTE_MASK, 0);

	KUNIT_EXPECT_EQ(test, priv->cs42l42.stream_use, BIT(SNDRV_PCM_STREAM_PLAYBACK));
}

static int cs42l42_test_case_init(struct kunit *test)
{
	struct cs42l42_test_priv *priv;
	struct regmap *regmap;
	struct device *dev;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv);
	priv->test = test;
	test->priv = priv;

	priv->regs = kunit_kzalloc(test, CS42L42_MAX_REGISTER + 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->regs);

	dev = kunit_device_register(test, "cs42l42_test_drv");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	regmap = devm_regmap_init(dev, NULL, priv, &cs42l42_test_regmap_config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, regmap);

	/* Construct the minimal set of driver structs */
	priv->cs42l42.dev = dev;
	priv->cs42l42.regmap = regmap;
	/* Existing clock-safety cases model the admitted base-M1 I2C path. */
	priv->cs42l42.resume_reprepare = true;

	priv->component.dev = dev;
	priv->component.card = &priv->soc_card;
	snd_soc_component_init_regmap(&priv->component, regmap);
	snd_soc_component_set_drvdata(&priv->component, &priv->cs42l42);

	priv->dai.component = &priv->component;
	priv->dai.dev = dev;
	priv->dai.name = cs42l42_dai.name;
	priv->dai.driver = &cs42l42_dai;

	priv->soc_card.snd_card = &priv->card;
#ifdef CONFIG_PM
	init_waitqueue_head(&priv->card.power_sleep);
	snd_refcount_init(&priv->card.power_ref);
#endif
	cs42l42_test_set_power(priv, SNDRV_CTL_POWER_D0);

	/* Idle codec: on the oscillator, PLL stopped, headphone muted */
	priv->regs[CS42L42_HP_CTL] = CS42L42_TEST_HP_MUTE_MASK;
	priv->pll_locks = true;

	return 0;
}

static void cs42l42_test_clk_desc(const struct cs42l42_test_clk_param *param, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "SCLK %u Hz, %u Hz", param->sclk, param->rate);
}

static void cs42l42_test_power_desc(const struct cs42l42_test_power_param *param, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s", param->name);
}

static void cs42l42_test_admission_desc(const struct cs42l42_test_admission_param *param,
					char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s", param->name);
}

KUNIT_ARRAY_PARAM(cs42l42_test_admissions, cs42l42_test_admissions, cs42l42_test_admission_desc);
KUNIT_ARRAY_PARAM(cs42l42_test_pll_clks, cs42l42_test_pll_clks, cs42l42_test_clk_desc);
KUNIT_ARRAY_PARAM(cs42l42_test_low_power, cs42l42_test_low_power, cs42l42_test_power_desc);

static struct kunit_case cs42l42_test_cases[] = {
	KUNIT_CASE_PARAM(cs42l42_test_resume_admission, cs42l42_test_admissions_gen_params),
	KUNIT_CASE(cs42l42_test_unadmitted_resume_without_prepare),
	KUNIT_CASE(cs42l42_test_unadmitted_prepare_preserves_capabilities),
	KUNIT_CASE_PARAM(cs42l42_test_unmute_below_d0_does_nothing,
			 cs42l42_test_low_power_gen_params),
	KUNIT_CASE(cs42l42_test_resume_requires_prepare),
	KUNIT_CASE(cs42l42_test_linked_resume_preflight),
	KUNIT_CASE(cs42l42_test_capture_resume_remains_supported),
	KUNIT_CASE(cs42l42_test_prepare_resume_capability_is_per_stream),
	KUNIT_CASE_PARAM(cs42l42_test_unmute_pll_locked, cs42l42_test_pll_clks_gen_params),
	KUNIT_CASE_PARAM(cs42l42_test_unmute_pll_lock_timeout, cs42l42_test_pll_clks_gen_params),
	KUNIT_CASE_PARAM(cs42l42_test_unmute_after_pll_lock_timeout,
			 cs42l42_test_pll_clks_gen_params),
	KUNIT_CASE_PARAM(cs42l42_test_mute_returns_to_osc, cs42l42_test_pll_clks_gen_params),
	KUNIT_CASE(cs42l42_test_unmute_no_pll),
	{ } /* terminator */
};

static struct kunit_suite cs42l42_test_suite = {
	.name = "snd-soc-cs42l42-test",
	.init = cs42l42_test_case_init,
	.test_cases = cs42l42_test_cases,
};

kunit_test_suite(cs42l42_test_suite);

MODULE_IMPORT_NS("SND_SOC_CS42L42_CORE");
MODULE_DESCRIPTION("KUnit test for Cirrus Logic cs42l42 codec driver");
MODULE_LICENSE("GPL");
