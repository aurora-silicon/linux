// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "ane_h16_boot.h"
#include "ane_h16_program.h"

#include "ane_h16_hwx_test_fixture.h"
#include "ane_h16_add128_fixture.h"

#define FW_IPC_DVA 0x100003d0000ULL
#define FW_POOL_DVA 0x10000384000ULL
#define FW_MAP_DVA 0x10001f00000ULL
#define FW_RING_SLOTS 8
#define FW_REPORTS 0x8000

enum fw_mode {
	FW_OK,
	FW_LOAD_ID_UNCHANGED,
	FW_LOAD_NAK,
	FW_CREATE_BODY,
	FW_REPORT_FOREIGN_UUID,
	FW_REPORT_WRONG_PROCESS,
	FW_REPORT_FAILURE,
	FW_REPORT_STALL,
	/* State 0 at once, then states 3 and 1 after late_polls sleeps. */
	FW_REPORT_LATE,
};

/* A synchronous fake firmware that answers on each doorbell. */
struct prog_fw {
	struct kunit *test;
	struct ane_fw_platform soc;
	struct ane_fw_surface ipc, pool;
	struct ane_fw_transport t;
	struct ane_h16_session s;
	enum fw_mode mode;
	u64 now;
	/* Time each doorbell takes to answer. */
	u64 doorbell_ns;
	u64 late_uuid;
	unsigned int late_polls;
	u64 next_dva;
	u32 head[ANE_FW_CHANNELS];
	u32 reports;
	u32 program, process;
	u16 ids[32];
	u32 values[32];
	unsigned int count;
	u8 exec[0xd00];
	size_t exec_length;
	u8 load[ANE_H16_LOAD_SIZE];
	unsigned int maps, writable_maps;
	/* The EXECUTE failure trace lines, by step. */
	u64 fail_step, fail_flags, fail_reports, fail_state, fail_other;
};

static u64 fw_load(void *ctx, const void *p)
{
	return get_unaligned_le64(p);
}

static void fw_store(void *ctx, void *p, u64 value)
{
	put_unaligned_le64(value, p);
}

static void fw_barrier(void *ctx)
{
}

static u32 fw_read32(void *ctx, u32 reg)
{
	return 0;
}

static u8 *fw_slot(struct prog_fw *f, unsigned int ch)
{
	return (u8 *)f->ipc.cpu + f->t.channels[ch].offset + f->head[ch] * ANE_FW_SLOT_SIZE;
}

static void fw_advance(struct prog_fw *f, unsigned int ch)
{
	f->head[ch] = (f->head[ch] + 1) % f->t.channels[ch].slots;
}

static void fw_report(struct prog_fw *f, u64 uuid, u32 process, u32 state)
{
	u32 offset = FW_REPORTS + (f->reports++ % 16) * 0x60;
	u8 *r = (u8 *)f->ipc.cpu + offset, *slot = fw_slot(f, ANE_FW_IO_T2H);

	memset(r, 0, 0x60);
	put_unaligned_le32(0xfe00 + (state == 1), r);
	put_unaligned_le16(0x300, r + 4);
	put_unaligned_le64(uuid, r + 8);
	put_unaligned_le32(f->program, r + 0x10);
	put_unaligned_le32(process, r + 0x14);
	put_unaligned_le32(state, r + 0x1c);
	put_unaligned_le64(0x60, slot + 8);
	put_unaligned_le64(0, slot + 16);
	put_unaligned_le64(f->ipc.dva + offset, slot);
	fw_advance(f, ANE_FW_IO_T2H);
}

static void fw_execute(struct prog_fw *f, const u8 *cmd)
{
	u64 uuid = get_unaligned_le64(cmd + 0x20);

	switch (f->mode) {
	case FW_REPORT_FOREIGN_UUID:
		fw_report(f, uuid + 100, f->process, 0);
		fw_report(f, uuid + 100, f->process, 1);
		break;
	case FW_REPORT_WRONG_PROCESS:
		fw_report(f, uuid, f->process, 0);
		fw_report(f, uuid, f->process + 1, 1);
		break;
	case FW_REPORT_FAILURE:
		fw_report(f, uuid, f->process, 0);
		fw_report(f, uuid, f->process, 2);
		break;
	case FW_REPORT_LATE:
		fw_report(f, uuid, f->process, 0);
		f->late_uuid = uuid;
		break;
	case FW_REPORT_STALL:
		/* Started on the engine, then never a terminal report. */
		fw_report(f, uuid, f->process, 0);
		fw_report(f, uuid, f->process, 3);
		break;
	default:
		fw_report(f, uuid, f->process, 0);
		fw_report(f, uuid, f->process, 3);
		fw_report(f, uuid, f->process, 1);
		break;
	}
}

static void fw_command(struct prog_fw *f, unsigned int ch)
{
	u8 *slot = fw_slot(f, ch), *cmd;
	u64 dva = get_unaligned_le64(slot), length = get_unaligned_le64(slot + 8);
	u16 id;

	if (dva & 1)
		return;
	cmd = (u8 *)f->pool.cpu + (dva - f->pool.dva);
	id = get_unaligned_le16(cmd + 4);
	if (f->count < ARRAY_SIZE(f->ids)) {
		f->ids[f->count] = id;
		f->values[f->count] = length >= 12 ? get_unaligned_le32(cmd + 8) : 0;
		f->count++;
	}
	switch (id) {
	case ANE_H16_LOAD_PROGRAM:
		memcpy(f->load, cmd, sizeof(f->load));
		if (f->mode == FW_LOAD_NAK)
			put_unaligned_le16(1, cmd + 6);
		else if (f->mode != FW_LOAD_ID_UNCHANGED)
			put_unaligned_le32(f->program, cmd + ANE_H16_LOAD_ID_OFFSET);
		break;
	case ANE_H16_CREATE_PROCESS:
		KUNIT_EXPECT_FALSE(f->test, memchr_inv(cmd + 0xc, 0, ANE_H16_CREATE_SIZE - 0xc));
		put_unaligned_le32(f->process, cmd + 0xc);
		if (f->mode == FW_CREATE_BODY)
			cmd[0x100] = 1;
		break;
	case ANE_H16_EXECUTE:
		f->exec_length = min_t(size_t, length, sizeof(f->exec));
		memcpy(f->exec, cmd, f->exec_length);
		break;
	}
	put_unaligned_le64(length, slot + 8);
	put_unaligned_le64(0, slot + 16);
	put_unaligned_le64(dva | 1, slot);
	fw_advance(f, ch);
	if (id == ANE_H16_EXECUTE)
		fw_execute(f, cmd);
}

static void fw_write32(void *ctx, u32 reg, u32 value)
{
	struct prog_fw *f = ctx;

	if (reg != f->soc.doorbell)
		return;
	/* Commands take doorbell_ns to answer; returning report slots is free. */
	if (value & (BIT(ANE_FW_IO) | BIT(ANE_FW_DATA_CHAIN)))
		f->now += f->doorbell_ns;
	if (value & BIT(ANE_FW_IO))
		fw_command(f, ANE_FW_IO);
	if (value & BIT(ANE_FW_DATA_CHAIN))
		fw_command(f, ANE_FW_DATA_CHAIN);
}

static u64 fw_now(void *ctx)
{
	return ((struct prog_fw *)ctx)->now;
}

static void fw_relax(void *ctx, u64 waited_ns)
{
	struct prog_fw *f = ctx;

	f->now += 10;
	if (f->late_uuid && !--f->late_polls) {
		fw_report(f, f->late_uuid, f->process, 3);
		fw_report(f, f->late_uuid, f->process, 1);
		f->late_uuid = 0;
	}
}

static int fw_alloc(void *ctx, size_t size, u32 tag, struct ane_fw_surface *s)
{
	return -ENOMEM;
}

static void fw_refused(void *ctx, u64 w0, u64 w1, u64 w2, int error)
{
}

