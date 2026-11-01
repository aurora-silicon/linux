// SPDX-License-Identifier: GPL-2.0-only
/* Firmware command envelopes and transport descriptions. */
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/unaligned.h>

#include "ave.h"
#include "ave-regs.h"

static const struct ave_fw_transport ave_transport_9003 = {
	.ipc_bytes = AVE_IPC_BYTES,
	.command_offset = AVE_IPC_CMD_OFF,
	.command_bytes = AVE_IPC_CMD_BYTES,
	.scalar_offset = AVE_IPC_SCALAR_OFF,
	.process_offset = AVE_IPC_PROC_OFF,
	.process_bytes = AVE_IPC_PROC_BYTES,
	.hello = { 2, 0, 0x100, 0 },
	.descriptors = 2,
	.io_index = 0,
	.t2h_index = 1,
	.boot_config = true,
	.sync_host_clock = true,
};

static const struct ave_fw_transport ave_transport_6070 = {
	.ipc_bytes = 0x700000,
	.command_offset = 0x40000,
	.command_bytes = 0x34000,
	.scalar_offset = 0x74000,
	.process_offset = 0x78000,
	.process_bytes = 0x10000,
	.hello = { 7, 0x9bc0, 0x100, 0 },
	.descriptors = 7,
	.io_index = 1,
	.t2h_index = 5,
	.slots = { 32, 8 },
	.doorbells = { 1, 3 },
	.offsets = { 0x8700, 0x9180 },
	.scratch_halt = true,
};

static void ave_header_9003(u8 *cmd, const struct ave_fw_abi *abi,
			    const struct ave_command *h)
{
	put_unaligned_le32(abi->wire_id[h->codec - 1][h->op], cmd);
	put_unaligned_le64(h->token, cmd + 0x08);
	put_unaligned_le64(h->client, cmd + 0x10);
	if (h->op != AVE_OP_CONFIG && h->op != AVE_OP_HALT) {
		put_unaligned_le32(h->unit ?: AVE_SESSION_UNIT, cmd + 0x18);
		put_unaligned_le32(h->codec, cmd + 0x1c);
		put_unaligned_le32(h->arg0, cmd + 0x24);
		put_unaligned_le32(h->arg1 ?: AVE_SESSION_ARG1, cmd + 0x28);
		put_unaligned_le32(h->arg2, cmd + 0x2c);
	}
	put_unaligned_le32(h->subtype, cmd + 0x20);
}

static void ave_header_6070(u8 *cmd, const struct ave_fw_abi *abi,
			    const struct ave_command *h)
{
	bool global = h->op == AVE_OP_CONFIG || h->op == AVE_OP_HALT;

	put_unaligned_le16(abi->wire_id[h->codec - 1][h->op], cmd);
	put_unaligned_le64(h->token, cmd + 0x08);
	if (!global) {
		put_unaligned_le32(h->client, cmd + 0x10);
		put_unaligned_le32(h->codec - 1, cmd + 0x18);
	}
	put_unaligned_le32(h->op == AVE_OP_PROCESS ? h->subtype :
						     abi->semantic[h->op],
			   cmd + 0x1c);
	put_unaligned_le32(global ? 0 : 0xc8, cmd + 0x20);
}

static void ave_config_9003(struct ave_dev *ave, u8 *cmd)
{
	ave_write_global(ave, cmd, AVE_OP_CONFIG);
	cmd[0x49] = 1;
	cmd[0x4a] = 1;
	put_unaligned_le32(ave->layout->mcc_dsid, cmd + 0x60);
}

static void ave_config_6070(struct ave_dev *ave, u8 *cmd)
{
	ave_write_global(ave, cmd, AVE_OP_CONFIG);
	cmd[0x41] = 1;
	put_unaligned_le64(ave->extra0_dma, cmd + 0x48);
	/* No MCC, host heap, shared area or optional firmware log. */
}

static int ave_reply_9003(const struct ave_fw_abi *abi, const u8 *msg, u32 len,
			  struct ave_reply *reply)
{
	u32 id;

	if (len < 0x48)
		return -EPROTO;
	id = get_unaligned_le16(msg);
	reply->op = id < AVE_OP_COUNT ? id : 0;
	reply->client = get_unaligned_le64(msg + 0x10);
	reply->subtype = get_unaligned_le32(msg + 0x20);
	reply->raw_status = get_unaligned_le32(msg + 0x40);
	reply->status = reply->raw_status;
	reply->bytes = len;
	return 0;
}

static int ave_reply_6070(const struct ave_fw_abi *abi, const u8 *msg, u32 len,
			  struct ave_reply *reply)
{
	u32 id;

