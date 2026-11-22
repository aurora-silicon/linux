// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple RTKit IPC library
 * Copyright (C) The Asahi Linux Contributors
 */
#include <linux/unaligned.h>

#include "rtkit-internal.h"

#define FOURCC(a, b, c, d) \
	(((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | ((u32)(d)))

#define APPLE_RTKIT_CRASHLOG_STR FOURCC('C', 's', 't', 'r')
#define APPLE_RTKIT_CRASHLOG_VERSION FOURCC('C', 'v', 'e', 'r')
#define APPLE_RTKIT_CRASHLOG_MBOX FOURCC('C', 'm', 'b', 'x')
#define APPLE_RTKIT_CRASHLOG_TIME FOURCC('C', 't', 'i', 'm')
#define APPLE_RTKIT_CRASHLOG_REGS FOURCC('C', 'r', 'g', '8')
#define APPLE_RTKIT_CRASHLOG_REGS_CL4 FOURCC('C', 'r', 'l', '4')

/*
 * Newer firmware writes a container instead of a single crashlog: a
 * 'CLGH' header and a table of records, each with an ID and an offset from
 * the container. The record whose ID is the container's current ID holds
 * the log of this crash, and may itself be a container (on T6050's DCP:
 * per-coprocessor, then per-task). The log at the end is an ordinary
 * 'CLHE' crashlog with the same sections.
 */
struct apple_rtkit_crashlog_container {
	u32 fourcc;
	u32 version;
	u32 size;
	u32 current_id;
	u32 count;
} __packed;
static_assert(sizeof(struct apple_rtkit_crashlog_container) == 0x14);

struct apple_rtkit_crashlog_record {
	u64 id;
	u64 _unk0;
	u32 offset;
	u32 _unk1;
} __packed;
static_assert(sizeof(struct apple_rtkit_crashlog_record) == 0x18);

#define APPLE_RTKIT_CRASHLOG_CONTAINER_VERSION 2
#define APPLE_RTKIT_CRASHLOG_CONTAINER_MAX_DEPTH 8
/* Set in a log's flags while the co-processor is still writing it. */
#define APPLE_RTKIT_CRASHLOG_INCOMPLETE BIT(8)

/* Register frame of the 'Crl4' section, offsets from the section start. */
#define APPLE_RTKIT_CRASHLOG_CL4_SECTION_SIZE 0x378
#define APPLE_RTKIT_CRASHLOG_CL4_FLAGS 0x10
#define APPLE_RTKIT_CRASHLOG_CL4_SIZE 0x14
#define APPLE_RTKIT_CRASHLOG_CL4_NOMINAL_SIZE 0xd8
#define APPLE_RTKIT_CRASHLOG_CL4_REGS 0x18
#define APPLE_RTKIT_CRASHLOG_CL4_LR 0x108
#define APPLE_RTKIT_CRASHLOG_CL4_PC 0x110
#define APPLE_RTKIT_CRASHLOG_CL4_SP 0x118
#define APPLE_RTKIT_CRASHLOG_CL4_PSR 0x120
#define APPLE_RTKIT_CRASHLOG_CL4_ESR 0x358
#define APPLE_RTKIT_CRASHLOG_CL4_FAR 0x360
#define APPLE_RTKIT_CRASHLOG_CL4_UNDECODABLE BIT(16)

/* For COMPILE_TEST on non-ARM64 architectures */
#ifndef PSR_MODE_EL0t
#define PSR_MODE_EL0t	0x00000000
#define PSR_MODE_EL1t	0x00000004
#define PSR_MODE_EL1h	0x00000005
#define PSR_MODE_EL2t	0x00000008
#define PSR_MODE_EL2h	0x00000009
#define PSR_MODE_MASK	0x0000000f
#endif

struct apple_rtkit_crashlog_mbox_entry {
	u64 msg0;
	u64 msg1;
	u32 timestamp;
	u8 _unk[4];
};
static_assert(sizeof(struct apple_rtkit_crashlog_mbox_entry) == 0x18);

struct apple_rtkit_crashlog_regs {
	u32 unk_0;
	u32 unk_4;
	u64 regs[31];
	u64 sp;
	u64 pc;
	u64 psr;
	u64 cpacr;
	u64 fpsr;
	u64 fpcr;
	u64 unk[64];
	u64 far;
	u64 unk_X;
	u64 esr;
	u64 unk_Z;
} __packed;
static_assert(sizeof(struct apple_rtkit_crashlog_regs) == 0x350);

/* How much of a section without a decoder is printed, and how much per line. */
#define APPLE_RTKIT_CRASHLOG_UNKNOWN_DUMP 0x100
#define APPLE_RTKIT_CRASHLOG_UNKNOWN_LINE 32

/*
 * A section this file has no decoder for: its name and the start of its
 * payload. An exception log of the PMP has no 'Cstr' to say what went wrong;
 * its call stack ('Ccst') and task state are in sections such as these.
 */
static void apple_rtkit_crashlog_dump_unknown(struct apple_rtkit *rtk, u32 fourcc,
					      const u8 *bfr, size_t size)
{
	static const char hex[] = "0123456789abcdef";
	char line[APPLE_RTKIT_CRASHLOG_UNKNOWN_LINE * 3 + 1], name[5];
	size_t len = size, i, j, n;

	if (len > APPLE_RTKIT_CRASHLOG_UNKNOWN_DUMP)
		len = APPLE_RTKIT_CRASHLOG_UNKNOWN_DUMP;
	for (i = 0; i < 4; i++) {
		u8 c = fourcc >> (24 - 8 * i);

		name[i] = c >= 0x20 && c < 0x7f ? c : '.';
	}
	name[4] = 0;
	dev_warn(rtk->dev, "RTKit:  + '%s', %zu bytes", name, size);
	for (i = 0; i < len; i += APPLE_RTKIT_CRASHLOG_UNKNOWN_LINE) {
		n = len - i;
		if (n > APPLE_RTKIT_CRASHLOG_UNKNOWN_LINE)
			n = APPLE_RTKIT_CRASHLOG_UNKNOWN_LINE;
		for (j = 0; j < n; j++) {
			line[3 * j] = hex[bfr[i + j] >> 4];
			line[3 * j + 1] = hex[bfr[i + j] & 0xf];
			line[3 * j + 2] = ' ';
		}
		line[n ? 3 * n - 1 : 0] = 0;
		dev_warn(rtk->dev, "RTKit:  + %03zx: %s", i, line);
	}
}

static void apple_rtkit_crashlog_dump_str(struct apple_rtkit *rtk, u8 *bfr,
					  size_t size)
{
	u32 idx;
	u8 *ptr, *end;

	memcpy(&idx, bfr, 4);

	ptr = bfr + 4;
	end = bfr + size;
	while (ptr < end) {
		u8 *newline = memchr(ptr, '\n', end - ptr);

		if (newline) {
			u8 tmp = *newline;
			*newline = '\0';
			dev_warn(rtk->dev, "RTKit: Message (id=%x): %s\n", idx,
				 ptr);
			*newline = tmp;
			ptr = newline + 1;
		} else {
			dev_warn(rtk->dev, "RTKit: Message (id=%x): %.*s", idx,
				 (int)(end - ptr), ptr);
			break;
		}
	}
}

static void apple_rtkit_crashlog_dump_version(struct apple_rtkit *rtk, u8 *bfr,
					      size_t size)
{
	/* The string need not be terminated inside the section. */
	dev_warn(rtk->dev, "RTKit: Version: %.*s", (int)(size - 16), bfr + 16);
}

static void apple_rtkit_crashlog_dump_time(struct apple_rtkit *rtk, u8 *bfr,
					   size_t size)
{
	u64 crash_time;

	memcpy(&crash_time, bfr, 8);
	dev_warn(rtk->dev, "RTKit: Crash time: %lld", crash_time);
}

static void apple_rtkit_crashlog_dump_mailbox(struct apple_rtkit *rtk, u8 *bfr,
					      size_t size)
{
	u32 type, index, i;
	size_t n_messages;
	struct apple_rtkit_crashlog_mbox_entry entry;

	if (size < 28) {
		dev_warn(rtk->dev, "RTKit: Mailbox section too small: 0x%zx", size);
		return;
	}
	memcpy(&type, bfr + 16, 4);
	memcpy(&index, bfr + 24, 4);
	n_messages = (size - 28) / sizeof(entry);

	dev_warn(rtk->dev, "RTKit: Mailbox history (type = %d, index = %d)",
		 type, index);
	for (i = 0; i < n_messages; ++i) {
		memcpy(&entry, bfr + 28 + i * sizeof(entry), sizeof(entry));
		dev_warn(rtk->dev, "RTKit:  #%03d@%08x: %016llx %016llx", i,
			 entry.timestamp, entry.msg0, entry.msg1);
	}
}

static void apple_rtkit_crashlog_dump_regs(struct apple_rtkit *rtk, u8 *bfr,
					   size_t size)
{
	struct apple_rtkit_crashlog_regs *regs;
	const char *el;
	int i;

	if (size < sizeof(*regs)) {
		dev_warn(rtk->dev, "RTKit: Regs section too small: 0x%zx", size);
		return;
	}

	regs = (struct apple_rtkit_crashlog_regs *)bfr;

	switch (regs->psr & PSR_MODE_MASK) {
	case PSR_MODE_EL0t:
		el = "EL0t";
		break;
	case PSR_MODE_EL1t:
		el = "EL1t";
		break;
	case PSR_MODE_EL1h:
		el = "EL1h";
		break;
	case PSR_MODE_EL2t:
		el = "EL2t";
		break;
	case PSR_MODE_EL2h:
		el = "EL2h";
		break;
	default:
		el = "unknown";
		break;
	}

	dev_warn(rtk->dev, "RTKit: Exception dump:");
	dev_warn(rtk->dev, "  == Exception taken from %s ==", el);
	dev_warn(rtk->dev, "  PSR    = 0x%llx", regs->psr);
	dev_warn(rtk->dev, "  PC     = 0x%llx\n", regs->pc);
	dev_warn(rtk->dev, "  ESR    = 0x%llx\n", regs->esr);
	dev_warn(rtk->dev, "  FAR    = 0x%llx\n", regs->far);
	dev_warn(rtk->dev, "  SP     = 0x%llx\n", regs->sp);
	dev_warn(rtk->dev, "\n");

	for (i = 0; i < 31; i += 4) {
		if (i < 28)
			dev_warn(rtk->dev,
					 "  x%02d-x%02d = %016llx %016llx %016llx %016llx\n",
					 i, i + 3,
					 regs->regs[i], regs->regs[i + 1],
					 regs->regs[i + 2], regs->regs[i + 3]);
		else
			dev_warn(rtk->dev,
					 "  x%02d-x%02d = %016llx %016llx %016llx\n", i, i + 3,
					 regs->regs[i], regs->regs[i + 1], regs->regs[i + 2]);
	}

	dev_warn(rtk->dev, "\n");
}

static void apple_rtkit_crashlog_dump_regs_cl4(struct apple_rtkit *rtk,
					       u8 *section, size_t size)
{
	u32 flags, regs_size;
	int i;

	if (size < APPLE_RTKIT_CRASHLOG_CL4_SECTION_SIZE) {
		dev_warn(rtk->dev, "RTKit: Crl4 section too small: 0x%zx", size);
		return;
	}
	flags = get_unaligned_le32(section + APPLE_RTKIT_CRASHLOG_CL4_FLAGS);
	regs_size = get_unaligned_le32(section + APPLE_RTKIT_CRASHLOG_CL4_SIZE);
	if (regs_size != APPLE_RTKIT_CRASHLOG_CL4_NOMINAL_SIZE ||
	    (flags & APPLE_RTKIT_CRASHLOG_CL4_UNDECODABLE)) {
		dev_warn(rtk->dev, "RTKit: Crl4 frame not decodable (flags %#x, size %#x)",
			 flags, regs_size);
		return;
	}

	dev_warn(rtk->dev, "RTKit: Exception dump (Crl4):");
	dev_warn(rtk->dev, "  PSR    = 0x%llx",
		 get_unaligned_le64(section + APPLE_RTKIT_CRASHLOG_CL4_PSR));
	dev_warn(rtk->dev, "  PC     = 0x%llx",
		 get_unaligned_le64(section + APPLE_RTKIT_CRASHLOG_CL4_PC));
	dev_warn(rtk->dev, "  LR     = 0x%llx",
		 get_unaligned_le64(section + APPLE_RTKIT_CRASHLOG_CL4_LR));
	dev_warn(rtk->dev, "  ESR    = 0x%llx",
		 get_unaligned_le64(section + APPLE_RTKIT_CRASHLOG_CL4_ESR));
	dev_warn(rtk->dev, "  FAR    = 0x%llx",
		 get_unaligned_le64(section + APPLE_RTKIT_CRASHLOG_CL4_FAR));
	dev_warn(rtk->dev, "  SP     = 0x%llx",
		 get_unaligned_le64(section + APPLE_RTKIT_CRASHLOG_CL4_SP));
	for (i = 0; i < 28; i += 4) {
		u8 *r = section + APPLE_RTKIT_CRASHLOG_CL4_REGS + i * 8;

		dev_warn(rtk->dev, "  x%02d-x%02d = %016llx %016llx %016llx %016llx",
			 i, i + 3, get_unaligned_le64(r), get_unaligned_le64(r + 8),
			 get_unaligned_le64(r + 16), get_unaligned_le64(r + 24));
	}
}

/**
 * apple_rtkit_crashlog_locate() - find this crash's log in a crashlog buffer
 * @bfr: a copy of the buffer
 * @size: its size
 * @log_offset: set to the offset of the 'CLHE' log
 * @log_id: set to the ID of the record that held it (0 for a bare log)
 * @depth: set to the number of containers followed
 *
 * A bare 'CLHE' log at offset 0 is returned as it is. In a 'CLGH' container,
 * the record with the current ID is followed, through nested containers,
 * to its log. Offsets only move forward, so a corrupt table cannot loop.
 *
 * Return: 0 if a complete log was found, -EAGAIN if the selected log is
 * still being written, -EINVAL otherwise.
 */
int apple_rtkit_crashlog_locate(const u8 *bfr, size_t size, size_t *log_offset,
				u32 *log_id, unsigned int *depth)
{
	struct apple_rtkit_crashlog_container container;
	struct apple_rtkit_crashlog_header header;
	struct apple_rtkit_crashlog_record record;
	size_t offset = 0, avail, next;
	unsigned int level;
	u32 id = 0, i;

	for (level = 0; level <= APPLE_RTKIT_CRASHLOG_CONTAINER_MAX_DEPTH; level++) {
		avail = size - offset;
		if (avail < sizeof(header))
			return -EINVAL;
		memcpy(&header, bfr + offset, sizeof(header));

		if (header.fourcc == APPLE_RTKIT_CRASHLOG_HEADER_FOURCC) {
			if (header.size < sizeof(header) || header.size > avail)
				return -EINVAL;
			if (level && (header.flags & APPLE_RTKIT_CRASHLOG_INCOMPLETE))
				return -EAGAIN;
			*log_offset = offset;
			*log_id = id;
			*depth = level;
			return 0;
		}
		if (header.fourcc != APPLE_RTKIT_CRASHLOG_CONTAINER_FOURCC)
			return -EINVAL;

		memcpy(&container, bfr + offset, sizeof(container));
		if (((container.version >> 8) & 0xff) !=
		    APPLE_RTKIT_CRASHLOG_CONTAINER_VERSION ||
		    container.size > avail || !container.count ||
		    container.count > (avail - sizeof(container)) / sizeof(record))
			return -EINVAL;

		next = 0;
		for (i = 0; i < container.count; i++) {
			memcpy(&record, bfr + offset + sizeof(container) +
			       i * sizeof(record), sizeof(record));
			if (record.id == container.current_id) {
				next = record.offset;
				break;
			}
		}
		if (next < sizeof(container) || next >= avail)
			return -EINVAL;

		id = container.current_id;
		offset += next;
	}

	return -EINVAL;
}

void apple_rtkit_crashlog_dump(struct apple_rtkit *rtk, u8 *bfr, size_t size)
{
	size_t offset;
	u32 section_fourcc, section_size;
	struct apple_rtkit_crashlog_header header;

	if (size < sizeof(header)) {
		dev_warn(rtk->dev, "RTKit: Crashlog buffer too small: 0x%zx",
			 size);
		return;
	}
	memcpy(&header, bfr, sizeof(header));
	if (header.fourcc == APPLE_RTKIT_CRASHLOG_CONTAINER_FOURCC) {
		unsigned int depth;
		u32 id;
		int ret;

		ret = apple_rtkit_crashlog_locate(bfr, size, &offset, &id, &depth);
		if (ret) {
			dev_warn(rtk->dev,
				 "RTKit: Crashlog container holds no complete log (%d)",
				 ret);
			return;
		}
		dev_warn(rtk->dev,
			 "RTKit: Crashlog container: log '%c%c%c%c' at 0x%zx (depth %u)",
			 (id >> 24) & 0xff, (id >> 16) & 0xff, (id >> 8) & 0xff,
			 id & 0xff, offset, depth);
		bfr += offset;
		size -= offset;
		memcpy(&header, bfr, sizeof(header));
	}

	if (header.fourcc != APPLE_RTKIT_CRASHLOG_HEADER_FOURCC) {
		dev_warn(rtk->dev, "RTKit: Expected crashlog header but got %x",
			 header.fourcc);
		return;
	}

	if (header.size > size) {
		dev_warn(rtk->dev, "RTKit: Crashlog size (%x) is too large",
			 header.size);
		return;
	}

	size = header.size;
	offset = sizeof(header);

	while (offset < size) {
		if (size - offset < 16) {
			dev_warn(rtk->dev,
				 "RTKit: Crashlog section header at 0x%zx is truncated",
				 offset);
			return;
		}
		memcpy(&section_fourcc, bfr + offset, 4);
		memcpy(&section_size, bfr + offset + 12, 4);

		if (section_fourcc == APPLE_RTKIT_CRASHLOG_HEADER_FOURCC) {
			dev_dbg(rtk->dev, "RTKit: End of crashlog reached");
			return;
		}
		/* A zero size would never advance. */
		if (section_size < 16 || section_size > size - offset) {
			dev_warn(rtk->dev,
				 "RTKit: Crashlog section %x at 0x%zx has a bad size 0x%x",
				 section_fourcc, offset, section_size);
			return;
		}
		/*
		 * The decoders read up to section_size bytes from 16 bytes into
		 * the section, so a section is only decoded with at least the
		 * footer's 16-byte header after it.
		 */
		if (section_size > size - offset - 16)
			break;

		switch (section_fourcc) {
		case APPLE_RTKIT_CRASHLOG_STR:
			apple_rtkit_crashlog_dump_str(rtk, bfr + offset + 16,
						      section_size);
			break;
		case APPLE_RTKIT_CRASHLOG_VERSION:
			apple_rtkit_crashlog_dump_version(
				rtk, bfr + offset + 16, section_size);
			break;
		case APPLE_RTKIT_CRASHLOG_MBOX:
			apple_rtkit_crashlog_dump_mailbox(
				rtk, bfr + offset + 16, section_size);
			break;
		case APPLE_RTKIT_CRASHLOG_TIME:
			apple_rtkit_crashlog_dump_time(rtk, bfr + offset + 16,
						       section_size);
			break;
		case APPLE_RTKIT_CRASHLOG_REGS:
			apple_rtkit_crashlog_dump_regs(rtk, bfr + offset + 16,
						       section_size);
			break;
		case APPLE_RTKIT_CRASHLOG_REGS_CL4:
			apple_rtkit_crashlog_dump_regs_cl4(rtk, bfr + offset,
							   section_size);
			break;
		default:
			dev_warn(rtk->dev,
				 "RTKit: Unknown crashlog section: %x",
				 section_fourcc);
			apple_rtkit_crashlog_dump_unknown(rtk, section_fourcc,
							  bfr + offset + 16,
							  section_size - 16);
		}

		offset += section_size;
	}

	dev_warn(rtk->dev,
		 "RTKit: End of crashlog reached but no footer present");
}
