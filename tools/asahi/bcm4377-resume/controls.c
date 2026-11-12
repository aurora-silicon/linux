// SPDX-License-Identifier: GPL-2.0-only
static bool timeout_during_resume;
static int resume_frames;
static char bars[2];

static int send(struct hci_dev *h, int type)
{
	unsigned char data[4] = {};
	struct sk_buff skb = { .pkt_type = type, .data = data, .len = sizeof(data) };

	return bcm4377_hci_send_frame(h, &skb);
}

/* The core sends its resume commands through the driver and carries on after one that fails. */
static int hci_resume_sync(struct hci_dev *h)
{
	int i;

	if (timeout_during_resume)
		hci_cmd_timeout(&h->cmd_timer.work);
	for (i = 0; i < resume_frames; i++)
		send(h, HCI_COMMAND_PKT);
	return resume_error;
}

/*
 * The sleep control register answers through BAR0. The firmware's status words are only
 * there through the windows the driver sets, never through the ones a sleep reset.
 */
static int ioread32(void *addr)
{
	struct bcm4377_data *b = active;
	char *vars = b->bar2 + b->hw->bar2_offset;

	if (addr == b->bar0 + BCM4377_BAR0_SLEEP_CONTROL)
		return 0;
	assert(!memcmp(cfg, cfg_probe, sizeof(cfg)));
	if (addr == vars + BCM4377_BAR2_BOOTSTAGE)
		return fw_bootstage;
	assert(addr == vars + BCM4377_BAR2_RTI_STATUS);
	return fw_rti_status;
}

/* What probe leaves in the function's private windows. */
static void probe_cfg(struct bcm4377_data *b)
{
	memset(cfg, 0, sizeof(cfg));
	assert(!bcm4377_init_cfg(b));
	memcpy(cfg_probe, cfg, sizeof(cfg));
	cfg_reads = 0;
	cfg_writes = 0;
}

/* A sleep that takes the function's power: a J616s reads window 1 as 0x19328000 after it. */
static void power_loss(void)
{
	memset(cfg, 0, sizeof(cfg));
	cfg[BCM4377_PCIECFG_BAR0_WINDOW1 / 4] = 0x19328000;
	fw_bootstage = 0;
	fw_rti_status = 0;
}

static int cold_cycle(struct device *dev)
{
	assert(!bcm4377_suspend(dev));
	power_loss();
	return bcm4377_resume(dev);
}

static void j616s(struct bcm4377_hw *hw, struct bcm4377_data *b)
{
	board_j493 = false;
	board_j616s = true;
	*hw = bcm4377_hw_variants[BCM4388];
	probe_cfg(b);
}

