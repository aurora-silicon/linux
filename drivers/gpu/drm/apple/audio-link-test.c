// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by audio.c: a Terminate's link release ends only the link of the
 * instance it is for. The production dcpaud_release_link() runs; the audio
 * service's answer to which instance the link is on, and its link calls,
 * are stubbed.
 */
#include <kunit/platform_device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

struct dcpaud_link_test {
	bool link_on;
	bool link_live;
	u64 asked;
	unsigned int completes;
	unsigned int stops;
	struct dma_device dma;
	struct dma_chan channel;
	char sequence[16];
	unsigned int nsequence;
};

static struct dcpaud_link_test *dcpaud_link_current;

static void dcpaud_link_record(char step)
{
	struct dcpaud_link_test *t = dcpaud_link_current;

	if (t->nsequence < sizeof(t->sequence) - 1)
		t->sequence[t->nsequence++] = step;
}

static bool dcpaud_link_test_live(struct device *dev)
{
	return dcpaud_link_current->link_live;
}

static int dcpaud_link_test_terminate(struct dma_chan *chan)
{
	return 0;
}

static void dcpaud_link_test_drain(struct dma_chan *chan)
{
	dcpaud_link_record('D');
}

static enum dma_status dcpaud_link_test_status(struct dma_chan *chan,
					      dma_cookie_t cookie, struct dma_tx_state *state)
{
	return DMA_COMPLETE;
}

static bool dcpaud_link_test_on(struct device *dev, u64 gen)
{
	dcpaud_link_current->asked = gen;
	return dcpaud_link_current->link_on;
}

static int dcpaud_link_test_call(struct device *dev, enum dcp_audio_link op,
				 const struct dcp_sound_cookie *cookie)
{
	if (op == DCP_AUDIO_LINK_COMPLETE) {
		dcpaud_link_current->completes++;
		dcpaud_link_record('C');
	} else if (op == DCP_AUDIO_LINK_STOP) {
		dcpaud_link_current->stops++;
		dcpaud_link_record('S');
	}
	return 0;
}

static struct dcp_audio *dcpaud_link_setup(struct kunit *test, struct platform_device **pdev)
{
	struct dcpaud_link_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct dcp_audio *dcpaud = kunit_kzalloc(test, sizeof(*dcpaud), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_NOT_NULL(test, dcpaud);
	*pdev = kunit_platform_device_alloc(test, "dcpaud-link", PLATFORM_DEVID_AUTO);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, *pdev);
	platform_set_drvdata(*pdev, dcpaud);
	dcpaud->dev = &(*pdev)->dev;
	dcpaud->dcp_dev = &(*pdev)->dev;
	dcpaud->native_link = true;
	mutex_init(&dcpaud->link_lock);
	dcpaud_link_current = t;
	t->link_live = true;
	t->channel.device = &t->dma;
	t->dma.device_terminate_all = dcpaud_link_test_terminate;
	t->dma.device_synchronize = dcpaud_link_test_drain;
	t->dma.device_tx_status = dcpaud_link_test_status;
	kunit_activate_static_stub(test, dcp_audiosrv_link_on, dcpaud_link_test_on);
	kunit_activate_static_stub(test, dcp_audiosrv_link_live, dcpaud_link_test_live);
	kunit_activate_static_stub(test, dcp_audiosrv_link, dcpaud_link_test_call);
	return dcpaud;
}

