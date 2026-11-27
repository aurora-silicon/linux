// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by macsmc.c; the command and SRAM transports are stubbed. */
#include <kunit/device.h>
#include <kunit/test.h>

struct macsmc_test_context {
	struct apple_smc smc;
	u8 shmem[SMC_MAX_SIZE];
	u32 data;
	int result;
	unsigned int calls;
	bool require_sram_lock;
	/* What the sleep tests saw, in order: keys read, values written, notices. */
	u32 seen[8];
	unsigned int seen_count;
	int ap_power_result;
	u64 record;
	smc_key last_write_key;
	size_t last_write_size;
	u8 last_write_byte;
};

#define MACSMC_TEST_AP_IDLE	0
#define MACSMC_TEST_AP_ON	1

static int macsmc_test_command(struct apple_smc *smc, u64 cmd, u64 arg,
			       u64 size, u64 wsize, u32 *ret_data)
{
	struct macsmc_test_context *ctx = container_of(smc, struct macsmc_test_context, smc);

	KUNIT_EXPECT_TRUE(kunit_get_current_test(), mutex_is_locked(&smc->mutex));
	ctx->calls++;
	if (cmd == SMC_MSG_WRITE_KEY) {
		ctx->last_write_key = arg;
		ctx->last_write_size = size;
		ctx->last_write_byte = ctx->shmem[0];
	}
	if (ctx->seen_count < ARRAY_SIZE(ctx->seen))
		ctx->seen[ctx->seen_count++] = cmd == SMC_MSG_WRITE_KEY ?
			get_unaligned((u32 *)ctx->shmem) : arg;
	if (ret_data && ctx->result >= 0)
		*ret_data = ctx->data;
	return ctx->result;
}

static void macsmc_test_read_sram(struct apple_smc *smc, void *buf, size_t size)
{
	struct macsmc_test_context *ctx = container_of(smc, struct macsmc_test_context, smc);

	if (ctx->require_sram_lock)
		KUNIT_EXPECT_TRUE(kunit_get_current_test(), mutex_is_locked(&smc->mutex));
	memcpy(buf, ctx->shmem, size);
}

static int macsmc_test_init(struct kunit *test)
{
	struct macsmc_test_context *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	mutex_init(&ctx->smc.mutex);
	ctx->smc.shmem.iomem = (void __iomem *)ctx->shmem;
	ctx->smc.key_count = 3;
	ctx->smc.wake_reasons = apple_smc_t8140_wake_reasons;
	test->priv = ctx;
	kunit_activate_static_stub(test, apple_smc_cmd_locked, macsmc_test_command);
	kunit_activate_static_stub(test, apple_smc_read_sram, macsmc_test_read_sram);
	return 0;
}

static void macsmc_value_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	u8 byte = 0;
	u32 word = 0;

	ctx->data = 0x12345678;
	ctx->result = sizeof(word);
	KUNIT_EXPECT_EQ(test, apple_smc_read_u32(&ctx->smc, SMC_KEY(TEST), &word), 0);
	KUNIT_EXPECT_EQ(test, word, ctx->data);
	ctx->result = sizeof(byte);
	KUNIT_EXPECT_EQ(test, apple_smc_read_u8(&ctx->smc, SMC_KEY(TEST), &byte), 0);
	KUNIT_EXPECT_EQ(test, byte, 0x78);
	ctx->result = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, apple_smc_read_u8(&ctx->smc, SMC_KEY(TEST), &byte), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, byte, 0x78);
}

static void macsmc_fixed_value_short_reply_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	u32 word = 0xa5a5a5a5;
	u8 byte = 0xa5;

	ctx->data = 0x12345678;
	ctx->result = 2;
	KUNIT_EXPECT_EQ(test, apple_smc_read_u32(&ctx->smc, SMC_KEY(TEST), &word), -EIO);
	KUNIT_EXPECT_EQ(test, word, 0U);
	word = 0xa5a5a5a5;
	KUNIT_EXPECT_EQ(test,
			apple_smc_rw_u32(&ctx->smc, SMC_KEY(TEST), 0, &word), -EIO);
	KUNIT_EXPECT_EQ(test, word, 0U);
	ctx->result = 0;
	KUNIT_EXPECT_EQ(test, apple_smc_read_u8(&ctx->smc, SMC_KEY(TEST), &byte), -EIO);
	KUNIT_EXPECT_EQ(test, byte, 0);
}

