// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by audio.c: on the legacy (pre-14.7) audio service a stopped
 * stream keeps its runtime PM reference until sync_stop has waited for the
 * DMA terminate, a failed START leaves exactly one reference to put, and the
 * element catalog is asked for with the full 256 KiB capacity.
 * The production trigger and sync_stop run; the audio service's link calls
 * and the DMA channel are stubbed.
 */
#include <kunit/platform_device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

struct dcpaud_legacy_test {
	struct dma_device dma;
	struct dma_chan channel;
	struct dma_async_tx_descriptor desc;
	char sequence[32];
	unsigned int nsequence;
	/* The runtime PM usage count when the terminate was waited for */
	int usage_at_drain;
	int startlink_err;
	size_t elements_asked;
	size_t catalog;
	struct device *dev;
};

static struct dcpaud_legacy_test *dcpaud_legacy_current;

static void dcpaud_legacy_record(char step)
{
	struct dcpaud_legacy_test *t = dcpaud_legacy_current;

	if (t->nsequence < sizeof(t->sequence) - 1)
		t->sequence[t->nsequence++] = step;
}

static int dcpaud_legacy_terminate(struct dma_chan *chan)
{
	dcpaud_legacy_record('X');
	return 0;
}

static void dcpaud_legacy_drain(struct dma_chan *chan)
{
	struct dcpaud_legacy_test *t = dcpaud_legacy_current;

	t->usage_at_drain = atomic_read(&t->dev->power.usage_count);
	dcpaud_legacy_record('D');
}

static dma_cookie_t dcpaud_legacy_submit(struct dma_async_tx_descriptor *tx)
{
	return 1;
}

static struct dma_async_tx_descriptor *
dcpaud_legacy_prep(struct dma_chan *chan, dma_addr_t buf, size_t len, size_t period,
		   enum dma_transfer_direction dir, unsigned long flags)
{
	struct dcpaud_legacy_test *t = dcpaud_legacy_current;

	dcpaud_legacy_record('Q');
	t->desc.chan = chan;
	t->desc.tx_submit = dcpaud_legacy_submit;
	return &t->desc;
}

static void dcpaud_legacy_issue(struct dma_chan *chan)
{
}

static enum dma_status dcpaud_legacy_status(struct dma_chan *chan, dma_cookie_t cookie,
					    struct dma_tx_state *state)
{
	return DMA_COMPLETE;
}

static int dcpaud_legacy_startlink(struct device *dev, struct dcp_sound_cookie *cookie)
{
	dcpaud_legacy_record('T');
	return dcpaud_legacy_current->startlink_err;
}

static int dcpaud_legacy_stoplink(struct device *dev)
{
	dcpaud_legacy_record('S');
	return 0;
}

static void dcpaud_legacy_rpm_disable(void *data)
{
	pm_runtime_disable(data);
}

static void dcpaud_legacy_close(void *data)
{
	snd_dmaengine_pcm_close(data);
}