static const struct ane_fw_transport_ops fw_ops = {
	.load64 = fw_load,
	.store64 = fw_store,
	.read_barrier = fw_barrier,
	.write_barrier = fw_barrier,
	.read32 = fw_read32,
	.write32 = fw_write32,
	.now_ns = fw_now,
	.relax = fw_relax,
	.alloc = fw_alloc,
	.refused = fw_refused,
};

static int fw_map(void *ctx, size_t size, bool write, struct ane_fw_surface *s)
{
	struct prog_fw *f = ctx;

	*s = (struct ane_fw_surface) {
		.cpu = kunit_kzalloc(f->test, size, GFP_KERNEL),
		.dva = f->next_dva,
		.size = size,
	};
	if (!s->cpu)
		return -ENOMEM;
	f->next_dva += size;
	f->maps++;
	f->writable_maps += write;
	return 0;
}

static void fw_log(void *ctx, const char *item, u64 a, u64 b)
{
	struct prog_fw *f = ctx;

	if (!strcmp(item, "EXECUTE step/flags")) {
		f->fail_step = a;
		f->fail_flags = b;
	} else if (!strcmp(item, "EXECUTE reports/last state")) {
		f->fail_reports = a;
		f->fail_state = b;
	} else if (!strcmp(item, "EXECUTE other reports")) {
		f->fail_other = a;
	}
}

static const struct ane_h16_session_ops fw_session_ops = {
	.map = fw_map,
	.log = fw_log,
};

static struct prog_fw *fw_create(struct kunit *test)
{
	static const char * const names[] = {
		"TERMINAL", "IO", "DEBUG", "BUF_H2T", "BUF_T2H",
		"SHAREDMALLOC", "IO_T2H", "DATA_CHAIN_H2T",
	};
	struct prog_fw *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	unsigned int i;

	if (!f)
		return NULL;
	f->test = test;
	f->soc.doorbell = 0x1884000;
	f->soc.irq_status = 0x188c000;
	f->soc.irq_ack = 0x1890000;
	f->ipc = (struct ane_fw_surface) {
		.cpu = kunit_kzalloc(test, SZ_64K, GFP_KERNEL),
		.dva = FW_IPC_DVA, .size = SZ_64K,
	};
	f->pool = (struct ane_fw_surface) {
		.cpu = kunit_kzalloc(test, SZ_256K, GFP_KERNEL),
		.dva = FW_POOL_DVA, .size = SZ_256K,
	};
	if (!f->ipc.cpu || !f->pool.cpu)
		return NULL;
	for (i = 0; i < ANE_FW_CHANNELS; i++) {
		u8 *p = (u8 *)f->ipc.cpu + i * ANE_FW_DESCRIPTOR_SIZE;

		strscpy((char *)p, names[i], 64);
		put_unaligned_le32(i == 0 ? 2 : (i >= 4 && i <= 6), p + 0x40);
		put_unaligned_le32(i, p + 0x44);
		put_unaligned_le64(FW_RING_SLOTS, p + 0x48);
		put_unaligned_le64(f->ipc.dva + 0x800 + i * 0x200, p + 0x50);
	}
	f->next_dva = FW_MAP_DVA;
	f->program = 0x11;
	f->process = 0;
	if (ane_fw_transport_init(&f->t, &f->soc, &fw_ops, f, f->ipc, f->pool, SZ_16K,
				  f->ipc.dva, ANE_FW_TABLE_SIZE) ||
	    ane_fw_transport_arm(&f->t) ||
	    ane_h16_session_init(&f->s, &f->t, &fw_session_ops, f, 1000))
		return NULL;
	return f;
}

static void snapshot_free(void *snapshot)
{
	ane_h16_hwx_snapshot_destroy(snapshot);
}

static struct ane_h16_hwx_snapshot *fw_snapshot(struct kunit *test, bool mixed, bool kernel)
{
	struct hwx_fixture *x = fixture_create(test, mixed, kernel);
	struct ane_h16_hwx_snapshot *snapshot;

	if (!x)
		return NULL;
	snapshot = kunit_kzalloc(test, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot || ane_h16_hwx_snapshot_create(x->bytes, x->size, snapshot) ||
	    kunit_add_action_or_reset(test, snapshot_free, snapshot))
		return NULL;
	return snapshot;
}

/* Generic section with ID base + k for surface k, as a host builder writes it. */
static size_t generic_fill(u8 *g, const struct ane_h16_hwx *hwx, u32 base)
{
	unsigned int i;

	unsigned int entries = hwx->surface_count + (hwx->scratch.size ? 1 : 0);

	memset(g, 0, ANE_H16_GENERIC_HEADER + entries * ANE_H16_GENERIC_ENTRY);
	put_unaligned_le32(ANE_H16_GENERIC_MAGIC, g);
	put_unaligned_le32(ANE_H16_GENERIC_VERSION, g + 4);
	put_unaligned_le32(entries, g + ANE_H16_GENERIC_COUNT);
	for (i = 0; i < hwx->surface_count; i++) {
		u8 *e = g + ANE_H16_GENERIC_HEADER + i * ANE_H16_GENERIC_ENTRY;

		put_unaligned_le32(1, e);
		put_unaligned_le32(base + i, e + 4);
		put_unaligned_le32(hwx->surfaces[i].direction - 1, e + 8);
		put_unaligned_le32(hwx->surfaces[i].direction, e + 0x10);
		put_unaligned_le64(hwx->surfaces[i].span, e + 0x20);
		put_unaligned_le32(0xffff, e + 0x28);
	}
	/* A linked program's scratch: last, input usage, covering the segment. */
	if (hwx->scratch.size) {
		u8 *e = g + ANE_H16_GENERIC_HEADER + i * ANE_H16_GENERIC_ENTRY;

		put_unaligned_le32(1, e);
		put_unaligned_le32(base + i, e + 4);
		put_unaligned_le32(1, e + 0x10);
		put_unaligned_le64(hwx->scratch.size, e + 0x20);
		put_unaligned_le32(0xffff, e + 0x28);
	}
	return ANE_H16_GENERIC_HEADER + entries * ANE_H16_GENERIC_ENTRY;
}

static u8 test_aux[ANE_H16_AUX_COUNT][0x298];
static const void *const test_aux_ptr[ANE_H16_AUX_COUNT] = {
	test_aux[0], test_aux[1], test_aux[2], test_aux[3],
};

static size_t test_aux_size[ANE_H16_AUX_COUNT] = { 0x298, 0x410 - 0x180, 0x38, 0x28 };

#define TEST_GENERIC_BASE 8

static int fw_load_program(struct prog_fw *f, struct ane_h16_program **p)
{
	struct ane_h16_hwx_snapshot *snapshot = fw_snapshot(f->test, false, false);
	struct ane_h16_hwx hwx;

	if (!snapshot || ane_h16_hwx_validate(snapshot->bytes, snapshot->size, &hwx))
		return -ENOMEM;
	test_aux_size[ANE_H16_AUX_GENERIC] =
		generic_fill(test_aux[ANE_H16_AUX_GENERIC], &hwx, TEST_GENERIC_BASE);
	return ane_h16_session_load(&f->s, snapshot, test_aux_ptr, test_aux_size, p);
}

static void expect_slot(struct kunit *test, const u8 *load, unsigned int slot, u32 resource,
			u32 class, u64 dva, u64 size)
{
	const u8 *d = load + ANE_H16_LOAD_SLOT_OFFSET + slot * ANE_H16_LOAD_SLOT_SIZE;

	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 4), resource);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 8), class);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0xc), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x10), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x14), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x18), dva);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x20), size);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x28), 0);
}

static void expect_empty(struct kunit *test, const u8 *load, unsigned int slot)
{
	const u8 *d = load + ANE_H16_LOAD_SLOT_OFFSET + slot * ANE_H16_LOAD_SLOT_SIZE;

	KUNIT_EXPECT_FALSE(test, memchr_inv(d, 0, ANE_H16_LOAD_SLOT_SIZE));
}

