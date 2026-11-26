// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

/*
 * The transmitter procedure against a fake receiver (its HDCP 2.2 register
 * map) and a fake SEP. Included from dcp-hdcp-tx.c.
 */

#include <kunit/test.h>

#define FAKE_REG_BASE	DP_HDCP_2_2_REG_RTX_OFFSET
#define FAKE_REG_SIZE	0x600
#define FAKE_LOG	64

struct fake_write {
	u32 addr;
	size_t len;
	u8 first;
};

struct fake {
	struct kunit *test;
	u8 regs[FAKE_REG_SIZE];

	struct fake_write writes[FAKE_LOG];
	unsigned int nwrites;
	enum dcp_hdcp_progress_code reports[FAKE_LOG];
	unsigned int nreports;
	char sep[FAKE_LOG];		/* one letter per SEP call */
	unsigned int nsep;
	unsigned int slept_ms;

	unsigned long events;		/* BIT(op): DCP sends it */
	int cancel_op;			/* -1, or the op whose wait is cancelled */
	bool stored;
	int revoked_ids;		/* returned by revoked() */
	unsigned int m_rejects;		/* M' checks the SEP fails first */
	unsigned int l_rejects;		/* L' checks the SEP fails first */
	bool sep_busy;			/* the SEP is not available */
	bool encryption;
	bool closed;
};

static int fake_read(void *ctx, u32 addr, void *buf, size_t len)
{
	struct fake *f = ctx;

	if (addr < FAKE_REG_BASE || addr + len > FAKE_REG_BASE + FAKE_REG_SIZE)
		return -EIO;
	memcpy(buf, f->regs + addr - FAKE_REG_BASE, len);
	return 0;
}

static int fake_write(void *ctx, u32 addr, const void *buf, size_t len)
{
	struct fake *f = ctx;

	if (addr < FAKE_REG_BASE || addr + len > FAKE_REG_BASE + FAKE_REG_SIZE)
		return -EIO;
	memcpy(f->regs + addr - FAKE_REG_BASE, buf, len);
	if (f->nwrites < FAKE_LOG)
		f->writes[f->nwrites++] = (struct fake_write){ addr, len,
							      ((u8 *)buf)[0] };
	return 0;
}

static int fake_wait(void *ctx, enum dcp_hdcp_auth_op op, unsigned int ms)
{
	struct fake *f = ctx;

	if (f->cancel_op == op)
		return -ECANCELED;
	return (f->events & BIT(op)) ? 0 : -ETIMEDOUT;
}

static void fake_report(void *ctx, enum dcp_hdcp_progress_code code,
			bool active, bool repeater, bool encrypted)
{
	struct fake *f = ctx;

	if (f->nreports < FAKE_LOG)
		f->reports[f->nreports++] = code;
}

static int fake_revoked(void *ctx, u8 *ids, unsigned int count)
{
	return ((struct fake *)ctx)->revoked_ids;
}

static void fake_sleep(void *ctx, unsigned int ms)
{
	((struct fake *)ctx)->slept_ms += ms;
}

static void sep_log(struct fake *f, char c)
{
	if (f->nsep < FAKE_LOG - 1)
		f->sep[f->nsep++] = c;
}

static int fake_sep_open(void *ctx, u32 ch, u8 *rtx)
{
	if (((struct fake *)ctx)->sep_busy)
		return -EAGAIN;
	sep_log(ctx, 'o');
	memset(rtx, 0xa1, HDCP_2_2_RTX_LEN);
	return 0;
}

static int fake_sep_ake_cert(void *ctx, u32 ch, const u8 *cert, const u8 *rrx,
			     bool *stored, u8 *ekh_km_m, u8 *ekpub_km)
{
	struct fake *f = ctx;

	sep_log(f, 'c');
	*stored = f->stored;
	memset(ekh_km_m, 0xb2, HDCP_2_2_E_KH_KM_M_LEN);
	memset(ekpub_km, 0xc3, HDCP_2_2_E_KPUB_KM_LEN);
	return 0;
}

static int fake_sep_h_prime(void *ctx, u32 ch, const u8 *rxcaps,
			    const u8 *txcaps, const u8 *h_prime)
{
	struct fake *f = ctx;

	sep_log(f, 'h');
	/* H' covers RxCaps and TxCaps as the receiver sent and got them. */
	KUNIT_EXPECT_EQ(f->test, txcaps[0], HDCP_2_2_RX_CAPS_VERSION_VAL);
	KUNIT_EXPECT_EQ(f->test, rxcaps[0], HDCP_2_2_RX_CAPS_VERSION_VAL);
	return 0;
}

