// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple SMC reboot driver: the MBSE key and NVMEM flag
 * sequence before a restart or power off, the boot error and panic counters,
 * and the ac_power_mode attribute over the PMU's power-on policy byte.
 *
 * SMC writes go to a static stub on apple_smc_write() (see
 * MFD_MACSMC_KUNIT_TEST). The PMU cells are real NVMEM cells of a RAM-backed
 * NVMEM device, found through a cell lookup table.
 *
 * Included by macsmc-reboot.c.
 */

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/ktime.h>
#include <linux/nvmem-provider.h>

enum {
	REBOOT_TEST_SHUTDOWN_FLAG,
	REBOOT_TEST_BOOT_STAGE,
	REBOOT_TEST_BOOT_ERROR_COUNT,
	REBOOT_TEST_PANIC_COUNT,
	REBOOT_TEST_PM_SETTING,
	REBOOT_TEST_NR_CELLS,
};

static const char * const macsmc_reboot_test_cells[] = {
	[REBOOT_TEST_SHUTDOWN_FLAG]	= "shutdown_flag",
	[REBOOT_TEST_BOOT_STAGE]	= "boot_stage",
	[REBOOT_TEST_BOOT_ERROR_COUNT]	= "boot_error_count",
	[REBOOT_TEST_PANIC_COUNT]	= "panic_count",
	[REBOOT_TEST_PM_SETTING]	= "pm_setting",
};

struct macsmc_reboot_test_write {
	u32 where;	/* SMC key, or NVMEM offset */
	size_t size;
	u32 val;
};

struct macsmc_reboot_test {
	struct apple_smc smc;
	u8 shmem[256];
	struct macsmc_reboot reboot;
	struct device *dev;
	u8 nvmem[REBOOT_TEST_NR_CELLS];
	/* NVMEM writes and SMC writes, each in order */
	struct macsmc_reboot_test_write nv_writes[8];
	unsigned int nr_nv_writes;
	struct macsmc_reboot_test_write smc_writes[4];
	unsigned int nr_smc_writes;
	int smc_ret;
	int fail_offset;
	struct nvmem_cell_lookup lookups[REBOOT_TEST_NR_CELLS];
};

static int macsmc_reboot_test_smc_write(struct apple_smc *smc, smc_key key,
					const void *buf, size_t size)
{
	struct macsmc_reboot_test *ctx = container_of(smc, struct macsmc_reboot_test, smc);
	struct macsmc_reboot_test_write *w;

	if (ctx->nr_smc_writes < ARRAY_SIZE(ctx->smc_writes)) {
		w = &ctx->smc_writes[ctx->nr_smc_writes];
		w->where = key;
		w->size = size;
		w->val = 0;
		memcpy(&w->val, buf, min(size, sizeof(w->val)));
	}
	ctx->nr_smc_writes++;
	return ctx->smc_ret;
}

static int macsmc_reboot_test_reg_read(void *priv, unsigned int offset, void *val,
				       size_t bytes)
{
	struct macsmc_reboot_test *ctx = priv;

	memcpy(val, ctx->nvmem + offset, bytes);
	return 0;
}

static int macsmc_reboot_test_reg_write(void *priv, unsigned int offset, void *val,
					size_t bytes)
{
	struct macsmc_reboot_test *ctx = priv;
	struct macsmc_reboot_test_write *w;

	if (offset == ctx->fail_offset)
		return -EIO;
	if (ctx->nr_nv_writes < ARRAY_SIZE(ctx->nv_writes)) {
		w = &ctx->nv_writes[ctx->nr_nv_writes];
		w->where = offset;
		w->size = bytes;
		w->val = *(u8 *)val;
	}
	ctx->nr_nv_writes++;
	memcpy(ctx->nvmem + offset, val, bytes);
	return 0;
}

static void macsmc_reboot_test_del_lookups(void *data)
{
	struct macsmc_reboot_test *ctx = data;

	nvmem_del_cell_lookups(ctx->lookups, ARRAY_SIZE(ctx->lookups));
}

KUNIT_DEFINE_ACTION_WRAPPER(macsmc_reboot_test_cell_put, nvmem_cell_put,
			    struct nvmem_cell *);

static struct nvmem_cell *macsmc_reboot_test_cell(struct kunit *test,
						  struct macsmc_reboot_test *ctx,
						  unsigned int idx)
{
	struct nvmem_cell *cell = nvmem_cell_get(ctx->dev, macsmc_reboot_test_cells[idx]);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cell);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, macsmc_reboot_test_cell_put,
							cell), 0);
	return cell;
}

