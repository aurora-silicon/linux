// SPDX-License-Identifier: GPL-2.0-only
/*
 * Wake on WLAN for the firmware-owned station: magic packet and disconnect.
 *
 * The function keeps running across suspend-to-idle (see mt_suspend()), so
 * the firmware stays associated with its rings and the MSI intact. A START
 * makes it stop delivering traffic and interrupt the host only for a wake
 * condition; the MSI is then armed as a system wake interrupt. On resume the
 * interrupt is disarmed, a STOP returns the firmware to normal delivery, and
 * the reason event that the firmware sends is reported to cfg80211.
 */
#include "mt7932.h"
#include "wow.h"

/* Bounded wait for a reason event the firmware has not delivered yet. */
#define MT7932_WOW_REASON_WAIT_MS 100

static const struct wiphy_wowlan_support mt_wowlan_support = {
	.flags = WIPHY_WOWLAN_MAGIC_PKT | WIPHY_WOWLAN_DISCONNECT,
};

/*
 * A SET whose command descriptor the device has consumed. Whether the
 * firmware then acts on it is what the qualification of the wake shows.
 */
static int mt_wow_retired(struct mt7932 *m)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(500);

	do {
		mt_tx_clean(&m->tx[17]);
		if (!m->tx[17].queued)
			return 0;
		usleep_range(100, 200);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

/* command_mutex held, or probe before the wiphy exists. */
static int mt_wow_send(struct mt7932 *m, u8 command, u8 triggers)
{
	u8 param[MT7932_WOW_PARAM_LEN];
	int ret;

	mt7932_wow_param(param, command, triggers);
	ret = mt_request(m, MT7932_CMD_SET_WOWLAN, true, true, false, param, sizeof(param));
	if (!ret)
		ret = mt_wow_retired(m);
	dev_dbg(&m->pdev->dev, "WOW_%s: triggers=%02x status=%d\n",
		command == MT7932_WOW_START ? "START" : "STOP", triggers, ret);
	return ret;
}

/* This driver binds only the hardware and firmware family qualified for WoW. */
void mt_wow_init(struct mt7932 *m)
{
	m->wow_reason = -ENODATA;
	m->wow_supported = true;
	m->wiphy->wowlan = &mt_wowlan_support;
	device_set_wakeup_capable(&m->pdev->dev, true);
}

/* response_lock held. */
void mt_wow_event_locked(struct mt7932 *m, const struct mt7932_event *event)
{
	int reason = mt7932_wow_reason(event);

	if (reason < 0) {
		dev_warn_ratelimited(&m->pdev->dev, "malformed WoWLAN wake event, %zu bytes\n",
				     event->length);
		return;
	}
	m->wow_reason = reason;
	complete(&m->wow_event);
}

/*
 * Returning 1 makes cfg80211 leave the network and suspend without WoWLAN,
 * which is what happens without a WoWLAN configuration. A START is only sent
 * for a settled association; a failed one is undone with a STOP.
 */
/*
 * Select the per-BSS WoW DTIM policy before START, and clear the BSS suspend
 * state on resume while preserving the selector in the fixed public shape.
 */
static int mt_wow_suspend_mode(struct mt7932 *m, bool enable)
{
	u8 body[MT7932_SUSPEND_PARAM_LEN];
	int ret;

	mt7932_suspend_param(body, enable);
	ret = mt_request(m, MT7932_CMD_SUSPEND_MODE, true, true, false,
			 body, sizeof(body));
	if (!ret)
		ret = mt_wow_retired(m);
	dev_dbg(&m->pdev->dev, "WOW_SUSPEND_MODE: enable=%u status=%d\n", enable, ret);
	return ret;
}

/* Firmware debug events would otherwise wake the host immediately. */
static int mt_wow_log(struct mt7932 *m, bool quiet)
{
	u8 body[4] = { quiet ? 0 : 2 };
	int ret = mt_request(m, 0xc5, true, true, false, body, sizeof(body));

	if (!ret)
		ret = mt_wow_retired(m);
	dev_dbg(&m->pdev->dev, "WOW_LOG: quiet=%u status=%d\n", quiet, ret);
	return ret;
}

/*
 * Public legacy CMD_ID_HIF_CTRL (0xf6): PCIe, TX, stop or restart. This is
 * the bounded transition gate: entering sleep requires its event to report
 * both TX and RX idle.
 */
static int mt_wow_hif(struct mt7932 *m, bool stop)
{
	struct mt7932_event event;
	u8 body[MT7932_HIF_PARAM_LEN];
	int ret;

	mt7932_hif_param(body, stop);
	ret = mt_request(m, MT7932_CMD_HIF_CTRL, true, true, true, body, sizeof(body));
	if (!ret)
		ret = mt7932_event_parse(m->reply, m->reply_length, &event);
	if (!ret)
		ret = stop ? mt7932_hif_idle(&event) : mt7932_hif_event(&event);
	dev_dbg(&m->pdev->dev, "WOW_HIF: stop=%u status=%d\n", stop, ret);
	return ret;
}

int mt_wow_suspend(struct wiphy *wiphy, struct cfg80211_wowlan *wow)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	unsigned long flags;
	bool settled;
	u8 triggers;
	int ret;

	if (!wow)
		return 0;
	triggers = mt7932_wow_triggers(wow->magic_pkt, wow->disconnect);
	if (!m->wow_supported || !triggers || !device_may_wakeup(&m->pdev->dev))
		return 1;
	mutex_lock(&m->command_mutex);
	spin_lock_irqsave(&m->response_lock, flags);
	settled = mt_rf_allowed(m) && m->connected && m->peer_valid && !m->connecting &&
		  !m->disconnecting && !m->roam_moving;
	if (settled) {
		m->wow_reason = -ENODATA;
		reinit_completion(&m->wow_event);
	}
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (!settled) {
		mutex_unlock(&m->command_mutex);
		return 1;
	}
	{
		/* Per-BSS power save for sleep (CID05 mode 2); power_work restores
		 * the requested mode after resume.
		 */
		u8 ps[4] = { 0, 2, 0, 0 };
		int r = mt_request(m, 5, true, true, false, ps, sizeof(ps));

		if (!r)
			r = mt_wow_retired(m);
		m->power_applied_valid = false;
		dev_dbg(&m->pdev->dev, "WOW_PS: mode=2 status=%d\n", r);
	}
	ret = mt_wow_log(m, true);
	if (!ret)
		ret = mt_wow_suspend_mode(m, true);
	if (!ret)
		ret = mt_wow_send(m, MT7932_WOW_START, triggers);
	if (!ret)
		ret = mt_wow_hif(m, true);
	if (ret) {
		mt_wow_hif(m, false);
		mt_wow_suspend_mode(m, false);
		mt_wow_send(m, MT7932_WOW_STOP, 0);
		mt_wow_log(m, false);
		schedule_work(&m->power_work);
		mutex_unlock(&m->command_mutex);
		dev_warn(&m->pdev->dev, "WoWLAN transition failed: %d; suspending without WoWLAN\n",
			 ret);
		return 1;
	}
	m->wow_wake_signal = false;
	m->wow_armed = true;
	mutex_unlock(&m->command_mutex);
	return 0;
}

