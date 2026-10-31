// SPDX-License-Identifier: GPL-2.0-only
static const struct ave_coproc_regs regs = { .ctrl = 0, .boot_arg = 4, .vcbar = 8 };
static const struct ave_variant variant = { .regs = &regs };
static u64 coproc[4];
static u32 mailbox[16];
static struct ave_dev ave;
static struct device dev;
static struct ave_ctx ctx[2];
static struct v4l2_m2m_ctx m2m[2];

static void init(void)
{
	memset(&ave, 0, sizeof(ave));
	memset(&dev, 0, sizeof(dev));
	memset(ctx, 0, sizeof(ctx));
	memset(m2m, 0, sizeof(m2m));
	memset(coproc, 0, sizeof(coproc));
	memset(mailbox, 0, sizeof(mailbox));
	halts = closes = freed = suspends = resumes = schedules = polls = sleep_ms = 0;
	halt_result = close_result = poll_result = 0;
	map_fail = ctrl_sticky = stopped_for_sleep = false;
	ave.dev = &dev;
	dev.drvdata = &ave;
	ave.variant = &variant;
	ave.coproc = coproc;
	ave.mbox = mailbox;
	ave.text.iova = 0x100000000;
	*(u64 *)((char *)coproc + regs.vcbar) = ave.text.iova | AVE_COPROC_VCBAR_VALID;
	for (unsigned int i = 0; i < 2; i++) {
		ctx[i].ave = &ave;
		ctx[i].fh.m2m_ctx = &m2m[i];
		m2m[i].src.type = 1;
		m2m[i].src.priv = &ctx[i];
		m2m[i].dst.priv = &ctx[i];
	}
}

static void owners(void)
{
	for (unsigned int i = 0; i < 2; i++) {
		ctx[i].sess = calloc(1, sizeof(*ctx[i].sess));
		assert(ctx[i].sess);
		ctx[i].sess->handed = ctx[i].sess->fw_open = true;
		ctx[i].sess->p.slots = 2;
		ave.sessions[i] = &ctx[i];
		dev.refs++;
	}
	ave.fw_state = AVE_FW_RUNNING;
	*(u32 *)((char *)coproc + regs.ctrl) = AVE_COPROC_CTRL_RUN;
	*(u32 *)((char *)coproc + regs.boot_arg) = AVE_COPROC_BOOT_ARG_VALUE;
	ave.ring[0].slots = 4;
	ave.ipc_fw_base = 0x1234;
}

static void buffer_failure(void)
{
	struct vb2_v4l2_buffer input[2][3] = {}, output[2][2] = {};

	init();
	owners();
	for (unsigned int i = 0; i < 2; i++) {
		ctx[i].sess->slot[0].src = &input[i][0];
		/* One idle owner and one with a firmware-owned frame. */
		ctx[i].sess->busy = i;
		ctx[i].ready = i;
		m2m[i].input[0] = &input[i][1];
		m2m[i].input[1] = &input[i][2];
		m2m[i].nsrc = 2;
		m2m[i].output[0] = &output[i][0];
		m2m[i].output[1] = &output[i][1];
		m2m[i].ndst = 2;
	}
	assert(!ave_inject_wedge(&ave, 1));
	assert(ave.fw_state == AVE_FW_BROKEN);
	assert(ctx[0].poll_work.scheduled && ctx[1].poll_work.scheduled);
	/* No runtime recovery is permitted while either owner remains. */
	assert(ave_runtime_suspend(&dev) == -EBUSY);
	assert(!ave.recovery_count && !halts);
	for (unsigned int i = 0; i < 2; i++) {
		ave_poll_work(&ctx[i].poll_work.work);
		assert(ctx[i].failed && !ctx[i].sess && !ave.sessions[i]);
		assert(m2m[i].src.error && m2m[i].dst.error);
		assert(!m2m[i].nsrc && !m2m[i].ndst && !ctx[i].ready);
		assert(!ctx[i].poll_work.scheduled && !ave_job_ready(&ctx[i]));
		for (unsigned int n = 0; n < 3; n++)
			assert(input[i][n].completions == 1 &&
			       input[i][n].state == VB2_BUF_STATE_ERROR);
		for (unsigned int n = 0; n < 2; n++)
			assert(output[i][n].completions == 1 &&
			       output[i][n].state == VB2_BUF_STATE_ERROR);
	}
	assert(!dev.refs && !freed && ave.failed_sessions && !closes);
	/* A QBUF racing the failure is returned instead of silently requeued. */
	{
		struct vb2_v4l2_buffer extra = {};

		extra.vb2_buf.vb2_queue = &m2m[0].src;
		ave_buf_queue(&extra.vb2_buf);
		assert(extra.completions == 1 && extra.state == VB2_BUF_STATE_ERROR);
	}
	*(u32 *)(ave.coproc + regs.boot_arg) = 0;
	assert(!ave_runtime_suspend(&dev));
	assert(readl(ave.coproc + regs.boot_arg) == AVE_COPROC_BOOT_ARG_VALUE);
	assert(ave.fw_state == AVE_FW_RECOVERING && ave.recovery_count == 1);
	assert(!readl(ave.coproc + regs.ctrl));
	mutex_lock(&ave.hw_lock);
	/* CTRL=0 alone is not proof of a power cycle. */
	assert(ave_fw_prepare_restart(&ave) == -EAGAIN && !freed);
	*(u32 *)(ave.coproc + regs.boot_arg) = 0;
	assert(!ave_fw_prepare_restart(&ave));
	mutex_unlock(&ave.hw_lock);
	assert(freed == 2 && !ave.failed_sessions && !ave.inject_wedge);
	assert(ave.fw_state == AVE_FW_HALTED);
	m2m[0].dst.streaming = true;
	ave_stop_streaming(&m2m[0].src);
	assert(ctx[0].failed); /* STREAMOFF one queue cannot clear the failure. */
	m2m[0].dst.streaming = false;
	ave_stop_streaming(&m2m[0].dst);
	assert(!ctx[0].failed);
	m2m[0].src.error = false; /* vb2 clears queue errors on STREAMOFF. */
	{
		struct vb2_v4l2_buffer extra = {};

		extra.vb2_buf.vb2_queue = &m2m[0].src;
		ave_buf_queue(&extra.vb2_buf); /* Before start_streaming on STREAMON. */
		assert(!extra.completions && m2m[0].nsrc == 1);
	}
}

