// SPDX-License-Identifier: GPL-2.0-only
/* Production lifecycle with failed endpoint, DART and parent transitions. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
typedef uint16_t u16;
typedef uint64_t u64;
#define ARRAY_SIZE(x) ((int)(sizeof(x) / sizeof((x)[0])))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
/* Single-threaded fixture: model publication without CPU barriers. */
#define smp_load_acquire(p) (*(p))
/* Single-threaded fixture: model publication without CPU barriers. */
#define smp_store_release(p, v) (*(p) = (v))
#define PCI_COMMAND 4
#define PCI_COMMAND_MASTER 4
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_emerg(...) ((void)0)
struct device {
	int unused;
};
struct task_struct {
	int unused;
} task;
#define current (&task)
struct pci_dev {
	struct device dev;
	u16 command;
	int writes;
	bool fail_write, state_saved;
};
struct apple_radio_sleep;
struct apple_pcie {
	struct device *dev;
	int lock;
	unsigned int radio_writers;
	bool radio_closed;
	struct apple_radio_sleep *radio;
};
struct apple_dart_sleep {
	bool active, sealed, verified;
};
enum apple_radio_phase {
	APPLE_RADIO_IDLE,
	APPLE_RADIO_OPEN,
	APPLE_RADIO_SEALING,
	APPLE_RADIO_SEALED,
	APPLE_RADIO_VERIFIED,
	APPLE_RADIO_ABORTED,
	APPLE_RADIO_FAILED,
	APPLE_RADIO_STOPPING,
	APPLE_RADIO_STOPPED,
	APPLE_RADIO_RECOVERING
};
struct apple_radio_function {
	struct pci_dev *pdev;
	u16 command;
	bool attempted, held;
	u64 suspended, resumed;
	int error;
	void (*finish)(struct device *dev);
};
struct apple_radio_sleep {
	struct apple_pcie *pcie;
	struct apple_dart_sleep dart;
	struct apple_radio_function fn[2];
	struct task_struct *owner;
	u64 generation;
	enum apple_radio_phase phase;
	bool bound, host_suspended, snapshot, recovery_ready, config_ready;
	struct pci_dev *root;
	void *port;
};
static struct apple_radio_sleep *apple_radio_active;
static bool irqoff, fw_suspend, fw_resume;
static int seal_error, abort_error, snapshot_error, power_error, recover_error;
static int snapshot_reads, finish_calls, config_reads, off_calls, restore_calls,
	recover_calls, port_up;
