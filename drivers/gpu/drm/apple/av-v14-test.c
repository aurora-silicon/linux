// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by av.c: which instance a native external role's audio client
 * opens and closes. One reusable service slot is published, torn down and
 * published again by the production init and teardown; the production open,
 * close, Terminate report and Terminate work run on it, with only the
 * service calls stubbed. The works are disabled for the workqueue and run
 * here, in test context, where the stubs apply.
 */
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

struct av_v14_test {
	struct apple_dcp *dcp;
	struct audiosrv_data *asrv;
	struct apple_dcp_afkep ep;
	struct apple_epic_service svc;
	struct apple_epic_service other;
	/* Service calls: the command, and the instance generation it went to */
	u32 calls[16];
	u64 call_gens[16];
	unsigned int ncalls;
	/* The card's disconnects, and link releases with the instance asked for */
	unsigned int disconnects;
	unsigned int releases;
	u64 release_gen;
	/* Within a link release: the old PCM closes, a successor is published and opened */
	bool successor_during_release;
	u32 link_status;
	u8 link_body[64];
	u8 link_bodies[16][64];
	u32 fail_command;
	int call_error;
	size_t capacities[16];
	unsigned int nobjects;
	size_t object_needed;
	int object_error;
};

static struct av_v14_test *av_v14_current;

static int av_v14_call(struct apple_epic_service *srv, u32 command, const void *body,
		       size_t body_len, size_t body_pad, void *reply, size_t reply_len)
{
	struct av_v14_test *t = av_v14_current;

	if (t->ncalls < ARRAY_SIZE(t->calls)) {
		t->calls[t->ncalls] = command;
		t->call_gens[t->ncalls] = t->asrv->gen;
	}
	if (body_len == sizeof(t->link_body) && t->ncalls < ARRAY_SIZE(t->calls))
		memcpy(t->link_bodies[t->ncalls], body, body_len);
	t->ncalls++;
	if (command == t->fail_command && t->call_error)
		return t->call_error;
	memset(reply, 0, reply_len);
	/* Open: status 0 at +0, opened at +16 */
	if (command == t->asrv->cmds.open && reply_len >= 24)
		put_unaligned_le64(1, reply + 16);
	if (body_len == sizeof(t->link_body)) {
		memcpy(t->link_body, body, body_len);
		put_unaligned_le32(command == t->fail_command ? 1 : t->link_status, reply + 48);
	}
	return 0;
}

static void av_v14_card_disconnect(struct platform_device *pdev)
{
	av_v14_current->disconnects++;
}

static void av_v14_replace(struct av_v14_test *t);

static void av_v14_release_link(struct platform_device *pdev, const u64 *gen)
{
	struct av_v14_test *t = av_v14_current;

	t->releases++;
	t->release_gen = gen ? *gen : 0;
	if (!t->successor_during_release)
		return;
	t->successor_during_release = false;
	dcp_audiosrv_close(t->dcp->dev);
	av_v14_replace(t);
	KUNIT_EXPECT_EQ(kunit_get_current_test(), dcp_audiosrv_open(t->dcp->dev), 0);
}

static void av_v14_publish(struct av_v14_test *t)
{
	t->svc.enabled = true;
	t->svc.torndown = false;
	if (t->ep.compact) {
		const char *name = "dcpav-audio-interface-epic";
		const char *class = "dcpav-interface-ep";

		KUNIT_ASSERT_EQ(kunit_get_current_test(),
				av_desktop_audio_prepare(&t->svc, name, class, 0), 0);
		av_audiosrv_init(&t->svc, name, class, 0);
	} else {
		av_audiosrv_init(&t->svc, "DCPAVAudioInterface", "test", 0);
	}
}

/* The instance removed, and its successor published in the same slot */
static void av_v14_replace(struct av_v14_test *t)
{
	av_audiosrv_teardown(&t->svc);
	av_v14_publish(t);
}

static void av_v14_terminate(struct av_v14_test *t, struct apple_epic_service *service)
{
	struct epic_service_call call = {
		.group = cpu_to_le16(2),
		.command = cpu_to_le32(1),
		.magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC),
	};

	av_audiosrv_report(service, 0, &call, sizeof(call));
}

static void av_v14_run_terminate_work(struct av_v14_test *t)
{
	av_work_service_terminate(&t->asrv->terminate_wq);
}

