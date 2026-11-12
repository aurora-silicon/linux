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
struct seq_file;

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

/* For a debugfs file: who is registered, and how far each is. */
void apple_sleep_registry_show(struct seq_file *s);

/*
 * The rest is for the commit point, in this order: interrupts off and one CPU up, so nothing
 * takes the registry's lock. @dev is the sleep driver's, for the log.
 *
 * Every registered coprocessor has to be ready: returns how many aren't, each logged, and sets
 * @pcie_ans_idled if ANS is among the ready ones.
 */
unsigned int apple_sleep_coprocs_refuse(struct device *dev, bool *pcie_ans_idled);
/* Each ready coprocessor's last check: the first refusal, or 0. */
int apple_sleep_coprocs_check(struct device *dev);
bool apple_sleep_enclave_registered(void);
/* The enclave's stop, last of all refusals: nonzero only if no request was sent. */
int apple_sleep_enclave_stop(struct device *dev);
/* The last stops, those registered first with @first and then the rest. */
void apple_sleep_coprocs_stop(bool first, bool *pcie_ans);

#endif
