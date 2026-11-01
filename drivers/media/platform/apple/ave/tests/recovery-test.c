// SPDX-License-Identifier: GPL-2.0-only
static const struct ave_coproc_regs regs = { .ctrl = 0, .boot_arg = 4, .vcbar = 8 };
static const struct ave_variant variant = {
 .regs = &regs, .vcbar_mask = ~0ULL, .vcbar_flags = AVE_COPROC_VCBAR_VALID,
};
static const struct ave_fw_transport test_transport = { .boot_config = true };
static const struct ave_fw_abi test_abi = { .transport = &test_transport };
static const struct ave_fw_layout test_layout = {
	.abi = &test_abi, .encode_ready = true, .max_slots = 2,
};
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
	stop_seen = power_seen = dma_alloc_fail = dma_attach_fail = dma_map_fail = false;
	mapping_frees = mapping_unmaps = mapping_detaches = 0;
	submit_result = 0;
	test_ctrl_addr = (char *)coproc + regs.ctrl;
	ave.dev = &dev;
	dev.drvdata = &ave;
	ave.variant = &variant;
	ave.layout = &test_layout;
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
		ctx[i].sess->p.codec = AVE_CODEC_H264;
		ctx[i].sess->hdr.cpu = (void *)1;
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
	struct dma_buf imported = { .refs = 1 };

	init();
	owners();
	for (unsigned int i = 0; i < 2; i++) {
		struct vb2_buffer *vb = &input[i][0].vb2_buf;

		vb->planes[0].mem_priv = i ?
			ave_vb2_memops.attach_dmabuf(vb, &dev, &imported, SZ_16K) :
			ave_vb2_memops.alloc(vb, &dev, SZ_16K);
		assert(!IS_ERR(vb->planes[0].mem_priv));
		if (i)
			assert(!ave_vb2_memops.map_dmabuf(vb->planes[0].mem_priv));
		submit_ctx = &ctx[i];
		submit_slot = 0;
		mutex_lock(&ave.hw_lock);
		assert(!ave_cmd_process(&ctx[i], 0, &input[i][0], 0, 0, false));
		mutex_unlock(&ave.hw_lock);
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
		/* DQBUF/REQBUFS/close immediately after ERROR: vb2 lets go. */
		if (i) {
			ave_vb2_memops.unmap_dmabuf(input[i][0].vb2_buf.planes[0].mem_priv);
			ave_vb2_memops.detach_dmabuf(input[i][0].vb2_buf.planes[0].mem_priv);
		} else {
			ave_vb2_memops.put(input[i][0].vb2_buf.planes[0].mem_priv);
		}
		input[i][0].vb2_buf.planes[0].mem_priv = NULL;
		assert(!mapping_frees && !mapping_unmaps && !mapping_detaches);
	}
	/* Even dropping the importing fd cannot destroy its retained attachment. */
	dma_buf_put(&imported);
	assert(imported.refs == 1 && dev.dma_refs == 1);
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
	power_seen = true; /* VENC and its DART have now lost power. */
	assert(!ave_fw_prepare_restart(&ave));
	mutex_unlock(&ave.hw_lock);
	assert(freed == 2 && !ave.failed_sessions && !ave.inject_wedge);
	assert(stop_seen && mapping_frees == 2 && mapping_unmaps == 1 && mapping_detaches == 1);
	assert(!imported.refs && !dev.dma_refs);
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
	stop_seen = power_seen = true;
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

static void strict_6070_recovery(void)
{
 static const struct ave_fw_transport transport = { .scratch_halt = true };
 static const struct ave_fw_abi abi = { .transport = &transport };
 static const struct ave_fw_layout layout = { .abi = &abi };

 init();
 ave.layout = &layout;
 ave.fw_state = AVE_FW_BROKEN;
 *(u32 *)(ave.coproc + regs.ctrl) = AVE_COPROC_CTRL_RUN;
 assert(ave_runtime_suspend(&dev) == -EBUSY);
 assert(ave.fw_state == AVE_FW_BROKEN && !halts && !ave.recovery_count);
 assert(readl(ave.coproc + regs.ctrl) == AVE_COPROC_CTRL_RUN);
 ave.fw_state = AVE_FW_RECOVERING;
 assert(ave_runtime_suspend(&dev) == -EBUSY);
 assert(readl(ave.coproc + regs.ctrl) == AVE_COPROC_CTRL_RUN);
}

