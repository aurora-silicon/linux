// SPDX-License-Identifier: (GPL-2.0-only OR MIT) AND GPL-2.0-only
/*
 * Apple SMC (System Management Controller) MFD driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <kunit/static_stub.h>

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/math.h>
#include <linux/mfd/core.h>
#include <linux/mfd/macsmc.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/sleep.h>
#include <linux/suspend.h>
#include <clocksource/arm_arch_timer.h>
#include <asm/arch_timer.h>
#include <linux/unaligned.h>

#define SMC_ENDPOINT			0x20

/* We don't actually know the true size here but this seem reasonable */
#define SMC_SHMEM_SIZE			0x1000
#define SMC_MAX_SIZE			255

#define SMC_MSG_READ_KEY		0x10
#define SMC_MSG_WRITE_KEY		0x11
#define SMC_MSG_GET_KEY_BY_INDEX	0x12
#define SMC_MSG_GET_KEY_INFO		0x13
#define SMC_MSG_INITIALIZE		0x17
#define SMC_MSG_NOTIFICATION		0x18
#define SMC_MSG_RW_KEY			0x20

#define SMC_DATA			GENMASK_ULL(63, 32)
#define SMC_WSIZE			GENMASK_ULL(31, 24)
#define SMC_SIZE			GENMASK_ULL(23, 16)
#define SMC_ID				GENMASK_ULL(15, 12)
#define SMC_MSG				GENMASK_ULL(7, 0)
#define SMC_RESULT			SMC_MSG

#define SMC_TIMEOUT_MS		500
/* Restart and power-off go through atomic writes; give the SMC time. */
#define SMC_ATOMIC_TIMEOUT_MS	5000

static const struct mfd_cell apple_smc_devs[] = {
	MFD_CELL_NAME("macsmc-input"),
	MFD_CELL_NAME("macsmc-power"),
	MFD_CELL_OF("macsmc-gpio", NULL, NULL, 0, 0, "apple,smc-gpio"),
	MFD_CELL_OF("macsmc-hwmon", NULL, NULL, 0, 0, "apple,smc-hwmon"),
	MFD_CELL_OF("macsmc-reboot", NULL, NULL, 0, 0, "apple,smc-reboot"),
	MFD_CELL_OF("macsmc-rtc", NULL, NULL, 0, 0, "apple,smc-rtc"),
};

/* This profile exposes notifications and the explicitly described outputs. */
static const struct mfd_cell apple_smc_t6050_devs[] = {
	MFD_CELL_NAME("macsmc-input"),
	MFD_CELL_NAME("macsmc-power"),
	MFD_CELL_OF("macsmc-hwmon", NULL, NULL, 0, 0, "apple,smc-hwmon"),
};

/*
 * Cells registered only where the device tree describes them: the MFD core would otherwise
 * create an unbound device and warn that it found no node on every SMC without one. The pcIO
 * outputs and the second GPIO bank ('gp' keys) only exist on some SMCs.
 */
static const struct mfd_cell apple_smc_described_devs[] = {
	MFD_CELL_OF("macsmc-gpio", NULL, NULL, 0, 0, "apple,smc-pcio-gpio"),
	MFD_CELL_OF("macsmc-low-gpio", NULL, NULL, 0, 0, "apple,smc-low-gpio"),
};

/*
 * T6050's reboot/power-off and RTC, for a kernel that owns the machine (J775D, booted bare
 * metal). Registered only where the device tree describes them, so a T6050 device tree without
 * them registers what it did before.
 */
static const struct mfd_cell apple_smc_t6050_described_devs[] = {
	MFD_CELL_OF("macsmc-reboot", NULL, NULL, 0, 0, "apple,smc-reboot"),
	MFD_CELL_OF("macsmc-rtc", NULL, NULL, 0, 0, "apple,smc-rtc"),
};

static int apple_smc_add_described_devices(struct apple_smc *smc, const struct mfd_cell *cells,
					   size_t count)
{
	struct device_node *np;
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		np = of_get_compatible_child(smc->dev->of_node, cells[i].of_compatible);
		if (!np)
			continue;
		of_node_put(np);
		ret = devm_mfd_add_devices(smc->dev, PLATFORM_DEVID_NONE, &cells[i], 1,
					   NULL, 0, NULL);
		if (ret)
			return dev_err_probe(smc->dev, ret, "Failed to register %s\n",
					     cells[i].name);
	}

	return 0;
}

/* T8152 currently exposes read-only telemetry; firmware owns fan control. */
static const struct mfd_cell apple_smc_telemetry_devs[] = {
	MFD_CELL_OF("macsmc-hwmon", NULL, NULL, 0, 0, "apple,smc-hwmon"),
};

static void apple_smc_read_sram(struct apple_smc *smc, void *buf, size_t size)
{
	if (IS_ENABLED(CONFIG_MFD_MACSMC_KUNIT_TEST))
		KUNIT_STATIC_STUB_REDIRECT(apple_smc_read_sram, smc, buf, size);
	memcpy_fromio(buf, smc->shmem.iomem, size);
}

/* In a noirq phase the mailbox's interrupt is off, so its reply has to be fetched. */
static bool apple_smc_reply_arrived(struct apple_smc *smc)
{
	apple_rtkit_poll(smc->rtk);

	return completion_done(&smc->cmd_done);
}

/* Must precede SRAM writes as well as command publication. Other profiles
 * retain their original method; this is not permanent reboot atomic_mode.
 */
static int apple_smc_saved_guard(struct apple_smc *smc)
{
	if (!smc->saved_enabled)
		return 0;
	if (READ_ONCE(smc->saved_fault))
		return READ_ONCE(smc->saved_fault);
	if (smc->saved_blocked || smp_load_acquire(&smc->saved_pending) || smc->saved_recovery)
		return -EBUSY;
	if (smc->ap_notified)
		return -EAGAIN;
	return 0;
}

static int apple_smc_cmd_locked(struct apple_smc *smc, u64 cmd, u64 arg,
				  u64 size, u64 wsize, u32 *ret_data)
{
	u8 result;
	int ret;
	u64 msg;

	lockdep_assert_held(&smc->mutex);
	if (IS_ENABLED(CONFIG_MFD_MACSMC_KUNIT_TEST))
		KUNIT_STATIC_STUB_REDIRECT(apple_smc_cmd_locked, smc, cmd, arg, size, wsize,
					   ret_data);

	ret = apple_smc_saved_guard(smc);
	if (ret)
		return ret;
	if (smc->boot_stage != APPLE_SMC_INITIALIZED)
		return -EIO;
	if (smc->atomic_mode)
		return -EIO;
	/* Between the AP power notices the firmware expects no commands. */
	if (smc->ap_notified)
		return -EAGAIN;

	reinit_completion(&smc->cmd_done);

	smc->msg_id = (smc->msg_id + 1) & 0xf;
	msg = (FIELD_PREP(SMC_MSG, cmd) |
	       FIELD_PREP(SMC_SIZE, size) |
	       FIELD_PREP(SMC_WSIZE, wsize) |
	       FIELD_PREP(SMC_ID, smc->msg_id) |
	       FIELD_PREP(SMC_DATA, arg));

	if (smc->saved_enabled)
		smp_store_release(&smc->saved_pending, true);
	ret = apple_rtkit_send_message(smc->rtk, SMC_ENDPOINT, msg, NULL, false);
	if (ret) {
		if (smc->saved_enabled)
			smp_store_release(&smc->saved_pending, false); /* definitely never posted */
		dev_err(smc->dev, "Failed to send command\n");
		return ret;
	}

	if (smc->polled) {
		bool done;

		ret = read_poll_timeout(apple_smc_reply_arrived, done, done, 100,
					SMC_TIMEOUT_MS * USEC_PER_MSEC, false, smc);
	} else {
		ret = wait_for_completion_timeout(&smc->cmd_done,
						  msecs_to_jiffies(SMC_TIMEOUT_MS)) > 0 ?
		      0 : -ETIMEDOUT;
	}
	if (ret) {
		dev_err(smc->dev, "Command timed out (%llx)", msg);
		return -ETIMEDOUT;
	}

	if (FIELD_GET(SMC_ID, smc->cmd_ret) != smc->msg_id) {
		dev_err(smc->dev, "Command sequence mismatch (expected %d, got %d)\n",
			smc->msg_id, (unsigned int)FIELD_GET(SMC_ID, smc->cmd_ret));
		return -EIO;
	}

	result = FIELD_GET(SMC_RESULT, smc->cmd_ret);
	if (result)
		return -EIO;

	if (ret_data)
		*ret_data = FIELD_GET(SMC_DATA, smc->cmd_ret);

	return FIELD_GET(SMC_SIZE, smc->cmd_ret);
}

