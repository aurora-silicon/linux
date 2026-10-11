
#ifndef AURORA_CENTAURI_H
#define AURORA_CENTAURI_H
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>

#define CEN_ALPHA_WINDOW_SIZE SZ_32M
#define CEN_ALPHA_SCRATCH_OFFSET SZ_8M
#define CEN_ALPHA_SCRATCH_SIZE (5 * SZ_1M)

struct centauri_ipc;
struct centauri_wifi;
struct centauri_irqs {
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
enum centauri_boot_phase {
	CEN_BOOT_ROM,
	CEN_BOOT_CYCLE,
	CEN_BOOT_IPC,
	CEN_BOOT_READY,
	CEN_BOOT_FAILED,
	CEN_BOOT_STOPPED,
};

struct centauri {
	struct pci_dev *pdev;
	void __iomem *bar;
	resource_size_t size;
	u32 platform_id, protocol;
	const char *firmware_name;
	struct dentry *debug;
	struct mutex boot_lock;
	struct delayed_work boot_work;
	enum centauri_boot_phase boot_phase;
	bool removing;
	void *firmware, *descriptor;
	struct centauri_irqs irqs, alpha_irqs;
	dma_addr_t firmware_dma, descriptor_dma;
	size_t firmware_size;
	u32 image_response;
	bool boot_attempted;
	bool link_ready;
	struct centauri_ipc *ipc;
	struct centauri_wifi *wifi;
};

int centauri_request_irqs(struct centauri_irqs *irqs, struct pci_dev *pdev,
			 unsigned int count, irq_handler_t handler, void *data);
void centauri_free_irqs(struct centauri_irqs *irqs);

int centauri_start_ipc(struct centauri *c);
void centauri_free_ipc(struct centauri *c);
int centauri_alpha_resources(struct centauri *c, struct cen_alpha_resources *resources);
bool centauri_stop_dma(struct pci_dev *pdev);

#if IS_ENABLED(CONFIG_APPLE_CENTAURI_WIFI)
irqreturn_t centauri_alpha_irq(int irq, void *data);
int centauri_wifi_register(struct centauri *c);
void centauri_wifi_unregister(struct centauri *c);
#else
static inline irqreturn_t centauri_alpha_irq(int irq, void *data)
{
	return IRQ_HANDLED;
}
static inline int centauri_wifi_register(struct centauri *c)
{
	return 0;
}
static inline void centauri_wifi_unregister(struct centauri *c)
{
}
#endif
#endif
