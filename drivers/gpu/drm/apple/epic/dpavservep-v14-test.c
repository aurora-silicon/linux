// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by dpavservep.c: the AV client of the DCP 14.7 external role.
 * The production open, close, Terminate, Close work and EDID copy run on a
 * fake service instance; only the client calls and the EDID read are
 * stubbed. The Close work is disabled for the workqueue and run here, in
 * test context, where the stubs apply; the Open stub can act while the Open
 * is on its way, to run each interleaving.
 */
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

#define AV14_OPEN	4
#define AV14_CLOSE	5

enum av14_meanwhile {
	AV14_NOTHING,
	AV14_HPD_LOW,		/* dcpavserv_v14_close(), as before an HPD low */
	AV14_TERMINATE,		/* the firmware's Terminate report */
	AV14_REPLACED,		/* teardown, and a successor in the same slot */
	AV14_COPY,		/* another EDID copy */
};

struct av14_test {
	struct apple_dcp *dcp;
	struct apple_dcp_afkep ep;
	struct apple_epic_service svc;
	/* Client calls, as group << 16 | command, and the epoch each went to */
	u32 calls[8];
	u64 call_epochs[8];
	unsigned int ncalls;
	int call_ret;
	u32 open_status;
	u64 open_opened;
	u32 close_status;
	enum av14_meanwhile during_open;
	unsigned int reads;
	bool read_valid;
};

static struct av14_test *av14_current;

static void av14_report(struct av14_test *t, u16 group, u32 command, u32 magic, size_t size);
static void av14_replace(struct av14_test *t);

static int av14_call(struct apple_epic_service *service, u16 group, u32 command,
		     void *reply, size_t size)
{
	struct av14_test *t = av14_current;
	enum av14_meanwhile meanwhile = AV14_NOTHING;

	if (t->ncalls < ARRAY_SIZE(t->calls)) {
		t->calls[t->ncalls] = (u32)group << 16 | command;
		t->call_epochs[t->ncalls] = t->dcp->dcpavserv.epoch;
	}
	t->ncalls++;
	if (command == AV14_OPEN) {
		meanwhile = t->during_open;
		t->during_open = AV14_NOTHING;
	}
	switch (meanwhile) {
	case AV14_HPD_LOW:
		dcpavserv_v14_close(t->dcp);
		break;
	case AV14_TERMINATE:
		av14_report(t, 2, 1, EPIC_SERVICE_CALL_MAGIC, sizeof(struct epic_service_call));
		break;
	case AV14_REPLACED:
		av14_replace(t);
		break;
	case AV14_COPY: {
		u64 epoch;
		const struct drm_edid *edid = dcpavserv_copy_edid(t->dcp, 0, &epoch);

		if (!IS_ERR_OR_NULL(edid))
			drm_edid_free(edid);
		break;
	}
	case AV14_NOTHING:
		break;
	}
	if (t->call_ret)
		return t->call_ret;
	memset(reply, 0, size);
	if (command == AV14_OPEN && size >= 24) {
		put_unaligned_le32(t->open_status, reply);
		put_unaligned_le64(t->open_opened, reply + 16);
	} else if (command == AV14_CLOSE && size >= 4) {
		put_unaligned_le32(t->close_status, reply);
	}
	return 0;
}

/* One EDID base block: valid, or with its checksum off by one */
static const struct drm_edid *av14_read(struct apple_epic_service *service)
{
	u8 block[EDID_LENGTH] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	unsigned int i;
	u8 sum = 0;

	av14_current->reads++;
	block[18] = 1;	/* EDID 1.3 */
	block[19] = 3;
	for (i = 0; i < EDID_LENGTH - 1; i++)
		sum += block[i];
	block[EDID_LENGTH - 1] = 0x100 - sum + (av14_current->read_valid ? 0 : 1);
	return drm_edid_alloc(block, sizeof(block));
}

static void av14_stop(void *data)
{
	struct apple_dcp *dcp = data;

	cancel_work_sync(&dcp->dcpavserv.close_wq);
}

/* Publish the instance in the slot, as dcpavserv_init() does */
static void av14_publish(struct av14_test *t)
{
	t->svc.enabled = true;
	t->svc.torndown = false;
	guard(mutex)(&t->dcp->dcpavserv.lock);
	t->dcp->dcpavserv.service = afk_service_get(&t->svc);
	t->dcp->dcpavserv.enabled = true;
	t->dcp->dcpavserv.epoch++;
	t->dcp->dcpavserv.opened = NULL;
	t->dcp->dcpavserv.opened_epoch = 0;
}

