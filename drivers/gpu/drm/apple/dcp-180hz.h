/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_180HZ_H
#define APPLE_DCP_180HZ_H
#include <linux/types.h>
struct apple_dcp;
struct apple_epic_service;
struct dcp_display_mode;
struct dcp_180hz_token {
	/* Successful physical configuration is independent of AV discovery. */
	u64 generation, sequence, connection_sequence;
	const void *phy, *route, *service;
	bool valid;
};
struct dcp_180hz_fact {
	struct dcp_180hz_token token;
	u32 value;
	bool valid;
};
struct dcp_180hz_proof {
	u64 sequence, connection_sequence, catalog_sequence, retry_generation;
	unsigned int retries;
	struct dcp_180hz_fact rate, lanes;
};
struct dcp_180hz_token dcp_180hz_config_begin(struct apple_epic_service *service, bool lanes);
void dcp_180hz_config_complete(struct apple_epic_service *service, bool lanes,
			      struct dcp_180hz_token token, u32 value, bool success);
void dcp_180hz_invalidate(struct apple_dcp *dcp);
void dcp_180hz_notify(struct apple_dcp *dcp);
void dcp_180hz_catalog_changed(struct apple_dcp *dcp);
bool dcp_180hz_mode_current(struct apple_dcp *dcp, const struct dcp_display_mode *mode);
bool dcp_external_modes_ready(struct apple_dcp *dcp);
bool dcp_180hz_pending(struct apple_dcp *dcp, const struct dcp_display_mode *mode);
void dcp_180hz_retry(struct apple_dcp *dcp);
int dcp_180hz_init(struct apple_dcp *dcp);
#endif
