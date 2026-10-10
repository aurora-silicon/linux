// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * DCP Audio Bits
 *
 * Copyright (C) The Asahi Linux Contributors
 *
 * TODO:
 *  - figure some nice identification of the sound card (in case
 *    there's many DCP instances)
 */

#define DEBUG

#include <linux/component.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/of_dma.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/jack.h>

#include "av.h"
#include "dcp.h"
#include "audio.h"
#include "parser.h"

#define DCPAUD_ELEMENTS_MAXSIZE		16384
#define DCPAUD_PRODUCTATTRS_MAXSIZE	1024

struct neo_dcp_audio {
	struct device *dev;
	struct device *neo_dcp_dev;
	struct device *dma_dev;
	struct device_link *dma_link;
	struct dma_chan *chan;
	struct snd_card *card;
	struct snd_jack *jack;
	struct snd_pcm_substream *substream;
	unsigned int open_cookie;
	bool open_unplugged;

	struct mutex data_lock;
	bool neo_dcp_connected; /// dcp status keep for delayed initialization
	bool connected;
	unsigned int connection_cookie;

	struct snd_pcm_chmap_elem selected_chmap;
	struct neo_dcp_sound_cookie selected_cookie;
	void *elements;
	void *productattrs;

	struct snd_pcm_chmap *chmap_info;
};

static const struct snd_pcm_hardware neo_dcp_pcm_hw = {
	.info	 = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		   SNDRV_PCM_INFO_INTERLEAVED,
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_LE |
		   SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE,
	.rates			= SNDRV_PCM_RATE_CONTINUOUS,
	.rate_min		= 0,
	.rate_max		= UINT_MAX,
	.channels_min		= 1,
	.channels_max		= 16,
	.buffer_bytes_max	= SIZE_MAX,
	.period_bytes_min	= 4096, /* TODO */
	.period_bytes_max	= SIZE_MAX,
	.periods_min		= 2,
	.periods_max		= UINT_MAX,
};

/*
 * Without a sink the DCP has no audio elements to derive constraints from.
 * Offer a plain stereo format so that the PCM can still be opened and
 * configured, as HDMI PCMs on other platforms can. Userspace (PipeWire,
 * PulseAudio) probes the PCM once when the card appears and drops the
 * output for good if that fails, so a monitor attached later would never
 * show up. Starting the stream still fails until a sink is connected.
 */
static const struct snd_pcm_hardware neo_dcp_pcm_hw_unplugged = {
	.info	 = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		   SNDRV_PCM_INFO_INTERLEAVED,
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
		   SNDRV_PCM_FMTBIT_S32_LE,
	.rates			= SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
				  SNDRV_PCM_RATE_48000,
	.rate_min		= 32000,
	.rate_max		= 48000,
	.channels_min		= 2,
	.channels_max		= 2,
	.buffer_bytes_max	= SIZE_MAX,
	.period_bytes_min	= 4096,
	.period_bytes_max	= SIZE_MAX,
	.periods_min		= 2,
	.periods_max		= UINT_MAX,
};

static int neo_dcpaud_read_remote_info(struct neo_dcp_audio *neo_dcpaud)
{
	int ret;

	ret = neo_dcp_audiosrv_get_elements(neo_dcpaud->neo_dcp_dev, neo_dcpaud->elements,
					DCPAUD_ELEMENTS_MAXSIZE);
	if (ret < 0)
		return ret;

	ret = neo_dcp_audiosrv_get_product_attrs(neo_dcpaud->neo_dcp_dev, neo_dcpaud->productattrs,
					     DCPAUD_PRODUCTATTRS_MAXSIZE);
	if (ret < 0)
		return ret;

	return 0;
}

static int neo_dcpaud_interval_bitmask(struct snd_interval *i,
				   unsigned int mask)
{
	struct snd_interval range;
	if (!mask)
		return -EINVAL;

	snd_interval_any(&range);
	range.min = __ffs(mask);
	range.max = __fls(mask);
	return snd_interval_refine(i, &range);
}

extern const struct snd_pcm_hw_constraint_list snd_pcm_known_rates;

static void neo_dcpaud_fill_fmt_sieve(struct snd_pcm_hw_params *params,
				  struct neo_dcp_sound_format_mask *sieve)
{
	struct snd_interval *c = hw_param_interval(params,
				SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_interval *r = hw_param_interval(params,
				SNDRV_PCM_HW_PARAM_RATE);
	struct snd_mask *f = hw_param_mask(params,
				SNDRV_PCM_HW_PARAM_FORMAT);
	int i;

	sieve->nchans = GENMASK(c->max, c->min);
	sieve->formats = f->bits[0] | ((u64) f->bits[1]) << 32; /* TODO: don't open-code */

	for (i = 0; i < snd_pcm_known_rates.count; i++) {
		unsigned int rate = snd_pcm_known_rates.list[i];

		if (snd_interval_test(r, rate))
			sieve->rates |= 1u << i;
	}
}

static void neo_dcpaud_consult_elements(struct neo_dcp_audio *neo_dcpaud,
				    struct snd_pcm_hw_params *params,
				    struct neo_dcp_sound_format_mask *hits)
{
	struct neo_dcp_sound_format_mask sieve;
	struct neo_dcp_parse_ctx elements = {
		.neo_dcp = dev_get_drvdata(neo_dcpaud->neo_dcp_dev),
		.blob = neo_dcpaud->elements + 4,
		.len = DCPAUD_ELEMENTS_MAXSIZE - 4,
		.pos = 0,
	};

	neo_dcpaud_fill_fmt_sieve(params, &sieve);
	dev_dbg(neo_dcpaud->dev, "elements in: %llx %x %x\n", sieve.formats, sieve.nchans, sieve.rates);
	neo_parse_sound_constraints(&elements, &sieve, hits);
	dev_dbg(neo_dcpaud->dev, "elements out: %llx %x %x\n", hits->formats, hits->nchans, hits->rates);
}

static int neo_dcpaud_select_cookie(struct neo_dcp_audio *neo_dcpaud,
				 struct snd_pcm_hw_params *params)
{
	struct neo_dcp_sound_format_mask sieve;
	struct neo_dcp_parse_ctx elements = {
		.neo_dcp = dev_get_drvdata(neo_dcpaud->neo_dcp_dev),
		.blob = neo_dcpaud->elements + 4,
		.len = DCPAUD_ELEMENTS_MAXSIZE - 4,
		.pos = 0,
	};

	neo_dcpaud_fill_fmt_sieve(params, &sieve);
	return neo_parse_sound_mode(&elements, &sieve, &neo_dcpaud->selected_chmap,
				&neo_dcpaud->selected_cookie);
}

static int neo_dcpaud_rule_channels(struct snd_pcm_hw_params *params,
                                struct snd_pcm_hw_rule *rule)
{
	struct neo_dcp_audio *neo_dcpaud = rule->private;
	struct snd_interval *c = hw_param_interval(params,
				SNDRV_PCM_HW_PARAM_CHANNELS);
	struct neo_dcp_sound_format_mask hits = {0, 0, 0};

        neo_dcpaud_consult_elements(neo_dcpaud, params, &hits);

        return neo_dcpaud_interval_bitmask(c, hits.nchans);
}

static int neo_dcpaud_refine_fmt_mask(struct snd_mask *m, u64 mask)
{
	struct snd_mask mask_mask;

	if (!mask)
		return -EINVAL;
	mask_mask.bits[0] = mask;
	mask_mask.bits[1] = mask >> 32;

	return snd_mask_refine(m, &mask_mask);
}

static int neo_dcpaud_rule_format(struct snd_pcm_hw_params *params,
                               struct snd_pcm_hw_rule *rule)
{
	struct neo_dcp_audio *neo_dcpaud = rule->private;
	struct snd_mask *f = hw_param_mask(params,
				SNDRV_PCM_HW_PARAM_FORMAT);
	struct neo_dcp_sound_format_mask hits;

        neo_dcpaud_consult_elements(neo_dcpaud, params, &hits);

        return neo_dcpaud_refine_fmt_mask(f, hits.formats);
}

static int neo_dcpaud_rule_rate(struct snd_pcm_hw_params *params,
                             struct snd_pcm_hw_rule *rule)
{
	struct neo_dcp_audio *neo_dcpaud = rule->private;
	struct snd_interval *r = hw_param_interval(params,
				SNDRV_PCM_HW_PARAM_RATE);
	struct neo_dcp_sound_format_mask hits;

        neo_dcpaud_consult_elements(neo_dcpaud, params, &hits);

        return snd_interval_rate_bits(r, hits.rates);
}

static int neo_dcpaud_init_dma(struct neo_dcp_audio *neo_dcpaud)
{
	struct dma_chan *chan;
	if (neo_dcpaud->chan)
		return 0;

	chan = of_dma_request_slave_channel(neo_dcpaud->dev->of_node, "tx");
	/* squelch dma channel request errors, the driver will try again alter */
	if (!chan) {
		dev_warn(neo_dcpaud->dev, "audio TX DMA channel request failed\n");
		return -ENXIO;
	} else if (chan == ERR_PTR(-EPROBE_DEFER)) {
		dev_info(neo_dcpaud->dev, "audio TX DMA channel is not ready yet\n");
		return -ENXIO;
	} else if (IS_ERR(chan)) {
		dev_warn(neo_dcpaud->dev, "audio TX DMA channel request failed: %ld\n", PTR_ERR(chan));
		return PTR_ERR(chan);
	}
	neo_dcpaud->chan = chan;

	snd_pcm_set_managed_buffer(neo_dcpaud->substream, SNDRV_DMA_TYPE_DEV_IRAM,
				   neo_dcpaud->chan->device->dev, 1024 * 1024,
				   SIZE_MAX);

	return 0;
}

static int neo_dcp_pcm_open(struct snd_pcm_substream *substream)
{
	struct neo_dcp_audio *neo_dcpaud = substream->pcm->private_data;
	struct snd_dmaengine_dai_dma_data dma_data = {
		.flags = SND_DMAENGINE_PCM_DAI_FLAG_PACK,
	};
	struct snd_pcm_hardware hw;
	int ret;

	mutex_lock(&neo_dcpaud->data_lock);
	ret = neo_dcpaud_init_dma(neo_dcpaud);
	if (ret < 0) {
		mutex_unlock(&neo_dcpaud->data_lock);
		return ret;
	}

	neo_dcpaud->open_unplugged = !neo_dcpaud->connected;
	neo_dcpaud->open_cookie = neo_dcpaud->connection_cookie;
	mutex_unlock(&neo_dcpaud->data_lock);

	if (neo_dcpaud->open_unplugged) {
		hw = neo_dcp_pcm_hw_unplugged;
		goto refine;
	}

	ret = neo_dcpaud_read_remote_info(neo_dcpaud);
	if (ret < 0)
		return ret;

	snd_pcm_hw_rule_add(substream->runtime, 0, SNDRV_PCM_HW_PARAM_FORMAT,
			    neo_dcpaud_rule_format, neo_dcpaud,
			    SNDRV_PCM_HW_PARAM_CHANNELS, SNDRV_PCM_HW_PARAM_RATE, -1);
	snd_pcm_hw_rule_add(substream->runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
			    neo_dcpaud_rule_channels, neo_dcpaud,
			    SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_HW_PARAM_RATE, -1);
	snd_pcm_hw_rule_add(substream->runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
			    neo_dcpaud_rule_rate, neo_dcpaud,
			    SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_HW_PARAM_CHANNELS, -1);

	hw = neo_dcp_pcm_hw;
refine:
	hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			  SNDRV_PCM_INFO_INTERLEAVED;
	hw.periods_min = 2;
	hw.periods_max = UINT_MAX;
	hw.period_bytes_min = 256;
	hw.period_bytes_max = SIZE_MAX; // TODO dma_get_max_seg_size(dma_dev);
	hw.buffer_bytes_max = SIZE_MAX;
	hw.fifo_size = 16;
	ret = snd_dmaengine_pcm_refine_runtime_hwparams(substream, &dma_data,
							&hw, neo_dcpaud->chan);
	if (ret)
		return ret;
	substream->runtime->hw = hw;

	return snd_dmaengine_pcm_open(substream, neo_dcpaud->chan);
}

static int neo_dcp_pcm_close(struct snd_pcm_substream *substream)
{
	struct neo_dcp_audio *neo_dcpaud = substream->pcm->private_data;
	neo_dcpaud->selected_chmap.channels = 0;
	neo_dcpaud->open_unplugged = false;

	return snd_dmaengine_pcm_close(substream);
}

static int neo_dcpaud_connection_up(struct neo_dcp_audio *neo_dcpaud)
{
	bool ret;
	mutex_lock(&neo_dcpaud->data_lock);
	ret = neo_dcpaud->connected &&
	      neo_dcpaud->open_cookie == neo_dcpaud->connection_cookie;
	mutex_unlock(&neo_dcpaud->data_lock);
	return ret;
}

static int neo_dcp_pcm_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params)
{
	struct neo_dcp_audio *neo_dcpaud = substream->pcm->private_data;
	struct dma_slave_config slave_config;
	struct dma_chan *chan = snd_dmaengine_pcm_get_chan(substream);
	int ret;

	if (neo_dcpaud->open_unplugged)
		return 0;

	if (!neo_dcpaud_connection_up(neo_dcpaud))
		return -ENXIO;

	ret = neo_dcpaud_select_cookie(neo_dcpaud, params);
	if (ret < 0)
		return ret;
	if (!ret)
		return -EINVAL;

	memset(&slave_config, 0, sizeof(slave_config));
	ret = snd_hwparams_to_dma_slave_config(substream, params, &slave_config);
	dev_info(neo_dcpaud->dev, "snd_hwparams_to_dma_slave_config: %d\n", ret);
	if (ret < 0)
		return ret;

	slave_config.direction = DMA_MEM_TO_DEV;
	/*
	 * The data entry from the DMA controller to the DPA peripheral
	 * is 32-bit wide no matter the actual sample size.
	 */
	slave_config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;

	ret = dmaengine_slave_config(chan, &slave_config);
	dev_info(neo_dcpaud->dev, "dmaengine_slave_config: %d\n", ret);
	return ret;
}

static int neo_dcp_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct neo_dcp_audio *neo_dcpaud = substream->pcm->private_data;

	if (!neo_dcpaud_connection_up(neo_dcpaud))
		return 0;

	return neo_dcp_audiosrv_unprepare(neo_dcpaud->neo_dcp_dev);
}