/* A reboot handler with every cell, as macsmc_reboot_probe() leaves it. */
static struct macsmc_reboot_test *macsmc_reboot_test_setup(struct kunit *test)
{
	struct nvmem_config config = {};
	struct nvmem_cell_info *cells;
	struct macsmc_reboot_test *ctx;
	struct nvmem_device *nvmem;
	struct device *provider;
	int i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	cells = kunit_kcalloc(test, REBOOT_TEST_NR_CELLS, sizeof(*cells), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, cells);
	ctx->fail_offset = -1;
	mutex_init(&ctx->smc.mutex);
	spin_lock_init(&ctx->smc.lock);
	/* Any SMC command fails: the SMC never booted. */
	ctx->smc.shmem.iomem = (void __force __iomem *)ctx->shmem;

	for (i = 0; i < REBOOT_TEST_NR_CELLS; i++) {
		cells[i].name = macsmc_reboot_test_cells[i];
		cells[i].offset = i;
		cells[i].bytes = 1;
	}

	provider = kunit_device_register(test, "nvmem");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, provider);
	config.dev = provider;
	config.name = "macsmc-reboot-test";
	config.id = NVMEM_DEVID_AUTO;
	config.cells = cells;
	config.ncells = REBOOT_TEST_NR_CELLS;
	config.ignore_wp = true;
	config.reg_read = macsmc_reboot_test_reg_read;
	config.reg_write = macsmc_reboot_test_reg_write;
	config.size = REBOOT_TEST_NR_CELLS;
	config.priv = ctx;
	nvmem = devm_nvmem_register(provider, &config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, nvmem);

	ctx->dev = kunit_device_register(test, "reboot");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->dev);
	for (i = 0; i < REBOOT_TEST_NR_CELLS; i++) {
		ctx->lookups[i].nvmem_name = nvmem_dev_name(nvmem);
		ctx->lookups[i].cell_name = macsmc_reboot_test_cells[i];
		ctx->lookups[i].dev_id = dev_name(ctx->dev);
		ctx->lookups[i].con_id = macsmc_reboot_test_cells[i];
	}
	nvmem_add_cell_lookups(ctx->lookups, ARRAY_SIZE(ctx->lookups));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, macsmc_reboot_test_del_lookups,
							ctx), 0);

	ctx->reboot.dev = ctx->dev;
	ctx->reboot.smc = &ctx->smc;
	mutex_init(&ctx->reboot.pm_setting_lock);
	for (i = 0; i < ARRAY_SIZE(nvmem_names); i++) {
		KUNIT_ASSERT_STREQ(test, nvmem_names[i], macsmc_reboot_test_cells[i]);
		ctx->reboot.nvm_cells[i] = macsmc_reboot_test_cell(test, ctx, i);
	}
	ctx->reboot.pm_setting = macsmc_reboot_test_cell(test, ctx, REBOOT_TEST_PM_SETTING);
	ctx->reboot.reboot_notify.notifier_call = macsmc_reboot_notify;
	dev_set_drvdata(ctx->dev, &ctx->reboot);

	kunit_activate_static_stub(test, apple_smc_write, macsmc_reboot_test_smc_write);
	return ctx;
}

static int macsmc_reboot_test_notify(struct macsmc_reboot_test *ctx, unsigned long action)
{
	return macsmc_reboot_notify(&ctx->reboot.reboot_notify, action, NULL);
}

static void macsmc_reboot_test_expect_nv(struct kunit *test, struct macsmc_reboot_test *ctx,
					 unsigned int idx, unsigned int cell, u8 val)
{
	KUNIT_ASSERT_LT(test, idx, ctx->nr_nv_writes);
	KUNIT_EXPECT_EQ_MSG(test, ctx->nv_writes[idx].where, cell, "write %u", idx);
	KUNIT_EXPECT_EQ_MSG(test, ctx->nv_writes[idx].size, 1, "write %u", idx);
	KUNIT_EXPECT_EQ_MSG(test, ctx->nv_writes[idx].val, val, "write %u", idx);
}