static void macsmc_request_bounds_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	u8 byte;

	KUNIT_EXPECT_EQ(test, apple_smc_read(&ctx->smc, SMC_KEY(TEST), &byte,
					     SMC_MAX_SIZE + 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_smc_read(&ctx->smc, SMC_KEY(TEST), &byte, SIZE_MAX),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, apple_smc_write(&ctx->smc, SMC_KEY(TEST), &byte, SIZE_MAX),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, apple_smc_read(&ctx->smc, SMC_KEY(TEST), &byte, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0U);
}

static void macsmc_sram_value_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	u8 value[SMC_MAX_SIZE + 2];
	unsigned int i;

	for (i = 0; i < sizeof(ctx->shmem); i++)
		ctx->shmem[i] = i;
	memset(value, 0xa5, sizeof(value));
	ctx->result = SMC_MAX_SIZE;
	KUNIT_EXPECT_EQ(test, apple_smc_read(&ctx->smc, SMC_KEY(TEST), value + 1,
					     SMC_MAX_SIZE), SMC_MAX_SIZE);
	KUNIT_EXPECT_MEMEQ(test, value + 1, ctx->shmem, SMC_MAX_SIZE);
	KUNIT_EXPECT_EQ(test, value[0], 0xa5);
	KUNIT_EXPECT_EQ(test, value[SMC_MAX_SIZE + 1], 0xa5);
}

static void macsmc_key_info_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	struct apple_smc_key_info info = {};
	const u8 wire[] = { 255, 'u', 'i', '3', '2', APPLE_SMC_READABLE };

	memcpy(ctx->shmem, wire, sizeof(wire));
	KUNIT_EXPECT_EQ(test, apple_smc_get_key_info(&ctx->smc, SMC_KEY(TEST), &info), 0);
	KUNIT_EXPECT_EQ(test, info.size, 255);
	KUNIT_EXPECT_EQ(test, info.type_code, SMC_KEY(ui32));
	KUNIT_EXPECT_EQ(test, info.flags, APPLE_SMC_READABLE);
	ctx->result = -EIO;
	memset(ctx->shmem, 0, sizeof(wire));
	KUNIT_EXPECT_EQ(test, apple_smc_get_key_info(&ctx->smc, SMC_KEY(TEST), &info), -EIO);
	KUNIT_EXPECT_EQ(test, info.size, 255);
}

static void macsmc_short_value_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	u8 value[4];

	memset(value, 0xa5, sizeof(value));
	ctx->data = 0x12345678;
	ctx->result = 0;
	KUNIT_EXPECT_EQ(test, apple_smc_read(&ctx->smc, SMC_KEY(TEST), value, 4), 0);
	KUNIT_EXPECT_MEMEQ(test, value, ((u8[]){ 0xa5, 0xa5, 0xa5, 0xa5 }), 4);
	ctx->result = 2;
	KUNIT_EXPECT_EQ(test, apple_smc_read(&ctx->smc, SMC_KEY(TEST), value, 4), 2);
	KUNIT_EXPECT_MEMEQ(test, value, ((u8[]){ 0x78, 0x56, 0xa5, 0xa5 }), 4);
	memset(value, 0xa5, sizeof(value));
	ctx->result = 5;
	KUNIT_EXPECT_EQ(test, apple_smc_read(&ctx->smc, SMC_KEY(TEST), value, 4), -EIO);
	KUNIT_EXPECT_MEMEQ(test, value, ((u8[]){ 0xa5, 0xa5, 0xa5, 0xa5 }), 4);
	ctx->result = 7;
	memset(ctx->shmem, 0x3c, sizeof(ctx->shmem));
	memset(ctx->shmem + 16, 0xa5, 8);
	KUNIT_EXPECT_EQ(test, apple_smc_read(&ctx->smc, SMC_KEY(TEST), ctx->shmem + 16, 8), 7);
	KUNIT_EXPECT_EQ(test, ctx->shmem[23], 0xa5);
}

