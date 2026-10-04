// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* M3 port of M4 display PCM integration. Audio command/DMA ordering follows Martin
 * Povišer's Asahi audio.c; the transport/cookie and connected-sink lifetime
 * are supplied by the M3 old-EPIC transport.
 */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/unaligned.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/dmaengine_pcm.h>
#include "m3_dcpext_audio_api.h"
#include "m3_dcpext_audio_format.h"

static u8 metadata_capture[M3_AUDIO_ELEMENTS_MAX + 48];
static struct debugfs_blob_wrapper metadata_blob = {
	.data = metadata_capture, .size = sizeof(metadata_capture),
};
static struct dentry *metadata_debug;
static bool metadata_only = true;
module_param(metadata_only, bool, 0400);
MODULE_PARM_DESC(metadata_only, "Discover fresh formats without registering PCM for initial qualification");
static unsigned int controller;
module_param(controller, uint, 0400);
MODULE_PARM_DESC(controller, "DCPEXT instance whose DPA DMA route is described by this device (initially 0)");

struct m3_audio_pcm {
	struct device *dev;
	const struct m3_dcpext_audio_ops *ops;
	struct dma_chan *chan;
	struct snd_card *card;
	u64 generation;
	u8 cookie[32];
	char name[32];
	bool opened, prepared, started, pinned;
};

static int m3_audio_simple(struct m3_audio_pcm *a, u32 cmd, bool cookie)
{
	u8 data[64] = {};
	u32 size = cmd == 4 ? 32 : cmd == 5 ? 16 : 64;
	int ret;
	if (cookie || (cmd >= 6 && cmd <= 11))
		memcpy(data, a->cookie, sizeof(a->cookie));
	ret = a->ops->call(0, cmd, data, size, &a->generation);
	/* M4 link methods return IOReturn at byte 48 of the 64-byte body. */
	if (!ret && size == 64 && get_unaligned_le32(data + 48)) {
		dev_err(a->dev, "audio command %u service status %#x\n", cmd,
			get_unaligned_le32(data + 48));
		ret = -EREMOTEIO;
	}
	return ret;
}

static int m3_audio_elements(struct m3_audio_pcm *a)
{
	u8 *data;
	u64 used;
	int ret;
	data = kzalloc(M3_AUDIO_ELEMENTS_MAX + 48, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	put_unaligned_le64(M3_AUDIO_ELEMENTS_MAX, data);
	ret = a->ops->call(1, 16, data, M3_AUDIO_ELEMENTS_MAX + 48, &a->generation);
	if (ret)
		goto out;
	memcpy(metadata_capture, data, sizeof(metadata_capture));
	used = get_unaligned_le64(data + 32);
	dev_info(a->dev, "audio metadata capacity=%llu status=%#x used=%llu\n",
		 get_unaligned_le64(data), get_unaligned_le32(data + 16), used);
	if (get_unaligned_le64(data) != M3_AUDIO_ELEMENTS_MAX || get_unaligned_le32(data + 16) ||
	    used < 8 || used > M3_AUDIO_ELEMENTS_MAX) {
		ret = -EPROTO;
		goto out;
	}
	ret = m3_audio_select_stereo(data + 48, used, a->cookie);
	if (!ret)
		dev_info(a->dev, "%s: fresh stereo 48kHz S16 cookie, metadata %llu bytes\n", a->name, used);
out:
	kfree(data);
	return ret;
}

static int m3_audio_open_service(struct m3_audio_pcm *a)
{
	int ret;
	a->generation = 0;
	ret = a->ops->identify(a->name, sizeof(a->name), &a->generation);
	if (ret)
		return ret;
	ret = m3_audio_simple(a, 4, false);
	if (ret)
		return ret;
	a->opened = true;
	ret = m3_audio_elements(a);
	if (ret) {
		m3_audio_simple(a, 5, false);
		a->opened = false;
	}
	return ret;
}

static const struct snd_pcm_hardware m3_audio_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000, .rate_max = 48000,
	.channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = 256, .period_bytes_max = 64 * 1024,
	.periods_min = 4, .periods_max = 32,
	.fifo_size = 16,
};