static int apple_smc_cmd(struct apple_smc *smc, u64 cmd, u64 arg,
			 u64 size, u64 wsize, u32 *ret_data)
{
	guard(mutex)(&smc->mutex);

	return apple_smc_cmd_locked(smc, cmd, arg, size, wsize, ret_data);
}

static int apple_smc_rw_locked(struct apple_smc *smc, smc_key key,
				const void *wbuf, size_t wsize,
				void *rbuf, size_t rsize)
{
	u64 smc_size, smc_wsize;
	u32 rdata;
	int ret;
	u64 cmd;

	lockdep_assert_held(&smc->mutex);

	ret = apple_smc_saved_guard(smc);
	if (ret)
		return ret;
	if (rsize > SMC_MAX_SIZE)
		return -EINVAL;
	if (wsize > SMC_MAX_SIZE)
		return -EINVAL;

	if (rsize && wsize) {
		cmd = SMC_MSG_RW_KEY;
		memcpy_toio(smc->shmem.iomem, wbuf, wsize);
		smc_size = rsize;
		smc_wsize = wsize;
	} else if (wsize && !rsize) {
		cmd = SMC_MSG_WRITE_KEY;
		memcpy_toio(smc->shmem.iomem, wbuf, wsize);
		/*
		 * Setting size to the length we want to write and wsize to 0
		 * looks silly but that's how the SMC protocol works ¯\_(ツ)_/¯
		 */
		smc_size = wsize;
		smc_wsize = 0;
	} else if (!wsize && rsize) {
		cmd = SMC_MSG_READ_KEY;
		smc_size = rsize;
		smc_wsize = 0;
	} else {
		return -EINVAL;
	}

	ret = apple_smc_cmd_locked(smc, cmd, key, smc_size, smc_wsize, &rdata);
	if (ret < 0)
		return ret;

	if (rsize && ret > rsize)
		return -EIO;

	if (rsize) {
		/*
		 * Small data <= 4 bytes is returned as part of the reply
		 * message which is sent over the mailbox FIFO. Everything
		 * bigger has to be copied from SRAM which is mapped as
		 * Device memory.
		 */
		if (rsize <= 4)
			memcpy(rbuf, &rdata, ret);
		else
			apple_smc_read_sram(smc, rbuf, ret);
	}

	return ret;
}

int apple_smc_read(struct apple_smc *smc, smc_key key, void *buf, size_t size)
{
	if (IS_ENABLED(CONFIG_MFD_MACSMC_KUNIT_TEST))
		KUNIT_STATIC_STUB_REDIRECT(apple_smc_read, smc, key, buf, size);

	guard(mutex)(&smc->mutex);

	return apple_smc_rw_locked(smc, key, NULL, 0, buf, size);
}
EXPORT_SYMBOL(apple_smc_read);

int apple_smc_write(struct apple_smc *smc, smc_key key, const void *buf, size_t size)
{
	if (IS_ENABLED(CONFIG_MFD_MACSMC_KUNIT_TEST))
		KUNIT_STATIC_STUB_REDIRECT(apple_smc_write, smc, key, buf, size);

	guard(mutex)(&smc->mutex);

	return apple_smc_rw_locked(smc, key, buf, size, NULL, 0);
}
EXPORT_SYMBOL(apple_smc_write);

int apple_smc_rw(struct apple_smc *smc, smc_key key, const void *wbuf, size_t wsize,
		 void *rbuf, size_t rsize)
{
	guard(mutex)(&smc->mutex);

	return apple_smc_rw_locked(smc, key, wbuf, wsize, rbuf, rsize);
}
EXPORT_SYMBOL(apple_smc_rw);

int apple_smc_get_key_by_index(struct apple_smc *smc, int index, smc_key *key)
{
	u32 data;
	int ret;

	if (index < 0 || index >= smc->key_count)
		return -EINVAL;

	ret = apple_smc_cmd(smc, SMC_MSG_GET_KEY_BY_INDEX, index, 0, 0, &data);
	if (ret < 0)
		return ret;

	*key = swab32(data);
	return ret;
}
EXPORT_SYMBOL(apple_smc_get_key_by_index);

int apple_smc_get_key_info(struct apple_smc *smc, smc_key key, struct apple_smc_key_info *info)
{
	u8 key_info[6];
	int ret;

	/* Keep the shared reply buffer stable until its metadata is copied. */
	guard(mutex)(&smc->mutex);
	ret = apple_smc_cmd_locked(smc, SMC_MSG_GET_KEY_INFO, key, 0, 0, NULL);
	if (ret >= 0 && info) {
		apple_smc_read_sram(smc, key_info, sizeof(key_info));
		info->size = key_info[0];
		info->type_code = get_unaligned_be32(&key_info[1]);
		info->flags = key_info[5];
	}
	return ret;
}
EXPORT_SYMBOL(apple_smc_get_key_info);

int apple_smc_enter_atomic(struct apple_smc *smc)
{
	guard(mutex)(&smc->mutex);
	int ret = apple_smc_saved_guard(smc);

	if (ret)
		return ret;

	/*
	 * Disable notifications since this is called before shutdown and no
	 * notification handler will be able to handle the notification
	 * using atomic operations only. Also ignore any failure here
	 * because we're about to shut down or reboot anyway.
	 * We can't use apple_smc_write_flag here since that would try to lock
	 * smc->mutex again.
	 */
	const u8 flag = 0;

	apple_smc_rw_locked(smc, SMC_KEY(NTAP), &flag, sizeof(flag), NULL, 0);

	smc->atomic_mode = true;

	return 0;
}
EXPORT_SYMBOL(apple_smc_enter_atomic);

