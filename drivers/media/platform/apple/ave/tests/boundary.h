/* SPDX-License-Identifier: GPL-2.0-only */
/* Only the hardware/IPC boundary is mocked. The test uses real session types. */
struct ave_dev {
	struct ave_ctx *sessions[AVE_MAX_SESSIONS];
	u32 next_client, text_crc, data_crc;
	dma_addr_t extra0_dma;
	const struct ave_fw_layout *layout;
	const struct ave_variant *variant;
	enum ave_fw_state fw_state;
	void *dev;
	unsigned long mctf_pool_flags;
	struct ave_buf mctf_pool[AVE_MCTF_BUFS];
	struct ave_buf mctf_pool_in[AVE_MCTF_IN_BUFS];
	struct ave_buf mctf_pool_out[AVE_MCTF_OUT_BUFS];
	u32 mctf_pool_crc;
	bool mctf_pool_crc_valid;
};

struct ave_ctx {
	struct ave_dev *ave;
	struct ave_session *sess;
	int ready;
};

struct vb2_v4l2_buffer {
	struct vb2_buffer { int unused; } vb2_buf;
};

static struct ave_vb2_mem *ave_vb2_get(struct vb2_buffer *vb) { return NULL; }
static void ave_vb2_put(void *mem) { assert(!mem); }

static u8 wire[0x1c000 + 32];
static size_t wire_len;
static unsigned int submissions;
static u64 next_dma = 0x10002000000ULL;
static unsigned int alloc_count, free_count;
static void ave_free(struct ave_dev *ave, struct ave_buf *buf)
{
	if (buf->cpu)
		free_count++;
	memset(buf, 0, sizeof(*buf));
}

static int ave_alloc_padded(struct ave_dev *ave, struct ave_buf *buf,
			    size_t size, size_t alloc, unsigned long attrs)
{
	assert(size);
	buf->size = size;
	buf->alloc = ALIGN(max(size, alloc), SZ_16K);
	buf->dma = next_dma;
	next_dma += ALIGN(buf->alloc, 0x100000);
	buf->attrs = attrs;
	buf->cpu = (void *)1;
	alloc_count++;
	return 0;
}

static void *ave_ipc_cmd(struct ave_dev *ave, unsigned int buf, size_t len)
{
	assert(len <= sizeof(wire) - 32);
	memset(wire, 0, len);
	memset(wire + len, 0xa5, 32);
	wire_len = len;
	return wire;
}

static int ave_ipc_submit(struct ave_dev *ave, unsigned int buf, size_t len,
			  u64 timeout, bool reply)
{
	assert(len == wire_len);
	for (unsigned int i = 0; i < 32; i++)
		assert(wire[len + i] == 0xa5);
#ifdef TEST_PER_ABI
	put_unaligned_le64(timeout, wire + ave->layout->abi->timeout_offset);
	put_unaligned_le64(1234567, wire + ave->layout->abi->created_offset);
#else
	put_unaligned_le64(timeout, wire + AVE_CMD_TIMEOUT);
	put_unaligned_le64(1234567, wire + AVE_CMD_CREATED);
#endif
	submissions++;
	return 0;
}

static int ave_ipc_scalar_buf(struct ave_dev *ave)
{
	return 1;
}

static int ave_ipc_settle(struct ave_dev *ave, unsigned int ms)
{
	return 0;
}

static int ave_ipc_wait_ack(struct ave_dev *ave, unsigned int buf)
{
	return 0;
}

static void ave_debugfs_save(struct ave_dev *ave, enum ave_codec codec,
			     enum ave_image kind, const void *cmd, size_t len)
{
}