int mt_wow_resume(struct wiphy *wiphy)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	struct cfg80211_wowlan_wakeup wakeup = { .pattern_idx = -1 };
	unsigned long flags;
	int reason, ret;

	mutex_lock(&m->command_mutex);
	if (!m->wow_armed) {
		mutex_unlock(&m->command_mutex);
		return 0;
	}
	m->wow_armed = false;
	/* The PCI resume has restored event delivery: the reason event may
	 * already be in, or arrive around the STOP. Do not wait for one that
	 * a wake from another source never brings beyond a short bound.
	 */
	ret = mt_wow_hif(m, false);
	if (ret)
		dev_err(&m->pdev->dev, "WoWLAN HIF resume failed: %d\n", ret);
	ret = mt_wow_suspend_mode(m, false);
	if (ret)
		dev_err(&m->pdev->dev, "WoWLAN suspend-policy clear failed: %d\n", ret);
	ret = mt_wow_send(m, MT7932_WOW_STOP, 0);
	if (ret)
		dev_err(&m->pdev->dev, "WoWLAN STOP failed: %d\n", ret);
	mt_wow_log(m, false);
	schedule_work(&m->power_work);
	wait_for_completion_timeout(&m->wow_event, msecs_to_jiffies(MT7932_WOW_REASON_WAIT_MS));
	spin_lock_irqsave(&m->response_lock, flags);
	reason = m->wow_reason;
	m->wow_reason = -ENODATA;
	spin_unlock_irqrestore(&m->response_lock, flags);
	mutex_unlock(&m->command_mutex);

	if (reason < 0) {
		dev_dbg(&m->pdev->dev, "WOW_RESUME: signal=%u, no WLAN wake reason\n",
			m->wow_wake_signal);
		cfg80211_report_wowlan_wakeup(&m->wdev, NULL, GFP_KERNEL);
		return 0;
	}
	if (reason == MT7932_WOW_REASON_MAGIC)
		wakeup.magic_pkt = true;
	else if (reason == MT7932_WOW_REASON_DISCONNECT)
		wakeup.disconnect = true;
	dev_info(&m->pdev->dev, "WoWLAN wake reason %d (%s)\n", reason,
		 wakeup.magic_pkt ? "magic packet" : wakeup.disconnect ? "disconnect" : "other");
	cfg80211_report_wowlan_wakeup(&m->wdev, &wakeup, GFP_KERNEL);
	return 0;
}

