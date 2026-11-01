// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: firmware commands
 *
 * Start and Process are large command images. Each is built in a zeroed
 * buffer: the header, the constant words (ave-image.c), then every field
 * that depends on the session or the frame.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/device.h>
#include <linux/math64.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <media/videobuf2-v4l2.h>

#include "ave.h"
#include "ave-regs.h"

/* Firmware-wide configuration, sent once after every firmware boot. */
int ave_cmd_config(struct ave_dev *ave)
{
	int buf = ave_ipc_scalar_buf(ave);
	u8 *cmd;
	int ret;

	if (buf < 0)
		return buf;
	cmd = ave_ipc_cmd(ave, buf,
			  ave->layout->abi->scalar_bytes[AVE_OP_CONFIG]);
	if (!cmd)
		return -EBUSY;
	ave->layout->abi->write_config(ave, cmd);
	ret = ave_ipc_submit(ave, buf,
			     ave->layout->abi->scalar_bytes[AVE_OP_CONFIG],
			     AVE_CMD_GLOBAL_TIMEOUT_US, true);
	return ret ?: ave_ipc_settle(ave, AVE_CMD_WAIT_MS);
}

/*
 * Firmware-wide stop, sent after every session has closed. The firmware
 * neither acknowledges nor answers it.
 */
int ave_cmd_halt(struct ave_dev *ave)
{
	int buf = ave_ipc_scalar_buf(ave);
	u8 *cmd;

	if (buf < 0)
		return buf;
	cmd = ave_ipc_cmd(ave, buf, ave->layout->abi->header_bytes);
	if (!cmd)
		return -EBUSY;
	ave_write_global(ave, cmd, AVE_OP_HALT);
	return ave_ipc_submit(ave, buf, ave->layout->abi->header_bytes,
			      AVE_CMD_GLOBAL_TIMEOUT_US, false);
}

static int ave_cmd_scalar(struct ave_ctx *ctx, enum ave_opcode op, u32 arg0,
			  u32 arg2)
{
	struct ave_dev *ave = ctx->ave;
	int buf = ave_ipc_scalar_buf(ave);
	u8 *cmd;

	if (buf < 0)
		return buf;
	if (!ave->layout->abi->scalar_bytes[op])
		return -EOPNOTSUPP;
	cmd = ave_ipc_cmd(ave, buf, ave->layout->abi->scalar_bytes[op]);
	if (!cmd)
		return -EBUSY;
	ave_write_header(ctx, cmd, op, op, arg0, arg2);
	return ave_ipc_submit(ave, buf, ave->layout->abi->scalar_bytes[op],
			      ave->layout->abi->transport->boot_config ?
				      AVE_CMD_SESSION_TIMEOUT_US :
				      AVE_CMD_GLOBAL_TIMEOUT_US,
			      true);
}

int ave_cmd_open(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	int ret;

	ave->next_client += AVE_CLIENT_STEP;
	ctx->sess->client = ave->next_client;
	ctx->sess->token = AVE_TOKEN_BASE;
	ret = ave_cmd_scalar(ctx, AVE_OP_OPEN, AVE_SESSION_ARG0,
			     ave_session_setup_arg2(ctx));
	if (ave->layout->abi->transport->boot_config) {
		if (!ret)
			ctx->sess->fw_open = true;
		return ret ?: ave_ipc_settle(ave, AVE_CMD_WAIT_MS);
	}
	ret = ret ?: ave_ipc_settle(ave, AVE_CMD_WAIT_MS);
	if (!ret)
		ctx->sess->fw_open = true;
	return ret;
}

/* Drain barrier: answered once every Process sent before it is answered. */
int ave_cmd_complete(struct ave_ctx *ctx, unsigned int count)
{
	int ret = 0;

	for (unsigned int i = 0; i < count && !ret; i++)
		ret = ave_cmd_scalar(ctx, AVE_OP_COMPLETE, 0, ave_session_arg2(ctx));
	return ret;
}

