// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple SMC GPIO driver: the gPxx key names, the output
 * command words, the board-specific lines (J700 pcIO outputs, the J713 radio
 * line) with their cached levels, the DT command templates and the
 * pcIO output-command variant.
 *
 * SMC writes go to a static stub on apple_smc_write() (see
 * MFD_MACSMC_KUNIT_TEST); the gpio_chip is registered with gpiolib so the
 * driver's callbacks run on a real chip. The SMC has no keys here, so no
 * line needs an SMC read.
 *
 * Included by gpio-macsmc.c.
 */

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/property.h>

#define GPIO_TEST_GP	((smc_key)_SMC_KEY("gP\0\0"))
#define GPIO_TEST_GPL	((smc_key)_SMC_KEY("gp\0\0"))

struct macsmc_gpio_test_write {
	smc_key key;
	size_t size;
	u64 val;
};

struct macsmc_gpio_test {
	struct apple_smc smc;
	u8 shmem[256];
	/*
	 * The driver state, and the pcIO commands it points at, live in ctx:
	 * ctx is allocated before the device, so they outlive the gpio_chip
	 * that the device's devres removes at the end of the case.
	 */
	struct macsmc_gpio gpio;
	u64 commands[2];
	struct macsmc_gpio *smcgp;
	struct device *dev;
	struct software_node swnode;
	struct macsmc_gpio_test_write writes[8];
	unsigned int nr_writes;
	int write_ret;
};

static int macsmc_gpio_test_write(struct apple_smc *smc, smc_key key,
				  const void *buf, size_t size)
{
	struct macsmc_gpio_test *ctx = container_of(smc, struct macsmc_gpio_test, smc);
	struct macsmc_gpio_test_write *w;

	if (ctx->nr_writes < ARRAY_SIZE(ctx->writes)) {
		w = &ctx->writes[ctx->nr_writes];
		w->key = key;
		w->size = size;
		w->val = 0;
		memcpy(&w->val, buf, min(size, sizeof(w->val)));
	}
	ctx->nr_writes++;
	return ctx->write_ret;
}

KUNIT_DEFINE_ACTION_WRAPPER(macsmc_gpio_test_remove_swnode, device_remove_software_node,
			    struct device *);
KUNIT_DEFINE_ACTION_WRAPPER(macsmc_gpio_test_unregister_swnode, software_node_unregister,
			    const struct software_node *);

static struct macsmc_gpio_test *macsmc_gpio_test_alloc(struct kunit *test,
						       const char *name,
						       const struct property_entry *props)
{
	struct macsmc_gpio_test *ctx;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	mutex_init(&ctx->smc.mutex);
	spin_lock_init(&ctx->smc.lock);
	/* Any SMC command fails: the SMC never booted. */
	ctx->smc.shmem.iomem = (void __force __iomem *)ctx->shmem;

	/*
	 * The gpiochip's device shares the parent's fwnode and drops its
	 * reference after the parent's devres runs, so the node is registered
	 * before the device and unregistered after it is gone. A managed node
	 * would get its reference dropped once per device.
	 */
	if (props) {
		ctx->swnode.properties = props;
		KUNIT_ASSERT_EQ(test, software_node_register(&ctx->swnode), 0);
		ret = kunit_add_action_or_reset(test, macsmc_gpio_test_unregister_swnode,
						&ctx->swnode);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}

	ctx->dev = kunit_device_register(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->dev);
	if (props) {
		KUNIT_ASSERT_EQ(test, device_add_software_node(ctx->dev, &ctx->swnode), 0);
		ret = kunit_add_action_or_reset(test, macsmc_gpio_test_remove_swnode, ctx->dev);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}

	ctx->smcgp = &ctx->gpio;
	ctx->smcgp->dev = ctx->dev;
	ctx->smcgp->smc = &ctx->smc;
	mutex_init(&ctx->smcgp->j713_reg_on_lock);

	kunit_activate_static_stub(test, apple_smc_write, macsmc_gpio_test_write);
	return ctx;
}