int apple_smc_write_atomic(struct apple_smc *smc, smc_key key, const void *buf, size_t size)
{
	guard(spinlock_irqsave)(&smc->lock);
	u8 result;
	int ret, poll_ret;
	u64 msg;
	ktime_t t0;
	s64 us;

	ret = apple_smc_saved_guard(smc);
	if (ret)
		return ret;
	if (size > SMC_MAX_SIZE || size == 0)
		return -EINVAL;

	if (smc->boot_stage != APPLE_SMC_INITIALIZED)
		return -EIO;
	if (!smc->atomic_mode)
		return -EIO;
	/*
	 * A poll timeout or a mismatched reply does not cancel the command or
	 * release its SRAM. Its late reply does: take it if it has arrived.
	 */
	if (smc->atomic_pending) {
		/* Wait for the earlier command's late reply instead of giving up. */
		ret = read_poll_timeout_atomic(apple_rtkit_poll, poll_ret,
					       poll_ret < 0 || !smc->atomic_pending,
					       100, SMC_ATOMIC_TIMEOUT_MS * USEC_PER_MSEC,
					       false, smc->rtk);
		if (poll_ret < 0 || smc->atomic_pending) {
			dev_err(smc->dev, "atomic: earlier command still pending (%d/%d)\n",
				ret, poll_ret);
			return -EBUSY;
		}
		dev_warn(smc->dev, "atomic: earlier command's reply arrived late\n");
	}

	memcpy_toio(smc->shmem.iomem, buf, size);
	smc->msg_id = (smc->msg_id + 1) & 0xf;
	msg = (FIELD_PREP(SMC_MSG, SMC_MSG_WRITE_KEY) |
	       FIELD_PREP(SMC_SIZE, size) |
	       FIELD_PREP(SMC_ID, smc->msg_id) |
	       FIELD_PREP(SMC_DATA, key));
	smc->atomic_pending = true;

	ret = apple_rtkit_send_message(smc->rtk, SMC_ENDPOINT, msg, NULL, true);
	if (ret < 0) {
		/* Never sent, so no reply will release the buffer. */
		smc->atomic_pending = false;
		dev_err(smc->dev, "Failed to send command (%d)\n", ret);
		return ret;
	}

	t0 = ktime_get();
	ret = read_poll_timeout_atomic(apple_rtkit_poll, poll_ret,
				       poll_ret < 0 || !smc->atomic_pending,
				       100, SMC_ATOMIC_TIMEOUT_MS * USEC_PER_MSEC,
				       false, smc->rtk);
	us = ktime_us_delta(ktime_get(), t0);
	if (us > 100 * USEC_PER_MSEC || ret < 0 || smc->atomic_pending)
		dev_warn(smc->dev, "atomic: key %p4ch reply after %lld us (ret %d)\n",
			 &key, us, ret);
	if (poll_ret < 0) {
		dev_err(smc->dev, "RTKit poll failed (%llx)", msg);
		return poll_ret;
	}
	if (ret < 0 || smc->atomic_pending) {
		dev_err(smc->dev, "Atomic SMC command timed out (%llx)\n", msg);
		return -ETIMEDOUT;
	}

	if (FIELD_GET(SMC_ID, smc->cmd_ret) != smc->msg_id) {
		/* This response cannot resolve ownership of the published command. */
		smc->atomic_pending = true;
		dev_err(smc->dev, "Command sequence mismatch (expected %d, got %d)\n",
			smc->msg_id, (unsigned int)FIELD_GET(SMC_ID, smc->cmd_ret));
		return -EIO;
	}

	result = FIELD_GET(SMC_RESULT, smc->cmd_ret);
	if (result)
		return -EIO;

	return FIELD_GET(SMC_SIZE, smc->cmd_ret);
}
EXPORT_SYMBOL(apple_smc_write_atomic);

static void apple_smc_rtkit_crashed(void *cookie, const void *bfr, size_t bfr_len)
{
	struct apple_smc *smc = cookie;

	smc->boot_stage = APPLE_SMC_ERROR_CRASHED;
	dev_err(smc->dev, "SMC crashed! Your system will reboot in a few seconds...\n");
}

static int apple_smc_rtkit_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_smc *smc = cookie;
	size_t bfr_end;

	if (!bfr->iova) {
		dev_err(smc->dev, "RTKit wants a RAM buffer\n");
		return -EIO;
	}

	if (check_add_overflow(bfr->iova, bfr->size - 1, &bfr_end))
		return -EFAULT;

	if (bfr->iova < smc->sram->start || bfr->iova > smc->sram->end ||
	    bfr_end > smc->sram->end) {
		dev_err(smc->dev, "RTKit buffer request outside SRAM region: [0x%llx, 0x%llx]\n",
			(unsigned long long)bfr->iova,
			(unsigned long long)bfr_end);
		return -EFAULT;
	}

	bfr->iomem = smc->sram_base + (bfr->iova - smc->sram->start);
	bfr->is_mapped = true;

	return 0;
}

static bool apple_smc_rtkit_recv_early(void *cookie, u8 endpoint, u64 message)
{
	struct apple_smc *smc = cookie;

	if (endpoint != SMC_ENDPOINT) {
		dev_warn(smc->dev, "Received message for unknown endpoint 0x%x\n", endpoint);
		return false;
	}

	if (smc->boot_stage == APPLE_SMC_BOOTING) {
		int ret;

		smc->shmem.iova = message;
		smc->shmem.size = SMC_SHMEM_SIZE;
		ret = apple_smc_rtkit_shmem_setup(smc, &smc->shmem);
		if (ret < 0) {
			smc->boot_stage = APPLE_SMC_ERROR_NO_SHMEM;
			dev_err(smc->dev, "Failed to initialize shared memory (%d)\n", ret);
		} else {
			smc->boot_stage = APPLE_SMC_INITIALIZED;
		}
		complete(&smc->init_done);
	} else if (FIELD_GET(SMC_MSG, message) == SMC_MSG_NOTIFICATION) {
		/* Handle these in the RTKit worker thread */
		return false;
	} else {
		if (smc->saved_enabled && (!smc->atomic_mode ||
					 smp_load_acquire(&smc->saved_pending))) {
			if (!smp_load_acquire(&smc->saved_pending) ||
			    FIELD_GET(SMC_ID, message) != smc->msg_id) {
				if (!smc->saved_fault)
					smc->saved_unexpected = message;
				WRITE_ONCE(smc->saved_fault, -EPROTO);
				return true; /* preserve pending ID/SRAM and first evidence */
			}
			smc->cmd_ret = message;
			smp_store_release(&smc->saved_pending, false);
			if (!smc->saved_scope)
				complete(&smc->cmd_done);
			return true;
		}
		smc->cmd_ret = message;
		if (smc->atomic_pending)
			smc->atomic_pending = false;
		else
			complete(&smc->cmd_done);
	}

	return true;
}

static void apple_smc_rtkit_recv(void *cookie, u8 endpoint, u64 message)
{
	struct apple_smc *smc = cookie;

	if (endpoint != SMC_ENDPOINT) {
		dev_warn(smc->dev, "Received message for unknown endpoint 0x%x\n", endpoint);
		return;
	}

	if (FIELD_GET(SMC_MSG, message) != SMC_MSG_NOTIFICATION) {
		dev_warn(smc->dev, "Received unknown message from worker: 0x%llx\n", message);
		return;
	}

	blocking_notifier_call_chain(&smc->event_handlers, FIELD_GET(SMC_DATA, message), NULL);
}

static const struct apple_rtkit_ops apple_smc_rtkit_ops = {
	.crashed = apple_smc_rtkit_crashed,
	.recv_message = apple_smc_rtkit_recv,
	.recv_message_early = apple_smc_rtkit_recv_early,
	.shmem_setup = apple_smc_rtkit_shmem_setup,
};