static void macsmc_key_error_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	smc_key key = SMC_KEY(TEST);

	ctx->result = -EIO;
	KUNIT_EXPECT_EQ(test, apple_smc_get_key_by_index(&ctx->smc, 0, &key), -EIO);
	KUNIT_EXPECT_EQ(test, key, SMC_KEY(TEST));
	ctx->result = 0;
	ctx->data = swab32(SMC_KEY(TEST));
	KUNIT_EXPECT_EQ(test, apple_smc_get_key_by_index(&ctx->smc, 2, &key), 0);
	KUNIT_EXPECT_EQ(test, key, SMC_KEY(TEST));
}

static void macsmc_key_index_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	smc_key key = SMC_KEY(TEST);
	int index = -1;

	KUNIT_EXPECT_EQ(test,
			apple_smc_get_key_by_index(&ctx->smc, READ_ONCE(index), &key),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, apple_smc_get_key_by_index(&ctx->smc, 3, &key), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_smc_get_key_by_index(&ctx->smc, INT_MAX, &key), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0U);
	KUNIT_EXPECT_EQ(test, key, SMC_KEY(TEST));
}

static void macsmc_key_info_lock_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	struct apple_smc_key_info info;

	ctx->require_sram_lock = true;
	KUNIT_EXPECT_EQ(test, apple_smc_get_key_info(&ctx->smc, SMC_KEY(TEST), &info), 0);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&ctx->smc.mutex));
	KUNIT_EXPECT_EQ(test, apple_smc_get_key_info(&ctx->smc, SMC_KEY(TEST), NULL), 0);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&ctx->smc.mutex));
}

static int macsmc_test_ap_power(struct apple_smc *smc, bool on)
{
	struct macsmc_test_context *ctx = container_of(smc, struct macsmc_test_context, smc);

	/* The answer needs a worker that may be waiting for this mutex. */
	KUNIT_EXPECT_FALSE(kunit_get_current_test(), mutex_is_locked(&smc->mutex));
	if (ctx->seen_count < ARRAY_SIZE(ctx->seen))
		ctx->seen[ctx->seen_count++] = on ? MACSMC_TEST_AP_ON : MACSMC_TEST_AP_IDLE;
	return ctx->ap_power_result;
}

static void macsmc_sleep_order_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;

	kunit_activate_static_stub(test, apple_smc_ap_power, macsmc_test_ap_power);
	ctx->result = 8;

	apple_smc_sleep_warn(&ctx->smc);
	KUNIT_EXPECT_EQ(test, apple_smc_sleep_commit(&ctx->smc), 0);
	KUNIT_EXPECT_TRUE(test, ctx->smc.ap_notified);
	apple_smc_sleep_wake(&ctx->smc, false);
	KUNIT_EXPECT_FALSE(test, ctx->smc.ap_notified);
	KUNIT_EXPECT_FALSE(test, ctx->smc.sleep_warned);

	/* As in the trace, values included, then the lid's read that every wake makes. */
	KUNIT_ASSERT_EQ(test, ctx->seen_count, 8U);
	KUNIT_EXPECT_EQ(test, ctx->seen[0], SMC_KEY(slpw));
	KUNIT_EXPECT_EQ(test, ctx->seen[1], SMC_KEY(slpa));
	KUNIT_EXPECT_EQ(test, ctx->seen[2], (u32)MACSMC_TEST_AP_IDLE);
	KUNIT_EXPECT_EQ(test, ctx->seen[3], (u32)MACSMC_TEST_AP_ON);
	KUNIT_EXPECT_EQ(test, ctx->seen[4], SMC_KEY(MBSE));
	KUNIT_EXPECT_EQ(test, ctx->seen[5], SMC_KEY(MBSW));
	KUNIT_EXPECT_EQ(test, ctx->seen[6], SMC_KEY(waka));
	KUNIT_EXPECT_EQ(test, ctx->seen[7], SMC_KEY(MSLD));
}

