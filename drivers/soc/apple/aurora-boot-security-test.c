// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Aurora boot security record reader.
 *
 * Each case builds a boot record in the live device tree with an
 * of_changeset (a test node under /chosen, or the real
 * /chosen/aurora,boot-security for the init cases) and runs the driver's
 * own decision and sysfs show functions on it: which Stage 1 measurement
 * counts as enforcing, how the chain state is demoted, and what every
 * sysfs file prints for absent, empty, unterminated, short, long and
 * oversized properties. No hardware is touched.
 *
 * The init suite is built into the driver while .init.text is live.
 */

#include <kunit/test.h>
#include <linux/kernfs.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/vmalloc.h>

#include "aurora-boot-security-test.h"

#define BS_GUARD 0xa5

/* No property of this name: bs_meas() leaves it out. */
#define BS_ABSENT NULL

struct bs_ctx {
	struct of_changeset cs;
	struct device_node *np;
	bool applied;
	const char *old_chain_state;
	const char *old_lockdown_reason;
	bool old_stage1_enforced;
};

struct bs_meas_spec {
	const char *stage, *policy, *component, *verdict;
	const u8 *sha;
	int sha_len;		/* -1: no sha256 property */
};

static void bs_reset_globals(void)
{
	*aurora_bs_test.rec = NULL;
	*aurora_bs_test.chain_state = "none";
	*aurora_bs_test.stage1_enforced = false;
	*aurora_bs_test.lockdown_reason = "no";
}

/* A property with exactly @len bytes of @val, no terminator added. */
static void bs_raw(struct kunit *test, struct device_node *np,
		   const char *name, const void *val, int len)
{
	struct bs_ctx *ctx = test->priv;
	struct property *p = kzalloc(sizeof(*p), GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, p);
	p->name = kstrdup(name, GFP_KERNEL);
	/* Like an FDT property: a valid pointer even when empty. */
	p->value = kzalloc(len + 1, GFP_KERNEL);
	if (!p->name || !p->value) {
		kfree(p->value);
		kfree(p->name);
		kfree(p);
		KUNIT_FAIL_AND_ABORT(test, "could not allocate property %s", name);
	}
	memcpy(p->value, val, len);
	p->length = len;
	/* Freed with the node when the changeset is destroyed. */
	ret = of_changeset_add_property(&ctx->cs, np, p);
	if (ret) {
		kfree(p->value);
		kfree(p->name);
		kfree(p);
		KUNIT_FAIL_AND_ABORT(test, "could not add property %s: %d", name, ret);
	}

	/* The detached fixture node owns this even if apply is never called. */
	p->next = np->deadprops;
	np->deadprops = p;
}

static void bs_str(struct kunit *test, struct device_node *np,
		   const char *name, const char *s)
{
	struct bs_ctx *ctx = test->priv;

	if (!s)
		return;
	KUNIT_ASSERT_EQ(test, of_changeset_add_prop_string(&ctx->cs, np, name, s), 0);
}

static struct device_node *bs_create_node(struct kunit *test,
					  struct device_node *parent,
					  const char *name)
{
	struct bs_ctx *ctx = test->priv;
	struct device_node *np;

	np = of_changeset_create_node(&ctx->cs, parent, name);
	KUNIT_ASSERT_NOT_NULL(test, np);
	/* The changeset holds a reference until bs_exit(). */
	of_node_put(np);
	return np;
}

static struct device_node *bs_record(struct kunit *test,
				     struct device_node *parent,
				     const char *name, const char *compatible)
{
	struct bs_ctx *ctx = test->priv;

	ctx->np = bs_create_node(test, parent, name);
	bs_str(test, ctx->np, "compatible", compatible);
	return ctx->np;
}

/* Avoid adding platform devices while exercising the record reader. */
static struct device_node *bs_test_record(struct kunit *test)
{
	if (!of_chosen)
		kunit_skip(test, "no /chosen");
	return bs_record(test, of_chosen, "kunit-aurora-boot-security",
			 "aurora,boot-security-v1");
}

/*
 * Add the measurements in boot order. An attached node goes to the head
 * of its parent's child list, so create them last to first.
 */
static void bs_meas(struct kunit *test, const struct bs_meas_spec *m, int n)
{
	struct bs_ctx *ctx = test->priv;
	char name[24];
	int i;

	for (i = n - 1; i >= 0; i--) {
		struct device_node *c;

		snprintf(name, sizeof(name), "measurement@%d", i);
		c = bs_create_node(test, ctx->np, name);
		bs_str(test, c, "stage", m[i].stage);
		bs_str(test, c, "policy", m[i].policy);
		bs_str(test, c, "component", m[i].component);
		bs_str(test, c, "verdict", m[i].verdict);
		if (m[i].sha_len >= 0)
			bs_raw(test, c, "sha256", m[i].sha, m[i].sha_len);
	}
}

static void bs_apply(struct kunit *test)
{
	struct bs_ctx *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, of_changeset_apply(&ctx->cs), 0);
	ctx->applied = true;
	*aurora_bs_test.rec = ctx->np;
}

/* What aurora_boot_security_init() derives from the record. */
static void bs_decide(void)
{
	*aurora_bs_test.stage1_enforced = aurora_bs_test.stage1_enforced_verified();
	*aurora_bs_test.chain_state = aurora_bs_test.effective_state();
}

static ssize_t bs_show(struct kobj_attribute *attr, char *buf)
{
	return attr->show(NULL, attr, buf);
}

KUNIT_DEFINE_ACTION_WRAPPER(bs_vfree, vfree, const void *);

