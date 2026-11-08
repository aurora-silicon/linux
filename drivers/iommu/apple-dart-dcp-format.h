/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DART_DCP_FORMAT_H
#define APPLE_DART_DCP_FORMAT_H

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define DCP_DART_PAGE		0x4000ULL
#define DCP_DART_SLICE		0x2000000ULL
#define DCP_DART_ALIAS		0x10000000000ULL
#define DCP_DART_APERTURE		0x1000000000ULL
#define DCP_DART_ENTRIES		2048
#define DCP_DART_PADDR		0x3ffffffc00ULL
#define DCP_DART_LEAF_FIELDS	0xffffff3ffffffc0fULL
#define DCP_ADT_MAX_SEGMENTS	16

struct dcp_adt_segment {
	u64 phys;
	u64 iova;
	u64 size;
	u32 kind;
};

struct dcp_adt_tables {
	u64 start;
	u64 end;
	u64 dram_start;
	u64 dram_size;
	u32 seen;
	u32 num_segments;
	struct dcp_adt_segment segment[DCP_ADT_MAX_SEGMENTS];
};

/* Internal display frontend: separate inherited storage for SID0 and SID16. */
struct dcp_adt_frontend {
	u64 start[2];
	u64 end[2];
	u32 seen;
	u32 nodes;
};

/* One J713 display coprocessor: its ADT nodes and its firmware segments. */
struct dcp_adt_instance {
	const char *dart;	/* firmware DART, SID23 */
	const char *dcp;
	const char *nub;
	const char *disp;	/* display DART; only the internal frontend is admitted */
	const char *segment_names;
	const u32 *kinds;
	u32 num_segments;
};

static const u32 dcp_adt_kinds_internal[] = {1, 0, 10, 2, 2, 2, 2};
static const u32 dcp_adt_kinds_external[] = {1, 0, 10, 2};
/* T8132 main external DCP (iop-dcp-nub lists six segments). */
static const u32 dcp_adt_kinds_main_external[] = {1, 0, 10, 2, 2, 2};

static const struct dcp_adt_instance dcp_adt_internal = {
	.dart = "dart-dcp",
	.dcp = "dcp",
	.nub = "iop-dcp-nub",
	.disp = "dart-disp0",
	.segment_names = "__TEXT;__DATA;__OS_LOG;unknown;unknown;unknown;unknown",
	.kinds = dcp_adt_kinds_internal,
	.num_segments = ARRAY_SIZE(dcp_adt_kinds_internal),
};

/* Main external DCP: internal node names and six firmware segments. */
static const struct dcp_adt_instance dcp_adt_main_external = {
	.dart = "dart-dcp",
	.dcp = "dcp",
	.nub = "iop-dcp-nub",
	.disp = "dart-disp0",
	.segment_names = "__TEXT;__DATA;__OS_LOG;unknown;unknown;unknown",
	.kinds = dcp_adt_kinds_main_external,
	.num_segments = ARRAY_SIZE(dcp_adt_kinds_main_external),
};

/* The external DCPs run the same firmware with a single extra segment. */
static const struct dcp_adt_instance dcp_adt_external[] = {
	{
		.dart = "dart-dcpext0",
		.dcp = "dcpext0",
		.nub = "iop-dcpext0-nub",
		.disp = "dart-dispext0",
		.segment_names = "__TEXT;__DATA;__OS_LOG;unknown",
		.kinds = dcp_adt_kinds_external,
		.num_segments = ARRAY_SIZE(dcp_adt_kinds_external),
	},
	{
		.dart = "dart-dcpext1",
		.dcp = "dcpext1",
		.nub = "iop-dcpext1-nub",
		.disp = "dart-dispext1",
		.segment_names = "__TEXT;__DATA;__OS_LOG;unknown",
		.kinds = dcp_adt_kinds_external,
		.num_segments = ARRAY_SIZE(dcp_adt_kinds_external),
	},
};

static inline int dcp_dart_pte_check(u64 pte, bool leaf)
{
	u64 fields = leaf ? DCP_DART_LEAF_FIELDS : DCP_DART_PADDR | 1;

	if (!pte)
		return 0;
	if (!(pte & 1) || (pte & ~fields))
		return -EINVAL;
	if (leaf && ((pte >> 52) > ((pte >> 40) & 0xfff)))
		return -EINVAL;
	return 0;
}