void mt_wow_set_wakeup(struct wiphy *wiphy, bool enabled)
{
	device_set_wakeup_enable(&mt_from_wiphy(wiphy)->pdev->dev, enabled);
}

/* The PCI suspend callback, after the wiphy's START: arm the MSI to wake. */
void mt_wow_arm_irq(struct mt7932 *m)
{
	int ret;

	if (!m->wow_armed || m->wake_irq_armed)
		return;
	ret = enable_irq_wake(pci_irq_vector(m->pdev, 0));
	if (ret) {
		dev_warn(&m->pdev->dev, "WoWLAN wake interrupt unavailable: %d\n", ret);
		return;
	}
	m->wake_irq_armed = true;
	/* Only the MCU command interrupt may wake the sleeping host. */
	mt_write(m, W + 0x204, MT7932_MCU_CMD_IRQ);
	mt_write(m, W + 0x1f4, MT7932_MCU_WAKE_RX_PCIE);
	/* Quiesce host DMA (WFDMA TX/RX enable off), then hand the function to
	 * the firmware (connac FW_OWN, as mt76 does before PCIe sleep), so that
	 * it does the matching itself.
	 */
	mt_rmw(m, W + 0x208, BIT(0) | BIT(2), 0);
	ret = mt_poll(m, W + 0x208, BIT(1) | BIT(3), 0, 100000);
	mt_write(m, 0xe0010, 1);
	m->wow_fw_own = !mt_poll(m, 0xe0010, BIT(2), BIT(2), 50000);
	dev_dbg(&m->pdev->dev, "WOW_SLEEP: dma-idle=%d fw-own=%u glo=%08x\n", ret,
		 m->wow_fw_own, mt_read(m, W + 0x208));
}

void mt_wow_irq(struct mt7932 *m, u32 cause, u32 software)
{
	if (READ_ONCE(m->wake_irq_armed) && mt7932_wow_wake_signal(cause, software))
		WRITE_ONCE(m->wow_wake_signal, true);
}

/* The PCI resume callback, before the wiphy's STOP. */
void mt_wow_disarm_irq(struct mt7932 *m)
{
	u32 cause, software = 0;

	if (!m->wake_irq_armed)
		return;
	/* The wake IRQ may not have run its thread while noirq was suspended.
	 * Decode and acknowledge the same W1C pair here before normal causes
	 * and DMA are restored.
	 */
	cause = mt_read(m, W + 0x200);
	if (cause & MT7932_MCU_CMD_IRQ) {
		software = mt_read(m, W + 0x1f0);
		mt_wow_irq(m, cause, software);
		mt_write(m, W + 0x1f0, software);
		mt_write(m, W + 0x200, MT7932_MCU_CMD_IRQ);
		mt_read(m, W + 0x200);
	}
	{
		int own = -1;

		if (m->wow_fw_own) {
			mt_write(m, 0xe0010, 2);
			own = mt_poll(m, 0xe0010, BIT(2), 0, 50000);
			m->wow_fw_own = false;
		}
		mt_rmw(m, W + 0x208, 0, BIT(0) | BIT(2));
		dev_dbg(&m->pdev->dev,
			"WOW_WAKE: driver-own=%d signal=%u cause=%08x sw=%08x glo=%08x raw=%08x\n",
			own, m->wow_wake_signal, cause, software, mt_read(m, W + 0x208),
			mt_read(m, W + 0x200));
	}
	disable_irq_wake(pci_irq_vector(m->pdev, 0));
	m->wake_irq_armed = false;
	mt_write(m, W + 0x1f4, 0xffff);
	mt_write(m, W + 0x204, m->irq_mask);
}