static int neo_dcp_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct neo_dcp_audio *neo_dcpaud = substream->pcm->private_data;

	/* alsa-lib prepares right after hw_params; see dcp_pcm_hw_unplugged */
	if (neo_dcpaud->open_unplugged)
		return 0;

	if (!neo_dcpaud_connection_up(neo_dcpaud))
		return -ENXIO;

	return neo_dcp_audiosrv_prepare(neo_dcpaud->neo_dcp_dev,
				    &neo_dcpaud->selected_cookie);
}

static int neo_dcp_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct neo_dcp_audio *neo_dcpaud = substream->pcm->private_data;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!neo_dcpaud_connection_up(neo_dcpaud))
			return -ENXIO;

		WARN_ON(pm_runtime_get_sync(neo_dcpaud->dev) < 0);
		ret = neo_dcp_audiosrv_startlink(neo_dcpaud->neo_dcp_dev,
					     &neo_dcpaud->selected_cookie);
		if (ret < 0)
			return ret;
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		break;

	default:
		return -EINVAL;
	}

	ret = snd_dmaengine_pcm_trigger(substream, cmd);
	if (ret < 0)
		return ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		ret = neo_dcp_audiosrv_stoplink(neo_dcpaud->neo_dcp_dev);
		pm_runtime_mark_last_busy(neo_dcpaud->dev);
		__pm_runtime_put_autosuspend(neo_dcpaud->dev);
		if (ret < 0)
			return ret;
		break;
	}

	return 0;
}