/* The chip macsmc_gpio_probe() registers, on an SMC without GPIO keys. */
static void macsmc_gpio_test_register(struct kunit *test, struct macsmc_gpio_test *ctx,
				      smc_key base_key, bool j700, bool j713)
{
	struct macsmc_gpio *smcgp = ctx->smcgp;

	smcgp->base_key = base_key;
	smcgp->j700_pcio = j700;
	smcgp->j713 = j713;
	KUNIT_ASSERT_EQ(test, macsmc_gpio_parse_templates(smcgp), 0);

	smcgp->gc.label = "macsmc-gpio-test";
	smcgp->gc.owner = THIS_MODULE;
	smcgp->gc.get = macsmc_gpio_get;
	smcgp->gc.set = macsmc_gpio_set;
	smcgp->gc.get_direction = macsmc_gpio_get_direction;
	smcgp->gc.init_valid_mask = macsmc_gpio_init_valid_mask;
	smcgp->gc.can_sleep = true;
	smcgp->gc.ngpio = MAX_GPIO;
	smcgp->gc.base = -1;
	smcgp->gc.parent = ctx->dev;
	KUNIT_ASSERT_EQ(test, devm_gpiochip_add_data(ctx->dev, &smcgp->gc, smcgp), 0);
}

static void macsmc_gpio_test_expect_write(struct kunit *test,
					  struct macsmc_gpio_test *ctx,
					  unsigned int idx, smc_key key,
					  size_t size, u64 val)
{
	KUNIT_ASSERT_LT(test, idx, ctx->nr_writes);
	KUNIT_EXPECT_EQ_MSG(test, ctx->writes[idx].key, key, "write %u", idx);
	KUNIT_EXPECT_EQ_MSG(test, ctx->writes[idx].size, size, "write %u", idx);
	KUNIT_EXPECT_EQ_MSG(test, ctx->writes[idx].val, val, "write %u", idx);
}

/* Pin numbers become two lowercase hex digits after the base key. */
static void macsmc_gpio_test_key_name(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, macsmc_gpio_key(GPIO_TEST_GP, 0), _SMC_KEY("gP00"));
	KUNIT_EXPECT_EQ(test, macsmc_gpio_key(GPIO_TEST_GP, 0x0a), _SMC_KEY("gP0a"));
	KUNIT_EXPECT_EQ(test, macsmc_gpio_key(GPIO_TEST_GP, 0x13), _SMC_KEY("gP13"));
	KUNIT_EXPECT_EQ(test, macsmc_gpio_key(GPIO_TEST_GP, 0x3f), _SMC_KEY("gP3f"));
	KUNIT_EXPECT_EQ(test, macsmc_gpio_key(GPIO_TEST_GPL, 0x05), _SMC_KEY("gp05"));
	KUNIT_EXPECT_EQ(test, macsmc_gpio_key(GPIO_TEST_GPL, 0xc1), _SMC_KEY("gpc1"));
}

/* Key names parse back to pin numbers; non-hex names are rejected. */
static void macsmc_gpio_test_key_nr(struct kunit *test)
{
	unsigned int i;

	KUNIT_EXPECT_EQ(test, macsmc_gpio_nr(_SMC_KEY("gP13")), 0x13);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_nr(_SMC_KEY("gP0a")), 10);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_nr(_SMC_KEY("gP0A")), 10);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_nr(_SMC_KEY("gp3f")), 63);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_nr(_SMC_KEY("gPzz")), -1);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_nr(_SMC_KEY("gPg0")), -1);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_nr(_SMC_KEY("gP0-")), -1);

	for (i = 0; i < MAX_GPIO; i++)
		KUNIT_EXPECT_EQ_MSG(test, macsmc_gpio_nr(macsmc_gpio_key(GPIO_TEST_GP, i)), i,
				    "pin %u", i);
}

/* A plain line is set with a CMD_OUTPUT word carrying the level in bit 0. */
static void macsmc_gpio_test_set_plain(struct kunit *test)
{
	struct macsmc_gpio_test *ctx = macsmc_gpio_test_alloc(test, "gpio", NULL);
	struct gpio_chip *gc = &ctx->smcgp->gc;

	macsmc_gpio_test_register(test, ctx, GPIO_TEST_GP, false, false);
	KUNIT_EXPECT_EQ(test, gc->set(gc, 5, 1), 0);
	KUNIT_EXPECT_EQ(test, gc->set(gc, 0x2c, 0), 0);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 2);
	macsmc_gpio_test_expect_write(test, ctx, 0, _SMC_KEY("gP05"), 4, CMD_OUTPUT | 1);
	macsmc_gpio_test_expect_write(test, ctx, 1, _SMC_KEY("gP2c"), 4, CMD_OUTPUT);

	ctx->write_ret = -EIO;
	KUNIT_EXPECT_EQ(test, gc->set(gc, 5, 1), -EIO);
}

