// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple SMC RTC driver: the 48-bit tick and 64-bit
 * seconds offset arithmetic and the PMU alarm programming.
 *
 * The CLKM counter comes from a static stub on apple_smc_read() (see
 * MFD_MACSMC_KUNIT_TEST). The offset and alarm cells are real NVMEM cells of
 * a RAM-backed NVMEM device, found through a cell lookup table, so the cell
 * sizes and byte layouts are checked by the NVMEM core as on hardware.
 *
 * Included by rtc-macsmc.c.
 */

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/nvmem-provider.h>

#define RTC_TEST_OFFSET		0
#define RTC_TEST_CTRL		8
#define RTC_TEST_MASK		9
#define RTC_TEST_ALARM		12
#define RTC_TEST_NVMEM_SIZE	16

#define RTC_TEST_TICKS		BIT_ULL(RTC_SEC_SHIFT)
#define RTC_TEST_MASK48		GENMASK_ULL(RTC_BITS - 1, 0)

/* 2023-11-14 22:13:20 UTC */
#define RTC_TEST_T		1700000000LL

struct macsmc_rtc_test_write {
	unsigned int offset;
	size_t bytes;
	u8 val[8];
};

struct macsmc_rtc_test {
	struct apple_smc smc;
	struct macsmc_rtc rtc;
	struct device *dev;
	u8 nvmem[RTC_TEST_NVMEM_SIZE];
	/* CLKM: the free-running 32768 Hz counter, and what its read returns */
	u64 clkm;
	int clkm_ret;
	/* NVMEM writes, in order; a write at fail_offset fails */
	struct macsmc_rtc_test_write writes[8];
	unsigned int nr_writes;
	int fail_offset;
	struct nvmem_cell_lookup lookups[4];
};

static int macsmc_rtc_test_read(struct apple_smc *smc, smc_key key, void *buf,
				size_t size)
{
	struct macsmc_rtc_test *ctx = container_of(smc, struct macsmc_rtc_test, smc);
	struct kunit *test = kunit_get_current_test();
	u8 le[8];

	KUNIT_EXPECT_EQ(test, key, SMC_KEY(CLKM));
	KUNIT_EXPECT_EQ(test, size, RTC_BYTES);
	if (ctx->clkm_ret < 0)
		return ctx->clkm_ret;

	put_unaligned_le64(ctx->clkm, le);
	memcpy(buf, le, min(size, sizeof(le)));
	return ctx->clkm_ret ?: size;
}

static int macsmc_rtc_test_reg_read(void *priv, unsigned int offset, void *val,
				    size_t bytes)
{
	struct macsmc_rtc_test *ctx = priv;

	memcpy(val, ctx->nvmem + offset, bytes);
	return 0;
}

static int macsmc_rtc_test_reg_write(void *priv, unsigned int offset, void *val,
				     size_t bytes)
{
	struct macsmc_rtc_test *ctx = priv;
	struct macsmc_rtc_test_write *w;

	if (offset == ctx->fail_offset)
		return -EIO;

	if (ctx->nr_writes < ARRAY_SIZE(ctx->writes)) {
		w = &ctx->writes[ctx->nr_writes];
		w->offset = offset;
		w->bytes = bytes;
		memcpy(w->val, val, min(bytes, sizeof(w->val)));
	}
	ctx->nr_writes++;
	memcpy(ctx->nvmem + offset, val, bytes);
	return 0;
}

static void macsmc_rtc_test_del_lookups(void *data)
{
	struct macsmc_rtc_test *ctx = data;

	nvmem_del_cell_lookups(ctx->lookups, ARRAY_SIZE(ctx->lookups));
}

KUNIT_DEFINE_ACTION_WRAPPER(macsmc_rtc_test_cell_put, nvmem_cell_put,
			    struct nvmem_cell *);

static struct nvmem_cell *macsmc_rtc_test_cell(struct kunit *test,
					       struct macsmc_rtc_test *ctx,
					       const char *id)
{
	struct nvmem_cell *cell = nvmem_cell_get(ctx->dev, id);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cell);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, macsmc_rtc_test_cell_put,
							cell), 0);
	return cell;
}

/*
 * An RTC whose offset cell holds @offset_bytes bytes: 6 for a tick offset,
 * 8 for a seconds offset.
 */