struct snd_pcm_ops neo_dcp_playback_ops = {
	.open = neo_dcp_pcm_open,
	.close = neo_dcp_pcm_close,
	.hw_params = neo_dcp_pcm_hw_params,
	.hw_free = neo_dcp_pcm_hw_free,
	.prepare = neo_dcp_pcm_prepare,
	.trigger = neo_dcp_pcm_trigger,
	.pointer = snd_dmaengine_pcm_pointer,
};

// Transitional workaround: for the chmap control TLV, advertise options
// copied from hdmi-codec.c
#include "hdmi-codec-chmap.h"

static int neo_dcpaud_chmap_ctl_get(struct snd_kcontrol *kcontrol,
			        struct snd_ctl_elem_value *ucontrol)
{
	struct snd_pcm_chmap *info = snd_kcontrol_chip(kcontrol);
	struct neo_dcp_audio *neo_dcpaud = info->private_data;
	unsigned int i;

	for (i = 0; i < info->max_channels; i++)
		ucontrol->value.integer.value[i] = \
				(i < neo_dcpaud->selected_chmap.channels) ?
				neo_dcpaud->selected_chmap.map[i] : SNDRV_CHMAP_UNKNOWN;

	return 0;
}


static int neo_dcpaud_create_chmap_ctl(struct neo_dcp_audio *neo_dcpaud)
{
	struct snd_pcm *pcm = neo_dcpaud->substream->pcm;
	struct snd_pcm_chmap *chmap_info;
	int ret;

	ret = snd_pcm_add_chmap_ctls(pcm, SNDRV_PCM_STREAM_PLAYBACK, NULL,
				     neo_dcp_pcm_hw.channels_max, 0, &chmap_info);
	if (ret < 0)
		return ret;

	chmap_info->kctl->get = neo_dcpaud_chmap_ctl_get;
	chmap_info->chmap = hdmi_codec_8ch_chmaps;
	chmap_info->private_data = neo_dcpaud;

	return 0;
}