static void apple_smc_rtkit_shutdown(void *data)
{
	struct apple_smc *smc = data;

	/* Shut down SMC firmware, if it's not completely wedged */
	if (!apple_smc_saved_guard(smc) && apple_rtkit_is_running(smc->rtk))
		apple_rtkit_quiesce(smc->rtk);
}

static void apple_smc_disable_notifications(void *data)
{
	struct apple_smc *smc = data;

	apple_smc_write_flag(smc, SMC_KEY(NTAP), false);
}

static int apple_smc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_smc *smc;
	__be32 count = 0;
	bool t6050;
	int ret;

	smc = devm_kzalloc(dev, sizeof(*smc), GFP_KERNEL);
	if (!smc)
		return -ENOMEM;

	mutex_init(&smc->mutex);
	spin_lock_init(&smc->lock);
	smc->dev = &pdev->dev;
	smc->saved_enabled = of_machine_is_compatible("apple,j714s") &&
		of_device_is_compatible(dev->of_node, "apple,t6050-smc");
	smc->sram_base = devm_platform_get_and_ioremap_resource(pdev, 1, &smc->sram);
	if (IS_ERR(smc->sram_base))
		return dev_err_probe(dev, PTR_ERR(smc->sram_base), "Failed to map SRAM region");

	if (smc->saved_enabled)
		smc->rtk = devm_apple_rtkit_init_m5_saved(dev, smc, NULL, 0, &apple_smc_rtkit_ops);
	else
		smc->rtk = devm_apple_rtkit_init(dev, smc, NULL, 0, &apple_smc_rtkit_ops);
	if (IS_ERR(smc->rtk))
		return dev_err_probe(dev, PTR_ERR(smc->rtk), "Failed to initialize RTKit");

	smc->boot_stage = APPLE_SMC_BOOTING;
	ret = apple_rtkit_wake(smc->rtk);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to wake up SMC");

	ret = devm_add_action_or_reset(dev, apple_smc_rtkit_shutdown, smc);
	if (ret)
		return ret;

	ret = apple_rtkit_start_ep(smc->rtk, SMC_ENDPOINT);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to start SMC endpoint");

	init_completion(&smc->init_done);
	init_completion(&smc->cmd_done);

	ret = apple_rtkit_send_message(smc->rtk, SMC_ENDPOINT,
				       FIELD_PREP(SMC_MSG, SMC_MSG_INITIALIZE), NULL, false);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to send init message");

	if (wait_for_completion_timeout(&smc->init_done, msecs_to_jiffies(SMC_TIMEOUT_MS)) == 0) {
		dev_err(dev, "Timed out initializing SMC");
		return -ETIMEDOUT;
	}

	if (smc->boot_stage != APPLE_SMC_INITIALIZED) {
		dev_err(dev, "SMC failed to boot successfully, boot stage=%d\n", smc->boot_stage);
		return -EIO;
	}

	dev_set_drvdata(&pdev->dev, smc);
	BLOCKING_INIT_NOTIFIER_HEAD(&smc->event_handlers);

	ret = apple_smc_read(smc, SMC_KEY(#KEY), &count, sizeof(count));
	if (ret >= 0 && ret != sizeof(count)) {
		count = 0;
		ret = -EIO;
	}
	if (ret < 0)
		return dev_err_probe(smc->dev, ret, "Failed to get key count");
	smc->key_count = be32_to_cpu(count);

	/* Notifications, GPIO and power management are not qualified on T8152. */
	if (of_device_is_compatible(dev->of_node, "apple,t8152-smc"))
		return devm_mfd_add_devices(smc->dev, PLATFORM_DEVID_NONE,
					    apple_smc_telemetry_devs,
					    ARRAY_SIZE(apple_smc_telemetry_devs),
					    NULL, 0, NULL);

	ret = devm_device_init_wakeup(dev);
	if (ret)
		return ret;

	/* Enable notifications */
	apple_smc_write_flag(smc, SMC_KEY(NTAP), true);
	ret = devm_add_action_or_reset(dev, apple_smc_disable_notifications, smc);
	if (ret)
		return ret;

	t6050 = of_device_is_compatible(dev->of_node, "apple,t6050-smc");
	if (t6050)
		ret = devm_mfd_add_devices(smc->dev, PLATFORM_DEVID_NONE,
					 apple_smc_t6050_devs,
					 ARRAY_SIZE(apple_smc_t6050_devs), NULL, 0, NULL);
	else
		ret = devm_mfd_add_devices(smc->dev, PLATFORM_DEVID_NONE,
					 apple_smc_devs, ARRAY_SIZE(apple_smc_devs),
					 NULL, 0, NULL);
	if (ret)
		return dev_err_probe(smc->dev, ret, "Failed to register sub-devices");

	ret = apple_smc_add_described_devices(smc, apple_smc_described_devs,
					      ARRAY_SIZE(apple_smc_described_devs));
	if (ret || !t6050)
		return ret;

	return apple_smc_add_described_devices(smc, apple_smc_t6050_described_devs,
					       ARRAY_SIZE(apple_smc_t6050_described_devs));
}

static const struct of_device_id apple_smc_of_match[] = {
	{ .compatible = "apple,t8152-smc" },
	{ .compatible = "apple,t6050-smc" },
	{ .compatible = "apple,t8103-smc" },
	{ .compatible = "apple,smc" },
	{},
};
MODULE_DEVICE_TABLE(of, apple_smc_of_match);

/*
 * The SMC keeps running through a firmware-driven suspend. It is told first and
 * stops last, and on a wake it hears first. In order, measured on T8140 and
 * unverified on other SoCs: MBSE = slpw before any other coprocessor is told;
 * MBSE = slpa and the AP power notice after all the others; on a wake the notice,
 * MBSE and MBSW read, MBSE = waka. The noirq phases give that order whatever the
 * device order is, so these commands poll. What the values mean is inferred
 * from their letters.
 *
 * SMC.OutboxNotEmpty wakes the SoC while anything waits in the outbox. An event that arrives
 * during the sleep's entry (the lid, a key) is left there, so the sleep returns at once.
 *
 * The debug link's cut, on a debug cable only, makes the SMC see a cable pulled and plugged
 * back. With the cut after slpw, the SMC woke the AP right after the commit, for USB-C_plug
 * or acdetach, in every such sleep (J700, 223 of 223), even with its outbox quiet for 500 ms
 * before the notice. A sleep that cuts the link warns the SMC only after the cut.
 */

/* The gaps macOS leaves before the AP idle notice and after it. */
#define SMC_SLEEP_ARM_MS	6
#define SMC_SLEEP_IDLE_MS	30
/*
 * Between a late warning (a sleep that cut the debug link) and slpa. macOS's shortest gap
 * was 770 ms; the warning at .prepare leaves 150-500 ms, and those sleeps run to the alarm.
 */
#define SMC_SLEEP_WARN_MS	200

/* After the debug link's cut. No message came in it in any logged sleep. */
#define SMC_OUTBOX_QUIET_MS	100
#define SMC_OUTBOX_SETTLE_MS	5000

static int apple_smc_ap_power(struct apple_smc *smc, bool on)
{
	if (IS_ENABLED(CONFIG_MFD_MACSMC_KUNIT_TEST))
		KUNIT_STATIC_STUB_REDIRECT(apple_smc_ap_power, smc, on);

	return smc->polled ? apple_rtkit_ap_power_noirq(smc->rtk, on) :
			     apple_rtkit_ap_power(smc->rtk, on);
}

static void apple_smc_set_ap_notified(struct apple_smc *smc, bool notified)
{
	guard(mutex)(&smc->mutex);

	smc->ap_notified = notified;
}

static void apple_smc_set_polled(struct apple_smc *smc, bool polled)
{
	guard(mutex)(&smc->mutex);

	smc->polled = polled;
}

static void apple_smc_write_mbse(struct apple_smc *smc, smc_key value)
{
	int ret;

	ret = apple_smc_write_u32(smc, SMC_KEY(MBSE), value);
	if (ret)
		dev_err(smc->dev, "Failed to issue MBSE = %p4ch: %d\n", &value, ret);
}

static void apple_smc_sleep_warn(struct apple_smc *smc)
{
	apple_smc_write_mbse(smc, SMC_KEY(slpw));
	smc->sleep_warned = true;
}

static int apple_smc_notice_idle(struct apple_smc *smc)
{
	int ret;

	/*
	 * Commands fail from here on. The mutex isn't held across the notice: the
	 * answer comes through the same worker as the notifications, and those can
	 * be waiting for this mutex.
	 */
	apple_smc_set_ap_notified(smc, true);

	ret = apple_smc_ap_power(smc, false);
	if (ret) {
		/* Taken and not answered, perhaps, so undo it. */
		apple_smc_ap_power(smc, true);
		apple_smc_set_ap_notified(smc, false);
		dev_err(smc->dev, "SMC did not take the sleep notice: %d\n", ret);
	} else {
		dev_dbg(smc->dev, "AP idle notice answered\n");
	}

	return ret;
}

static int apple_smc_sleep_commit(struct apple_smc *smc)
{
	int ret;

	apple_smc_write_mbse(smc, SMC_KEY(slpa));
	/* Device interrupts are off here. */
	mdelay(SMC_SLEEP_ARM_MS);

	ret = apple_smc_notice_idle(smc);
	if (!ret)
		mdelay(SMC_SLEEP_IDLE_MS);

	return ret;
}

/* MBSW's bits, as smc-pmu's event_name properties number them on T8140. Other SoCs differ. */
static const struct {
	u8 bit;
	const char *name;
} apple_smc_wake_reasons[] = {
	{ 0, "pwrbtn" }, { 9, "ipd_touch" }, { 10, "lid" }, { 16, "ipd_kbd" },
	{ 18, "charger" }, { 23, "wifibt" }, { 40, "acattach" }, { 41, "acdetach" },
	{ 42, "USB-C_plug" }, { 43, "USB2_plug" }, { 44, "USB2_wake" }, { 45, "aupo" },
	{ 48, "rtc" },
};

static int apple_smc_wake_reason_names(u64 record, char *buf, size_t size)
{
	int i, len = 0;

	buf[0] = 0;
	for (i = 0; i < ARRAY_SIZE(apple_smc_wake_reasons); i++)
		if (record & BIT_ULL(apple_smc_wake_reasons[i].bit))
			len += scnprintf(buf + len, size - len, "%s%s", len ? " " : "",
					 apple_smc_wake_reasons[i].name);

	return len;
}

/* Returns the first error reading the wake record; *mbsw: whether MBSW was read. */
static int apple_smc_notice_on(struct apple_smc *smc, bool *mbsw)
{
	u8 record[8] = {};
	char names[128];
	u32 event = 0;
	int ret, error = 0;

	if (!smc->saved_wake) {
		ret = apple_smc_ap_power(smc, true);
		if (ret)
			dev_err(smc->dev, "SMC did not take the wake notice: %d\n", ret);
		apple_smc_set_ap_notified(smc, false);
	}

	ret = apple_smc_read_u32(smc, SMC_KEY(MBSE), &event);
	if (ret) {
		error = ret < 0 ? ret : -EPROTO;
		dev_warn(smc->dev, "Failed to read MBSE after a wake: %d\n", ret);
	}
	ret = apple_smc_read(smc, SMC_KEY(MBSW), record, sizeof(record));
	if (ret < 0) {
		error = error ?: ret;
		dev_warn(smc->dev, "Failed to read MBSW after a wake: %d\n", ret);
	} else if (ret != sizeof(record)) {
		error = error ?: -EIO;
	}

	smc->wake_record = get_unaligned_le64(record);
	if (smc->sleep_policy)
		names[0] = 0;
	else
		apple_smc_wake_reason_names(smc->wake_record, names, sizeof(names));
	dev_info(smc->dev, "wake record: MBSE %#x, MBSW %8ph%s%s\n", event, record,
		 names[0] ? ": " : "", names);
	*mbsw = ret >= 0;

	return error;
}

/* MBSW's bit for a wake reason, by its name, or -1. */
static int apple_smc_wake_reason_bit(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(apple_smc_wake_reasons); i++)
		if (!strcmp(apple_smc_wake_reasons[i].name, name))
			return apple_smc_wake_reasons[i].bit;

	return -1;
}

