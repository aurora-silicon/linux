// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* H16 program lifecycle on the shared firmware transport. */
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "ane_h16_program.h"

/*
 * The one LOAD slot table. Slot 1 carries the HWX kernel section, or the
 * __TEXT,__const constants when the program has no kernel section, and slot 2
 * the task stream. macOS populated slot 1 in every captured load, with at
 * least 0x4000 bytes at a sub-page DVA, which is the constants section of a
 * program without a kernel. Slots 1 and 2 carry a per-program pair with
 * slot 1 = slot 2 + 1. A later correction to any slot is one line here.
 */
static const struct ane_h16_slot_rule ane_h16_slot_rules[ANE_H16_LOAD_SLOTS] = {
	{ ANE_H16_SLOT_AUX_GENERIC, ANE_H16_RESOURCE_GENERIC, 4 },
	{ ANE_H16_SLOT_KERNEL, ANE_H16_RESOURCE_KERNEL, 3 },
	{ ANE_H16_SLOT_TEXT, ANE_H16_RESOURCE_TEXT, 4 },
	{ ANE_H16_SLOT_AUX_OPERATION, ANE_H16_RESOURCE_OPERATION, 4 },
	{ ANE_H16_SLOT_AUX_PROCEDURE, ANE_H16_RESOURCE_PROCEDURE, 4 },
	{ ANE_H16_SLOT_EMPTY, 0, 0 },
	{ ANE_H16_SLOT_AUX_TEXT_PROPERTIES, ANE_H16_RESOURCE_TEXT_PROPERTIES, 4 },
	{ ANE_H16_SLOT_EMPTY, 0, 0 },
	{ ANE_H16_SLOT_EMPTY, 0, 0 },
};

const struct ane_h16_slot_rule *ane_h16_slot_rule(unsigned int slot)
{
	return slot < ANE_H16_LOAD_SLOTS ? &ane_h16_slot_rules[slot] : NULL;
}
EXPORT_SYMBOL_GPL(ane_h16_slot_rule);

/* Place a section that lies wholly inside one mapped region of that kind. */
static int h16_section_dva(const struct ane_h16_load_plan *plan,
			   const struct ane_h16_load_layout *layout,
			   enum ane_h16_region_kind kind,
			   const struct ane_h16_hwx_section *section,
			   struct ane_h16_load_slot *slot)
{
	unsigned int i;

	if (!section->size)
		return -ENOEXEC;
	for (i = 0; i < plan->count; i++) {
		const struct ane_h16_hwx_region *r = &plan->regions[i];
		u64 offset = section->vm - r->vm;

		if (r->kind != kind || section->vm < r->vm || offset >= r->size ||
		    section->size > r->size - offset)
			continue;
		if (offset > layout->regions[i].size ||
		    section->size > layout->regions[i].size - offset)
			return -EINVAL;
		slot->dva = layout->regions[i].dva + offset;
		slot->size = section->size;
		return 0;
	}
	return -ENOEXEC;
}

int ane_h16_load_slots(const struct ane_h16_hwx *hwx,
		       const struct ane_h16_load_plan *plan,
		       const struct ane_h16_load_layout *layout,
		       struct ane_h16_load_slot slots[ANE_H16_LOAD_SLOTS])
{
	struct ane_h16_load_slot next[ANE_H16_LOAD_SLOTS] = {};
	unsigned int i, aux;
	int ret;

