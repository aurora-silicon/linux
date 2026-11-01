/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_FW_COLD_H__
#define __ANE_FW_COLD_H__

#include "ane_fw_transport.h"
#include "ane_h16_execute.h"

#define ANE_FW_DARTS 3
#define ANE_FW_DAPF_RANGES 4
#define ANE_FW_LOAD_SIZE 0x1c0
#define ANE_FW_CREATE_SIZE 0x258
#define ANE_FW_KERNEL_INDEX 1007

struct ane_fw_cold_state {
	u32 sys, cpu, subblock, gate_request, gate_ack;
	u64 ce_state, ce_error, rvbar;
	u32 control, status, pending, scratch[8];
	struct {
		u32 version, protect, unprotect, lock, error;
		u32 enabled[16];
	} dart[ANE_FW_DARTS];
};

/* DAPF records and invalid TTBR payloads are random at reset, not markers. */
static inline bool ane_fw_cold_valid(const struct ane_fw_cold_state *s,
				     bool raw_boot)
{
	unsigned int i, j;

	if (!raw_boot || (s->sys & 0xff) != 0xff || (s->cpu & 0xff) ||
	    s->subblock != 0x300 || s->gate_request != 3 || s->gate_ack ||
	    (s->ce_state & 3) || s->ce_error || s->control || s->status != 0x6a ||
	    s->rvbar != 0x10000000001ULL || s->pending)
		return false;
	for (i = 0; i < ARRAY_SIZE(s->scratch); i++)
		if (s->scratch[i])
			return false;
	for (i = 0; i < ANE_FW_DARTS; i++) {
		if ((s->dart[i].version & 0xffff) != 0x203 ||
		    (s->dart[i].protect & BIT(0)) ||
		    (s->dart[i].lock & (BIT(0) | BIT(2))) || s->dart[i].error)
			return false;
		for (j = 0; j < ARRAY_SIZE(s->dart[i].enabled); j++)
			if (s->dart[i].enabled[j])
				return false;
	}
	return true;
}

struct ane_fw_load_section {
	struct ane_fw_surface surface;
	u32 index;
};

/* Inputs have already been built from the validated original HWX by the
 * load frontend. This is an internal interface, never a section-upload ABI.
 */
struct ane_fw_program {
	struct ane_fw_load_section sections[9];
	struct ane_h16_execute call;
	bool kernel_table;
	u32 kernel_bar;
};

/* Kernel-table references must use the LOAD kernel resource index. A legacy
 * H14 tag of 2 asserts the H17 firmware. Reject before publishing LOAD.
 * Record geometry follows the public MIT libane operation format.
 */
static inline int ane_fw_operation_validate(const struct ane_fw_program *program)
{
	const struct ane_fw_surface *s;
	const u8 *p;
	u32 records, count, tag, slot;
	unsigned int i, j, k;
	bool kernel_seen = false;

	if (!program || program->call.buffer_count > ANE_H16_MAX_BUFFERS ||
	    (program->call.buffer_count && !program->call.buffers))
		return -EINVAL;
	s = &program->sections[3].surface;
	p = s->cpu;
	if (!p || s->size < 4)
		return -EINVAL;
	records = get_unaligned_le32(p);
	if (!records || records > 64 || s->size != 4 + (u64)records * 0x40c)
		return -EINVAL;
	if (program->kernel_table &&
	    (program->sections[1].index != ANE_FW_KERNEL_INDEX ||
	     program->kernel_bar >= 64))
		return -EINVAL;
	for (i = 0; i < records; i++) {
		const u8 *r = p + 4 + i * 0x40c;
		u64 seen = 0;

		count = get_unaligned_le32(r + 8);
		if (get_unaligned_le32(r) || get_unaligned_le32(r + 4) || count > 64)
			return -EINVAL;
		for (j = 0; j < count; j++) {
			slot = get_unaligned_le32(r + 0xc + j * 8);
			tag = get_unaligned_le32(r + 0x10 + j * 8);
			if (slot >= 64 || (seen & BIT_ULL(slot)) || tag == 2)
				return -EINVAL;
			seen |= BIT_ULL(slot);
			if (program->kernel_table && slot == program->kernel_bar) {
				if (tag != program->sections[1].index)
					return -EINVAL;
				kernel_seen = true;
				continue;
			}
			for (k = 0; k < program->call.buffer_count; k++)
				if (tag == program->call.buffers[k].index)
					break;
			if (k == program->call.buffer_count)
				return -EINVAL;
		}
	}
	return program->kernel_table && !kernel_seen ? -EINVAL : 0;
}

static inline int ane_fw_load_encode(void *command, const struct ane_fw_program *p)
{
	u8 *bytes = command;
	unsigned int i;
	int ret;

	if (!command)
		return -EINVAL;
	ret = ane_fw_operation_validate(p);
	if (ret)
		return ret;
	memset(bytes, 0, ANE_FW_LOAD_SIZE);
	put_unaligned_le16(ANE_H16_LOAD_PROGRAM, bytes + 4);
	for (i = 0; i < ARRAY_SIZE(p->sections); i++) {
		const struct ane_fw_load_section *s = &p->sections[i];
		u8 *d = bytes + 8 + i * 0x30;

		if (!s->surface.size)
			continue;
		if (!ane_fw_surface_valid(&s->surface))
			return -EINVAL;
		put_unaligned_le32(1, d);
		put_unaligned_le32(s->index, d + 4);
		put_unaligned_le32(i == 1 ? 3 : 4, d + 8);
		put_unaligned_le32(1, d + 0x10);
		put_unaligned_le64(s->surface.dva, d + 0x18);
		put_unaligned_le64(s->surface.size, d + 0x20);
	}
	put_unaligned_le32(U32_MAX, bytes + 0x1b8);
	return 0;
}

int ane_fw_staged_init(struct platform_device *pdev, const struct ane_fw_platform *soc);
bool ane_fw_staged_exit(void);

/* Called only with immutable, backend-owned sections/resources and expected
 * output bytes. A terminal event alone never establishes output validity.
 */
int ane_fw_program_run(struct ane_fw_program *program,
		       const struct ane_fw_surface *output, const void *expected);
int ane_fw_program_alloc(size_t size, struct ane_fw_surface *surface);

#endif /* __ANE_FW_COLD_H__ */
