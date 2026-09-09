/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AURORA_CENTAURI_H
#define AURORA_CENTAURI_H
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#define CEN_ALPHA_SCRATCH_OFFSET SZ_8M
#define CEN_ALPHA_SCRATCH_SIZE (5 * SZ_1M)

struct centauri_ipc;
struct centauri_wifi;
struct cen_alpha_resources {
	void *context, *messages, *window;
	dma_addr_t context_dma, messages_dma, window_dma;
	void __iomem *bar;
	u32 index[4], mtr, mcr;
};
struct centauri {
	struct pci_dev *pdev;
	void __iomem *bar;
	resource_size_t size;
	u32 stage_offset;
	struct dentry *debug;
	struct mutex boot_lock;
	void *firmware, *descriptor, *msi_page;
	dma_addr_t firmware_dma, descriptor_dma, msi_dma;
	size_t firmware_size;
	u32 image_response;
	u16 msi_original;
	int msi_cap;
	bool boot_attempted;
	bool link_ready;
	struct centauri_ipc *ipc;
	struct centauri_wifi *wifi;
};

int centauri_j813_port_cycle(struct pci_dev *pdev);
int centauri_start_ipc(struct centauri *c);
void centauri_free_ipc(struct centauri *c);
int centauri_alpha_resources(struct centauri *c, struct cen_alpha_resources *resources);
int centauri_alpha_log_buffer(struct centauri *c, void **cpu, dma_addr_t *dma, size_t *size);
int centauri_wifi_register(struct centauri *c);
void centauri_wifi_unregister(struct centauri *c);
#endif