/*
 * Tell the sleep driver where the lid is now, for the display. Woken for the lid alone,
 * which the SMC reports for the lid opening and for it closing: also if it's closed now.
 */
static void apple_smc_lid_wake(struct apple_smc *smc)
{
	int bit = apple_smc_wake_reason_bit("lid");
	bool lid_alone = bit >= 0 && smc->wake_record == BIT_ULL(bit);
	u8 lid = 0;
	int ret;

	/* No MSLD on a machine without a lid: the display then isn't told either way. */
	ret = apple_smc_read_u8(smc, SMC_KEY(MSLD), &lid);
	if (ret < 0) {
		if (lid_alone)
			dev_warn(smc->dev, "Failed to read the lid after a wake: %d\n", ret);
		return;
	}
	apple_sleep_lid_at_wake(lid);
	if (!lid_alone)
		return;
	dev_info(smc->dev, "woken for the lid, which is %s\n", lid ? "closed" : "open");
	if (lid)
		apple_sleep_lid_closed_wake();
}

/* Only what a cable pulled and plugged back reports: what the debug link's cut makes. */
static bool apple_smc_port_reasons_only(u64 record)
{
	static const char * const names[] = { "acattach", "acdetach", "USB-C_plug" };
	u64 port = 0;
	int i, bit;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		bit = apple_smc_wake_reason_bit(names[i]);
		if (bit >= 0)
			port |= BIT_ULL(bit);
	}

	return record && !(record & ~port);
}

static void apple_smc_sleep_wait(u64 acknowledged, u32 minimum_us);