static void macsmc_test_outbox_poll(struct apple_smc *smc, const char *after,
				    unsigned int min_ms, unsigned int quiet_ms)
{
}

static void macsmc_sleep_warn_late_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;

	kunit_activate_static_stub(test, apple_smc_ap_power, macsmc_test_ap_power);
	kunit_activate_static_stub(test, apple_smc_outbox_poll, macsmc_test_outbox_poll);
	ctx->result = 8;

	/* A sleep that cuts the debug link: slpw comes at the noirq suspend, before slpa. */
	ctx->smc.sleep_warn_late = true;
	KUNIT_EXPECT_EQ(test, apple_smc_sleep_enter(&ctx->smc), 0);
	KUNIT_EXPECT_FALSE(test, ctx->smc.sleep_warn_late);
	KUNIT_EXPECT_TRUE(test, ctx->smc.sleep_warned);
	KUNIT_ASSERT_EQ(test, ctx->seen_count, 3U);
	KUNIT_EXPECT_EQ(test, ctx->seen[0], SMC_KEY(slpw));
	KUNIT_EXPECT_EQ(test, ctx->seen[1], SMC_KEY(slpa));
	KUNIT_EXPECT_EQ(test, ctx->seen[2], (u32)MACSMC_TEST_AP_IDLE);
}

static void macsmc_sleep_refused_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;

	kunit_activate_static_stub(test, apple_smc_ap_power, macsmc_test_ap_power);
	ctx->ap_power_result = -ETIME;
	ctx->result = 4;

	/* No answer: refuse, undo the notice and leave commands working. */
	KUNIT_EXPECT_EQ(test, apple_smc_sleep_commit(&ctx->smc), -ETIME);
	KUNIT_EXPECT_FALSE(test, ctx->smc.ap_notified);
	KUNIT_ASSERT_EQ(test, ctx->seen_count, 3U);
	KUNIT_EXPECT_EQ(test, ctx->seen[0], SMC_KEY(slpa));
	KUNIT_EXPECT_EQ(test, ctx->seen[1], (u32)MACSMC_TEST_AP_IDLE);
	KUNIT_EXPECT_EQ(test, ctx->seen[2], (u32)MACSMC_TEST_AP_ON);
}

static void macsmc_notice_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	bool mbsw;

	kunit_activate_static_stub(test, apple_smc_ap_power, macsmc_test_ap_power);
	ctx->result = 8;

	/* The test trigger writes no MBSE value: those announce a real sleep. */
	KUNIT_EXPECT_EQ(test, apple_smc_notice_idle(&ctx->smc), 0);
	apple_smc_notice_on(&ctx->smc, &mbsw, false);
	KUNIT_ASSERT_EQ(test, ctx->seen_count, 4U);
	KUNIT_EXPECT_EQ(test, ctx->seen[0], (u32)MACSMC_TEST_AP_IDLE);
	KUNIT_EXPECT_EQ(test, ctx->seen[1], (u32)MACSMC_TEST_AP_ON);
	KUNIT_EXPECT_EQ(test, ctx->seen[2], SMC_KEY(MBSE));
	KUNIT_EXPECT_EQ(test, ctx->seen[3], SMC_KEY(MBSW));
}

/* MBSW reads ctx->record, MSLD reads closed. */
static int macsmc_test_lid_read(struct apple_smc *smc, smc_key key, void *buf, size_t size)
{
	struct macsmc_test_context *ctx = container_of(smc, struct macsmc_test_context, smc);

	if (ctx->seen_count < ARRAY_SIZE(ctx->seen))
		ctx->seen[ctx->seen_count++] = key;
	memset(buf, 0, size);
	if (key == SMC_KEY(MBSW) && size == sizeof(ctx->record))
		put_unaligned_le64(ctx->record, buf);
	else if (key == SMC_KEY(MSLD))
		*(u8 *)buf = 1;
	return size;
}

