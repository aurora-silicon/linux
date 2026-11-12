/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The coprocessor and enclave registry that the Apple SoC sleep controllers
 * (sleep.c for T8140, t6040-sleep.c for T6040) share. Drivers register through
 * <linux/soc/apple/sleep.h>; the controller bound on this SoC walks the table
 * at its commit.
 */
#ifndef _APPLE_SLEEP_INTERNAL_H_
#define _APPLE_SLEEP_INTERNAL_H_

#include <linux/mutex.h>
#include <linux/types.h>

struct device;
struct platform_device;

/* The coprocessors the last mile stops. Each driver idles its own first. */
#define APPLE_SLEEP_COPROCS	6

struct apple_sleep_coproc {
	struct device *dev;
	void __iomem *asc;
	bool pcie_ans;
	int (*last_check)(void *data);
	int (*last_stop)(void *data);
	void *data;
	bool ready;
	bool stopped;
	u32 status;
	int stop_ret;
	/* No ASC: its last stop takes it whole, power word @ps included, before the others. */
	bool first;
	u32 ps;
};

/* Irreversible enclave stop; nonzero is legal only if SLEEP was not sent. */
struct apple_sleep_enclave {
	struct device *dev;
	int (*stop)(void *data);
	void *data;
};

extern struct apple_sleep_coproc apple_sleep_coprocs[APPLE_SLEEP_COPROCS];
extern struct apple_sleep_enclave apple_sleep_enclave;
extern struct mutex apple_sleep_coprocs_lock;

struct apple_sleep_coproc *apple_sleep_coproc_find(struct device *dev);

/*
 * The sleep controller bound on this SoC. Each binds only to its own SoC's
 * compatible, so at most one is bound.
 *
 * @commit_stops: this sleep's commit stops the registered coprocessors.
 * @enclave_admit: 0 to take an enclave registration, or the error to refuse
 *	it with. NULL takes every one.
 */
struct apple_sleep_ctl {
	bool (*commit_stops)(void);
	int (*enclave_admit)(void);
};

int apple_sleep_registry_bind(const struct apple_sleep_ctl *ctl);
void apple_sleep_registry_unbind(const struct apple_sleep_ctl *ctl);

/* Must match aura's struct aura_s2r_mailbox. */
#define AURA_S2R_MAGIC		0x3152325341525541ULL	/* "AURAS2R1" */

struct aura_s2r_mailbox {
	u64 magic;		/* aura, before the kernel starts */
	u64 boot_mpidr;		/* aura: the only core that may take resume_pa */
	u64 resume_pa;		/* kernel, armed before a commit; aura clears it */
	u64 resumes;		/* aura: resume_pa taken this many times */
};

/*
 * A SoC whose sleep doesn't fit the lists of struct apple_sleep_soc brings its own sequences.
 * @probe then runs in place of the driver's own, and installs the part's suspend operations.
 * @restore, if set, runs when the system comes back from a hibernation image.
 * @registry: the part's answers to the registry, which the driver binds around @probe.
 */
struct apple_sleep_part {
	int (*probe)(struct platform_device *pdev);
	int (*restore)(struct device *dev);
	struct apple_sleep_ctl registry;
};

#endif