static int m3_pcm_open(struct snd_pcm_substream *s)
{
	struct m3_audio_pcm *a = s->pcm->private_data;
	int ret;
	struct dma_tx_state state;
	if (dmaengine_tx_status(a->chan, 0, &state) == DMA_ERROR)
		return -EIO;
	if (!s->dma_buffer.area || s->dma_buffer.bytes != 256 * 1024)
		return -ENOMEM;
	ret = m3_audio_open_service(a);
	if (ret)
		return ret;
	s->runtime->hw = m3_audio_hw;
	/* The DMA helper applies the integer-period constraint itself. The
	 * constraint API may return positive success, not just zero.
	 */
	ret = snd_dmaengine_pcm_open(s, a->chan);
	if (ret) {
		m3_audio_simple(a, 5, false);
		a->opened = false;
	}
	return ret;
}
static int m3_pcm_hw_params(struct snd_pcm_substream *s, struct snd_pcm_hw_params *p)
{
	struct m3_audio_pcm *a = s->pcm->private_data;
	struct dma_slave_config cfg = {};
	int ret = snd_hwparams_to_dma_slave_config(s, p, &cfg);
	if (ret)
		return ret;
	cfg.direction = DMA_MEM_TO_DEV;
	/* DPA data entry is always 32 bits, including stereo 16-bit PCM. */
	cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	return dmaengine_slave_config(a->chan, &cfg);
}
static int m3_pcm_prepare(struct snd_pcm_substream *s)
{
	struct m3_audio_pcm *a = s->pcm->private_data;
	int ret;

	/* ALSA permits prepare again after drop/XRUN or before a first start.
	 * DCP retains the prepared link until unprepare and rejects duplicate
	 * prepare commands. The PCM format is fixed for this open lifetime.
	 */
	if (a->prepared)
		return 0;
	ret = m3_audio_simple(a, 6, true);
	if (!ret)
		a->prepared = true;
	return ret;
}
static int m3_pcm_trigger(struct snd_pcm_substream *s, int cmd)
{
	struct m3_audio_pcm *a = s->pcm->private_data;
	int ret;
	if (cmd == SNDRV_PCM_TRIGGER_START) {
		ret = m3_audio_simple(a, 7, true);
		if (ret)
			return ret;
		a->started = true;
		/* Bring-up uses one fixed coherent buffer for the card lifetime.
		 * Keep it and callback storage alive even if SIO cannot retire DMA.
		 * TODO: removable PCM after SIO error recovery is qualified.
		 */
		if (!a->pinned) {
			__module_get(THIS_MODULE);
			a->pinned = true;
		}
		ret = snd_dmaengine_pcm_trigger(s, cmd);
		if (ret) {
			m3_audio_simple(a, 10, false);
			a->started = false;
		}
		return ret;
	}
	if (cmd == SNDRV_PCM_TRIGGER_STOP) {
		ret = snd_dmaengine_pcm_trigger(s, cmd);
		if (ret)
			return ret;
		/* Drain DMA before the link is stopped; sync_stop is sleepable. */
		return 0;
	}
	return -EINVAL;
}
static int m3_pcm_sync_stop(struct snd_pcm_substream *s)
{
	struct m3_audio_pcm *a = s->pcm->private_data;
	int ret = snd_dmaengine_pcm_sync_stop(s);
	if (!ret && a->started) {
		ret = m3_audio_simple(a, 10, false);
		a->started = false;
	}
	return ret;
}
static int m3_pcm_hw_free(struct snd_pcm_substream *s)
{
	struct m3_audio_pcm *a = s->pcm->private_data;
	int ret = 0;
	if (a->prepared) {
		ret = m3_audio_simple(a, 11, false);
		a->prepared = false;
	}
	return ret;
}
static int m3_pcm_close(struct snd_pcm_substream *s)
{
	struct m3_audio_pcm *a = s->pcm->private_data;
	int ret = snd_dmaengine_pcm_close(s);
	if (a->opened) {
		int err = m3_audio_simple(a, 5, false);
		a->opened = false;
		if (!ret)
			ret = err;
	}
	return ret;
}
static const struct snd_pcm_ops m3_pcm_ops = {
	.open = m3_pcm_open, .close = m3_pcm_close,
	.hw_params = m3_pcm_hw_params, .hw_free = m3_pcm_hw_free,
	.prepare = m3_pcm_prepare, .trigger = m3_pcm_trigger,
	.sync_stop = m3_pcm_sync_stop, .pointer = snd_dmaengine_pcm_pointer,
};