/* On the J713 family gP13 is the radio's reg_on: template | level, cached. */
static void macsmc_gpio_test_j713_reg_on(struct kunit *test)
{
	struct macsmc_gpio_test *ctx = macsmc_gpio_test_alloc(test, "gpio", NULL);
	struct gpio_chip *gc = &ctx->smcgp->gc;
	const unsigned int pin = MACSMC_J713_RADIO_REG_ON_GPIO;

	macsmc_gpio_test_register(test, ctx, GPIO_TEST_GP, false, true);
	KUNIT_EXPECT_EQ(test, gc->get_direction(gc, pin), GPIO_LINE_DIRECTION_OUT);
	/* No readback: nothing is known before the first write. */
	KUNIT_EXPECT_EQ(test, gc->get(gc, pin), -EOPNOTSUPP);

	KUNIT_EXPECT_EQ(test, gc->set(gc, pin, 1), 0);
	KUNIT_EXPECT_EQ(test, gc->get(gc, pin), 1);
	KUNIT_EXPECT_EQ(test, gc->set(gc, pin, 0), 0);
	KUNIT_EXPECT_EQ(test, gc->get(gc, pin), 0);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 2);
	macsmc_gpio_test_expect_write(test, ctx, 0, _SMC_KEY("gP13"), 4,
				      MACSMC_J713_RADIO_REG_ON_TEMPLATE | 1);
	macsmc_gpio_test_expect_write(test, ctx, 1, _SMC_KEY("gP13"), 4,
				      MACSMC_J713_RADIO_REG_ON_TEMPLATE);

	/* A failed write does not change the reported level. */
	ctx->write_ret = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, gc->set(gc, pin, 1), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, gc->get(gc, pin), 0);
}

/* The radio line is gP13 only: not gp13, not another gP pin. */
static void macsmc_gpio_test_j713_other_lines(struct kunit *test)
{
	struct macsmc_gpio_test *ctx = macsmc_gpio_test_alloc(test, "gpio", NULL);
	struct gpio_chip *gc = &ctx->smcgp->gc;

	macsmc_gpio_test_register(test, ctx, GPIO_TEST_GPL, false, true);
	KUNIT_EXPECT_EQ(test, gc->set(gc, MACSMC_J713_RADIO_REG_ON_GPIO, 1), 0);
	macsmc_gpio_test_expect_write(test, ctx, 0, _SMC_KEY("gp13"), 4, CMD_OUTPUT | 1);

	ctx->smcgp->base_key = GPIO_TEST_GP;
	KUNIT_EXPECT_EQ(test, gc->set(gc, MACSMC_J713_RADIO_REG_ON_GPIO - 1, 1), 0);
	macsmc_gpio_test_expect_write(test, ctx, 1, _SMC_KEY("gP12"), 4, CMD_OUTPUT | 1);

	/* Without the J713 flag gP13 is a plain line too. */
	ctx->smcgp->j713 = false;
	KUNIT_EXPECT_EQ(test, gc->set(gc, MACSMC_J713_RADIO_REG_ON_GPIO, 0), 0);
	macsmc_gpio_test_expect_write(test, ctx, 2, _SMC_KEY("gP13"), 4, CMD_OUTPUT);
}

static const u32 macsmc_gpio_test_out_tmpl[] = { 5, 0x00a00000, 0x3e, 0x00300000 };

static const struct property_entry macsmc_gpio_test_out_props[] = {
	PROPERTY_ENTRY_U32_ARRAY("apple,output-templates", macsmc_gpio_test_out_tmpl),
	{}
};