static struct av_v14_test *av_v14_setup(struct kunit *test)
{
	struct av_v14_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct audiosrv_data *asrv;
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->dcp = kunit_kzalloc(test, sizeof(*t->dcp), GFP_KERNEL);
	asrv = kunit_kzalloc(test, sizeof(*asrv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->dcp);
	KUNIT_ASSERT_NOT_NULL(test, asrv);
	dev = kunit_device_register(test, "av-v14");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	dev_set_drvdata(dev, t->dcp);
	t->dcp->dev = dev;
	t->dcp->fw_compat = test->param_value && *(const bool *)test->param_value ?
		DCP_FIRMWARE_V_26_6_DESKTOP : DCP_FIRMWARE_V_14_7;
	t->dcp->external = true;
	t->dcp->external_native = true;

	init_rwsem(&asrv->srv_rwsem);
	mutex_init(&asrv->plug_lock);
	mutex_init(&asrv->open_lock);
	asrv->cmds = dcp_av_audio_cmds_v13_5;
	INIT_WORK(&asrv->start_av_service_wq, av_work_service_start);
	disable_work(&asrv->start_av_service_wq);
	INIT_WORK(&asrv->terminate_wq, av_work_service_terminate);
	disable_work(&asrv->terminate_wq);
	t->asrv = asrv;
	t->dcp->audiosrv = asrv;

	t->ep.dcp = t->dcp;
	t->ep.endpoint = AV_AUDIO_ENDPOINT_DESKTOP;
	t->ep.compact = t->dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP;
	spin_lock_init(&t->svc.lock);
	t->svc.ep = &t->ep;
	spin_lock_init(&t->other.lock);
	t->other.ep = &t->ep;
	t->other.enabled = true;
	av_v14_current = t;
	kunit_activate_static_stub(test, av_audiosrv_call, av_v14_call);
	kunit_activate_static_stub(test, dcpaud_disconnect, av_v14_card_disconnect);
	kunit_activate_static_stub(test, dcpaud_release_link, av_v14_release_link);
	/* A card, never dereferenced: its disconnect and link release are stubbed */
	asrv->audio_dev = (struct platform_device *)t;
	av_v14_publish(t);
	return t;
}

/* Serial: one Open per instance while a PCM is open, one Close after it. */
static void av_v14_open_close(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);

	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	KUNIT_EXPECT_EQ(test, t->svc.users, 2U);	/* published, and open */
	dcp_audiosrv_close(t->dcp->dev);
	dcp_audiosrv_close(t->dcp->dev);
	KUNIT_ASSERT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_EQ(test, t->calls[0], t->asrv->cmds.open);
	KUNIT_EXPECT_EQ(test, t->calls[1], t->asrv->cmds.close);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/* A successor in the slot of an instance left open is opened, not skipped. */
static void av_v14_successor_opened(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);

	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	av_v14_replace(t);
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	KUNIT_ASSERT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_EQ(test, t->calls[1], t->asrv->cmds.open);
	KUNIT_EXPECT_EQ(test, t->call_gens[1], t->asrv->gen);
	KUNIT_EXPECT_NE(test, t->call_gens[1], t->call_gens[0]);
	/* Its Close goes to it */
	dcp_audiosrv_close(t->dcp->dev);
	KUNIT_ASSERT_EQ(test, t->ncalls, 3U);
	KUNIT_EXPECT_EQ(test, t->calls[2], t->asrv->cmds.close);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/* The Close of an instance gone is not sent to its successor in the same slot. */
static void av_v14_old_close_not_to_successor(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);

	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	av_v14_replace(t);
	dcp_audiosrv_close(t->dcp->dev);
	KUNIT_EXPECT_EQ(test, t->ncalls, 1U);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/*
 * A Terminate's work, queued for an instance, runs after its successor took
 * the slot and was opened: the successor stays open. The successor's own
 * Terminate closes it.
 */
static void av_v14_old_terminate_vs_successor(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);

	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	av_v14_terminate(t, &t->svc);
	av_v14_replace(t);
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	KUNIT_ASSERT_EQ(test, t->ncalls, 2U);
	av_v14_run_terminate_work(t);
	KUNIT_EXPECT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_PTR_EQ(test, t->asrv->open_srv, &t->svc);
	KUNIT_EXPECT_EQ(test, t->asrv->open_gen, t->asrv->gen);

