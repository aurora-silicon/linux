// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Apple SART driver. Included by sart.c.
 *
 * The SART under test is a struct apple_sart whose register block is a
 * zeroed buffer of kernel memory, as large as the widest layout (SARTv4).
 * The tests call the driver's own entry points and compare the whole block
 * with a model that encodes each entry independently of the driver's
 * macros, so a write to a wrong register shows up as well as a wrong value.
 * No device is probed and no MMIO is touched.
 */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/slab.h>

#define SART_TEST_REGS_SIZE 0x100
#define SART_TEST_NREGS	    (SART_TEST_REGS_SIZE / 4)
#define SART_TEST_PG(n)	    ((phys_addr_t)(n) << 12)

/* What the test expects of each register layout, in numbers of its own */
struct sart_test_ver {
	const char *name;
	struct apple_sart_ops *ops;
	unsigned int paddr_reg;
	/* 0: the size shares the config register with the flags */
	unsigned int size_reg;
	u8 allow;
	/* in 4K pages */
	u32 size_max;
};

static const struct sart_test_ver sart_test_vers[] = {
	{ "v0", &sart_ops_v0, 0x40, 0, 0x0f, 0x7ffff },
	{ "v2", &sart_ops_v2, 0x40, 0, 0xff, 0xffffff },
	{ "v3", &sart_ops_v3, 0x40, 0x80, 0xff, 0x3fffffff },
	{ "v4", &sart_ops_v4, 0x60, 0xc0, 0xff, 0x3fffffff },
};

static void sart_test_ver_desc(const struct sart_test_ver *v, char *desc)
{
	strscpy(desc, v->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(sart_test_ver, sart_test_vers, sart_test_ver_desc);

struct sart_test {
	struct apple_sart sart;
	const struct sart_test_ver *ver;
	__le32 *regs;
	u32 expect[SART_TEST_NREGS];
	suspend_state_t pm_state;
	unsigned int pm_flags;
};

static struct sart_test *sart_test_new(struct kunit *test,
				       const struct sart_test_ver *ver)
{
	/* A case may set up several SARTs: test->priv counts them */
	unsigned long nr = (unsigned long)test->priv;
	struct sart_test *t;
	struct device *dev;
	char name[16];

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->regs = kunit_kzalloc(test, SART_TEST_REGS_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->regs);
	snprintf(name, sizeof(name), "sart-test%lu", nr);
	test->priv = (void *)(nr + 1);
	dev = kunit_device_register(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	t->ver = ver;
	t->sart.dev = dev;
	t->sart.ops = ver->ops;
	t->sart.regs = (void __iomem __force *)t->regs;
	dev_set_drvdata(dev, &t->sart);

	return t;
}

static u32 sart_test_rd(struct sart_test *t, unsigned int reg)
{
	return le32_to_cpu(t->regs[reg / 4]);
}

static void __maybe_unused sart_test_wr(struct sart_test *t, unsigned int reg, u32 val)
{
	t->regs[reg / 4] = cpu_to_le32(val);
}

/* The model: entry idx holds these flags, address and size (in pages) */
static void sart_test_model(struct sart_test *t, int idx, u8 flags,
			    u32 paddr_pg, u32 size_pg)
{
	const struct sart_test_ver *v = t->ver;

	if (v->size_reg) {
		t->expect[idx] = flags;
		t->expect[v->size_reg / 4 + idx] = size_pg;
	} else {
		t->expect[idx] = (u32)flags << 24 | size_pg;
	}
	t->expect[v->paddr_reg / 4 + idx] = paddr_pg;
}

/* An entry that is in the registers before the driver sees them */
static void sart_test_preload(struct sart_test *t, int idx, u8 flags,
			      u32 paddr_pg, u32 size_pg)
{
	int i;

	sart_test_model(t, idx, flags, paddr_pg, size_pg);
	for (i = 0; i < SART_TEST_NREGS; i++)
		t->regs[i] = cpu_to_le32(t->expect[i]);
}

static void sart_test_check(struct kunit *test, struct sart_test *t)
{
	int i;

	for (i = 0; i < SART_TEST_NREGS; i++)
		KUNIT_EXPECT_EQ_MSG(test, le32_to_cpu(t->regs[i]), t->expect[i],
				    "register %#x", 4 * i);
}

static unsigned int sart_test_nonzero(struct sart_test *t)
{
	unsigned int n = 0;
	int i;

	for (i = 0; i < SART_TEST_NREGS; i++)
		n += !!t->regs[i];
	return n;
}

static struct sart_test *sart_test_two_regions(struct kunit *test, int ver)
{
	struct sart_test *t = sart_test_new(test, &sart_test_vers[ver]);

	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(&t->sart, 0x812345000ULL, 0x40000));
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(&t->sart, 0x9abcd000, 0x1000));
	return t;
}

/* The register values of two added regions, written out for each layout */
static void sart_test_add_encoding(struct kunit *test)
{
	struct sart_test *t;

	/* SARTv0: 5 bits of flags over a 19-bit size, addresses at 0x40 */
	t = sart_test_two_regions(test, 0);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x00), 0x0f000040);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x40), 0x00812345);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x04), 0x0f000001);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x44), 0x0009abcd);
	KUNIT_EXPECT_EQ(test, sart_test_nonzero(t), 4);

	/* SARTv2: 8 bits of flags over a 24-bit size, addresses at 0x40 */
	t = sart_test_two_regions(test, 1);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x00), 0xff000040);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x40), 0x00812345);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x04), 0xff000001);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x44), 0x0009abcd);
	KUNIT_EXPECT_EQ(test, sart_test_nonzero(t), 4);

	/* SARTv3: flags, addresses at 0x40, sizes at 0x80 */
	t = sart_test_two_regions(test, 2);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x00), 0xff);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x40), 0x00812345);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x80), 0x40);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x04), 0xff);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x44), 0x0009abcd);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x84), 0x1);
	KUNIT_EXPECT_EQ(test, sart_test_nonzero(t), 6);

	/* SARTv4: flags, addresses at 0x60, sizes at 0xc0 */
	t = sart_test_two_regions(test, 3);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x00), 0xff);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x60), 0x00812345);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0xc0), 0x40);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x04), 0xff);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0x64), 0x0009abcd);
	KUNIT_EXPECT_EQ(test, sart_test_rd(t, 0xc4), 0x1);
	KUNIT_EXPECT_EQ(test, sart_test_nonzero(t), 6);
}

