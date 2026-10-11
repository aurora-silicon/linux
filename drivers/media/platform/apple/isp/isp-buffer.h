/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ISP_BUFFER_H__
#define __ISP_BUFFER_H__

#include <linux/build_bug.h>
#include <linux/types.h>

#define ISP_BUFFER_PLANES 4

/* Existing legacy/H17 buffer record. The reserved high tag word stays zero. */
struct isp_buflist_buffer {
	u64 iovas[ISP_BUFFER_PLANES];
	u32 flags[ISP_BUFFER_PLANES];
	u32 num_planes;
	u32 pool_type;
	u32 tag;
	u32 pad;
} __packed;
static_assert(sizeof(struct isp_buflist_buffer) == 0x40);

enum isp_buffer_owner {
	ISP_BUFFER_HOST,
	ISP_BUFFER_SUBMITTED,
	ISP_BUFFER_REPORT_PENDING,
};

struct isp_buffer_lease {
	struct isp_buflist_buffer submitted;
	enum isp_buffer_owner owner;
	bool tag64;
};

struct isp_buffer_match {
	struct isp_buffer_lease *lease;
	bool ambiguous;
};

static inline bool isp_buffer_matches(const struct isp_buffer_lease *lease,
				      const struct isp_buflist_buffer *report)
{
	const struct isp_buflist_buffer *submitted = &lease->submitted;

	if (lease->owner != ISP_BUFFER_SUBMITTED ||
	    !report->num_planes || report->num_planes > ISP_BUFFER_PLANES ||
	    report->num_planes != submitted->num_planes ||
	    report->pool_type != submitted->pool_type || report->tag != submitted->tag)
		return false;
	if (lease->tag64 && report->pad != submitted->pad)
		return false;

	/* Existing firmware exports the low32 address, including for high IOVAs. */
	for (unsigned int i = 0; i < report->num_planes; i++)
		if ((u32)report->iovas[i] != (u32)submitted->iovas[i])
			return false;

	return true;
}

static inline void isp_buffer_match_candidate(struct isp_buffer_match *match,
					      struct isp_buffer_lease *lease,
					      const struct isp_buflist_buffer *report)
{
	if (!isp_buffer_matches(lease, report))
		return;
	if (match->lease)
		match->ambiguous = true;
	else
		match->lease = lease;
}

static inline struct isp_buffer_lease *
isp_buffer_unique_match(const struct isp_buffer_match *match)
{
	return match->ambiguous ? NULL : match->lease;
}

static inline bool isp_buffer_submit_tag(struct isp_buffer_lease *lease,
					 const struct isp_buflist_buffer *descriptor,
					 bool tag64)
{
	if (lease->owner != ISP_BUFFER_HOST || !descriptor->num_planes ||
	    descriptor->num_planes > ISP_BUFFER_PLANES)
		return false;
	lease->submitted = *descriptor;
	lease->tag64 = tag64;
	lease->owner = ISP_BUFFER_SUBMITTED;
	return true;
}

static inline bool isp_buffer_submit(struct isp_buffer_lease *lease,
				     const struct isp_buflist_buffer *descriptor)
{
	return isp_buffer_submit_tag(lease, descriptor, false);
}

static inline bool isp_buffer_return(struct isp_buffer_lease *lease,
				     const struct isp_buflist_buffer *report)
{
	/* Batch callers first validate that this is the unique live match. */
	if (!isp_buffer_matches(lease, report))
		return false;
	lease->owner = ISP_BUFFER_REPORT_PENDING;
	return true;
}

static inline bool isp_buffer_acknowledge(struct isp_buffer_lease *lease)
{
	if (lease->owner != ISP_BUFFER_REPORT_PENDING)
		return false;
	lease->owner = ISP_BUFFER_HOST;
	return true;
}

static inline bool isp_buffer_reclaimable(const struct isp_buffer_lease *lease,
					  bool firmware_quiescent)
{
	return lease->owner == ISP_BUFFER_HOST || firmware_quiescent;
}

#endif /* __ISP_BUFFER_H__ */