/* add-128 shape: input, output, input surfaces of 32 KiB, no kernel section. */
static void program_add128_slots_test(struct kunit *test)
{
	static const u32 direction[3] = { 1, 2, 1 };
	struct ane_h16_process_binding bindings[ANE_H16_SURFACES], scratch;
	struct ane_h16_load_slot slots[ANE_H16_LOAD_SLOTS];
	struct ane_h16_load_layout layout = {};
	struct ane_h16_load_plan *plan = kunit_kzalloc(test, sizeof(*plan), GFP_KERNEL);
	struct ane_h16_hwx *hwx = kunit_kzalloc(test, sizeof(*hwx), GFP_KERNEL);
	u8 load[ANE_H16_LOAD_SIZE];
	u8 *generic = kunit_kzalloc(test, ANE_H16_GENERIC_HEADER + 3 * ANE_H16_GENERIC_ENTRY,
				    GFP_KERNEL);
	size_t generic_size;
	unsigned int i, count;

	KUNIT_ASSERT_NOT_NULL(test, plan);
	KUNIT_ASSERT_NOT_NULL(test, hwx);
	KUNIT_ASSERT_NOT_NULL(test, generic);

	for (i = 0; i < 3; i++) {
		hwx->surfaces[i].section.vm = 0x30000000 + i * SZ_32K;
		hwx->surfaces[i].direction = direction[i];
		hwx->surfaces[i].span = SZ_32K;
		hwx->surfaces[i].allocation = SZ_32K;
		plan->regions[i] = (struct ane_h16_hwx_region) {
			.vm = hwx->surfaces[i].section.vm, .size = SZ_32K,
			.permissions = direction[i], .kind = ANE_H16_REGION_SURFACE,
		};
		plan->allocation[i] = SZ_32K;
	}
	hwx->surface_count = 3;
	hwx->text = (struct ane_h16_hwx_section) { .vm = 0x30018000, .size = 0x140 };
	hwx->constants = (struct ane_h16_hwx_section) { .vm = 0x30018140, .size = 0x4000 };
	plan->regions[3] = (struct ane_h16_hwx_region) {
		.vm = 0x30018000, .size = 0x4140, .file_size = 0x4140,
		.permissions = 5, .kind = ANE_H16_REGION_TEXT,
	};
	plan->allocation[3] = SZ_32K;
	plan->count = 4;
	for (i = 0; i < plan->count; i++)
		layout.regions[i] = (struct ane_h16_load_mapping) {
			.dva = 0x10002000000ULL + i * SZ_1M, .size = plan->allocation[i],
			.permissions = plan->regions[i].permissions,
		};
	layout.region_count = plan->count;
	for (i = 0; i < ANE_H16_AUX_COUNT; i++) {
		layout.aux_dva[i] = 0x10003000000ULL + i * SZ_16K;
		layout.aux_size[i] = test_aux_size[i];
	}

	/* IDs from the generic section: x = 5, y = 4, z = 6 as the add-128 builder writes. */
	generic_size = generic_fill(generic, hwx, 0);
	put_unaligned_le32(5, generic + ANE_H16_GENERIC_HEADER + 4);
	put_unaligned_le32(4, generic + ANE_H16_GENERIC_HEADER + ANE_H16_GENERIC_ENTRY + 4);
	put_unaligned_le32(6, generic + ANE_H16_GENERIC_HEADER + 2 * ANE_H16_GENERIC_ENTRY + 4);
	KUNIT_ASSERT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size, bindings,
						       &count, &scratch), 0);
	KUNIT_EXPECT_EQ(test, count, 3);
	KUNIT_EXPECT_EQ(test, bindings[0].index, 5);
	KUNIT_EXPECT_EQ(test, bindings[0].class, 0);
	KUNIT_EXPECT_EQ(test, bindings[1].index, 4);
	KUNIT_EXPECT_EQ(test, bindings[1].class, 1);
	KUNIT_EXPECT_EQ(test, bindings[2].index, 6);
	KUNIT_EXPECT_EQ(test, bindings[2].class, 0);
	/* No scratch segment, no scratch binding. */
	KUNIT_EXPECT_EQ(test, scratch.index, 0);

	/* A generic section that disagrees with the program refuses. */
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size - 4,
						       bindings, &count, &scratch), -ENOEXEC);
	put_unaligned_le32(2, generic + ANE_H16_GENERIC_HEADER + 4);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size, bindings,
						       &count, &scratch), -ENOEXEC);
	put_unaligned_le32(4, generic + ANE_H16_GENERIC_HEADER + 4);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size, bindings,
						       &count, &scratch), -ENOEXEC);
	put_unaligned_le32(5, generic + ANE_H16_GENERIC_HEADER + 4);
	put_unaligned_le32(1, generic + ANE_H16_GENERIC_HEADER + 8);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size, bindings,
						       &count, &scratch), -ENOEXEC);
	put_unaligned_le32(0, generic + ANE_H16_GENERIC_HEADER + 8);
	put_unaligned_le64(SZ_32K - 2, generic + ANE_H16_GENERIC_HEADER + 0x20);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size, bindings,
						       &count, &scratch), -ENOEXEC);
	put_unaligned_le64(SZ_32K, generic + ANE_H16_GENERIC_HEADER + 0x20);
	put_unaligned_le32(2, generic + ANE_H16_GENERIC_COUNT);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size, bindings,
						       &count, &scratch), -ENOEXEC);
	put_unaligned_le32(3, generic + ANE_H16_GENERIC_COUNT);
	put_unaligned_le32(0x11, generic + 4);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size, bindings,
						       &count, &scratch), -ENOEXEC);
	put_unaligned_le32(ANE_H16_GENERIC_VERSION, generic + 4);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size, bindings,
						       &count, &scratch), 0);

	KUNIT_ASSERT_EQ(test, ane_h16_load_slots(hwx, plan, &layout, slots), 0);
	KUNIT_ASSERT_EQ(test, ane_h16_load_encode(load, sizeof(load), slots), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(load), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(load + 4), ANE_H16_LOAD_PROGRAM);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(load + 6), 0);
	expect_slot(test, load, 0, 1002, 4, layout.aux_dva[0], layout.aux_size[0]);
	/* No kernel section: slot 1 carries the constants at their sub-page DVA. */
	expect_slot(test, load, 1, 1007, 3, layout.regions[3].dva + 0x140, 0x4000);
	expect_slot(test, load, 2, 1006, 4, layout.regions[3].dva, 0x140);
	expect_slot(test, load, 3, 1003, 4, layout.aux_dva[1], test_aux_size[1]);
	expect_slot(test, load, 4, 1004, 4, layout.aux_dva[2], 0x38);
	expect_empty(test, load, 5);
	expect_slot(test, load, 6, 1005, 4, layout.aux_dva[3], 0x28);
	expect_empty(test, load, 7);
	expect_empty(test, load, 8);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(load + 0x1b8), 0xffffffff);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(load + 0x1bc), 0);

	/* Missing aux, short aux, mismatched layout and wrong length refuse. */
	layout.aux_dva[2] = 0;
	KUNIT_EXPECT_EQ(test, ane_h16_load_slots(hwx, plan, &layout, slots), -EINVAL);
	layout.aux_dva[2] = 0x10003008000ULL;
	layout.aux_size[3] = ANE_H16_AUX_MIN - 4;
	KUNIT_EXPECT_EQ(test, ane_h16_load_slots(hwx, plan, &layout, slots), -EINVAL);
	layout.aux_size[3] = 0x28;
	layout.regions[1].size = SZ_16K;
	KUNIT_EXPECT_EQ(test, ane_h16_load_slots(hwx, plan, &layout, slots), -EINVAL);
	layout.regions[1].size = SZ_32K;
	layout.region_count = 3;
	KUNIT_EXPECT_EQ(test, ane_h16_load_slots(hwx, plan, &layout, slots), -EINVAL);
	KUNIT_EXPECT_EQ(test, ane_h16_load_encode(load, sizeof(load) - 8, slots), -EINVAL);

	/* A program needs an input and exactly one output. */
	hwx->surfaces[0].direction = 2;
	generic_size = generic_fill(generic, hwx, 4);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size, bindings,
						       &count, &scratch), -ENOEXEC);
	hwx->surfaces[0].direction = 3;
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, generic, generic_size, bindings,
						       &count, &scratch), -ENOEXEC);
}