/* Sixteen regions take the sixteen entries in order; the next is refused */
static void sart_test_add_fills_table(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	int i;

	for (i = 0; i < 16; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				apple_sart_add_allowed_region(&t->sart,
							      SART_TEST_PG(0x100000 + 0x100 * i),
							      SART_TEST_PG(i + 1)));
		sart_test_model(t, i, t->ver->allow, 0x100000 + 0x100 * i, i + 1);
		sart_test_check(test, t);
	}
	KUNIT_EXPECT_EQ(test, t->sart.used_entries, 0xffff);

	KUNIT_EXPECT_EQ(test, -EBUSY,
			apple_sart_add_allowed_region(&t->sart, SART_TEST_PG(0x200000),
						      SART_TEST_PG(1)));
	sart_test_check(test, t);
	KUNIT_EXPECT_EQ(test, t->sart.used_entries, 0xffff);
}

/* The bootloader's entries are neither reused nor rewritten */
static void sart_test_add_skips_protected(struct kunit *test)
{
	static const int slots[] = { 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
	struct sart_test *t = sart_test_new(test, test->param_value);
	int i;

	sart_test_preload(t, 0, t->ver->allow, 0x800000, 0x400);
	sart_test_preload(t, 1, 0x01, 0x900000, 0x10);
	sart_test_preload(t, 5, t->ver->allow, 0xa00000, 0x1);
	t->sart.protected_entries = BIT(0) | BIT(1) | BIT(5);

	for (i = 0; i < ARRAY_SIZE(slots); i++) {
		KUNIT_ASSERT_EQ(test, 0,
				apple_sart_add_allowed_region(&t->sart,
							      SART_TEST_PG(0x100000 + i),
							      SART_TEST_PG(1)));
		sart_test_model(t, slots[i], t->ver->allow, 0x100000 + i, 1);
		sart_test_check(test, t);
	}
	KUNIT_EXPECT_EQ(test, t->sart.used_entries, 0xffff & ~0x23);

	KUNIT_EXPECT_EQ(test, -EBUSY,
			apple_sart_add_allowed_region(&t->sart, SART_TEST_PG(0x200000),
						      SART_TEST_PG(1)));
	sart_test_check(test, t);
	KUNIT_EXPECT_EQ(test, t->sart.protected_entries, 0x23);
}

/* A region that is not whole pages is refused and costs no entry */
static void sart_test_add_rejects_unaligned(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;

	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x1000) + 0x800,
						      SART_TEST_PG(1)));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x1000) + 1,
						      SART_TEST_PG(1)));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x1000) + 0xfff,
						      SART_TEST_PG(1)));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x1000), 0x1800));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x1000), 0xfff));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x1000),
						      SART_TEST_PG(1) + 1));
	KUNIT_EXPECT_EQ(test, sart->used_entries, 0);
	sart_test_check(test, t);

	/* The first entry is still the next one handed out */
	KUNIT_EXPECT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x1000),
						      SART_TEST_PG(1)));
	sart_test_model(t, 0, t->ver->allow, 0x1000, 1);
	sart_test_check(test, t);
	KUNIT_EXPECT_EQ(test, sart->used_entries, BIT(0));
}