static void macsmc_lid_wake_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;

	kunit_activate_static_stub(test, apple_smc_ap_power, macsmc_test_ap_power);
	kunit_activate_static_stub(test, apple_smc_read, macsmc_test_lid_read);
	ctx->result = 4;

	/* The lid alone: MSLD is read last, after macOS's own order. */
	ctx->record = BIT_ULL(10);
	apple_smc_sleep_wake(&ctx->smc, false);
	KUNIT_ASSERT_EQ(test, ctx->seen_count, 5U);
	KUNIT_EXPECT_EQ(test, ctx->seen[0], (u32)MACSMC_TEST_AP_ON);
	KUNIT_EXPECT_EQ(test, ctx->seen[1], SMC_KEY(MBSE));
	KUNIT_EXPECT_EQ(test, ctx->seen[2], SMC_KEY(MBSW));
	KUNIT_EXPECT_EQ(test, ctx->seen[3], SMC_KEY(waka));
	KUNIT_EXPECT_EQ(test, ctx->seen[4], SMC_KEY(MSLD));

	/* The lid with anything else is a normal wake: the lid is still read, for the display. */
	ctx->seen_count = 0;
	ctx->record = BIT_ULL(10) | BIT_ULL(0);
	apple_smc_sleep_wake(&ctx->smc, false);
	KUNIT_ASSERT_EQ(test, ctx->seen_count, 5U);
	KUNIT_EXPECT_EQ(test, ctx->seen[4], SMC_KEY(MSLD));
	if (IS_ENABLED(CONFIG_APPLE_SOC_SLEEP))
		KUNIT_EXPECT_EQ(test, apple_sleep_lid_closed_at_wake(), 1);
}

static void macsmc_wake_reason_test(struct kunit *test)
{
	static const struct apple_smc_wake_reason j714s[] = {
		{ 8, "lid" }, { 9, "sdconn" }, { 13, "wifibt" }, { }
	};
	struct macsmc_test_context *ctx = test->priv;
	char names[128];

	KUNIT_EXPECT_EQ(test, apple_smc_wake_reason_names(&ctx->smc, 0, names, sizeof(names)), 0);
	KUNIT_EXPECT_STREQ(test, names, "");

	/* A bit with no name is left out. */
	apple_smc_wake_reason_names(&ctx->smc, BIT_ULL(23) | BIT_ULL(42) | BIT_ULL(1), names,
				    sizeof(names));
	KUNIT_EXPECT_STREQ(test, names, "wifibt USB-C_plug");

	/* T8132's own numbering: its lid is bit 8, and bit 10 is nothing. */
	ctx->smc.wake_reasons = apple_smc_t8132_wake_reasons;
	apple_smc_wake_reason_names(&ctx->smc, BIT_ULL(8) | BIT_ULL(10) | BIT_ULL(13), names,
				    sizeof(names));
	KUNIT_EXPECT_STREQ(test, names, "lid wifibt");
	KUNIT_EXPECT_EQ(test, apple_smc_wake_reason_bit(&ctx->smc, "lid"), 8);
	KUNIT_EXPECT_EQ(test, apple_smc_wake_reason_bit(&ctx->smc, "ipd_touch"), -1);

	/* A device tree's own, here the J714s's: its SD reader at bit 9. */
	ctx->smc.wake_reasons = j714s;
	apple_smc_wake_reason_names(&ctx->smc, BIT_ULL(8) | BIT_ULL(9) | BIT_ULL(10) | BIT_ULL(13),
				    names, sizeof(names));
	KUNIT_EXPECT_STREQ(test, names, "lid sdconn wifibt");
	KUNIT_EXPECT_EQ(test, apple_smc_wake_reason_bit(&ctx->smc, "lid"), 8);
	KUNIT_EXPECT_EQ(test, apple_smc_wake_reason_bit(&ctx->smc, "wifibt"), 13);
}