/* A page for a show function, with a second page for the canary. */
static char *bs_page(struct kunit *test)
{
	char *buf = vzalloc(2 * PAGE_SIZE);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, bs_vfree, buf), 0);
	KUNIT_ASSERT_TRUE(test, PAGE_ALIGNED(buf));
	memset(buf + PAGE_SIZE, BS_GUARD, PAGE_SIZE);
	return buf;
}

static void bs_expect_guard(struct kunit *test, const char *buf, size_t from,
			    size_t to)
{
	size_t i;

	for (i = from; i < to; i++)
		if (buf[i] != (char)BS_GUARD)
			break;
	KUNIT_EXPECT_EQ_MSG(test, i, to, "wrote past the buffer at %zu", i);
}

static int bs_init(struct kunit *test)
{
	struct bs_ctx *ctx;

	if (*aurora_bs_test.rec || *aurora_bs_test.aurora_kobj || *aurora_bs_test.bs_kobj)
		kunit_skip(test, "boot-security state is in use");
	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	of_changeset_init(&ctx->cs);
	test->priv = ctx;
	ctx->old_chain_state = *aurora_bs_test.chain_state;
	ctx->old_lockdown_reason = *aurora_bs_test.lockdown_reason;
	ctx->old_stage1_enforced = *aurora_bs_test.stage1_enforced;
	bs_reset_globals();
	return 0;
}

static void bs_exit(struct kunit *test)
{
	struct bs_ctx *ctx = test->priv;

	if (!ctx)
		return;
	test->priv = NULL;
	*aurora_bs_test.rec = NULL;
	*aurora_bs_test.chain_state = ctx->old_chain_state;
	*aurora_bs_test.lockdown_reason = ctx->old_lockdown_reason;
	*aurora_bs_test.stage1_enforced = ctx->old_stage1_enforced;
	if (ctx->applied)
		of_changeset_revert(&ctx->cs);
	of_changeset_destroy(&ctx->cs);
}

static const u8 bs_sha[33] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
	0x20,
};

#define BS_SHA_HEX "000102030405060708090a0b0c0d0e0f" \
		   "101112131415161718191a1b1c1d1e1f"

/* --- Which measurement makes the chain enforced ------------------------ */

static void bs_stage1_no_measurements(struct kunit *test)
{
	struct device_node *np = bs_test_record(test);

	bs_str(test, np, "state", "verified");
	bs_apply(test);
	bs_decide();
	KUNIT_EXPECT_FALSE(test, *aurora_bs_test.stage1_enforced);
	KUNIT_EXPECT_STREQ(test, *aurora_bs_test.chain_state, "partial");
}

/* One measurement without a hash. */
#define BS_M(stage, policy, verdict) { stage, policy, "next", verdict, NULL, -1 }

struct bs_one_case {
	const char *desc;
	struct bs_meas_spec m;
	bool enforced;
};

static const struct bs_one_case bs_one_cases[] = {
	{ "stage1 enforce verified", BS_M("stage1", "enforce", "verified"), true },
	{ "stage1 audit verified", BS_M("stage1", "audit", "verified"), false },
	{ "stage1 off verified", BS_M("stage1", "off", "verified"), false },
	{ "stage1 enforce test key", BS_M("stage1", "enforce", "test-key"), false },
	{ "stage1 enforce failed", BS_M("stage1", "enforce", "failed"), false },
	{ "stage1 enforce unverified", BS_M("stage1", "enforce", "unverified"), false },
	{ "stage1 enforce no verdict", BS_M("stage1", "enforce", BS_ABSENT), false },
	{ "stage1 enforce empty verdict", BS_M("stage1", "enforce", ""), false },
	{ "stage1 no policy verified", BS_M("stage1", BS_ABSENT, "verified"), false },
	{ "stage2 enforce verified", BS_M("stage2", "enforce", "verified"), false },
	{ "uboot enforce verified", BS_M("uboot", "enforce", "verified"), false },
	{ "no stage enforce verified", BS_M(BS_ABSENT, "enforce", "verified"), false },
	{ "empty stage enforce verified", BS_M("", "enforce", "verified"), false },
	{ "Stage1 Enforce Verified", BS_M("Stage1", "Enforce", "Verified"), false },
	{ "stage1x prefix", BS_M("stage1x", "enforce", "verified"), false },
	{ "stage1 enforced prefix", BS_M("stage1", "enforced", "verified"), false },
	{ "stage1 enforce verified-ish", BS_M("stage1", "enforce", "verified "), false },
};

KUNIT_ARRAY_PARAM_DESC(bs_one, bs_one_cases, desc);

static void bs_stage1_one_measurement(struct kunit *test)
{
	const struct bs_one_case *p = test->param_value;

	bs_test_record(test);
	bs_meas(test, &p->m, 1);
	bs_apply(test);
	KUNIT_EXPECT_EQ(test, aurora_bs_test.stage1_enforced_verified(), p->enforced);
}

/* Report the build flavour separately from the recorded verdict. */
static void bs_stage1_dev_enforce(struct kunit *test)
{
	const struct bs_meas_spec m = BS_M("stage1", "enforce", "verified");
	struct device_node *np = bs_test_record(test);
	char *buf = bs_page(test);

	bs_str(test, np, "state", "verified");
	bs_str(test, np, "stage1-flavour", "dev");
	bs_meas(test, &m, 1);
	bs_apply(test);
	bs_decide();

	KUNIT_EXPECT_TRUE(test, *aurora_bs_test.stage1_enforced);
	KUNIT_EXPECT_STREQ(test, *aurora_bs_test.chain_state, "verified");
	bs_show(aurora_bs_test.stage1_flavour, buf);
	KUNIT_EXPECT_STREQ(test, buf, "dev\n");
}