/* A legacy service instance with a sink, and a configured stream on it */
static struct snd_pcm_substream *dcpaud_legacy_setup(struct kunit *test,
						     struct dcp_audio **audio)
{
	struct dcpaud_legacy_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct dcp_audio *dcpaud = kunit_kzalloc(test, sizeof(*dcpaud), GFP_KERNEL);
	struct snd_pcm_substream *sub = kunit_kzalloc(test, sizeof(*sub), GFP_KERNEL);
	struct snd_pcm_runtime *runtime = kunit_kzalloc(test, sizeof(*runtime), GFP_KERNEL);
	struct snd_pcm *pcm = kunit_kzalloc(test, sizeof(*pcm), GFP_KERNEL);
	struct platform_device *pdev;

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_NOT_NULL(test, dcpaud);
	KUNIT_ASSERT_NOT_NULL(test, sub);
	KUNIT_ASSERT_NOT_NULL(test, runtime);
	KUNIT_ASSERT_NOT_NULL(test, pcm);
	pdev = kunit_platform_device_alloc(test, "dcpaud-legacy", PLATFORM_DEVID_AUTO);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);
	platform_set_drvdata(pdev, dcpaud);
	dcpaud->dev = &pdev->dev;
	dcpaud->dcp_dev = &pdev->dev;
	mutex_init(&dcpaud->data_lock);
	INIT_DELAYED_WORK(&dcpaud->void_work, dcpaud_void_work);
	dcpaud->connected = true;
	dcpaud->substream = sub;

	/* Runtime PM on, without callbacks: only the usage count matters */
	pm_runtime_no_callbacks(dcpaud->dev);
	pm_runtime_enable(dcpaud->dev);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dcpaud_legacy_rpm_disable,
							dcpaud->dev), 0);

	dcpaud_legacy_current = t;
	t->dev = dcpaud->dev;
	t->usage_at_drain = -1;
	t->channel.device = &t->dma;
	t->dma.device_terminate_all = dcpaud_legacy_terminate;
	t->dma.device_synchronize = dcpaud_legacy_drain;
	t->dma.device_tx_status = dcpaud_legacy_status;
	t->dma.device_prep_dma_cyclic = dcpaud_legacy_prep;
	t->dma.device_issue_pending = dcpaud_legacy_issue;
	kunit_activate_static_stub(test, dcp_audiosrv_startlink, dcpaud_legacy_startlink);
	kunit_activate_static_stub(test, dcp_audiosrv_stoplink, dcpaud_legacy_stoplink);

	pcm->private_data = dcpaud;
	pcm->nonatomic = true;
	sub->pcm = pcm;
	sub->runtime = runtime;
	mutex_init(&sub->self_group.mutex);
	spin_lock_init(&sub->self_group.lock);
	sub->group = &sub->self_group;
	runtime->frame_bits = 64;
	runtime->rate = 48000;
	runtime->buffer_size = 8192;
	runtime->period_size = 512;
	KUNIT_ASSERT_EQ(test, snd_dmaengine_pcm_open(sub, &t->channel), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dcpaud_legacy_close, sub), 0);

	*audio = dcpaud;
	return sub;
}

static int dcpaud_legacy_usage(struct dcp_audio *dcpaud)
{
	return atomic_read(&dcpaud->dev->power.usage_count);
}

static void dcpaud_legacy_stop_waits_for_dma(struct kunit *test)
{
	struct dcp_audio *audio;
	struct snd_pcm_substream *sub = dcpaud_legacy_setup(test, &audio);

	KUNIT_ASSERT_EQ(test, dcp_pcm_trigger(sub, SNDRV_PCM_TRIGGER_START), 0);
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_usage(audio), 1);

	/* The terminate is only started: the DPA stays powered */
	KUNIT_ASSERT_EQ(test, dcp_pcm_trigger(sub, SNDRV_PCM_TRIGGER_STOP), 0);
	KUNIT_EXPECT_STREQ(test, dcpaud_legacy_current->sequence, "TQXS");
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_usage(audio), 1);

	/* The reference goes once the terminate was waited for */
	KUNIT_ASSERT_EQ(test, dcp_pcm_sync_stop(sub), 0);
	KUNIT_EXPECT_STREQ(test, dcpaud_legacy_current->sequence, "TQXSD");
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_current->usage_at_drain, 1);
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_usage(audio), 0);
	KUNIT_EXPECT_FALSE(test, audio->pm_held);

	/* A second sync_stop (close after prepare) has nothing to put */
	KUNIT_ASSERT_EQ(test, dcp_pcm_sync_stop(sub), 0);
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_usage(audio), 0);
}

static void dcpaud_legacy_suspend_waits_for_dma(struct kunit *test)
{
	struct dcp_audio *audio;
	struct snd_pcm_substream *sub = dcpaud_legacy_setup(test, &audio);

	KUNIT_ASSERT_EQ(test, dcp_pcm_trigger(sub, SNDRV_PCM_TRIGGER_START), 0);
	KUNIT_ASSERT_EQ(test, dcp_pcm_trigger(sub, SNDRV_PCM_TRIGGER_SUSPEND), 0);
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_usage(audio), 1);
	KUNIT_ASSERT_EQ(test, dcp_pcm_sync_stop(sub), 0);
	KUNIT_EXPECT_STREQ(test, dcpaud_legacy_current->sequence, "TQXSD");
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_current->usage_at_drain, 1);
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_usage(audio), 0);
}