static int fake_sep_pairing(void *ctx, u32 ch, const u8 *ekh_km)
{
	sep_log(ctx, 'p');
	return 0;
}

static int fake_sep_lc_init(void *ctx, u32 ch, u8 *rn)
{
	sep_log(ctx, 'l');
	memset(rn, 0xd4, HDCP_2_2_RN_LEN);
	return 0;
}

static int fake_sep_l_prime(void *ctx, u32 ch, const u8 *l_prime)
{
	struct fake *f = ctx;

	sep_log(f, 'L');
	if (f->l_rejects) {
		f->l_rejects--;
		return -EACCES;
	}
	return 0;
}

static int fake_sep_ske(void *ctx, u32 ch, u8 *edkey_ks, u8 *riv)
{
	sep_log(ctx, 's');
	memset(edkey_ks, 0xe5, HDCP_2_2_E_DKEY_KS_LEN);
	memset(riv, 0xf6, HDCP_2_2_RIV_LEN);
	return 0;
}

static int fake_sep_list(void *ctx, u32 ch, const u8 *ids, unsigned int count,
			 const u8 *rxinfo, const u8 *seq_num_v,
			 const u8 *v_prime, u8 *v)
{
	sep_log(ctx, 'r');
	memset(v, 0x11, HDCP_2_2_V_PRIME_HALF_LEN);
	memset(v + HDCP_2_2_V_PRIME_HALF_LEN, 0x22, HDCP_2_2_V_PRIME_HALF_LEN);
	return 0;
}

static int fake_sep_stream(void *ctx, u32 ch, u8 stream_id, u8 type,
			   const u8 *seq_num_m, const u8 *m_prime)
{
	struct fake *f = ctx;

	sep_log(f, 'm');
	if (f->m_rejects) {
		f->m_rejects--;
		return -EACCES;
	}
	return 0;
}

static int fake_sep_finish(void *ctx, u32 ch)
{
	sep_log(ctx, 'f');
	return 0;
}

static int fake_sep_set_encryption(void *ctx, u32 ch, bool on)
{
	struct fake *f = ctx;

	sep_log(f, on ? 'E' : 'e');
	f->encryption = on;
	return 0;
}

static void fake_sep_close(void *ctx, u32 ch)
{
	struct fake *f = ctx;

	sep_log(f, 'x');
	f->closed = true;
}

static const struct dcp_hdcp_tx_ops fake_ops = {
	.read = fake_read,
	.write = fake_write,
	.wait = fake_wait,
	.report = fake_report,
	.revoked = fake_revoked,
	.sleep = fake_sleep,
	.sep_open = fake_sep_open,
	.sep_ake_cert = fake_sep_ake_cert,
	.sep_h_prime = fake_sep_h_prime,
	.sep_pairing = fake_sep_pairing,
	.sep_lc_init = fake_sep_lc_init,
	.sep_l_prime = fake_sep_l_prime,
	.sep_ske = fake_sep_ske,
	.sep_repeater_list = fake_sep_list,
	.sep_repeater_stream = fake_sep_stream,
	.sep_finish = fake_sep_finish,
	.sep_set_encryption = fake_sep_set_encryption,
	.sep_close = fake_sep_close,
};

static u8 *reg(struct fake *f, u32 addr)
{
	return f->regs + addr - FAKE_REG_BASE;
}

/* A receiver that does HDCP 2.2, a repeater or not. */
static void fake_receiver(struct fake *f, bool repeater)
{
	u8 *caps = reg(f, DP_HDCP_2_2_REG_RX_CAPS_OFFSET);

	caps[0] = HDCP_2_2_RX_CAPS_VERSION_VAL;
	caps[1] = 0;
	caps[2] = BIT(1) | (repeater ? BIT(0) : 0);
	memset(reg(f, DP_HDCP_2_2_REG_CERT_RX_OFFSET), 0x5a,
	       sizeof(struct hdcp2_cert_rx));
	f->events = BIT(DCP_HDCP_AUTH_H_PRIME) | BIT(DCP_HDCP_AUTH_PAIRING);
	if (repeater) {
		u8 *rxinfo = reg(f, DP_HDCP_2_2_REG_RXINFO_OFFSET);

		/* Depth 1, one device. */
		rxinfo[0] = 0x02;
		rxinfo[1] = 0x10;
		f->events |= BIT(DCP_HDCP_AUTH_LIST);
	}
}

