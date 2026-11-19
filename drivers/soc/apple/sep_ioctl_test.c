// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit robustness tests for the SEP character device ioctls: every
 * /dev/sep-key request and the /dev/sep-bio session ioctls are driven from
 * real user memory (kunit_vm_mmap) with bad pointers, unaligned and
 * truncated buffers, out-of-range fields and random bytes. Every refusal
 * must be a clean errno; nothing reaches the enclave or the sensor.
 *
 * Included from bio_shim.c.
 */

#include <kunit/test.h>
#include <linux/ioctl.h>
#include <linux/mman.h>
#include <linux/prandom.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

int sep_kunit_key_read_request(unsigned int cmd, unsigned long arg);
void *sep_kunit_bio_new(void);
long sep_kunit_bio_ioctl(void *bio, unsigned int cmd, unsigned long arg);
void sep_kunit_bio_worker_stopped(void *bio);
void sep_kunit_bio_free(void *bio);
int sep_kunit_rearm_read(unsigned long arg);

/* /dev/sep-key (key.rs): sizes are baked into the numbers. */
#define SK_MAGIC 0xB1
#define SK_INFO _IOC(_IOC_READ, SK_MAGIC, 0x20, 16)
#define SK_CREATE _IOC(_IOC_READ | _IOC_WRITE, SK_MAGIC, 0x21, 2124)
#define SK_SIGN _IOC(_IOC_READ | _IOC_WRITE, SK_MAGIC, 0x22, 2164)
#define SK_PUBLIC _IOC(_IOC_READ | _IOC_WRITE, SK_MAGIC, 0x23, 2124)
#define SK_SELFTEST _IOC(_IOC_READ | _IOC_WRITE, SK_MAGIC, 0x24, 2104)
#define SK_CANCEL _IOC(_IOC_NONE, SK_MAGIC, 0x25, 0)
#define SK_SEAL _IOC(_IOC_READ | _IOC_WRITE, SK_MAGIC, 0x26, 2384)
#define SK_UNSEAL _IOC(_IOC_READ | _IOC_WRITE, SK_MAGIC, 0x27, 2384)

#define SK_HANDLE_MAX 2048
#define SK_SECRET_MAX 64
#define SK_SEALED_MAX 256

/* Offsets inside the requests (repr(C), no padding). */
#define SIGN_HANDLE_LEN 4
#define SIGN_HANDLE 116
#define PUBLIC_HANDLE_LEN 4
#define PUBLIC_HANDLE 76
#define SEAL_HANDLE_LEN 4
#define SEAL_SECRET_LEN 8
#define SEAL_SEALED_LEN 12
#define SEAL_SEALED 80
#define SEAL_HANDLE 336

/* /dev/sep-bio session ioctls (bio.rs). */
#define SB_GET_INFO _IOC(_IOC_READ, SK_MAGIC, 0x01, 32)
#define SB_LIST _IOC(_IOC_READ, SK_MAGIC, 0x02, 4616)
#define SB_ENROL_START _IOC(_IOC_WRITE, SK_MAGIC, 0x03, 144)
#define SB_ENROL_POLL _IOC(_IOC_READ, SK_MAGIC, 0x04, 40)
#define SB_VERIFY_START _IOC(_IOC_WRITE, SK_MAGIC, 0x05, 40)
#define SB_VERIFY_POLL _IOC(_IOC_READ, SK_MAGIC, 0x06, 72)
#define SB_CANCEL _IOC(_IOC_NONE, SK_MAGIC, 0x07, 0)
#define SB_DELETE _IOC(_IOC_WRITE, SK_MAGIC, 0x08, 16)
#define SB_DELETE_ALL _IOC(_IOC_NONE, SK_MAGIC, 0x09, 0)

#define MAP_PAGES 4

struct sep_ioctl_map {
	unsigned long base;	/* user address of MAP_PAGES pages */
	size_t len;
};