/* apple,output-templates: <pin template> pairs; the write is template | level. */
static void macsmc_gpio_test_output_template(struct kunit *test)
{
	struct macsmc_gpio_test *ctx =
		macsmc_gpio_test_alloc(test, "gpio", macsmc_gpio_test_out_props);
	struct gpio_chip *gc = &ctx->smcgp->gc;

	macsmc_gpio_test_register(test, ctx, GPIO_TEST_GP, false, false);
	KUNIT_EXPECT_TRUE(test, gpiochip_line_is_valid(gc, 5));
	KUNIT_EXPECT_TRUE(test, gpiochip_line_is_valid(gc, 0x3e));
	KUNIT_EXPECT_FALSE(test, gpiochip_line_is_valid(gc, 6));
	KUNIT_EXPECT_EQ(test, gc->get_direction(gc, 5), GPIO_LINE_DIRECTION_OUT);

	KUNIT_EXPECT_EQ(test, gc->set(gc, 5, 1), 0);
	KUNIT_EXPECT_EQ(test, gc->set(gc, 0x3e, 0), 0);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 2);
	macsmc_gpio_test_expect_write(test, ctx, 0, _SMC_KEY("gP05"), 4, 0x00a00001);
	macsmc_gpio_test_expect_write(test, ctx, 1, _SMC_KEY("gP3e"), 4, 0x00300000);

	ctx->write_ret = -EIO;
	KUNIT_EXPECT_EQ(test, gc->set(gc, 5, 0), -EIO);
}

static const u32 macsmc_gpio_test_pcio_tmpl[] = { 21, 0x40000, 0x150000 };

static const struct property_entry macsmc_gpio_test_pcio_props[] = {
	PROPERTY_ENTRY_U32_ARRAY("apple,output-templates", macsmc_gpio_test_out_tmpl),
	PROPERTY_ENTRY_U32_ARRAY("apple,pcio-templates", macsmc_gpio_test_pcio_tmpl),
	{}
};

/*
 * J700: pins 19 and 8 and the pcio-templates pins are eight-byte pcIO
 * writes, hi << 32 | lo | level, whose level is remembered for reads.
 */
static void macsmc_gpio_test_j700_pcio(struct kunit *test)
{
	struct macsmc_gpio_test *ctx =
		macsmc_gpio_test_alloc(test, "gpio", macsmc_gpio_test_pcio_props);
	struct gpio_chip *gc = &ctx->smcgp->gc;

	macsmc_gpio_test_register(test, ctx, GPIO_TEST_GP, true, false);
	KUNIT_EXPECT_EQ(test, gc->get_direction(gc, MACSMC_J700_AFE_RESET_GPIO),
			GPIO_LINE_DIRECTION_OUT);

	KUNIT_EXPECT_EQ(test, gc->set(gc, MACSMC_J700_AFE_RESET_GPIO, 1), 0);
	KUNIT_EXPECT_EQ(test, gc->set(gc, MACSMC_J700_RADIO_REG_ON_GPIO, 1), 0);
	KUNIT_EXPECT_EQ(test, gc->set(gc, 21, 1), 0);
	KUNIT_EXPECT_EQ(test, gc->set(gc, MACSMC_J700_AFE_RESET_GPIO, 0), 0);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 4);
	macsmc_gpio_test_expect_write(test, ctx, 0, _SMC_KEY("pcIO"), 8,
				      ((u64)MACSMC_J700_AFE_RESET_HI << 32) |
				      MACSMC_J700_AFE_RESET_LO | 1);
	macsmc_gpio_test_expect_write(test, ctx, 1, _SMC_KEY("pcIO"), 8,
				      ((u64)MACSMC_J700_RADIO_REG_ON_HI << 32) |
				      MACSMC_J700_RADIO_REG_ON_LO | 1);
	macsmc_gpio_test_expect_write(test, ctx, 2, _SMC_KEY("pcIO"), 8,
				      (0x150000ULL << 32) | 0x40000 | 1);
	macsmc_gpio_test_expect_write(test, ctx, 3, _SMC_KEY("pcIO"), 8,
				      ((u64)MACSMC_J700_AFE_RESET_HI << 32) |
				      MACSMC_J700_AFE_RESET_LO);

	KUNIT_EXPECT_EQ(test, gc->get(gc, MACSMC_J700_AFE_RESET_GPIO), 0);
	KUNIT_EXPECT_EQ(test, gc->get(gc, MACSMC_J700_RADIO_REG_ON_GPIO), 1);
	KUNIT_EXPECT_EQ(test, gc->get(gc, 21), 1);

	/* A failed write keeps the remembered level. */
	ctx->write_ret = -EIO;
	KUNIT_EXPECT_EQ(test, gc->set(gc, MACSMC_J700_RADIO_REG_ON_GPIO, 0), -EIO);
	KUNIT_EXPECT_EQ(test, gc->get(gc, MACSMC_J700_RADIO_REG_ON_GPIO), 1);

	/* An output template still wins over pcIO for its own pin. */
	ctx->write_ret = 0;
	KUNIT_EXPECT_EQ(test, gc->set(gc, 5, 1), 0);
	macsmc_gpio_test_expect_write(test, ctx, 5, _SMC_KEY("gP05"), 4, 0x00a00001);
}