static u8 *sparse_bytes(struct kunit *test, const struct ane_h16_sparse_run *runs,
			unsigned int count, size_t size)
{
	u8 *bytes = kunit_kzalloc(test, size, GFP_KERNEL);
	unsigned int i;

	if (!bytes)
		return NULL;
	for (i = 0; i < count; i++) {
		if (runs[i].offset + runs[i].length > size)
			return NULL;
		memcpy(bytes + runs[i].offset, runs[i].bytes, runs[i].length);
	}
	return bytes;
}

/*
 * The pinned add-128 program and the sections h16-sections.py builds for it
 * load and run against the fake firmware. Segments are x (in), y (out), z (in)
 * while the tensors list x, z, y; EXECUTE names x = 5, y = 4, z = 6.
 */
static void program_add128_hwx_test(struct kunit *test)
{
	static const u32 ids[3] = { 5, 4, 6 }, classes[3] = { 0, 1, 0 };
	struct ane_h16_hwx_snapshot *snapshot;
	struct ane_h16_program *p = NULL;
	const void *aux[ANE_H16_AUX_COUNT];
	size_t aux_size[ANE_H16_AUX_COUNT] = {
		ADD128_GENERIC_SIZE, ADD128_OPERATION_SIZE,
		ADD128_PROCEDURE_SIZE, ADD128_PROPERTIES_SIZE,
	};
	struct prog_fw *f = fw_create(test);
	u8 *hwx;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	hwx = sparse_bytes(test, add128_hwx_runs, ARRAY_SIZE(add128_hwx_runs), ADD128_HWX_SIZE);
	aux[0] = sparse_bytes(test, add128_generic_runs, ARRAY_SIZE(add128_generic_runs),
			      ADD128_GENERIC_SIZE);
	aux[1] = sparse_bytes(test, add128_operation_runs, ARRAY_SIZE(add128_operation_runs),
			      ADD128_OPERATION_SIZE);
	aux[2] = sparse_bytes(test, add128_procedure_runs, ARRAY_SIZE(add128_procedure_runs),
			      ADD128_PROCEDURE_SIZE);
	aux[3] = sparse_bytes(test, add128_properties_runs, ARRAY_SIZE(add128_properties_runs),
			      ADD128_PROPERTIES_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, hwx);
	for (i = 0; i < ANE_H16_AUX_COUNT; i++)
		KUNIT_ASSERT_NOT_NULL(test, aux[i]);
	snapshot = kunit_kzalloc(test, sizeof(*snapshot), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, snapshot);
	KUNIT_ASSERT_EQ(test, ane_h16_hwx_snapshot_create(hwx, ADD128_HWX_SIZE, snapshot), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, snapshot_free, snapshot), 0);
	KUNIT_ASSERT_MEMEQ(test, snapshot->digest, add128_hwx_sha256, SHA256_DIGEST_SIZE);

	KUNIT_ASSERT_EQ(test, ane_h16_session_load(&f->s, snapshot, aux, aux_size, &p), 0);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->hwx.surface_count, 3);
	KUNIT_EXPECT_EQ(test, p->hwx.surfaces[1].direction, 2);
	KUNIT_EXPECT_EQ(test, p->resource_count, 3);
	expect_slot(test, f->load, 0, 1002, 4, p->aux[0].dva, ADD128_GENERIC_SIZE);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->load + ANE_H16_LOAD_SLOT_OFFSET +
						 ANE_H16_LOAD_SLOT_SIZE + 8), 3);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->load + ANE_H16_LOAD_SLOT_OFFSET +
						 ANE_H16_LOAD_SLOT_SIZE + 0x20), 0x4000);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->load + ANE_H16_LOAD_SLOT_OFFSET +
						 2 * ANE_H16_LOAD_SLOT_SIZE + 0x20), 0x130);
	expect_slot(test, f->load, 3, 1003, 4, p->aux[1].dva, ADD128_OPERATION_SIZE);
	expect_slot(test, f->load, 4, 1004, 4, p->aux[2].dva, ADD128_PROCEDURE_SIZE);
	expect_empty(test, f->load, 5);
	expect_slot(test, f->load, 6, 1005, 4, p->aux[3].dva, ADD128_PROPERTIES_SIZE);
	expect_empty(test, f->load, 7);
	expect_empty(test, f->load, 8);
	/* Only the output is device-writable. */
	KUNIT_EXPECT_EQ(test, f->writable_maps, 1);

	for (i = 0; i < 2; i++)
		KUNIT_ASSERT_EQ(test, ane_h16_session_execute(&f->s, p), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x28), 3);
	for (i = 0; i < 3; i++) {
		const u8 *d = f->exec + 0x60 + i * 0x30;

		KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 4), ids[i]);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 8), classes[i]);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 0x10), 3);
		KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x18), p->surfaces[i]->dva);
		KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x20), SZ_32K);
	}
	KUNIT_EXPECT_FALSE(test, f->s.quarantined);
	KUNIT_EXPECT_EQ(test, ane_h16_session_close(&f->s, p), 0);
}

/* Synthetic HWX with a kernel section: slot 1 is class 3 with index 1007. */
static void program_kernel_slots_test(struct kunit *test)
{
	struct ane_h16_hwx_snapshot *snapshot = fw_snapshot(test, false, true);
	struct ane_h16_load_slot slots[ANE_H16_LOAD_SLOTS];
	struct ane_h16_load_layout layout = {};
	struct ane_h16_load_plan *plan = kunit_kzalloc(test, sizeof(*plan), GFP_KERNEL);
	struct ane_h16_hwx *hwx = kunit_kzalloc(test, sizeof(*hwx), GFP_KERNEL);
	unsigned int i, kernel = ANE_H16_REGIONS, text = ANE_H16_REGIONS;

	KUNIT_ASSERT_NOT_NULL(test, snapshot);
	KUNIT_ASSERT_NOT_NULL(test, plan);
	KUNIT_ASSERT_NOT_NULL(test, hwx);
	KUNIT_ASSERT_EQ(test, ane_h16_load_plan_create(snapshot, plan), 0);
	KUNIT_ASSERT_EQ(test, ane_h16_hwx_validate(snapshot->bytes, snapshot->size, hwx), 0);
	for (i = 0; i < plan->count; i++) {
		layout.regions[i] = (struct ane_h16_load_mapping) {
			.dva = 0x10002000000ULL + i * SZ_1M, .size = plan->allocation[i],
			.permissions = plan->regions[i].permissions,
		};
		if (plan->regions[i].kind == ANE_H16_REGION_KERNEL)
			kernel = i;
		if (plan->regions[i].kind == ANE_H16_REGION_TEXT)
			text = i;
	}
	KUNIT_ASSERT_LT(test, kernel, ANE_H16_REGIONS);
	KUNIT_ASSERT_LT(test, text, ANE_H16_REGIONS);
	layout.region_count = plan->count;
	for (i = 0; i < ANE_H16_AUX_COUNT; i++) {
		layout.aux_dva[i] = 0x10003000000ULL + i * SZ_16K;
		layout.aux_size[i] = test_aux_size[i];
	}
	KUNIT_ASSERT_EQ(test, ane_h16_load_slots(hwx, plan, &layout, slots), 0);
	KUNIT_EXPECT_TRUE(test, slots[1].present);
	KUNIT_EXPECT_EQ(test, slots[1].resource, 1007);
	KUNIT_EXPECT_EQ(test, slots[1].class, 3);
	KUNIT_EXPECT_EQ(test, slots[1].dva, layout.regions[kernel].dva);
	KUNIT_EXPECT_EQ(test, slots[1].size, hwx->kernel.size);
	KUNIT_EXPECT_EQ(test, slots[2].resource, slots[1].resource - 1);
	KUNIT_EXPECT_EQ(test, slots[2].dva,
			layout.regions[text].dva + (hwx->text.vm - plan->regions[text].vm));
	KUNIT_EXPECT_EQ(test, slots[2].size, hwx->text.size);
	for (i = 0; i < ANE_H16_LOAD_SLOTS; i++)
		KUNIT_EXPECT_EQ(test, slots[i].present, i != 5 && i < 7);
}