static int neo_dcpaud_create_pcm(struct neo_dcp_audio *neo_dcpaud)
{
	struct snd_card *card = neo_dcpaud->card;
	struct snd_pcm *pcm;
	int ret;

#define NUM_PLAYBACK 1
#define NUM_CAPTURE 0

	ret = snd_pcm_new(card, card->shortname, 0, NUM_PLAYBACK, NUM_CAPTURE, &pcm);
	if (ret)
		return ret;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &neo_dcp_playback_ops);
	neo_dcpaud->substream = pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
	pcm->nonatomic = true;
	pcm->private_data = neo_dcpaud;
	strscpy(pcm->name, card->shortname, sizeof(pcm->name));

	return 0;
}

/* expects to be called with data_lock locked and unlocks it */
static void neo_dcpaud_report_hotplug(struct neo_dcp_audio *neo_dcpaud, bool connected)
{
	struct snd_pcm_substream *substream = neo_dcpaud->substream;

	if (!neo_dcpaud->card || neo_dcpaud->connected == connected) {
		mutex_unlock(&neo_dcpaud->data_lock);
		return;
	}

	neo_dcpaud->connected = connected;
	if (connected)
		neo_dcpaud->connection_cookie++;
	mutex_unlock(&neo_dcpaud->data_lock);

	snd_jack_report(neo_dcpaud->jack, connected ? SND_JACK_AVOUT : 0);

	if (!connected) {
		snd_pcm_stream_lock(substream);
		if (substream->runtime)
			snd_pcm_stop(substream, SNDRV_PCM_STATE_DISCONNECTED);
		snd_pcm_stream_unlock(substream);
	}
}