static void macsmc_reboot_test_expect_mbse(struct kunit *test, struct macsmc_reboot_test *ctx,
					   smc_key val)
{
	KUNIT_ASSERT_EQ(test, ctx->nr_smc_writes, 1);
	KUNIT_EXPECT_EQ(test, ctx->smc_writes[0].where, SMC_KEY(MBSE));
	KUNIT_EXPECT_EQ(test, ctx->smc_writes[0].size, sizeof(u32));
	KUNIT_EXPECT_EQ(test, ctx->smc_writes[0].val, val);
}

/* Restart: MBSE = rest, a clean boot_stage and shutdown_flag 0 (power back on). */
static void macsmc_reboot_test_notify_restart(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);

	ctx->nvmem[REBOOT_TEST_BOOT_STAGE] = BOOT_STAGE_KERNEL_STARTED;
	ctx->nvmem[REBOOT_TEST_SHUTDOWN_FLAG] = 1;
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_notify(ctx, SYS_RESTART), NOTIFY_OK);
	macsmc_reboot_test_expect_mbse(test, ctx, SMC_KEY(rest));
	KUNIT_ASSERT_EQ(test, ctx->nr_nv_writes, 2);
	macsmc_reboot_test_expect_nv(test, ctx, 0, REBOOT_TEST_BOOT_STAGE, BOOT_STAGE_SHUTDOWN);
	macsmc_reboot_test_expect_nv(test, ctx, 1, REBOOT_TEST_SHUTDOWN_FLAG, 0);
}

/* Power off: MBSE = offw, a clean boot_stage and shutdown_flag 1 (stay off). */
static void macsmc_reboot_test_notify_power_off(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);

	ctx->nvmem[REBOOT_TEST_BOOT_STAGE] = BOOT_STAGE_KERNEL_STARTED;
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_notify(ctx, SYS_POWER_OFF), NOTIFY_OK);
	macsmc_reboot_test_expect_mbse(test, ctx, SMC_KEY(offw));
	KUNIT_ASSERT_EQ(test, ctx->nr_nv_writes, 2);
	macsmc_reboot_test_expect_nv(test, ctx, 0, REBOOT_TEST_BOOT_STAGE, BOOT_STAGE_SHUTDOWN);
	macsmc_reboot_test_expect_nv(test, ctx, 1, REBOOT_TEST_SHUTDOWN_FLAG, 1);
}

/* A halt is not ours to prepare: nothing is written. */
static void macsmc_reboot_test_notify_other(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);

	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_notify(ctx, SYS_HALT), NOTIFY_DONE);
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_notify(ctx, SYS_DOWN + 100), NOTIFY_DONE);
	KUNIT_EXPECT_EQ(test, ctx->nr_smc_writes, 0);
	KUNIT_EXPECT_EQ(test, ctx->nr_nv_writes, 0);
}

/* Missing cells are skipped; MBSE is still written. */
static void macsmc_reboot_test_notify_no_cells(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);

	ctx->reboot.nvm.shutdown_flag = NULL;
	ctx->reboot.nvm.boot_stage = NULL;
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_notify(ctx, SYS_POWER_OFF), NOTIFY_OK);
	macsmc_reboot_test_expect_mbse(test, ctx, SMC_KEY(offw));
	KUNIT_EXPECT_EQ(test, ctx->nr_nv_writes, 0);
}

/* A failed MBSE write does not stop the PMU flags from being written. */
static void macsmc_reboot_test_notify_mbse_error(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);

	ctx->smc_ret = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_notify(ctx, SYS_POWER_OFF), NOTIFY_OK);
	KUNIT_EXPECT_EQ(test, ctx->nr_nv_writes, 2);
	KUNIT_EXPECT_EQ(test, ctx->nvmem[REBOOT_TEST_SHUTDOWN_FLAG], 1);

	/* Nor does a failed boot_stage write stop shutdown_flag. */
	ctx->smc_ret = 0;
	ctx->fail_offset = REBOOT_TEST_BOOT_STAGE;
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_notify(ctx, SYS_RESTART), NOTIFY_OK);
	KUNIT_EXPECT_EQ(test, ctx->nvmem[REBOOT_TEST_SHUTDOWN_FLAG], 0);
}