static void program_replies_test(struct kunit *test)
{
	struct ane_h16_load_slot slots[ANE_H16_LOAD_SLOTS] = {};
	u8 request[ANE_H16_CREATE_SIZE], reply[ANE_H16_CREATE_SIZE];
	u32 id;

	slots[0] = (struct ane_h16_load_slot) {
		.present = true, .resource = 1002, .class = 4,
		.dva = 0x10002000000ULL, .size = 0x298,
	};
	KUNIT_ASSERT_EQ(test, ane_h16_load_encode(request, ANE_H16_LOAD_SIZE, slots), 0);
	memcpy(reply, request, ANE_H16_LOAD_SIZE);
	/* ID 0 is valid. */
	put_unaligned_le32(0, reply + 0x1b8);
	KUNIT_EXPECT_EQ(test, ane_h16_load_reply(reply, ANE_H16_LOAD_SIZE, request, &id), 0);
	KUNIT_EXPECT_EQ(test, id, 0);
	/* Request ID left unchanged. */
	put_unaligned_le32(0xffffffff, reply + 0x1b8);
	KUNIT_EXPECT_EQ(test, ane_h16_load_reply(reply, ANE_H16_LOAD_SIZE, request, &id), -EPROTO);
	put_unaligned_le32(0x61, reply + 0x1b8);
	/* Nonzero acknowledgment, wrong length, wrong command, altered slot echo. */
	put_unaligned_le16(1, reply + 6);
	KUNIT_EXPECT_EQ(test, ane_h16_load_reply(reply, ANE_H16_LOAD_SIZE, request, &id),
			-EREMOTEIO);
	put_unaligned_le16(0, reply + 6);
	KUNIT_EXPECT_EQ(test, ane_h16_load_reply(reply, ANE_H16_LOAD_SIZE - 8, request, &id),
			-EPROTO);
	put_unaligned_le16(ANE_H16_CREATE_PROCESS, reply + 4);
	KUNIT_EXPECT_EQ(test, ane_h16_load_reply(reply, ANE_H16_LOAD_SIZE, request, &id), -EPROTO);
	put_unaligned_le16(ANE_H16_LOAD_PROGRAM, reply + 4);
	reply[0x20] ^= 1;
	KUNIT_EXPECT_EQ(test, ane_h16_load_reply(reply, ANE_H16_LOAD_SIZE, request, &id), -EPROTO);
	reply[0x20] ^= 1;
	KUNIT_EXPECT_EQ(test, ane_h16_load_reply(reply, ANE_H16_LOAD_SIZE, request, &id), 0);
	KUNIT_EXPECT_EQ(test, id, 0x61);

	/* CREATE: zero body, process ID 0 valid, any nonzero body refused. */
	KUNIT_ASSERT_EQ(test, ane_h16_create_encode(request, ANE_H16_CREATE_SIZE, 0x61), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(request + 4), ANE_H16_CREATE_PROCESS);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(request + 8), 0x61);
	KUNIT_EXPECT_FALSE(test, memchr_inv(request + 0xc, 0, ANE_H16_CREATE_SIZE - 0xc));
	memcpy(reply, request, ANE_H16_CREATE_SIZE);
	KUNIT_EXPECT_EQ(test, ane_h16_create_reply(reply, ANE_H16_CREATE_SIZE, 0x61, &id), 0);
	KUNIT_EXPECT_EQ(test, id, 0);
	reply[0x257] = 1;
	KUNIT_EXPECT_EQ(test, ane_h16_create_reply(reply, ANE_H16_CREATE_SIZE, 0x61, &id),
			-EPROTO);
	reply[0x257] = 0;
	KUNIT_EXPECT_EQ(test, ane_h16_create_reply(reply, ANE_H16_CREATE_SIZE, 0x60, &id),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, ane_h16_create_reply(reply, 0x10, 0x61, &id), -EPROTO);
	put_unaligned_le16(2, reply + 6);
	KUNIT_EXPECT_EQ(test, ane_h16_create_reply(reply, ANE_H16_CREATE_SIZE, 0x61, &id),
			-EREMOTEIO);
	KUNIT_EXPECT_EQ(test, ane_h16_create_encode(request, 0x10, 0x61), -EINVAL);

	/* TERMINATE 0x10 and UNLOAD 0xc carry IDs that the reply must echo. */
	KUNIT_ASSERT_EQ(test, ane_h16_terminate_encode(request, 0x10, 0x61, 0), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(request + 4), ANE_H16_TERMINATE_PROCESS);
	memcpy(reply, request, 0x10);
	KUNIT_EXPECT_EQ(test, ane_h16_id_reply(reply, 0x10, request), 0);
	put_unaligned_le16(1, reply + 6);
	KUNIT_EXPECT_EQ(test, ane_h16_id_reply(reply, 0x10, request), -EREMOTEIO);
	put_unaligned_le16(0, reply + 6);
	put_unaligned_le16(ANE_H16_UNLOAD_PROGRAM, reply + 4);
	KUNIT_EXPECT_EQ(test, ane_h16_id_reply(reply, 0x10, request), -EPROTO);
	KUNIT_ASSERT_EQ(test, ane_h16_unload_encode(request, 0xc, 0x61), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(request + 4), ANE_H16_UNLOAD_PROGRAM);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(request + 8), 0x61);
	KUNIT_EXPECT_EQ(test, ane_h16_unload_encode(request, 0x10, 0x61), -EINVAL);
	KUNIT_EXPECT_EQ(test, ane_h16_gate_encode(request, 0xc, 2), -EINVAL);
}

static void program_deny_list_test(struct kunit *test)
{
	static const u16 denied[] = { 0x27, 0x28, 0x30, 0x31, 0x34 };
	static const u16 allowed[] = { 0x0, 0x3, 0x4, 0x22, 0x2d, 0x2f, 0x200, 0x20c };
	struct prog_fw *f = fw_create(test);
	u8 command[16] = {};
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	for (i = 0; i < ARRAY_SIZE(denied); i++) {
		KUNIT_EXPECT_TRUE(test, ane_fw_command_denied(denied[i]));
		put_unaligned_le16(denied[i], command + 4);
		KUNIT_EXPECT_EQ(test, ane_fw_transport_publish(&f->t, ANE_FW_IO, command,
							       sizeof(command), 100), -EPERM);
		KUNIT_EXPECT_EQ(test, ane_fw_transport_publish(&f->t, ANE_FW_DATA_CHAIN,
							       command, sizeof(command), 100),
				-EPERM);
	}
	for (i = 0; i < ARRAY_SIZE(allowed); i++)
		KUNIT_EXPECT_FALSE(test, ane_fw_command_denied(allowed[i]));
	/* Nothing reached a ring and the transport stays usable. */
	KUNIT_EXPECT_EQ(test, f->count, 0);
	KUNIT_EXPECT_EQ(test, f->t.error, 0);
	KUNIT_EXPECT_EQ(test, ane_h16_stage_parse("program"), ANE_H16_PROGRAM);
}

