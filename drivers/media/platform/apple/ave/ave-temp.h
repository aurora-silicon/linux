/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * T8112/H14G temporary stage-1 decoder. No hardware access or allocation.
 * Format facts: Linux arm64 pgtable-hwdef.h and io-pgtable-arm.c (GPL-2.0).
 * Firmware interoperability facts: SPEC-6070-hello r6/r7/r8.
 *
 * Copyright The Aurora Silicon Contributors
 */
#ifndef APPLE_AVE_TEMP_H
#define APPLE_AVE_TEMP_H

#include <linux/bits.h>
#include <linux/byteorder/generic.h>
#include <linux/string.h>
#include <linux/types.h>

#define AVE_TEMP_LO		0x18000U
#define AVE_TEMP_HI		0x20000U
#define AVE_TEMP_ROOT		0x1fff0U
#define AVE_TEMP_WORDS		((AVE_TEMP_HI - AVE_TEMP_LO) / 8)
#define AVE_TEMP_TCR		0x40808126ULL
#define AVE_TEMP_DATA_VA	0xd0000ULL
#define AVE_TEMP_PAGE		0x4000ULL
#define AVE_TEMP_ADDR		GENMASK_ULL(47, 14)

/* Private debugfs evidence, three immutable per-snapshot copies and model. */
struct ave_temp_evidence {
	__le64 observed[3][AVE_TEMP_WORDS];
	__le64 model[AVE_TEMP_WORDS];
};

struct ave_temp_inputs {
	u64 tcr, ttbr;
	u64 text_iova, data_iova, data_link;
	u64 text_size, data_size;
	u32 page_shift, page_size, index_bits;
	u64 final_va_bits, final_mode;
	u64 pt_phys, pt_virt, pt_size;
};

enum ave_temp_fault {
	AVE_TEMP_OK,
	AVE_TEMP_GEOMETRY,
	AVE_TEMP_VA_RANGE,
	AVE_TEMP_TABLE_RANGE,
	AVE_TEMP_INVALID,
	AVE_TEMP_TYPE,
	AVE_TEMP_ADDRESS,
	AVE_TEMP_ACCESS_FLAG,
	AVE_TEMP_OUTPUT,
	AVE_TEMP_PERMISSION,
};

struct ave_temp_walk {
	u64 va, output, expected, descriptor;
	u32 offset, level;
	enum ave_temp_fault fault;
	bool translated, writable, executable;
};

struct ave_temp_result {
	struct ave_temp_walk pc, store;
	u32 compared, mismatches, first_offset;
	u64 first_observed, first_expected;
	bool model_valid;
};

static inline const char *ave_temp_fault_name(enum ave_temp_fault f)
{
	switch (f) {
	case AVE_TEMP_OK: return "ok";
	case AVE_TEMP_GEOMETRY: return "unsupported geometry";
	case AVE_TEMP_VA_RANGE: return "VA out of range";
	case AVE_TEMP_TABLE_RANGE: return "table outside DATA temporary window";
	case AVE_TEMP_INVALID: return "invalid descriptor";
	case AVE_TEMP_TYPE: return "reserved descriptor type";
	case AVE_TEMP_ADDRESS: return "reserved/misaligned output";
	case AVE_TEMP_ACCESS_FLAG: return "access flag clear";
	case AVE_TEMP_OUTPUT: return "wrong output";
	case AVE_TEMP_PERMISSION: return "wrong permission";
	default: return "unknown";
	}
}

/* Small root alignment is 16 bytes, not the leaf-table's 16 KiB. */
static inline bool ave_temp_geometry(const struct ave_temp_inputs *in)
{
	return (u32)in->tcr == (u32)AVE_TEMP_TCR &&
	       !(in->tcr & GENMASK_ULL(63, 35)) &&
	       in->data_size >= AVE_TEMP_HI &&
	       in->data_iova <= BIT_ULL(48) - AVE_TEMP_HI &&
	       in->ttbr == in->data_iova + AVE_TEMP_ROOT &&
	       !(in->ttbr & 15) && !(in->data_iova & (AVE_TEMP_PAGE - 1));
}

static inline bool ave_temp_read(const __le64 *table,
				 const struct ave_temp_inputs *in, u64 address,
				 u64 *value, u32 *offset)
{
	u64 off;

	if (address < in->data_iova)
		return false;
	off = address - in->data_iova;
	if (off < AVE_TEMP_LO || off > AVE_TEMP_HI - 8 || (off & 7))
		return false;
	*offset = off;
	*value = le64_to_cpu(table[(off - AVE_TEMP_LO) / 8]);
	return true;
}

/* EL1 permissions, including hierarchical APTable/PXNTable.
 * SCTLR.WXN is clear in the qualified reset path. UXN only restricts EL0.
 * Hardware AF/dirty management and LPA2 are absent in this temporary regime.
 */