static inline u64 dcp_dart_pte_phys(u64 pte)
{
	return (pte & DCP_DART_PADDR) << 4;
}

static inline bool dcp_dart_pte_full_rw(u64 pte, bool coherent)
{
	return pte && !dcp_dart_pte_check(pte, true) && !(pte & 0xc) &&
		(!coherent || !(pte & 2)) && !(pte >> 52) &&
		((pte >> 40) & 0xfff) == 0xfff;
}

/* Observed J713 profile classes, not generic ADT flag bits. */
static inline bool dcp_dart_pte_firmware(u64 pte, u32 kind)
{
	if (kind != 0 && kind != 1 && kind != 2)
		return false;
	return !!(pte & 4) == (kind == 1) && dcp_dart_pte_full_rw(pte & ~4ULL, true);
}

static inline bool dcp_dart_pte_frontend(u64 pte)
{
	/* Frontend requires no-cache, unlike the coherent SID23 firmware leaves. */
	return (pte & 2) && dcp_dart_pte_full_rw(pte, false);
}

static inline const struct dcp_adt_segment *
dcp_adt_segment_match(const struct dcp_adt_tables *tables, u64 phys, u64 iova, u64 size)
{
	const struct dcp_adt_segment *found = NULL;

	for (u32 i = 0; i < tables->num_segments; i++) {
		const struct dcp_adt_segment *seg = &tables->segment[i];

		/* OS log is relocated/published separately as a physical carveout. */
		if (seg->kind == 10 || seg->phys != phys || seg->iova != iova || seg->size != size)
			continue;
		if (found)
			return NULL;
		found = seg;
	}
	return found;
}

static inline bool dcp_dart_contains(u64 base, u64 end, u64 start, u64 size)
{
	return base < end && size && start >= base && start < end && size <= end - start;
}

static inline int dcp_adt_value(struct dcp_adt_tables *out,
				const struct dcp_adt_instance *inst, unsigned int kind,
				const char *name, const u8 *value, u32 length)
{
	u32 bit = 0;

	if (kind == 1 && !strcmp(name, "target-type")) {
		bit = 1;
		/* Every M4 Mac shares the retained display handoff. */
		if (!(length == 5 && (!memcmp(value, "J713", 5) ||
				      !memcmp(value, "J715", 5) ||
				      !memcmp(value, "J604", 5) ||
				      !memcmp(value, "J623", 5) ||
				      !memcmp(value, "J624", 5))) &&
		    !(length == 6 && !memcmp(value, "J773g", 6)))
			return -ENODEV;
	} else if (kind == 3 && !strcmp(name, "compatible")) {
		bit = 2;
		if (length != 11 || memcmp(value, "dart,t8110", 11))
			return -ENODEV;
	} else if (kind == 3 && !strcmp(name, "pt-region-23")) {
		bit = 4;
		if (length != 16)
			return -EINVAL;
		out->start = get_unaligned_le64(value);
		out->end = get_unaligned_le64(value + 8);
	} else if (kind == 4 && !strcmp(name, "dram-base")) {
		bit = 8;
		if (length != 8)
			return -EINVAL;
		out->dram_start = get_unaligned_le64(value);
	} else if (kind == 4 && !strcmp(name, "dram-size")) {
		bit = 16;
		if (length != 8)
			return -EINVAL;
		out->dram_size = get_unaligned_le64(value);
	} else if (kind == 6 && !strcmp(name, "segment-ranges")) {
		bit = 32;
		if (!length || length % 32 || length / 32 > DCP_ADT_MAX_SEGMENTS)
			return -EINVAL;
		out->num_segments = length / 32;
		for (u32 i = 0; i < out->num_segments; i++) {
			struct dcp_adt_segment *seg = &out->segment[i];
			const u8 *record = value + i * 32;

			seg->phys = get_unaligned_le64(record);
			seg->iova = get_unaligned_le64(record + 16); /* remap, not original IOVA */
			seg->size = (get_unaligned_le32(record + 24) + 0x3fffULL) & ~0x3fffULL;
			seg->kind = get_unaligned_le32(record + 28);
		}
	} else if (kind == 6 && !strcmp(name, "uuid")) {
		/* Any image: the firmware version gates the layout. */
		bit = 64;
		if (length != 37 || value[36])
			return -EINVAL;
	} else if (kind == 6 && !strcmp(name, "segment-names")) {
		size_t names = strlen(inst->segment_names) + 1;

		bit = 128;
		if (length != names || memcmp(value, inst->segment_names, names))
			return -ENODEV;
	}
	if (out->seen & bit)
		return -EINVAL;
	out->seen |= bit;
	return 0;
}