static int power_on(struct hci_dev *h)
{
	int ret;

	req_locked = true;
	ret = hci_dev_open_sync(h);
	req_locked = false;
	return ret;
}

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
	b.bar0 = bars;
	b.bar2 = bars + 1;
	probe_cfg(&b);
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
	/*
	 * J616s: the BCM4388 is back from the sleep with its windows reset and no firmware.
	 * Nothing is sent to it, and the resume itself queues the recovery, once.
	 */
	case 46:
		j616s(&hw, &b);
		resume_frames = 2;
		assert(!cold_cycle(&dev));
		assert(!unquiesces && !b.quiesced);
		assert(b.needs_reset && b.resume_cold && !memcmp(cfg, cfg_probe, sizeof(cfg)));
		assert(h.stat.err_tx == 2 && !h.stat.cmd_tx && !enqueued && !skbs_freed);
		assert(b.resume_work.queues == 1 && !reset_count && !request.fn);
		hci_cmd_timeout(&h.cmd_timer.work);
		assert(b.resume_work.queues == 1);
		deliver_work(&b);
		run_request(&h);
		assert(reset_count == 1 && cal_count == 1 && ptb_count == 1 && addr_count == 1);
		assert(!bacmp(&h.bdaddr, &own) && (h.flags & HCI_UP) && !b.cal_needed);
		assert(!b.needs_reset && !b.resume_cold);
		assert(!send(&h, HCI_COMMAND_PKT) && enqueued == 1 && skbs_freed == 1);
		recover(&b);
		assert(reset_count == 1);
		break;
	/* A cold resume succeeds whatever the core's resume returns; a warm one reports it. */
	case 47:
		j616s(&hw, &b);
		resume_error = -EIO;
		assert(!cold_cycle(&dev));
		assert(b.resume_work.queues == 1);
		deliver_work(&b);
		run_request(&h);
		assert(reset_count == 1 && (h.flags & HCI_UP));
		assert(!bcm4377_suspend(&dev));
		assert(bcm4377_resume(&dev) == -EIO);
		assert(unquiesces == 1 && !b.resume_cold && b.resume_work.queues == 1);
		break;
	/* A controller that was not powered gets no recovery: its next open resets the function. */
	case 48:
		j616s(&hw, &b);
		h.flags = 0;
		assert(!cold_cycle(&dev));
		assert(b.needs_reset && b.resume_cold);
		assert(!unquiesces && !b.resume_work.queues);
		recover(&b);
		assert(!reset_count && send(&h, HCI_COMMAND_PKT) == -ENODEV);
		assert(!power_on(&h));
		assert(reset_count == 1 && cal_count == 1 && addr_count == 1 && (h.flags & HCI_UP));
		assert(!b.resume_cold && !send(&h, HCI_COMMAND_PKT) && enqueued == 1);
		break;
	/* A command timeout inside the resume takes the recovery; the resume queues no second. */
	case 49:
		j616s(&hw, &b);
		timeout_during_resume = true;
		assert(!cold_cycle(&dev));
		assert(b.resume_work.queues == 1);
		deliver_work(&b);
		run_request(&h);
		assert(reset_count == 1 && !b.resume_cold);
		recover(&b);
		assert(reset_count == 1);
		break;
	/* Windows the sleep reset over a firmware that still runs: put back, then as before. */
	case 50:
		j616s(&hw, &b);
		resume_frames = 2;
		assert(!bcm4377_suspend(&dev));
		power_loss();
		fw_bootstage = 2;
		fw_rti_status = 2;
		assert(!bcm4377_resume(&dev));
		assert(unquiesces == 1 && !misdirected && !b.needs_reset && !b.resume_cold);
		assert(!b.resume_work.queues && enqueued == 2 && !h.stat.err_tx);
		recover(&b);
		assert(reset_count == 1);
		break;
	/* Windows the sleep left alone: nothing is written, resume and recovery as before. */
	case 51:
		j616s(&hw, &b);
		resume_frames = 2;
		resume_cycle(&dev);
		assert(!cfg_writes && unquiesces == 1 && !b.needs_reset && !b.resume_cold);
		assert(!b.resume_work.queues && enqueued == 2 && !h.stat.err_tx);
		recover(&b);
		assert(reset_count == 1 && cal_count == 1 && addr_count == 1);
		break;
	/* Either status word alone says no firmware runs, and each such sleep is recovered. */
	case 52:
		j616s(&hw, &b);
		assert(!bcm4377_suspend(&dev));
		power_loss();
		fw_bootstage = 2;
		assert(!bcm4377_resume(&dev));
		assert(b.resume_cold && !unquiesces);
		deliver_work(&b);
		run_request(&h);
		assert(!bcm4377_suspend(&dev));
		power_loss();
		fw_rti_status = 2;
		assert(!bcm4377_resume(&dev));
		assert(b.resume_cold && !unquiesces);
		deliver_work(&b);
		run_request(&h);
		assert(reset_count == 2 && cal_count == 2 && addr_count == 2);
		assert(b.resume_work.queues == 2 && !b.resume_cold);
		break;
	/* The J493 pair takes the same path. */
	case 53:
		assert(!cold_cycle(&dev));
		assert(!unquiesces && b.resume_cold && b.resume_work.queues == 1);
		deliver_work(&b);
		run_request(&h);
		assert(reset_count == 1 && cal_count == 1 && ptb_count == 1 && addr_count == 1);
		assert((h.flags & HCI_UP) && !b.resume_cold);
		break;
	/* Any other chip and board pair is not looked at: its resume is the one before. */
	case 54:
		hw = bcm4377_hw_variants[BCM4388];
		probe_cfg(&b);
		assert(!cold_cycle(&dev));
		assert(!cfg_reads && !cfg_writes && unquiesces == 1);
		assert(!b.needs_reset && !b.resume_cold && !send(&h, HCI_COMMAND_PKT));
		board_j493 = false;
		board_j616s = true;
		hw = bcm4377_hw_variants[BCM4378];
		probe_cfg(&b);
		assert(!cold_cycle(&dev));
		assert(!cfg_reads && !cfg_writes && unquiesces == 2);
		assert(!b.needs_reset && !b.resume_cold && !send(&h, HCI_COMMAND_PKT));
		recover(&b);
		assert(!reset_count && !b.resume_work.queues);
		break;
	/* Windows that cannot be written back: no status word is read, the function is reset. */
	case 55:
		j616s(&hw, &b);
		assert(!bcm4377_suspend(&dev));
		power_loss();
		cfg_error = -EIO;
		assert(!bcm4377_resume(&dev));
		cfg_error = 0;
		assert(!unquiesces && b.needs_reset && b.resume_cold && b.resume_work.queues == 1);
		deliver_work(&b);
		run_request(&h);
		assert(reset_count == 1 && !b.resume_cold);
		break;
	/* The next suspend fences the recovery a cold resume queued, as work and as request. */
	case 56:
		j616s(&hw, &b);
		assert(!cold_cycle(&dev));
		assert(b.resume_work.queued);
		assert(!cold_cycle(&dev));
		assert(!request.fn && b.resume_work.queues == 2);
		deliver_work(&b);
		assert(request.fn);
		assert(!cold_cycle(&dev));
		assert(!request.fn && b.resume_work.queues == 3 && quiesces == 3 && !unquiesces);
		assert(!reset_count && b.resume_cold);
		deliver_work(&b);
		run_request(&h);
		assert(reset_count == 1 && !b.resume_cold && (h.flags & HCI_UP));
		break;
	/* A power-off before the queued recovery runs invalidates it; the next open resets. */
	case 57:
		j616s(&hw, &b);
		assert(!cold_cycle(&dev));
		deliver_work(&b);
		assert(request.fn);
		h.flags &= ~HCI_UP;
		bcm4377_hci_close(&h);
		run_request(&h);
		assert(!reset_count && b.resume_cold && b.needs_reset);
		assert(!power_on(&h));
		assert(reset_count == 1 && !b.resume_cold && (h.flags & HCI_UP));
		break;
	/* The send callback: each frame reaches its ring, and none does while cold. */
	case 58:
		j616s(&hw, &b);
		assert(!send(&h, HCI_COMMAND_PKT) && enqueued_ring == &b.hci_h2d_ring);
		assert(!send(&h, HCI_ACLDATA_PKT) && enqueued_ring == &b.acl_h2d_ring);
		assert(!send(&h, HCI_SCODATA_PKT) && enqueued_ring == &b.sco_h2d_ring);
		assert(enqueued == 3 && skbs_freed == 3 && h.stat.byte_tx == 12 && !h.stat.err_tx);
		assert(h.stat.cmd_tx == 1 && h.stat.acl_tx == 1 && h.stat.sco_tx == 1);
		assert(!cold_cycle(&dev));
		assert(send(&h, HCI_COMMAND_PKT) == -ENODEV);
		assert(send(&h, HCI_ACLDATA_PKT) == -ENODEV);
		assert(send(&h, HCI_SCODATA_PKT) == -ENODEV);
		/* The core frees a frame the driver refused. */
		assert(enqueued == 3 && skbs_freed == 3 && h.stat.byte_tx == 12);
		assert(h.stat.err_tx == 3 && h.stat.cmd_tx == 1 && h.stat.acl_tx == 1);
		assert(h.stat.sco_tx == 1);
		deliver_work(&b);
		run_request(&h);
		assert(!send(&h, HCI_COMMAND_PKT) && enqueued == 4 && skbs_freed == 4);
		break;
	/* A reset that fails leaves the function cold; the one that succeeds ends it. */
	case 59:
		j616s(&hw, &b);
		assert(!cold_cycle(&dev));
		reset_error = -EIO;
		deliver_work(&b);
		run_request(&h);
		assert(reset_count == 1 && !(h.flags & HCI_UP) && b.needs_reset && b.resume_cold);
		reset_error = 0;
		assert(!power_on(&h));
		assert(reset_count == 2 && !b.resume_cold && (h.flags & HCI_UP));
		assert(!send(&h, HCI_COMMAND_PKT));
		break;
	/* Armed for wake: no QUIESCE went out, and the cold resume is the same. */
	case 60:
		j616s(&hw, &b);
		fixture_wakeup = true;
		assert(!cold_cycle(&dev));
		assert(!quiesces && !unquiesces && !b.wake_armed);
		assert(b.resume_cold && b.resume_work.queues == 1);
		deliver_work(&b);
		run_request(&h);
		assert(reset_count == 1 && !b.resume_cold);
		break;
	/* A sleep that leaves the windows alone does not end the cold state an earlier one left. */
	case 61:
		j616s(&hw, &b);
		assert(!cold_cycle(&dev));
		assert(b.resume_work.queued);
		resume_cycle(&dev);
		assert(b.resume_cold && b.needs_reset && !unquiesces);
		assert(b.resume_work.queues == 2);
		deliver_work(&b);
		run_request(&h);
		assert(reset_count == 1 && !b.resume_cold && (h.flags & HCI_UP));
		break;
	/* A setup retry the suspend held back is queued again by a cold resume too. */
	case 62:
		j616s(&hw, &b);
		h.flags = HCI_SETUP | HCI_AUTO_OFF;
		cal_failures = 1;
		assert(bcm4377_hci_setup(&h) == -ETIMEDOUT);
		bcm4377_hci_close(&h);
		assert(b.setup_retry_pending && b.setup_retry_work.work.queued);
		assert(!cold_cycle(&dev));
		assert(b.resume_cold && !b.resume_work.queues);
		assert(b.setup_retry_pending && b.setup_retry_work.work.queued);
		break;
 default:abort();
 }
	if (atoi(argv[1]) == 6 || atoi(argv[1]) == 44 || atoi(argv[1]) == 45)
		assert(rebind_count == 1);
	else
		assert(!rebind_count);
 assert(!request.fn);printf("case %s passed\n",argv[1]);return 0;
}