static struct macsmc_rtc_test *macsmc_rtc_test_setup(struct kunit *test,
						     bool offset_in_seconds,
						     unsigned int offset_bytes)
{
	static const char * const names[] = {
		"rtc_offset", "rtc_ctrl", "rtc_alarm_mask", "rtc_alarm",
	};
	const unsigned int offsets[] = {
		RTC_TEST_OFFSET, RTC_TEST_CTRL, RTC_TEST_MASK, RTC_TEST_ALARM,
	};
	const unsigned int sizes[] = { offset_bytes, 1, 1, 4 };
	struct nvmem_config config = {};
	struct nvmem_cell_info *cells;
	struct macsmc_rtc_test *ctx;
	struct nvmem_device *nvmem;
	struct device *provider;
	int i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	cells = kunit_kcalloc(test, ARRAY_SIZE(names), sizeof(*cells), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, cells);
	ctx->fail_offset = -1;
	mutex_init(&ctx->smc.mutex);

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		cells[i].name = names[i];
		cells[i].offset = offsets[i];
		cells[i].bytes = sizes[i];
	}

	provider = kunit_device_register(test, "nvmem");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, provider);
	config.dev = provider;
	config.name = "macsmc-rtc-test";
	config.id = NVMEM_DEVID_AUTO;
	config.cells = cells;
	config.ncells = ARRAY_SIZE(names);
	config.ignore_wp = true;
	config.reg_read = macsmc_rtc_test_reg_read;
	config.reg_write = macsmc_rtc_test_reg_write;
	config.size = RTC_TEST_NVMEM_SIZE;
	config.priv = ctx;
	nvmem = devm_nvmem_register(provider, &config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, nvmem);

	ctx->dev = kunit_device_register(test, "rtc");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->dev);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ctx->lookups[i].nvmem_name = nvmem_dev_name(nvmem);
		ctx->lookups[i].cell_name = names[i];
		ctx->lookups[i].dev_id = dev_name(ctx->dev);
		ctx->lookups[i].con_id = names[i];
	}
	nvmem_add_cell_lookups(ctx->lookups, ARRAY_SIZE(ctx->lookups));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, macsmc_rtc_test_del_lookups,
							ctx), 0);

	ctx->rtc.dev = ctx->dev;
	ctx->rtc.smc = &ctx->smc;
	ctx->rtc.offset_in_seconds = offset_in_seconds;
	mutex_init(&ctx->rtc.alarm_lock);
	ctx->rtc.rtc_offset = macsmc_rtc_test_cell(test, ctx, "rtc_offset");
	ctx->rtc.alarm_ctrl = macsmc_rtc_test_cell(test, ctx, "rtc_ctrl");
	ctx->rtc.alarm_mask = macsmc_rtc_test_cell(test, ctx, "rtc_alarm_mask");
	ctx->rtc.alarm_value = macsmc_rtc_test_cell(test, ctx, "rtc_alarm");
	dev_set_drvdata(ctx->dev, &ctx->rtc);

	kunit_activate_static_stub(test, apple_smc_read, macsmc_rtc_test_read);
	return ctx;
}

static void macsmc_rtc_test_put_tick_offset(struct macsmc_rtc_test *ctx, u64 off)
{
	u8 le[8];

	put_unaligned_le64(off & RTC_TEST_MASK48, le);
	memcpy(ctx->nvmem + RTC_TEST_OFFSET, le, RTC_BYTES);
}

static u64 macsmc_rtc_test_tick_offset(struct macsmc_rtc_test *ctx)
{
	u8 le[8] = {};

	memcpy(le, ctx->nvmem + RTC_TEST_OFFSET, RTC_BYTES);
	return get_unaligned_le64(le);
}

static time64_t macsmc_rtc_test_now(struct kunit *test, struct macsmc_rtc_test *ctx)
{
	struct rtc_time tm = {};
	int ret;

	ret = macsmc_rtc_get_time(ctx->dev, &tm);
	KUNIT_EXPECT_GE(test, ret, 0);
	if (ret < 0)
		return S64_MIN;
	return rtc_tm_to_time64(&tm);
}

static int macsmc_rtc_test_set(struct macsmc_rtc_test *ctx, time64_t t)
{
	struct rtc_time tm;

	rtc_time64_to_tm(t, &tm);
	return macsmc_rtc_set_time(ctx->dev, &tm);
}