	if (!hwx || !plan || !layout || !slots || !plan->count ||
	    layout->region_count != plan->count)
		return -EINVAL;
	for (i = 0; i < plan->count; i++)
		if (!layout->regions[i].dva || layout->regions[i].size != plan->allocation[i])
			return -EINVAL;
	for (i = 0; i < ANE_H16_LOAD_SLOTS; i++) {
		const struct ane_h16_slot_rule *rule = &ane_h16_slot_rules[i];
		struct ane_h16_load_slot *slot = &next[i];

		switch (rule->source) {
		case ANE_H16_SLOT_EMPTY:
			continue;
		case ANE_H16_SLOT_KERNEL:
			if (hwx->kernel.size)
				ret = h16_section_dva(plan, layout, ANE_H16_REGION_KERNEL,
						      &hwx->kernel, slot);
			else
				ret = h16_section_dva(plan, layout, ANE_H16_REGION_TEXT,
						      &hwx->constants, slot);
			break;
		case ANE_H16_SLOT_TEXT:
			ret = h16_section_dva(plan, layout, ANE_H16_REGION_TEXT,
					      &hwx->text, slot);
			break;
		default:
			aux = rule->source == ANE_H16_SLOT_AUX_GENERIC ? ANE_H16_AUX_GENERIC :
			      rule->source == ANE_H16_SLOT_AUX_OPERATION ? ANE_H16_AUX_OPERATION :
			      rule->source == ANE_H16_SLOT_AUX_PROCEDURE ? ANE_H16_AUX_PROCEDURE :
			      ANE_H16_AUX_TEXT_PROPERTIES;
			ret = 0;
			slot->dva = layout->aux_dva[aux];
			slot->size = layout->aux_size[aux];
			if (!slot->dva || slot->size < ANE_H16_AUX_MIN ||
			    slot->size > ANE_H16_AUX_MAX)
				ret = -EINVAL;
			break;
		}
		if (ret)
			return ret;
		slot->resource = rule->resource;
		slot->class = rule->class;
		slot->present = true;
	}
	memcpy(slots, next, sizeof(next));
	return 0;
}
EXPORT_SYMBOL_GPL(ane_h16_load_slots);

int ane_h16_load_encode(void *command, size_t length,
			const struct ane_h16_load_slot slots[ANE_H16_LOAD_SLOTS])
{
	u8 *p = command;
	unsigned int i;

	if (!p || !slots || length != ANE_H16_LOAD_SIZE)
		return -EINVAL;
	for (i = 0; i < ANE_H16_LOAD_SLOTS; i++)
		if (slots[i].present &&
		    (!slots[i].size || slots[i].size > U64_MAX - slots[i].dva))
			return -EINVAL;
	memset(p, 0, length);
	for (i = 0; i < ANE_H16_LOAD_SLOTS; i++) {
		u8 *d = p + ANE_H16_LOAD_SLOT_OFFSET + i * ANE_H16_LOAD_SLOT_SIZE;

		if (!slots[i].present)
			continue;
		put_unaligned_le32(1, d);
		put_unaligned_le32(slots[i].resource, d + 4);
		put_unaligned_le32(slots[i].class, d + 8);
		put_unaligned_le32(1, d + 0x10);
		put_unaligned_le64(slots[i].dva, d + 0x18);
		put_unaligned_le64(slots[i].size, d + 0x20);
	}
	put_unaligned_le32(ANE_H16_LOAD_ID_REQUEST, p + ANE_H16_LOAD_ID_OFFSET);
	return ane_h16_command_prepare(p, length, ANE_H16_LOAD_PROGRAM, ANE_FW_IO);
}
EXPORT_SYMBOL_GPL(ane_h16_load_encode);

static int h16_reply_header(const u8 *reply, size_t length, size_t expected, u16 id)
{
	if (!reply || length != expected || get_unaligned_le32(reply) ||
	    get_unaligned_le16(reply + 4) != id)
		return -EPROTO;
	return get_unaligned_le16(reply + 6) ? -EREMOTEIO : 0;
}

/* The reply echoes every slot and replaces the request ID with the program ID. */
int ane_h16_load_reply(const void *reply, size_t length, const void *request,
		       u32 *program)
{
	const u8 *p = reply, *q = request;
	u32 id;
	int ret;

	if (!program || !q)
		return -EINVAL;
	ret = h16_reply_header(p, length, ANE_H16_LOAD_SIZE, ANE_H16_LOAD_PROGRAM);
	if (ret)
		return ret;
	id = get_unaligned_le32(p + ANE_H16_LOAD_ID_OFFSET);
	if (id == ANE_H16_LOAD_ID_REQUEST || get_unaligned_le32(p + 0x1bc) ||
	    memcmp(p + ANE_H16_LOAD_SLOT_OFFSET, q + ANE_H16_LOAD_SLOT_OFFSET,
		   ANE_H16_LOAD_ID_OFFSET - ANE_H16_LOAD_SLOT_OFFSET))
		return -EPROTO;
	/* ID 0 is a valid program ID. */
	*program = id;
	return 0;
}
EXPORT_SYMBOL_GPL(ane_h16_load_reply);