/* Priority and Stop go out together, Close after Stop is answered. */
int ave_cmd_close(struct ave_ctx *ctx)
{
	u32 arg2 = ave_session_arg2(ctx);
	bool legacy = ctx->ave->layout->abi->transport->boot_config;
	int ret = 0, err;

	if (legacy)
		ret = ave_cmd_scalar(ctx, AVE_OP_PRIORITY, AVE_SESSION_ARG0,
				     arg2);
	if (!ret && (legacy || ctx->sess->handed))
		ret = ave_cmd_scalar(ctx, AVE_OP_STOP, AVE_SESSION_ARG0, arg2);
	err = ave_ipc_settle(ctx->ave, AVE_CMD_WAIT_MS);
	ret = ret ?: err;
	if (ctx->ave->fw_state != AVE_FW_RUNNING)
		return ret;
	/* A failed 6070 Stop cannot establish a safe Close/Halt boundary. */
	if (ret && !legacy)
		return ret;
	err = ave_cmd_scalar(ctx, AVE_OP_CLOSE, AVE_SESSION_ARG0, arg2);
	if (!err)
		err = ave_ipc_settle(ctx->ave, AVE_CMD_WAIT_MS);
	if (legacy || !err)
		ctx->sess->fw_open = false;
	return ret ?: err;
}

int ave_cmd_start(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	const struct ave_params *p = &sess->p;
	const struct ave_fw_layout *fw = ave->layout;
	size_t len = fw->bytes[p->codec - 1][AVE_IMAGE_START];
	u8 *cmd;
	int ret;

	if (!fw->encode_ready)
		return -EOPNOTSUPP;
	cmd = ave_ipc_cmd(ave, AVE_BUF_CMD, len);
	if (!cmd)
		return -EBUSY;
	ret = ave_build_start(ctx, cmd, len);
	if (ret)
		return ret;
	ave_debugfs_save(ave, p->codec, AVE_IMAGE_START, cmd, len);

	/* A failed submission may already have exposed the Start buffers. */
	sess->handed = true;
	ret = ave_ipc_submit(ave, AVE_BUF_CMD, len, AVE_CMD_SESSION_TIMEOUT_US,
			     true);
	if (ret)
		return ret;
	return ave_ipc_settle(ave, AVE_START_WAIT_MS);
}

int ave_cmd_process(struct ave_ctx *ctx, unsigned int slot,
		    struct vb2_v4l2_buffer *src, dma_addr_t input, u32 bitrate,
		    bool force_key)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	const struct ave_params *p = &sess->p;
	const struct ave_fw_layout *fw = ave->layout;
	size_t len = fw->bytes[p->codec - 1][AVE_IMAGE_PROCESS];
	struct ave_slot *s;
	unsigned int buf = AVE_BUF_PROC(sess->index * AVE_MAX_SLOTS + slot);
	u8 *cmd;
	int ret;

	if (!fw->encode_ready)
		return -EOPNOTSUPP;

	if (slot >= p->slots || p->slots > fw->max_slots)
		return -EINVAL;
	s = &sess->slot[slot];
	ret = ave_ipc_wait_ack(ave, buf);
	if (ret)
		return ret;
	cmd = ave_ipc_cmd(ave, buf, len);
	if (!cmd)
		return -EBUSY;
	ret = ave_build_process(ctx, cmd, len, slot, input, bitrate, force_key);
	if (ret)
		return ret;
	if (!sess->frames)
		ave_debugfs_save(ave, p->codec, AVE_IMAGE_PROCESS, cmd, len);

	/* Pin before publishing the command, including an uncertain send failure. */
	s->input_mem = ave_vb2_get(&src->vb2_buf);
	ret = ave_ipc_submit(ave, buf, len, fw->process_timeout_us, false);
	if (ret)
		return ret;
	s->src = src;
	s->busy = true;
	s->status = 0;
	sess->progress = jiffies;
	sess->last_sent = jiffies;
	sess->nudged = false;
	sess->busy++;
	sess->frames++;
	return 0;
}
