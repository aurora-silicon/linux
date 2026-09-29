/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_DCP_BRIDGE_H
#define M3_DCP_BRIDGE_H

struct device;
struct work_struct;
struct apple_rtkit;
struct m3_dcp_bridge;

struct m3_dcp_buffer {
	u64 size, physical, dva;
	u32 id, flags;
};

typedef int (*m3_dcp_callback_fn)(struct m3_dcp_bridge *bridge, void *cookie,
				u32 tag, const void *input, u32 input_size,
				void *output, u32 output_size);

struct m3_dcp_bridge *m3_dcp_bridge_create(struct device *dev,
					struct apple_rtkit *rtkit, void *rpc, bool kernel_client);
void m3_dcp_bridge_receive(struct m3_dcp_bridge *bridge, u64 message);
void m3_dcp_bridge_activate(struct m3_dcp_bridge *bridge);
void m3_dcp_bridge_crashed(struct m3_dcp_bridge *bridge);
int m3_dcp_bridge_call(struct m3_dcp_bridge *bridge, u32 tag,
		      const void *input, u32 input_size, void *output, u32 output_size,
		      u32 completion_id, m3_dcp_callback_fn callback, void *cookie);
int m3_dcp_bridge_pump(struct m3_dcp_bridge *bridge, unsigned long timeout,
		      m3_dcp_callback_fn callback, void *cookie);
int m3_dcp_bridge_alloc(struct m3_dcp_bridge *bridge, struct m3_dcp_buffer *info);
int m3_dcp_bridge_map(struct m3_dcp_bridge *bridge, struct m3_dcp_buffer *info);
int m3_dcp_bridge_retire(struct m3_dcp_bridge *bridge, u32 id);

void m3_dcp_bridge_set_work(struct m3_dcp_bridge *bridge, struct work_struct *work);
#endif