int ane_h16_create_encode(void *command, size_t length, u32 program)
{
	u8 *p = command;

	if (!p || length != ANE_H16_CREATE_SIZE)
		return -EINVAL;
	/* No process resources travel in CREATE on 27.0; the body stays zero. */
	memset(p, 0, length);
	put_unaligned_le32(program, p + 8);
	return ane_h16_command_prepare(p, length, ANE_H16_CREATE_PROCESS, ANE_FW_IO);
}
EXPORT_SYMBOL_GPL(ane_h16_create_encode);

int ane_h16_create_reply(const void *reply, size_t length, u32 program, u32 *process)
{
	const u8 *p = reply;
	int ret;

	if (!process)
		return -EINVAL;
	ret = h16_reply_header(p, length, ANE_H16_CREATE_SIZE, ANE_H16_CREATE_PROCESS);
	if (ret)
		return ret;
	if (get_unaligned_le32(p + 8) != program ||
	    memchr_inv(p + 0x10, 0, ANE_H16_CREATE_SIZE - 0x10))
		return -EPROTO;
	/* ID 0 is a valid process ID. */
	*process = get_unaligned_le32(p + 0xc);
	return 0;
}
EXPORT_SYMBOL_GPL(ane_h16_create_reply);

int ane_h16_terminate_encode(void *command, size_t length, u32 program, u32 process)
{
	u8 *p = command;

	if (!p || length != ANE_H16_TERMINATE_SIZE)
		return -EINVAL;
	memset(p, 0, length);
	put_unaligned_le32(program, p + 8);
	put_unaligned_le32(process, p + 0xc);
	return ane_h16_command_prepare(p, length, ANE_H16_TERMINATE_PROCESS, ANE_FW_IO);
}
EXPORT_SYMBOL_GPL(ane_h16_terminate_encode);

int ane_h16_unload_encode(void *command, size_t length, u32 program)
{
	u8 *p = command;

	if (!p || length != ANE_H16_UNLOAD_SIZE)
		return -EINVAL;
	memset(p, 0, length);
	put_unaligned_le32(program, p + 8);
	return ane_h16_command_prepare(p, length, ANE_H16_UNLOAD_PROGRAM, ANE_FW_IO);
}
EXPORT_SYMBOL_GPL(ane_h16_unload_encode);

int ane_h16_gate_encode(void *command, size_t length, u32 value)
{
	u8 *p = command;

	if (!p || length != ANE_H16_GATE_SIZE || value > 1)
		return -EINVAL;
	memset(p, 0, length);
	put_unaligned_le32(value, p + 8);
	return ane_h16_command_prepare(p, length, ANE_H16_POWER_GATE, ANE_FW_DATA_CHAIN);
}
EXPORT_SYMBOL_GPL(ane_h16_gate_encode);

/*
 * Short commands (0x002d, TERMINATE, UNLOAD) have no recorded reply body, so
 * only the transport reply shape is a predicate: same length and command,
 * zero first word and a zero acknowledgment.
 */
int ane_h16_id_reply(const void *reply, size_t length, const void *request)
{
	const u8 *q = request;

	if (!q || length < 8 || length > ANE_H16_TERMINATE_SIZE)
		return -EINVAL;
	return h16_reply_header(reply, length, length, get_unaligned_le16(q + 4));
}
EXPORT_SYMBOL_GPL(ane_h16_id_reply);

/*
 * Kernel-owned EXECUTE resource map. EXECUTE names each buffer by its ID in
 * the generic section that LOAD installs, so the ID comes from that section:
 * entry k describes surface k in HWX surface order. The entry must agree with
 * the surface direction and size. IDs are unique and never the reserved
 * kernel IDs 2 and 3. The class follows the round 5 usage mapping (input
 * usage 1, output usage 2).
 */
