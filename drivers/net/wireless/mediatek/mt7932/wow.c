// SPDX-License-Identifier: GPL-2.0-only
/*
 * Wake-on-WLAN. The firmware keeps the association while the host sleeps and
 * filters what it receives. From a deep sleep it wakes the host through the
 * radio's GPIO, which the SMC turns into an AP wake: nothing in-band can wake
 * the SoC from S2R. Suspend-to-idle keeps the host interface running, and the
 * firmware's report of the wake, serviced while the system sleeps, ends it.
 */
#include <linux/suspend.h>

#include "mt7932.h"
#include "wow.h"

#ifdef CONFIG_PM
const struct wiphy_wowlan_support mt_wowlan_support = {
	.flags = WIPHY_WOWLAN_MAGIC_PKT | WIPHY_WOWLAN_DISCONNECT,
};

/* response_lock held. */
static bool mt_wow_associated_locked(struct mt7932 *m)
{
	return m->connected && m->peer_valid && !m->connecting && !m->disconnecting &&
	       !m->link_failed && !m->stopping && m->rf_ready;
}

/*
 * cfg80211 calls this before the device's own suspend. Returning 1 makes it
 * leave the network and call again without a configuration.
 */
int mt_wow_suspend(struct wiphy *wiphy, struct cfg80211_wowlan *wow)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	u8 detect = mt7932_wow_detect(wow);
	unsigned long flags;
	bool associated;

	WRITE_ONCE(m->wow_detect, 0);
	if (!wow)
		return 0;
	/* Hibernation leaves the network; both kinds of suspend hand the radio to the firmware. */
	if (!detect || mt7932_wow_sleep(pm_suspend_target_state, pm_suspend_no_platform()) ==
		       MT7932_WOW_SLEEP_NONE)
		return 1;
	spin_lock_irqsave(&m->response_lock, flags);
	associated = mt_wow_associated_locked(m);
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (!associated)
		return 1;
	WRITE_ONCE(m->wow_detect, detect);
	return 0;
}

int mt_wow_resume(struct wiphy *wiphy)
{
	WRITE_ONCE(mt_from_wiphy(wiphy)->wow_detect, 0);
	return 0;
}

/*
 * command_mutex held, before the host interface stops. The firmware takes the
 * commands from the ring before it answers the stop.
 */
void mt_wow_arm(struct mt7932 *m)
{
	u8 detect = READ_ONCE(m->wow_detect);
	u8 body[MT7932_WOW_CTRL_LENGTH];
	unsigned long flags;
	bool associated;
	int ret = 0;

	lockdep_assert_held(&m->command_mutex);
	WRITE_ONCE(m->wow_detect, 0);
	if (!detect)
		return;
	spin_lock_irqsave(&m->response_lock, flags);
	associated = mt_wow_associated_locked(m);
	m->wow_report = false;
	m->wow_reason = MT7932_WOW_REASON_NONE;
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (!associated) {
		dev_dbg(&m->pdev->dev, "not associated at the sleep: Wi-Fi wake not armed\n");
		return;
	}
	/* Whatever reached the firmware is undone at the wake. */
	m->wow_armed = true;
	if (detect & MT7932_WOW_DETECT_MAGIC) {
		mt7932_wow_magic(body, m->mac);
		ret = mt_request(m, MT7932_CID_WOW_CONFIG, true, true, false, body,
				 MT7932_WOW_MAGIC_LENGTH);
	}
	if (!ret)
		ret = mt_power_submit(m, MT7932_WOW_POWER_MODE);
	if (!ret) {
		mt7932_suspend_mode(body, true);
		ret = mt_request(m, MT7932_CID_SUSPEND_MODE, true, true, false, body,
				 MT7932_SUSPEND_MODE_LENGTH);
	}
	if (!ret) {
		mt7932_wow_ctrl(body, true, detect);
		ret = mt_request(m, MT7932_CID_WOW_CTRL, true, true, false, body,
				 MT7932_WOW_CTRL_LENGTH);
	}
	if (ret) {
		dev_err(&m->pdev->dev, "Wi-Fi wake not armed: %d\n", ret);
		return;
	}
	spin_lock_irqsave(&m->response_lock, flags);
	m->wow_report = true;
	spin_unlock_irqrestore(&m->response_lock, flags);
	dev_dbg(&m->pdev->dev, "Wi-Fi wake armed:%s%s, staying associated\n",
		detect & MT7932_WOW_DETECT_MAGIC ? " magic packet" : "",
		detect & MT7932_WOW_DETECT_DISCONNECT ? " disconnect" : "");
}

