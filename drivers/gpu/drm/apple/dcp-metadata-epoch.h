/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_METADATA_EPOCH_H
#define APPLE_DCP_METADATA_EPOCH_H

#include <linux/types.h>

/* Caller serializes these fields with the metadata snapshot lock. */
struct dcp_metadata_epoch {
	u64 generation;
	bool connected;
	bool associated_once;
	bool invalidated;
};

struct dcp_metadata_ticket {
	u64 generation;
	bool associated;
};

static inline void dcp_metadata_epoch_connection(struct dcp_metadata_epoch *epoch,
						 u64 generation, bool connected)
{
	if (epoch->associated_once &&
	    (!connected || generation != epoch->generation))
		epoch->invalidated = true;
	if (connected)
		epoch->associated_once = true;
	epoch->generation = generation;
	epoch->connected = connected;
}

static inline struct dcp_metadata_ticket
dcp_metadata_begin(const struct dcp_metadata_epoch *epoch)
{
	return (struct dcp_metadata_ticket) {
		.generation = epoch->generation,
		.associated = epoch->connected && !epoch->invalidated,
	};
}

static inline bool dcp_metadata_matches(const struct dcp_metadata_epoch *epoch,
					struct dcp_metadata_ticket ticket)
{
	return ticket.associated && epoch->connected && !epoch->invalidated &&
	       ticket.generation == epoch->generation;
}

#endif