int ane_h16_process_bindings(const struct ane_h16_hwx *hwx, const void *generic,
			     size_t generic_size,
			     struct ane_h16_process_binding *bindings,
			     unsigned int *count)
{
	const u8 *g = generic;
	u32 seen[2] = {};
	unsigned int i, j;

	if (!hwx || !g || !bindings || !count || hwx->surface_count < 2 ||
	    hwx->surface_count > ANE_H16_SURFACES)
		return -EINVAL;
	if (generic_size != ANE_H16_GENERIC_HEADER +
			    hwx->surface_count * ANE_H16_GENERIC_ENTRY ||
	    get_unaligned_le32(g) != ANE_H16_GENERIC_MAGIC ||
	    !get_unaligned_le32(g + 4) ||
	    get_unaligned_le32(g + 4) > ANE_H16_GENERIC_VERSION ||
	    get_unaligned_le32(g + ANE_H16_GENERIC_COUNT) != hwx->surface_count)
		return -ENOEXEC;
	for (i = 0; i < hwx->surface_count; i++) {
		const struct ane_h16_hwx_surface *s = &hwx->surfaces[i];
		const u8 *e = g + ANE_H16_GENERIC_HEADER + i * ANE_H16_GENERIC_ENTRY;
		u32 id = get_unaligned_le32(e + 4);
		u64 bytes = get_unaligned_le64(e + 0x20);

		if (s->direction < 1 || s->direction > 2)
			return -ENOEXEC;
		if (get_unaligned_le32(e) != 1 || id == 2 || id == 3 ||
		    id > ANE_H16_GENERIC_ID_MAX ||
		    get_unaligned_le32(e + 8) != s->direction - 1 ||
		    get_unaligned_le32(e + 0x10) != s->direction ||
		    bytes < s->span || bytes > s->allocation)
			return -ENOEXEC;
		for (j = 0; j < i; j++)
			if (bindings[j].index == id)
				return -ENOEXEC;
		bindings[i] = (struct ane_h16_process_binding) {
			.hwx_vm = s->section.vm,
			.index = id,
			.class = ane_h16_usage_class(s->direction),
		};
		seen[s->direction - 1]++;
	}
	if (!seen[0] || seen[1] != 1)
		return -ENOEXEC;
	*count = hwx->surface_count;
	return 0;
}
EXPORT_SYMBOL_GPL(ane_h16_process_bindings);

/*
 * The mcache custom-BAR record comes from the program descriptor relocation
 * tuple (BAR ID, field, bit offset, width at +0x868, +0x870, +0x878, +0x87c of
 * the linear 0x8a0 form). The validator pins those words. The scratch form's
 * tuple layout is not established, so it sends no record, which is legal.
 */
int ane_h16_custom_bars(const struct ane_h16_hwx_snapshot *snapshot,
			const struct ane_h16_hwx *hwx, u32 dsid,
			struct ane_h16_custom_bar *bars, unsigned int *count)
{
	const u8 *c;
	u32 bar, offset, width;
	int ret;

	if (!hwx || !bars || !count)
		return -EINVAL;
	ret = ane_h16_hwx_snapshot_check(snapshot);
	if (ret)
		return ret;
	if (!hwx->program_offset || snapshot->size < 0x8a0 ||
	    hwx->program_offset > snapshot->size - 0x8a0)
		return -ENOEXEC;
	c = (const u8 *)snapshot->bytes + hwx->program_offset;
	*count = 0;
	if (get_unaligned_le32(c + 4) != 0x8a0)
		return 0;
	bar = get_unaligned_le32(c + 0x868);
	offset = get_unaligned_le32(c + 0x878);
	width = get_unaligned_le32(c + 0x87c);
	if (!width || width > 16 || offset > 64 - width || dsid >= BIT(width))
		return -ENOEXEC;
	bars[0] = (struct ane_h16_custom_bar) {
		.configuration = 1,
		.value_type = 0,
		.bar = bar,
		.bit_offset = offset,
		.bit_width = width,
		.value = dsid,
	};
	*count = 1;
	return 0;
}
EXPORT_SYMBOL_GPL(ane_h16_custom_bars);

int ane_h16_session_init(struct ane_h16_session *s, struct ane_fw_transport *transport,
			 const struct ane_h16_session_ops *ops, void *ctx,
			 u64 timeout_ns)
{
	if (!s || !transport || !transport->armed || !ops || !ops->map || !ops->log ||
	    !timeout_ns)
		return -EINVAL;
	memset(s, 0, sizeof(*s));
	s->transport = transport;
	s->ops = ops;
	s->ctx = ctx;
	s->timeout_ns = timeout_ns;
	s->dsid = ANE_H16_MCACHE_DSID;
	return 0;
}
EXPORT_SYMBOL_GPL(ane_h16_session_init);