/* The largest size the layout's field holds is taken, one page more is not */
static void sart_test_add_size_max(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	size_t max = (size_t)t->ver->size_max << 12;
	phys_addr_t paddr;
	size_t size;
	u8 flags;

	KUNIT_EXPECT_EQ(test, 0, apple_sart_add_allowed_region(sart, SART_TEST_PG(0x1000), max));
	sart_test_model(t, 0, t->ver->allow, 0x1000, t->ver->size_max);
	sart_test_check(test, t);

	/* A full size field must not spill into the flags */
	sart->ops->get_entry(sart, 0, &flags, &paddr, &size);
	KUNIT_EXPECT_EQ(test, flags, t->ver->allow);
	KUNIT_EXPECT_EQ(test, size, max);
	KUNIT_EXPECT_EQ(test, paddr, SART_TEST_PG(0x1000));

	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x2000),
						      max + SART_TEST_PG(1)));
	sart_test_check(test, t);
	KUNIT_EXPECT_EQ(test, sart->used_entries, BIT(0));
}

/* The highest address the 32-bit page register holds, there and back */
static void sart_test_high_paddr(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	phys_addr_t paddr;
	size_t size;
	u8 flags;

	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0xffffffff),
						      SART_TEST_PG(1)));
	sart_test_model(t, 0, t->ver->allow, 0xffffffff, 1);
	sart_test_check(test, t);

	sart->ops->get_entry(sart, 0, &flags, &paddr, &size);
	KUNIT_EXPECT_EQ(test, paddr, 0xffffffff000ULL);
	KUNIT_EXPECT_EQ(test, size, 0x1000);

	KUNIT_EXPECT_EQ(test, 0,
			apple_sart_remove_allowed_region(sart, 0xffffffff000ULL, 0x1000));
	sart_test_model(t, 0, 0, 0, 0);
	sart_test_check(test, t);
}

/* get_entry reads what a bootloader left in any entry, the last included */
static void sart_test_get_entry(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	phys_addr_t paddr;
	size_t size;
	u8 flags;

	sart_test_preload(t, 7, 0x05, 0x1, 0x1);
	sart_test_preload(t, 15, t->ver->allow, 0xfedcba98, t->ver->size_max);

	sart->ops->get_entry(sart, 7, &flags, &paddr, &size);
	KUNIT_EXPECT_EQ(test, flags, 0x05);
	KUNIT_EXPECT_EQ(test, paddr, 0x1000);
	KUNIT_EXPECT_EQ(test, size, 0x1000);

	sart->ops->get_entry(sart, 15, &flags, &paddr, &size);
	KUNIT_EXPECT_EQ(test, flags, t->ver->allow);
	KUNIT_EXPECT_EQ(test, paddr, 0xfedcba98000ULL);
	KUNIT_EXPECT_EQ(test, size, (size_t)t->ver->size_max << 12);

	sart->ops->get_entry(sart, 8, &flags, &paddr, &size);
	KUNIT_EXPECT_EQ(test, flags, 0);
	KUNIT_EXPECT_EQ(test, paddr, 0);
	KUNIT_EXPECT_EQ(test, size, 0);

	/* Reading changes nothing */
	sart_test_check(test, t);
}

/* A removed region's entry reads empty and is the next one handed out */
static void sart_test_remove_frees_slot(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	int i;

	for (i = 0; i < 3; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				apple_sart_add_allowed_region(sart, SART_TEST_PG(0x4000 + i),
							      SART_TEST_PG(2 + i)));
		sart_test_model(t, i, t->ver->allow, 0x4000 + i, 2 + i);
	}

	KUNIT_EXPECT_EQ(test, 0,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x4001),
							 SART_TEST_PG(3)));
	sart_test_model(t, 1, 0, 0, 0);
	sart_test_check(test, t);
	KUNIT_EXPECT_EQ(test, sart->used_entries, BIT(0) | BIT(2));

	KUNIT_EXPECT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x7000),
						      SART_TEST_PG(8)));
	sart_test_model(t, 1, t->ver->allow, 0x7000, 8);
	sart_test_check(test, t);
	KUNIT_EXPECT_EQ(test, sart->used_entries, 0x7);
}

