// SPDX-License-Identifier: GPL-2.0-only
/* Execute the real suspend/finish hooks around serialized driver removal. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define BRCMF_MAX_IFS 16

struct device_driver {
	int unused;
};
struct pci_driver {
	struct device_driver driver;
};
struct device {
	pthread_mutex_t lock;
	struct device_driver *driver;
	void *data;
};
struct brcmf_pciedev_info {
	bool setup_complete, sleep_reset_blocked, mbdata_completed;
	bool wowl_irq, wowl_enabled, fwseed;
	int state, mbdata_resp_wait;
};
struct brcmf_pciedev {
	struct brcmf_pciedev_info *devinfo;
};
struct brcmf_if {
	void *ndev;
};
struct brcmf_pub {
	struct brcmf_if *iflist[BRCMF_MAX_IFS];
};
struct brcmf_bus {
	struct {
		struct brcmf_pciedev *pcie;
	} bus_priv;
	struct brcmf_pub *drvr;
	bool removing;
};
static struct pci_driver brcmf_pciedrvr;
static struct device_driver foreign_driver;
static struct device endpoint = { .lock = PTHREAD_MUTEX_INITIALIZER };
static _Thread_local bool endpoint_locked;
static bool owned;
static void (*finish_callback)(struct device *dev);
static unsigned int allows, cancels;

/* Synchronize the two overlap cases without sleeps or scheduler assumptions. */
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;
static bool watch_lock, lock_attempted, pause_allow, in_allow, release_allow;

static void device_lock(struct device *dev)
{
	assert(dev == &endpoint);
	pthread_mutex_lock(&sync_lock);
	if (watch_lock) {
		lock_attempted = true;
		pthread_cond_broadcast(&sync_cond);
	}
	pthread_mutex_unlock(&sync_lock);
	pthread_mutex_lock(&dev->lock);
	endpoint_locked = true;
}

static void device_unlock(struct device *dev)
{
	assert(endpoint_locked);
	endpoint_locked = false;
	pthread_mutex_unlock(&dev->lock);
}

static void *dev_get_drvdata(struct device *dev)
{
	/* This is a lifetime assertion, not a simulated lock. */
	assert(endpoint_locked);
	return dev->data;
}

static void brcmf_bus_allow_reset_work(struct brcmf_bus *bus)
{
	assert(endpoint_locked && bus && bus == endpoint.data);
	pthread_mutex_lock(&sync_lock);
	in_allow = true;
	pthread_cond_broadcast(&sync_cond);
	while (pause_allow && !release_allow)
		pthread_cond_wait(&sync_cond, &sync_lock);
	pthread_mutex_unlock(&sync_lock);
	bus->removing = false;
	allows++;
}

static void brcmf_bus_cancel_reset_work(struct brcmf_bus *bus)
{
	assert(endpoint_locked && bus == endpoint.data);
	bus->removing = true;
	cancels++;
}

static bool apple_radio_sleep_owned(struct device *dev)
{
	assert(dev == &endpoint);
	return owned;
}

static int
apple_radio_sleep_endpoint_prepare(struct device *dev,
				   void (*finish)(struct device *dev))
{
	assert(dev == &endpoint);
	if (owned)
		finish_callback = finish;
	return 0;
}

static int apple_radio_sleep_endpoint_suspended(struct device *dev)
{
	assert(dev == &endpoint);
	return 0;
}

static int brcmf_pcie_pm_leave_D3(struct device *dev)
{
	(void)dev;
	return 0;
}

#define BRCMF_NETIF_STOP_REASON_BUS 0
#define BRCMF_BUS_DOWN 0
#define BRCMF_BUS_UP 1
#define BRCMFMAC_PCIE_STATE_DOWN 0
#define BRCMF_H2D_HOST_D3_INFORM 0
#define BRCMF_PCIE_MBDATA_TIMEOUT 100
#define brcmf_dbg(...) ((void)0)
#define brcmf_err(...) ((void)0)
/* The asynchronous setup publication is supplied before the callback starts. */
#define smp_load_acquire(p) (*(p))
#define wait_event_timeout(wait, condition, timeout) \
	((void)(wait), (void)(condition), (void)(timeout))
static void brcmf_txflowblock_if(struct brcmf_if *ifp, int reason, bool block)
{
	(void)ifp;
	(void)reason;
	(void)block;
}
static void netif_tx_disable(void *ndev)
{
	(void)ndev;
}
static void brcmf_msgbuf_suspend(struct brcmf_pub *drvr)
{
	(void)drvr;
}
static void brcmf_msgbuf_resume(struct brcmf_pub *drvr)
{
	(void)drvr;
}
static void brcmf_pcie_fwcon_timer(struct brcmf_pciedev_info *info, bool on)
{
	(void)info;
	(void)on;
}
static void brcmf_bus_change_state(struct brcmf_bus *bus, int state)
{
	(void)bus;
	(void)state;
}
static void brcmf_pcie_send_mb_data(struct brcmf_pciedev_info *info, int value)
{
	(void)value;
	info->mbdata_completed = true;
}

/* WoWLAN and the s2idle D0 sleep: never taken in these deep-sleep cases. */
#define PM_SUSPEND_TO_IDLE 1
#define PM_SUSPEND_MEM 3
static int pm_suspend_target_state = PM_SUSPEND_MEM;
static bool device_may_wakeup(struct device *dev)
{
	(void)dev;
	return false;
}
static int brcmf_pcie_wowl_sleep(struct device *dev)
{
	(void)dev;
	assert(!"WoWLAN sleep without WoWLAN armed");
	return -EINVAL;
}
static int brcmf_pcie_s2idle_sleep(struct device *dev)
{
	(void)dev;
	assert(!"s2idle D0 sleep in a deep sleep");
	return -EINVAL;
}