static int h16_fail(struct ane_h16_session *s, int error)
{
	if (!s->error)
		s->error = error;
	if (!s->quarantined) {
		s->quarantined = true;
		s->pending.quarantined = true;
		s->ops->log(s->ctx, "program quarantine", (u64)-s->error, 0);
	}
	return s->error;
}

static u64 h16_now(struct ane_h16_session *s)
{
	return s->transport->ops->now_ns(s->transport->ctx);
}

/* Publish one command and wait for its transport reply, kept in the pool. */
static int h16_send(struct ane_h16_session *s, unsigned int ch, const void *command,
		    size_t length, const u8 **reply)
{
	struct ane_fw_transport *t = s->transport;
	int ret;

	if (ane_fw_command_denied(get_unaligned_le16((const u8 *)command + 4)))
		return -EPERM;
	ret = ane_fw_transport_publish(t, ch, command, length, h16_now(s) + s->timeout_ns);
	if (ret)
		return ret;
	ret = ane_fw_transport_wait(t, ch);
	*reply = (const u8 *)t->pool.cpu + (t->pending[ch].dva - t->pool.dva);
	return ret;
}

/*
 * The transport copies commands into a monotonic pool arena that is never
 * recycled before reset. Refuse a sequence up front when it cannot fit, so
 * running out of arena is a clean -ENOSPC and never a half-sent sequence.
 */
static int h16_pool_room(struct ane_h16_session *s, const size_t *lengths,
			 unsigned int count)
{
	const struct ane_fw_transport *t = s->transport;
	size_t used = t->pool_used;
	unsigned int i;

	for (i = 0; i < count; i++) {
		used = ALIGN(used, ANE_FW_SLOT_SIZE);
		if (used > t->pool.size || lengths[i] > t->pool.size - used)
			return -ENOSPC;
		used += lengths[i];
	}
	return 0;
}

static int h16_map(struct ane_h16_session *s, size_t size, bool write,
		   struct ane_fw_surface *out)
{
	int ret = s->ops->map(s->ctx, size, write, out);

	if (ret)
		return ret;
	if (!ane_fw_surface_valid(out) || out->size != size || !IS_ALIGNED(out->dva, SZ_16K))
		return -EPROTO;
	return 0;
}

static int h16_program_map(struct ane_h16_session *s, struct ane_h16_program *p,
			   const void *const aux[ANE_H16_AUX_COUNT],
			   const size_t aux_size[ANE_H16_AUX_COUNT],
			   struct ane_h16_load_layout *layout)
{
	struct ane_h16_process_binding bindings[ANE_H16_SURFACES];
	const struct ane_h16_hwx *hwx = &p->hwx;
	unsigned int i, j, count;
	int ret;

	ret = ane_h16_process_bindings(hwx, aux[ANE_H16_AUX_GENERIC],
				       aux_size[ANE_H16_AUX_GENERIC], bindings, &count);
	if (ret)
		return ret;
	for (i = 0; i < p->plan.count; i++) {
		const struct ane_h16_hwx_region *r = &p->plan.regions[i];

		if (r->file_size > p->plan.allocation[i] ||
		    r->file_offset > p->snapshot.size ||
		    r->file_size > p->snapshot.size - r->file_offset)
			return -ENOEXEC;
		ret = h16_map(s, p->plan.allocation[i], r->permissions & 2, &p->regions[i]);
		if (ret)
			return ret;
		memcpy(p->regions[i].cpu, (const u8 *)p->snapshot.bytes + r->file_offset,
		       r->file_size);
		layout->regions[i] = (struct ane_h16_load_mapping) {
			.dva = p->regions[i].dva,
			.size = p->regions[i].size,
			.permissions = r->permissions,
		};
	}
	layout->region_count = p->plan.count;
	for (i = 0; i < ANE_H16_AUX_COUNT; i++) {
		ret = h16_map(s, ALIGN(aux_size[i], SZ_16K), false, &p->aux[i]);
		if (ret)
			return ret;
		memcpy(p->aux[i].cpu, aux[i], aux_size[i]);
		layout->aux_dva[i] = p->aux[i].dva;
		layout->aux_size[i] = aux_size[i];
	}
	ret = ane_h16_load_resources_create(&p->snapshot, layout->regions,
					    layout->region_count, bindings, count,
					    p->resources);
	if (ret)
		return ret;
	p->resource_count = count;
	for (i = 0; i < count; i++) {
		for (j = 0; j < p->plan.count; j++)
			if (p->regions[j].dva == p->resources[i].buffer.mapping_dva)
				p->surfaces[i] = &p->regions[j];
		if (!p->surfaces[i])
			return -EINVAL;
	}
	ret = ane_h16_load_slots(hwx, &p->plan, layout, p->slots);
	if (ret)
		return ret;
	return ane_h16_custom_bars(&p->snapshot, hwx, s->dsid, p->bars, &p->bar_count);
}