static void macsmc_rtc_test_expect_write(struct kunit *test,
					 struct macsmc_rtc_test *ctx,
					 unsigned int idx, unsigned int offset,
					 size_t bytes, u64 val)
{
	struct macsmc_rtc_test_write *w = &ctx->writes[idx];
	u8 le[8];

	KUNIT_ASSERT_LT(test, idx, ctx->nr_writes);
	put_unaligned_le64(val, le);
	KUNIT_EXPECT_EQ_MSG(test, w->offset, offset, "write %u", idx);
	KUNIT_EXPECT_EQ_MSG(test, w->bytes, bytes, "write %u", idx);
	KUNIT_EXPECT_MEMEQ_MSG(test, w->val, le, bytes, "write %u", idx);
}

/* Tick offset: time = sign_extend48(CLKM + offset) / 2^15, rounded down. */
static void macsmc_rtc_test_tick_read(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, false, RTC_BYTES);
	u64 ctr = 0x123456789abULL;

	ctx->clkm = ctr;
	macsmc_rtc_test_put_tick_offset(ctx, (RTC_TEST_T << RTC_SEC_SHIFT) + 12345 - ctr);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_now(test, ctx), RTC_TEST_T);

	/* One tick before the next second, and the second itself. */
	ctx->clkm = ctr + RTC_TEST_TICKS - 12345 - 1;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_now(test, ctx), RTC_TEST_T);
	ctx->clkm++;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_now(test, ctx), RTC_TEST_T + 1);
	KUNIT_EXPECT_EQ(test, ctx->nr_writes, 0);
}

/* The sum is taken modulo 2^48: a carry out of bit 47 is not time. */
static void macsmc_rtc_test_tick_wrap(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, false, RTC_BYTES);

	ctx->clkm = RTC_TEST_MASK48 + 1 - RTC_TEST_TICKS;
	macsmc_rtc_test_put_tick_offset(ctx, (RTC_TEST_T << RTC_SEC_SHIFT) + RTC_TEST_TICKS);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_now(test, ctx), RTC_TEST_T);
}

/* Set then read: the stored 6 bytes, and the set second starts at the set. */
static void macsmc_rtc_test_tick_set(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, false, RTC_BYTES);
	u64 ctr = 0x00ab12345678ULL;

	ctx->clkm = ctr;
	/* .set_time returns 0, not the cell write's byte count. */
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_set(ctx, RTC_TEST_T), 0);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 1);
	macsmc_rtc_test_expect_write(test, ctx, 0, RTC_TEST_OFFSET, RTC_BYTES,
				     ((RTC_TEST_T << RTC_SEC_SHIFT) - ctr) & RTC_TEST_MASK48);

	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_now(test, ctx), RTC_TEST_T);
	ctx->clkm = ctr + RTC_TEST_TICKS - 1;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_now(test, ctx), RTC_TEST_T);
	ctx->clkm = ctr + 3 * RTC_TEST_TICKS + 100;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_now(test, ctx), RTC_TEST_T + 3);
}

/* A time before 1970 is a negative 48-bit sum and must read back as such. */
static void macsmc_rtc_test_tick_negative(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, false, RTC_BYTES);
	struct rtc_time tm = {};

	ctx->clkm = 1000 * RTC_TEST_TICKS + 5;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_set(ctx, -1), 0);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_tick_offset(ctx),
			((u64)-(1LL << RTC_SEC_SHIFT) - ctx->clkm) & RTC_TEST_MASK48);

	KUNIT_EXPECT_GE(test, macsmc_rtc_get_time(ctx->dev, &tm), 0);
	KUNIT_EXPECT_EQ(test, tm.tm_year, 69);
	KUNIT_EXPECT_EQ(test, tm.tm_mon, 11);
	KUNIT_EXPECT_EQ(test, tm.tm_mday, 31);
	KUNIT_EXPECT_EQ(test, tm.tm_hour, 23);
	KUNIT_EXPECT_EQ(test, tm.tm_min, 59);
	KUNIT_EXPECT_EQ(test, tm.tm_sec, 59);
}

/* Seconds offset: time = CLKM / 2^15 + offset, offset a LE64 at the cell. */
static void macsmc_rtc_test_secs_read(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, true, 8);

	put_unaligned_le64(RTC_TEST_T, ctx->nvmem + RTC_TEST_OFFSET);
	ctx->clkm = 5 * RTC_TEST_TICKS + RTC_TEST_TICKS - 1;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_now(test, ctx), RTC_TEST_T + 5);

	/* Bits above the counter's 48 are not read: CLKM is a 6-byte key. */
	ctx->clkm = BIT_ULL(RTC_BITS) | (7 * RTC_TEST_TICKS);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_now(test, ctx), RTC_TEST_T + 7);
}