static inline int dcp_adt_frontend_value(struct dcp_adt_frontend *out,
				       const char *name, const u8 *value, u32 length)
{
	u32 bit;
	unsigned int index;

	if (!strcmp(name, "compatible")) {
		bit = 1;
		if (length != 11 || memcmp(value, "dart,t8110", 11))
			return -ENODEV;
	} else if (!strcmp(name, "pt-region-0") || !strcmp(name, "pt-region-16")) {
		index = !strcmp(name, "pt-region-16");
		bit = 2U << index;
		if (length != 16)
			return -EINVAL;
		out->start[index] = get_unaligned_le64(value);
		out->end[index] = get_unaligned_le64(value + 8);
	} else {
		return 0;
	}
	if (out->seen & bit)
		return -EINVAL;
	out->seen |= bit;
	return 0;
}

/* Bounded, read-only ADT subset: one coprocessor's nodes under ROOT / arm-io, and ROOT / chosen. */
static inline int dcp_adt_node(const u8 *data, size_t size, size_t *cursor,
			       unsigned int parent, unsigned int depth,
			       const struct dcp_adt_instance *inst,
			       struct dcp_adt_tables *out, struct dcp_adt_frontend *frontend)
{
	size_t first, pos;
	u32 props, children;
	const char *node_name = NULL;
	unsigned int kind = 0;
	int ret;

	if (depth > 32 || *cursor > size || size - *cursor < 8)
		return -EINVAL;
	props = get_unaligned_le32(data + *cursor);
	children = get_unaligned_le32(data + *cursor + 4);
	if (props > 4096 || children > 4096)
		return -EINVAL;
	first = *cursor + 8;
	pos = first;
	for (u32 i = 0; i < props; i++) {
		u32 length;

		if (pos > size || size - pos < 36 || !memchr(data + pos, 0, 32))
			return -EINVAL;
		length = get_unaligned_le32(data + pos + 32) & 0x7fffffff;
		if (length > size - pos - 36 || ((length + 3ULL) & ~3ULL) > size - pos - 36)
			return -EINVAL;
		if (!strcmp((const char *)data + pos, "name")) {
			if (node_name || !length || !memchr(data + pos + 36, 0, length))
				return -EINVAL;
			node_name = (const char *)data + pos + 36;
		}
		pos += 36 + ((length + 3ULL) & ~3ULL);
	}
	if (!node_name)
		return -EINVAL;
	if (!depth)
		kind = 1;
	else if (parent == 1 && !strcmp(node_name, "arm-io"))
		kind = 2;
	else if (parent == 2 && !strcmp(node_name, inst->dart))
		kind = 3;
	else if (parent == 1 && !strcmp(node_name, "chosen"))
		kind = 4;
	else if (parent == 2 && !strcmp(node_name, inst->dcp))
		kind = 5;
	else if (parent == 5 && !strcmp(node_name, inst->nub))
		kind = 6;
	else if (parent == 2 && !strcmp(node_name, inst->disp))
		kind = 7;
	if (kind == 7 && frontend && frontend->nodes++)
		return -EINVAL;
	*cursor = pos;
	pos = first;
	for (u32 i = 0; i < props; i++) {
		u32 length = get_unaligned_le32(data + pos + 32) & 0x7fffffff;

		ret = dcp_adt_value(out, inst, kind, (const char *)data + pos, data + pos + 36,
				    length);
		if (ret)
			return ret;
		if (kind == 7 && frontend) {
			ret = dcp_adt_frontend_value(frontend, (const char *)data + pos,
						   data + pos + 36, length);
			if (ret)
				return ret;
		}
		pos += 36 + ((length + 3ULL) & ~3ULL);
	}
	for (u32 i = 0; i < children; i++) {
		ret = dcp_adt_node(data, size, cursor, kind, depth + 1, inst, out, frontend);
		if (ret)
			return ret;
	}
	return 0;
}