static struct fake *fake_new(struct kunit *test, struct dcp_hdcp_tx *tx,
			     bool repeater, u8 type)
{
	struct fake *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->test = test;
	f->cancel_op = -1;
	fake_receiver(f, repeater);
	memset(tx, 0, sizeof(*tx));
	tx->ops = &fake_ops;
	tx->ctx = f;
	tx->channel = 7;
	tx->type = type;
	return f;
}

static bool wrote(struct fake *f, u32 addr, size_t len)
{
	unsigned int i;

	for (i = 0; i < f->nwrites; i++)
		if (f->writes[i].addr == addr && f->writes[i].len == len)
			return true;
	return false;
}

static bool reported(struct fake *f, enum dcp_hdcp_progress_code code)
{
	unsigned int i;

	for (i = 0; i < f->nreports; i++)
		if (f->reports[i] == code)
			return true;
	return false;
}

static void tx_stored_km_type0(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f = fake_new(test, &tx, false, 0);

	f->stored = true;
	KUNIT_ASSERT_EQ(test, dcp_hdcp_authenticate(&tx), 0);
	KUNIT_EXPECT_STREQ(test, f->sep, "ochlLsfE");
	KUNIT_EXPECT_TRUE(test, wrote(f, DP_HDCP_2_2_REG_RTX_OFFSET, 8));
	KUNIT_EXPECT_TRUE(test, wrote(f, DP_HDCP_2_2_REG_TXCAPS_OFFSET, 3));
	KUNIT_EXPECT_TRUE(test, wrote(f, DP_HDCP_2_2_REG_EKH_KM_WR_OFFSET, 32));
	KUNIT_EXPECT_FALSE(test, wrote(f, DP_HDCP_2_2_REG_EKPUB_KM_OFFSET, 128));
	KUNIT_EXPECT_TRUE(test, wrote(f, DP_HDCP_2_2_REG_RN_OFFSET, 8));
	KUNIT_EXPECT_TRUE(test, wrote(f, DP_HDCP_2_2_REG_EDKEY_KS_OFFSET, 16));
	KUNIT_EXPECT_TRUE(test, wrote(f, DP_HDCP_2_2_REG_RIV_OFFSET, 8));
	KUNIT_EXPECT_TRUE(test, wrote(f, DP_HDCP_2_2_REG_STREAM_TYPE_OFFSET, 1));
	KUNIT_EXPECT_EQ(test, *reg(f, DP_HDCP_2_2_REG_STREAM_TYPE_OFFSET), 0);
	KUNIT_EXPECT_FALSE(test, reported(f, DCP_HDCP_PROGRESS_H_VERIFIED));
	KUNIT_EXPECT_EQ(test, f->reports[f->nreports - 1],
			DCP_HDCP_PROGRESS_ENCRYPTED);
	/* L' after 16 ms, encryption 200 ms after the keys. */
	KUNIT_EXPECT_EQ(test, f->slept_ms, HDCP_2_2_DP_LPRIME_TIMEOUT_MS +
			HDCP_2_2_DELAY_BEFORE_ENCRYPTION_EN);
	KUNIT_EXPECT_TRUE(test, f->encryption);
	KUNIT_EXPECT_FALSE(test, tx.repeater);

	dcp_hdcp_abort(&tx);
	KUNIT_EXPECT_FALSE(test, f->encryption);
	KUNIT_EXPECT_TRUE(test, f->closed);
}

static void tx_first_pairing(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f = fake_new(test, &tx, false, 0);

	KUNIT_ASSERT_EQ(test, dcp_hdcp_authenticate(&tx), 0);
	KUNIT_EXPECT_STREQ(test, f->sep, "ochplLsfE");
	KUNIT_EXPECT_TRUE(test, wrote(f, DP_HDCP_2_2_REG_EKPUB_KM_OFFSET, 128));
	KUNIT_EXPECT_TRUE(test, reported(f, DCP_HDCP_PROGRESS_H_VERIFIED));

	/* No pairing information: the attempt fails, nothing is encrypted. */
	f = fake_new(test, &tx, false, 0);
	f->events &= ~BIT(DCP_HDCP_AUTH_PAIRING);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, f->encryption);
}