static int neo_dcpaud_create_jack(struct neo_dcp_audio *neo_dcpaud)
{
	struct snd_card *card = neo_dcpaud->card;

	return snd_jack_new(card, "HDMI/DP", SND_JACK_AVOUT,
			    &neo_dcpaud->jack, true, false);
}

static void neo_dcpaud_set_card_names(struct neo_dcp_audio *neo_dcpaud)
{
	struct snd_card *card = neo_dcpaud->card;

	strscpy(card->driver, "apple_dcp", sizeof(card->driver));
	strscpy(card->longname, "Apple DisplayPort", sizeof(card->longname));
	strscpy(card->shortname, "Apple DisplayPort", sizeof(card->shortname));
}

#ifdef CONFIG_SND_DEBUG
static void neo_dcpaud_expose_debugfs_blob(struct neo_dcp_audio *neo_dcpaud, const char *name, void *base, size_t size)
{
	struct debugfs_blob_wrapper *wrapper;
	wrapper = devm_kzalloc(neo_dcpaud->dev, sizeof(*wrapper), GFP_KERNEL);
	if (!wrapper)
		return;
	wrapper->data = base;
	wrapper->size = size;
	debugfs_create_blob(name, 0600, neo_dcpaud->card->debugfs_root, wrapper);
}
#else
static void neo_dcpaud_expose_debugfs_blob(struct neo_dcp_audio *neo_dcpaud, const char *name, void *base, size_t size) {}
#endif

