/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCPEXT_NATIVE_H
#define M3_DCPEXT_NATIVE_H

#define M3_DCP_TAG(c, n) (((u32)(c) << 24) | ((u32)('0' + (n) / 100) << 16) | \
			 ((u32)('0' + (n) / 10 % 10) << 8) | (u32)('0' + (n) % 10))
struct m3_dcpext_native;
struct m3_dcpext_rpc;
struct device;
struct seq_file;
void m3_dcpext_native_completion_show(struct m3_dcpext_native *dcp, struct seq_file *seq);
void m3_dcpext_native_analytics_show(struct m3_dcpext_native *dcp, struct seq_file *seq);
struct m3_dcpext_native *m3_dcpext_native_start(struct device *dev, struct m3_dcpext_rpc *bridge,
				       bool defer_open);
int m3_dcpext_native_open(struct m3_dcpext_native *dcp);
int m3_dcpext_native_swap(struct m3_dcpext_native *dcp, const void *surface, u64 dva,
		       u32 width, u32 height);
int m3_dcpext_native_background(struct m3_dcpext_native *dcp, u32 color);
int m3_dcpext_native_panel(struct m3_dcpext_native *dcp, unsigned int action);
/* Caller owns the returned snapshot and must kfree it. */
void *m3_dcpext_native_property(struct m3_dcpext_native *dcp, const char *key, u32 *size);
int m3_dcpext_native_pump(struct m3_dcpext_native *, unsigned long timeout);
int m3_dcpext_native_mode(struct m3_dcpext_native *, u32 color, u32 timing);
int m3_dcpext_native_power(struct m3_dcpext_native *, bool on);
u64 m3_dcpext_native_generation(struct m3_dcpext_native *);
void m3_dcpext_native_invalidate_sink(struct m3_dcpext_native *);
#endif