static int sep_ioctl_map(struct kunit *test, struct sep_ioctl_map *m)
{
	m->len = MAP_PAGES * PAGE_SIZE;
	m->base = kunit_vm_mmap(test, NULL, 0, m->len, PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, 0);
	if (IS_ERR_VALUE(m->base) || !m->base)
		return -ENOMEM;
	return 0;
}

/* Puts @n bytes of @src at user offset @off of @m. */
static void put(struct kunit *test, struct sep_ioctl_map *m, size_t off,
		const void *src, size_t n)
{
	KUNIT_ASSERT_LE(test, off + n, m->len);
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)(m->base + off), src, n), 0);
}

/*
 * A ref-key record carrying a public point:
 * SET { SEQ { "rk", SET { SEQ { "pub", OCTET STRING(04 || X || Y) } } } }.
 */
static size_t sep_record(u8 *out)
{
	static const u8 head[] = {
		0x31, 0x52, 0x30, 0x50, 0x0c, 0x02, 'r', 'k',
		0x31, 0x4a, 0x30, 0x48, 0x0c, 0x03, 'p', 'u', 'b',
		0x04, 0x41, 0x04,
	};
	memcpy(out, head, sizeof(head));
	memset(out + sizeof(head), 0x11, 64);
	return sizeof(head) + 64;
}

/* A handle "SEPK" v1 around the record; returns the handle length. */
static u32 sep_handle(u8 *h, u8 flags)
{
	u32 n;

	memset(h, 0, SK_HANDLE_MAX);
	memcpy(h, "SEPK", 4);
	h[4] = 1;
	h[5] = flags;
	n = sep_record(h + 12);
	put_unaligned_le32(n, h + 8);
	return n + 12;
}

static void *sep_request(struct kunit *test, unsigned int cmd, u8 hflags)
{
	u8 *r = kunit_kzalloc(test, _IOC_SIZE(cmd) ? _IOC_SIZE(cmd) : 1, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, r);
	switch (cmd) {
	case SK_SIGN:
		put_unaligned_le32(sep_handle(r + SIGN_HANDLE, hflags), r + SIGN_HANDLE_LEN);
		break;
	case SK_PUBLIC:
		put_unaligned_le32(sep_handle(r + PUBLIC_HANDLE, hflags), r + PUBLIC_HANDLE_LEN);
		break;
	case SK_SEAL:
	case SK_UNSEAL:
		put_unaligned_le32(sep_handle(r + SEAL_HANDLE, hflags), r + SEAL_HANDLE_LEN);
		put_unaligned_le32(SK_SECRET_MAX, r + SEAL_SECRET_LEN);
		r[SEAL_SEALED] = 0x04;
		memset(r + SEAL_SEALED + 1, 7, 64 + 16 + 8);
		put_unaligned_le32(1 + 64 + 16 + 8, r + SEAL_SEALED_LEN);
		break;
	}
	return r;
}

static const unsigned int sep_key_cmds[] = {
	SK_INFO, SK_CREATE, SK_SIGN, SK_PUBLIC, SK_SELFTEST, SK_CANCEL,
	SK_SEAL, SK_UNSEAL,
};

static bool sep_key_reads(unsigned int cmd)
{
	return cmd != SK_INFO && cmd != SK_CANCEL;
}

/* Valid requests at every alignment are accepted; nothing faults. */
static void sep_key_valid_requests_any_alignment(struct kunit *test)
{
	struct sep_ioctl_map m;
	size_t off;
	int i;

	KUNIT_ASSERT_EQ(test, sep_ioctl_map(test, &m), 0);
	for (i = 0; i < ARRAY_SIZE(sep_key_cmds); i++) {
		unsigned int cmd = sep_key_cmds[i];
		u8 *r = sep_request(test, cmd, 1);

		for (off = 0; off < 9; off++) {
			put(test, &m, off, r, _IOC_SIZE(cmd));
			KUNIT_EXPECT_EQ_MSG(test, sep_kunit_key_read_request(cmd, m.base + off),
					    0, "cmd %#x offset %zu", cmd, off);
		}
	}
}