static const size_t h16_load_sequence[] = {
	ANE_H16_LOAD_SIZE, ANE_H16_CREATE_SIZE, ANE_H16_TERMINATE_SIZE, ANE_H16_UNLOAD_SIZE,
};

/*
 * One loaded program per firmware session. On this firmware family the first
 * EXECUTE of a newly loaded program writes into the previous program's output
 * buffer, and a larger output overruns it. A second LOAD is therefore refused
 * even after the first program closed; a reboot starts a new session.
 */
int ane_h16_session_load(struct ane_h16_session *s,
			 struct ane_h16_hwx_snapshot *snapshot,
			 const void *const aux[ANE_H16_AUX_COUNT],
			 const size_t aux_size[ANE_H16_AUX_COUNT],
			 struct ane_h16_program **program)
{
	struct ane_h16_load_layout layout = {};
	struct ane_h16_program *p;
	u8 *command;
	const u8 *reply;
	unsigned int i;
	int ret;

	if (!s || !snapshot || !aux || !aux_size || !program)
		return -EINVAL;
	if (s->quarantined)
		return -EIO;
	if (s->load_claimed)
		return -EBUSY;
	for (i = 0; i < ANE_H16_AUX_COUNT; i++)
		if (!aux[i] || aux_size[i] < ANE_H16_AUX_MIN ||
		    aux_size[i] > ANE_H16_AUX_MAX || !IS_ALIGNED(aux_size[i], 4))
			return -EINVAL;
	ret = ane_h16_hwx_snapshot_check(snapshot);
	if (ret)
		return ret;
	ret = h16_pool_room(s, h16_load_sequence, ARRAY_SIZE(h16_load_sequence));
	if (ret)
		return ret;
	p = kzalloc_obj(*p);
	command = kzalloc(ANE_H16_CREATE_SIZE, GFP_KERNEL);
	if (!p || !command) {
		ret = -ENOMEM;
		goto free;
	}
	ret = ane_h16_load_plan_create(snapshot, &p->plan);
	if (!ret)
		ret = ane_h16_hwx_validate(snapshot->bytes, snapshot->size, &p->hwx);
	if (ret)
		goto free;
	p->snapshot = *snapshot;
	/*
	 * From the first mapping on, firmware-visible memory exists that is never
	 * released before reset, so the single program claim is taken here.
	 */
	s->load_claimed = true;
	memset(snapshot, 0, sizeof(*snapshot));
	s->program = p;
	ret = h16_program_map(s, p, aux, aux_size, &layout);
	if (!ret)
		ret = ane_h16_load_encode(command, ANE_H16_LOAD_SIZE, p->slots);
	if (ret)
		goto out;
	ret = h16_send(s, ANE_FW_IO, command, ANE_H16_LOAD_SIZE, &reply);
	if (!ret)
		ret = ane_h16_load_reply(reply, ANE_H16_LOAD_SIZE, command, &p->key.program);
	if (ret) {
		s->ops->log(s->ctx, "LOAD refused", (u64)-ret, 0);
		ret = h16_fail(s, ret);
		goto out;
	}
	s->ops->log(s->ctx, "LOAD program", p->key.program, p->bar_count);
	ret = ane_h16_create_encode(command, ANE_H16_CREATE_SIZE, p->key.program);
	if (!ret)
		ret = h16_send(s, ANE_FW_IO, command, ANE_H16_CREATE_SIZE, &reply);
	if (!ret)
		ret = ane_h16_create_reply(reply, ANE_H16_CREATE_SIZE, p->key.program,
					   &p->key.process);
	if (ret) {
		s->ops->log(s->ctx, "CREATE refused", (u64)-ret, 0);
		ret = h16_fail(s, ret);
		goto out;
	}
	/* Each validated program has exactly one procedure. */
	p->key.procedure = 0;
	p->created = true;
	s->ops->log(s->ctx, "CREATE process", p->key.program, p->key.process);
	*program = p;
out:
	kfree(command);
	return ret;
free:
	kfree(command);
	kfree(p);
	return ret;
}
EXPORT_SYMBOL_GPL(ane_h16_session_load);