/* Any one enforcing, verified Stage 1 entry counts, wherever it is. */
static void bs_stage1_among_others(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "audit", "stage2", "verified", NULL, -1 },
		{ "stage2", "audit", "u-boot", "failed", NULL, -1 },
		{ "uboot", "off", "kernel", "measured", NULL, -1 },
		{ "stage1", "enforce", "stage2", "verified", NULL, -1 },
	};

	bs_test_record(test);
	bs_meas(test, m, ARRAY_SIZE(m));
	bs_apply(test);
	KUNIT_EXPECT_TRUE(test, aurora_bs_test.stage1_enforced_verified());
}

static void bs_stage1_none_among_many(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "audit", "stage2", "verified", NULL, -1 },
		{ "stage1", "enforce", "stage2", "failed", NULL, -1 },
		{ "stage2", "enforce", "u-boot", "verified", NULL, -1 },
		{ "uboot", "enforce", "kernel", "verified", NULL, -1 },
	};

	bs_test_record(test);
	bs_meas(test, m, ARRAY_SIZE(m));
	bs_apply(test);
	KUNIT_EXPECT_FALSE(test, aurora_bs_test.stage1_enforced_verified());
}

/* A string property without its terminator is not that string. */
static void bs_stage1_unterminated(struct kunit *test)
{
	static const char * const names[] = { "stage", "policy", "verdict" };
	static const char * const vals[] = { "stage1", "enforce", "verified" };
	struct bs_ctx *ctx;
	int bad, i;

	for (bad = -1; bad < (int)ARRAY_SIZE(names); bad++) {
		struct device_node *c;

		if (bad >= 0)
			KUNIT_ASSERT_EQ(test, bs_init(test), 0);
		ctx = test->priv;
		bs_test_record(test);
		c = bs_create_node(test, ctx->np, "measurement@0");
		for (i = 0; i < ARRAY_SIZE(names); i++)
			bs_raw(test, c, names[i], vals[i],
			       strlen(vals[i]) + (i != bad));
		bs_apply(test);
		KUNIT_EXPECT_EQ_MSG(test, aurora_bs_test.stage1_enforced_verified(), bad < 0,
				    "unterminated %s", bad < 0 ? "none" : names[bad]);
		bs_exit(test);
	}
}

/* Only the first string of a string list is read. */
static void bs_stage1_string_list(struct kunit *test)
{
	static const char lists[] = "audit\0enforce";
	struct bs_ctx *ctx = test->priv;
	struct device_node *c;

	bs_test_record(test);
	c = bs_create_node(test, ctx->np, "measurement@0");
	bs_str(test, c, "stage", "stage1");
	bs_raw(test, c, "policy", lists, sizeof(lists));
	bs_str(test, c, "verdict", "verified");
	bs_apply(test);
	KUNIT_EXPECT_FALSE(test, aurora_bs_test.stage1_enforced_verified());
}

/* --- The chain state ---------------------------------------------------- */

struct bs_state_case {
	const char *desc;
	const char *state;	/* NULL: no state property */
	int len;		/* bytes of @state to store; 0: with its NUL */
	bool enforced;
	const char *expect;	/* NULL: anything but "verified" */
};

static const struct bs_state_case bs_state_cases[] = {
	{ "absent, not enforced", NULL, 0, false, "unverified" },
	{ "absent, enforced", NULL, 0, true, "unverified" },
	{ "empty", "", 0, true, "unverified" },
	{ "zero length", "", -1, true, "unverified" },
	{ "verified, enforced", "verified", 0, true, "verified" },
	{ "verified, not enforced", "verified", 0, false, "partial" },
	{ "partial, enforced", "partial", 0, true, "partial" },
	{ "partial, not enforced", "partial", 0, false, "partial" },
	{ "unverified, enforced", "unverified", 0, true, "unverified" },
	{ "unterminated verified, enforced", "verified", 8, true, "unverified" },
	{ "unterminated verified, not enforced", "verified", 8, false, "unverified" },
	{ "truncated verifie, enforced", "verifie", 0, true, NULL },
	{ "Verified, not enforced", "Verified", 0, false, NULL },
	{ "verified space, not enforced", "verified ", 0, false, NULL },
	{ "verified newline, not enforced", "verified\n", 0, false, NULL },
	{ "verifiedx, not enforced", "verifiedx", 0, false, NULL },
};

KUNIT_ARRAY_PARAM_DESC(bs_state, bs_state_cases, desc);

static void bs_state_demotion(struct kunit *test)
{
	const struct bs_state_case *p = test->param_value;
	struct device_node *np = bs_test_record(test);

	if (p->state)
		bs_raw(test, np, "state", p->state,
		       p->len > 0 ? p->len :
		       p->len < 0 ? 0 : strlen(p->state) + 1);
	bs_apply(test);
	*aurora_bs_test.stage1_enforced = p->enforced;
	if (p->expect)
		KUNIT_EXPECT_STREQ(test, aurora_bs_test.effective_state(), p->expect);
	else
		KUNIT_EXPECT_STRNEQ(test, aurora_bs_test.effective_state(), "verified");
}

/* A record that names the second string of a list is not verified. */
static void bs_state_string_list(struct kunit *test)
{
	static const char list[] = "partial\0verified";
	struct device_node *np = bs_test_record(test);

	bs_raw(test, np, "state", list, sizeof(list));
	bs_apply(test);
	*aurora_bs_test.stage1_enforced = true;
	KUNIT_EXPECT_STREQ(test, aurora_bs_test.effective_state(), "partial");
}