extern bool neo_hdmi_audio;

static int neo_dcpaud_init_snd_card(struct neo_dcp_audio *neo_dcpaud)
{
	int ret;
	if (!neo_hdmi_audio)
		return -ENODEV;


	ret = snd_card_new(neo_dcpaud->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			   THIS_MODULE, 0, &neo_dcpaud->card);
	if (ret)
		return ret;

	neo_dcpaud_set_card_names(neo_dcpaud);

	ret = neo_dcpaud_create_pcm(neo_dcpaud);
	if (ret)
		goto err_free_card;

	ret = neo_dcpaud_create_chmap_ctl(neo_dcpaud);
	if (ret)
		goto err_free_card;

	ret = neo_dcpaud_create_jack(neo_dcpaud);
	if (ret)
		goto err_free_card;

	ret = snd_card_register(neo_dcpaud->card);
	if (ret)
		goto err_free_card;

	return 0;
err_free_card:
	dev_warn(neo_dcpaud->dev, "Failed to initialize sound card: %d\n", ret);
	snd_card_free(neo_dcpaud->card);
	neo_dcpaud->card = NULL;
	return ret;
}

void neo_dcpaud_connect(struct platform_device *pdev, bool connected)
{
	struct neo_dcp_audio *neo_dcpaud = platform_get_drvdata(pdev);

	mutex_lock(&neo_dcpaud->data_lock);

	neo_dcpaud_report_hotplug(neo_dcpaud, connected);
}

void neo_dcpaud_disconnect(struct platform_device *pdev)
{
	struct neo_dcp_audio *neo_dcpaud = platform_get_drvdata(pdev);

	mutex_lock(&neo_dcpaud->data_lock);

	neo_dcpaud_report_hotplug(neo_dcpaud, false);
}

static int neo_dcpaud_comp_bind(struct device *dev, struct device *main, void *data)
{
	struct neo_dcp_audio *neo_dcpaud = dev_get_drvdata(dev);
	struct device_node *endpoint, *neo_dcp_node = NULL;
	struct platform_device *neo_dcp_pdev, *dma_pdev;
	struct of_phandle_args dma_spec;
	int index;
	int ret;

	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable runtime PM: %d\n", ret);

	/* find linked DCP instance */
	endpoint = of_graph_get_endpoint_by_regs(dev->of_node, 0, 0);
	if (endpoint) {
		neo_dcp_node = of_graph_get_remote_port_parent(endpoint);
		of_node_put(endpoint);
	}
	if (!neo_dcp_node || !of_device_is_available(neo_dcp_node)) {
		of_node_put(neo_dcp_node);
		dev_info(dev, "No audio support\n");
		goto rpm_put;
	}

	index = of_property_match_string(dev->of_node, "dma-names", "tx");
	if (index < 0) {
		dev_err(dev, "No dma-names property\n");
		goto rpm_put;
	}

	if (of_parse_phandle_with_args(dev->of_node, "dmas", "#dma-cells", index,
				       &dma_spec) || !dma_spec.np) {
		dev_err(dev, "Failed to parse dmas property\n");
		goto rpm_put;
	}

	neo_dcp_pdev = of_find_device_by_node(neo_dcp_node);
	of_node_put(neo_dcp_node);
	if (!neo_dcp_pdev) {
		dev_info(dev, "No DP/HDMI audio device, dcp not ready\n");
		goto rpm_put;
	}
	neo_dcpaud->neo_dcp_dev = &neo_dcp_pdev->dev;

	dma_pdev = of_find_device_by_node(dma_spec.np);
	of_node_put(dma_spec.np);
	if (!dma_pdev) {
		dev_info(dev, "No DMA device\n");
		goto rpm_put;
	}
	neo_dcpaud->dma_dev = &dma_pdev->dev;

	neo_dcpaud->dma_link = device_link_add(dev, neo_dcpaud->dma_dev,
					   DL_FLAG_PM_RUNTIME |
					   DL_FLAG_RPM_ACTIVE |
					   DL_FLAG_STATELESS);

	/* ignore errors to prevent audio issues affecting the display side */
	ret = neo_dcpaud_init_snd_card(neo_dcpaud);

	if (!ret) {
		neo_dcpaud_expose_debugfs_blob(neo_dcpaud, "selected_cookie", &neo_dcpaud->selected_cookie,
					sizeof(neo_dcpaud->selected_cookie));
		neo_dcpaud_expose_debugfs_blob(neo_dcpaud, "elements", neo_dcpaud->elements,
					DCPAUD_ELEMENTS_MAXSIZE);
		neo_dcpaud_expose_debugfs_blob(neo_dcpaud, "product_attrs", neo_dcpaud->productattrs,
					DCPAUD_PRODUCTATTRS_MAXSIZE);
	}

rpm_put:
	pm_runtime_put(dev);

	return 0;
}