/* Only the exact address and size that were added can be removed, once */
static void sart_test_remove_nonexistent(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;

	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x4000),
						      SART_TEST_PG(4)));
	sart_test_model(t, 0, t->ver->allow, 0x4000, 4);

	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x9000),
							 SART_TEST_PG(4)));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x4000),
							 SART_TEST_PG(5)));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x4000),
							 SART_TEST_PG(3)));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x4001),
							 SART_TEST_PG(4)));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x4000) + 1,
							 SART_TEST_PG(4)));
	sart_test_check(test, t);
	KUNIT_EXPECT_EQ(test, sart->used_entries, BIT(0));

	KUNIT_EXPECT_EQ(test, 0,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x4000),
							 SART_TEST_PG(4)));
	sart_test_model(t, 0, 0, 0, 0);
	sart_test_check(test, t);
	KUNIT_EXPECT_EQ(test, sart->used_entries, 0);

	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x4000),
							 SART_TEST_PG(4)));
	sart_test_check(test, t);
}

/* A bootloader's entry can't be removed, whatever address and size are asked */
static void sart_test_remove_protected(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;

	sart_test_preload(t, 3, t->ver->allow, 0x800000, 0x400);
	sart->protected_entries = BIT(3);

	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x800000),
							 SART_TEST_PG(0x400)));
	sart_test_check(test, t);
	KUNIT_EXPECT_EQ(test, sart->protected_entries, BIT(3));
}

static void sart_test_inherited_region(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	const phys_addr_t base = SART_TEST_PG(0x80000);
	const size_t len = SART_TEST_PG(0x10);

	sart_test_preload(t, 2, t->ver->allow, 0x80000, 0x10);
	/* Allowed, but not the bootloader's */
	sart_test_preload(t, 4, t->ver->allow, 0x90000, 0x10);
	/* The bootloader's, but with other flags */
	sart_test_preload(t, 6, 0x01, 0xa0000, 0x10);
	sart->protected_entries = BIT(2) | BIT(6);

	KUNIT_EXPECT_TRUE(test, apple_sart_is_inherited_region(sart, base, len));
	KUNIT_EXPECT_TRUE(test, apple_sart_is_inherited_region(sart, base, 0x1000));
	KUNIT_EXPECT_TRUE(test, apple_sart_is_inherited_region(sart, base + len - 0x1000,
							       0x1000));
	KUNIT_EXPECT_TRUE(test, apple_sart_is_inherited_region(sart, base + 1, 1));
	KUNIT_EXPECT_TRUE(test, apple_sart_is_inherited_region(sart, base + len - 1, 1));
	KUNIT_EXPECT_TRUE(test, apple_sart_is_inherited_region(sart, base + 1, len - 1));

	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, base, len + 1));
	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, base + 1, len));
	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, base + len, 1));
	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, base - 1, 1));
	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, base - 0x1000,
								len + 0x1000));
	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, base + len - 0x1000,
								0x2000));
	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, base, 0));
	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, PHYS_ADDR_MAX, 2));
	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, base, SIZE_MAX));

	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, SART_TEST_PG(0x90000),
								len));
	KUNIT_EXPECT_FALSE(test, apple_sart_is_inherited_region(sart, SART_TEST_PG(0xa0000),
								len));
	sart_test_check(test, t);
}

/* Shutdown empties every entry but the bootloader's */
static void sart_test_shutdown(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	struct platform_device *pdev;
	int i;

	pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pdev);
	platform_set_drvdata(pdev, sart);

	sart_test_preload(t, 0, t->ver->allow, 0x800000, 0x400);
	sart_test_preload(t, 15, t->ver->allow, 0x900000, 0x10);
	/* Not the bootloader's and not handed out either */
	sart_test_preload(t, 12, t->ver->allow, 0xa00000, 0x10);
	sart->protected_entries = BIT(0) | BIT(15);
	for (i = 0; i < 3; i++)
		KUNIT_ASSERT_EQ(test, 0,
				apple_sart_add_allowed_region(sart, SART_TEST_PG(0x4000 + i),
							      SART_TEST_PG(1)));
	KUNIT_ASSERT_EQ(test, sart->used_entries, BIT(1) | BIT(2) | BIT(3));
	KUNIT_ASSERT_NE(test, sart_test_rd(t, 0x04), 0);

	apple_sart_shutdown(pdev);

	sart_test_model(t, 12, 0, 0, 0);
	sart_test_check(test, t);
}