static void apple_smc_sleep_wake(struct apple_smc *smc)
{
	bool mbsw;
	int ret = apple_smc_notice_on(smc, &mbsw);

	/* Zero when nothing the SMC watches woke the AP: the PMU's RTC, for one. */
	if (mbsw && !smc->sleep_policy)
		apple_sleep_radio_at_wake(smc->wake_record &
					  BIT_ULL(apple_smc_wake_reason_bit("wifibt")));
	if (smc->sleep_policy) {
		if (!ret)
			ret = apple_smc_write_u32(smc, SMC_KEY(MBSE), SMC_KEY(waka));

		if (!ret) {
			apple_smc_sleep_wait(__arch_counter_get_cntpct(), smc->sleep_policy->waka_to_pmvc_us);
			ret = apple_smc_write_u32(smc, SMC_KEY(pmVC), smc->sleep_policy->pmvc);
		}
		if (ret) {
			smc->saved_recovery = true;
			smc->saved_restore_error = ret;
			dev_err(smc->dev, "SMC wake policy failed: %d\n", ret);
			return;
		}
	} else {
		apple_smc_write_mbse(smc, SMC_KEY(waka));
	}
	smc->sleep_warned = false;
	if (smc->sleep_policy)
		return; /* M5 wake-reason bit meanings are separate profile data. */
	apple_smc_lid_wake(smc);
	if (apple_smc_port_reasons_only(smc->wake_record))
		apple_sleep_debug_link_cut_wake();
	/* A dark wake isn't one: with wakeup_count in use, it would abort the sleep after it. */
	if (smc->wake_record && !pm_dark_wake())
		pm_wakeup_event(smc->dev, 0);
}

/* The M5 Pro's sleep policy, supplied by the firmware-sleep profile.
 * Timestamps acknowledge actual protocol progress. Floors do not assert native
 * interval invariance; the sleep owner computes the final timer target.
 */
static void apple_smc_sleep_wait(u64 acknowledged, u32 minimum_us)
{
	u64 deadline = acknowledged + arch_timer_get_cntfrq() * (u64)minimum_us / USEC_PER_SEC;

	while (__arch_counter_get_cntpct() < deadline)
		cpu_relax();
}

#define SMC_SAVED_STEP_US	10
#define SMC_SAVED_LOOPS		50000

static int apple_smc_saved_poll(struct apple_smc *smc)
{
	int ret = apple_rtkit_m5_saved_poll(smc->rtk);

	if (ret < 0)
		return ret;
	if (smc->boot_stage != APPLE_SMC_INITIALIZED || apple_rtkit_is_crashed(smc->rtk))
		return -EIO;
	return READ_ONCE(smc->saved_fault);
}

static int apple_smc_saved_wait_reply(struct apple_smc *smc)
{
	int i, ret;

	for (i = 0; i < SMC_SAVED_LOOPS; i++) {
		ret = apple_smc_saved_poll(smc);
		if (ret)
			return ret;
		if (!smp_load_acquire(&smc->saved_pending))
			return 0;
		udelay(SMC_SAVED_STEP_US);
	}
	return -ETIMEDOUT;
}

static int apple_smc_saved_drain(struct apple_smc *smc)
{
	int i, ret;

	for (i = 0; i < 64; i++) {
		ret = apple_rtkit_m5_saved_poll(smc->rtk);
		if (ret < 0)
			return ret;
		if (smc->saved_fault)
			return smc->saved_fault;
		if (!smc->saved_committed &&
		    apple_rtkit_m5_saved_notification_pending(smc->rtk))
			return -ECANCELED; /* replay in process context; not protocol poison */
		if (!ret)
			return 0;
	}
	return -ENOBUFS;
}

static int apple_smc_saved_write_u32(struct apple_smc *smc, smc_key key, u32 value)
{
	u64 msg;
	int ret;

	/* The suspend task already owns this mutex; never acquire it here. */
	lockdep_assert_held(&smc->mutex);
	if (!smc->saved_scope || smc->atomic_mode || smc->ap_notified ||
	    smc->boot_stage != APPLE_SMC_INITIALIZED)
		return -EIO;
	if (smc->saved_fault || smc->saved_pending)
		return smc->saved_fault ?: -EBUSY;
	ret = apple_smc_saved_drain(smc);
	if (ret)
		return ret;
	/* Guard precedes the copy. Timeout keeps both this ID and this SRAM. */
	memcpy_toio(smc->shmem.iomem, &value, sizeof(value));
	smc->msg_id = (smc->msg_id + 1) & 0xf;
	msg = FIELD_PREP(SMC_MSG, SMC_MSG_WRITE_KEY) |
		FIELD_PREP(SMC_SIZE, sizeof(value)) |
		FIELD_PREP(SMC_ID, smc->msg_id) | FIELD_PREP(SMC_DATA, key);
	smp_store_release(&smc->saved_pending, true);
	ret = apple_rtkit_m5_saved_send(smc->rtk, msg);
	if (ret) {
		smp_store_release(&smc->saved_pending, false); /* negative TX: never posted */
		return ret;
	}
	ret = apple_smc_saved_wait_reply(smc);
	if (ret)
		return ret;
	if (FIELD_GET(SMC_RESULT, smc->cmd_ret))
		return -EIO;
	/* The existing write_u32 callers require a zero-length write ACK. */
	return FIELD_GET(SMC_SIZE, smc->cmd_ret) ? -EPROTO : 0;
}

static u64 apple_smc_saved_stamp(void)
{
	return __arch_counter_get_cntpct();
}

static int apple_smc_sleep_saved_begin(void *data)
{
	struct apple_smc *smc = data;
	int ret;

	if (!smc->saved_enabled || !smc->sleep_policy)
		return -EOPNOTSUPP;
	if (!mutex_trylock(&smc->mutex))
		return -EBUSY;
	ret = apple_smc_saved_guard(smc);
	if (!ret && (smc->atomic_mode || smc->ap_notified))
		ret = -EBUSY;
	if (ret) {
		mutex_unlock(&smc->mutex);
		return ret;
	}
	/* Gate new commands before synchronizing RX/worker callbacks. Never
	 * flush a worker while holding the mutex its notifier might acquire.
	 */
	smc->saved_blocked = true;
	mutex_unlock(&smc->mutex);
	ret = apple_rtkit_m5_saved_begin(smc->rtk, SMC_ENDPOINT, apple_smc_saved_stamp);
	if (ret)
		goto unblock;
	if (!mutex_trylock(&smc->mutex)) {
		ret = -EBUSY;
		goto end;
	}
	smc->saved_scope = true;
	smc->saved_committed = false;
	smc->saved_pmvc_sent = false;
	/* Drain only a bounded prefix before reserving any ID. An old reply
	 * cannot satisfy a fresh command or a stale AP completion a new post.
	 */
	ret = apple_smc_saved_drain(smc);
	if (!ret)
		return 0; /* held until platform wake in the SAME suspend task */
	smc->saved_scope = false;
	mutex_unlock(&smc->mutex);
end:
	apple_rtkit_m5_saved_end(smc->rtk);
unblock:
	/* Begin never posts a command or AP notice. A clean setup refusal must
	 * not poison normal SMC or suppress the earlier slpw/waka rollback.
	 * The closed gate makes every intervening normal mutex holder return
	 * before SRAM/TX; no callback/work flush is awaited under this lock.
	 */
	mutex_lock(&smc->mutex);
	smc->saved_recovery = smc->saved_pending || smc->saved_fault ||
		smc->ap_notified || apple_rtkit_m5_saved_unresolved(smc->rtk);
	smc->saved_blocked = false;
	smc->saved_restore_error = smc->saved_recovery ? ret : 0;
	smc->saved_notice_error = ret;
	mutex_unlock(&smc->mutex);
	return ret;
}

