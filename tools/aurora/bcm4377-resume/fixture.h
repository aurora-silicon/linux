/* SPDX-License-Identifier: GPL-2.0-only */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <stddef.h>
#include <pthread.h>
#define bt_dev_dbg(...) ((void)0)
#define bt_dev_err(...) ((void)0)
#define HCI_DEV_SUSPEND 1
#define HCI_DEV_RESUME 2
#define HCI_UP_INDEX 5
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define HCI_SETUP 1
#define HCI_AUTO_OFF 2
#define HCI_UNREGISTER 4
#define HCI_USER_CHANNEL 8
#define HCI_RUNNING 16
#define HCI_UP 32
#define HCI_RFKILLED 64
#define HCI_POWERING_DOWN 128
#define GFP_KERNEL 0
#define lockdep_assert_held(p) assert(req_locked)
#define BCM4377_BAR0_SLEEP_CONTROL 0
#define BCM4377_BAR0_SLEEP_CONTROL_QUIESCE 1
#define BCM4377_BAR0_SLEEP_CONTROL_UNQUIESCE 0
#define system_freezable_wq 0
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
struct work_struct { int queued; };
struct delayed_work { struct work_struct work; };
typedef struct { unsigned char b[6]; } bdaddr_t;
static const bdaddr_t zero={};
#define BDADDR_ANY (&zero)
static void bacpy(bdaddr_t *a,const bdaddr_t *b) { *a=*b; }
static int bacmp(const bdaddr_t *a,const bdaddr_t *b) { return memcmp(a,b,6); }
struct bcm4377_data;
struct firmware { const unsigned char *data; int size; };
struct hci_dev { struct bcm4377_data *data; unsigned int flags; void *req_workqueue,*workqueue; struct work_struct power_on,cmd_sync_work,cmd_work; struct delayed_work cmd_timer,ncmd_timer; int req_lock,cmd_cnt,suspend_state,wake_reason,wake_addr_type; bdaddr_t wake_addr; int *req_skb; void (*reset)(struct hci_dev*); bdaddr_t setup_addr,public_addr,bdaddr; };
struct ring { bool enabled; };
struct pci_dev {
	int irq, dev;
	void *data;
};
struct device { struct pci_dev *pci; };
struct bcm4377_hw { unsigned int id; int (*send_calibration)(struct bcm4377_data*); int (*send_ptb)(struct bcm4377_data*,const struct firmware*); };
struct bcm4377_data {
 struct hci_dev *hdev; struct pci_dev *pdev; struct bcm4377_hw *hw;
	bool wake_armed, quiesced, needs_reset, cal_needed, cal_loaded;
	bool setup_retry_used, setup_retry_pending, setup_retry_blocked;
	struct delayed_work setup_retry_work;
	bdaddr_t bdaddr;
	char *bar0;
	pthread_mutex_t resume_lock;
	uint64_t resume_epoch, resume_pending;
	bool resume_armed, resume_blocked, resume_powered;
	bool resume_guarded, resume_recovering;
	struct work_struct resume_work;
 struct ring acl_d2h_ring,acl_h2d_ring,sco_d2h_ring,sco_h2d_ring,hci_d2h_ring,hci_h2d_ring,sco_event_ring,sco_ack_ring,hci_acl_event_ring,hci_acl_ack_ring;
};
static int cal_count,ptb_count,reset_count,open_count,queued_count,addr_count,cal_failures,ptb_error,addr_error,reset_error,rings_error,close_error;
static int rebind_count;

/* Controlled boundary for the existing driver's asynchronous rebind work. */
static void bcm4377_hci_reset(struct hci_dev *hdev)
{
	rebind_count++;
}

static bool missing_blob;
static bool active_callback_pause,active_running;
static pthread_barrier_t active_started,active_release;
static pthread_t active_thread;
static const struct firmware fw={.data=(const unsigned char*)"own PTB"};
static void *hci_get_drvdata(struct hci_dev *h) { return h->data; }
static bool hci_dev_test_flag(struct hci_dev *h,unsigned int flag) { return h->flags&flag; }
static int send_cal(struct bcm4377_data *b) { cal_count++; if(cal_failures) {cal_failures--;return -ETIMEDOUT;} return 0; }
static int send_ptb(struct bcm4377_data *b,const struct firmware *f) { assert(f==&fw);ptb_count++;return ptb_error; }
static const struct firmware *bcm4377_request_blob(struct bcm4377_data *b,const char *suffix) { assert(!strcmp(suffix,"ptb"));return missing_blob?NULL:&fw; }
static void release_firmware(const struct firmware *f) { assert(f==&fw); }
static int bcm4377_check_bdaddr(struct bcm4377_data *b) { return 0; }
static int bcm4377_destroy_transfer_ring(struct bcm4377_data *b,struct ring *r) { r->enabled=false;return close_error; }
static int bcm4377_destroy_completion_ring(struct bcm4377_data *b,struct ring *r) { r->enabled=false;return close_error; }
static int bcm4377_hci_set_bdaddr(struct hci_dev *h,const bdaddr_t *a) { addr_count++;if(addr_error)return addr_error;h->bdaddr=*a;return 0; }
static int bcm4377_pci_reset(struct bcm4377_data *b) { reset_count++;if(active_callback_pause){pthread_barrier_wait(&active_started);pthread_barrier_wait(&active_release);}if(reset_error)return reset_error;b->needs_reset=false;return 0; }
static int bcm4377_hci_open_rings(struct bcm4377_data *b) { open_count++;return rings_error; }
static int mod_delayed_work(int wq,struct delayed_work *w,int delay) { w->work.queued=1;return 1; }
static int queue_work(void *wq,struct work_struct *w) { w->queued=1;queued_count++;return 1; }

