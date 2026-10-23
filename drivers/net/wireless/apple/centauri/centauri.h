/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AURORA_CENTAURI_H
#define AURORA_CENTAURI_H
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/of.h>
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

/* One row per machine with an N1: the FTAB file name, the word written to BAR+0x807c,
 * (board-id << 16) | chip-id from the machine's ADT /chosen, and the physical addresses and
 * the one differing register value of the port cycle (centauri_port.c).
 * J813: board 0x24, chip 0x8142 (measured); port, PHY lane and PERST pad from the live ADT.
 * J873 (Mac mini M6): board 0x24, chip 0x8152 from its ADT, the composition taken from the J813
 * word; port 0 config 0x388028000, lane 0x38f010000 and PERST# gpio0 pin 3 (0x31a000000 + 4 * 3)
 * from its ADT; config +0x130 is 0x208 as in the m1n1 T8152 bring-up that links the port.
 */
struct centauri_board {
	const char *compatible;
	const char *firmware;
	u32 board_word;
	u64 port, phy, pad;
	u32 tune_130;
	u16 bridge_id;	/* root port device ID, 0 = any Apple root port */
};

static inline const struct centauri_board *centauri_board(void)
{
	static const struct centauri_board boards[] = {
		{ "apple,j813", "apple/centauri/j813-ftab.bin", 0x00248142,
		  0x488028000ULL, 0x48f010000ULL, 0x39a000300ULL, 0x03020000, 0x100c },
		{ "apple,j873", "apple/centauri/j873-ftab.bin", 0x00248152,
		  0x388028000ULL, 0x38f010000ULL, 0x31a00000cULL, 0x208, 0 },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(boards); i++)
		if (of_machine_is_compatible(boards[i].compatible))
			return &boards[i];
	return NULL;
}

int centauri_port_cycle(struct pci_dev *pdev);
int centauri_start_ipc(struct centauri *c);
void centauri_free_ipc(struct centauri *c);
int centauri_alpha_resources(struct centauri *c, struct cen_alpha_resources *resources);
int centauri_alpha_log_buffer(struct centauri *c, void **cpu, dma_addr_t *dma, size_t *size);
int centauri_wifi_register(struct centauri *c);
void centauri_wifi_unregister(struct centauri *c);
#endif
