/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AURORA_CENTAURI_H
#define AURORA_CENTAURI_H
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>

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
	struct dentry *debug, *crash_file, *crash_saved_file;
	/* The crash region and the firmware log as a restart found them, kept for debugfs */
	void *crash_saved, *log_saved;
	u32 crash_saved_len, log_saved_len;
	struct dentry *log_saved_file;
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
	/* Restart of a stopped firmware: requested by the Wi-Fi code, confirmed and run here */
	struct delayed_work recover_work;
	unsigned long recover_at[3];
	bool removing;
	bool recover_gave_up;	/* restart limit reached: requests are ignored */
	bool restart_failed;	/* the last restart failed with the link up: try again */
	bool warm_reset;	/* the root port describes the N1's warm reset: restart on a stop */
	bool inject_stall;	/* debugfs test: the next check finds the firmware stopped */
};

/* Per-board hardware descriptors: FTAB firmware path, ROM board word
 * ((board-id << 16) | chip-id written to BAR+0x807c), PCIe root-port
 * addresses and link-tuning parameters (centauri_port.c), plus firmware
 * protocol feature flags (link_tlv for startInterface link-address TLV
 * and scan_tlvs for explicit channel-list/OCE scan TLVs).
 *
 * keep_port_tables: on platforms where port+0x3000/0x3800 hold the
 * PCIe RID2SID and MSIMAP tables programmed by pcie-apple, save and
 * restore RID2SID across the port reset cycle so function 0 retains
 * its IOMMU stream mapping.
 */
struct centauri_board {
	const char *compatible;
	const char *firmware;
	u32 board_word;
	u64 port, phy, pad;
	u32 tune_130;
	u16 bridge_id;	/* root port device ID, 0 = any Apple root port */
	bool keep_port_tables;
	u16 link_tlv;
	bool scan_tlvs;
};

static inline const struct centauri_board *centauri_board(void)
{
	static const struct centauri_board boards[] = {
		{ "apple,j813", "apple/centauri/j813-ftab.bin", 0x00248142,
		  0x488028000ULL, 0x48f010000ULL, 0x39a000300ULL, 0x03020000, 0x100c, false,
		  385, false },
		{ "apple,j815", "apple/centauri/j815-ftab.bin", 0x00268142,
		  0x488028000ULL, 0x48f010000ULL, 0x39a000300ULL, 0x03020000, 0x100c, false,
		  386, true },
		{ "apple,j873g", "apple/centauri/j873-ftab.bin", 0x00248152,
		  0x388028000ULL, 0x38f010000ULL, 0x31a00000cULL, 0x208, 0, true,
		  386, true },
		{ "apple,j804", "apple/centauri/j804-ftab.bin", 0x00228152,
		  0x388028000ULL, 0x38f010000ULL, 0x31a00000cULL, 0x208, 0, true,
		  386, true },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(boards); i++)
		if (of_machine_is_compatible(boards[i].compatible))
			return &boards[i];
	return NULL;
}

int centauri_port_cycle(struct pci_dev *pdev, int *reset_ret);
bool centauri_warm_reset_described(struct pci_dev *pdev);
int centauri_start_ipc(struct centauri *c);
void centauri_save_crash(struct centauri *c);
bool centauri_beta_running(struct centauri *c);
bool centauri_stop_dma(struct pci_dev *pdev);
bool centauri_stop_ipc(struct centauri *c);
void centauri_free_ipc(struct centauri *c);
int centauri_alpha_resources(struct centauri *c, struct cen_alpha_resources *resources);
int centauri_alpha_log_buffer(struct centauri *c, void **cpu, dma_addr_t *dma, size_t *size);
int centauri_wifi_register(struct centauri *c);
void centauri_wifi_unregister(struct centauri *c);
int centauri_wifi_suspend(struct centauri *c);
int centauri_wifi_resume(struct centauri *c);
void centauri_wifi_kick(struct centauri *c);
bool centauri_wifi_stalled(struct centauri *c);
void centauri_recovery_request(struct centauri *c, const char *why);
#endif