static void program_custom_bar_test(struct kunit *test)
{
	struct ane_h16_hwx_snapshot *linear = fw_snapshot(test, false, false);
	struct ane_h16_hwx_snapshot *mixed = fw_snapshot(test, true, false);
	struct ane_h16_custom_bar bars[ANE_H16_MAX_CUSTOM_BARS];
	unsigned int count = 9;

	KUNIT_ASSERT_NOT_NULL(test, linear);
	KUNIT_ASSERT_NOT_NULL(test, mixed);
	KUNIT_ASSERT_EQ(test, ane_h16_custom_bars(linear, &linear->info, 36, bars, &count), 0);
	KUNIT_ASSERT_EQ(test, count, 1);
	KUNIT_EXPECT_EQ(test, bars[0].configuration, 1);
	KUNIT_EXPECT_EQ(test, bars[0].value_type, 0);
	KUNIT_EXPECT_EQ(test, bars[0].bar, 4);
	KUNIT_EXPECT_EQ(test, bars[0].bit_offset, 7);
	KUNIT_EXPECT_EQ(test, bars[0].bit_width, 8);
	KUNIT_EXPECT_EQ(test, bars[0].flags, 0);
	KUNIT_EXPECT_EQ(test, bars[0].value, 36);
	/* The DSID must fit the field. */
	KUNIT_EXPECT_EQ(test, ane_h16_custom_bars(linear, &linear->info, 256, bars, &count),
			-ENOEXEC);
	/* Scratch form: no established tuple layout, no record. */
	KUNIT_ASSERT_EQ(test, ane_h16_custom_bars(mixed, &mixed->info, 36, bars, &count), 0);
	KUNIT_EXPECT_EQ(test, count, 0);
	/* Usage type to descriptor class, round 5 section 7. */
	KUNIT_EXPECT_EQ(test, ane_h16_usage_class(1), 0);
	KUNIT_EXPECT_EQ(test, ane_h16_usage_class(2), 1);
	KUNIT_EXPECT_EQ(test, ane_h16_usage_class(5), 6);
	KUNIT_EXPECT_EQ(test, ane_h16_usage_class(7), 2);
	KUNIT_EXPECT_EQ(test, ane_h16_usage_class(10), 4);
	KUNIT_EXPECT_EQ(test, ane_h16_usage_class(13), 0);
	KUNIT_EXPECT_EQ(test, ane_h16_usage_class(0), 8);
	KUNIT_EXPECT_EQ(test, ane_h16_usage_class(99), 8);
}

static void program_session_test(struct kunit *test)
{
	static const u16 expected[] = {
		0x200, 0x202, 0x2d, 0x20c, 0x2d, 0x2d, 0x20c, 0x2d, 0x203, 0x201,
	};
	static const u32 gates[] = { 0, 0, 0, 0, 1, 0, 0, 1 };
	struct prog_fw *f = fw_create(test);
	struct ane_h16_program *p = NULL, *second = NULL, *third = NULL;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, fw_load_program(f, &p), 0);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->key.program, 0x11);
	KUNIT_EXPECT_EQ(test, p->key.process, 0);
	KUNIT_EXPECT_EQ(test, p->resource_count, 2);
	/* Firmware saw the request ID and the synthetic slots. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->load + 0x1b8), 0xffffffff);
	expect_slot(test, f->load, 0, 1002, 4, p->aux[0].dva,
		    test_aux_size[ANE_H16_AUX_GENERIC]);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->load + ANE_H16_LOAD_SLOT_OFFSET +
						 ANE_H16_LOAD_SLOT_SIZE + 4), 1007);
	expect_slot(test, f->load, 6, 1005, 4, p->aux[3].dva, 0x28);
	/* Only the output surface is device-writable. */
	KUNIT_EXPECT_EQ(test, f->writable_maps, 1);

	KUNIT_EXPECT_EQ(test, ane_h16_session_execute(&f->s, p), 0);
	/* The first request after boot sets up EXELOOP state: queued. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x30), ANE_H16_EXECUTE_QUEUED);
	KUNIT_EXPECT_EQ(test, ane_h16_session_execute(&f->s, p), 0);
	/* EXECUTE: IDs, priority 5, UUID 2, two descriptors and one mcache record. */
	KUNIT_EXPECT_EQ(test, f->exec_length, 0xcc0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 8), 0x11);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0xc), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x18), 5);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->exec + 0x20), 2);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x28), 2);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x60 + 8), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x90 + 8), 1);
	/* Descriptor IDs are the generic-section IDs. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x60 + 4), TEST_GENERIC_BASE);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x90 + 4), TEST_GENERIC_BASE + 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->exec + 0x60 + 0x18),
			p->surfaces[0]->dva);
	/* Each descriptor covers its whole HWX section. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(f->exec + 0x60 + 0x20),
			p->hwx.surfaces[0].section.size);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0xca0), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0xca8 + 8), 4);
	/* Later requests of the same program take the direct path. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x30), 0);

	/* A second LOAD while the first is loaded refuses, before any traffic. */
	i = f->count;
	KUNIT_EXPECT_EQ(test, fw_load_program(f, &second), -EBUSY);
	KUNIT_EXPECT_EQ(test, f->count, i);
	KUNIT_EXPECT_PTR_EQ(test, second, NULL);

	KUNIT_EXPECT_EQ(test, ane_h16_session_close(&f->s, p), 0);
	KUNIT_EXPECT_TRUE(test, p->closed);
	KUNIT_EXPECT_EQ(test, ane_h16_session_execute(&f->s, p), -EINVAL);

	KUNIT_ASSERT_EQ(test, f->count, ARRAY_SIZE(expected));
	for (i = 0; i < ARRAY_SIZE(expected); i++)
		KUNIT_EXPECT_EQ(test, f->ids[i], expected[i]);
	for (i = 0; i < ARRAY_SIZE(gates); i++)
		if (f->ids[i] == 0x2d)
			KUNIT_EXPECT_EQ(test, f->values[i], gates[i]);

	/* After TERMINATE and UNLOAD a new program loads in the same session. */
	KUNIT_ASSERT_EQ(test, fw_load_program(f, &second), 0);
	KUNIT_ASSERT_NOT_NULL(test, second);
	KUNIT_EXPECT_EQ(test, second->generation, 2);
	/* The firmware reuses the program ID; the session tells them apart. */
	KUNIT_EXPECT_EQ(test, second->key.program, p->key.program);
	KUNIT_ASSERT_EQ(test, ane_h16_session_execute(&f->s, second), 0);
	/* The engine last ran the first program: this request is queued. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x30), ANE_H16_EXECUTE_QUEUED);
	KUNIT_ASSERT_EQ(test, ane_h16_session_execute(&f->s, second), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x30), 0);
	i = f->count;
	KUNIT_EXPECT_EQ(test, fw_load_program(f, &third), -EBUSY);
	KUNIT_EXPECT_EQ(test, f->count, i);
	KUNIT_EXPECT_PTR_EQ(test, third, NULL);
	KUNIT_EXPECT_EQ(test, ane_h16_session_close(&f->s, second), 0);
	/* Closing the first program's file late is harmless. */
	KUNIT_EXPECT_EQ(test, ane_h16_session_close(&f->s, p), 0);
	KUNIT_EXPECT_FALSE(test, f->s.quarantined);
}