/*
 * command_mutex held, once the host interface runs again. A command that
 * cannot be queued leaves the firmware filtering, so the caller resets it.
 */
int mt_wow_disarm(struct mt7932 *m)
{
	u8 body[MT7932_WOW_CTRL_LENGTH];
	int ret;

	lockdep_assert_held(&m->command_mutex);
	if (!m->wow_armed)
		return 0;
	m->wow_armed = false;
	mt7932_suspend_mode(body, false);
	ret = mt_request(m, MT7932_CID_SUSPEND_MODE, true, true, false, body,
			 MT7932_SUSPEND_MODE_LENGTH);
	if (!ret) {
		mt7932_wow_ctrl(body, false, 0);
		ret = mt_request(m, MT7932_CID_WOW_CTRL, true, true, false, body,
				 MT7932_WOW_CTRL_LENGTH);
	}
	if (!ret) {
		mt7932_wow_clear(body);
		ret = mt_request(m, MT7932_CID_WOW_CONFIG, true, true, false, body,
				 MT7932_WOW_CLEAR_LENGTH);
	}
	/* The sleep's profile stays until power_work submits the requested one. */
	m->power_applied_valid = false;
	if (ret)
		dev_err(&m->pdev->dev, "Wi-Fi wake not stopped: %d\n", ret);
	return ret;
}

/* response_lock held. The firmware reports why it woke the host. */
void mt_wow_reason_event(struct mt7932 *m, const struct mt7932_event *event)
{
	int reason = event->length < 36 ? -EINVAL :
		     mt7932_wow_reason(event->packet + 36, event->length - 36);

	if (reason < 0) {
		dev_dbg(&m->pdev->dev, "short wake reason event: %zu bytes\n", event->length);
		return;
	}
	if (!m->wow_report)
		return;
	m->wow_report = false;
	m->wow_reason = reason;
	schedule_work(&m->wow_work);
	/* In suspend-to-idle this report is the wake; nothing else from the firmware is. */
	if (READ_ONCE(m->wow_idle) && reason != MT7932_WOW_REASON_NONE)
		pm_system_wakeup();
}

void mt_wow_work(struct work_struct *work)
{
	struct mt7932 *m = container_of(work, struct mt7932, wow_work);
	struct cfg80211_wowlan_wakeup wakeup;
	unsigned long flags;
	u8 reason;

	spin_lock_irqsave(&m->response_lock, flags);
	reason = m->wow_reason;
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (!mt7932_wow_wakeup(reason, &wakeup)) {
		dev_dbg(&m->pdev->dev, "Wi-Fi did not wake the host\n");
		cfg80211_report_wowlan_wakeup(&m->wdev, NULL, GFP_KERNEL);
		return;
	}
	dev_info(&m->pdev->dev, "woken by Wi-Fi: firmware reason %u%s\n", reason,
		 wakeup.magic_pkt ? " (magic packet)" :
		 wakeup.disconnect ? " (disconnect)" : "");
	cfg80211_report_wowlan_wakeup(&m->wdev, &wakeup, GFP_KERNEL);
}
#else
void mt_wow_arm(struct mt7932 *m)
{
}

int mt_wow_disarm(struct mt7932 *m)
{
	return 0;
}

void mt_wow_reason_event(struct mt7932 *m, const struct mt7932_event *event)
{
}

void mt_wow_work(struct work_struct *work)
{
}
#endif