static void dcpaud_link_release_instance(struct kunit *test)
{
	struct platform_device *pdev;
	struct dcp_audio *dcpaud = dcpaud_link_setup(test, &pdev);
	u64 gen = 7;

	/* The link is on another instance: left as it is */
	dcpaud->link_prepared = true;
	dcpaud->link_started = true;
	dcpaud_release_link(pdev, &gen);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->asked, 7ULL);
	KUNIT_EXPECT_TRUE(test, dcpaud->link_prepared);
	KUNIT_EXPECT_TRUE(test, dcpaud->link_started);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->stops + dcpaud_link_current->completes, 0U);

	/* On the instance asked for: stopped and completed */
	dcpaud_link_current->link_on = true;
	dcpaud_release_link(pdev, &gen);
	KUNIT_EXPECT_FALSE(test, dcpaud->link_prepared);
	KUNIT_EXPECT_FALSE(test, dcpaud->link_started);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->stops, 1U);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->completes, 1U);

	/* Without an instance (the display's own power-off): whichever link */
	dcpaud_link_current->link_on = false;
	dcpaud->link_prepared = true;
	dcpaud_release_link(pdev, NULL);
	KUNIT_EXPECT_FALSE(test, dcpaud->link_prepared);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->completes, 2U);
}

static void dcpaud_link_test_close(void *data)
{
	snd_dmaengine_pcm_close(data);
}

static struct snd_pcm_substream *dcpaud_link_test_open(struct kunit *test,
						     struct dcp_audio *dcpaud)
{
	struct snd_pcm_substream *sub = kunit_kzalloc(test, sizeof(*sub), GFP_KERNEL);
	struct snd_pcm_runtime *runtime = kunit_kzalloc(test, sizeof(*runtime), GFP_KERNEL);
	struct snd_pcm *pcm = kunit_kzalloc(test, sizeof(*pcm), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sub);
	KUNIT_ASSERT_NOT_NULL(test, runtime);
	KUNIT_ASSERT_NOT_NULL(test, pcm);
	pcm->private_data = dcpaud;
	pcm->nonatomic = true;
	sub->pcm = pcm;
	sub->runtime = runtime;
	KUNIT_ASSERT_EQ(test, snd_dmaengine_pcm_open(sub, &dcpaud_link_current->channel), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dcpaud_link_test_close, sub), 0);
	return sub;
}

static void dcpaud_disconnect_sync_completes_live_owner(struct kunit *test)
{
	struct platform_device *pdev;
	struct dcp_audio *audio = dcpaud_link_setup(test, &pdev);
	struct snd_pcm_substream *sub = dcpaud_link_test_open(test, audio);

	/* Disconnect cleared connected; the deferred owner release has not run. */
	audio->connected = false;
	audio->link_prepared = true;
	audio->link_started = true;
	KUNIT_ASSERT_EQ(test, dcp_pcm_native_sync_stop(sub), 0);
	KUNIT_EXPECT_STREQ(test, dcpaud_link_current->sequence, "DSC");
	KUNIT_EXPECT_FALSE(test, audio->link_prepared);
	KUNIT_EXPECT_FALSE(test, audio->link_started);
	dcpaud_release_link(pdev, NULL);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->stops, 1U);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->completes, 1U);
}

static void dcpaud_gone_service_drops_link(struct kunit *test)
{
	struct platform_device *pdev;
	struct dcp_audio *audio = dcpaud_link_setup(test, &pdev);
	struct snd_pcm_substream *sub = dcpaud_link_test_open(test, audio);

	audio->link_prepared = true;
	audio->link_started = true;
	dcpaud_link_current->link_live = false;
	KUNIT_ASSERT_EQ(test, dcp_pcm_native_sync_stop(sub), 0);
	KUNIT_EXPECT_STREQ(test, dcpaud_link_current->sequence, "D");
	KUNIT_EXPECT_FALSE(test, audio->link_prepared);
	KUNIT_EXPECT_FALSE(test, audio->link_started);
}

static struct kunit_case dcpaud_link_cases[] = {
	KUNIT_CASE(dcpaud_link_release_instance),
	KUNIT_CASE(dcpaud_disconnect_sync_completes_live_owner),
	KUNIT_CASE(dcpaud_gone_service_drops_link),
	{}
};

static struct kunit_suite dcpaud_link_suite = {
	.name = "apple-dcpaud-release-link",
	.test_cases = dcpaud_link_cases,
};

kunit_test_suite(dcpaud_link_suite);