/* The instance torn down (production teardown) and a successor in its slot */
static void av14_replace(struct av14_test *t)
{
	dcpavserv_teardown(&t->svc);
	av14_publish(t);
}

/* A published AV instance, on the 14.7 external role or (!native) not */
static struct av14_test *av14_setup(struct kunit *test, bool native)
{
	struct av14_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->dcp = kunit_kzalloc(test, sizeof(*t->dcp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->dcp);
	dev = kunit_device_register(test, native ? "dcpavserv-v14" : "dcpavserv-v14-other");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	t->dcp->dev = dev;
	t->dcp->fw_compat = DCP_FIRMWARE_V_14_7;
	t->dcp->external = native;
	t->dcp->external_native = native;
	mutex_init(&t->dcp->hpd_mutex);
	mutex_init(&t->dcp->dcpavserv.lock);
	INIT_WORK(&t->dcp->dcpavserv.close_wq, dcpavserv_close_work);
	/* Queued work runs here, in test context: av14_run_close_work() */
	disable_work(&t->dcp->dcpavserv.close_wq);
	t->ep.dcp = t->dcp;
	spin_lock_init(&t->svc.lock);
	t->svc.ep = &t->ep;
	av14_publish(t);
	t->open_opened = 1;
	t->read_valid = true;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, av14_stop, t->dcp), 0);
	av14_current = t;
	kunit_activate_static_stub(test, dcpavserv_v14_call, av14_call);
	kunit_activate_static_stub(test, dcpavserv_read_edid_legacy, av14_read);
	return t;
}

static int av14_copy(struct kunit *test, struct av14_test *t)
{
	const struct drm_edid *edid;
	u64 epoch;

	edid = dcpavserv_copy_edid(t->dcp, 0, &epoch);
	if (IS_ERR(edid))
		return PTR_ERR(edid);
	KUNIT_EXPECT_NOT_NULL(test, edid);
	drm_edid_free(edid);
	return 0;
}

static void av14_run_close_work(struct av14_test *t)
{
	dcpavserv_close_work(&t->dcp->dcpavserv.close_wq);
}

static void av14_report(struct av14_test *t, u16 group, u32 command, u32 magic, size_t size)
{
	struct epic_service_call call = {
		.group = cpu_to_le16(group),
		.command = cpu_to_le32(command),
		.magic = cpu_to_le32(magic),
	};

	dcpavserv_report(&t->svc, EPIC_SUBTYPE_STD_SERVICE, &call, size);
}

static void av14_terminate(struct av14_test *t)
{
	av14_report(t, 2, 1, EPIC_SERVICE_CALL_MAGIC, sizeof(struct epic_service_call));
}

/* Each instance is opened once (DCPAV Open, group 0), before its first EDID read. */
static void av14_open_once(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);

	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_EXPECT_EQ(test, t->ncalls, 1U);
	KUNIT_EXPECT_EQ(test, t->calls[0], (u32)AV14_OPEN);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->dcpavserv.opened, &t->svc);
	KUNIT_EXPECT_EQ(test, t->dcp->dcpavserv.opened_epoch, t->dcp->dcpavserv.epoch);
	KUNIT_EXPECT_FALSE(test, t->dcp->dcpavserv.opening);
	KUNIT_EXPECT_EQ(test, t->reads, 2U);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/* A refused open is not recorded: the read goes on, the next copy opens again. */
static void av14_open_refused(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);

	t->open_status = 0xe00002bc;
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.opened);
	t->open_status = 0;
	t->open_opened = 0;	/* answered, but not opened */
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.opened);
	t->call_ret = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.opened);
	KUNIT_EXPECT_EQ(test, t->ncalls, 3U);
	KUNIT_EXPECT_EQ(test, t->reads, 3U);
	KUNIT_EXPECT_FALSE(test, t->dcp->dcpavserv.opening);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/* Not the 14.7 external role (as the T8122 dcpext): no client calls, EDID as read. */
static void av14_other_role(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, false);

	t->read_valid = false;
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	dcpavserv_v14_close(t->dcp);
	av14_terminate(t);
	KUNIT_EXPECT_EQ(test, t->ncalls, 0U);
	KUNIT_EXPECT_EQ(test, t->reads, 1U);
}

/* The 14.7 external role's EDID is checked block by block before it is published. */
static void av14_edid_checked(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);

	t->read_valid = false;
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), -EPROTO);
	t->read_valid = true;
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
}