	av_v14_terminate(t, &t->svc);
	av_v14_run_terminate_work(t);
	KUNIT_ASSERT_EQ(test, t->ncalls, 3U);
	KUNIT_EXPECT_EQ(test, t->calls[2], t->asrv->cmds.close);
	KUNIT_EXPECT_EQ(test, t->call_gens[2], t->asrv->gen);
	KUNIT_EXPECT_NULL(test, t->asrv->open_srv);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/* An instance published over the open one, without a teardown, is opened too. */
static void av_v14_republished(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);

	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	av_v14_publish(t);
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	KUNIT_EXPECT_EQ(test, t->ncalls, 2U);
	dcp_audiosrv_close(t->dcp->dev);
	KUNIT_EXPECT_EQ(test, t->ncalls, 3U);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/*
 * A Terminate's disconnect is past its instance check when, within its link
 * release, the old PCM closes and a successor is published in the same slot
 * and opened: the Terminate's final Close leaves the successor open.
 */
static void av_v14_successor_during_release(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);
	u64 old;

	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	old = t->asrv->gen;
	av_v14_terminate(t, &t->svc);
	t->successor_during_release = true;
	av_v14_run_terminate_work(t);
	/* The Terminate's, and the old instance's teardown's */
	KUNIT_EXPECT_EQ(test, t->disconnects, 2U);
	KUNIT_EXPECT_EQ(test, t->releases, 1U);
	KUNIT_EXPECT_EQ(test, t->release_gen, old);
	/* Open, the old PCM's Close, the successor's Open; no Close after it */
	KUNIT_ASSERT_EQ(test, t->ncalls, 3U);
	KUNIT_EXPECT_EQ(test, t->calls[1], t->asrv->cmds.close);
	KUNIT_EXPECT_EQ(test, t->call_gens[1], old);
	KUNIT_EXPECT_EQ(test, t->calls[2], t->asrv->cmds.open);
	KUNIT_EXPECT_NE(test, t->call_gens[2], old);
	KUNIT_EXPECT_PTR_EQ(test, t->asrv->open_srv, &t->svc);
	KUNIT_EXPECT_EQ(test, t->asrv->open_gen, t->asrv->gen);
	dcp_audiosrv_close(t->dcp->dev);
	KUNIT_EXPECT_EQ(test, t->ncalls, 4U);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/* A Terminate that finds a successor published does nothing to it or the card. */
static void av_v14_terminate_after_successor(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);

	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	av_v14_terminate(t, &t->svc);
	av_v14_replace(t);
	av_v14_run_terminate_work(t);
	/* The old instance's teardown's only */
	KUNIT_EXPECT_EQ(test, t->disconnects, 1U);
	KUNIT_EXPECT_EQ(test, t->releases, 0U);
}

/*
 * A link ends on the instance it was prepared on: once a successor is open,
 * the old link's stop and complete are not sent, and its link is not on any
 * open instance; a link prepared on the successor is.
 */
static void av_v14_link_instance(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);
	struct dcp_sound_cookie cookie = {};
	u64 old;

	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	old = t->asrv->gen;
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_PREPARE,
					      &cookie, 0), 0);
	KUNIT_EXPECT_TRUE(test, dcp_audiosrv_link_on(t->dcp->dev, old));
	av_v14_replace(t);
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	KUNIT_ASSERT_EQ(test, t->ncalls, 3U);
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_STOP,
					      &cookie, 0), -ENODEV);
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_COMPLETE,
					      &cookie, 0),
			-ENODEV);
	KUNIT_EXPECT_EQ(test, t->ncalls, 3U);
	KUNIT_EXPECT_FALSE(test, dcp_audiosrv_link_on(t->dcp->dev, old));
	KUNIT_EXPECT_FALSE(test, dcp_audiosrv_link_on(t->dcp->dev, t->asrv->gen));
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_PREPARE,
					      &cookie, 0), 0);
	KUNIT_EXPECT_TRUE(test, dcp_audiosrv_link_on(t->dcp->dev, t->asrv->gen));
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_COMPLETE,
					      &cookie, 0), 0);
	KUNIT_EXPECT_EQ(test, t->ncalls, 5U);
}

/* A Terminate from an instance that is not the published one does nothing. */
static void av_v14_terminate_unpublished(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);

	KUNIT_EXPECT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	av_v14_terminate(t, &t->other);
	av_v14_run_terminate_work(t);
	KUNIT_EXPECT_EQ(test, t->ncalls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, t->asrv->open_srv, &t->svc);
	dcp_audiosrv_close(t->dcp->dev);
	KUNIT_EXPECT_EQ(test, t->ncalls, 2U);
}