/* A linked program: the HWX scratch is a kernel buffer sent with every EXECUTE. */
static void program_scratch_test(struct kunit *test)
{
	struct ane_h16_process_binding bindings[ANE_H16_SURFACES], scratch;
	struct ane_h16_hwx_snapshot *snapshot = fw_snapshot(test, true, false);
	struct ane_h16_hwx *hwx = kunit_kzalloc(test, sizeof(*hwx), GFP_KERNEL);
	struct prog_fw *f = fw_create(test);
	struct ane_h16_program *p = NULL;
	u8 *g = test_aux[ANE_H16_AUX_GENERIC], *e;
	unsigned int count;
	size_t size;
	const u8 *d;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, hwx);
	KUNIT_ASSERT_NOT_NULL(test, snapshot);
	KUNIT_ASSERT_EQ(test, ane_h16_hwx_validate(snapshot->bytes, snapshot->size, hwx), 0);
	KUNIT_ASSERT_EQ(test, hwx->scratch.size, SZ_16K);
	size = generic_fill(g, hwx, TEST_GENERIC_BASE);
	e = g + ANE_H16_GENERIC_HEADER + 2 * ANE_H16_GENERIC_ENTRY;
	KUNIT_ASSERT_EQ(test, ane_h16_process_bindings(hwx, g, size, bindings, &count,
						       &scratch), 0);
	KUNIT_EXPECT_EQ(test, count, 2);
	KUNIT_EXPECT_EQ(test, scratch.index, TEST_GENERIC_BASE + 2);
	KUNIT_EXPECT_EQ(test, scratch.class, 0);
	KUNIT_EXPECT_EQ(test, scratch.hwx_vm, hwx->scratch.vm);
	/* No scratch entry, an output-like or short one, or a reused ID refuse. */
	put_unaligned_le32(2, g + ANE_H16_GENERIC_COUNT);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, g, size - ANE_H16_GENERIC_ENTRY,
						       bindings, &count, &scratch), -ENOEXEC);
	put_unaligned_le32(3, g + ANE_H16_GENERIC_COUNT);
	put_unaligned_le32(2, e + 0x10);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, g, size, bindings, &count,
						       &scratch), -ENOEXEC);
	put_unaligned_le32(1, e + 0x10);
	put_unaligned_le64(SZ_16K - 4, e + 0x20);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, g, size, bindings, &count,
						       &scratch), -ENOEXEC);
	put_unaligned_le64(SZ_16K, e + 0x20);
	put_unaligned_le32(TEST_GENERIC_BASE, e + 4);
	KUNIT_EXPECT_EQ(test, ane_h16_process_bindings(hwx, g, size, bindings, &count,
						       &scratch), -ENOEXEC);
	put_unaligned_le32(TEST_GENERIC_BASE + 2, e + 4);

	test_aux_size[ANE_H16_AUX_GENERIC] = size;
	KUNIT_ASSERT_EQ(test, ane_h16_session_load(&f->s, snapshot, test_aux_ptr, test_aux_size,
						   &p), 0);
	KUNIT_ASSERT_TRUE(test, p->has_scratch);
	/* Userspace still sees two surfaces. */
	KUNIT_EXPECT_EQ(test, p->resource_count, 2);
	/* The output and the scratch are the device-writable mappings. */
	KUNIT_EXPECT_EQ(test, f->writable_maps, 2);
	KUNIT_ASSERT_EQ(test, ane_h16_session_execute(&f->s, p), 0);
	KUNIT_EXPECT_EQ(test, f->exec_length, ANE_H16_EXECUTE_BASE_SIZE);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->exec + 0x28), 3);
	d = f->exec + 0x60 + 2 * 0x30;
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 4), TEST_GENERIC_BASE + 2);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(d + 8), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x18), p->scratch.mapping_dva);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(d + 0x20), SZ_16K);
	KUNIT_EXPECT_EQ(test, ane_h16_session_close(&f->s, p), 0);
	KUNIT_EXPECT_FALSE(test, f->s.quarantined);
}

static void program_bad_load_test(struct kunit *test)
{
	static const enum fw_mode modes[] = { FW_LOAD_ID_UNCHANGED, FW_LOAD_NAK, FW_CREATE_BODY };
	static const int errors[] = { -EPROTO, -EREMOTEIO, -EPROTO };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		struct prog_fw *f = fw_create(test);
		struct ane_h16_program *p = NULL;

		KUNIT_ASSERT_NOT_NULL(test, f);
		f->mode = modes[i];
		KUNIT_EXPECT_EQ(test, fw_load_program(f, &p), errors[i]);
		KUNIT_EXPECT_PTR_EQ(test, p, NULL);
		KUNIT_EXPECT_TRUE(test, f->s.quarantined);
		/* No further firmware traffic after quarantine. */
		KUNIT_EXPECT_EQ(test, f->count, modes[i] == FW_CREATE_BODY ? 2 : 1);
		KUNIT_EXPECT_EQ(test, fw_load_program(f, &p), -EIO);
	}
}

static void program_bad_report_test(struct kunit *test)
{
	static const enum fw_mode modes[] = {
		FW_REPORT_FOREIGN_UUID, FW_REPORT_WRONG_PROCESS, FW_REPORT_FAILURE,
	};
	static const int errors[] = { -ETIMEDOUT, -EPROTO, -EIO };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		struct prog_fw *f = fw_create(test);
		struct ane_h16_program *p = NULL;
		unsigned int sent;

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_EQ(test, fw_load_program(f, &p), 0);
		f->mode = modes[i];
		KUNIT_EXPECT_EQ(test, ane_h16_session_execute(&f->s, p), errors[i]);
		/* No closing 0x002d after an uncertain or failed request. */
		KUNIT_EXPECT_EQ(test, f->ids[f->count - 1], ANE_H16_EXECUTE);
		KUNIT_EXPECT_TRUE(test, f->s.quarantined);
		KUNIT_EXPECT_TRUE(test, f->s.pending.quarantined);
		sent = f->count;
		KUNIT_EXPECT_EQ(test, ane_h16_session_execute(&f->s, p), -EIO);
		/* Teardown is not attempted while quarantined; mappings stay. */
		KUNIT_EXPECT_EQ(test, ane_h16_session_close(&f->s, p), -EIO);
		KUNIT_EXPECT_EQ(test, f->count, sent);
	}
}

/* A request that starts and never ends names the wait step and the last state. */
static void program_stall_trace_test(struct kunit *test)
{
	struct prog_fw *f = fw_create(test);
	struct ane_h16_program *p = NULL;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, fw_load_program(f, &p), 0);
	f->mode = FW_REPORT_STALL;
	KUNIT_EXPECT_EQ(test, ane_h16_session_execute(&f->s, p), -ETIMEDOUT);
	/* The fake firmware asked for no log buffer: nothing to print. */
	KUNIT_EXPECT_EQ(test, f->s.fwlog_lines, 0);
	KUNIT_EXPECT_TRUE(test, f->s.quarantined);
	KUNIT_EXPECT_EQ(test, f->fail_step, 3);
	KUNIT_EXPECT_EQ(test, f->fail_flags, ANE_H16_EXECUTE_QUEUED);
	KUNIT_EXPECT_EQ(test, f->fail_reports, 2);
	KUNIT_EXPECT_EQ(test, f->fail_state, 3);
	KUNIT_EXPECT_EQ(test, f->fail_other, 0);

	/* Reports for another UUID only: none seen for this request. */
	f = fw_create(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, fw_load_program(f, &p), 0);
	f->mode = FW_REPORT_FOREIGN_UUID;
	KUNIT_EXPECT_EQ(test, ane_h16_session_execute(&f->s, p), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->fail_step, 3);
	KUNIT_EXPECT_EQ(test, f->fail_reports, 0);
	KUNIT_EXPECT_EQ(test, f->fail_state, U32_MAX);
	KUNIT_EXPECT_EQ(test, f->fail_other, 2);
}