typedef uint64_t u64;
#define spin_lock_irqsave(l,f) do { (f)=0;pthread_mutex_lock(l); } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f);pthread_mutex_unlock(l); } while(0)
static bool req_locked,board_j493=true,allocation_error;
static int submit_error,suspend_error,resume_error,recovery_error,quiesces,unregistered,timer_fences;
static bool old_timeout_at_fence,producer_pause;
static pthread_barrier_t producer_captured,producer_release;
static pthread_t producer_thread;
static void hci_cmd_timeout(struct work_struct *work);
static struct bcm4377_data *active;
static bool hdev_is_powered(struct hci_dev *h) {return (h->flags&HCI_UP) && !(h->flags&HCI_AUTO_OFF);}
static bool of_machine_is_compatible(const char *s) {assert(!strcmp(s,"apple,j493"));return board_j493;}
static void *kmalloc(size_t n,int flags) {return allocation_error?NULL:malloc(n);}
static void kfree(void *p) {free(p);}
#define bt_dev_warn(...) ((void)0)
struct queued {int (*fn)(struct hci_dev*,void*);void (*destroy)(struct hci_dev*,void*,int);void *data;};
static struct queued request;
static int hci_cmd_sync_queue(struct hci_dev *h,int (*fn)(struct hci_dev*,void*),void *data,void (*destroy)(struct hci_dev*,void*,int)) {if(producer_pause){pthread_barrier_wait(&producer_captured);pthread_barrier_wait(&producer_release);}if(submit_error)return submit_error;assert(!request.fn);request=(struct queued){fn,destroy,data};h->cmd_sync_work.queued=1;return 0;}
static int hci_cmd_sync_dequeue(struct hci_dev *h,int (*fn)(struct hci_dev*,void*),void *data,void *destroy) {if(request.fn==fn){request.destroy(h,request.data,-ECANCELED);request=(struct queued){};h->cmd_sync_work.queued=0;return 1;}return 0;}
static void run_request(struct hci_dev *h) {if(!request.fn)return;struct queued r=request;request=(struct queued){};h->cmd_sync_work.queued=0;req_locked=true;int e=r.fn(h,r.data);r.destroy(h,r.data,e);req_locked=false;}
static void flush_work(struct work_struct *w) { if(w==&active->hdev->cmd_sync_work){if(active_running){pthread_barrier_wait(&active_release);pthread_join(active_thread,NULL);active_running=false;}else run_request(active->hdev);}else w->queued=0; }
static void cancel_work_sync(struct work_struct *w) {if(producer_pause){pthread_barrier_wait(&producer_release);pthread_join(producer_thread,NULL);producer_pause=false;}w->queued=0;}
static void cancel_delayed_work_sync(struct delayed_work *w) {if(w==&active->hdev->cmd_timer){timer_fences++;if(old_timeout_at_fence){old_timeout_at_fence=false;hci_cmd_timeout(&w->work);}}w->work.queued=0;}
#define disable_delayed_work_sync cancel_delayed_work_sync
static struct pci_dev *to_pci_dev(struct device *d) {return d->pci;}
static void *pci_get_drvdata(struct pci_dev *p) {return p->data;}
static void iowrite32(int val,void *addr) {if(val==1){assert(!request.fn && !active->resume_work.queued && !active_running);quiesces++;}}
static bool fixture_wakeup;
static bool device_may_wakeup(struct device *d) { return fixture_wakeup; }
/* Not a J713 sleep: the radio sleep hooks do nothing, as on every other board. */
static bool apple_radio_sleep_owned(struct device *d) { return false; }
static int apple_radio_sleep_endpoint_prepare(struct device *d,
					      void (*finish)(struct device *))
{
	return 0;
}
static int apple_radio_sleep_endpoint_suspended(struct device *d) { return 0; }
static int apple_radio_sleep_endpoint_resume(struct device *d) { return 0; }
static void apple_radio_sleep_endpoint_resumed(struct device *d, int result) {}
static void pci_save_state(struct pci_dev *p) {}
static int enable_irq_wake(int irq) { return 0; }
static void disable_irq_wake(int irq) {}
static int ioread32(void *addr) { return 0; }
static int hci_suspend_dev(struct hci_dev *h);
static int hci_resume_dev(struct hci_dev *h);
static void hci_unregister_dev(struct hci_dev *h) {assert(!request.fn&&!active->resume_work.queued&&!active_running);h->flags|=HCI_UNREGISTER;unregistered++;}
static int hci_dev_close_sync(struct hci_dev *h);
static int hci_dev_open_sync(struct hci_dev *h);
int hci_reset_dev_sync(struct hci_dev *h);

typedef unsigned short u16;
static void atomic_set(int *v,int n) {*v=n;}
static void hci_cmd_sync_cancel_sync(struct hci_dev *h,int error) {}
static int hci_skb_opcode(int *p) {return *p;}
static bool mgmt_powering_down(struct hci_dev *h) {return h->flags&HCI_POWERING_DOWN;}
static void hci_req_sync_lock(struct hci_dev *h) {assert(!req_locked);req_locked=true;}
static void hci_req_sync_unlock(struct hci_dev *h) {assert(req_locked);req_locked=false;}
static int hci_suspend_sync(struct hci_dev *h) {return suspend_error;}
static int hci_resume_sync(struct hci_dev *h);
static void hci_clear_wake_reason(struct hci_dev *h) {}
static void mgmt_suspending(struct hci_dev *h,int state) {}
static void mgmt_resuming(struct hci_dev *h,int reason,bdaddr_t *addr,int type) {}
static void hci_sock_dev_event(struct hci_dev *h,int event) {}

static bool test_bit(int bit,unsigned int *flags) {return *flags&(1U<<bit);}