/* The valid mask is the board's fixed lines plus the templated ones. */
static void macsmc_gpio_test_valid_mask(struct kunit *test)
{
	struct macsmc_gpio_test *ctx =
		macsmc_gpio_test_alloc(test, "gpio", macsmc_gpio_test_pcio_props);
	struct gpio_chip *gc = &ctx->smcgp->gc;
	unsigned int i, valid = 0;

	macsmc_gpio_test_register(test, ctx, GPIO_TEST_GP, true, false);
	for (i = 0; i < MAX_GPIO; i++)
		valid += gpiochip_line_is_valid(gc, i);
	KUNIT_EXPECT_EQ(test, valid, 5);
	KUNIT_EXPECT_TRUE(test, gpiochip_line_is_valid(gc, MACSMC_J700_AFE_RESET_GPIO));
	KUNIT_EXPECT_TRUE(test, gpiochip_line_is_valid(gc, MACSMC_J700_RADIO_REG_ON_GPIO));
	KUNIT_EXPECT_TRUE(test, gpiochip_line_is_valid(gc, 21));
	KUNIT_EXPECT_TRUE(test, gpiochip_line_is_valid(gc, 5));
	KUNIT_EXPECT_TRUE(test, gpiochip_line_is_valid(gc, 0x3e));
	KUNIT_EXPECT_EQ(test, ctx->nr_writes, 0);
}

/* Off the J700, pcio-templates do nothing: those pins are plain gPxx lines. */
static void macsmc_gpio_test_pcio_template_not_j700(struct kunit *test)
{
	struct macsmc_gpio_test *ctx =
		macsmc_gpio_test_alloc(test, "gpio", macsmc_gpio_test_pcio_props);
	struct gpio_chip *gc = &ctx->smcgp->gc;

	macsmc_gpio_test_register(test, ctx, GPIO_TEST_GP, false, false);
	KUNIT_EXPECT_FALSE(test, gpiochip_line_is_valid(gc, 21));
	KUNIT_EXPECT_FALSE(test, gpiochip_line_is_valid(gc, MACSMC_J700_AFE_RESET_GPIO));
	KUNIT_EXPECT_EQ(test, gc->set(gc, 21, 1), 0);
	KUNIT_EXPECT_EQ(test, gc->set(gc, MACSMC_J700_AFE_RESET_GPIO, 1), 0);
	macsmc_gpio_test_expect_write(test, ctx, 0, _SMC_KEY("gP15"), 4, CMD_OUTPUT | 1);
	macsmc_gpio_test_expect_write(test, ctx, 1, _SMC_KEY("gP13"), 4, CMD_OUTPUT | 1);
}

static int macsmc_gpio_test_parse(struct kunit *test, const char *name,
				  const struct property_entry *props)
{
	struct macsmc_gpio_test *ctx = macsmc_gpio_test_alloc(test, name, props);

	return macsmc_gpio_parse_templates(ctx->smcgp);
}

/* Malformed template lists are refused, the pin bound is 0..63. */
static void macsmc_gpio_test_template_errors(struct kunit *test)
{
	static const u32 odd[] = { 5, 0x00a00000, 6 };
	static const u32 pin64[] = { 64, 0x00a00000 };
	static const u32 pin63[] = { 63, 0x00a00000 };
	static const u32 pcio_short[] = { 21, 0x40000, 0x150000, 22 };
	static const u32 pcio_pin64[] = { 64, 0x40000, 0x150000 };
	const struct property_entry p_odd[] = {
		PROPERTY_ENTRY_U32_ARRAY("apple,output-templates", odd), {}
	};
	const struct property_entry p_pin64[] = {
		PROPERTY_ENTRY_U32_ARRAY("apple,output-templates", pin64), {}
	};
	const struct property_entry p_pin63[] = {
		PROPERTY_ENTRY_U32_ARRAY("apple,output-templates", pin63), {}
	};
	const struct property_entry p_pcio_short[] = {
		PROPERTY_ENTRY_U32_ARRAY("apple,pcio-templates", pcio_short), {}
	};
	const struct property_entry p_pcio_pin64[] = {
		PROPERTY_ENTRY_U32_ARRAY("apple,pcio-templates", pcio_pin64), {}
	};

	KUNIT_EXPECT_EQ(test, macsmc_gpio_test_parse(test, "parse0", p_odd), -EINVAL);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_test_parse(test, "parse1", p_pin64), -EINVAL);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_test_parse(test, "parse2", p_pin63), 0);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_test_parse(test, "parse3", p_pcio_short), -EINVAL);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_test_parse(test, "parse4", p_pcio_pin64), -EINVAL);
	KUNIT_EXPECT_EQ(test, macsmc_gpio_test_parse(test, "parse5", NULL), 0);
}