/* Logged boot errors and panics are cleared: panic_count first. */
static void macsmc_reboot_test_error_counts(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);

	ctx->nvmem[REBOOT_TEST_BOOT_ERROR_COUNT] = 2;
	ctx->nvmem[REBOOT_TEST_PANIC_COUNT] = 0;
	macsmc_power_init_error_counts(&ctx->reboot);
	KUNIT_ASSERT_EQ(test, ctx->nr_nv_writes, 2);
	macsmc_reboot_test_expect_nv(test, ctx, 0, REBOOT_TEST_PANIC_COUNT, 0);
	macsmc_reboot_test_expect_nv(test, ctx, 1, REBOOT_TEST_BOOT_ERROR_COUNT, 0);

	ctx->nr_nv_writes = 0;
	ctx->nvmem[REBOOT_TEST_PANIC_COUNT] = 0xff;
	macsmc_power_init_error_counts(&ctx->reboot);
	KUNIT_EXPECT_EQ(test, ctx->nr_nv_writes, 2);
	KUNIT_EXPECT_EQ(test, ctx->nvmem[REBOOT_TEST_PANIC_COUNT], 0);

	/* Nothing logged: nothing written. */
	ctx->nr_nv_writes = 0;
	macsmc_power_init_error_counts(&ctx->reboot);
	KUNIT_EXPECT_EQ(test, ctx->nr_nv_writes, 0);
}

/* Without both counter cells the counters are left alone. */
static void macsmc_reboot_test_error_counts_missing(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);

	ctx->nvmem[REBOOT_TEST_BOOT_ERROR_COUNT] = 3;
	ctx->nvmem[REBOOT_TEST_PANIC_COUNT] = 4;
	ctx->reboot.nvm.panic_count = NULL;
	macsmc_power_init_error_counts(&ctx->reboot);
	KUNIT_EXPECT_EQ(test, ctx->nr_nv_writes, 0);
	KUNIT_EXPECT_EQ(test, ctx->nvmem[REBOOT_TEST_BOOT_ERROR_COUNT], 3);
}

/* Bit 5 clear powers on at connect; otherwise bit 0 picks off or restore. */
static void macsmc_reboot_test_ac_mode_decode(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, macsmc_ac_power_mode(0x21), AC_POWER_OFF);
	KUNIT_EXPECT_EQ(test, macsmc_ac_power_mode(0xff), AC_POWER_OFF);
	KUNIT_EXPECT_EQ(test, macsmc_ac_power_mode(0x20), AC_POWER_RESTORE);
	KUNIT_EXPECT_EQ(test, macsmc_ac_power_mode(0xfe), AC_POWER_RESTORE);
	KUNIT_EXPECT_EQ(test, macsmc_ac_power_mode(0x01), AC_POWER_ALWAYS);
	KUNIT_EXPECT_EQ(test, macsmc_ac_power_mode(0x00), AC_POWER_ALWAYS);
	KUNIT_EXPECT_EQ(test, macsmc_ac_power_mode(0xde), AC_POWER_ALWAYS);
}

static void macsmc_reboot_test_expect_show(struct kunit *test, struct macsmc_reboot_test *ctx,
					   u8 setting, const char *expect)
{
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	ctx->nvmem[REBOOT_TEST_PM_SETTING] = setting;
	KUNIT_EXPECT_EQ(test, ac_power_mode_show(ctx->dev, &dev_attr_ac_power_mode, buf),
			strlen(expect));
	KUNIT_EXPECT_STREQ(test, buf, expect);
}

static void macsmc_reboot_test_ac_mode_show(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);

	macsmc_reboot_test_expect_show(test, ctx, 0x21, "[off] restore always\n");
	macsmc_reboot_test_expect_show(test, ctx, 0xa0, "off [restore] always\n");
	macsmc_reboot_test_expect_show(test, ctx, 0x80, "off restore [always]\n");
	KUNIT_EXPECT_EQ(test, ctx->nr_nv_writes, 0);
}

static ssize_t macsmc_reboot_test_store(struct macsmc_reboot_test *ctx, const char *buf)
{
	return ac_power_mode_store(ctx->dev, &dev_attr_ac_power_mode, buf, strlen(buf));
}