static void bs_state_and_advisory_show(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "enforce", "stage2", "verified", NULL, -1 },
		{ "stage2", "enforce", "u-boot", "verified", NULL, -1 },
	};
	struct device_node *np = bs_test_record(test);
	char *buf = bs_page(test);

	bs_str(test, np, "state", "verified");
	bs_meas(test, m, ARRAY_SIZE(m));
	bs_apply(test);
	bs_decide();

	KUNIT_EXPECT_EQ(test, bs_show(aurora_bs_test.state, buf), 9);
	KUNIT_EXPECT_STREQ(test, buf, "verified\n");
	KUNIT_EXPECT_EQ(test, bs_show(aurora_bs_test.advisory, buf), 2);
	KUNIT_EXPECT_STREQ(test, buf, "0\n");
}

static void bs_state_and_advisory_show_audit(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "audit", "stage2", "verified", NULL, -1 },
		{ "stage2", "enforce", "u-boot", "verified", NULL, -1 },
	};
	struct device_node *np = bs_test_record(test);
	char *buf = bs_page(test);

	bs_str(test, np, "state", "verified");
	bs_meas(test, m, ARRAY_SIZE(m));
	bs_apply(test);
	bs_decide();

	bs_show(aurora_bs_test.state, buf);
	KUNIT_EXPECT_STREQ(test, buf, "partial\n");
	bs_show(aurora_bs_test.advisory, buf);
	KUNIT_EXPECT_STREQ(test, buf, "1\n");
}

/* Before a record is read (or without one) the files say so. */
static void bs_state_show_defaults(struct kunit *test)
{
	char *buf = bs_page(test);

	bs_show(aurora_bs_test.state, buf);
	KUNIT_EXPECT_STREQ(test, buf, "none\n");
	bs_show(aurora_bs_test.advisory, buf);
	KUNIT_EXPECT_STREQ(test, buf, "1\n");
	KUNIT_EXPECT_EQ(test, bs_show(aurora_bs_test.lockdown, buf), 3);
	KUNIT_EXPECT_STREQ(test, buf, "no\n");
}

/* --- policy ------------------------------------------------------------- */

static void bs_policy_show_case(struct kunit *test,
				const struct bs_meas_spec *m, int n,
				const char *expect)
{
	char *buf = bs_page(test);

	bs_test_record(test);
	bs_meas(test, m, n);
	bs_apply(test);
	KUNIT_EXPECT_EQ(test, bs_show(aurora_bs_test.policy, buf),
			(ssize_t)strlen(expect));
	KUNIT_EXPECT_STREQ(test, buf, expect);
}

static void bs_policy_show_empty(struct kunit *test)
{
	bs_policy_show_case(test, NULL, 0, "\n");
}

static void bs_policy_show_chain(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "enforce", "stage2", "verified", NULL, -1 },
		{ "stage2", "audit", "u-boot", "verified", NULL, -1 },
		{ "uboot", "off", "kernel", "measured", NULL, -1 },
	};

	bs_policy_show_case(test, m, ARRAY_SIZE(m),
			    "stage1 enforce, stage2 audit, uboot off\n");
}

/* A stage that measured several components is listed once. */
static void bs_policy_show_collapses_repeats(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "enforce", "stage2", "verified", NULL, -1 },
		{ "stage2", "audit", "u-boot", "verified", NULL, -1 },
		{ "stage2", "audit", "dtb", "verified", NULL, -1 },
		{ "uboot", "off", "kernel", "measured", NULL, -1 },
		{ "uboot", "off", "initrd", "measured", NULL, -1 },
	};

	bs_policy_show_case(test, m, ARRAY_SIZE(m),
			    "stage1 enforce, stage2 audit, uboot off\n");
}

/* Only consecutive repeats collapse: a stage that comes back is listed. */
static void bs_policy_show_returning_stage(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "enforce", "stage2", "verified", NULL, -1 },
		{ "stage2", "audit", "u-boot", "verified", NULL, -1 },
		{ "stage1", "enforce", "late", "verified", NULL, -1 },
	};

	bs_policy_show_case(test, m, ARRAY_SIZE(m),
			    "stage1 enforce, stage2 audit, stage1 enforce\n");
}

static void bs_policy_show_no_policy(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", BS_ABSENT, "stage2", "verified", NULL, -1 },
		{ "stage2", "", "u-boot", "verified", NULL, -1 },
	};

	bs_policy_show_case(test, m, ARRAY_SIZE(m), "stage1 , stage2\x20\n");
}

/* More stages than a page holds: truncated inside the page. */
static void bs_policy_show_bounded(struct kunit *test)
{
	struct bs_meas_spec *m;
	char *buf = bs_page(test);
	char *stage;
	ssize_t ret;
	int i, n = PAGE_SIZE / 64 + 8;

	m = kunit_kcalloc(test, n, sizeof(*m), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, m);
	for (i = 0; i < n; i++) {
		/* 100 distinct characters per stage: no two in a row match. */
		stage = kunit_kzalloc(test, 101, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, stage);
		memset(stage, 'a' + i % 2, 100);
		m[i] = (struct bs_meas_spec){ stage, "enforce", "c", "verified",
					      NULL, -1 };
	}
	bs_test_record(test);
	bs_meas(test, m, n);
	bs_apply(test);
	ret = bs_show(aurora_bs_test.policy, buf);
	KUNIT_EXPECT_EQ(test, ret, (ssize_t)PAGE_SIZE - 1);
	KUNIT_EXPECT_EQ(test, strlen(buf), (size_t)ret);
	bs_expect_guard(test, buf, PAGE_SIZE, 2 * PAGE_SIZE);
}

