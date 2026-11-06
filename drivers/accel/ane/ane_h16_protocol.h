/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_PROTOCOL_H__
#define __ANE_H16_PROTOCOL_H__

#include "ane_fw_protocol.h"

/* Derived field facts from H16/macOS 27.0, 2026-10-31. No payload bytes. */
enum ane_h16_command {
	ANE_H16_LOAD_PROGRAM = 0x0200,
	ANE_H16_UNLOAD_PROGRAM = 0x0201,
	ANE_H16_CREATE_PROCESS = 0x0202,
	ANE_H16_TERMINATE_PROCESS = 0x0203,
	ANE_H16_EXECUTE = 0x020c,
	ANE_H16_POWER_GATE = 0x002d,
	ANE_H16_EVENT_REPORT = 0x0300,
};

struct ane_h16_command_info {
	u16 id;
	u16 min_size;
	u16 max_size;
	u8 size_step;
	u8 channel;
	bool from_firmware;
};

static inline const struct ane_h16_command_info *ane_h16_command_info(u16 id)
{
	static const struct ane_h16_command_info commands[] = {
		{ ANE_H16_LOAD_PROGRAM, 0x1c0, 0x1c0, 1, 1, false },
		{ ANE_H16_UNLOAD_PROGRAM, 0xc, 0xc, 1, 1, false },
		{ ANE_H16_CREATE_PROCESS, 0x258, 0x258, 1, 1, false },
		{ ANE_H16_TERMINATE_PROCESS, 0x10, 0x10, 1, 1, false },
		{ ANE_H16_EXECUTE, 0xca8, 0xcd8, 0x18, 7, false },
		{ ANE_H16_POWER_GATE, 0xc, 0xc, 1, 7, false },
		{ ANE_H16_EVENT_REPORT, 0x60, 0x60, 1, 6, true },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		if (commands[i].id == id)
			return &commands[i];
	return NULL;
}

/* Validate the runtime wire shape, never infer a body from the H14 ABI. */
static inline int
ane_h16_command_prepare(void *command, size_t length, u16 id, unsigned int channel)
{
	const struct ane_h16_command_info *info = ane_h16_command_info(id);

	if (!info || info->from_firmware)
		return -EOPNOTSUPP;
	if (!command || channel != info->channel || length < info->min_size ||
	    length > info->max_size || (length - info->min_size) % info->size_step)
		return -EINVAL;
	if (id == ANE_H16_POWER_GATE &&
	    get_unaligned_le32((u8 *)command + 8) > 1)
		return -EINVAL;
	ane_fw_command_header(command, id, true);
	return 0;
}

/* The first word selects report slots; rolling words are also valid. */
static inline int
ane_h16_event_report_status(const void *event, size_t length, unsigned int channel)
{
	const u8 *p = event;
	const struct ane_h16_command_info *info =
		ane_h16_command_info(ANE_H16_EVENT_REPORT);

	if (!p || channel != info->channel || length != info->min_size ||
	    get_unaligned_le16(p + 4) != info->id || get_unaligned_le16(p + 6))
		return -EPROTO;
	return 0;
}

#endif /* __ANE_H16_PROTOCOL_H__ */