/* NULL, kernel and unmapped pointers and buffers running off the mapping. */
static void sep_key_bad_pointers_fault_cleanly(struct kunit *test)
{
	struct sep_ioctl_map m;
	u8 local[64];
	int i;

	KUNIT_ASSERT_EQ(test, sep_ioctl_map(test, &m), 0);
	/* Unmap the last page: a request that runs into it must fault. */
	KUNIT_ASSERT_EQ(test, vm_munmap(m.base + (MAP_PAGES - 1) * PAGE_SIZE, PAGE_SIZE), 0);
	for (i = 0; i < ARRAY_SIZE(sep_key_cmds); i++) {
		unsigned int cmd = sep_key_cmds[i];
		size_t size = _IOC_SIZE(cmd);
		unsigned long end = m.base + (MAP_PAGES - 1) * PAGE_SIZE;
		int want = sep_key_reads(cmd) ? -EFAULT : 0;
		u8 *r = sep_request(test, cmd, 1);

		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd, 0), want);
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd, (unsigned long)local), want);
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd, end), want);
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd, -1UL), want);
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd, TASK_SIZE - 8), want);
		if (!sep_key_reads(cmd))
			continue;
		/* Truncated: the last 1, 7 or 64 bytes fall off the mapping. */
		put(test, &m, end - m.base - size + 64, r, size - 64);
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd, end - size + 64), want);
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd, end - size + 7), want);
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd, end - size + 1), want);
		/* Exactly fits: accepted. */
		put(test, &m, end - m.base - size, r, size);
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd, end - size), 0);
	}
}

/* Length fields at and past their bounds, and every create policy bit. */
static void sep_key_lengths_and_enums(struct kunit *test)
{
	static const u32 bad_handle[] = { 0, 1, 12, 95, 97, 2048, 2049, 0x7fffffff, 0xffffffff };
	static const u32 bad_secret[] = { 0, 65, 256, 0xffffffff };
	static const u32 bad_sealed[] = { 0, 1, 65, 81, 257, 0xffffffff };
	struct sep_ioctl_map m;
	u8 *r;
	int i;

	KUNIT_ASSERT_EQ(test, sep_ioctl_map(test, &m), 0);
	r = sep_request(test, SK_SIGN, 0);
	for (i = 0; i < ARRAY_SIZE(bad_handle); i++) {
		put_unaligned_le32(bad_handle[i], r + SIGN_HANDLE_LEN);
		put(test, &m, 3, r, _IOC_SIZE(SK_SIGN));
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(SK_SIGN, m.base + 3), -EINVAL);
	}
	r = sep_request(test, SK_SEAL, 1);
	for (i = 0; i < ARRAY_SIZE(bad_secret); i++) {
		put_unaligned_le32(bad_secret[i], r + SEAL_SECRET_LEN);
		put(test, &m, 1, r, _IOC_SIZE(SK_SEAL));
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(SK_SEAL, m.base + 1), -EINVAL);
	}
	r = sep_request(test, SK_UNSEAL, 1);
	for (i = 0; i < ARRAY_SIZE(bad_sealed); i++) {
		put_unaligned_le32(bad_sealed[i], r + SEAL_SEALED_LEN);
		put(test, &m, 1, r, _IOC_SIZE(SK_UNSEAL));
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(SK_UNSEAL, m.base + 1), -EINVAL);
	}
	/* SEAL/UNSEAL of a key without a biometric list. */
	r = sep_request(test, SK_UNSEAL, 0);
	put(test, &m, 0, r, _IOC_SIZE(SK_UNSEAL));
	KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(SK_UNSEAL, m.base), -EINVAL);
	KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(SK_SEAL, m.base), -EINVAL);
	/* CREATE: one policy bit at most; the password ones are refused. */
	r = sep_request(test, SK_CREATE, 0);
	for (i = 0; i < 32; i++) {
		u32 flags = 1U << i;
		int want = flags <= 2 ? 0 : (flags == 4 || flags == 8) ? -EOPNOTSUPP : -EINVAL;

		put_unaligned_le32(flags, r);
		put(test, &m, 0, r, _IOC_SIZE(SK_CREATE));
		KUNIT_EXPECT_EQ_MSG(test, sep_kunit_key_read_request(SK_CREATE, m.base), want,
				    "flags %#x", flags);
	}
	/* Handle flags other than none, biometry, biometry-any. */
	r = sep_request(test, SK_SIGN, 0);
	for (i = 0; i < 256; i++) {
		r[SIGN_HANDLE + 5] = i;
		put(test, &m, 0, r, _IOC_SIZE(SK_SIGN));
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(SK_SIGN, m.base),
				(i == 0 || i == 1 || i == 3) ? 0 : -EINVAL);
	}
}