static inline int dcp_adt_parse_common(const void *data, size_t size,
				       const struct dcp_adt_instance *inst,
				       struct dcp_adt_tables *out,
				       struct dcp_adt_frontend *frontend)
{
	size_t cursor = 0;
	int ret;

	*out = (struct dcp_adt_tables){};
	if (frontend)
		*frontend = (struct dcp_adt_frontend){};
	if (size > 0x200000)
		return -E2BIG;
	ret = dcp_adt_node(data, size, &cursor, 0, 0, inst, out, frontend);
	if (ret)
		return ret;
	if (out->seen != 255 || out->num_segments != inst->num_segments || !out->dram_size ||
	    out->dram_start + out->dram_size < out->dram_start ||
	    out->start >= out->end || out->end - out->start > 0x100000 ||
	    ((out->start | out->end) & (DCP_DART_PAGE - 1)) ||
	    !dcp_dart_contains(out->dram_start, out->dram_start + out->dram_size,
			       out->start, out->end - out->start))
		return -EINVAL;
	for (u32 i = 0; i < out->num_segments; i++) {
		const struct dcp_adt_segment *seg = &out->segment[i];

		if (seg->kind != inst->kinds[i] || (seg->phys & (DCP_DART_PAGE - 1)) ||
		    !dcp_dart_contains(out->dram_start, out->dram_start + out->dram_size,
				       seg->phys, seg->size))
			return -EINVAL;
		if (seg->kind == 10)
			continue; /* DT's explicitly physical OS log can be relocated. */
		if ((seg->kind != 0 && seg->kind != 1 && seg->kind != 2) ||
		    (seg->iova & (DCP_DART_PAGE - 1)) ||
		    !dcp_dart_contains(DCP_DART_ALIAS, DCP_DART_ALIAS + DCP_DART_APERTURE,
				       seg->iova, seg->size))
			return -EINVAL;
		for (u32 j = 0; j < i; j++) {
			const struct dcp_adt_segment *other = &out->segment[j];

			if (other->kind != 10 && seg->iova < other->iova + other->size &&
			    other->iova < seg->iova + seg->size)
				return -EINVAL;
		}
	}
	return 0;
}

static inline int dcp_adt_parse(const void *data, size_t size, struct dcp_adt_tables *out)
{
	return dcp_adt_parse_common(data, size, &dcp_adt_internal, out, NULL);
}

static inline int dcp_adt_parse_instance(const void *data, size_t size,
					 const struct dcp_adt_instance *inst,
					 struct dcp_adt_tables *out)
{
	return dcp_adt_parse_common(data, size, inst, out, NULL);
}

/* Parse metadata only: live MMIO, RAM exclusion and page tables remain unproven. */
static inline int dcp_adt_parse_frontend(const void *data, size_t size,
					const struct dcp_adt_instance *inst,
					struct dcp_adt_tables *firmware,
					struct dcp_adt_frontend *frontend)
{
	int ret = dcp_adt_parse_common(data, size, inst ? inst : &dcp_adt_internal,
				       firmware, frontend);

	if (ret)
		return ret;
	if (frontend->nodes != 1 || frontend->seen != 7)
		return -ENODEV;
	for (unsigned int i = 0; i < 2; i++) {
		u64 start = frontend->start[i], end = frontend->end[i];

		if (end <= start || end - start != (i ? 0x10000 : 0x20000) ||
		    ((start | end) & (DCP_DART_PAGE - 1)) ||
		    !dcp_dart_contains(firmware->dram_start,
				       firmware->dram_start + firmware->dram_size,
				       start, end - start) ||
		    (start < firmware->end && firmware->start < end))
			return -EINVAL;
	}
	if (frontend->start[0] < frontend->end[1] &&
	    frontend->start[1] < frontend->end[0])
		return -EINVAL;
	return 0;
}

#endif