static bool irq_disabled;
static jmp_buf terminal;
static bool irqs_disabled(void)
{
	return irqoff;
}
static bool pm_suspend_via_firmware(void)
{
	return fw_suspend;
}
static bool pm_resume_via_firmware(void)
{
	return fw_resume;
}
static _Noreturn void apple_radio_terminal(void)
{
	longjmp(terminal, 1);
}
static int pci_read_config_word(struct pci_dev *d, int r, u16 *v)
{
	assert(r == 4);
	config_reads++;
	*v = d->command;
	return 0;
}
static int pci_write_config_word(struct pci_dev *d, int r, u16 v)
{
	assert(r == 4);
	d->command = v;
	d->writes++;
	if (d->fail_write) {
		d->fail_write = false;
		return 1;
	}
	return 0;
}
static int pci_wait_for_pending_transaction(struct pci_dev *d)
{
	(void)d;
	return 1;
}
static int apple_radio_snapshot(struct apple_radio_sleep *s, bool verify)
{
	(void)s;
	(void)verify;
	snapshot_reads++;
	return snapshot_error;
}
static int apple_dart_sleep_seal(struct apple_dart_sleep *d)
{
	if (seal_error)
		return seal_error;
	d->sealed = true;
	return 0;
}
static int apple_dart_sleep_power_off(struct apple_dart_sleep *d)
{
	assert(d->sealed);
	off_calls++;
	return power_error;
}
static int apple_dart_sleep_power_restore(struct apple_dart_sleep *d)
{
	restore_calls++;
	if (recover_error)
		return recover_error;
	d->verified = true;
	return 0;
}
static int apple_radio_recover(struct apple_radio_sleep *s)
{
	assert(s->phase == APPLE_RADIO_RECOVERING && s->pcie->radio_closed &&
	       irq_disabled);
	recover_calls++;
	if (recover_error)
		return recover_error;
	s->dart.verified = true;
	s->config_ready = true;
	return 0;
}
static int apple_dart_sleep_abort(struct apple_dart_sleep *d)
{
	if (abort_error)
		return abort_error;
	d->sealed = false;
	return 0;
}
static int apple_dart_sleep_finish(struct apple_dart_sleep *d)
{
	d->active = false;
	return 0;
}
static void finish_endpoint(struct device *d)
{
	(void)d;
	assert(!apple_radio_active);
	finish_calls++;
}
static void apple_pcie_port_enable_irq(void *p)
{
	(void)p;
	irq_disabled = false;
}
static void apple_pcie_port_disable_irq(void *p)
{
	(void)p;
	irq_disabled = true;
}
static void device_set_awake_path(struct device *d)
{
	(void)d;
}
static void apple_sleep_radio_port_up(void)
{
	port_up++;
}
/* ACTUAL_FUNCTIONS */
static struct apple_radio_sleep s;
static struct apple_pcie host;
static struct pci_dev wifi, bt, root;
static void fresh(void)
{
	memset(&s, 0, sizeof(s));
	memset(&host, 0, sizeof(host));
	memset(&wifi, 0, sizeof(wifi));
	memset(&bt, 0, sizeof(bt));
	wifi.command = bt.command = 7;
	s.fn[0].pdev = &wifi;
	s.fn[1].pdev = &bt;
	s.pcie = &host;
	host.radio = &s;
	s.bound = true;
	s.generation = 9;
	s.owner = current;
	s.phase = APPLE_RADIO_OPEN;
	s.recovery_ready = true;
	s.root = &root;
	root.state_saved = wifi.state_saved = bt.state_saved = true;
	s.host_suspended = true;
	s.fn[0].suspended = s.fn[1].suspended = 9;
	s.dart.active = true;
	apple_radio_active = &s;
	irqoff = false;
	fw_suspend = true;
	fw_resume = false;
	irq_disabled = false;
	seal_error = abort_error = snapshot_error = power_error =
		recover_error = 0;
	snapshot_reads = finish_calls = config_reads = off_calls =
		restore_calls = recover_calls = port_up = 0;
}
int main(void)
{
	fresh();
	assert(apple_radio_seal(&s, 8) == -ESTALE);
	assert(!wifi.writes);
	fresh();
	s.fn[1].suspended = 8;
	assert(apple_radio_suspend_noirq(&host) == -EBUSY);
	assert(!wifi.writes && !host.radio_closed && !off_calls &&
	       !irq_disabled);
	fresh();
	host.radio_writers = 1;
	assert(apple_radio_suspend_noirq(&host) == -EBUSY);
	assert(!wifi.writes);
	fresh();
	bt.fail_write = true;
	assert(apple_radio_suspend_noirq(&host) == -EIO);
	assert(wifi.command == 7 && bt.command == 7 && !host.radio_closed);
	fresh();
	seal_error = -EBUSY;
	assert(apple_radio_suspend_noirq(&host) == -EBUSY);
	assert(wifi.command == 7 && !off_calls);
	fresh();
	root.state_saved = false;
	assert(apple_radio_suspend_noirq(&host) == -EUCLEAN);
	assert(!off_calls && wifi.command == 7);
	fresh();
	assert(!apple_radio_suspend_noirq(&host));
	assert(s.phase == APPLE_RADIO_STOPPED && host.radio_closed &&
	       off_calls == 1 && irq_disabled);
	assert(!apple_radio_config_allowed(&host));
	assert(wifi.command == 3 && bt.command == 3);
	/* Refusal/pm_test: no port reset or retrain, but translations and BME must return. */
	assert(!apple_radio_resume_noirq(&host));
	assert(restore_calls == 1 && !recover_calls && !port_up);
	assert(wifi.command == 7 && bt.command == 7 &&
	       s.phase == APPLE_RADIO_VERIFIED && !host.radio_closed &&
	       !irq_disabled);
	assert(apple_radio_config_allowed(&host));
	s.fn[0].resumed = s.fn[1].resumed = 9;
	s.fn[0].finish = s.fn[1].finish = finish_endpoint;
	assert(!apple_radio_finish(&s, 9));
	assert(!apple_radio_active && finish_calls == 2);
	/* Real loss: only the full port/routes/DART/link reconstruction admits child config. */
	fresh();
	assert(!apple_radio_suspend_noirq(&host));
	fw_resume = true;
	assert(!apple_radio_resume_noirq(&host));
	assert(recover_calls == 1 && !restore_calls && port_up == 1);
	assert(apple_radio_config_allowed(&host) && !host.radio_closed &&
	       !irq_disabled);
	assert(apple_radio_finish(&s, 9) ==
	       -EUCLEAN); /* Missing endpoint success cannot count. */
	s.fn[0].resumed = s.fn[1].resumed = 9;
	s.fn[1].error = -EIO;
	assert(apple_radio_finish(&s, 9) == -EUCLEAN);
	s.fn[1].error = 0;
	assert(!apple_radio_finish(&s, 9));
	for (int physical = 0; physical < 2; physical++) {
		fresh();
		assert(!apple_radio_suspend_noirq(&host));
		fw_resume = physical;
		recover_error = -EIO;
		if (!setjmp(terminal)) {
			apple_radio_resume_noirq(&host);
			assert(!"failed restore released endpoints");
		}
		assert(host.radio_closed && irq_disabled &&
		       s.phase == APPLE_RADIO_FAILED &&
		       !apple_radio_config_allowed(&host));
	}
	fresh();
	fw_suspend = false;
	assert(!apple_radio_suspend_noirq(&host));
	assert(!off_calls && !wifi.writes);
	puts("PASS: radio PM, suspend refusal, DMA fencing and failed recovery");
}