/* Close (group 0, command 5) goes only to the instance that was opened, once. */
static void av14_close_opened_only(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);

	dcpavserv_v14_close(t->dcp);
	KUNIT_EXPECT_EQ(test, t->ncalls, 0U);
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	dcpavserv_v14_close(t->dcp);
	dcpavserv_v14_close(t->dcp);
	KUNIT_ASSERT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_EQ(test, t->calls[1], (u32)AV14_CLOSE);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.opened);
	/* A Close the firmware refuses still leaves nothing recorded open */
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	t->close_status = 0xe00002bc;
	dcpavserv_v14_close(t->dcp);
	KUNIT_EXPECT_EQ(test, t->ncalls, 4U);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.opened);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/*
 * Terminate (report group 2, command 1) on the opened instance queues its
 * Close; the instance gets no new use, before or after that Close, until a
 * successor is published. Other reports change nothing.
 */
static void av14_terminate_opened(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);
	u64 epoch = t->dcp->dcpavserv.epoch;

	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	av14_report(t, 2, 2, EPIC_SERVICE_CALL_MAGIC, sizeof(struct epic_service_call));
	av14_report(t, 1, 1, EPIC_SERVICE_CALL_MAGIC, sizeof(struct epic_service_call));
	av14_report(t, 2, 1, 0, sizeof(struct epic_service_call));
	av14_report(t, 2, 1, EPIC_SERVICE_CALL_MAGIC, sizeof(struct epic_service_call) - 1);
	KUNIT_EXPECT_EQ(test, t->dcp->dcpavserv.terminated_epoch, 0ULL);

	av14_terminate(t);
	KUNIT_EXPECT_EQ(test, t->dcp->dcpavserv.terminated_epoch, epoch);
	KUNIT_EXPECT_EQ(test, t->dcp->dcpavserv.close_epoch, epoch);
	/* While its Close is queued: no EDID, no new open */
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), -ENODEV);
	av14_run_close_work(t);
	KUNIT_ASSERT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_EQ(test, t->calls[1], (u32)AV14_CLOSE);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.opened);
	/* Closed: still no new use of the terminated instance */
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), -ENODEV);
	av14_run_close_work(t);
	KUNIT_EXPECT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_EQ(test, t->reads, 1U);
	/* Its successor, in the same slot, is opened and read */
	av14_replace(t);
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_EXPECT_EQ(test, t->ncalls, 3U);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/*
 * A Close (as before an HPD low) after a Terminate does not end the
 * Terminate: whether it comes after the queued Close ran or before it, no
 * EDID copy opens or reads the terminated instance again. The successor, a
 * new epoch in the same slot, is opened.
 */
static void av14_terminate_then_close(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);
	u64 epoch;
	int round;

	for (round = 0; round < 2; round++) {
		unsigned int calls = t->ncalls, reads = t->reads;

		epoch = t->dcp->dcpavserv.epoch;
		KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
		av14_terminate(t);
		if (!round)
			av14_run_close_work(t);
		dcpavserv_v14_close(t->dcp);
		if (round)
			av14_run_close_work(t);
		/* One Open, one Close, in this instance */
		KUNIT_ASSERT_EQ(test, t->ncalls, calls + 2);
		KUNIT_EXPECT_EQ(test, t->calls[calls], (u32)AV14_OPEN);
		KUNIT_EXPECT_EQ(test, t->calls[calls + 1], (u32)AV14_CLOSE);
		KUNIT_EXPECT_EQ(test, t->dcp->dcpavserv.terminated_epoch, epoch);
		/* Not opened or read again */
		KUNIT_EXPECT_EQ(test, av14_copy(test, t), -ENODEV);
		KUNIT_EXPECT_EQ(test, av14_copy(test, t), -ENODEV);
		KUNIT_EXPECT_EQ(test, t->ncalls, calls + 2);
		KUNIT_EXPECT_EQ(test, t->reads, reads + 1);
		/* Its successor in the same slot: a new epoch, opened */
		av14_replace(t);
		KUNIT_EXPECT_NE(test, t->dcp->dcpavserv.epoch, epoch);
	}
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_EXPECT_EQ(test, t->calls[4], (u32)AV14_OPEN);
	KUNIT_EXPECT_EQ(test, t->call_epochs[4], t->dcp->dcpavserv.epoch);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/* An HPD low's Close while the Open is on its way: its opener closes the client. */