/* Each compatible selects the register layout of its SART generation */
static void sart_test_of_match(struct kunit *test)
{
	static const struct {
		const char *compat;
		int len;
		const struct apple_sart_ops *ops;
	} cases[] = {
		{ "apple,t8015-sart", 17, &sart_ops_v0 },
		{ "apple,t8103-sart", 17, &sart_ops_v2 },
		{ "apple,t6000-sart", 17, &sart_ops_v3 },
		{ "apple,t6050-sart", 17, &sart_ops_v4 },
		{ "apple,t8152-sart", 17, &sart_ops_v4 },
		/* A newer SoC falls back to the generation it is compatible with */
		{ "apple,t8112-sart\0apple,t6000-sart", 34, &sart_ops_v3 },
		{ "apple,t6020-sart\0apple,t6000-sart", 34, &sart_ops_v3 },
	};
	const struct of_device_id *match;
	struct device_node *np;
	struct property *prop;
	int i;

	np = kunit_kzalloc(test, sizeof(*np), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, np);
	prop = kunit_kzalloc(test, sizeof(*prop), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, prop);
	prop->name = (char *)"compatible";
	np->properties = prop;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		prop->value = (void *)cases[i].compat;
		prop->length = cases[i].len;
		match = of_match_node(apple_sart_of_match, np);
		KUNIT_ASSERT_NOT_NULL_MSG(test, match, "%s", cases[i].compat);
		KUNIT_EXPECT_PTR_EQ_MSG(test, match->data, cases[i].ops, "%s",
					cases[i].compat);
	}

	prop->value = (void *)"apple,t8103-dart";
	prop->length = 17;
	KUNIT_EXPECT_NULL(test, of_match_node(apple_sart_of_match, np));
}

/* A consumer without an apple,sart phandle has no SART */
static void sart_test_get_no_phandle(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, &sart_test_vers[1]);
	struct device *dev = t->sart.dev;
	struct device_node *np;
	struct apple_sart *sart;

	sart = devm_apple_sart_get(dev);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(sart), -ENODEV);

	np = kunit_kzalloc(test, sizeof(*np), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, np);
	dev->of_node = np;
	sart = devm_apple_sart_get(dev);
	dev->of_node = NULL;
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(sart), -ENODEV);
}

#ifdef CONFIG_SUSPEND
static void sart_test_pm_restore(void *data)
{
	struct sart_test *t = data;

	pm_suspend_target_state = t->pm_state;
	pm_suspend_global_flags = t->pm_flags;
}

/* Pretend the system is in a suspend to RAM that the firmware carries out */
static void sart_test_pm_firmware_sleep(struct kunit *test, struct sart_test *t)
{
	t->pm_state = pm_suspend_target_state;
	t->pm_flags = pm_suspend_global_flags;
	KUNIT_ASSERT_EQ(test, 0, kunit_add_action_or_reset(test, sart_test_pm_restore, t));

	pm_suspend_target_state = PM_SUSPEND_MEM;
	pm_suspend_clear_flags();
	pm_set_suspend_via_firmware();
}

static void sart_test_snapshot(struct kunit *test, struct sart_test *t)
{
	sart_test_pm_firmware_sleep(test, t);
	KUNIT_ASSERT_EQ(test, 0, apple_sart_suspend_late(t->sart.dev));
	KUNIT_ASSERT_TRUE(test, t->sart.saved_valid);
}

/* The SART's power domain went off: every register reads zero */
static void sart_test_power_loss(struct sart_test *t)
{
	memset(t->regs, 0, SART_TEST_REGS_SIZE);
}

/* Only a suspend to RAM carried out by the firmware is snapshotted */
static void sart_test_suspend_not_firmware(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;

	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x4000),
						      SART_TEST_PG(4)));
	sart_test_pm_firmware_sleep(test, t);

	pm_suspend_clear_flags();
	sart->saved_valid = true;
	sart->replay_valid = true;
	KUNIT_EXPECT_EQ(test, 0, apple_sart_suspend_late(sart->dev));
	KUNIT_EXPECT_FALSE(test, sart->saved_valid);
	KUNIT_EXPECT_FALSE(test, sart->replay_valid);

	pm_set_suspend_via_firmware();
	pm_suspend_target_state = PM_SUSPEND_TO_IDLE;
	KUNIT_EXPECT_EQ(test, 0, apple_sart_suspend_late(sart->dev));
	KUNIT_EXPECT_FALSE(test, sart->saved_valid);

	/* Without a snapshot the wake writes nothing */
	sart_test_power_loss(t);
	pm_set_resume_via_firmware();
	KUNIT_EXPECT_EQ(test, 0, apple_sart_resume_noirq(sart->dev));
	KUNIT_EXPECT_EQ(test, sart_test_nonzero(t), 0);
	KUNIT_EXPECT_FALSE(test, sart->replay_valid);
}

