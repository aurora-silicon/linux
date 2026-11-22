/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SDHCI_PCI_GLI_PWRSEQ_H
#define SDHCI_PCI_GLI_PWRSEQ_H

#include <linux/mutex.h>
#include <linux/types.h>

#define GLI_PWRSEQ_ATTEMPTS	3
#define GLI_PWRSEQ_ON_MS		100
#define GLI_PWRSEQ_OFF_MS	250

enum gli_pwrseq_state {
	GLI_OFF_NO_MEDIA,
	GLI_POWERING,
	GLI_ENUMERATING,
	GLI_ON_MEDIA,
	GLI_QUIESCING,
};

enum gli_pwrseq_event {
	GLI_HELPER_INSERT,
	GLI_MEDIA_CHANGE,
	GLI_SUSPEND,
	GLI_RESUME,
};

/*
 * Sleepable operations, never called from an IRQ or re-entered by callbacks.
 * bus_lock/unlock must implement pci_lock_rescan_remove()/unlock in an adapter.
 * Lock order: PCI rescan/remove lock, then the single power-state mutex.
 *
 * rail/port may partially succeed on error. port(false) must synchronously
 * stop link/config traffic before rail(false). config(false) must exclude
 * and drain all config access, including users outside this state machine.
 * port(true) enables and trains at Gen1; gen2 must retain Gen1 on failure.
 * rescan may partially enumerate on error. remove must drain I/O, DMA and
 * endpoint interrupts, publish removal and remove the endpoint. It must
 * recheck debounced card presence and refuse with -EBUSY if media or I/O owns
 * the slot, leaving it usable. media returns debounced, polarity-corrected
 * SDHCI card presence (0/1), or a negative error. The helper is only a hint.
 */
struct gli_pwrseq_ops {
	void (*bus_lock)(void *ctx);
	void (*bus_unlock)(void *ctx);
	int (*rail)(void *ctx, bool on);
	int (*port)(void *ctx, bool on);
	void (*config)(void *ctx, bool allow);
	int (*rescan)(void *ctx);
	int (*media)(void *ctx);
	int (*remove)(void *ctx);
	int (*gen2)(void *ctx);
	void (*delay)(void *ctx, unsigned int ms);
};

struct gli_pwrseq {
	/* Serializes all state and callbacks, inside the PCI bus lock. */
	struct mutex lock;
	const struct gli_pwrseq_ops *ops;
	void *ctx;
	enum gli_pwrseq_state state;
	bool rail_on;
	bool port_on;
	bool endpoint;
	bool suspended;
	bool pending_insert;
};

/* Initialize only an already-off, empty slot with config access blocked. */
void gli_pwrseq_init(struct gli_pwrseq *seq,
		     const struct gli_pwrseq_ops *ops, void *ctx);
int gli_pwrseq_event(struct gli_pwrseq *seq, enum gli_pwrseq_event event);

#endif