/* ACTUAL_FUNCTIONS */

static void bind(bool setup_complete)
{
	struct brcmf_bus *bus = calloc(1, sizeof(*bus));

	assert(endpoint_locked && !endpoint.data && bus);
	bus->bus_priv.pcie = calloc(1, sizeof(*bus->bus_priv.pcie));
	assert(bus->bus_priv.pcie);
	bus->bus_priv.pcie->devinfo =
		calloc(1, sizeof(*bus->bus_priv.pcie->devinfo));
	assert(bus->bus_priv.pcie->devinfo);
	bus->bus_priv.pcie->devinfo->setup_complete = setup_complete;
	endpoint.driver = &brcmf_pciedrvr.driver;
	endpoint.data = bus;
}

static void remove_binding(void)
{
	struct brcmf_bus *bus = endpoint.data;

	assert(endpoint_locked && bus);
	free(bus->bus_priv.pcie->devinfo);
	free(bus->bus_priv.pcie);
	free(bus);
	endpoint.data = NULL;
	endpoint.driver = NULL;
}

static void fresh(bool setup_complete)
{
	assert(!endpoint.data);
	watch_lock = lock_attempted = pause_allow = in_allow = release_allow =
		false;
	allows = cancels = 0;
	finish_callback = NULL;
	owned = true;
	device_lock(&endpoint);
	bind(setup_complete);
	device_unlock(&endpoint);
}

static int suspend_endpoint(void)
{
	int ret;

	device_lock(&endpoint);
	ret = brcmf_pcie_pm_enter_D3(&endpoint);
	device_unlock(&endpoint);
	return ret;
}

static void host_complete(void)
{
	/* Match apple_radio_finish: end ownership before invoking endpoint hooks. */
	owned = false;
	if (finish_callback)
		finish_callback(&endpoint);
}

static void *finish_thread(void *unused)
{
	(void)unused;
	host_complete();
	return NULL;
}

static void *remove_thread(void *unused)
{
	(void)unused;
	device_lock(&endpoint);
	remove_binding();
	device_unlock(&endpoint);
	return NULL;
}

static void wait_flag(bool *flag)
{
	pthread_mutex_lock(&sync_lock);
	while (!*flag)
		pthread_cond_wait(&sync_cond, &sync_lock);
	pthread_mutex_unlock(&sync_lock);
}

int main(void)
{
	pthread_t finisher, remover;
	struct brcmf_bus *replacement;

	/* A refused suspend with a live binding must undo its reset-work block. */
	fresh(false);
	assert(suspend_endpoint() == -EBUSY && cancels == 1);
	host_complete();
	assert(allows == 1);
	remove_thread(NULL);

	/* The reported race: async setup failure removes the failed PM device. */
	fresh(false);
	assert(suspend_endpoint() == -EBUSY);
	remove_thread(NULL);
	host_complete();
	assert(!allows);

	/* An endpoint can keep its driver pointer while drvdata is already clear. */
	endpoint.driver = &brcmf_pciedrvr.driver;
	host_complete();
	assert(!allows);
	endpoint.driver = NULL;

	/* Same-driver replacement must not inherit the old binding's cleanup. */
	fresh(false);
	assert(suspend_endpoint() == -EBUSY);
	remove_thread(NULL);
	device_lock(&endpoint);
	bind(true);
	replacement = endpoint.data;
	replacement->removing = true;
	device_unlock(&endpoint);
	host_complete();
	assert(!allows && replacement->removing);
	endpoint.driver = &foreign_driver;
	replacement->bus_priv.pcie->devinfo->sleep_reset_blocked = true;
	host_complete();
	assert(!allows && replacement->removing);
	remove_thread(NULL);

	/* Removal holds the device lock while the finish callback arrives. */
	fresh(false);
	assert(suspend_endpoint() == -EBUSY);
	device_lock(&endpoint);
	watch_lock = true;
	assert(!pthread_create(&finisher, NULL, finish_thread, NULL));
	wait_flag(&lock_attempted);
	remove_binding();
	device_unlock(&endpoint);
	assert(!pthread_join(finisher, NULL));
	assert(!allows);

	/* Conversely, removal must wait until finish stops using the bus. */
	fresh(false);
	assert(suspend_endpoint() == -EBUSY);
	pause_allow = true;
	assert(!pthread_create(&finisher, NULL, finish_thread, NULL));
	wait_flag(&in_allow);
	pthread_mutex_lock(&sync_lock);
	watch_lock = true;
	pthread_mutex_unlock(&sync_lock);
	assert(!pthread_create(&remover, NULL, remove_thread, NULL));
	wait_flag(&lock_attempted);
	pthread_mutex_lock(&sync_lock);
	release_allow = true;
	pthread_cond_broadcast(&sync_cond);
	pthread_mutex_unlock(&sync_lock);
	assert(!pthread_join(finisher, NULL));
	assert(!pthread_join(remover, NULL));
	assert(allows == 1 && !endpoint.data);

	/* Completed setup admits D3; completion releases the block exactly once. */
	fresh(true);
	assert(!suspend_endpoint());
	host_complete();
	host_complete();
	assert(allows == 1);
	remove_thread(NULL);

	/* Other hosts do not acquire a deferred reset-work cleanup. */
	fresh(true);
	owned = false;
	assert(!suspend_endpoint() && !finish_callback && !cancels);
	host_complete();
	assert(!allows);
	remove_thread(NULL);
	puts("PASS: Wi-Fi abort, binding replacement and overlapping remove/finish");
	return 0;
}