/* An offset never written is not a time: reading it must fail. */
static void macsmc_rtc_test_secs_unset(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, true, 8);
	struct rtc_time tm;

	ctx->clkm = 1234 * RTC_TEST_TICKS;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_get_time(ctx->dev, &tm), -EINVAL);
}

/* A seconds-mode offset cell shorter than 8 bytes is rejected, not half read. */
static void macsmc_rtc_test_secs_short_cell(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, true, RTC_BYTES);
	struct rtc_wkalrm alrm = { .enabled = 1 };
	struct rtc_time tm;

	memset(ctx->nvmem, 0x11, RTC_BYTES);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_get_time(ctx->dev, &tm), -EIO);
	rtc_time64_to_tm(RTC_TEST_T, &alrm.time);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_set_alarm(ctx->dev, &alrm), -EIO);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_read_alarm(ctx->dev, &alrm), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->nr_writes, 0);
}

/* The seconds offset rounds the counter to the nearest second. */
static void macsmc_rtc_test_secs_set(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, true, 8);

	ctx->clkm = 10 * RTC_TEST_TICKS + RTC_TEST_TICKS / 2 - 1;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_set(ctx, RTC_TEST_T), 0);
	macsmc_rtc_test_expect_write(test, ctx, 0, RTC_TEST_OFFSET, 8, RTC_TEST_T - 10);

	ctx->clkm = 10 * RTC_TEST_TICKS + RTC_TEST_TICKS / 2;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_set(ctx, RTC_TEST_T), 0);
	macsmc_rtc_test_expect_write(test, ctx, 1, RTC_TEST_OFFSET, 8, RTC_TEST_T - 11);
	KUNIT_EXPECT_EQ(test, ctx->nr_writes, 2);

	/* Read back half a second later: at most half a second off. */
	ctx->clkm += RTC_TEST_TICKS / 2;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_now(test, ctx), RTC_TEST_T);
}

/* CLKM failures and short replies fail the call and write nothing. */
static void macsmc_rtc_test_clkm_errors(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, false, RTC_BYTES);
	struct rtc_time tm;

	ctx->clkm_ret = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_get_time(ctx->dev, &tm), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_set(ctx, RTC_TEST_T), -ETIMEDOUT);

	ctx->clkm_ret = 4;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_get_time(ctx->dev, &tm), -EIO);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_set(ctx, RTC_TEST_T), -EIO);

	ctx->rtc.offset_in_seconds = true;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_test_set(ctx, RTC_TEST_T), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->nr_writes, 0);
}

/*
 * Arming: a latched event is cleared by writing it back, then the value
 * (counter seconds, LE32), the mask and the control byte with EN | MONITOR.
 * The PMU's other control bits are kept.
 */
static void macsmc_rtc_test_alarm_arm(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, false, RTC_BYTES);
	struct rtc_wkalrm alrm = { .enabled = 1 };
	const s64 off = 1000000;

	/* A tick offset with a fraction: whole seconds are used. */
	macsmc_rtc_test_put_tick_offset(ctx, (off << RTC_SEC_SHIFT) + 1234);
	ctx->nvmem[RTC_TEST_CTRL] = 0x80 | RTC_CTRL_ALARM_EVENT;
	ctx->nvmem[RTC_TEST_MASK] = 1;

	rtc_time64_to_tm(off + 4321, &alrm.time);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_set_alarm(ctx->dev, &alrm), 0);
	KUNIT_EXPECT_EQ(test, ctx->rtc.alarm_secs, 4321);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 4);
	macsmc_rtc_test_expect_write(test, ctx, 0, RTC_TEST_CTRL, 1,
				     0x80 | RTC_CTRL_ALARM_EVENT);
	macsmc_rtc_test_expect_write(test, ctx, 1, RTC_TEST_ALARM, 4, 4321);
	macsmc_rtc_test_expect_write(test, ctx, 2, RTC_TEST_MASK, 1, 0);
	macsmc_rtc_test_expect_write(test, ctx, 3, RTC_TEST_CTRL, 1,
				     0x80 | RTC_CTRL_ALARM_EN | RTC_CTRL_MONITOR);
}