/* Unknown commands, and known numbers with the wrong size or direction. */
static void sep_key_unknown_commands(struct kunit *test)
{
	struct rnd_state rnd;
	int i, n;

	prandom_seed_state(&rnd, 0x5eb0);
	for (i = 0; i < ARRAY_SIZE(sep_key_cmds); i++) {
		unsigned int cmd = sep_key_cmds[i];

		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd ^ (1 << _IOC_SIZESHIFT), 0), -ENOTTY);
		KUNIT_EXPECT_EQ(test, sep_kunit_key_read_request(cmd ^ (1U << _IOC_DIRSHIFT), 0), -ENOTTY);
	}
	for (n = 0; n < 20000; n++) {
		unsigned int cmd = prandom_u32_state(&rnd);
		int ret = sep_kunit_key_read_request(cmd, 0);

		for (i = 0; i < ARRAY_SIZE(sep_key_cmds); i++)
			if (cmd == sep_key_cmds[i])
				break;
		if (i == ARRAY_SIZE(sep_key_cmds))
			KUNIT_EXPECT_EQ(test, ret, -ENOTTY);
	}
}

/* Random bytes, valid requests with random bytes flipped, random offsets. */
static void sep_key_random_requests(struct kunit *test)
{
	struct sep_ioctl_map m;
	struct rnd_state rnd;
	u8 *buf;
	int n;

	KUNIT_ASSERT_EQ(test, sep_ioctl_map(test, &m), 0);
	buf = kunit_kzalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	prandom_seed_state(&rnd, 0xf00dcafe);
	for (n = 0; n < 6000; n++) {
		unsigned int cmd = sep_key_cmds[prandom_u32_state(&rnd) % ARRAY_SIZE(sep_key_cmds)];
		size_t size = _IOC_SIZE(cmd);
		size_t off = prandom_u32_state(&rnd) % 64;
		int flips, ret;

		if (!size)
			continue;
		if (n & 1) {
			prandom_bytes_state(&rnd, buf, size);
		} else {
			u8 *r = sep_request(test, cmd, prandom_u32_state(&rnd) & 3);

			memcpy(buf, r, size);
			kunit_kfree(test, r);
			for (flips = 1 + prandom_u32_state(&rnd) % 4; flips; flips--)
				buf[prandom_u32_state(&rnd) % size] = prandom_u32_state(&rnd);
		}
		put(test, &m, off, buf, size);
		ret = sep_kunit_key_read_request(cmd, m.base + off);
		KUNIT_EXPECT_TRUE_MSG(test, ret == 0 || ret == -EINVAL || ret == -EOPNOTSUPP,
				      "cmd %#x returned %d", cmd, ret);
	}
}

static const unsigned int sep_bio_cmds[] = {
	SB_GET_INFO, SB_LIST, SB_ENROL_START, SB_ENROL_POLL, SB_VERIFY_START,
	SB_VERIFY_POLL, SB_CANCEL, SB_DELETE, SB_DELETE_ALL,
};

static bool sep_bio_errno_ok(long ret)
{
	switch (ret) {
	case 0: case -EINVAL: case -EFAULT: case -EBUSY: case -ENOENT:
	case -ENOSPC: case -EIO: case -EPERM: case -ENODEV: case -ENOTTY:
	case -EAGAIN: case -EOVERFLOW:
		return true;
	}
	return false;
}