/* --- stage1_flavour, uefi_secure_boot ------------------------------------ */

struct bs_str_case {
	const char *desc;
	bool uefi;		/* uefi-secure-boot, else stage1-flavour */
	const char *val;	/* NULL: absent */
	int len;		/* 0: with its NUL, else bytes stored */
	const char *expect;
};

static const struct bs_str_case bs_str_cases[] = {
	{ "flavour absent", false, NULL, 0, "unknown\n" },
	{ "flavour empty", false, "", 0, "unknown\n" },
	{ "flavour zero length", false, "", -1, "unknown\n" },
	{ "flavour dev", false, "dev", 0, "dev\n" },
	{ "flavour secure", false, "secure", 0, "secure\n" },
	{ "flavour unterminated", false, "secure", 6, "unknown\n" },
	{ "flavour list", false, "dev\0secure", 11, "dev\n" },
	{ "uefi absent", true, NULL, 0, "unknown\n" },
	{ "uefi empty", true, "", 0, "unknown\n" },
	{ "uefi enforcing", true, "enforcing", 0, "enforcing\n" },
	{ "uefi audit", true, "audit", 0, "audit\n" },
	{ "uefi off", true, "off", 0, "off\n" },
	{ "uefi unterminated", true, "enforcing", 9, "unknown\n" },
};

KUNIT_ARRAY_PARAM_DESC(bs_str, bs_str_cases, desc);

static void bs_string_show(struct kunit *test)
{
	const struct bs_str_case *p = test->param_value;
	const char *name = p->uefi ? "uefi-secure-boot" : "stage1-flavour";
	struct device_node *np = bs_test_record(test);
	char *buf = bs_page(test);
	ssize_t ret;

	if (p->val)
		bs_raw(test, np, name, p->val,
		       p->len > 0 ? p->len :
		       p->len < 0 ? 0 : strlen(p->val) + 1);
	bs_apply(test);
	if (p->uefi)
		ret = bs_show(aurora_bs_test.uefi_secure_boot, buf);
	else
		ret = bs_show(aurora_bs_test.stage1_flavour, buf);
	KUNIT_EXPECT_EQ(test, ret, (ssize_t)strlen(p->expect));
	KUNIT_EXPECT_STREQ(test, buf, p->expect);
}

/* A page-long flavour is cut inside the page. */
static void bs_string_show_long(struct kunit *test)
{
	struct device_node *np = bs_test_record(test);
	char *buf = bs_page(test);
	char *val = vzalloc(2 * PAGE_SIZE);

	KUNIT_ASSERT_NOT_NULL(test, val);
	memset(val, 'x', 2 * PAGE_SIZE - 1);
	bs_raw(test, np, "stage1-flavour", val, 2 * PAGE_SIZE);
	bs_apply(test);
	KUNIT_EXPECT_EQ(test, bs_show(aurora_bs_test.stage1_flavour, buf),
			(ssize_t)PAGE_SIZE - 1);
	bs_expect_guard(test, buf, PAGE_SIZE, 2 * PAGE_SIZE);
}

/* --- lp_coih ------------------------------------------------------------ */

struct bs_coih_case {
	const char *desc;
	int len;		/* -1: absent */
	int hex_bytes;		/* bytes printed; 0: "unknown" */
};

static const struct bs_coih_case bs_coih_cases[] = {
	{ "absent", -1, 0 },
	{ "empty", 0, 0 },
	{ "one byte", 1, 1 },
	{ "four bytes", 4, 4 },
	{ "sha384 size", 48, 48 },
	{ "64 bytes", 64, 64 },
	{ "65 bytes", 65, 64 },
	{ "200 bytes", 200, 64 },
};

KUNIT_ARRAY_PARAM_DESC(bs_coih, bs_coih_cases, desc);

static void bs_lp_coih_show(struct kunit *test)
{
	const struct bs_coih_case *p = test->param_value;
	struct device_node *np = bs_test_record(test);
	char *buf = bs_page(test);
	char *expect;
	u8 *val;
	int i;

	expect = kunit_kzalloc(test, 2 * 64 + 2, GFP_KERNEL);
	val = kunit_kzalloc(test, max(p->len, 1), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, expect);
	KUNIT_ASSERT_NOT_NULL(test, val);
	for (i = 0; i < p->len; i++)
		val[i] = 0xc0 + i;
	if (p->len >= 0)
		bs_raw(test, np, "apple,lp-coih", val, p->len);
	bs_apply(test);

	if (p->hex_bytes) {
		for (i = 0; i < p->hex_bytes; i++)
			snprintf(expect + 2 * i, 3, "%02x", val[i]);
		strcat(expect, "\n");
	} else {
		strscpy(expect, "unknown\n", 2 * 64 + 2);
	}
	KUNIT_EXPECT_EQ(test, bs_show(aurora_bs_test.lp_coih, buf),
			(ssize_t)strlen(expect));
	KUNIT_EXPECT_STREQ(test, buf, expect);
}

/* --- measurements ------------------------------------------------------- */

static void bs_measurements_show_case(struct kunit *test,
				      const struct bs_meas_spec *m, int n,
				      const char *expect)
{
	char *buf = bs_page(test);

	bs_test_record(test);
	bs_meas(test, m, n);
	bs_apply(test);
	KUNIT_EXPECT_EQ(test, bs_show(aurora_bs_test.measurements, buf),
			(ssize_t)strlen(expect));
	KUNIT_EXPECT_STREQ(test, buf, expect);
}