static int apple_smc_sleep_saved_end(void *data)
{
	struct apple_smc *smc = data;
	int ret, restore = 0;

	lockdep_assert_held(&smc->mutex);
	/* Poll also exposes a latched RTKit crash/report/protocol refusal when
	 * no SMC command remains pending. Never reopen the normal lane over it.
	 */
	ret = apple_smc_saved_poll(smc);
	/* Bounded reconciliation; never release pending ownership on timeout. */
	if (!ret && smc->saved_pending)
		ret = apple_smc_saved_wait_reply(smc);
	if (smc->ap_notified) {
		restore = apple_rtkit_m5_saved_ap_power(smc->rtk, true, NULL, NULL);
		if (!restore) {
			smc->ap_notified = false;
			smc->saved_wake = true;
		}
	}
	if (!ret)
		ret = restore ?: smc->saved_fault;
	if (!ret && smc->saved_pending)
		ret = -EBUSY;
	smc->saved_restore_error = ret;
	smc->saved_recovery = !!ret;
	smc->saved_scope = false;
	smc->saved_committed = false;
	smc->saved_blocked = false;
	mutex_unlock(&smc->mutex);
	apple_rtkit_m5_saved_end(smc->rtk);
	/* Original refusal stays a refusal; this result describes restoration. */
	return ret;
}

/* Readiness is a returning operation BEFORE SEP/ANS/PMP stop. A drained
 * notification remains retained and can cancel only on this side of the boundary.
 */
static int apple_smc_sleep_saved_check(void *data)
{
	struct apple_smc *smc = data;

	lockdep_assert_held(&smc->mutex);
	if (!irqs_disabled() || !smc->saved_enabled || !smc->saved_scope ||
	    smc->saved_committed || !smc->sleep_policy || !smc->sleep_warned ||
	    smc->atomic_mode || smc->ap_notified || smc->saved_recovery ||
	    smc->boot_stage != APPLE_SMC_INITIALIZED)
		return -EBUSY;
	if (smc->saved_fault || smp_load_acquire(&smc->saved_pending))
		return smc->saved_fault ?: -EBUSY;
	return apple_smc_saved_drain(smc);
}

static int apple_smc_sleep_late_notice(void *data, u64 *sent, u64 *ack)
{
	struct apple_smc *smc = data;
	const struct apple_sleep_smc_policy *p = smc->sleep_policy;
	u64 answered;
	int ret;

	*sent = *ack = 0;
	if (!p || !smc->saved_scope || !irqs_disabled() ||
	    !smc->sleep_warned || smc->ap_notified)
		return -EINVAL;
	if (smc->saved_pmvc_sent) {
		answered = smc->saved_pmvc_at;
	} else {
		ret = apple_smc_saved_write_u32(smc, SMC_KEY(pmVC), p->pmvc);
		answered = __arch_counter_get_cntpct();
		if (ret)
			goto out;
	}
	apple_smc_sleep_wait(answered, p->pmvc_to_slpa_us);
	ret = apple_smc_saved_write_u32(smc, SMC_KEY(MBSE), SMC_KEY(slpa));
	answered = __arch_counter_get_cntpct();
	if (ret)
		goto out;
	apple_smc_sleep_wait(answered, p->slpa_to_idle_us);
	ret = apple_smc_saved_drain(smc);
	if (ret)
		goto out;
	/* Even a failed ACK may mean AP IDLE took effect. End must reconcile. */
	smc->ap_notified = true;
	ret = apple_rtkit_m5_saved_ap_power(smc->rtk, false, sent, ack);
	if (!ret)
		ret = apple_smc_saved_drain(smc);
out:
	smc->saved_notice_error = ret;
	return ret;
}

/* All returning checks and the irreversible peer stops are behind us.
 * A newly arriving wake notification is retained for replay, not a cancellation.
 * Actual transport/protocol errors still reach the caller's fatal disposition.
 */
static int apple_smc_sleep_commit_notice(void *data, u64 *sent, u64 *ack)
{
	struct apple_smc *smc = data;

	lockdep_assert_held(&smc->mutex);
	if (!smc->saved_scope || smc->saved_committed)
		return -EINVAL;
	smc->saved_committed = true;
	return apple_smc_sleep_late_notice(data, sent, ack);
}

/*
 * macOS writes pmVC twice after ANS, once after each USB_AON power-down,
 * 34 ms before slpa. Only after the irreversible stops; slpa's wait counts from the last one.
 */
static int apple_smc_sleep_saved_pmvc(void *data)
{
	struct apple_smc *smc = data;
	int ret;

	lockdep_assert_held(&smc->mutex);
	if (!smc->sleep_policy || !smc->saved_scope || smc->saved_committed ||
	    !irqs_disabled() || !smc->sleep_warned || smc->ap_notified)
		return -EINVAL;
	ret = apple_smc_saved_write_u32(smc, SMC_KEY(pmVC), smc->sleep_policy->pmvc);
	if (ret)
		return ret;
	smc->saved_pmvc_at = __arch_counter_get_cntpct();
	smc->saved_pmvc_sent = true;
	return 0;
}

static const struct apple_sleep_smc_saved_ops apple_smc_saved_ops = {
	.begin = apple_smc_sleep_saved_begin,
	.check = apple_smc_sleep_saved_check,
	.commit_notice = apple_smc_sleep_commit_notice,
	.late_notice = apple_smc_sleep_late_notice,
	.end = apple_smc_sleep_saved_end,
	.pmvc = apple_smc_sleep_saved_pmvc,
};

static int apple_smc_prepare_policy(struct apple_smc *smc)
{
	const struct apple_sleep_smc_policy *p = smc->sleep_policy;
	struct apple_smc_key_info info;
	int ret;

	/* A timed-out warning may have taken effect. Keep rollback ownership
	 * even when its ACK never arrives; the command guard preserves SRAM.
	 */
	smc->sleep_warned = true;
	ret = apple_smc_write_u32(smc, SMC_KEY(MBSE), SMC_KEY(slpw));
	if (ret)
		return ret;
	smc->sleep_warn_ack = __arch_counter_get_cntpct();
	/* Process context: let the minimum pass without stretching the noirq tail. */
	usleep_range(p->warn_to_pmvc_us, p->warn_to_pmvc_us + 1000);
	if (!smc->sleep_pmvc_info) {
		ret = apple_smc_get_key_info(smc, SMC_KEY(pmVC), &info);
		if (ret < 0)
			return ret;
		if (info.size != sizeof(u32))
			return -EINVAL;
		smc->sleep_pmvc_info = true;
	}
	ret = apple_smc_write_u32(smc, SMC_KEY(pmVC), p->pmvc);
	if (ret)
		return ret;
	return apple_sleep_smc_register_saved(smc->dev, &apple_smc_saved_ops, smc);
}

static int apple_smc_sleep_prepare(struct apple_smc *smc)
{
	if (smc->saved_enabled) {
		int ret;

		if (!mutex_trylock(&smc->mutex))
			return -EBUSY;
		ret = apple_smc_saved_guard(smc);
		mutex_unlock(&smc->mutex);
		if (ret)
			return ret; /* Do not replace a quarantined policy with NULL. */
	}
	smc->sleep_policy = apple_sleep_smc_policy();
	if (smc->sleep_policy) {
		int ret = apple_smc_prepare_policy(smc);

		if (ret && smc->sleep_warned) {
			int rollback = apple_smc_write_u32(smc, SMC_KEY(MBSE), SMC_KEY(waka));

			if (rollback) {
				smc->saved_recovery = true;
				smc->saved_restore_error = rollback;
			} else {
				smc->sleep_warned = false;
			}
		}
		if (ret && !smc->saved_recovery)
			smc->sleep_policy = NULL;
		return ret;
	}
	if (apple_sleep_debug_link_up())
		smc->sleep_warn_late = true;
	else
		apple_smc_sleep_warn(smc);

	return 0;
}