static int h16_gate(struct ane_h16_session *s, u32 value)
{
	u8 command[ANE_H16_GATE_SIZE];
	const u8 *reply;
	int ret = ane_h16_gate_encode(command, sizeof(command), value);

	if (!ret)
		ret = h16_send(s, ANE_FW_DATA_CHAIN, command, sizeof(command), &reply);
	if (!ret)
		ret = ane_h16_id_reply(reply, sizeof(command), command);
	return ret;
}

/* Wait for the terminal report of one pending UUID, servicing allocations. */
static int h16_wait_report(struct ane_h16_session *s, struct ane_h16_pending *request,
			   u64 deadline)
{
	struct ane_fw_transport *t = s->transport;
	u8 *report;
	int ret;

	report = kmalloc(ANE_H16_REPORT_MAX, GFP_KERNEL);
	if (!report)
		return -ENOMEM;
	for (;;) {
		size_t size;
		u32 pending;

		ret = ane_fw_transport_consume(t, report, ANE_H16_REPORT_MAX, &size);
		if (!ret) {
			ret = ane_h16_pending_dispatch(&s->pending, report, size,
						       ANE_FW_IO_T2H,
						       (unsigned long)div_u64(h16_now(s),
									      NSEC_PER_USEC));
			if (ret == ANE_H16_EVENT_COMPLETE) {
				ret = 0;
				break;
			}
			if (ret == ANE_H16_EVENT_FAILED) {
				ret = -EIO;
				break;
			}
			/* Foreign or late UUIDs and ISP-routed reports never retire this call. */
			if (ret == ANE_H16_EVENT_PROGRESS || ret == -ENOENT || ret == -ENOMSG)
				continue;
			break;
		}
		if (ret != -EAGAIN)
			break;
		ret = ane_fw_transport_irq(t, &pending);
		if (!ret) {
			ret = ane_fw_transport_shared(t);
			if (ret == -EAGAIN)
				ret = 0;
		}
		if (ret)
			break;
		if (h16_now(s) >= deadline) {
			ret = -ETIMEDOUT;
			break;
		}
		t->ops->relax(t->ctx);
	}
	kfree(report);
	return ret;
}

/*
 * 0x002d with 0, EXECUTE on channel 7 with its UUID already pending, the
 * matching state-1 report on channel 6, 0x002d with 1, then a DMA read
 * barrier before the caller reads outputs. Any uncertain outcome quarantines
 * the session and keeps every mapping.
 */