static void bs_measurements_none(struct kunit *test)
{
	bs_measurements_show_case(test, NULL, 0, "");
}

static void bs_measurements_chain(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "enforce", "stage2", "verified", bs_sha, 32 },
		{ "stage2", "audit", "u-boot", "verified", bs_sha, 32 },
		{ "uboot", "off", "kernel", "measured", bs_sha, 32 },
	};
	static const char expect[] =
		"stage1 enforce stage2 verified " BS_SHA_HEX "\n"
		"stage2 audit u-boot verified " BS_SHA_HEX "\n"
		"uboot off kernel measured " BS_SHA_HEX "\n";

	bs_measurements_show_case(test, m, ARRAY_SIZE(m), expect);
}

/* Only a 32-byte sha256 is printed; any other length prints no hash. */
static void bs_measurements_bad_hash(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "enforce", "short", "verified", bs_sha, 31 },
		{ "stage1", "enforce", "long", "verified", bs_sha, 33 },
		{ "stage1", "enforce", "empty", "verified", bs_sha, 0 },
		{ "stage1", "enforce", "one", "verified", bs_sha, 1 },
		{ "stage1", "enforce", "absent", "verified", NULL, -1 },
		{ "stage1", "enforce", "good", "verified", bs_sha, 32 },
	};
	static const char expect[] =
		"stage1 enforce short verified\x20\n"
		"stage1 enforce long verified\x20\n"
		"stage1 enforce empty verified\x20\n"
		"stage1 enforce one verified\x20\n"
		"stage1 enforce absent verified\x20\n"
		"stage1 enforce good verified " BS_SHA_HEX "\n";

	bs_measurements_show_case(test, m, ARRAY_SIZE(m), expect);
}

static void bs_measurements_missing_fields(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ BS_ABSENT, BS_ABSENT, BS_ABSENT, BS_ABSENT, NULL, -1 },
		{ "stage2", BS_ABSENT, "u-boot", BS_ABSENT, bs_sha, 32 },
	};
	static const char expect[] =
		"   \x20\n"
		"stage2  u-boot  " BS_SHA_HEX "\n";

	bs_measurements_show_case(test, m, ARRAY_SIZE(m), expect);
}

/* format_measurements() never writes past the size it is given. */
static void bs_format_measurements_bounded(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "enforce", "stage2", "verified", bs_sha, 32 },
		{ "stage2", "audit", "u-boot", "verified", bs_sha, 32 },
	};
	static const size_t sizes[] = { 1, 2, 16, 95, 96, 97, 98, 190, 191, 300 };
	static const char full[] =
		"stage1 enforce stage2 verified " BS_SHA_HEX "\n"
		"stage2 audit u-boot verified " BS_SHA_HEX "\n";
	char *buf = kunit_kzalloc(test, 512, GFP_KERNEL);
	ssize_t ret;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	bs_test_record(test);
	bs_meas(test, m, ARRAY_SIZE(m));
	bs_apply(test);

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		size_t sz = sizes[i], want = min(sz - 1, strlen(full));

		memset(buf, BS_GUARD, 512);
		ret = aurora_bs_test.format_measurements(buf, sz);
		KUNIT_EXPECT_EQ_MSG(test, ret, (ssize_t)want, "size %zu", sz);
		KUNIT_EXPECT_EQ_MSG(test, buf[want], 0, "size %zu", sz);
		/* A cut first line leaves no room for the second. */
		if (sz <= 97)
			KUNIT_EXPECT_EQ_MSG(test, strncmp(buf, full, want), 0,
					    "size %zu", sz);
		bs_expect_guard(test, buf, sz, 512);
	}
}

/* Far more measurements than a page holds. */
static void bs_measurements_show_bounded(struct kunit *test)
{
	static const struct bs_meas_spec one = {
		"stage1", "enforce", "stage2", "verified", bs_sha, 32
	};
	struct bs_meas_spec *m;
	char *buf = bs_page(test);
	ssize_t ret;
	int i, n = PAGE_SIZE / 64 + 8;

	m = kunit_kcalloc(test, n, sizeof(*m), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, m);
	for (i = 0; i < n; i++)
		m[i] = one;
	bs_test_record(test);
	bs_meas(test, m, n);
	bs_apply(test);
	ret = bs_show(aurora_bs_test.measurements, buf);
	KUNIT_EXPECT_EQ(test, ret, (ssize_t)PAGE_SIZE - 1);
	KUNIT_EXPECT_EQ(test, strlen(buf), (size_t)ret);
	bs_expect_guard(test, buf, PAGE_SIZE, 2 * PAGE_SIZE);
}

/* The IMA text is built from the same fields; without IMA a no-op. */
static void bs_measure_record_full_page(struct kunit *test)
{
	static const struct bs_meas_spec one = {
		"stage1", "enforce", "stage2", "verified", bs_sha, 32
	};
	struct bs_meas_spec *m;
	int i, n = PAGE_SIZE / 64 + 8;

	if (IS_ENABLED(CONFIG_IMA))
		kunit_skip(test, "IMA is enabled");
	m = kunit_kcalloc(test, n, sizeof(*m), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, m);
	for (i = 0; i < n; i++)
		m[i] = one;
	bs_test_record(test);
	bs_meas(test, m, n);
	bs_apply(test);
	bs_decide();
	aurora_bs_test.measure_record();
}

