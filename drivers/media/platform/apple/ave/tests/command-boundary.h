/* SPDX-License-Identifier: GPL-2.0-only */
/* Publishing a Process must already own the mapping it references. */
static struct ave_ctx *submit_ctx;
static unsigned int submit_slot;
static int submit_result;
static u8 command[256];
static void *ave_ipc_cmd(struct ave_dev *ave, unsigned int buf, size_t len)
{
	return command;
}
static int ave_ipc_wait_ack(struct ave_dev *ave, unsigned int buf)
{
	return 0;
}
static int ave_build_start(struct ave_ctx *ctx, void *cmd, size_t len)
{
	return 0;
}
static int ave_build_process(struct ave_ctx *ctx, void *cmd, size_t len,
			     unsigned int slot, dma_addr_t input, u32 bitrate, bool force_key)
{
	return 0;
}
static void ave_debugfs_save(struct ave_dev *ave, enum ave_codec codec,
			     enum ave_image kind, const void *cmd, size_t len)
{
}
static int ave_ipc_submit(struct ave_dev *ave, unsigned int buf, size_t len,
			  u64 timeout, bool reply)
{
	struct ave_vb2_mem *mem = submit_ctx->sess->slot[submit_slot].input_mem;
	struct test_mapping *mapping;

	if (reply) {
		assert(submit_ctx->sess->handed);
	} else {
		assert(mem && mem->refs == 2);
		mapping = mem->priv;
		mapping->firmware_visible = true;
	}
	if (submit_result)
		ave_fw_mark_broken(ave, submit_result, "test submission");
	return submit_result;
}