/* A disabled alarm is remembered but disarms; enabling it arms the same second. */
static void macsmc_rtc_test_alarm_disabled(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, true, 8);
	struct rtc_wkalrm alrm = { .enabled = 0 };

	put_unaligned_le64(RTC_TEST_T, ctx->nvmem + RTC_TEST_OFFSET);
	/* Left armed by firmware, no event. */
	ctx->nvmem[RTC_TEST_CTRL] = RTC_CTRL_ALARM_EN | RTC_CTRL_MONITOR;

	rtc_time64_to_tm(RTC_TEST_T + 60, &alrm.time);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_set_alarm(ctx->dev, &alrm), 0);
	KUNIT_EXPECT_EQ(test, ctx->rtc.alarm_secs, 60);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 2);
	macsmc_rtc_test_expect_write(test, ctx, 0, RTC_TEST_MASK, 1, 1);
	macsmc_rtc_test_expect_write(test, ctx, 1, RTC_TEST_CTRL, 1, RTC_CTRL_MONITOR);

	ctx->nr_writes = 0;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_alarm_irq_enable(ctx->dev, 1), 0);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 3);
	macsmc_rtc_test_expect_write(test, ctx, 0, RTC_TEST_ALARM, 4, 60);
	macsmc_rtc_test_expect_write(test, ctx, 1, RTC_TEST_MASK, 1, 0);
	macsmc_rtc_test_expect_write(test, ctx, 2, RTC_TEST_CTRL, 1,
				     RTC_CTRL_ALARM_EN | RTC_CTRL_MONITOR);

	ctx->nr_writes = 0;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_alarm_irq_enable(ctx->dev, 0), 0);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 2);
	macsmc_rtc_test_expect_write(test, ctx, 1, RTC_TEST_CTRL, 1, RTC_CTRL_MONITOR);
}

/* The 32-bit compare holds counter seconds 1..U32_MAX; others are refused. */
static void macsmc_rtc_test_alarm_range(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, true, 8);
	struct rtc_wkalrm alrm = { .enabled = 1 };
	const time64_t off = 86400;

	put_unaligned_le64(off, ctx->nvmem + RTC_TEST_OFFSET);

	rtc_time64_to_tm(off, &alrm.time);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_set_alarm(ctx->dev, &alrm), -ERANGE);
	rtc_time64_to_tm(off - 5, &alrm.time);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_set_alarm(ctx->dev, &alrm), -ERANGE);
	rtc_time64_to_tm(off + (time64_t)U32_MAX + 1, &alrm.time);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_set_alarm(ctx->dev, &alrm), -ERANGE);
	KUNIT_EXPECT_EQ(test, ctx->nr_writes, 0);

	rtc_time64_to_tm(off + 1, &alrm.time);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_set_alarm(ctx->dev, &alrm), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(ctx->nvmem + RTC_TEST_ALARM), 1);
	rtc_time64_to_tm(off + U32_MAX, &alrm.time);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_set_alarm(ctx->dev, &alrm), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(ctx->nvmem + RTC_TEST_ALARM), U32_MAX);
}

/* A negative tick offset moves the alarm's counter second forward. */
static void macsmc_rtc_test_alarm_negative_offset(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, false, RTC_BYTES);
	struct rtc_wkalrm alrm = { .enabled = 1 };

	macsmc_rtc_test_put_tick_offset(ctx, (u64)-(5LL << RTC_SEC_SHIFT));
	rtc_time64_to_tm(100, &alrm.time);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_set_alarm(ctx->dev, &alrm), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(ctx->nvmem + RTC_TEST_ALARM), 105);

	memset(&alrm, 0, sizeof(alrm));
	KUNIT_EXPECT_EQ(test, macsmc_rtc_read_alarm(ctx->dev, &alrm), 0);
	KUNIT_EXPECT_EQ(test, rtc_tm_to_time64(&alrm.time), 100);
	KUNIT_EXPECT_TRUE(test, alrm.enabled);
	KUNIT_EXPECT_FALSE(test, alrm.pending);
}