static void tx_repeater_types(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f;
	u8 *sm;
	u8 type;

	for (type = 0; type <= 1; type++) {
		f = fake_new(test, &tx, true, type);
		f->stored = true;
		KUNIT_ASSERT_EQ(test, dcp_hdcp_authenticate(&tx), 0);
		KUNIT_EXPECT_STREQ(test, f->sep, "ochlLsrmfE");
		KUNIT_EXPECT_TRUE(test, tx.repeater);
		KUNIT_EXPECT_EQ(test, tx.device_count, 1);
		KUNIT_EXPECT_EQ(test, tx.depth, 1);
		/* The ack carries V's low half. */
		KUNIT_EXPECT_TRUE(test, wrote(f, DP_HDCP_2_2_REG_V_OFFSET, 16));
		KUNIT_EXPECT_EQ(test, *reg(f, DP_HDCP_2_2_REG_V_OFFSET), 0x22);
		/* seq_num_M 0, k 1, stream 0 with the type asked for. */
		KUNIT_EXPECT_TRUE(test, wrote(f, DP_HDCP_2_2_REG_SEQ_NUM_M_OFFSET, 7));
		sm = reg(f, DP_HDCP_2_2_REG_SEQ_NUM_M_OFFSET);
		KUNIT_EXPECT_EQ(test, get_unaligned_be24(sm), 0);
		KUNIT_EXPECT_EQ(test, get_unaligned_be16(sm + 3), 1);
		KUNIT_EXPECT_EQ(test, sm[5], 0);
		KUNIT_EXPECT_EQ(test, sm[6], type);
		KUNIT_EXPECT_EQ(test, tx.seq_num_m, 1);
		KUNIT_EXPECT_FALSE(test, wrote(f, DP_HDCP_2_2_REG_STREAM_TYPE_OFFSET, 1));
		KUNIT_EXPECT_TRUE(test, reported(f, DCP_HDCP_PROGRESS_LIST_VERIFIED));
		KUNIT_EXPECT_TRUE(test, reported(f, DCP_HDCP_PROGRESS_STREAM_READY));
	}
}

static void tx_type1_refused_downstream_legacy(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f;
	u8 legacy[] = { BIT(0), BIT(1) };	/* HDCP 1.x device, 2.0 repeater */
	int i;

	for (i = 0; i < ARRAY_SIZE(legacy); i++) {
		f = fake_new(test, &tx, true, 1);
		reg(f, DP_HDCP_2_2_REG_RXINFO_OFFSET)[1] |= legacy[i];
		KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -EPERM);
		KUNIT_EXPECT_FALSE(test, f->encryption);
		KUNIT_EXPECT_TRUE(test, tx.legacy_downstream);
		dcp_hdcp_abort(&tx);
		KUNIT_EXPECT_TRUE(test, f->closed);
		/* Never asked to turn encryption off: it never went on. */
		KUNIT_EXPECT_EQ(test, f->sep[f->nsep - 1], 'x');

		/* Type 0 goes through the same topology. */
		f = fake_new(test, &tx, true, 0);
		reg(f, DP_HDCP_2_2_REG_RXINFO_OFFSET)[1] |= legacy[i];
		KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), 0);
	}
}

static void tx_topology_exceeded(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f = fake_new(test, &tx, true, 0);

	reg(f, DP_HDCP_2_2_REG_RXINFO_OFFSET)[1] |= BIT(3);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -E2BIG);
	f = fake_new(test, &tx, true, 0);
	reg(f, DP_HDCP_2_2_REG_RXINFO_OFFSET)[1] |= BIT(2);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -E2BIG);
	/* A list that names no device. */
	f = fake_new(test, &tx, true, 0);
	reg(f, DP_HDCP_2_2_REG_RXINFO_OFFSET)[1] = 0;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -E2BIG);
	/* The first list of a session must carry seq_num_V 0. */
	f = fake_new(test, &tx, true, 0);
	reg(f, DP_HDCP_2_2_REG_SEQ_NUM_V_OFFSET)[2] = 1;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -EPROTO);
	KUNIT_EXPECT_FALSE(test, f->encryption);
}

static void tx_revoked(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f = fake_new(test, &tx, false, 0);

	/* The receiver's own ID: refused before the SEP sees the cert. */
	f->revoked_ids = 1;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -EPERM);
	KUNIT_EXPECT_STREQ(test, f->sep, "o");

	/* An SRM that cannot be read fails closed. */
	f = fake_new(test, &tx, false, 0);
	f->revoked_ids = -EINVAL;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -EINVAL);
	KUNIT_EXPECT_FALSE(test, f->encryption);
}

static void tx_timeout_and_rxstatus(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f = fake_new(test, &tx, false, 0);

	f->stored = true;
	f->events = 0;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, f->encryption);

	/* A lost event, but RxStatus says H' is there. */
	f = fake_new(test, &tx, false, 0);
	f->stored = true;
	f->events = 0;
	*reg(f, DP_HDCP_2_2_REG_RXSTATUS_OFFSET) = BIT(1);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), 0);
}