static struct kunit_case bs_cases[] = {
	KUNIT_CASE(bs_stage1_no_measurements),
	KUNIT_CASE_PARAM(bs_stage1_one_measurement, bs_one_gen_params),
	KUNIT_CASE(bs_stage1_dev_enforce),
	KUNIT_CASE(bs_stage1_among_others),
	KUNIT_CASE(bs_stage1_none_among_many),
	KUNIT_CASE(bs_stage1_unterminated),
	KUNIT_CASE(bs_stage1_string_list),
	KUNIT_CASE_PARAM(bs_state_demotion, bs_state_gen_params),
	KUNIT_CASE(bs_state_string_list),
	KUNIT_CASE(bs_state_and_advisory_show),
	KUNIT_CASE(bs_state_and_advisory_show_audit),
	KUNIT_CASE(bs_state_show_defaults),
	KUNIT_CASE(bs_policy_show_empty),
	KUNIT_CASE(bs_policy_show_chain),
	KUNIT_CASE(bs_policy_show_collapses_repeats),
	KUNIT_CASE(bs_policy_show_returning_stage),
	KUNIT_CASE(bs_policy_show_no_policy),
	KUNIT_CASE(bs_policy_show_bounded),
	KUNIT_CASE_PARAM(bs_string_show, bs_str_gen_params),
	KUNIT_CASE(bs_string_show_long),
	KUNIT_CASE_PARAM(bs_lp_coih_show, bs_coih_gen_params),
	KUNIT_CASE(bs_measurements_none),
	KUNIT_CASE(bs_measurements_chain),
	KUNIT_CASE(bs_measurements_bad_hash),
	KUNIT_CASE(bs_measurements_missing_fields),
	KUNIT_CASE(bs_format_measurements_bounded),
	KUNIT_CASE(bs_measurements_show_bounded),
	KUNIT_CASE(bs_measure_record_full_page),
	{}
};

static struct kunit_suite bs_suite = {
	.name = "aurora-boot-security",
	.init = bs_init,
	.exit = bs_exit,
	.test_cases = bs_cases,
};

kunit_test_suite(bs_suite);

#if IS_BUILTIN(CONFIG_APPLE_AURORA_BOOT_SECURITY_KUNIT_TEST)
/* --- The init path, on the real /chosen/aurora,boot-security ----------- */

static int bs_init_init(struct kunit *test)
{
	struct device_node *np;
	/* Never lock down the test kernel. */
	if (IS_ENABLED(CONFIG_APPLE_AURORA_BOOT_SECURITY_LOCKDOWN))
		kunit_skip(test, "would lock down the kernel");
	if (!of_chosen)
		kunit_skip(test, "no /chosen");
	np = of_find_node_by_path("/chosen/aurora,boot-security");
	of_node_put(np);
	if (np)
		kunit_skip(test, "this kernel booted with a record");
	return bs_init(test);
}

/* What aurora_boot_security_exit() undoes, which is gone when built in. */
static void bs_init_exit(struct kunit *test)
{
	if (!test->priv)
		return;
	if (bs_kobj) {
		sysfs_remove_groups(bs_kobj, bs_groups);
		kobject_put(bs_kobj);
		bs_kobj = NULL;
	}
	kobject_put(aurora_kobj);
	aurora_kobj = NULL;
	of_node_put(rec);
	bs_exit(test);
}

static struct device_node *bs_chosen_record(struct kunit *test,
					    const char *compatible)
{
	return bs_record(test, of_chosen, "aurora,boot-security", compatible);
}

static void bs_init_apply(struct kunit *test)
{
	struct bs_ctx *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, of_changeset_apply(&ctx->cs), 0);
	ctx->applied = true;
}

static int __init bs_run_driver_init(void)
{
	int ret = aurora_boot_security_init();

	if (ret) {
		aurora_kobj = NULL;
		bs_kobj = NULL;
	}
	return ret;
}

static void __init bs_init_no_record(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, bs_run_driver_init(), -ENODEV);
	KUNIT_EXPECT_NULL(test, rec);
	KUNIT_EXPECT_NULL(test, aurora_kobj);
	KUNIT_EXPECT_STREQ(test, chain_state, "none");
	KUNIT_EXPECT_FALSE(test, stage1_enforced);
}

/* A record that is not v1 is ignored, its claims and all. */
static void __init bs_init_wrong_compatible(struct kunit *test)
{
	static const char * const compats[] = {
		NULL, "", "aurora,boot-security-v2", "aurora,boot-security",
	};
	static const struct bs_meas_spec m[] = {
		{ "stage1", "enforce", "stage2", "verified", NULL, -1 },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(compats); i++) {
		struct device_node *np;

		np = bs_chosen_record(test, compats[i]);
		bs_str(test, np, "state", "verified");
		bs_meas(test, m, ARRAY_SIZE(m));
		bs_init_apply(test);
		KUNIT_EXPECT_EQ_MSG(test, bs_run_driver_init(), -ENODEV,
				    "compatible %s", compats[i] ?: "absent");
		KUNIT_EXPECT_NULL(test, rec);
		KUNIT_EXPECT_NULL(test, aurora_kobj);
		KUNIT_EXPECT_STREQ(test, chain_state, "none");
		KUNIT_EXPECT_FALSE(test, stage1_enforced);
		/* The node's reference was dropped: the revert frees it. */
		bs_exit(test);
		KUNIT_ASSERT_EQ(test, bs_init(test), 0);
	}
}