static inline struct ave_temp_walk
ave_temp_walk(const __le64 *table, const struct ave_temp_inputs *in,
	      u64 va, u64 expected, bool execute)
{
	struct ave_temp_walk w = { .va = va, .expected = expected };
	bool table_ro = false, table_pxn = false;
	u64 address = in->ttbr + ((va >> 25) & 1) * 8;

	w.fault = AVE_TEMP_GEOMETRY;
	if (!ave_temp_geometry(in))
		return w;
	w.fault = AVE_TEMP_VA_RANGE;
	if (va >= BIT_ULL(26))
		return w;
	for (w.level = 2; w.level <= 3; w.level++) {
		u64 desc, base, mask;
		u32 shift = w.level == 2 ? 25 : 14;

		w.fault = AVE_TEMP_TABLE_RANGE;
		if (!ave_temp_read(table, in, address, &desc, &w.offset))
			return w;
		w.descriptor = desc;
		w.fault = AVE_TEMP_INVALID;
		if (!(desc & 1))
			return w;
		w.fault = AVE_TEMP_TYPE;
		if (w.level == 3 && !(desc & 2))
			return w;
		w.fault = AVE_TEMP_ADDRESS;
		if (desc & (GENMASK_ULL(49, 48) | GENMASK_ULL(13, 12)))
			return w;
		base = desc & AVE_TEMP_ADDR;
		if (w.level == 2 && (desc & 2)) {
			table_ro |= !!(desc & BIT_ULL(62));
			table_pxn |= !!(desc & BIT_ULL(59));
			address = base + ((va >> 14) & 0x7ff) * 8;
			/* Validate the entire child page, before following it. */
			w.fault = AVE_TEMP_TABLE_RANGE;
			if (base < in->data_iova + AVE_TEMP_LO ||
			    base > in->data_iova + AVE_TEMP_HI - AVE_TEMP_PAGE)
				return w;
			w.fault = AVE_TEMP_OUTPUT;
			if (base != in->data_link + AVE_TEMP_LO +
			    ((va >> 25) & 1) * AVE_TEMP_PAGE)
				return w;
			w.fault = AVE_TEMP_PERMISSION;
			if (execute ? table_pxn : table_ro)
				return w;
			continue;
		}
		mask = BIT_ULL(shift) - 1;
		if (base & mask)
			return w;
		w.output = base | (va & mask);
		w.translated = true;
		w.writable = !table_ro && !(desc & BIT_ULL(7));
		w.executable = !table_pxn && !(desc & BIT_ULL(53));
		w.fault = AVE_TEMP_ACCESS_FLAG;
		if (!(desc & BIT_ULL(10)))
			return w;
		w.fault = AVE_TEMP_OUTPUT;
		if (w.output != expected)
			return w;
		w.fault = AVE_TEMP_PERMISSION;
		if (execute ? !w.executable : !w.writable)
			return w;
		w.fault = AVE_TEMP_OK;
		return w;
	}
	return w;
}

/*
 * Deterministic linear mapping model from the qualified H14G address contract:
 * two root entries to the two temporary pages, executable RO TEXT, RW DATA,
 * zero holes. This is a model of the required reset mappings, not a copy of
 * observed descriptors. Unsupported loader overrides cannot yield a match.
 */
static inline bool ave_temp_model_inputs(const struct ave_temp_inputs *in)
{
	return ave_temp_geometry(in) && in->data_link == in->data_iova &&
	       !(in->text_iova & (AVE_TEMP_PAGE - 1)) &&
	       in->text_size && in->text_size <= AVE_TEMP_DATA_VA &&
	       !(in->text_size & (AVE_TEMP_PAGE - 1)) &&
	       in->data_size >= AVE_TEMP_HI &&
	       in->data_size <= BIT_ULL(25) - AVE_TEMP_DATA_VA &&
	       !(in->data_size & (AVE_TEMP_PAGE - 1)) &&
	       in->text_iova <= BIT_ULL(48) - in->text_size &&
	       in->data_iova <= BIT_ULL(48) - in->data_size &&
	       in->page_shift == 14 && in->page_size == AVE_TEMP_PAGE &&
	       in->index_bits == 11 && in->final_va_bits == 31 &&
	       in->final_mode == 3 && !in->pt_phys && !in->pt_virt && !in->pt_size;
}

static inline u64 ave_temp_model_word(const struct ave_temp_inputs *in, u32 off)
{
	u64 va = (u64)(off - AVE_TEMP_LO) / 8 * AVE_TEMP_PAGE;

	if (off == AVE_TEMP_ROOT)
		return (in->data_link + AVE_TEMP_LO) | 0x403;
	if (off == AVE_TEMP_ROOT + 8)
		return (in->data_link + AVE_TEMP_LO + AVE_TEMP_PAGE) | 0x403;
	if (off >= AVE_TEMP_LO + AVE_TEMP_PAGE)
		return 0;
	if (va < in->text_size)
		return (in->text_iova + va) | 0x483;
	if (va >= AVE_TEMP_DATA_VA && va - AVE_TEMP_DATA_VA < in->data_size)
		return (in->data_link + va - AVE_TEMP_DATA_VA) | 0x403;
	return 0;
}

static inline void ave_temp_compare(const __le64 *table,
				    const struct ave_temp_inputs *in,
				    struct ave_temp_result *result)
{
	result->model_valid = ave_temp_model_inputs(in);
	if (!result->model_valid)
		return;
	for (u32 off = AVE_TEMP_LO; off < AVE_TEMP_HI; off += 8) {
		u64 observed = le64_to_cpu(table[(off - AVE_TEMP_LO) / 8]);
		u64 expected = ave_temp_model_word(in, off);

		result->compared++;
		if (observed == expected)
			continue;
		if (!result->mismatches) {
			result->first_offset = off;
			result->first_observed = observed;
			result->first_expected = expected;
		}
		result->mismatches++;
	}
}

#endif /* APPLE_AVE_TEMP_H */