static void macsmc_port_reasons_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;

	/* The debug link's cut: USB-C_plug, acdetach, or both. */
	KUNIT_EXPECT_TRUE(test, apple_smc_port_reasons_only(&ctx->smc, BIT_ULL(42)));
	KUNIT_EXPECT_TRUE(test, apple_smc_port_reasons_only(&ctx->smc, BIT_ULL(41) | BIT_ULL(42)));
	/* Anything else, with it or alone, is a real wake. */
	KUNIT_EXPECT_FALSE(test, apple_smc_port_reasons_only(&ctx->smc, 0));
	KUNIT_EXPECT_FALSE(test, apple_smc_port_reasons_only(&ctx->smc, BIT_ULL(42) | BIT_ULL(0)));
	KUNIT_EXPECT_FALSE(test, apple_smc_port_reasons_only(&ctx->smc, BIT_ULL(42) | BIT_ULL(10)));
	KUNIT_EXPECT_FALSE(test, apple_smc_port_reasons_only(&ctx->smc, BIT_ULL(23)));
	KUNIT_EXPECT_FALSE(test, apple_smc_port_reasons_only(&ctx->smc, BIT_ULL(43)));
}

/*
 * The SMC's first message may come as soon as its endpoint is started: the
 * state the probe sets up before the RTKit is enough to take it.
 */
static void macsmc_early_message_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	struct resource sram = DEFINE_RES_MEM(0x10000, SMC_SHMEM_SIZE);
	struct apple_smc *smc;

	smc = kunit_kzalloc(test, sizeof(*smc), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, smc);
	apple_smc_init_state(smc);
	smc->sram = &sram;
	smc->sram_base = (void __iomem *)ctx->shmem;

	KUNIT_EXPECT_EQ(test, smc->boot_stage, APPLE_SMC_BOOTING);
	KUNIT_EXPECT_FALSE(test, completion_done(&smc->init_done));

	KUNIT_EXPECT_TRUE(test, apple_smc_rtkit_recv_early(smc, SMC_ENDPOINT, sram.start));
	KUNIT_EXPECT_EQ(test, smc->boot_stage, APPLE_SMC_INITIALIZED);
	KUNIT_EXPECT_TRUE(test, completion_done(&smc->init_done));
	KUNIT_EXPECT_PTR_EQ(test, smc->shmem.iomem, smc->sram_base);
}

static struct device *macsmc_test_attach_dev(struct kunit *test,
					     struct macsmc_test_context *ctx)
{
	struct device *dev = kunit_device_register(test, "macsmc-test");

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	dev_set_drvdata(dev, &ctx->smc);
	ctx->smc.dev = dev;
	return dev;
}

static void macsmc_wktp_probe_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	const u8 ro_wire[] = { 1, 'u', 'i', '8', ' ', APPLE_SMC_READABLE };
	const u8 wide_wire[] = { 2, 'u', 'i', '1', '6',
				 APPLE_SMC_READABLE | APPLE_SMC_WRITABLE };
	const u8 valid_wire[] = { 1, 'u', 'i', '8', ' ',
				  APPLE_SMC_READABLE | APPLE_SMC_WRITABLE |
				  APPLE_SMC_FUNCTION };

	macsmc_test_attach_dev(test, ctx);

	/* Absent WKTP leaves has_wktp clear and issues no write. */
	ctx->result = -EIO;
	apple_smc_probe_wktp(&ctx->smc);
	KUNIT_EXPECT_FALSE(test, ctx->smc.has_wktp);
	KUNIT_EXPECT_EQ(test, ctx->calls, 1U);

	/* Read-only or wrong-sized WKTP is ignored. */
	ctx->result = 0;
	ctx->calls = 0;
	memcpy(ctx->shmem, ro_wire, sizeof(ro_wire));
	apple_smc_probe_wktp(&ctx->smc);
	KUNIT_EXPECT_FALSE(test, ctx->smc.has_wktp);
	KUNIT_EXPECT_EQ(test, ctx->calls, 1U);

	ctx->calls = 0;
	memcpy(ctx->shmem, wide_wire, sizeof(wide_wire));
	apple_smc_probe_wktp(&ctx->smc);
	KUNIT_EXPECT_FALSE(test, ctx->smc.has_wktp);
	KUNIT_EXPECT_EQ(test, ctx->calls, 1U);

	/* Valid 1-byte writable WKTP enables tracking and writes full wake (0). */
	ctx->calls = 0;
	memcpy(ctx->shmem, valid_wire, sizeof(valid_wire));
	apple_smc_probe_wktp(&ctx->smc);
	KUNIT_EXPECT_TRUE(test, ctx->smc.has_wktp);
	KUNIT_EXPECT_EQ(test, ctx->calls, 2U);
	KUNIT_EXPECT_EQ(test, ctx->last_write_key, SMC_KEY(WKTP));
	KUNIT_EXPECT_EQ(test, ctx->last_write_size, sizeof(u8));
	KUNIT_EXPECT_EQ(test, ctx->last_write_byte, (u8)SMC_WAKE_TYPE_FULL);
}