static void dcpaud_legacy_failed_start(struct kunit *test)
{
	struct dcp_audio *audio;
	struct snd_pcm_substream *sub = dcpaud_legacy_setup(test, &audio);

	/* START fails at the link; ALSA undoes it with a STOP */
	dcpaud_legacy_current->startlink_err = -EIO;
	KUNIT_ASSERT_EQ(test, dcp_pcm_trigger(sub, SNDRV_PCM_TRIGGER_START), -EIO);
	KUNIT_ASSERT_EQ(test, dcp_pcm_trigger(sub, SNDRV_PCM_TRIGGER_STOP), 0);
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_usage(audio), 1);

	/* Retried before sync_stop ran: still one reference */
	dcpaud_legacy_current->startlink_err = 0;
	KUNIT_ASSERT_EQ(test, dcp_pcm_trigger(sub, SNDRV_PCM_TRIGGER_START), 0);
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_usage(audio), 1);
	KUNIT_ASSERT_EQ(test, dcp_pcm_trigger(sub, SNDRV_PCM_TRIGGER_STOP), 0);
	KUNIT_ASSERT_EQ(test, dcp_pcm_sync_stop(sub), 0);
	KUNIT_EXPECT_STREQ(test, dcpaud_legacy_current->sequence, "TXSTQXSD");
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_usage(audio), 0);
}

static void dcpaud_legacy_void_takes_no_reference(struct kunit *test)
{
	struct dcp_audio *audio;
	struct snd_pcm_substream *sub = dcpaud_legacy_setup(test, &audio);

	/* No sink: the stream plays into the void; a 10 s period never elapses here */
	audio->open_unplugged = true;
	sub->runtime->period_size = 480000;
	KUNIT_ASSERT_EQ(test, dcp_pcm_trigger(sub, SNDRV_PCM_TRIGGER_START), 0);
	KUNIT_EXPECT_TRUE(test, audio->void_running);
	KUNIT_ASSERT_EQ(test, dcp_pcm_trigger(sub, SNDRV_PCM_TRIGGER_STOP), 0);
	KUNIT_ASSERT_EQ(test, dcp_pcm_sync_stop(sub), 0);
	KUNIT_EXPECT_STREQ(test, dcpaud_legacy_current->sequence, "D");
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_usage(audio), 0);
}

static int dcpaud_legacy_get_elements(struct device *dev, void *elements, size_t maxsize)
{
	struct dcpaud_legacy_test *t = dcpaud_legacy_current;

	t->elements_asked = maxsize;
	/* What the service returns: cut off at the buffer's end */
	memset(elements, 0xa5, min(t->catalog, maxsize));
	return 0;
}

static int dcpaud_legacy_get_attrs(struct device *dev, void *attrs, size_t maxsize)
{
	return 0;
}

static void dcpaud_legacy_free(void *data)
{
	kvfree(data);
}

static void dcpaud_legacy_elements_capacity(struct kunit *test)
{
	struct dcp_audio *audio;
	u8 *elements;

	dcpaud_legacy_setup(test, &audio);
	elements = kvzalloc(DCPAUD_ELEMENTS_MAXSIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, elements);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dcpaud_legacy_free, elements), 0);
	audio->elements = elements;
	audio->productattrs = kunit_kzalloc(test, DCPAUD_PRODUCTATTRS_MAXSIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, audio->productattrs);
	kunit_activate_static_stub(test, dcp_audiosrv_get_elements, dcpaud_legacy_get_elements);
	kunit_activate_static_stub(test, dcp_audiosrv_get_product_attrs, dcpaud_legacy_get_attrs);

	/* A TV behind a USB-C to HDMI adapter: 17312 bytes of elements */
	dcpaud_legacy_current->catalog = 17312;
	KUNIT_ASSERT_FALSE(test, dcpaud_native_link(audio));
	KUNIT_ASSERT_EQ(test, dcpaud_read_remote_info(audio), 0);
	KUNIT_EXPECT_EQ(test, dcpaud_legacy_current->elements_asked, (size_t)SZ_256K);
	KUNIT_EXPECT_EQ(test, elements[17311], 0xa5);
	KUNIT_EXPECT_EQ(test, elements[17312], 0);
}

static struct kunit_case dcpaud_legacy_cases[] = {
	KUNIT_CASE(dcpaud_legacy_stop_waits_for_dma),
	KUNIT_CASE(dcpaud_legacy_suspend_waits_for_dma),
	KUNIT_CASE(dcpaud_legacy_failed_start),
	KUNIT_CASE(dcpaud_legacy_void_takes_no_reference),
	KUNIT_CASE(dcpaud_legacy_elements_capacity),
	{}
};

static struct kunit_suite dcpaud_legacy_suite = {
	.name = "apple-dcpaud-legacy",
	.test_cases = dcpaud_legacy_cases,
};

kunit_test_suite(dcpaud_legacy_suite);
