/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AURORA_CENTAURI_T6050_H
#define AURORA_CENTAURI_T6050_H
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>

#define CEN_ALPHA_WINDOW_SIZE SZ_32M
#define CEN_ALPHA_SCRATCH_OFFSET SZ_8M
#define CEN_ALPHA_SCRATCH_SIZE (5 * SZ_1M)

struct centauri_t6050_ipc;
struct centauri_t6050_wifi;
struct centauri_t6050_irqs {
	struct pci_dev *pdev;
	void *data;
	unsigned int count;
};

struct cen_alpha_resources {
	struct pci_dev *pdev;
	void *context, *messages, *window;
	dma_addr_t context_dma, messages_dma, window_dma;
	void __iomem *bar;
	u32 index[4], mtr, mcr;
};
enum centauri_t6050_boot_phase {
	CEN_BOOT_ROM,
	CEN_BOOT_CYCLE,
	CEN_BOOT_IPC,
	CEN_BOOT_READY,
	CEN_BOOT_FAILED,
	CEN_BOOT_STOPPED,
};

struct centauri_t6050 {
	struct pci_dev *pdev;
	void __iomem *bar;
	resource_size_t size;
	u32 platform_id, protocol;
	const char *firmware_name;
	struct dentry *debug;
	struct mutex boot_lock;
	struct delayed_work boot_work;
	enum centauri_t6050_boot_phase boot_phase;
	bool removing, ready;
	void *firmware, *descriptor;
	struct centauri_t6050_irqs irqs, alpha_irqs;
	dma_addr_t firmware_dma, descriptor_dma;
	size_t firmware_size;
	u32 image_response;
	bool boot_attempted;
	bool link_ready;
	struct centauri_t6050_ipc *ipc;
	struct centauri_t6050_wifi *wifi;
};

int centauri_t6050_request_irqs(struct centauri_t6050_irqs *irqs, struct pci_dev *pdev,
			 unsigned int count, irq_handler_t handler, void *data);
void centauri_t6050_free_irqs(struct centauri_t6050_irqs *irqs);

int centauri_t6050_start_ipc(struct centauri_t6050 *c);
void centauri_t6050_free_ipc(struct centauri_t6050 *c);
int centauri_t6050_alpha_resources(struct centauri_t6050 *c, struct cen_alpha_resources *resources);
bool centauri_t6050_stop_dma(struct pci_dev *pdev);

#if IS_ENABLED(CONFIG_APPLE_CENTAURI_T6050_WIFI)
irqreturn_t centauri_t6050_alpha_irq(int irq, void *data);
int centauri_t6050_wifi_register(struct centauri_t6050 *c);
void centauri_t6050_wifi_unregister(struct centauri_t6050 *c);
#else
static inline irqreturn_t centauri_t6050_alpha_irq(int irq, void *data)
{
	return IRQ_HANDLED;
}
static inline int centauri_t6050_wifi_register(struct centauri_t6050 *c)
{
	return 0;
}
static inline void centauri_t6050_wifi_unregister(struct centauri_t6050 *c)
{
}
#endif
#endif
