/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_PROTOCOL_H__
#define __ANE_FW_PROTOCOL_H__

#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define ANE_FW_CHANNELS 8
#define ANE_FW_DESCRIPTOR_SIZE 0x100
#define ANE_FW_TABLE_SIZE (ANE_FW_CHANNELS * ANE_FW_DESCRIPTOR_SIZE)
#define ANE_FW_SLOT_SIZE 64
#define ANE_FW_TICK_RATE 24000000U

struct ane_fw_channel {
	u64 offset;
	u32 slots;
	u32 type;
	u32 bit;
	u32 index;
};

static inline bool ane_fw_contains(u64 base, u64 bytes, u64 addr, u64 size)
{
	return size <= bytes && addr >= base && addr - base <= bytes - size;
}

/* Parse a host-owned descriptor snapshot; output indices are logical names. */
static inline int
ane_fw_channels_parse(const void *table, u64 ipc_dva, size_t ipc_size,
		      u64 table_dva, struct ane_fw_channel channels[ANE_FW_CHANNELS])
{
	static const char * const names[ANE_FW_CHANNELS] = {
		"TERMINAL", "IO", "DEBUG", "BUF_H2T", "BUF_T2H",
		"SHAREDMALLOC", "IO_T2H", "DATA_CHAIN_H2T",
	};
	struct ane_fw_channel parsed[ANE_FW_CHANNELS] = {};
	u64 table_off;
	u32 seen = 0;
	unsigned int i, j;

	if (!table || !ane_fw_contains(ipc_dva, ipc_size, table_dva, ANE_FW_TABLE_SIZE))
		return -EPROTO;
	table_off = table_dva - ipc_dva;
	for (i = 0; i < ANE_FW_CHANNELS; i++) {
		const u8 *desc = (const u8 *)table + i * ANE_FW_DESCRIPTOR_SIZE;
		struct ane_fw_channel *c;
		u64 slots, ring, end;

		if (!memchr(desc, 0, 64))
			return -EPROTO;
		for (j = 0; j < ANE_FW_CHANNELS; j++)
			if (!strcmp((const char *)desc, names[j]))
				break;
		if (j == ANE_FW_CHANNELS || (seen & BIT(j)))
			return -EPROTO;
		seen |= BIT(j);
		c = &parsed[j];
		c->index = i;
		c->type = get_unaligned_le32(desc + 0x40);
		c->bit = get_unaligned_le32(desc + 0x44);
		slots = get_unaligned_le64(desc + 0x48);
		ring = get_unaligned_le64(desc + 0x50);
		if (c->type > 2 || c->bit >= 32 || !slots || slots > 0x400 ||
		    (ring & (ANE_FW_SLOT_SIZE - 1)) ||
		    !ane_fw_contains(ipc_dva, ipc_size, ring,
				     slots * ANE_FW_SLOT_SIZE))
			return -EPROTO;
		c->offset = ring - ipc_dva;
		c->slots = slots;
		end = c->offset + slots * ANE_FW_SLOT_SIZE;
		if (c->offset < table_off + ANE_FW_TABLE_SIZE && end > table_off)
			return -EPROTO;
	}
	if (seen != GENMASK(ANE_FW_CHANNELS - 1, 0) ||
	    parsed[1].type != 0 || parsed[5].type != 1)
		return -EPROTO;
	for (i = 0; i < ANE_FW_CHANNELS; i++) {
		u64 end = parsed[i].offset + parsed[i].slots * ANE_FW_SLOT_SIZE;

		for (j = i + 1; j < ANE_FW_CHANNELS; j++)
			if (parsed[i].offset < parsed[j].offset +
					       parsed[j].slots * ANE_FW_SLOT_SIZE &&
			    parsed[j].offset < end)
				return -EPROTO;
	}
	memcpy(channels, parsed, sizeof(parsed));
	return 0;
}

/* Call only after parsing the table and checking its immutable snapshot. */
static inline void
ane_fw_channels_host_init(void *ipc, const struct ane_fw_channel *channels)
{
	unsigned int i, slot;

	for (i = 0; i < ANE_FW_CHANNELS; i++) {
		const struct ane_fw_channel *c = &channels[i];

		if (c->type)
			continue;
		for (slot = 0; slot < c->slots; slot++) {
			u8 *p = (u8 *)ipc + c->offset + slot * ANE_FW_SLOT_SIZE;

			memset(p, 0, 24);
			put_unaligned_le64(1, p);
		}
	}
}

/* H14 preserves its reserved word; H16 requires bytes 0..3 to be zero. */
static inline void ane_fw_command_header(void *command, u16 opcode, bool zero_reserved)
{
	u8 *p = command;

	if (zero_reserved)
		put_unaligned_le32(0, p);
	put_unaligned_le16(opcode, p + 4);
	put_unaligned_le16(0, p + 6);
}