/* apple,smc-pcio-gpio: line N writes output_commands[N] | level as LE64 to pcIO. */
static void macsmc_gpio_test_pcio_outputs(struct kunit *test)
{
	struct macsmc_gpio_test *ctx = macsmc_gpio_test_alloc(test, "gpio", NULL);
	struct macsmc_gpio *smcgp = ctx->smcgp;
	struct gpio_chip *gc = &smcgp->gc;
	static const u64 commands[] = { 0x0013000000010000ULL, 0x0008000000800000ULL };

	BUILD_BUG_ON(sizeof(commands) != sizeof(ctx->commands));
	memcpy(ctx->commands, commands, sizeof(commands));
	smcgp->output_commands = ctx->commands;

	/* As macsmc_pcio_probe() sets it up. */
	gc->label = "macsmc-pcio-output-test";
	gc->owner = THIS_MODULE;
	gc->get = macsmc_pcio_get;
	gc->set = macsmc_pcio_set;
	gc->direction_output = macsmc_pcio_set;
	gc->get_direction = macsmc_pcio_get_direction;
	gc->can_sleep = true;
	gc->ngpio = ARRAY_SIZE(commands);
	gc->base = -1;
	gc->parent = ctx->dev;
	KUNIT_ASSERT_EQ(test, devm_gpiochip_add_data(ctx->dev, gc, smcgp), 0);

	KUNIT_EXPECT_EQ(test, gc->get_direction(gc, 1), GPIO_LINE_DIRECTION_OUT);
	KUNIT_EXPECT_EQ(test, gc->get(gc, 0), -EOPNOTSUPP);

	/* A positive status from the write is success. */
	ctx->write_ret = 8;
	KUNIT_EXPECT_EQ(test, gc->set(gc, 0, 1), 0);
	ctx->write_ret = 0;
	KUNIT_EXPECT_EQ(test, gc->direction_output(gc, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, gc->set(gc, 1, 1), 0);
	KUNIT_ASSERT_EQ(test, ctx->nr_writes, 3);
	macsmc_gpio_test_expect_write(test, ctx, 0, _SMC_KEY("pcIO"), 8, commands[0] | 1);
	macsmc_gpio_test_expect_write(test, ctx, 1, _SMC_KEY("pcIO"), 8, commands[1]);
	macsmc_gpio_test_expect_write(test, ctx, 2, _SMC_KEY("pcIO"), 8, commands[1] | 1);

	ctx->write_ret = -EBUSY;
	KUNIT_EXPECT_EQ(test, gc->set(gc, 0, 0), -EBUSY);
}

static struct kunit_case macsmc_gpio_test_cases[] = {
	KUNIT_CASE(macsmc_gpio_test_key_name),
	KUNIT_CASE(macsmc_gpio_test_key_nr),
	KUNIT_CASE(macsmc_gpio_test_set_plain),
	KUNIT_CASE(macsmc_gpio_test_j713_reg_on),
	KUNIT_CASE(macsmc_gpio_test_j713_other_lines),
	KUNIT_CASE(macsmc_gpio_test_output_template),
	KUNIT_CASE(macsmc_gpio_test_j700_pcio),
	KUNIT_CASE(macsmc_gpio_test_valid_mask),
	KUNIT_CASE(macsmc_gpio_test_pcio_template_not_j700),
	KUNIT_CASE(macsmc_gpio_test_template_errors),
	KUNIT_CASE(macsmc_gpio_test_pcio_outputs),
	{}
};

static struct kunit_suite macsmc_gpio_test_suite = {
	.name = "macsmc-gpio",
	.test_cases = macsmc_gpio_test_cases,
};

kunit_test_suite(macsmc_gpio_test_suite);
