// SPDX-License-Identifier: GPL-2.0-only
#include "mt7932.h"

/* Stock CID05 is a four-byte SET with no requested response. Successful
 * submission is accepted configuration, not proof that the radio is asleep.
 * cfg80211 implements GET_POWER_SAVE from the accepted requested setting.
 */
static int mt_power_submit(struct mt7932 *m, u8 mode)
{
	u8 body[4] = { 0, mode, 0, 0 }; /* owned BSS0, not a station/WTBL ID */
	int ret;

	ret = mt_request(m, 5, true, true, false, body, sizeof(body));
	m->power_applied_valid = !ret;
	if (!ret)
		m->power_applied = mode;
	return ret;
}

/* command_mutex held; each new BSS session starts explicitly awake. */
int mt_power_init(struct mt7932 *m)
{
	unsigned long flags;

	spin_lock_irqsave(&m->response_lock, flags);
	m->power_tim = false;
	spin_unlock_irqrestore(&m->response_lock, flags);
	return mt_power_submit(m, 0);
}

/* command_mutex fences BSS reuse/retirement while the SET is submitted. */
static int mt_power_apply(struct mt7932 *m, u8 mode)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&m->response_lock, flags);
	if (m->stopping || !mt_rf_allowed(m))
		ret = -ESHUTDOWN;
	else if (!m->connected || m->disconnecting || !m->peer_valid)
		ret = -ENOTCONN;
	else if (mode && !m->power_tim)
		ret = -EAGAIN;
	spin_unlock_irqrestore(&m->response_lock, flags);
	if (ret)
		return ret;
	if (m->power_applied_valid && m->power_applied == mode)
		return 0;
	return mt_power_submit(m, mode);
}

int mt_set_power_mgmt(struct wiphy *wiphy, struct net_device *netdev,
		      bool enabled, int timeout)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	u8 mode = enabled ? (timeout == -1 ? 2 : 1) : 0;
	int ret = 0;

	if (netdev != m->netdev)
		return -ENODEV;
	/* Stock maps positive timeouts to mode1; it does not send a timer value. */
	if (timeout < -1)
		return -EINVAL;
	mutex_lock(&m->command_mutex);
	if (READ_ONCE(m->stopping))
		ret = -ESHUTDOWN;
	else if (READ_ONCE(m->disconnecting))
		ret = -EBUSY;
	else if (READ_ONCE(m->connected))
		ret = mt_power_apply(m, mode);
	/* Disconnected desired policy is applied after association/TIM discovery. */
	if (!ret)
		m->power_mode = mode;
	mutex_unlock(&m->command_mutex);
	return ret;
}

void mt_power_work(struct work_struct *work)
{
	struct mt7932 *m = container_of(work, struct mt7932, power_work);
	int ret;

	mutex_lock(&m->command_mutex);
	ret = mt_power_apply(m, m->power_mode);
	if (ret && ret != -EAGAIN && ret != -ENOTCONN && ret != -ESHUTDOWN)
		dev_warn(&m->pdev->dev, "station power-save submission failed: %d\n", ret);
	mutex_unlock(&m->command_mutex);
}