static void macsmc_wktp_full_wake_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	struct device *dev = macsmc_test_attach_dev(test, ctx);

	pm_suspend_clear_flags();

	/* Without WKTP capability, complete() issues no write. */
	apple_smc_complete(dev);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0U);

	/* Full wake writes WKTP = 0 to exit Silent Running / Power Nap. */
	ctx->smc.has_wktp = true;
	ctx->shmem[0] = 0xa5;
	apple_smc_complete(dev);
	KUNIT_EXPECT_EQ(test, ctx->calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->last_write_key, SMC_KEY(WKTP));
	KUNIT_EXPECT_EQ(test, ctx->last_write_size, sizeof(u8));
	KUNIT_EXPECT_EQ(test, ctx->last_write_byte, (u8)SMC_WAKE_TYPE_FULL);
}

static void macsmc_wktp_dark_wake_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	struct device *dev = macsmc_test_attach_dev(test, ctx);

	if (!IS_ENABLED(CONFIG_PM_SLEEP))
		kunit_skip(test, "CONFIG_PM_SLEEP disabled");

	ctx->smc.has_wktp = true;
	pm_suspend_clear_flags();
	pm_set_dark_wake();
	apple_smc_complete(dev);
	pm_suspend_clear_flags();

	KUNIT_EXPECT_EQ(test, ctx->calls, 0U);
}

static void macsmc_wktp_quarantined_test(struct kunit *test)
{
	struct macsmc_test_context *ctx = test->priv;
	struct device *dev = macsmc_test_attach_dev(test, ctx);

	pm_suspend_clear_flags();
	ctx->smc.has_wktp = true;
	ctx->smc.saved_enabled = true;
	ctx->smc.saved_recovery = true;

	apple_smc_complete(dev);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0U);

	ctx->smc.saved_recovery = false;
	ctx->smc.saved_fault = -EIO;
	apple_smc_complete(dev);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0U);
}

static struct kunit_case macsmc_parser_cases[] = {
	KUNIT_CASE(macsmc_key_info_lock_test),
	KUNIT_CASE(macsmc_key_index_test),
	KUNIT_CASE(macsmc_key_error_test),
	KUNIT_CASE(macsmc_short_value_test),
	KUNIT_CASE(macsmc_fixed_value_short_reply_test),
	KUNIT_CASE(macsmc_value_test),
	KUNIT_CASE(macsmc_request_bounds_test),
	KUNIT_CASE(macsmc_sram_value_test),
	KUNIT_CASE(macsmc_key_info_test),
	KUNIT_CASE(macsmc_sleep_order_test),
	KUNIT_CASE(macsmc_sleep_warn_late_test),
	KUNIT_CASE(macsmc_sleep_refused_test),
	KUNIT_CASE(macsmc_notice_test),
	KUNIT_CASE(macsmc_lid_wake_test),
	KUNIT_CASE(macsmc_wake_reason_test),
	KUNIT_CASE(macsmc_port_reasons_test),
	KUNIT_CASE(macsmc_early_message_test),
	KUNIT_CASE(macsmc_wktp_probe_test),
	KUNIT_CASE(macsmc_wktp_full_wake_test),
	KUNIT_CASE(macsmc_wktp_dark_wake_test),
	KUNIT_CASE(macsmc_wktp_quarantined_test),
	{}
};

static struct kunit_suite macsmc_parser_suite = {
	.name = "macsmc-parser",
	.init = macsmc_test_init,
	.test_cases = macsmc_parser_cases,
};

kunit_test_suite(macsmc_parser_suite);