/* After a firmware sleep every entry is back, the bootloader's included */
static void sart_test_suspend_resume(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;

	sart_test_preload(t, 0, t->ver->allow, 0x800000, 0x400);
	sart_test_preload(t, 15, t->ver->allow, 0xfedcba98, t->ver->size_max);
	sart->protected_entries = BIT(0) | BIT(15);
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x4000),
						      SART_TEST_PG(4)));
	sart_test_model(t, 1, t->ver->allow, 0x4000, 4);
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x812345),
						      SART_TEST_PG(0x40)));
	sart_test_model(t, 2, t->ver->allow, 0x812345, 0x40);

	sart_test_snapshot(test, t);
	sart_test_check(test, t);
	sart_test_power_loss(t);

	/* A wake that the firmware did not carry out leaves the SART alone */
	KUNIT_EXPECT_EQ(test, 0, apple_sart_resume_noirq(sart->dev));
	KUNIT_EXPECT_EQ(test, sart_test_nonzero(t), 0);
	KUNIT_EXPECT_TRUE(test, sart->saved_valid);

	pm_set_resume_via_firmware();
	KUNIT_EXPECT_EQ(test, 0, apple_sart_resume_noirq(sart->dev));
	sart_test_check(test, t);
	KUNIT_EXPECT_FALSE(test, sart->saved_valid);
	KUNIT_EXPECT_TRUE(test, sart->replay_valid);
	KUNIT_EXPECT_EQ(test, sart->used_entries, BIT(1) | BIT(2));

	/* The snapshot is used once */
	sart_test_power_loss(t);
	KUNIT_EXPECT_EQ(test, 0, apple_sart_resume_noirq(sart->dev));
	KUNIT_EXPECT_EQ(test, sart_test_nonzero(t), 0);
}

/*
 * What the wake does with entries nobody uses that don't read empty after the
 * sleep. Before SARTv4 they are left alone. SARTv4 empties them again, except
 * the last one, which is only written when in use or whole in the snapshot.
 */
static void sart_test_resume_idle_entries(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	bool v4 = t->ver->ops == &sart_ops_v4;

	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x4000),
						      SART_TEST_PG(4)));
	sart_test_model(t, 0, t->ver->allow, 0x4000, 4);
	sart_test_snapshot(test, t);

	sart_test_power_loss(t);
	/* Left behind by the power-up in entries 5 and 15 */
	sart_test_wr(t, 0x14, t->ver->size_reg ? 0xff : 0xff000000);
	sart_test_wr(t, t->ver->paddr_reg + 0x14, 0x1234);
	sart_test_wr(t, 0x3c, t->ver->size_reg ? 0xff : 0xff000000);
	if (!v4) {
		t->expect[0x14 / 4] = sart_test_rd(t, 0x14);
		t->expect[(t->ver->paddr_reg + 0x14) / 4] = 0x1234;
	}
	t->expect[0x3c / 4] = sart_test_rd(t, 0x3c);

	pm_set_resume_via_firmware();
	KUNIT_EXPECT_EQ(test, 0, apple_sart_resume_noirq(sart->dev));
	sart_test_check(test, t);
}

/* The last entry is written back like any other when a driver uses it */
static void sart_test_resume_last_in_use(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	int i;

	for (i = 0; i < 16; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				apple_sart_add_allowed_region(sart, SART_TEST_PG(0x4000 + i),
							      SART_TEST_PG(1 + i)));
		sart_test_model(t, i, t->ver->allow, 0x4000 + i, 1 + i);
	}
	sart_test_snapshot(test, t);
	sart_test_power_loss(t);
	sart_test_wr(t, 0x3c, t->ver->size_reg ? 0xff : 0xff000000);

	pm_set_resume_via_firmware();
	KUNIT_EXPECT_EQ(test, 0, apple_sart_resume_noirq(sart->dev));
	sart_test_check(test, t);
}

/* The replay before a coprocessor's run bit: SARTv3 and SARTv4, after a wake */
static void sart_test_replay(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	bool v3 = t->ver->size_reg;

	sart_test_preload(t, 0, t->ver->allow, 0x800000, 0x400);
	sart->protected_entries = BIT(0);
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x812345),
						      SART_TEST_PG(0x40)));
	sart_test_model(t, 1, t->ver->allow, 0x812345, 0x40);

	/* No sleep yet: nothing to replay */
	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP, apple_sart_replay(sart));

	sart_test_snapshot(test, t);
	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP, apple_sart_replay(sart));
	sart_test_power_loss(t);
	pm_set_resume_via_firmware();
	KUNIT_ASSERT_EQ(test, 0, apple_sart_resume_noirq(sart->dev));
	sart_test_check(test, t);

	/* The coprocessor's domain cycled once more after the wake */
	sart_test_power_loss(t);
	if (!v3) {
		KUNIT_EXPECT_EQ(test, -EOPNOTSUPP, apple_sart_replay(sart));
		KUNIT_EXPECT_EQ(test, sart_test_nonzero(t), 0);
		return;
	}
	KUNIT_EXPECT_EQ(test, 0, apple_sart_replay(sart));
	sart_test_check(test, t);

	/* An entry that does not hold what was written is counted */
	sart->saved[1].paddr |= BIT_ULL(50);
	KUNIT_EXPECT_EQ(test, 1, apple_sart_replay(sart));
	sart_test_check(test, t);
}

