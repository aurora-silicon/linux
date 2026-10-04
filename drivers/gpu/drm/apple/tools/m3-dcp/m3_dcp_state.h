/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include <linux/completion.h>
#include <linux/dma-mapping.h>
/* Private layout shared by the explicit startup and one-shot query probes. */
struct m3_dcp_rtkit {
	struct device *dev;
	struct apple_rtkit *rtkit;
	struct completion ready;
	void *rpc;
	dma_addr_t rpc_dva;
	int result;
	bool awaiting_rpc;
	struct m3_dcp_bridge *bridge;
	struct m3_dcp_native *native;
};