/* An uncertain send failure must keep its pin even without a slot src. */
static void failed_submission(void)
{
	struct vb2_v4l2_buffer input = {};
	void *mem;

	init();
	owners();
	mem = ave_vb2_memops.alloc(&input.vb2_buf, &dev, SZ_16K);
	assert(!IS_ERR(mem));
	input.vb2_buf.planes[0].mem_priv = mem;
	submit_ctx = &ctx[0];
	submit_slot = 0;
	submit_result = -EIO;
	mutex_lock(&ave.hw_lock);
	assert(ave_cmd_process(&ctx[0], 0, &input, 0, 0, false) == -EIO);
	mutex_unlock(&ave.hw_lock);
	assert(!ctx[0].sess->slot[0].src && ctx[0].sess->slot[0].input_mem);
	ave_vb2_memops.put(mem); /* The caller returns this failed submission. */
	ave_poll_work(&ctx[0].poll_work.work);
	ave_poll_work(&ctx[1].poll_work.work);
	assert(!mapping_frees);
	for (unsigned int i = 0; i < AVE_RECOVERY_ATTEMPTS; i++)
		assert(ave_recovery_allow(&ave.recovery, jiffies));
	assert(ave_runtime_suspend(&dev) == -EBUSY);
	assert(!stop_seen && !mapping_frees && !freed);
	jiffies += AVE_RECOVERY_WINDOW;
	ctrl_sticky = true;
	assert(ave_runtime_suspend(&dev) == -EIO);
	assert(!stop_seen && !mapping_frees && !freed);
	ctrl_sticky = false;
	assert(!ave_runtime_suspend(&dev));
	assert(stop_seen && !mapping_frees);
	assert(ave_fw_prepare_restart(&ave) == -EAGAIN);
	*(u32 *)(ave.coproc + regs.boot_arg) = 0;
	power_seen = true;
	mutex_lock(&ave.hw_lock);
	assert(!ave_fw_prepare_restart(&ave));
	mutex_unlock(&ave.hw_lock);
	assert(mapping_frees == 1 && freed == 2);
}

/* Start can expose scratch even if submission reports failure. */
static void failed_start(void)
{
	init();
	owners();
	submit_ctx = &ctx[0];
	ctx[0].sess->handed = false;
	submit_result = -EIO;
	mutex_lock(&ave.hw_lock);
	assert(ave_cmd_start(&ctx[0]) == -EIO);
	assert(ctx[0].sess->handed);
	mutex_unlock(&ave.hw_lock);
	ave_poll_work(&ctx[0].poll_work.work);
	ave_poll_work(&ctx[1].poll_work.work);
	assert(!freed && ave.failed_sessions);
	assert(!ave_runtime_suspend(&dev));
	assert(stop_seen && !freed);
	*(u32 *)(ave.coproc + regs.boot_arg) = 0;
	power_seen = true;
	mutex_lock(&ave.hw_lock);
	assert(!ave_fw_prepare_restart(&ave));
	mutex_unlock(&ave.hw_lock);
	assert(freed == 2);
}

