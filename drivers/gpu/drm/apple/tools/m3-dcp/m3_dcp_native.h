/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCP_NATIVE_H
#define M3_DCP_NATIVE_H

#define M3_DCP_TAG(c, n) (((u32)(c) << 24) | ((u32)('0' + (n) / 100) << 16) | \
			 ((u32)('0' + (n) / 10 % 10) << 8) | (u32)('0' + (n) % 10))
struct m3_dcp_native;
struct m3_dcp_bridge;
struct device;
struct seq_file;
void m3_dcp_native_completion_show(struct m3_dcp_native *dcp, struct seq_file *seq);
void m3_dcp_native_analytics_show(struct m3_dcp_native *dcp, struct seq_file *seq);
struct m3_dcp_native *m3_dcp_native_start(struct device *dev, struct m3_dcp_bridge *bridge,
				       bool defer_open);
int m3_dcp_native_open(struct m3_dcp_native *dcp);
int m3_dcp_native_swap(struct m3_dcp_native *dcp, const void *surface, u64 dva,
		       u32 width, u32 height, int brightness_nits);
void m3_dcp_native_brightness_invalidate(struct m3_dcp_native *dcp);
int m3_dcp_native_background(struct m3_dcp_native *dcp, u32 color);
int m3_dcp_native_panel(struct m3_dcp_native *dcp, unsigned int action);
/* Caller owns the returned snapshot and must kfree it. */
void *m3_dcp_native_property(struct m3_dcp_native *dcp, const char *key, u32 *size);
#endif
