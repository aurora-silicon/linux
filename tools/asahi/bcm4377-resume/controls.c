// SPDX-License-Identifier: GPL-2.0-only
static bool timeout_during_resume;
static int hci_resume_sync(struct hci_dev *h) { if(timeout_during_resume)hci_cmd_timeout(&h->cmd_timer.work);return resume_error; }
static int hci_dev_close_sync(struct hci_dev *h) {assert(req_locked);h->flags&=~HCI_UP;return bcm4377_hci_close(h);}
static int hci_dev_open_sync(struct hci_dev *h) {assert(req_locked);int e=bcm4377_hci_open(h);if(!e)e=bcm4377_hci_post_init(h);if(!e)h->flags|=HCI_UP;return e;}
static void deliver_work(struct bcm4377_data *b) {if(b->resume_work.queued){b->resume_work.queued=0;bcm4377_resume_recovery_work(&b->resume_work);}}
static void resume_cycle(struct device *dev) {assert(!bcm4377_suspend(dev));assert(!bcm4377_resume(dev));}
static void recover(struct bcm4377_data *b) {hci_cmd_timeout(&b->hdev->cmd_timer.work);deliver_work(b);run_request(b->hdev);}
static void *active_callback(void *data) {run_request(data);return NULL;}
static void *producer(void *data) {deliver_work(data);return NULL;}
static void pause_producer(struct bcm4377_data *b) {bcm4377_hci_timeout(b->hdev);producer_pause=true;pthread_barrier_init(&producer_captured,NULL,2);pthread_barrier_init(&producer_release,NULL,2);pthread_create(&producer_thread,NULL,producer,b);pthread_barrier_wait(&producer_captured);}
int main(int argc,char **argv) {
 struct bcm4377_hw hw=bcm4377_hw_variants[BCM4378];struct pci_dev pci={};
 struct bcm4377_data b={.hw=&hw,.pdev=&pci};struct hci_dev h={.data=&b,.flags=HCI_UP|HCI_RUNNING};b.hdev=&h;active=&b;pci.data=&b;struct device dev={.pci=&pci};pthread_mutex_init(&b.resume_lock,NULL);if(bcm4377_resume_recovery_supported(&b))h.reset=bcm4377_hci_timeout;
 bdaddr_t own={{2,3,4,5,6,7}},setup={{8,9,10,11,12,13}},pub={{14,15,16,17,18,19}};b.bdaddr=own;h.setup_addr=setup;h.public_addr=pub;
 switch(atoi(argv[1])) {
 case 0: resume_cycle(&dev);recover(&b);assert(reset_count==1&&cal_count==1&&ptb_count==1&&addr_count==1&&!bacmp(&h.bdaddr,&own)&&(h.flags&HCI_UP)&&!b.cal_needed);break;
 case 1: timeout_during_resume=true;resume_cycle(&dev);deliver_work(&b);run_request(&h);assert(reset_count==1);break;
 case 2: resume_cycle(&dev);recover(&b);recover(&b);assert(reset_count==1);break;
 case 3: resume_cycle(&dev);reset_error=-EIO;recover(&b);recover(&b);assert(reset_count==1&&!(h.flags&HCI_UP)&&b.needs_reset);break;
 case 4: resume_cycle(&dev);cal_failures=1;recover(&b);recover(&b);assert(reset_count==1&&cal_count==1&&b.cal_needed&&!(h.flags&HCI_UP));break;
 case 5: resume_cycle(&dev);addr_error=-EIO;recover(&b);assert(reset_count==1&&b.cal_loaded&&b.cal_needed&&!(h.flags&HCI_UP));break;
	case 6:
		recover(&b);
		assert(!reset_count && !request.fn && rebind_count == 1);
		break;
 case 7: h.flags=HCI_RUNNING;resume_cycle(&dev);recover(&b);assert(!reset_count);break;
 case 8: h.flags|=HCI_AUTO_OFF;resume_cycle(&dev);recover(&b);assert(!reset_count);break;
 case 9: h.flags|=HCI_SETUP;resume_cycle(&dev);recover(&b);assert(!reset_count);break;
 case 10: h.flags|=HCI_USER_CHANNEL;resume_cycle(&dev);recover(&b);assert(!reset_count);break;
 case 11: h.flags|=HCI_RFKILLED;resume_cycle(&dev);recover(&b);assert(!reset_count);break;
 case 12: h.flags|=HCI_POWERING_DOWN;resume_cycle(&dev);recover(&b);assert(!reset_count);break;
 case 13: board_j493=false;resume_cycle(&dev);recover(&b);assert(!reset_count);break;
	case 14:
		hw = bcm4377_hw_variants[BCM4377];
		resume_cycle(&dev);
		recover(&b);
		assert(!reset_count);
		break;
	case 15:
		hw = bcm4377_hw_variants[BCM4387];
		resume_cycle(&dev);
		recover(&b);
		assert(!reset_count);
		break;
	case 16:
		hw = bcm4377_hw_variants[BCM4388];
		resume_cycle(&dev);
		recover(&b);
		assert(!reset_count);
		break;
 case 17: resume_cycle(&dev);bcm4377_hci_timeout(&h);deliver_work(&b);assert(request.fn);bcm4377_hci_close(&h);run_request(&h);assert(!reset_count);break;
 case 18: resume_cycle(&dev);bcm4377_hci_timeout(&h);deliver_work(&b);h.flags&=~HCI_UP;bcm4377_hci_close(&h);h.flags|=HCI_UP;run_request(&h);assert(!reset_count);break;
 case 19: resume_cycle(&dev);bcm4377_hci_timeout(&h);deliver_work(&b);h.flags|=HCI_RFKILLED;run_request(&h);assert(!reset_count);break;
 case 20: resume_cycle(&dev);bcm4377_hci_timeout(&h);deliver_work(&b);h.flags|=HCI_POWERING_DOWN;run_request(&h);assert(!reset_count);break;
 case 21: resume_cycle(&dev);bcm4377_hci_timeout(&h);assert(!bcm4377_suspend(&dev));assert(!b.resume_work.queued&&!request.fn);assert(!bcm4377_resume(&dev));recover(&b);assert(reset_count==1);break;
 case 22: resume_cycle(&dev);bcm4377_hci_timeout(&h);deliver_work(&b);assert(!bcm4377_suspend(&dev));assert(!request.fn);assert(!bcm4377_resume(&dev));recover(&b);assert(reset_count==1);break;
 case 23: resume_cycle(&dev);bcm4377_hci_timeout(&h);bcm4377_hci_unregister_dev(&h);deliver_work(&b);assert(unregistered==1&&!reset_count&&!request.fn);break;
 case 24: resume_cycle(&dev);bcm4377_hci_timeout(&h);deliver_work(&b);bcm4377_hci_unregister_dev(&h);assert(unregistered==1&&!reset_count&&!request.fn);break;
 case 25: resume_cycle(&dev);allocation_error=true;recover(&b);allocation_error=false;recover(&b);assert(!reset_count&&!request.fn);break;
 case 26: resume_cycle(&dev);submit_error=-ENODEV;recover(&b);submit_error=0;recover(&b);assert(!reset_count&&!request.fn);break;
 case 27: suspend_error=-EBUSY;assert(bcm4377_suspend(&dev)==-EBUSY);assert(!quiesces&&!b.resume_powered);recover(&b);assert(!reset_count&&(h.flags&HCI_UP));suspend_error=0;resume_cycle(&dev);recover(&b);assert(reset_count==1);break;
 case 28: resume_cycle(&dev);recover(&b);resume_cycle(&dev);recover(&b);assert(reset_count==2&&cal_count==2&&addr_count==2);break;
 case 29: h.flags=HCI_SETUP|HCI_AUTO_OFF;cal_failures=1;assert(bcm4377_hci_setup(&h)==-ETIMEDOUT);bcm4377_hci_close(&h);assert(b.setup_retry_pending&&b.setup_retry_work.work.queued);assert(!reset_count&&!b.resume_work.queued);break;
 case 30: resume_cycle(&dev);b.bdaddr=zero;recover(&b);assert(!bacmp(&h.bdaddr,&setup));break;
 case 31: resume_cycle(&dev);b.bdaddr=zero;h.setup_addr=zero;recover(&b);assert(!bacmp(&h.bdaddr,&pub));break;
 case 32: resume_cycle(&dev);missing_blob=true;recover(&b);assert(reset_count==1&&b.cal_needed&&!(h.flags&HCI_UP));break;
 case 33: resume_cycle(&dev);bcm4377_hci_timeout(&h);deliver_work(&b);u64 *old=malloc(sizeof(*old));*old=*(u64*)request.data;assert(!bcm4377_suspend(&dev));assert(!bcm4377_resume(&dev));req_locked=true;assert(!bcm4377_resume_recovery_sync(&h,old));req_locked=false;free(old);assert(!reset_count);recover(&b);assert(reset_count==1);break;
 case 34: resume_cycle(&dev);bcm4377_hci_timeout(&h);deliver_work(&b);h.flags|=HCI_UNREGISTER;run_request(&h);assert(!reset_count);break;
 case 35: resume_cycle(&dev);pause_producer(&b);bcm4377_hci_close(&h);h.flags|=HCI_UP;pthread_barrier_wait(&producer_release);pthread_join(producer_thread,NULL);producer_pause=false;run_request(&h);assert(!reset_count);break;
 case 36: resume_cycle(&dev);pause_producer(&b);assert(!bcm4377_suspend(&dev));assert(!request.fn&&!b.resume_work.queued&&!reset_count);break;
 case 37: resume_cycle(&dev);pause_producer(&b);bcm4377_hci_unregister_dev(&h);assert(!request.fn&&!b.resume_work.queued&&!reset_count&&unregistered==1);break;
 case 38: resume_cycle(&dev);old_timeout_at_fence=true;assert(!bcm4377_suspend(&dev));assert(!request.fn&&!b.resume_pending&&!b.resume_work.queued);assert(!bcm4377_resume(&dev));recover(&b);assert(reset_count==1&&timer_fences>=2);break;
 case 39: resume_cycle(&dev);assert(!reset_count&&!request.fn&&!cal_count&&!ptb_count);break;
 case 40: resume_cycle(&dev);bcm4377_hci_timeout(&h);deliver_work(&b);active_callback_pause=active_running=true;pthread_barrier_init(&active_started,NULL,2);pthread_barrier_init(&active_release,NULL,2);pthread_create(&active_thread,NULL,active_callback,&h);pthread_barrier_wait(&active_started);assert(!request.fn);assert(!bcm4377_suspend(&dev));assert(reset_count==1&&quiesces==2&&!active_running&&!req_locked);break;
 case 41: resume_cycle(&dev);bcm4377_hci_timeout(&h);deliver_work(&b);active_callback_pause=active_running=true;pthread_barrier_init(&active_started,NULL,2);pthread_barrier_init(&active_release,NULL,2);pthread_create(&active_thread,NULL,active_callback,&h);pthread_barrier_wait(&active_started);bcm4377_hci_unregister_dev(&h);assert(reset_count==1&&unregistered==1&&!active_running&&!req_locked);break;
	case 42:
		fixture_wakeup = true;
		resume_cycle(&dev);
		assert(!quiesces && !b.wake_armed && !b.quiesced);
		recover(&b);
		assert(reset_count == 1);
		break;
	case 43:
		resume_cycle(&dev);
		assert(quiesces == 1 && !b.quiesced && !b.wake_armed);
		break;
	case 44:
		resume_cycle(&dev);
		recover(&b);
		recover(&b);
		assert(reset_count == 1 && !rebind_count && b.resume_guarded);
		h.flags &= ~HCI_UP;
		bcm4377_hci_close(&h);
		h.flags |= HCI_UP;
		recover(&b);
		assert(reset_count == 1 && rebind_count == 1);
		break;
	case 45:
		resume_cycle(&dev);
		bcm4377_hci_timeout(&h);
		deliver_work(&b);
		h.flags &= ~HCI_UP;
		bcm4377_hci_close(&h);
		h.flags |= HCI_UP;
		run_request(&h);
		assert(!reset_count && !b.resume_guarded);
		recover(&b);
		assert(rebind_count == 1);
		break;
 default:abort();
 }
	if (atoi(argv[1]) == 6 || atoi(argv[1]) == 44 || atoi(argv[1]) == 45)
		assert(rebind_count == 1);
	else
		assert(!rebind_count);
 assert(!request.fn);printf("case %s passed\n",argv[1]);return 0;
}