/* Rejected prepares must not create a link which Stop/Complete can act on. */
static void av_native_link_payload_status(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);
	struct dcp_sound_cookie cookie;
	bool desktop = t->ep.compact;
	const u32 desktop_calls[] = { 4, 6, 6, 7, 8, 9, 10, 11 };
	const u32 legacy_calls[] = { 4, 6, 6, 7, 10, 11 };
	const u32 *calls = desktop ? desktop_calls : legacy_calls;
	size_t ncalls = desktop ? ARRAY_SIZE(desktop_calls) : ARRAY_SIZE(legacy_calls);

	memset(&cookie, 0xa5, sizeof(cookie));
	KUNIT_ASSERT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	t->link_status = 1;
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_PREPARE,
					      &cookie, 0x5a), -EIO);
	KUNIT_EXPECT_FALSE(test, dcp_audiosrv_link_on(t->dcp->dev, t->asrv->gen));
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_START,
					      &cookie, 0x5a), -ENODEV);
	t->link_status = 0;
	for (enum dcp_audio_link op = DCP_AUDIO_LINK_PREPARE;
	     op <= DCP_AUDIO_LINK_COMPLETE; op++)
		KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, op, &cookie, 0x5a), 0);
	KUNIT_ASSERT_EQ(test, t->ncalls, ncalls);
	for (size_t n = 0; n < ncalls; n++) {
		KUNIT_EXPECT_EQ(test, t->calls[n], calls[n]);
		if (!n)
			continue;
		KUNIT_EXPECT_MEMEQ(test, t->link_bodies[n], &cookie, sizeof(cookie));
		KUNIT_EXPECT_EQ(test, t->link_bodies[n][0x20], 0x5a);
		for (size_t i = 0x21; i < sizeof(t->link_body); i++)
			KUNIT_EXPECT_EQ(test, t->link_bodies[n][i], 0);
	}
	dcp_audiosrv_close(t->dcp->dev);
}

/* A failed DidStart unwinds Start; a failed WillStop still sends Stop. */
static void av_desktop_notification_errors(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);
	struct dcp_sound_cookie cookie = {};

	if (!t->ep.compact)
		return;
	KUNIT_ASSERT_EQ(test, dcp_audiosrv_open(t->dcp->dev), 0);
	KUNIT_ASSERT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_PREPARE,
					      &cookie, 1), 0);
	t->ncalls = 0;
	t->fail_command = 7;
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_START,
					      &cookie, 1), -EIO);
	KUNIT_ASSERT_EQ(test, t->ncalls, 1U);
	KUNIT_EXPECT_EQ(test, t->calls[0], 7U);
	t->ncalls = 0;
	t->fail_command = 8;
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_START,
					      &cookie, 1), -EIO);
	KUNIT_ASSERT_EQ(test, t->ncalls, 4U);
	KUNIT_EXPECT_EQ(test, t->calls[0], 7U);
	KUNIT_EXPECT_EQ(test, t->calls[1], 8U);
	KUNIT_EXPECT_EQ(test, t->calls[2], 9U);
	KUNIT_EXPECT_EQ(test, t->calls[3], 10U);
	t->fail_command = 9;
	t->call_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_STOP,
					      &cookie, 1), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, t->calls[4], 9U);
	KUNIT_EXPECT_EQ(test, t->calls[5], 10U);
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_link(t->dcp->dev, DCP_AUDIO_LINK_COMPLETE,
					      &cookie, 1), 0);
	KUNIT_EXPECT_EQ(test, t->calls[6], 11U);
	dcp_audiosrv_close(t->dcp->dev);
}

static int av_native_object_call(struct apple_epic_service *srv, u16 group,
				u32 command, void *output, size_t capacity, size_t *used)
{
	struct av_v14_test *t = av_v14_current;

	if (t->nobjects < ARRAY_SIZE(t->capacities))
		t->capacities[t->nobjects] = capacity;
	t->nobjects++;
	if (t->object_error)
		return t->object_error;
	if (capacity < t->object_needed)
		return -ENOSPC;
	*used = t->object_needed;
	return 0;
}