static void neo_dcpaud_comp_unbind(struct device *dev, struct device *main,
			       void *data)
{
	struct neo_dcp_audio *neo_dcpaud = dev_get_drvdata(dev);

	/* snd_card_free_when_closed() checks for NULL */
	snd_card_free_when_closed(neo_dcpaud->card);

	if (neo_dcpaud->dma_link)
		device_link_del(neo_dcpaud->dma_link);
}

static const struct component_ops neo_dcpaud_comp_ops = {
	.bind	= neo_dcpaud_comp_bind,
	.unbind	= neo_dcpaud_comp_unbind,
};

static int neo_dcpaud_probe(struct platform_device *pdev)
{
	struct neo_dcp_audio *neo_dcpaud;

	neo_dcpaud = devm_kzalloc(&pdev->dev, sizeof(*neo_dcpaud), GFP_KERNEL);
	if (!neo_dcpaud)
		return -ENOMEM;

	neo_dcpaud->elements = devm_kzalloc(&pdev->dev, DCPAUD_ELEMENTS_MAXSIZE,
					GFP_KERNEL);
	if (!neo_dcpaud->elements)
		return -ENOMEM;

	neo_dcpaud->productattrs = devm_kzalloc(&pdev->dev, DCPAUD_PRODUCTATTRS_MAXSIZE,
					    GFP_KERNEL);
	if (!neo_dcpaud->productattrs)
		return -ENOMEM;

	neo_dcpaud->dev = &pdev->dev;
	mutex_init(&neo_dcpaud->data_lock);
	platform_set_drvdata(pdev, neo_dcpaud);

	return component_add(&pdev->dev, &neo_dcpaud_comp_ops);
}

static void neo_dcpaud_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &neo_dcpaud_comp_ops);
}

static void neo_dcpaud_shutdown(struct platform_device *pdev)
{
	component_del(&pdev->dev, &neo_dcpaud_comp_ops);
}

static __maybe_unused int neo_dcpaud_suspend(struct device *dev)
{
	/*
	 * Using snd_power_change_state() does not work since the sound card
	 * is what resumes runtime PM.
	 */

	return 0;
}

static __maybe_unused int neo_dcpaud_resume(struct device *dev)
{
	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(neo_dcpaud_pm_ops, neo_dcpaud_suspend, neo_dcpaud_resume, NULL);

static const struct of_device_id neo_dcpaud_of_match[] = {
	{ .compatible = "apple,dpaudio" },
	{}
};

static struct platform_driver neo_dcpaud_driver = {
	.driver = {
		.name = "dcp-dp-audio",
		.of_match_table	= neo_dcpaud_of_match,
		.pm		= pm_ptr(&neo_dcpaud_pm_ops),
	},
	.probe		= neo_dcpaud_probe,
	.remove		= neo_dcpaud_remove,
	.shutdown	= neo_dcpaud_shutdown,
};

void __init neo_dcp_audio_register(void)
{
        platform_driver_register(&neo_dcpaud_driver);
}

void __exit neo_dcp_audio_unregister(void)
{
        platform_driver_unregister(&neo_dcpaud_driver);
}