static void sleep_guarantees(void)
{
	init();
	ave.fw_state = AVE_FW_RUNNING;
	halt_result = -ETIMEDOUT;
	assert(ave_suspend(&dev) == -EBUSY);
	assert(suspends == 1 && resumes == 1 && stopped_for_sleep);
	assert(ave.fw_state == AVE_FW_BROKEN && !ave.recovery_count);
	assert(ave_suspend(&dev) == -EBUSY && halts == 1);
	assert(suspends == 2 && resumes == 2);
	ave.fw_state = AVE_FW_RECOVERING;
	assert(ave_suspend(&dev) == -EBUSY);
	assert(resumes == 3);
	ave.fw_state = AVE_FW_RUNNING;
	halt_result = 0;
	assert(!ave_suspend(&dev));
	assert(ave.fw_state == AVE_FW_HALTED && resumes == 3);
	assert(!ave_resume(&dev) && resumes == 4);
}

static void recovery_failures(void)
{
	init();
	jiffies = 100;
	for (unsigned int i = 0; i < AVE_RECOVERY_ATTEMPTS; i++) {
		ave.fw_state = AVE_FW_BROKEN;
		ave.ipc_fw_base = 1;
		ave.ring[0].slots = 4;
		halt_result = -ETIMEDOUT;
		assert(!ave_runtime_suspend(&dev));
		assert(ave.fw_state == AVE_FW_RECOVERING);
	}
	assert(halts == 3 && ave.recovery_count == 3);
	ave.fw_state = AVE_FW_BROKEN;
	assert(ave_runtime_suspend(&dev) == -EBUSY && halts == 3);
	jiffies += AVE_RECOVERY_WINDOW;
	ctrl_sticky = true;
	*(u32 *)(ave.coproc + regs.ctrl) = AVE_COPROC_CTRL_RUN;
	assert(ave_runtime_suspend(&dev) == -EIO);
	assert(ave.fw_state == AVE_FW_BROKEN && ave.recovery_count == 4);
	ctrl_sticky = false;
	assert(!ave_runtime_suspend(&dev));
	assert(!ave_runtime_suspend(&dev)); /* Does not count the PM retry again. */
	assert(ave.recovery_count == 5);
}

static bool never_answered(struct ave_dev *dev, void *arg) { return false; }
static void busy_ring_deadline(void)
{
	init();
	owners();
	poll_result = 1; /* A continuous stream of unrelated acks/messages. */
	mutex_lock(&ave.hw_lock);
	assert(ave_ipc_pump(&ave, 5, never_answered, NULL) == -ETIMEDOUT);
	mutex_unlock(&ave.hw_lock);
	assert(polls < 10 && ave.fw_state == AVE_FW_BROKEN);
	assert(ctx[0].poll_work.scheduled && ctx[1].poll_work.scheduled);
	ave_poll_work(&ctx[0].poll_work.work);
	ave_poll_work(&ctx[1].poll_work.work);
	mutex_lock(&ave.hw_lock);
	ave_sess_reap(&ave); /* Fake reset for test cleanup only. */
	mutex_unlock(&ave.hw_lock);
}

static void restore_data(void)
{
	u8 pristine[64], live[64];

	init();
	memset(pristine, 0xa5, sizeof(pristine));
	memset(live, 0x3c, sizeof(live));
	ave.data_pristine = pristine;
	ave.data.phys = (phys_addr_t)live;
	ave.data.size = sizeof(live);
	ave.data_dirty = true;
	map_fail = true;
	assert(ave_fw_restore_data(&ave) == -ENOMEM && ave.data_dirty);
	map_fail = false;
	assert(!ave_fw_restore_data(&ave) && !ave.data_dirty);
	assert(!memcmp(live, pristine, sizeof(live)));
	assert(!ave_fw_restore_data(&ave));
}

int main(void)
{
	buffer_failure();
	sleep_guarantees();
	recovery_failures();
	busy_ring_deadline();
	restore_data();
	puts("ok: recovery ownership, queues, quarantine, reset proof, rate limit, "
	     "PM rollback, DATA, IPC deadline");
	return 0;
}