/* A store changes bits 0 and 5 only, and only when the mode changes. */
static void macsmc_reboot_test_ac_mode_store(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);

	ctx->nvmem[REBOOT_TEST_PM_SETTING] = 0x80 | PM_SETTING_AC_POWER_MASK;
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_store(ctx, "restore\n"), 8);
	KUNIT_EXPECT_EQ(test, ctx->nvmem[REBOOT_TEST_PM_SETTING], 0xa0);
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_store(ctx, "always"), 6);
	KUNIT_EXPECT_EQ(test, ctx->nvmem[REBOOT_TEST_PM_SETTING], 0x81);
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_store(ctx, "off"), 3);
	KUNIT_EXPECT_EQ(test, ctx->nvmem[REBOOT_TEST_PM_SETTING], 0xa1);
	KUNIT_ASSERT_EQ(test, ctx->nr_nv_writes, 3);
	macsmc_reboot_test_expect_nv(test, ctx, 0, REBOOT_TEST_PM_SETTING, 0xa0);

	/* Unchanged: no write. */
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_store(ctx, "off\n"), 4);
	KUNIT_EXPECT_EQ(test, ctx->nr_nv_writes, 3);
}

static void macsmc_reboot_test_ac_mode_store_errors(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);

	ctx->nvmem[REBOOT_TEST_PM_SETTING] = 0x21;
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_store(ctx, "on"), -EINVAL);
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_store(ctx, ""), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->nr_nv_writes, 0);

	ctx->fail_offset = REBOOT_TEST_PM_SETTING;
	KUNIT_EXPECT_EQ(test, macsmc_reboot_test_store(ctx, "always"), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->nvmem[REBOOT_TEST_PM_SETTING], 0x21);
}

/* ac_power_mode exists only with a pm_setting cell. */
static void macsmc_reboot_test_ac_mode_visible(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);
	struct attribute *attr = &dev_attr_ac_power_mode.attr;

	KUNIT_EXPECT_EQ(test, macsmc_reboot_attr_visible(&ctx->dev->kobj, attr, 0), 0644);
	ctx->reboot.pm_setting = NULL;
	KUNIT_EXPECT_EQ(test, macsmc_reboot_attr_visible(&ctx->dev->kobj, attr, 0), 0);
	dev_set_drvdata(ctx->dev, NULL);
	KUNIT_EXPECT_EQ(test, macsmc_reboot_attr_visible(&ctx->dev->kobj, attr, 0), 0);
}

/*
 * off1 and phra are atomic writes. On a booted SMC that has not entered
 * atomic mode they are refused, and the handlers return at once instead of
 * waiting for the power to go (mdelay(100), then a warning).
 */
static void macsmc_reboot_test_atomic_refused(struct kunit *test)
{
	struct macsmc_reboot_test *ctx = macsmc_reboot_test_setup(test);
	struct sys_off_data data = { .cb_data = &ctx->reboot };
	ktime_t start;

	ctx->smc.boot_stage = APPLE_SMC_INITIALIZED;
	ctx->smc.atomic_mode = false;
	/* What refuses is the atomic mode check, not the boot stage. */
	KUNIT_ASSERT_EQ(test, apple_smc_write_u32_atomic(&ctx->smc, SMC_KEY(MBSE),
							 SMC_KEY(off1)), -EIO);

	start = ktime_get();
	KUNIT_EXPECT_EQ(test, macsmc_power_off(&data), NOTIFY_OK);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), start), 50);
	start = ktime_get();
	KUNIT_EXPECT_EQ(test, macsmc_restart(&data), NOTIFY_OK);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), start), 50);
}

static struct kunit_case macsmc_reboot_test_cases[] = {
	KUNIT_CASE(macsmc_reboot_test_notify_restart),
	KUNIT_CASE(macsmc_reboot_test_notify_power_off),
	KUNIT_CASE(macsmc_reboot_test_notify_other),
	KUNIT_CASE(macsmc_reboot_test_notify_no_cells),
	KUNIT_CASE(macsmc_reboot_test_notify_mbse_error),
	KUNIT_CASE(macsmc_reboot_test_error_counts),
	KUNIT_CASE(macsmc_reboot_test_error_counts_missing),
	KUNIT_CASE(macsmc_reboot_test_ac_mode_decode),
	KUNIT_CASE(macsmc_reboot_test_ac_mode_show),
	KUNIT_CASE(macsmc_reboot_test_ac_mode_store),
	KUNIT_CASE(macsmc_reboot_test_ac_mode_store_errors),
	KUNIT_CASE(macsmc_reboot_test_ac_mode_visible),
	KUNIT_CASE(macsmc_reboot_test_atomic_refused),
	{}
};

static struct kunit_suite macsmc_reboot_test_suite = {
	.name = "macsmc-reboot",
	.test_cases = macsmc_reboot_test_cases,
};

kunit_test_suite(macsmc_reboot_test_suite);