/*
 * A region removed after a wake, as a driver reclaiming a buffer does, is not brought back
 * by a later replay; the bootloader's entry and the regions still in use are. One removed
 * while the snapshot still waits for its wake stays in it: that wake writes it back as found.
 */
static void sart_test_replay_after_remove(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	bool v3 = t->ver->size_reg;

	sart_test_preload(t, 0, t->ver->allow, 0x800000, 0x400);
	sart->protected_entries = BIT(0);
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x4000),
						      SART_TEST_PG(4)));
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x812345),
						      SART_TEST_PG(0x40)));
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x9000),
						      SART_TEST_PG(8)));
	sart_test_model(t, 1, t->ver->allow, 0x4000, 4);
	sart_test_model(t, 2, t->ver->allow, 0x812345, 0x40);

	sart_test_snapshot(test, t);
	/* Before the wake: the snapshot keeps it */
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x9000),
							 SART_TEST_PG(8)));
	KUNIT_EXPECT_EQ(test, sart->saved[3].size, (size_t)SART_TEST_PG(8));

	sart_test_power_loss(t);
	pm_set_resume_via_firmware();
	KUNIT_ASSERT_EQ(test, 0, apple_sart_resume_noirq(sart->dev));
	KUNIT_ASSERT_TRUE(test, sart->replay_valid);
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x9000),
							 SART_TEST_PG(8)));

	/* After it: gone from the snapshot, and the protected entry can't be removed */
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x4000),
							 SART_TEST_PG(4)));
	sart_test_model(t, 1, 0, 0, 0);
	KUNIT_EXPECT_EQ(test, sart->saved[1].size, (size_t)0);
	KUNIT_EXPECT_EQ(test, sart->saved[1].flags, 0);
	KUNIT_EXPECT_EQ(test, sart->saved[3].size, (size_t)0);
	KUNIT_EXPECT_EQ(test, -EINVAL,
			apple_sart_remove_allowed_region(sart, SART_TEST_PG(0x800000),
							 SART_TEST_PG(0x400)));
	KUNIT_EXPECT_EQ(test, sart->saved[0].size, (size_t)SART_TEST_PG(0x400));

	sart_test_power_loss(t);
	if (!v3) {
		KUNIT_EXPECT_EQ(test, -EOPNOTSUPP, apple_sart_replay(sart));
		return;
	}
	KUNIT_EXPECT_EQ(test, 0, apple_sart_replay(sart));
	sart_test_check(test, t);
}

/* The check before a sleep exists on SARTv4 only, and needs the snapshot */
static void sart_test_sleep_validate_unsupported(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, test->param_value);
	struct apple_sart *sart = &t->sart;
	bool v4 = t->ver->ops == &sart_ops_v4;

	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x4000),
						      SART_TEST_PG(4)));
	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP, apple_sart_sleep_validate(sart));
	sart_test_snapshot(test, t);
	KUNIT_EXPECT_EQ(test, v4 ? 0 : -EOPNOTSUPP, apple_sart_sleep_validate(sart));
}

static struct sart_test *sart_test_validate_new(struct kunit *test)
{
	struct sart_test *t = sart_test_new(test, &sart_test_vers[3]);
	struct apple_sart *sart = &t->sart;

	sart_test_preload(t, 0, 0xff, 0x800000, 0x400);
	sart->protected_entries = BIT(0);
	KUNIT_ASSERT_EQ(test, 0,
			apple_sart_add_allowed_region(sart, SART_TEST_PG(0x4000),
						      SART_TEST_PG(4)));
	sart_test_model(t, 1, 0xff, 0x4000, 4);
	return t;
}

/* Entries that moved after the snapshot refuse the sleep */
static void sart_test_sleep_validate_changed(struct kunit *test)
{
	struct sart_test *t = sart_test_validate_new(test);
	struct apple_sart *sart = &t->sart;

	sart_test_snapshot(test, t);
	KUNIT_EXPECT_EQ(test, 0, apple_sart_sleep_validate(sart));

	/* Entry 1: address, size, flags; then an idle entry that appeared */
	sart_test_wr(t, 0x64, 0x4001);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));
	sart_test_wr(t, 0x64, 0x4000);
	KUNIT_EXPECT_EQ(test, 0, apple_sart_sleep_validate(sart));

	sart_test_wr(t, 0xc4, 5);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));
	sart_test_wr(t, 0xc4, 4);

	sart_test_wr(t, 0x04, 0x7f);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));
	sart_test_wr(t, 0x04, 0xff);

	sart_test_wr(t, 0x3c, 0xff);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));
	sart_test_wr(t, 0x3c, 0);

	KUNIT_EXPECT_EQ(test, 0, apple_sart_sleep_validate(sart));
	/* The check is read-only */
	sart_test_check(test, t);
	KUNIT_EXPECT_TRUE(test, sart->saved_valid);
}