int ane_h16_session_execute(struct ane_h16_session *s, struct ane_h16_program *p)
{
	struct ane_h16_execute_buffer buffers[ANE_H16_SURFACES];
	struct ane_h16_pending request = {};
	struct ane_h16_execute call = {};
	size_t length, sequence[5];
	u8 *command;
	const u8 *reply;
	unsigned int i;
	u64 deadline;
	int ret;

	if (!s || !p || s->program != p)
		return -EINVAL;
	if (s->quarantined)
		return -EIO;
	if (!p->created || p->closed)
		return -EINVAL;
	ret = ane_h16_hwx_snapshot_check(&p->snapshot);
	if (!ret)
		ret = ane_h16_resources_validate(&p->hwx, p->resources,
						 p->resource_count);
	if (ret)
		return ret;
	for (i = 0; i < p->resource_count; i++)
		buffers[i] = p->resources[i].buffer;
	length = ANE_H16_EXECUTE_BASE_SIZE + p->bar_count * ANE_H16_CUSTOM_BAR_STRIDE;
	/* Keep room for the teardown pair after this request. */
	sequence[0] = ANE_H16_GATE_SIZE;
	sequence[1] = length;
	sequence[2] = ANE_H16_GATE_SIZE;
	sequence[3] = ANE_H16_TERMINATE_SIZE;
	sequence[4] = ANE_H16_UNLOAD_SIZE;
	ret = h16_pool_room(s, sequence, ARRAY_SIZE(sequence));
	if (ret)
		return ret;
	command = kzalloc(length, GFP_KERNEL);
	if (!command)
		return -ENOMEM;
	/* Inputs and poisoned outputs must reach memory before any doorbell. */
	s->transport->ops->write_barrier(s->transport->ctx);
	ret = h16_gate(s, 0);
	if (ret)
		goto fail;
	deadline = h16_now(s) + s->timeout_ns;
	request.key = p->key;
	request.deadline = (unsigned long)div_u64(deadline, NSEC_PER_USEC);
	ret = ane_h16_pending_insert(&s->pending, &request,
				     (unsigned long)div_u64(h16_now(s), NSEC_PER_USEC));
	if (ret)
		goto fail;
	call.key = request.key;
	call.buffers = buffers;
	call.buffer_count = p->resource_count;
	call.bars = p->bars;
	call.bar_count = p->bar_count;
	ret = ane_h16_execute_encode(command, length, &call);
	if (!ret)
		ret = ane_h16_command_prepare(command, length, ANE_H16_EXECUTE,
					      ANE_FW_DATA_CHAIN);
	if (!ret)
		ret = h16_send(s, ANE_FW_DATA_CHAIN, command, length, &reply);
	/* The channel-7 reply acknowledges transport only, never completion. */
	if (!ret)
		ret = h16_wait_report(s, &request, deadline);
	if (ret) {
		if (request.active)
			ane_h16_pending_abort(&s->pending, &request, ret);
		goto fail;
	}
	ret = h16_gate(s, 1);
	if (ret)
		goto fail;
	s->transport->ops->read_barrier(s->transport->ctx);
	kfree(command);
	return 0;
fail:
	kfree(command);
	s->ops->log(s->ctx, "EXECUTE failed", (u64)-ret, request.key.uuid);
	return h16_fail(s, ret);
}
EXPORT_SYMBOL_GPL(ane_h16_session_execute);

/* TERMINATE then UNLOAD. Mappings and the single-program claim stay. */
int ane_h16_session_close(struct ane_h16_session *s, struct ane_h16_program *p)
{
	u8 command[ANE_H16_TERMINATE_SIZE];
	const u8 *reply;
	int ret;

	if (!s || !p || s->program != p)
		return -EINVAL;
	if (p->closed || !p->created)
		return 0;
	p->closed = true;
	if (s->quarantined)
		return -EIO;
	ret = ane_h16_terminate_encode(command, ANE_H16_TERMINATE_SIZE, p->key.program,
				       p->key.process);
	if (!ret)
		ret = h16_send(s, ANE_FW_IO, command, ANE_H16_TERMINATE_SIZE, &reply);
	if (!ret)
		ret = ane_h16_id_reply(reply, ANE_H16_TERMINATE_SIZE, command);
	if (!ret)
		ret = ane_h16_unload_encode(command, ANE_H16_UNLOAD_SIZE, p->key.program);
	if (!ret)
		ret = h16_send(s, ANE_FW_IO, command, ANE_H16_UNLOAD_SIZE, &reply);
	if (!ret)
		ret = ane_h16_id_reply(reply, ANE_H16_UNLOAD_SIZE, command);
	if (ret)
		return h16_fail(s, ret);
	s->ops->log(s->ctx, "TERMINATE and UNLOAD", p->key.program, p->key.process);
	return 0;
}
EXPORT_SYMBOL_GPL(ane_h16_session_close);

MODULE_DESCRIPTION("H16 program load, execute and teardown on the shared transport");
MODULE_LICENSE("Dual MIT/GPL");