/* Reading decodes the value against the offset and the EN/EVENT bits. */
static void macsmc_rtc_test_alarm_read(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, true, 8);
	struct rtc_wkalrm alrm = {};

	put_unaligned_le64(RTC_TEST_T, ctx->nvmem + RTC_TEST_OFFSET);
	put_unaligned_le32(777, ctx->nvmem + RTC_TEST_ALARM);
	ctx->nvmem[RTC_TEST_CTRL] = RTC_CTRL_ALARM_EN | RTC_CTRL_ALARM_EVENT;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_read_alarm(ctx->dev, &alrm), 0);
	KUNIT_EXPECT_EQ(test, rtc_tm_to_time64(&alrm.time), RTC_TEST_T + 777);
	KUNIT_EXPECT_TRUE(test, alrm.enabled);
	KUNIT_EXPECT_TRUE(test, alrm.pending);

	ctx->nvmem[RTC_TEST_CTRL] = RTC_CTRL_MONITOR;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_read_alarm(ctx->dev, &alrm), 0);
	KUNIT_EXPECT_FALSE(test, alrm.enabled);
	KUNIT_EXPECT_FALSE(test, alrm.pending);
	KUNIT_EXPECT_EQ(test, ctx->nr_writes, 0);
}

/* The interrupt is one-shot: an event clears and disarms; no event is not ours. */
static void macsmc_rtc_test_alarm_irq(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, true, 8);
	const u8 armed = RTC_CTRL_ALARM_EN | RTC_CTRL_MONITOR;

	ctx->nvmem[RTC_TEST_CTRL] = armed;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_alarm_irq(0, &ctx->rtc), IRQ_NONE);
	KUNIT_EXPECT_EQ(test, ctx->nr_writes, 0);

	ctx->nvmem[RTC_TEST_CTRL] = armed | RTC_CTRL_ALARM_EVENT;
	KUNIT_EXPECT_EQ(test, macsmc_rtc_alarm_irq(0, &ctx->rtc), IRQ_HANDLED);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 3);
	macsmc_rtc_test_expect_write(test, ctx, 0, RTC_TEST_CTRL, 1,
				     armed | RTC_CTRL_ALARM_EVENT);
	macsmc_rtc_test_expect_write(test, ctx, 1, RTC_TEST_MASK, 1, 1);
	macsmc_rtc_test_expect_write(test, ctx, 2, RTC_TEST_CTRL, 1, RTC_CTRL_MONITOR);
}

/* A failed mask write leaves the alarm unarmed and reports the error. */
static void macsmc_rtc_test_alarm_write_error(struct kunit *test)
{
	struct macsmc_rtc_test *ctx = macsmc_rtc_test_setup(test, true, 8);
	struct rtc_wkalrm alrm = { .enabled = 1 };

	put_unaligned_le64(RTC_TEST_T, ctx->nvmem + RTC_TEST_OFFSET);
	ctx->fail_offset = RTC_TEST_MASK;
	rtc_time64_to_tm(RTC_TEST_T + 10, &alrm.time);
	KUNIT_EXPECT_EQ(test, macsmc_rtc_set_alarm(ctx->dev, &alrm), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->nvmem[RTC_TEST_CTRL], 0);
	KUNIT_EXPECT_EQ(test, ctx->nr_writes, 1);
	macsmc_rtc_test_expect_write(test, ctx, 0, RTC_TEST_ALARM, 4, 10);
}

static struct kunit_case macsmc_rtc_test_cases[] = {
	KUNIT_CASE(macsmc_rtc_test_tick_read),
	KUNIT_CASE(macsmc_rtc_test_tick_wrap),
	KUNIT_CASE(macsmc_rtc_test_tick_set),
	KUNIT_CASE(macsmc_rtc_test_tick_negative),
	KUNIT_CASE(macsmc_rtc_test_secs_read),
	KUNIT_CASE(macsmc_rtc_test_secs_unset),
	KUNIT_CASE(macsmc_rtc_test_secs_short_cell),
	KUNIT_CASE(macsmc_rtc_test_secs_set),
	KUNIT_CASE(macsmc_rtc_test_clkm_errors),
	KUNIT_CASE(macsmc_rtc_test_alarm_arm),
	KUNIT_CASE(macsmc_rtc_test_alarm_disabled),
	KUNIT_CASE(macsmc_rtc_test_alarm_range),
	KUNIT_CASE(macsmc_rtc_test_alarm_negative_offset),
	KUNIT_CASE(macsmc_rtc_test_alarm_read),
	KUNIT_CASE(macsmc_rtc_test_alarm_irq),
	KUNIT_CASE(macsmc_rtc_test_alarm_write_error),
	{}
};

static struct kunit_suite macsmc_rtc_test_suite = {
	.name = "macsmc-rtc",
	.test_cases = macsmc_rtc_test_cases,
};

kunit_test_suite(macsmc_rtc_test_suite);
