/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fake device and shared-memory boundary for the production channel parser.
 * Every read the parser makes is routed through fw_read(), which plays a
 * firmware that rewrites a descriptor field as soon as the host has read it.
 */
typedef u64 __le64;
#define cpu_to_le64(x) ((u64)(x))
#define dma_wmb() ((void)0)
#ifndef AVE_IPC_MAX_CHANNELS
#define AVE_IPC_MAX_CHANNELS 8
#endif

struct ave_dev {
	const struct ave_fw_layout *layout;
	struct ave_buf ipc;
	u64 ipc_fw_base;
	u32 channel_bytes;
	struct ave_ring ring[AVE_RINGS];
};

enum {
	FW_SLOTS = BIT(0), /* grow the ring past the IPC window */
	FW_ADDR = BIT(1), /* move the ring to the start of the window */
	FW_NAME = BIT(2), /* rename the channel */
	FW_ONCE = BIT(3), /* assert no descriptor byte is fetched twice */
};

static struct ave_dev *fw_ave;
static unsigned int fw_mode, fw_descriptors;
static unsigned char fw_fetched[AVE_IPC_MAX_CHANNELS * AVE_CHAN_DESC_STRIDE];

static bool fw_overlaps(long a, size_t len, long b, size_t blen)
{
	return a < b + (long)blen && b < a + (long)len;
}

static void fw_read(const void *p, size_t len)
{
	const u8 *table = (const u8 *)fw_ave->ipc.cpu + AVE_IPC_CHANNEL_OFF;
	size_t bytes = fw_descriptors * AVE_CHAN_DESC_STRIDE;
	size_t start;

	if ((const u8 *)p < table || (const u8 *)p >= table + bytes)
		return;
	start = (const u8 *)p - table;
	assert(start + len <= bytes);
	for (size_t b = start; b < start + len; b++) {
		if (fw_mode & FW_ONCE)
			assert(!fw_fetched[b]);
		fw_fetched[b]++;
	}
	for (unsigned int i = start / AVE_CHAN_DESC_STRIDE;
	     i * AVE_CHAN_DESC_STRIDE < start + len; i++) {
		long at = (long)start - (long)(i * AVE_CHAN_DESC_STRIDE);
		u8 *desc = (u8 *)table + i * AVE_CHAN_DESC_STRIDE;

		if (fw_mode & FW_SLOTS &&
		    fw_overlaps(at, len, AVE_CHAN_DESC_SLOTS, 4))
			put_unaligned_le32(0x100000, desc + AVE_CHAN_DESC_SLOTS);
		if (fw_mode & FW_ADDR &&
		    fw_overlaps(at, len, AVE_CHAN_DESC_ADDR, 8))
			put_unaligned_le64(fw_ave->ipc_fw_base,
					   desc + AVE_CHAN_DESC_ADDR);
		if (fw_mode & FW_NAME &&
		    fw_overlaps(at, len, 0, AVE_CHAN_DESC_NAME_LEN))
			(memcpy)(desc, "X", 2);
	}
}

static inline u32 fw_le32(const void *p)
{
	u32 v = get_unaligned_le32(p);

	fw_read(p, 4);
	return v;
}

static inline u64 fw_le64(const void *p)
{
	u64 v = get_unaligned_le64(p);

	fw_read(p, 8);
	return v;
}

static void *fw_memcpy(void *d, const void *s, size_t n)
{
	(memcpy)(d, s, n);
	fw_read(s, n);
	return d;
}

static void *fw_memchr(const void *s, int c, size_t n)
{
	void *r = (memchr)(s, c, n);

	fw_read(s, r ? (size_t)((const u8 *)r - (const u8 *)s) + 1 : n);
	return r;
}

static int fw_strcmp(const char *a, const char *b)
{
	size_t n = 0;
	int r;

	while (a[n] && a[n] == b[n])
		n++;
	r = (unsigned char)a[n] - (unsigned char)b[n];
	fw_read(a, n + 1);
	fw_read(b, n + 1);
	return r;
}

static long fw_strscpy(char *d, const char *s, size_t n)
{
	size_t len = strnlen(s, n);

	fw_read(s, len < n ? len + 1 : n);
	if (len == n) {
		(memcpy)(d, s, n - 1);
		d[n - 1] = 0;
		return -E2BIG;
	}
	(memcpy)(d, s, len + 1);
	return len;
}

#define get_unaligned_le32(p) fw_le32(p)
#define get_unaligned_le64(p) fw_le64(p)
#define memcpy(d, s, n) fw_memcpy(d, s, n)
#define memchr(s, c, n) fw_memchr(s, c, n)
#define strcmp(a, b) fw_strcmp(a, b)
#define strscpy(d, s, n) fw_strscpy(d, s, n)