static int m3_audio_probe(struct platform_device *pdev)
{
	struct m3_audio_pcm *a;
	struct snd_pcm *pcm;
	int ret;
	/* This first DT node describes DCPEXT0 -> DPA1 -> SIO 0x66.
	 * Never redirect it to a controller whose DMA mapping is unqualified.
	 */
	if (controller != 0)
		return -EINVAL;
	a = devm_kzalloc(&pdev->dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = &pdev->dev;
	a->ops = m3_dcpext_audio_get(controller);
	if (!a->ops)
		return -EPROBE_DEFER;
	a->chan = of_dma_request_slave_channel(pdev->dev.of_node, "tx");
	if (IS_ERR(a->chan))
		return dev_err_probe(a->dev, PTR_ERR(a->chan), "SIO channel unavailable\n");
	platform_set_drvdata(pdev, a);
	pm_runtime_enable(a->dev);
	ret = pm_runtime_resume_and_get(a->dev);
	if (ret < 0)
		goto release;
	ret = m3_audio_open_service(a);
	if (ret)
		goto power;
	ret = m3_audio_simple(a, 5, false);
	a->opened = false;
	if (ret)
		goto power;
	if (metadata_only) {
		dev_info(a->dev, "metadata-only qualification completed; no PCM or DMA issued\n");
		return 0;
	}
	ret = snd_card_new(a->dev, -1, "M3HDMI0", THIS_MODULE, 0, &a->card);
	if (ret)
		goto power;
	strscpy(a->card->driver, "M3HDMI", sizeof(a->card->driver));
	snprintf(a->card->shortname, sizeof(a->card->shortname), "HDMI %s", a->name);
	snprintf(a->card->longname, sizeof(a->card->longname), "Apple M3 DCPEXT0 HDMI - %s", a->name);
	ret = snd_pcm_new(a->card, a->card->shortname, 0, 1, 0, &pcm);
	if (ret)
		goto card;
	pcm->private_data = a;
	pcm->nonatomic = true;
	strscpy(pcm->name, a->card->shortname, sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &m3_pcm_ops);
	ret = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV,
					   a->chan->device->dev, 256 * 1024, 0);
	if (ret)
		goto card;
	ret = snd_card_register(a->card);
	if (!ret)
		return 0;
card:
	snd_card_free(a->card);
power:
	pm_runtime_put_sync(a->dev);
release:
	pm_runtime_disable(a->dev);
	dma_release_channel(a->chan);
	return ret;
}
static void m3_audio_remove(struct platform_device *pdev)
{
	struct m3_audio_pcm *a = platform_get_drvdata(pdev);
	if (a->card)
		snd_card_free(a->card);
	dma_release_channel(a->chan);
	pm_runtime_put_sync(a->dev);
	pm_runtime_disable(a->dev);
}
static const struct of_device_id m3_audio_match[] = {
	{ .compatible = "apple,j514s-hdmi-audio" }, {}
};
MODULE_DEVICE_TABLE(of, m3_audio_match);
static struct platform_driver m3_audio_driver = {
	.probe = m3_audio_probe, .remove = m3_audio_remove,
	.driver = { .name = "m3-dcpext-audio", .of_match_table = m3_audio_match,
		    .suppress_bind_attrs = true },
};
static int __init m3_audio_init(void)
{
	int ret;
	metadata_debug = debugfs_create_blob("m3-hdmi-elements", 0400, NULL, &metadata_blob);
	ret = platform_driver_register(&m3_audio_driver);
	if (ret)
		debugfs_remove(metadata_debug);
	return ret;
}
static void __exit m3_audio_exit(void)
{
	platform_driver_unregister(&m3_audio_driver);
	debugfs_remove(metadata_debug);
}
module_init(m3_audio_init);
module_exit(m3_audio_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple M3 external display PCM audio");