static void av14_open_vs_close(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);

	t->during_open = AV14_HPD_LOW;
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), -ENODEV);
	KUNIT_ASSERT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_EQ(test, t->calls[0], (u32)AV14_OPEN);
	KUNIT_EXPECT_EQ(test, t->calls[1], (u32)AV14_CLOSE);
	KUNIT_EXPECT_EQ(test, t->call_epochs[1], t->call_epochs[0]);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.opened);
	KUNIT_EXPECT_FALSE(test, t->dcp->dcpavserv.opening);
	KUNIT_EXPECT_EQ(test, t->reads, 0U);
	/* The next EDID copy opens the instance again */
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_EXPECT_EQ(test, t->ncalls, 3U);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->dcpavserv.opened, &t->svc);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/* A Terminate while the Open is on its way: closed by its opener, no new use. */
static void av14_open_vs_terminate(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);

	t->during_open = AV14_TERMINATE;
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), -ENODEV);
	KUNIT_ASSERT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_EQ(test, t->calls[1], (u32)AV14_CLOSE);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.opened);
	/* No Close work was queued for it, and running one sends nothing */
	av14_run_close_work(t);
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), -ENODEV);
	KUNIT_EXPECT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_EQ(test, t->reads, 0U);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/*
 * The instance torn down and its slot reused while the Open is on its way:
 * the old open went with the old instance; nothing is closed, and the
 * successor is opened by the next copy.
 */
static void av14_open_vs_replaced(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);

	t->during_open = AV14_REPLACED;
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), -ENODEV);
	KUNIT_EXPECT_EQ(test, t->ncalls, 1U);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.opened);
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_ASSERT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_EQ(test, t->calls[1], (u32)AV14_OPEN);
	KUNIT_EXPECT_NE(test, t->call_epochs[1], t->call_epochs[0]);
	KUNIT_EXPECT_EQ(test, t->dcp->dcpavserv.opened_epoch, t->dcp->dcpavserv.epoch);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/*
 * An old instance's Close work, queued at its Terminate, runs after its
 * successor took the same slot and was opened: the successor stays open.
 */
static void av14_old_close_work_vs_successor(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);

	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	av14_terminate(t);
	av14_replace(t);
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_ASSERT_EQ(test, t->ncalls, 2U);
	av14_run_close_work(t);
	KUNIT_EXPECT_EQ(test, t->ncalls, 2U);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->dcpavserv.opened, &t->svc);
	KUNIT_EXPECT_EQ(test, t->dcp->dcpavserv.opened_epoch, t->dcp->dcpavserv.epoch);
	/* A Terminate of the successor closes the successor */
	av14_terminate(t);
	av14_run_close_work(t);
	KUNIT_ASSERT_EQ(test, t->ncalls, 3U);
	KUNIT_EXPECT_EQ(test, t->calls[2], (u32)AV14_CLOSE);
	KUNIT_EXPECT_EQ(test, t->call_epochs[2], t->dcp->dcpavserv.epoch);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/* Another EDID copy while the Open is on its way does not open the instance again. */
static void av14_open_vs_copy(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);

	t->during_open = AV14_COPY;
	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	KUNIT_EXPECT_EQ(test, t->ncalls, 1U);
	KUNIT_EXPECT_EQ(test, t->reads, 2U);
	KUNIT_EXPECT_PTR_EQ(test, t->dcp->dcpavserv.opened, &t->svc);
	KUNIT_EXPECT_FALSE(test, t->dcp->dcpavserv.opening);
	KUNIT_EXPECT_EQ(test, t->svc.users, 1U);
}

/* Detach drops the open and waits for the Close work; the slot's pin goes. */
static void av14_detach(struct kunit *test)
{
	struct av14_test *t = av14_setup(test, true);

	KUNIT_EXPECT_EQ(test, av14_copy(test, t), 0);
	av14_terminate(t);
	dpavservep_detach(t->dcp);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.opened);
	KUNIT_EXPECT_NULL(test, t->dcp->dcpavserv.service);
	/* The Close work for the detached instance sends nothing */
	av14_run_close_work(t);
	KUNIT_EXPECT_EQ(test, t->ncalls, 1U);
	KUNIT_EXPECT_EQ(test, t->svc.users, 0U);
	KUNIT_EXPECT_TRUE(test, t->svc.torndown);
}

static struct kunit_case av14_cases[] = {
	KUNIT_CASE(av14_open_once),
	KUNIT_CASE(av14_open_refused),
	KUNIT_CASE(av14_other_role),
	KUNIT_CASE(av14_edid_checked),
	KUNIT_CASE(av14_close_opened_only),
	KUNIT_CASE(av14_terminate_opened),
	KUNIT_CASE(av14_terminate_then_close),
	KUNIT_CASE(av14_open_vs_close),
	KUNIT_CASE(av14_open_vs_terminate),
	KUNIT_CASE(av14_open_vs_replaced),
	KUNIT_CASE(av14_old_close_work_vs_successor),
	KUNIT_CASE(av14_open_vs_copy),
	KUNIT_CASE(av14_detach),
	{}
};

static struct kunit_suite av14_suite = {
	.name = "apple-dcpavserv-v14",
	.test_cases = av14_cases,
};

kunit_test_suite(av14_suite);