/*
 * A saved-context policy (the M5) is warned at .suspend, as macOS's slpw comes 340-425 ms before
 * its commit, after the HID packets; .prepare only checks the guard.
 */
static int apple_smc_prepare(struct device *dev)
{
	struct apple_smc *smc = dev_get_drvdata(dev);

	if (!pm_suspend_via_firmware())
		return 0;
	if (apple_sleep_smc_policy()) {
		int ret = 0;

		if (smc->saved_enabled) {
			if (!mutex_trylock(&smc->mutex))
				return -EBUSY;
			ret = apple_smc_saved_guard(smc);
			mutex_unlock(&smc->mutex);
		}
		return ret;
	}
	return apple_smc_sleep_prepare(smc);
}

static int apple_smc_suspend(struct device *dev)
{
	struct apple_smc *smc = dev_get_drvdata(dev);

	if (!pm_suspend_via_firmware() || !apple_sleep_smc_policy())
		return 0;
	return apple_smc_sleep_prepare(smc);
}

/* Warned, and a refusal came before the notice: tell it the sleep is off. */
static void apple_smc_complete(struct device *dev)
{
	struct apple_smc *smc = dev_get_drvdata(dev);

	apple_sleep_smc_unregister(dev);

	/* Not saved_pending: an ordinary command may be in flight here, and it is read without the mutex. */
	if (smc->saved_enabled && (smc->saved_recovery || smc->saved_fault)) {
		dev_err(dev, "SMC quarantined after the sleep: notice=%d restore=%d fault=%d pending=%d\n",
			smc->saved_notice_error, smc->saved_restore_error,
			smc->saved_fault, smc->saved_pending);
		return; /* Keep warning, policy and ownership for diagnosis/recovery. */
	}
	smc->sleep_warn_late = false;
	if (smc->sleep_warned) {
		if (smc->sleep_policy) {
			int ret = apple_smc_write_u32(smc, SMC_KEY(MBSE), SMC_KEY(waka));

			if (!ret) {
				apple_smc_sleep_wait(__arch_counter_get_cntpct(), smc->sleep_policy->waka_to_pmvc_us);
				ret = apple_smc_write_u32(smc, SMC_KEY(pmVC), smc->sleep_policy->pmvc);
			}
			if (ret) {
				smc->saved_recovery = true;
				smc->saved_restore_error = ret;
				dev_err(smc->dev, "SMC sleep warning rollback failed: %d\n", ret);
				return;
			}
		} else {
			apple_smc_write_mbse(smc, SMC_KEY(waka));
		}
		smc->sleep_warned = false;
	}
	smc->sleep_policy = NULL;
}

/*
 * For at least @min_ms, then until the outbox is quiet for @quiet_ms. The events go to their
 * handlers as usual, through the RTKit worker.
 */
static void apple_smc_outbox_poll(struct apple_smc *smc, const char *after,
				  unsigned int min_ms, unsigned int quiet_ms)
{
	unsigned int ms, quiet = 0, count = 0, last = 0;
	int ret;

	if (IS_ENABLED(CONFIG_MFD_MACSMC_KUNIT_TEST))
		KUNIT_STATIC_STUB_REDIRECT(apple_smc_outbox_poll, smc, after, min_ms, quiet_ms);

	for (ms = 0; ms < SMC_OUTBOX_SETTLE_MS && (ms < min_ms || quiet < quiet_ms); ms++) {
		ret = apple_rtkit_poll(smc->rtk);
		if (ret > 0) {
			count += ret;
			last = ms;
			quiet = 0;
		} else {
			quiet++;
		}
		usleep_range(1000, 1200);
	}
	/* A debug cable only. */
	dev_info(smc->dev, "%s: %u messages in %u ms, the last at %u ms%s\n", after, count, ms,
		 last, ms >= SMC_OUTBOX_SETTLE_MS ? ", still busy" : "");
}

/* The noirq suspend's part, polled: the cut, a late warning, the commit's notices. */
static int apple_smc_sleep_enter(struct apple_smc *smc)
{
	if (apple_sleep_debug_link_cut())
		apple_smc_outbox_poll(smc, "debug link cut", 0, SMC_OUTBOX_QUIET_MS);
	if (smc->sleep_warn_late) {
		smc->sleep_warn_late = false;
		apple_smc_sleep_warn(smc);
		apple_smc_outbox_poll(smc, "late sleep warning", SMC_SLEEP_WARN_MS, 0);
	}

	return apple_smc_sleep_commit(smc);
}

static int apple_smc_suspend_noirq(struct device *dev)
{
	struct apple_smc *smc = dev_get_drvdata(dev);
	int ret;

	if (smc->sleep_policy)
		return 0; /* Deferred to shared final returning check, before last_stop. */
	if (!smc->sleep_warned && !smc->sleep_warn_late)
		return 0;

	apple_smc_set_polled(smc, true);
	ret = apple_smc_sleep_enter(smc);
	apple_smc_set_polled(smc, false);

	return ret;
}

static int apple_smc_resume_noirq(struct device *dev)
{
	struct apple_smc *smc = dev_get_drvdata(dev);

	if (smc->saved_enabled && (smc->saved_recovery || smc->saved_pending ||
				   smc->saved_fault))
		return smc->saved_restore_error ?: smc->saved_fault ?: -EBUSY;
	if (smc->saved_enabled && smc->sleep_policy) {
		if (smc->saved_wake) {
			apple_smc_set_polled(smc, true);
			apple_smc_sleep_wake(smc);
			apple_smc_set_polled(smc, false);
			smc->saved_wake = false;
		}
		return smc->saved_recovery ? smc->saved_restore_error : 0;
	}
	if (smc->ap_notified) {
		apple_smc_set_polled(smc, true);
		apple_smc_sleep_wake(smc);
		apple_smc_set_polled(smc, false);
	}

	return 0;
}

static const struct dev_pm_ops apple_smc_pm_ops = {
	.prepare = pm_sleep_ptr(apple_smc_prepare),
	.suspend = pm_sleep_ptr(apple_smc_suspend),
	.complete = pm_sleep_ptr(apple_smc_complete),
	NOIRQ_SYSTEM_SLEEP_PM_OPS(apple_smc_suspend_noirq, apple_smc_resume_noirq)
};

static void apple_smc_remove(struct platform_device *pdev)
{
	/* Driver core holds device_lock: the sleep's PM scope must have ended
	 * before this can run, and unregister excludes future scope setup.
	 */
	apple_sleep_smc_unregister(&pdev->dev);
}

static struct platform_driver apple_smc_driver = {
	.driver = {
		.name = "macsmc",
		.of_match_table = apple_smc_of_match,
		.pm = pm_sleep_ptr(&apple_smc_pm_ops),
	},
	.probe = apple_smc_probe,
	.remove = apple_smc_remove,
};
module_platform_driver(apple_smc_driver);

MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_AUTHOR("Sven Peter <sven@kernel.org>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC driver");

#if IS_ENABLED(CONFIG_MFD_MACSMC_KUNIT_TEST)
#include "macsmc-test.c"
#endif