/* The phases of one EXECUTE add up, and only successful ones are accounted. */
static void program_timing_test(struct kunit *test)
{
	struct prog_fw *f = fw_create(test);
	struct ane_h16_program *p = NULL;
	const struct ane_h16_exec_sample *x;
	struct ane_h16_exec_stats st;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, fw_load_program(f, &p), 0);
	f->doorbell_ns = 100;
	f->mode = FW_REPORT_LATE;
	f->late_polls = 7;
	KUNIT_ASSERT_EQ(test, ane_h16_session_execute(&f->s, p), 0);
	x = &f->s.timing;
	KUNIT_EXPECT_EQ(test, x->ns[ANE_H16_T_GATE_OPEN], 100ULL);
	KUNIT_EXPECT_EQ(test, x->ns[ANE_H16_T_SUBMIT_ACK], 100ULL);
	/* The state-0 report was there with the ack. */
	KUNIT_EXPECT_EQ(test, x->ns[ANE_H16_T_ACK_REPORT], 0ULL);
	/* Seven sleeps of 10 ns until the completion. */
	KUNIT_EXPECT_EQ(test, x->ns[ANE_H16_T_REPORT_DONE], 70ULL);
	KUNIT_EXPECT_EQ(test, x->ns[ANE_H16_T_GATE_CLOSE], 100ULL);
	KUNIT_EXPECT_EQ(test, x->ns[ANE_H16_T_FIRMWARE], 370ULL);
	KUNIT_EXPECT_EQ(test, x->polls, 7ULL);

	ane_h16_session_account(&f->s, 5, 6, 400);
	f->mode = FW_OK;
	KUNIT_ASSERT_EQ(test, ane_h16_session_execute(&f->s, p), 0);
	/* All three reports came with the ack: nothing between them. */
	KUNIT_EXPECT_EQ(test, f->s.timing.ns[ANE_H16_T_REPORT_DONE], 0ULL);
	KUNIT_EXPECT_EQ(test, f->s.timing.polls, 0ULL);
	ane_h16_session_account(&f->s, 15, 8, 340);
	ane_h16_session_stats(&f->s, &st, false);
	KUNIT_EXPECT_EQ(test, st.count, 2ULL);
	KUNIT_EXPECT_EQ(test, st.last.ns[ANE_H16_T_COPY_IN], 15ULL);
	KUNIT_EXPECT_EQ(test, st.sum.ns[ANE_H16_T_COPY_IN], 20ULL);
	KUNIT_EXPECT_EQ(test, st.min.ns[ANE_H16_T_REPORT_DONE], 0ULL);
	KUNIT_EXPECT_EQ(test, st.max.ns[ANE_H16_T_REPORT_DONE], 70ULL);
	KUNIT_EXPECT_EQ(test, st.min.ns[ANE_H16_T_IOCTL], 340ULL);
	KUNIT_EXPECT_EQ(test, st.max.ns[ANE_H16_T_IOCTL], 400ULL);
	KUNIT_EXPECT_EQ(test, st.sum.polls, 7ULL);
	KUNIT_EXPECT_EQ(test, st.min.polls, 0ULL);
	KUNIT_EXPECT_EQ(test, st.max.polls, 7ULL);

	/* A reset starts over: the next sample sets min and max alone. */
	ane_h16_session_stats(&f->s, NULL, true);
	ane_h16_session_account(&f->s, 9, 9, 500);
	ane_h16_session_stats(&f->s, &st, false);
	KUNIT_EXPECT_EQ(test, st.count, 1ULL);
	KUNIT_EXPECT_EQ(test, st.min.ns[ANE_H16_T_IOCTL], 500ULL);
	KUNIT_EXPECT_EQ(test, st.sum.ns[ANE_H16_T_COPY_OUT], 9ULL);

	/* A stalled request times out and leaves the statistics alone. */
	f->mode = FW_REPORT_STALL;
	KUNIT_EXPECT_EQ(test, ane_h16_session_execute(&f->s, p), -ETIMEDOUT);
	ane_h16_session_stats(&f->s, &st, false);
	KUNIT_EXPECT_EQ(test, st.count, 1ULL);
}

static void program_fwlog_line_test(struct kunit *test)
{
	static const char text[] = "R: [0:1]: CMD = 0x020c\tok\x01  ";
	u8 *r = kunit_kzalloc(test, ANE_H16_FWLOG_RECORD, GFP_KERNEL);
	char line[ANE_H16_FWLOG_RECORD];

	KUNIT_ASSERT_NOT_NULL(test, r);
	KUNIT_EXPECT_EQ(test, ane_h16_fwlog_line(r, line), 0);
	r[0] = 0x12;
	memcpy(r + 2, text, sizeof(text));
	KUNIT_EXPECT_EQ(test, ane_h16_fwlog_line(r, line), 26);
	KUNIT_EXPECT_STREQ(test, line, "R: [0:1]: CMD = 0x020c ok.");
	/* An unterminated record stops at its end. */
	memset(r + 2, 'a', ANE_H16_FWLOG_RECORD - 2);
	KUNIT_EXPECT_EQ(test, ane_h16_fwlog_line(r, line), ANE_H16_FWLOG_RECORD - 2);
	KUNIT_EXPECT_EQ(test, line[ANE_H16_FWLOG_RECORD - 2], 0);
}

/* The transport finds the log allocation by its tag. */
static void program_shared_tag_test(struct kunit *test)
{
	struct ane_fw_transport *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->shared_count = 2;
	t->shared_tag[0] = ANE_FW_SHARED_LOG;
	t->shared_tag[1] = ANE_FW_SHARED_EXPP;
	t->shared[0].dva = 0x10001300000ULL;
	t->shared[1].dva = 0x10001358000ULL;
	KUNIT_EXPECT_PTR_EQ(test, ane_fw_transport_shared_tagged(t, ANE_FW_SHARED_LOG),
			    &t->shared[0]);
	KUNIT_EXPECT_PTR_EQ(test, ane_fw_transport_shared_tagged(t, ANE_FW_SHARED_EXPP),
			    &t->shared[1]);
	t->shared_retired[0] = true;
	KUNIT_EXPECT_NULL(test, ane_fw_transport_shared_tagged(t, ANE_FW_SHARED_LOG));
	KUNIT_EXPECT_NULL(test, ane_fw_transport_shared_tagged(t, 0x43434d44));
}

/*
 * Each completed EXECUTE gives its arena back, so a long run never fills
 * the pool; LOAD and CREATE keep theirs.
 */
static void program_pool_recycle_test(struct kunit *test)
{
	struct prog_fw *f = fw_create(test);
	struct ane_h16_program *p = NULL;
	size_t mark;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, fw_load_program(f, &p), 0);
	mark = ane_fw_transport_mark(&f->t);
	KUNIT_EXPECT_GT(test, mark, 0);
	/* Far more requests than one pass through the 256 KiB arena holds. */
	for (i = 0; i < 1000; i++) {
		KUNIT_ASSERT_EQ(test, ane_h16_session_execute(&f->s, p), 0);
		KUNIT_ASSERT_EQ(test, ane_fw_transport_mark(&f->t), mark);
	}
	KUNIT_EXPECT_FALSE(test, f->s.quarantined);
	/* A failed request keeps its space: nothing is reused after a fault. */
	f->mode = FW_REPORT_FAILURE;
	KUNIT_EXPECT_EQ(test, ane_h16_session_execute(&f->s, p), -EIO);
	KUNIT_EXPECT_GT(test, f->t.pool_used, mark);
	/* Release refuses while a command is in flight or below the arena use. */
	f->t.pending[ANE_FW_IO].active = true;
	KUNIT_EXPECT_EQ(test, ane_fw_transport_release(&f->t, mark), -EBUSY);
	f->t.pending[ANE_FW_IO].active = false;
	KUNIT_EXPECT_EQ(test, ane_fw_transport_release(&f->t, f->t.pool_used + 1), -EINVAL);
}

/* Running out of arena refuses cleanly, before any traffic. */
static void program_pool_test(struct kunit *test)
{
	struct prog_fw *f = fw_create(test);
	struct ane_h16_program *p = NULL;
	unsigned int sent;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, fw_load_program(f, &p), 0);
	sent = f->count;
	f->t.pool_used = f->t.pool.size - 0x200;
	KUNIT_EXPECT_EQ(test, ane_h16_session_execute(&f->s, p), -ENOSPC);
	KUNIT_EXPECT_EQ(test, f->count, sent);
	KUNIT_EXPECT_FALSE(test, f->s.quarantined);
	KUNIT_EXPECT_EQ(test, ane_h16_session_close(&f->s, p), 0);
	KUNIT_EXPECT_EQ(test, f->count, sent + 2);
}

static struct kunit_case program_cases[] = {
	KUNIT_CASE(program_add128_slots_test),
	KUNIT_CASE(program_kernel_slots_test),
	KUNIT_CASE(program_add128_hwx_test),
	KUNIT_CASE(program_replies_test),
	KUNIT_CASE(program_deny_list_test),
	KUNIT_CASE(program_custom_bar_test),
	KUNIT_CASE(program_session_test),
	KUNIT_CASE(program_scratch_test),
	KUNIT_CASE(program_bad_load_test),
	KUNIT_CASE(program_bad_report_test),
	KUNIT_CASE(program_stall_trace_test),
	KUNIT_CASE(program_timing_test),
	KUNIT_CASE(program_fwlog_line_test),
	KUNIT_CASE(program_shared_tag_test),
	KUNIT_CASE(program_pool_test),
	KUNIT_CASE(program_pool_recycle_test),
	{}
};

static struct kunit_suite program_suite = {
	.name = "ane-h16-program",
	.test_cases = program_cases,
};

kunit_test_suite(program_suite);
MODULE_DESCRIPTION("KUnit tests for the H16 program lifecycle");
MODULE_LICENSE("Dual MIT/GPL");
