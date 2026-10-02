// SPDX-License-Identifier: GPL-2.0-only
#include "mt7932.h"

/* Research helper, deliberately not called by production station reporting:
 * the reply identity is per-peer, but its vector is shared and unattributed.
 * Retained while qualifying a real per-peer rate source, not a public metric.
 * One serialized query for the owned peer. Keep the command owner until the
 * reply has been checked; disconnect may close admission in IRQ context but
 * cannot reuse BSS/station resources across this critical section.
 */
void mt_station_update_rate(struct mt7932 *m, const u8 *mac)
{
	struct mt7932_event event;
	struct rate_info rate;
	u8 body[28] = {}, peer[ETH_ALEN];
	u32 generation;
	unsigned long flags;
	bool valid = false;
	int ret;

	mutex_lock(&m->command_mutex);
	spin_lock_irqsave(&m->response_lock, flags);
	if (!m->connected || m->disconnecting || !m->peer_valid || !mt_rf_allowed(m) ||
	    m->station_rate_query_failed || (mac && !ether_addr_equal(mac, m->connect_bssid)) ||
	    time_before(jiffies, m->station_rate_query_time + HZ / 2)) {
		spin_unlock_irqrestore(&m->response_lock, flags);
		goto unlock;
	}
	generation = m->connection_generation;
	ether_addr_copy(peer, m->connect_bssid);
	body[0] = m->peer_ids.station[0];
	memcpy(body + 4, peer, ETH_ALEN);
	m->station_rate_query_time = jiffies;
	spin_unlock_irqrestore(&m->response_lock, flags);

	ret = mt_request(m, 0x85, true, false, true, body, sizeof(body));
	if (!ret)
		ret = mt7932_event_parse(m->reply, m->reply_length, &event);
	if (!ret)
		valid = mt7932_tx_rate_reply(&event, m->seq, peer, m->phy_cap[4], &rate);
	if (valid && (rate.bw != RATE_INFO_BW_20 ||
	    (m->connect_channel > 14 && !rate.flags &&
	     (rate.legacy == 10 || rate.legacy == 20 || rate.legacy == 55 || rate.legacy == 110))))
		valid = false;

	spin_lock_irqsave(&m->response_lock, flags);
	if (generation == m->connection_generation && m->connected &&
	    !m->disconnecting && m->peer_valid && ether_addr_equal(peer, m->connect_bssid)) {
		/* A timed-out command must not turn every station dump into another
		 * five-second wait. A new connection/module epoch may try again.
		 */
		if (ret)
			m->station_rate_query_failed = true;
		spin_lock(&m->data_lock);
		m->station_tx_rate_valid = valid;
		if (valid) {
			m->station_tx_rate = rate;
			m->station_tx_rate_time = jiffies;
		}
		spin_unlock(&m->data_lock);
	}
	spin_unlock_irqrestore(&m->response_lock, flags);
unlock:
	mutex_unlock(&m->command_mutex);
}
