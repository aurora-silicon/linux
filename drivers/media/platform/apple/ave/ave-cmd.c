// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: firmware commands
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/device.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <media/videobuf2-v4l2.h>

#include "ave.h"
#include "ave-regs.h"

static void ave_cmd_header(struct ave_dev *ave, u8 *cmd, enum ave_opcode op,
			   u32 client, enum ave_codec codec)
{
	put_unaligned_le32(op, cmd + AVE_CMD_OPCODE);
	put_unaligned_le64(++ave->next_token, cmd + AVE_CMD_TOKEN);
	put_unaligned_le64(client, cmd + AVE_CMD_CLIENT);
	put_unaligned_le32(1, cmd + AVE_CMD_UNIT);
	put_unaligned_le32(codec, cmd + AVE_CMD_CODEC);
	put_unaligned_le32(op, cmd + AVE_CMD_OPCODE_ECHO);
}

/* Firmware-wide configuration, sent once after every firmware boot. */
int ave_cmd_config(struct ave_dev *ave)
{
	u8 cmd[AVE_CONFIG_BYTES] = {};

	put_unaligned_le32(AVE_OP_CONFIG, cmd + AVE_CMD_OPCODE);
	put_unaligned_le64(++ave->next_token, cmd + AVE_CMD_TOKEN);
	put_unaligned_le32(U32_MAX, cmd + AVE_CONFIG_ALL_UNITS);
	cmd[AVE_CONFIG_OPT_A] = 1;
	cmd[AVE_CONFIG_OPT_B] = 1;
	return ave_ipc_call(ave, cmd, sizeof(cmd));
}

/* Firmware-wide stop, sent after every session has closed. */
int ave_cmd_halt(struct ave_dev *ave)
{
	u8 cmd[AVE_CMD_HDR_BYTES] = {};

	put_unaligned_le32(AVE_OP_HALT, cmd + AVE_CMD_OPCODE);
	put_unaligned_le64(++ave->next_token, cmd + AVE_CMD_TOKEN);
	put_unaligned_le32(AVE_HALT_ALL, cmd + AVE_CMD_OPCODE_ECHO);
	return ave_ipc_call(ave, cmd, sizeof(cmd));
}

static int ave_cmd_simple(struct ave_ctx *ctx, enum ave_opcode op, enum ave_codec codec,
			  u32 arg0, u32 arg2)
{
	u8 cmd[AVE_CMD_HDR_BYTES] = {};

	ave_cmd_header(ctx->ave, cmd, op, ctx->client, codec);
	put_unaligned_le32(arg0, cmd + AVE_CMD_ARG0);
	put_unaligned_le32(AVE_SESSION_ARG1, cmd + AVE_CMD_ARG1);
	put_unaligned_le32(arg2, cmd + AVE_CMD_ARG2);
	return ave_ipc_call(ctx->ave, cmd, sizeof(cmd));
}

int ave_cmd_open(struct ave_ctx *ctx, enum ave_codec codec)
{
	struct ave_dev *ave = ctx->ave;
	int ret;

	ctx->client = ++ave->next_client;
	ret = ave_cmd_simple(ctx, AVE_OP_OPEN, codec, AVE_SESSION_ARG0, AVE_OPEN_ARG2);
	if (!ret)
		ctx->session_open = true;
	return ret;
}

/*
 * Session parameters. The Start layout (resolution, formats, rate control,
 * GOP, the per-session buffer table) is not described yet, so sessions cannot
 * start; everything around it is in place.
 */
int ave_cmd_start(struct ave_ctx *ctx, enum ave_codec codec)
{
	return -EOPNOTSUPP;
}

int ave_cmd_process(struct ave_ctx *ctx, enum ave_codec codec,
		    struct vb2_v4l2_buffer *src, struct vb2_v4l2_buffer *dst,
		    u32 *bytesused, bool *keyframe)
{
	return -EOPNOTSUPP;
}

/* Flush, stop and close the session. */
int ave_cmd_finish(struct ave_ctx *ctx, enum ave_codec codec)
{
	u32 tail = codec == AVE_CODEC_HEVC ? AVE_FINISH_ARG2_HEVC : AVE_FINISH_ARG2_H264;
	int ret, err;

	if (!ctx->session_open)
		return 0;

	ret = ave_cmd_simple(ctx, AVE_OP_COMPLETE, codec, 0, tail);
	err = ave_cmd_simple(ctx, AVE_OP_STOP, codec, AVE_SESSION_ARG0, tail);
	ret = ret ?: err;
	err = ave_cmd_simple(ctx, AVE_OP_CLOSE, codec, AVE_SESSION_ARG0, tail);
	ctx->session_open = false;
	return ret ?: err;
}