/* Flags without a range: fine in an entry nobody uses, not in anyone's */
static void sart_test_sleep_validate_flags_only(struct kunit *test)
{
	struct sart_test *t = sart_test_validate_new(test);
	struct apple_sart *sart = &t->sart;

	sart_test_wr(t, 0x3c, 0xff);
	sart_test_wr(t, 0x14, 0x01);
	sart_test_snapshot(test, t);
	KUNIT_EXPECT_EQ(test, 0, apple_sart_sleep_validate(sart));

	/* The same in the bootloader's entry */
	sart->protected_entries |= BIT(15);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));
	sart->protected_entries &= ~BIT(15);

	/* The same in a driver's entry */
	sart->used_entries |= BIT(5);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));
	sart->used_entries &= ~BIT(5);
	KUNIT_EXPECT_EQ(test, 0, apple_sart_sleep_validate(sart));
}

/* Half an entry refuses the sleep, whoever's it is */
static void sart_test_sleep_validate_half_set(struct kunit *test)
{
	struct sart_test *t;
	struct apple_sart *sart;

	/* Idle: flags and an address, no size */
	t = sart_test_validate_new(test);
	sart = &t->sart;
	sart_test_wr(t, 0x14, 0xff);
	sart_test_wr(t, 0x74, 0x1234);
	sart_test_snapshot(test, t);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));

	/* Idle: a range, no flags */
	t = sart_test_validate_new(test);
	sart = &t->sart;
	sart_test_wr(t, 0x74, 0x1234);
	sart_test_wr(t, 0xd4, 0x10);
	sart_test_snapshot(test, t);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));

	/* A driver's: flags, no size */
	t = sart_test_validate_new(test);
	sart = &t->sart;
	sart_test_wr(t, 0xc4, 0);
	sart_test_snapshot(test, t);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));
}

/* An entry that is someone's and reads empty refuses the sleep */
static void sart_test_sleep_validate_in_use_empty(struct kunit *test)
{
	struct sart_test *t;
	struct apple_sart *sart;

	t = sart_test_validate_new(test);
	sart = &t->sart;
	sart_test_wr(t, 0x04, 0);
	sart_test_wr(t, 0x64, 0);
	sart_test_wr(t, 0xc4, 0);
	sart_test_snapshot(test, t);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));

	t = sart_test_validate_new(test);
	sart = &t->sart;
	sart_test_wr(t, 0x00, 0);
	sart_test_wr(t, 0x60, 0);
	sart_test_wr(t, 0xc0, 0);
	sart_test_snapshot(test, t);
	KUNIT_EXPECT_EQ(test, -ESTALE, apple_sart_sleep_validate(sart));
}
#endif /* CONFIG_SUSPEND */

static struct kunit_case sart_test_cases[] = {
	KUNIT_CASE(sart_test_add_encoding),
	KUNIT_CASE_PARAM(sart_test_add_fills_table, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_add_skips_protected, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_add_rejects_unaligned, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_add_size_max, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_high_paddr, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_get_entry, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_remove_frees_slot, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_remove_nonexistent, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_remove_protected, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_inherited_region, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_shutdown, sart_test_ver_gen_params),
	KUNIT_CASE(sart_test_of_match),
	KUNIT_CASE(sart_test_get_no_phandle),
#ifdef CONFIG_SUSPEND
	KUNIT_CASE_PARAM(sart_test_suspend_not_firmware, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_suspend_resume, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_resume_idle_entries, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_resume_last_in_use, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_replay, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_replay_after_remove, sart_test_ver_gen_params),
	KUNIT_CASE_PARAM(sart_test_sleep_validate_unsupported, sart_test_ver_gen_params),
	KUNIT_CASE(sart_test_sleep_validate_changed),
	KUNIT_CASE(sart_test_sleep_validate_flags_only),
	KUNIT_CASE(sart_test_sleep_validate_half_set),
	KUNIT_CASE(sart_test_sleep_validate_in_use_empty),
#endif
	{}
};

static struct kunit_suite sart_test_suite = {
	.name = "apple-sart",
	.test_cases = sart_test_cases,
};
kunit_test_suite(sart_test_suite);