	if (len < 0x40)
		return -EPROTO;
	id = get_unaligned_le16(msg);
	for (unsigned int op = 1; op < AVE_OP_COUNT; op++) {
		if (!abi->reply_id[op] || abi->reply_id[op] != id)
			continue;
		if (len != (op == AVE_OP_PROCESS ? 0x48 : 0x40))
			return -EPROTO;
		reply->op = op;
		reply->client = get_unaligned_le32(msg + 0x10);
		reply->subtype = get_unaligned_le32(msg + 0x1c);
		reply->raw_status = get_unaligned_le32(msg + 0x38);
		reply->status = reply->raw_status;
		reply->bytes = len;
		if (reply->status == 0x00ee0000)
			reply->status = 0;
		else if (!reply->status)
			reply->status = U32_MAX;
		return 0;
	}
	return -EPROTO;
}

#define AVE_9003_IDS                                                      \
	{ [AVE_OP_CONFIG] = 1,	[AVE_OP_HALT] = 2,     [AVE_OP_OPEN] = 3, \
	  [AVE_OP_CLOSE] = 4,	[AVE_OP_START] = 6,    [AVE_OP_STOP] = 7, \
	  [AVE_OP_PROCESS] = 8, [AVE_OP_COMPLETE] = 9, [AVE_OP_PRIORITY] = 10 }

const struct ave_fw_abi ave_abi_9003 = {
	.transport = &ave_transport_9003,
	.wire_id = { AVE_9003_IDS, AVE_9003_IDS },
	.reply_id = AVE_9003_IDS,
	.scalar_bytes = {
		[AVE_OP_CONFIG] = 0x78,
		[AVE_OP_HALT] = 0x48,
		[AVE_OP_OPEN] = 0x48,
		[AVE_OP_CLOSE] = 0x48,
		[AVE_OP_STOP] = 0x48,
		[AVE_OP_COMPLETE] = 0x48,
		[AVE_OP_PRIORITY] = 0x48,
	},
	.header_bytes = 0x48,
	.timeout_offset = 0x30,
	.created_offset = 0x38,
	.padded_surfaces = true,
	.write_header = ave_header_9003,
	.write_config = ave_config_9003,
	.parse_reply = ave_reply_9003,
};

#define AVE_6070_IDS(start, process)                            \
	{ [AVE_OP_CONFIG] = 1,	  [AVE_OP_OPEN] = 2,            \
	  [AVE_OP_RESET] = 3,	  [AVE_OP_START] = (start),     \
	  [AVE_OP_STOP] = 6,	  [AVE_OP_PROCESS] = (process), \
	  [AVE_OP_FLUSH] = 11,	  [AVE_OP_CLOSE] = 12,          \
	  [AVE_OP_COMPLETE] = 13, [AVE_OP_HALT] = 14 }

const struct ave_fw_abi ave_abi_6070 = {
	.transport = &ave_transport_6070,
	.wire_id = { AVE_6070_IDS(4, 7), AVE_6070_IDS(5, 8) },
	.reply_id = {
		[AVE_OP_CONFIG] = 0xe01,
		[AVE_OP_OPEN] = 0xe02,
		[AVE_OP_RESET] = 0xe03,
		[AVE_OP_START] = 0xe04,
		[AVE_OP_STOP] = 0xe05,
		[AVE_OP_PROCESS] = 0xe06,
		[AVE_OP_FLUSH] = 0xe09,
		[AVE_OP_CLOSE] = 0xe0a,
		[AVE_OP_COMPLETE] = 0xe0b,
	},
	.scalar_bytes = {
		[AVE_OP_CONFIG] = 0x70,
		[AVE_OP_OPEN] = 0x40,
		[AVE_OP_STOP] = 0x40,
		[AVE_OP_FLUSH] = 0x40,
		[AVE_OP_CLOSE] = 0x48,
		[AVE_OP_COMPLETE] = 0x40,
		[AVE_OP_HALT] = 0x40,
	},
	.semantic = {
		[AVE_OP_CONFIG] = U32_MAX,
		[AVE_OP_OPEN] = 3,
		[AVE_OP_RESET] = 11,
		[AVE_OP_START] = 6,
		[AVE_OP_STOP] = 7,
		[AVE_OP_FLUSH] = 10,
		[AVE_OP_CLOSE] = 4,
		[AVE_OP_COMPLETE] = 9,
		[AVE_OP_HALT] = U32_MAX,
	},
	.header_bytes = 0x40,
	.timeout_offset = 0x28,
	.created_offset = 0x30,
	.write_header = ave_header_6070,
	.write_config = ave_config_6070,
	.parse_reply = ave_reply_6070,
};