static void av_desktop_catalog_growth(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);
	size_t used = 0;

	if (!t->ep.compact)
		return;
	kunit_activate_static_stub(test, dcp_audiosrv_osobject_call, av_native_object_call);
	t->object_needed = SZ_8M;
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_osobject_get(&t->svc, 16, NULL, SZ_8M, &used), 0);
	KUNIT_EXPECT_EQ(test, used, (size_t)SZ_8M);
	KUNIT_ASSERT_EQ(test, t->nobjects, 6U);
	for (size_t i = 0; i < t->nobjects; i++)
		KUNIT_EXPECT_EQ(test, t->capacities[i], (size_t)SZ_256K << i);
	t->nobjects = 0;
	t->object_needed = SZ_8M + 1;
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_osobject_get(&t->svc, 16, NULL, SZ_8M, &used),
			-ENOSPC);
	KUNIT_EXPECT_EQ(test, t->nobjects, 6U);
	t->nobjects = 0;
	t->object_error = -EMSGSIZE;
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_osobject_get(&t->svc, 16, NULL, SZ_8M, &used),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, t->nobjects, 1U);
	t->object_error = 0;
	t->nobjects = 0;
	t->object_needed = SZ_8M;
	KUNIT_EXPECT_EQ(test, dcp_audiosrv_osobject_get(&t->svc, 18, NULL, SZ_8M, &used), 0);
	KUNIT_EXPECT_EQ(test, t->capacities[0], (size_t)SZ_32K);
	KUNIT_EXPECT_EQ(test, t->nobjects, 9U);
}

static void av_desktop_service_admission(struct kunit *test)
{
	struct av_v14_test *t = av_v14_setup(test);
	const char *name = "dcpav-audio-interface-epic";
	const char *class = "dcpav-interface-ep";
	bool desktop = t->dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP;

	KUNIT_EXPECT_TRUE(test, dcp_audiosrv_native_link(t->dcp->dev));
	KUNIT_EXPECT_EQ(test, av_desktop_audio_prepare(&t->svc, name, class, 0),
			desktop ? 0 : -EINVAL);
	t->dcp->fw_compat = DCP_FIRMWARE_V_26_6_DESKTOP;
	t->ep.endpoint = AV_ENDPOINT;
	KUNIT_EXPECT_EQ(test, av_desktop_audio_prepare(&t->svc, name, class, 0), -EINVAL);
	t->ep.endpoint = AV_AUDIO_ENDPOINT_DESKTOP;
	KUNIT_EXPECT_EQ(test, av_desktop_audio_prepare(&t->svc, "dcpav-device-epic", class, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, av_desktop_audio_prepare(&t->svc, NULL, class, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, av_desktop_audio_prepare(&t->svc, name, class, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, av_desktop_audio_prepare(&t->svc, name, name, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, av_desktop_audio_prepare(&t->svc, name, NULL, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, av_desktop_audio_prepare(&t->svc, "dcpav-audio-epic", class, 0),
			-EINVAL);
	t->dcp->external = false;
	KUNIT_EXPECT_FALSE(test, dcp_audiosrv_native_link(t->dcp->dev));
	KUNIT_EXPECT_EQ(test, av_desktop_audio_prepare(&t->svc, name, class, 0), -EINVAL);
	dcp_audiosrv_close(t->dcp->dev);
}

static const bool av_native_desktop[] = { false, true };

static void av_native_desc(const bool *desktop, char *desc)
{
	scnprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s", *desktop ? "desktop-26" : "external-14.7");
}

KUNIT_ARRAY_PARAM(av_native, av_native_desktop, av_native_desc);

static struct kunit_case av_v14_cases[] = {
	KUNIT_CASE_PARAM(av_v14_open_close, av_native_gen_params),
	KUNIT_CASE_PARAM(av_v14_successor_opened, av_native_gen_params),
	KUNIT_CASE_PARAM(av_v14_old_close_not_to_successor, av_native_gen_params),
	KUNIT_CASE_PARAM(av_v14_old_terminate_vs_successor, av_native_gen_params),
	KUNIT_CASE_PARAM(av_v14_republished, av_native_gen_params),
	KUNIT_CASE_PARAM(av_v14_successor_during_release, av_native_gen_params),
	KUNIT_CASE_PARAM(av_v14_terminate_after_successor, av_native_gen_params),
	KUNIT_CASE_PARAM(av_v14_link_instance, av_native_gen_params),
	KUNIT_CASE_PARAM(av_v14_terminate_unpublished, av_native_gen_params),
	KUNIT_CASE_PARAM(av_native_link_payload_status, av_native_gen_params),
	KUNIT_CASE_PARAM(av_desktop_service_admission, av_native_gen_params),
	KUNIT_CASE_PARAM(av_desktop_notification_errors, av_native_gen_params),
	KUNIT_CASE_PARAM(av_desktop_catalog_growth, av_native_gen_params),
	{}
};

static struct kunit_suite av_v14_suite = {
	.name = "apple-av-native-audio",
	.test_cases = av_v14_cases,
};

kunit_test_suite(av_v14_suite);