static void __init bs_init_check_files(struct kunit *test)
{
	static const char * const files[] = {
		"state", "advisory", "policy", "stage1_flavour",
		"uefi_secure_boot", "lockdown", "lp_coih", "measurements",
	};
	struct kernfs_node *kn;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, aurora_kobj);
	KUNIT_ASSERT_NOT_NULL(test, bs_kobj);
	KUNIT_EXPECT_PTR_EQ(test, aurora_kobj->parent, firmware_kobj);
	KUNIT_EXPECT_STREQ(test, kobject_name(aurora_kobj), "aurora");
	KUNIT_EXPECT_STREQ(test, kobject_name(bs_kobj), "boot_security");
	for (i = 0; i < ARRAY_SIZE(files); i++) {
		kn = kernfs_find_and_get(bs_kobj->sd, files[i]);
		KUNIT_EXPECT_NOT_NULL_MSG(test, kn, "no %s", files[i]);
		kernfs_put(kn);
	}
}

static void __init bs_init_verified(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "enforce", "stage2", "verified", bs_sha, 32 },
		{ "stage2", "enforce", "u-boot", "verified", bs_sha, 32 },
	};
	struct bs_ctx *ctx = test->priv;
	struct device_node *np = bs_chosen_record(test, "aurora,boot-security-v1");
	char *buf = bs_page(test);

	bs_str(test, np, "state", "verified");
	bs_str(test, np, "stage1-flavour", "secure");
	bs_str(test, np, "uefi-secure-boot", "audit");
	bs_meas(test, m, ARRAY_SIZE(m));
	bs_init_apply(test);

	KUNIT_ASSERT_EQ(test, bs_run_driver_init(), 0);
	KUNIT_EXPECT_PTR_EQ(test, rec, ctx->np);
	KUNIT_EXPECT_TRUE(test, stage1_enforced);
	KUNIT_EXPECT_STREQ(test, chain_state, "verified");
	KUNIT_EXPECT_STREQ(test, lockdown_reason, "no");
	bs_init_check_files(test);

	state_attr.show(bs_kobj, &state_attr, buf);
	KUNIT_EXPECT_STREQ(test, buf, "verified\n");
	advisory_attr.show(bs_kobj, &advisory_attr, buf);
	KUNIT_EXPECT_STREQ(test, buf, "0\n");
	policy_attr.show(bs_kobj, &policy_attr, buf);
	KUNIT_EXPECT_STREQ(test, buf, "stage1 enforce, stage2 enforce\n");
	stage1_flavour_attr.show(bs_kobj, &stage1_flavour_attr, buf);
	KUNIT_EXPECT_STREQ(test, buf, "secure\n");
	uefi_secure_boot_attr.show(bs_kobj, &uefi_secure_boot_attr, buf);
	KUNIT_EXPECT_STREQ(test, buf, "audit\n");
}

/* The record claims verified, but Stage 1 only audited. */
static void __init bs_init_claims_verified(struct kunit *test)
{
	static const struct bs_meas_spec m[] = {
		{ "stage1", "audit", "stage2", "verified", NULL, -1 },
		{ "stage2", "enforce", "u-boot", "verified", NULL, -1 },
	};
	struct device_node *np = bs_chosen_record(test, "aurora,boot-security-v1");
	char *buf = bs_page(test);

	bs_str(test, np, "state", "verified");
	bs_meas(test, m, ARRAY_SIZE(m));
	bs_init_apply(test);

	KUNIT_ASSERT_EQ(test, bs_run_driver_init(), 0);
	KUNIT_EXPECT_FALSE(test, stage1_enforced);
	KUNIT_EXPECT_STREQ(test, chain_state, "partial");
	bs_init_check_files(test);
	state_attr.show(bs_kobj, &state_attr, buf);
	KUNIT_EXPECT_STREQ(test, buf, "partial\n");
	advisory_attr.show(bs_kobj, &advisory_attr, buf);
	KUNIT_EXPECT_STREQ(test, buf, "1\n");
	stage1_flavour_attr.show(bs_kobj, &stage1_flavour_attr, buf);
	KUNIT_EXPECT_STREQ(test, buf, "unknown\n");
}

/* v1 later in the compatible list; an empty record is "unverified". */
static void __init bs_init_compatible_list(struct kunit *test)
{
	static const char compat[] = "vendor,other\0aurora,boot-security-v1";
	struct device_node *np = bs_chosen_record(test, NULL);
	char *buf = bs_page(test);

	bs_raw(test, np, "compatible", compat, sizeof(compat));
	bs_init_apply(test);

	KUNIT_ASSERT_EQ(test, bs_run_driver_init(), 0);
	KUNIT_EXPECT_NOT_NULL(test, rec);
	KUNIT_EXPECT_STREQ(test, chain_state, "unverified");
	bs_init_check_files(test);
	policy_attr.show(bs_kobj, &policy_attr, buf);
	KUNIT_EXPECT_STREQ(test, buf, "\n");
	/* Nothing to list: nothing written, not even a newline. */
	KUNIT_EXPECT_EQ(test, measurements_attr.show(bs_kobj, &measurements_attr, buf), 0);
	lp_coih_attr.show(bs_kobj, &lp_coih_attr, buf);
	KUNIT_EXPECT_STREQ(test, buf, "unknown\n");
}

static struct kunit_case __refdata bs_init_cases[] = {
	KUNIT_CASE(bs_init_no_record),
	KUNIT_CASE(bs_init_wrong_compatible),
	KUNIT_CASE(bs_init_verified),
	KUNIT_CASE(bs_init_claims_verified),
	KUNIT_CASE(bs_init_compatible_list),
	{}
};

static struct kunit_suite bs_init_suite = {
	.name = "aurora-boot-security-init",
	.init = bs_init_init,
	.exit = bs_init_exit,
	.test_cases = bs_init_cases,
};

kunit_test_init_section_suites(&bs_init_suite);
#else
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("Dual MIT/GPL");
#endif