/* A firmware refusal is distinct from corruption and never means success. */
static inline int
ane_fw_reply_status(u64 header, u64 slot_length, u64 slot_result,
		    u64 command_dva, size_t length, u16 opcode, const void *command,
		    bool zero_reserved)
{
	const u8 *p = command;

	if (!(header & 1))
		return -EAGAIN;
	if (!command || length < 8 || (command_dva & 1) || header != (command_dva | 1) ||
	    slot_length != length || slot_result ||
	    (zero_reserved && get_unaligned_le32(p)) ||
	    get_unaligned_le16(p + 4) != opcode)
		return -EPROTO;
	return get_unaligned_le16(p + 6) ? -EREMOTEIO : 0;
}

static inline int ane_fw_config_status(const void *command, size_t length)
{
	if (!command || length != 16 || get_unaligned_le32((const u8 *)command + 8) !=
			    ANE_FW_TICK_RATE)
		return -EPROTO;
	return 0;
}

/* Legacy H14/comparator layout only; H16 27.0 reports are 0x60 bytes. */
static inline int
ane_fw_finished_event(const void *event, size_t length, u64 cookie,
		      u32 program, u32 process)
{
	const u8 *p = event;
	u32 state;

	if (!p || length != 0x28 || get_unaligned_le32(p + 4) != 0x300 ||
	    get_unaligned_le64(p + 8) != cookie ||
	    get_unaligned_le32(p + 0x10) != program ||
	    get_unaligned_le32(p + 0x14) != process ||
	    get_unaligned_le32(p + 0x18))
		return -EPROTO;
	state = get_unaligned_le32(p + 0x1c);
	return state > 1 ? -EPROTO : state;
}

enum ane_fw_rtk_phase {
	ANE_FW_RTK_HELLO,
	ANE_FW_RTK_EPMAP,
	ANE_FW_RTK_IOP_ACK,
	ANE_FW_RTK_AP_ACK,
	ANE_FW_RTK_READY,
};

struct ane_fw_rtk {
	u32 endpoints[8];
	u8 fragments;
	enum ane_fw_rtk_phase phase;
};

#define ANE_FW_RTK_TYPE(t) ((u64)(t) << 52)
#define ANE_FW_RTK_LAST BIT_ULL(51)

/* The caller owns one absolute deadline for the complete management session. */
static inline int
ane_fw_rtk_receive(struct ane_fw_rtk *rtk, u64 word0, u64 word1,
		   void *ctx, int (*send)(void *ctx, u64 word0, u64 word1))
{
	static const u8 system_eps[] = { 1, 2, 3, 4, 8, 10 };
	u32 type = (word0 >> 52) & 0xff;
	unsigned int base, ep, i;
	int ret;

	if (word1 & 0xff)
		return -EOPNOTSUPP;
	switch (type) {
	case 1:
		if (rtk->phase != ANE_FW_RTK_HELLO || (word0 & 0xffff) > 12 ||
		    ((word0 >> 16) & 0xffff) < 12)
			return -EPROTO;
		ret = send(ctx, ANE_FW_RTK_TYPE(2) | (12 << 16) | 12, 0);
		if (!ret)
			rtk->phase = ANE_FW_RTK_EPMAP;
		return ret;
	case 8:
		if (rtk->phase != ANE_FW_RTK_EPMAP)
			return -EPROTO;
		base = (word0 >> 32) & 7;
		if (rtk->fragments & BIT(base))
			return -EPROTO;
		rtk->fragments |= BIT(base);
		rtk->endpoints[base] = word0;
		ret = send(ctx, ANE_FW_RTK_TYPE(8) | ((u64)base << 32) |
			   (word0 & ANE_FW_RTK_LAST ? ANE_FW_RTK_LAST : 1), 0);
		if (ret || !(word0 & ANE_FW_RTK_LAST))
			return ret;
		for (i = 0; i < ARRAY_SIZE(system_eps); i++) {
			ep = system_eps[i];
			if (!(rtk->endpoints[0] & BIT(ep)))
				continue;
			ret = send(ctx, ANE_FW_RTK_TYPE(5) | ((u64)ep << 32) | 2, 0);
			if (ret)
				return ret;
		}
		ret = send(ctx, ANE_FW_RTK_TYPE(6) | 0x20, 0);
		if (!ret)
			rtk->phase = ANE_FW_RTK_IOP_ACK;
		return ret;
	case 7:
		if (rtk->phase != ANE_FW_RTK_IOP_ACK || (word0 & 0xffff) != 0x20)
			return -EPROTO;
		ret = send(ctx, ANE_FW_RTK_TYPE(0xb) | 0x20, 0);
		if (!ret)
			rtk->phase = ANE_FW_RTK_AP_ACK;
		return ret;
	case 0xb:
		if (rtk->phase != ANE_FW_RTK_AP_ACK || (word0 & 0xffff) != 0x20)
			return -EPROTO;
		for (ep = 0x20; ep < 0x100; ep++) {
			if (!(rtk->endpoints[ep / 32] & BIT(ep % 32)))
				continue;
			ret = send(ctx, ANE_FW_RTK_TYPE(5) | ((u64)ep << 32) | 2, 0);
			if (ret)
				return ret;
		}
		rtk->phase = ANE_FW_RTK_READY;
		return 0;
	case 0xc:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

#endif /* __ANE_FW_PROTOCOL_H__ */