/* Allocator failures and normal vb2-only lifetimes do not leak references. */
static void mapping_lifetimes(void)
{
	struct vb2_buffer vb = {};
	struct dma_buf dbuf = { .refs = 1 };
	void *mem;

	init();
	dma_alloc_fail = true;
	assert(IS_ERR(ave_vb2_memops.alloc(&vb, &dev, SZ_16K)));
	dma_alloc_fail = false;
	dma_attach_fail = true;
	assert(IS_ERR(ave_vb2_memops.attach_dmabuf(&vb, &dev, &dbuf, SZ_16K)));
	assert(dbuf.refs == 1 && !dev.dma_refs);
	dma_attach_fail = false;
	mem = ave_vb2_memops.attach_dmabuf(&vb, &dev, &dbuf, SZ_16K);
	assert(!IS_ERR(mem));
	dma_map_fail = true;
	assert(ave_vb2_memops.map_dmabuf(mem) == -EIO);
	dma_map_fail = false;
	assert(!ave_vb2_memops.map_dmabuf(mem));
	ave_vb2_memops.unmap_dmabuf(mem);
	assert(!mapping_unmaps);
	assert(!ave_vb2_memops.map_dmabuf(mem)); /* Reuses the retained mapping. */
	ave_vb2_memops.unmap_dmabuf(mem);
	ave_vb2_memops.detach_dmabuf(mem);
	assert(mapping_unmaps == 1 && mapping_detaches == 1 && mapping_frees == 1);
	assert(dbuf.refs == 1 && !dev.dma_refs);
}

/*
 * MCTF DMA memory is the device pool: a session borrows it and gives it
 * back; no teardown path (normal, failed, reaped) frees or unmaps it, and
 * the pool is lent to one session at a time.
 */
static void mctf_pool_lifetime(void)
{
	static char pool_mem[4];
	struct ave_session *sess;

	init();
	power_seen = stop_seen = false;
	ave.mctf_pool_flags = 1UL << AVE_MCTF_POOL_READY | 1UL << AVE_MCTF_POOL_LENT;
	sess = calloc(1, sizeof(*sess));
	assert(sess);
	sess->mctf_borrowed = true;
	sess->handed = true;
	sess->mctf_buf[0].cpu = &pool_mem[0];
	sess->mctf_buf[0].dma = 0x10000004000ULL;
	sess->mctf_in[0].cpu = &pool_mem[1];
	sess->mctf_out[1].cpu = &pool_mem[2];
	ctx[0].sess = sess;
	/* Normal STREAMOFF teardown with a running firmware: nothing freed. */
	ave.fw_state = AVE_FW_RUNNING;
	ave_sess_destroy(&ctx[0]);
	assert(!freed && !ctx[0].sess);
	assert(ave.mctf_pool_flags == 1UL << AVE_MCTF_POOL_READY);

	/* A failed session is quarantined holding the pool; reaping returns it unfreed. */
	sess = calloc(1, sizeof(*sess));
	assert(sess);
	sess->mctf_borrowed = true;
	sess->handed = true;
	sess->mctf_buf[3].cpu = &pool_mem[3];
	ave.mctf_pool_flags |= 1UL << AVE_MCTF_POOL_LENT;
	ctx[0].sess = sess;
	ave.fw_state = AVE_FW_BROKEN;
	ave_sess_destroy(&ctx[0]);
	assert(ave.failed_sessions == sess && !freed);
	assert(ave.mctf_pool_flags & 1UL << AVE_MCTF_POOL_LENT);
	stop_seen = power_seen = true;
	mutex_lock(&ave.hw_lock);
	ave_sess_reap(&ave);
	mutex_unlock(&ave.hw_lock);
	assert(!ave.failed_sessions && !freed);
	assert(ave.mctf_pool_flags == 1UL << AVE_MCTF_POOL_READY);
}

int main(void)
{
	mctf_pool_lifetime();
	mapping_lifetimes();
	failed_submission();
	failed_start();
	strict_6070_recovery();
	buffer_failure();
	sleep_guarantees();
	recovery_failures();
	busy_ring_deadline();
	restore_data();
	puts("ok: recovery ownership, queues, quarantine, reset proof, rate limit, "
	     "PM rollback, DATA, IPC deadline, MMAP/dma-buf release ordering, "
	     "MCTF pool lifetime");
	return 0;
}