static void tx_close_mid_auth(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f = fake_new(test, &tx, true, 0);

	/* DCP closes while the ReceiverID list is awaited. */
	f->cancel_op = DCP_HDCP_AUTH_LIST;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -ECANCELED);
	KUNIT_EXPECT_FALSE(test, reported(f, DCP_HDCP_PROGRESS_LIST_VERIFIED));
	dcp_hdcp_abort(&tx);
	KUNIT_EXPECT_TRUE(test, f->closed);
	KUNIT_EXPECT_FALSE(test, f->encryption);
	/* A second abort is harmless. */
	f->nsep = 0;
	memset(f->sep, 0, sizeof(f->sep));
	dcp_hdcp_abort(&tx);
	KUNIT_EXPECT_EQ(test, f->nsep, 0);
}

static void tx_m_prime_retry(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f = fake_new(test, &tx, true, 1);

	f->m_rejects = 1;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), 0);
	KUNIT_EXPECT_EQ(test, tx.seq_num_m, 2);
	KUNIT_EXPECT_EQ(test, get_unaligned_be24(reg(f, DP_HDCP_2_2_REG_SEQ_NUM_M_OFFSET)), 1);

	f = fake_new(test, &tx, true, 1);
	f->m_rejects = 2;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -EACCES);
	KUNIT_EXPECT_FALSE(test, f->encryption);
}

static void tx_not_capable_and_link_check(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f = fake_new(test, &tx, false, 0);

	KUNIT_EXPECT_EQ(test, dcp_hdcp_capable(&tx), 1);
	reg(f, DP_HDCP_2_2_REG_RX_CAPS_OFFSET)[2] = 0;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_capable(&tx), 0);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -ENODEV);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&(struct dcp_hdcp_tx){
					.ops = &fake_ops, .ctx = f, .type = 2 }),
			-EINVAL);

	KUNIT_EXPECT_EQ(test, dcp_hdcp_check_link(&tx), 0);
	*reg(f, DP_HDCP_2_2_REG_RXSTATUS_OFFSET) = BIT(3);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_check_link(&tx), -EAGAIN);
	*reg(f, DP_HDCP_2_2_REG_RXSTATUS_OFFSET) = BIT(4);
	KUNIT_EXPECT_EQ(test, dcp_hdcp_check_link(&tx), -EAGAIN);
}

static void tx_l_prime_retry(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f = fake_new(test, &tx, false, 0);

	/* Each retry starts with a fresh r_n. */
	f->stored = true;
	f->l_rejects = DCP_HDCP_LC_TRIES - 1;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), 0);
	KUNIT_EXPECT_STREQ(test, f->sep, "ochlLlLlLsfE");

	f = fake_new(test, &tx, false, 0);
	f->stored = true;
	f->l_rejects = DCP_HDCP_LC_TRIES;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -EACCES);
	KUNIT_EXPECT_STREQ(test, f->sep, "ochlLlLlL");
}

static void tx_sep_busy(struct kunit *test)
{
	struct dcp_hdcp_tx tx;
	struct fake *f = fake_new(test, &tx, false, 0);

	f->sep_busy = true;
	KUNIT_EXPECT_EQ(test, dcp_hdcp_authenticate(&tx), -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->nwrites, 0);
	dcp_hdcp_abort(&tx);
	KUNIT_EXPECT_EQ(test, f->nsep, 0);
}

static struct kunit_case dcp_hdcp_tx_cases[] = {
	KUNIT_CASE(tx_stored_km_type0),
	KUNIT_CASE(tx_first_pairing),
	KUNIT_CASE(tx_repeater_types),
	KUNIT_CASE(tx_type1_refused_downstream_legacy),
	KUNIT_CASE(tx_topology_exceeded),
	KUNIT_CASE(tx_revoked),
	KUNIT_CASE(tx_timeout_and_rxstatus),
	KUNIT_CASE(tx_close_mid_auth),
	KUNIT_CASE(tx_m_prime_retry),
	KUNIT_CASE(tx_l_prime_retry),
	KUNIT_CASE(tx_sep_busy),
	KUNIT_CASE(tx_not_capable_and_link_check),
	{ }
};

static struct kunit_suite dcp_hdcp_tx_suite = {
	.name = "apple-dcp-hdcp-tx",
	.test_cases = dcp_hdcp_tx_cases,
};
kunit_test_suite(dcp_hdcp_tx_suite);