/* The session ioctls in any order, with any bytes, after cancel and stop. */
static void sep_bio_session_random(struct kunit *test)
{
	struct sep_ioctl_map m;
	struct rnd_state rnd;
	void *bio;
	u8 *buf;
	int n;

	KUNIT_ASSERT_EQ(test, sep_ioctl_map(test, &m), 0);
	buf = kunit_kzalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	bio = sep_kunit_bio_new();
	KUNIT_ASSERT_NOT_NULL(test, bio);
	prandom_seed_state(&rnd, 0xb10b10);
	for (n = 0; n < 8000; n++) {
		u32 pick = prandom_u32_state(&rnd);
		unsigned int cmd = sep_bio_cmds[pick % ARRAY_SIZE(sep_bio_cmds)];
		size_t size = _IOC_SIZE(cmd);
		size_t off = (pick >> 8) % 32;
		unsigned long arg = m.base + off;
		long ret;

		if (size) {
			prandom_bytes_state(&rnd, buf, size);
			/* Often a well-formed VERIFY_START / ENROL_START. */
			if (cmd == SB_VERIFY_START && (pick & 0x10000))
				memset(buf, 0, 8);
			put(test, &m, off, buf, size);
		}
		switch ((pick >> 20) & 15) {
		case 0:
			arg = 0;
			break;
		case 1:
			arg = (unsigned long)buf;
			break;
		case 2:
			arg = -1UL;
			break;
		case 3:
			sep_kunit_bio_worker_stopped(bio);
			break;
		}
		ret = sep_kunit_bio_ioctl(bio, cmd, arg);
		KUNIT_EXPECT_TRUE_MSG(test, sep_bio_errno_ok(ret), "cmd %#x returned %ld", cmd, ret);
		/* Unknown numbers next to the real ones. */
		ret = sep_kunit_bio_ioctl(bio, cmd ^ (1 << _IOC_SIZESHIFT), arg);
		KUNIT_EXPECT_EQ(test, ret, -ENOTTY);
	}
	sep_kunit_bio_free(bio);
}

/* Fixed sequences: a second start is busy; malformed starts are refused. */
static void sep_bio_session_rules(struct kunit *test)
{
	struct sep_ioctl_map m;
	u8 req[144] = { 0 };
	void *bio;

	KUNIT_ASSERT_EQ(test, sep_ioctl_map(test, &m), 0);
	bio = sep_kunit_bio_new();
	KUNIT_ASSERT_NOT_NULL(test, bio);
	/* VERIFY_START: flags and reserved must be zero. */
	req[0] = 1;
	put(test, &m, 1, req, 40);
	KUNIT_EXPECT_EQ(test, sep_kunit_bio_ioctl(bio, SB_VERIFY_START, m.base + 1), -EINVAL);
	memset(req, 0, sizeof(req));
	req[4] = 1;
	put(test, &m, 1, req, 40);
	KUNIT_EXPECT_EQ(test, sep_kunit_bio_ioctl(bio, SB_VERIFY_START, m.base + 1), -EINVAL);
	/* ENROL_START: finger 0, 11, a label without the FP1- form. */
	memset(req, 0, sizeof(req));
	put_unaligned_le32(1000, req + 4);
	put(test, &m, 0, req, 144);
	KUNIT_EXPECT_EQ(test, sep_kunit_bio_ioctl(bio, SB_ENROL_START, m.base), -EINVAL);
	put_unaligned_le32(11, req + 8);
	put(test, &m, 0, req, 144);
	KUNIT_EXPECT_EQ(test, sep_kunit_bio_ioctl(bio, SB_ENROL_START, m.base), -EINVAL);
	/* A good enrolment request, then a second start while it runs. */
	put_unaligned_le32(7, req + 8);
	memcpy(req + 16, "FP1-20261111-7-12345678-user", 28);
	put(test, &m, 0, req, 144);
	KUNIT_EXPECT_EQ(test, sep_kunit_bio_ioctl(bio, SB_ENROL_START, m.base), 0);
	KUNIT_EXPECT_EQ(test, sep_kunit_bio_ioctl(bio, SB_ENROL_START, m.base), -EBUSY);
	memset(req, 0, 40);
	put(test, &m, 512, req, 40);
	KUNIT_EXPECT_EQ(test, sep_kunit_bio_ioctl(bio, SB_VERIFY_START, m.base + 512), -EBUSY);
	/* Cancel, poll after cancel, cancel again. */
	KUNIT_EXPECT_EQ(test, sep_kunit_bio_ioctl(bio, SB_CANCEL, 0), 0);
	KUNIT_EXPECT_TRUE(test, sep_bio_errno_ok(sep_kunit_bio_ioctl(bio, SB_ENROL_POLL, m.base + 1024)));
	KUNIT_EXPECT_TRUE(test, sep_bio_errno_ok(sep_kunit_bio_ioctl(bio, SB_CANCEL, 0)));
	sep_kunit_bio_worker_stopped(bio);
	/* Polls into a faulting buffer fail cleanly. */
	KUNIT_EXPECT_TRUE(test, sep_bio_errno_ok(sep_kunit_bio_ioctl(bio, SB_VERIFY_POLL, 0)));
	KUNIT_EXPECT_EQ(test, sep_kunit_bio_ioctl(bio, SB_LIST, -1UL), -EFAULT);
	KUNIT_EXPECT_EQ(test, sep_kunit_bio_ioctl(bio, SB_GET_INFO, 0), -EFAULT);
	sep_kunit_bio_free(bio);
}

/* REARM: the password length and reserved word, from user memory. */
static void sep_rearm_request(struct kunit *test)
{
	static const u32 bad_len[] = { 0, 1025, 0x7fffffff, 0xffffffff };
	struct sep_ioctl_map m;
	u8 *req;
	int i;

	KUNIT_ASSERT_EQ(test, sep_ioctl_map(test, &m), 0);
	req = kunit_kzalloc(test, 1032, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	memset(req + 8, 'p', 1024);
	for (i = 0; i < ARRAY_SIZE(bad_len); i++) {
		put_unaligned_le32(bad_len[i], req);
		put(test, &m, 5, req, 1032);
		KUNIT_EXPECT_EQ(test, sep_kunit_rearm_read(m.base + 5), -EINVAL);
	}
	put_unaligned_le32(1024, req);
	put_unaligned_le32(1, req + 4);
	put(test, &m, 5, req, 1032);
	KUNIT_EXPECT_EQ(test, sep_kunit_rearm_read(m.base + 5), -EINVAL);
	put_unaligned_le32(0, req + 4);
	put(test, &m, 5, req, 1032);
	KUNIT_EXPECT_EQ(test, sep_kunit_rearm_read(m.base + 5), 0);
	put_unaligned_le32(1, req);
	put(test, &m, 5, req, 1032);
	KUNIT_EXPECT_EQ(test, sep_kunit_rearm_read(m.base + 5), 0);
	KUNIT_EXPECT_EQ(test, sep_kunit_rearm_read(0), -EFAULT);
	KUNIT_EXPECT_EQ(test, sep_kunit_rearm_read(m.base + m.len - 1000), -EFAULT);
}

static struct kunit_case sep_ioctl_cases[] = {
	KUNIT_CASE(sep_key_valid_requests_any_alignment),
	KUNIT_CASE(sep_key_bad_pointers_fault_cleanly),
	KUNIT_CASE(sep_key_lengths_and_enums),
	KUNIT_CASE(sep_key_unknown_commands),
	KUNIT_CASE(sep_key_random_requests),
	KUNIT_CASE(sep_bio_session_random),
	KUNIT_CASE(sep_bio_session_rules),
	KUNIT_CASE(sep_rearm_request),
	{}
};

static struct kunit_suite sep_ioctl_suite = {
	.name = "apple_sep_ioctl",
	.test_cases = sep_ioctl_cases,
};

kunit_test_suite(sep_ioctl_suite);
