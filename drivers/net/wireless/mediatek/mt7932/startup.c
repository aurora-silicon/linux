/* SPDX-License-Identifier: GPL-2.0-only */
#include "mt7932.h"

static int mt_stock_config(struct mt7932 *m);
static int mt_publish_policy(struct mt7932 *m, const struct mt7932_reg_snapshot *reg,
			     const struct mt7932_policy *policy);
static int mt_privacy_defaults(struct mt7932 *m);
static int mt_startup_once(struct mt7932 *m);
static void mt_startup_work(struct work_struct *work);
static void mt_regulatory_notify(struct wiphy *wiphy, struct regulatory_request *request);

struct mt_config_record {
	u8 type, key_len, value_len, reserved;
	char key[32], value[32];
};

static int mt_stock_config(struct mt7932 *m)
{
	static const struct { const char *key; u8 at, fallback; } updates[] = {
		{"StaVHT",1,0}, {"ApVHT",1,1}, {"P2pGoVHT",1,1}, {"P2pGcVHT",1,1},
		{"Nss",4,0}, {"LdpcTx",6,0}, {"LdpcRx",7,0}, {"StbcTx",8,0}, {"StbcRx",9,0},
	};
	struct mt_config_record *records;
	const struct firmware *file;
	u8 body[284];
	unsigned int count, i, j, batch;
	int ret;

	ret = request_firmware_direct(&file, "mediatek/mt7932/config-original.bin", &m->pdev->dev);
	if (ret)
		return ret;
	ret = -EINVAL;
	if (file->size < 8 || memcmp(file->data, "J7CF", 4))
		goto out;
	count = get_unaligned_le32(file->data + 4);
	if ((count != 64 && count != 65) || file->size != 8 + count * 68)
		goto out;
	/*
	 * Each absent normalized key appends one record. Reserve for all of
	 * them, including structurally valid packages missing every such key.
	 */
	records = kcalloc(count + ARRAY_SIZE(updates), sizeof(*records), GFP_KERNEL);
	if (!records) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(records, file->data + 8, count * 68);
	for (i = 0; i < count; i++) {
		if (records[i].type != 3 || !records[i].key_len || records[i].key_len >= 32 ||
		    records[i].value_len >= 32 || records[i].reserved ||
		    strnlen(records[i].key, 32) != records[i].key_len ||
		    strnlen(records[i].value, 32) != records[i].value_len)
			goto free;
		for (j = 0; j < i; j++)
			if (!strcmp(records[i].key, records[j].key))
				goto free;
	}
	/* The measured J700 profile has PHY DBDC=1. A different capability needs
	 * the separately specified conditional DbdcMode insertion, not a default.
	 */
	if (!m->phy_cap[5]) {
		ret = -EOPNOTSUPP;
		goto free;
	}
	for (i = 0; i < ARRAY_SIZE(updates); i++) {
		u8 requested = updates[i].fallback, value;

		for (j = 0; j < count; j++)
			if (!strcmp(records[j].key, updates[i].key))
				break;
		if (j < count) {
			if (kstrtou8(records[j].value, 0, &requested))
				goto free;
		} else {
			count++;
			records[j].type = 3;
			records[j].key_len = strscpy(records[j].key, updates[i].key, 32);
		}
		value = updates[i].at == 4 ? min(requested, m->phy_cap[4]) : requested & m->phy_cap[updates[i].at];
		memset(records[j].value, 0, 32);
		records[j].value_len = scnprintf(records[j].value, 32, "0x%x", value);
	}
	{
		/* Same ceiling as BASIC_CONFIG, without changing original artifacts.
		 * CID0f uses a distinct enum; keep Sta2gBw untouched.
		 */
		for (j = 0; j < count; j++)
			if (!strcmp(records[j].key, "Sta5gBw"))
				break;
		if (j == count) {
			ret = -EINVAL;
			goto free;
		}
		memset(records[j].value, 0, sizeof(records[j].value));
		records[j].value[0] = '0' + MT7932_MAX_5G_BW;
		records[j].value_len = 1;
		for (j = 0; j < count; j++)
			if (!strcmp(records[j].key, "DisRoaming"))
				break;
		if (j == count) {
			ret = -EINVAL;
			goto free;
		}
		memset(records[j].value, 0, sizeof(records[j].value));
		records[j].value[0] = '1';
		records[j].value_len = 1;
	}
	for (i = 0; i < count; i += batch) {
		batch = min(4U, count - i);
		memset(body, 0, sizeof(body));
		put_unaligned_le32(1, body + 4);
		body[8] = batch;
		put_unaligned_le16(batch * 68, body + 10);
		memcpy(body + 12, records + i, batch * 68);
		ret = mt_request(m, 0x70, true, true, false, body, sizeof(body));
		if (ret)
			goto free;
	}
	dev_info(&m->pdev->dev, "STOCK_CONFIG_SUBMITTED: %u original/PHY-derived entries\n", count);
free:
	kfree(records);
out:
	release_firmware(file);
	return ret;
}

static int mt_publish_policy(struct mt7932 *m, const struct mt7932_reg_snapshot *reg,
			     const struct mt7932_policy *policy)
{
	u8 dbdc[36] = {0,0,0,0,2,0,36};
	unsigned int i;
	int ret;

	if (!m->startup_started) {
		ret = mt_request(m, 0x28, true, true, false, dbdc, sizeof(dbdc));
		if (ret)
			return ret;
	}
	for (i = 0; i < policy->modes; i++) {
		ret = mt_request(m, 0xca, true, true, false, policy->mode[i], 328);
		if (ret)
			return ret;
	}
	ret = mt_request(m, 0x0f, true, true, false, reg->domain, reg->length);
	if (ret)
		return ret;
	for (i = 0; i < MT7932_POLICY_TABLES; i++) {
		ret = mt_request(m, 0x5d, true, true, false, policy->table[i], policy->length[i]);
		if (ret)
			return ret;
	}
	/* Native SETs have no response/atomic policy-commit event. Command
	 * submission is not misrepresented as firmware readback or an ACK.
	 */
	dev_info(&m->pdev->dev, "REGULATORY_POLICY_SUBMITTED: %c%c firmware=%c%c channels=%u (native no-response SETs)\n",
		 reg->alpha2[0], reg->alpha2[1], reg->domain[0], reg->domain[1],
		 (reg->length - 12) / 8);
	return 0;
}

static int mt_privacy_defaults(struct mt7932 *m)
{
	static const unsigned int ids[] = {0,1,3,45,50,72,107,108,127,191,221,255};
	u8 body[60] = {};
	unsigned int i;
	int ret;

	if (m->stof_supported) {
		body[0] = 5;
		put_unaligned_le16(32, body + 2);
		put_unaligned_le32(6000, body + 4);
		put_unaligned_le32(6000, body + 20);
		ret = mt_request(m, 0x92, true, true, false, body, 36);
		if (ret)
			return ret;
	}
	memset(body, 0, sizeof(body));
	body[4] = 0x27;
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		body[7 + ids[i] / 8] |= 1U << (ids[i] % 8);
	body[43] = 8;
	body[46] = 4;
	ret = mt_request(m, 0x9b, true, true, false, body, sizeof(body));
	if (ret)
		return ret;
	memset(body, 0, sizeof(body));
	body[0] = 1;
	body[5] = body[6] = 0x14;
	ret = mt_request(m, 0x9b, true, true, false, body, sizeof(body));
	if (ret)
		return ret;
	memset(body, 0, 4);
	body[0] = 2;
	return mt_request(m, 0xc5, true, true, false, body, 4);
}

static int mt_startup_once(struct mt7932 *m)
{
	int ret;
	u8 rm_cap[44] = {1,0,0,0,0,1,0x70,0,2};
	/* Original fullmac platform roam lock, not a scan-disable/DMA fence.
	 * Legacy CID3e is distinct from the RTS extended CIDed/ext3e.
	 */
	u8 roam_lock[12] = {0,0,0,0,0,0,0,1};

	m->startup_started = true;
	ret = mt_request(m, 0x5a, true, true, false, rm_cap, sizeof(rm_cap));
	if (!ret)
		ret = mt_stock_config(m);
	if (!ret)
		ret = mt_privacy_defaults(m);
	if (!ret)
		ret = mt_calibration_gate(m);
	if (!ret)
		ret = mt_request(m, 0x3e, true, true, false, roam_lock, sizeof(roam_lock));
	dev_info(&m->pdev->dev, "OWN_CALIBRATION_STARTUP_RESULT: %d\n", ret);
	if (!ret)
		ret = mt_enable_scan(m);
	if (ret)
		dev_err(&m->pdev->dev, "SCAN_INTERFACE_SETUP_FAILED: %d\n", ret);
	return ret;
}

static void mt_startup_work(struct work_struct *work)
{
	struct mt7932 *m = container_of(work, struct mt7932, startup_work);
	struct mt7932_reg_snapshot reg;
	struct mt7932_policy policy;
	const struct firmware *file;
	unsigned long flags;
	u32 generation;
	char path[64];
	int ret;

	for (;;) {
		spin_lock_irqsave(&m->response_lock, flags);
		generation = m->reg_generation;
		reg = m->reg_desired;
		if (m->stopping || generation == m->reg_attempted) {
			spin_unlock_irqrestore(&m->response_lock, flags);
			return;
		}
		m->reg_attempted = generation;
		spin_unlock_irqrestore(&m->response_lock, flags);

		/* No command/wiphy lock while joining lifecycle workers. RX, D7
		 * calibration and TX-free remain alive until ownership is retired.
		 */
		if (m->netdev) {
			mt_disconnect(m->wiphy, m->netdev, WLAN_REASON_DEAUTH_LEAVING);
			/* Prefer natural completion: CID1b has no guaranteed stop ACK.
			 * reg_pending prevents another band batch being submitted.
			 */
			if (READ_ONCE(m->scan_request) || READ_ONCE(m->retired_scan_seq))
				wait_for_completion_timeout(&m->scan_done, msecs_to_jiffies(5000));
			mt_abort_scan(m->wiphy, &m->wdev);
			flush_work(&m->scan_finish_work);
			flush_work(&m->cal_work);
		}
		ret = reg.error;
		if (READ_ONCE(m->retired_scan_seq))
			ret = -EBUSY;
		file = NULL;
		if (READ_ONCE(m->policy_failed) || READ_ONCE(m->link_failed) ||
		    (m->startup_started && !READ_ONCE(m->rf_ready)))
			ret = -EIO;
		if (!ret) {
			if (!memcmp(reg.alpha2, "00", 2))
				scnprintf(path, sizeof(path),
					  "mediatek/mt7932/policy/world-XZ.bin");
			else
				scnprintf(path, sizeof(path), "mediatek/mt7932/policy/%c%c.bin",
					  reg.alpha2[0], reg.alpha2[1]);
			ret = request_firmware_direct(&file, path, &m->pdev->dev);
		}
		if (!ret)
			ret = mt7932_policy_parse(&policy, file->data, file->size, reg.domain);
		mutex_lock(&m->command_mutex);
		if (READ_ONCE(m->stopping) || generation != READ_ONCE(m->reg_generation))
			goto next;
		if (!ret) {
			ret = mt_publish_policy(m, &reg, &policy);
			if (!ret && !m->startup_started)
				ret = mt_startup_once(m);
			/* A partial SET sequence is not a rollback. Only missing or
			 * invalid files BEFORE submission are recoverable by a hint.
			 */
			if (ret)
				WRITE_ONCE(m->policy_failed, true);
		}
		spin_lock_irqsave(&m->response_lock, flags);
		if (!ret && generation == m->reg_generation)
			WRITE_ONCE(m->reg_pending, false);
		spin_unlock_irqrestore(&m->response_lock, flags);
		if (ret)
			dev_warn(&m->pdev->dev, "REGULATORY_BLOCKED: %c%c generation=%u error=%d recovery-required=%u\n",
				 reg.alpha2[0], reg.alpha2[1], generation, ret,
				 m->policy_failed || m->link_failed);
		else
			dev_info(&m->pdev->dev, "REGULATORY_READY: %c%c firmware=%c%c generation=%u current=%u\n",
				 reg.alpha2[0], reg.alpha2[1], reg.domain[0], reg.domain[1],
				 generation, !m->reg_pending);
next:
		mutex_unlock(&m->command_mutex);
		if (file)
			release_firmware(file);
	}
}

static void mt_regulatory_notify(struct wiphy *wiphy, struct regulatory_request *request)
{
	struct mt7932 *m = mt_from_wiphy(wiphy);
	struct mt7932_reg_snapshot reg = {};
	unsigned long irqflags;
	unsigned int i, count = 0;

	/* cfg80211 calls after updating channels under RTNL in this kernel.
	 * Copy, never retain the transient request or take command_mutex here.
	 */
	if (!request)
		return;
	memcpy(reg.alpha2, request->alpha2, 2);
	if (!((reg.alpha2[0] >= 'A' && reg.alpha2[0] <= 'Z' &&
	       reg.alpha2[1] >= 'A' && reg.alpha2[1] <= 'Z') ||
	      !memcmp(reg.alpha2, "00", 2)))
		reg.error = -EOPNOTSUPP;
	memcpy(reg.domain, reg.alpha2, 2);
	/* Stock database-present startup resolves the absent 00 row to XZ.
	 * Keep Linux's world channel restrictions and requested identity; only
	 * the firmware country/domain and original power package use XZ.
	 * Never fall back for an arbitrary unsupported explicit country.
	 */
	if (!memcmp(reg.alpha2, "00", 2))
		memcpy(reg.domain, "XZ", 2);
	reg.domain[4] = 1; /* 2 GHz remains 20 MHz. */
	reg.domain[5] = mt7932_domain_5g_bw();
	for (i = 0; i < 17; i++) {
		struct ieee80211_channel *channel = i < 13 ? &m->channels[i] : &m->channels5[i - 13];
		u32 flags = channel->flags;

		reg.power[i] = channel->max_power;
		if (flags & IEEE80211_CHAN_DISABLED)
			continue;
		if (flags & (IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_OFDM |
			     IEEE80211_CHAN_NO_20MHZ | IEEE80211_CHAN_PSD | IEEE80211_CHAN_IR_CONCURRENT))
			reg.error = -EOPNOTSUPP;
		/* Native per-rate units are not a proven dBm conversion. Do not
		 * silently ignore a numeric reduction below the qualified 20 dBm
		 * channel envelope or reinterpret sentinel bytes as power caps.
		 */
		if (channel->max_power < 20)
			reg.error = -EOPNOTSUPP;
		put_unaligned_le16(channel->hw_value, reg.domain + 12 + count * 8);
		/* Driver bandwidth limits are already in channel->flags. Adding
		 * the old blanket 20 MHz mask here would contradict the wiphy
		 * and silently force every native 5 GHz association back to 20.
		 */
		put_unaligned_le32(flags, reg.domain + 16 + count * 8);
		count++;
		reg.domain[i < 13 ? 8 : 9]++;
	}
	reg.length = 12 + count * 8;
	if (!count)
		reg.error = -EOPNOTSUPP;
	spin_lock_irqsave(&m->response_lock, irqflags);
	if (m->stopping) {
		spin_unlock_irqrestore(&m->response_lock, irqflags);
		return;
	}
	if (!memcmp(&reg, &m->reg_desired, sizeof(reg)) && !m->reg_pending) {
		spin_unlock_irqrestore(&m->response_lock, irqflags);
		return;
	}
	m->reg_desired = reg;
	m->reg_generation++;
	WRITE_ONCE(m->reg_pending, true);
	if (m->netdev) {
		netif_stop_queue(m->netdev);
		if (m->connecting) {
			m->connect_error = -ECANCELED;
			complete(&m->assoc_start);
			complete(&m->assoc_done);
			complete(&m->discovery_done);
		}
	}
	if (READ_ONCE(m->interface_registered))
		schedule_work(&m->startup_work);
	spin_unlock_irqrestore(&m->response_lock, irqflags);
	dev_info(&m->pdev->dev, "REGULATORY_REQUEST: %c%c initiator=%u generation=%u\n",
		 reg.alpha2[0], reg.alpha2[1], request->initiator, m->reg_generation);
}

static const struct cfg80211_ops mt_cfg_ops = {
	.scan = mt_scan, .abort_scan = mt_abort_scan,
	.connect = mt_connect, .disconnect = mt_disconnect,
	.deauth = mt_deauth,
	.change_virtual_intf = mt_change_interface,
	.get_station = mt_get_station,
	.get_channel = mt_get_channel,
	.dump_station = mt_dump_station,
	.set_power_mgmt = mt_set_power_mgmt,
};

int mt_register_regulatory_gate(struct mt7932 *m)
{
	static const int rates[] = {10,20,55,110,60,90,120,180,240,360,480,540};
	static const u32 ciphers[] = { WLAN_CIPHER_SUITE_CCMP };
	static const u32 akms[] = { WLAN_AKM_SUITE_PSK };
	unsigned int i;
	int ret;

	INIT_WORK(&m->startup_work, mt_startup_work);
	m->reg_pending = true;
	m->wiphy = wiphy_new(&mt_cfg_ops, sizeof(m));
	if (!m->wiphy)
		return -ENOMEM;
	*(struct mt7932 **)wiphy_priv(m->wiphy) = m;
	set_wiphy_dev(m->wiphy, &m->pdev->dev);
	ether_addr_copy(m->wiphy->perm_addr, m->mac);
	m->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	m->wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	m->wiphy->cipher_suites = ciphers;
	m->wiphy->n_cipher_suites = ARRAY_SIZE(ciphers);
	m->wiphy->akm_suites = akms;
	m->wiphy->n_akm_suites = ARRAY_SIZE(akms);
	wiphy_ext_feature_set(m->wiphy, NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK);
	m->wiphy->max_scan_ssids = 1;
	m->wiphy->max_scan_ie_len = 600;
	m->wiphy->reg_notifier = mt_regulatory_notify;
	/* World roaming may relax NO_IR after observing an AP beacon without
	 * changing alpha2. Request the callback so firmware's CID0f domain is
	 * updated through the same idle/scan fence as other regulatory changes.
	 */
	m->wiphy->flags |= WIPHY_FLAG_CHANNEL_CHANGE_ON_BEACON;
	/* The native fullmac path applies policy at idle, not seamlessly inside
	 * an association. Keep the existing explicit user/core-country model:
	 * accepting this STA's AP hint then disconnecting to apply it would make
	 * cfg80211 restore the user country and repeat the join forever.
	 * Global hints from other radios still reach the notifier and are handled.
	 */
	m->wiphy->regulatory_flags |= REGULATORY_COUNTRY_IE_IGNORE;
	for (i = 0; i < 13; i++) {
		m->channels[i].band = NL80211_BAND_2GHZ;
		m->channels[i].center_freq = 2412 + 5 * i;
		m->channels[i].hw_value = i + 1;
		m->channels[i].max_power = 20;
		m->channels[i].flags = IEEE80211_CHAN_NO_HT40 | IEEE80211_CHAN_NO_80MHZ | IEEE80211_CHAN_NO_160MHZ;
	}
	for (i = 0; i < ARRAY_SIZE(rates); i++) {
		m->rates[i].bitrate = rates[i];
		m->rates[i].hw_value = i;
	}
	m->band2.channels = m->channels;
	m->band2.n_channels = ARRAY_SIZE(m->channels);
	m->band2.bitrates = m->rates;
	m->band2.n_bitrates = ARRAY_SIZE(m->rates);
	m->wiphy->bands[NL80211_BAND_2GHZ] = &m->band2;
	{
		for (i = 0; i < ARRAY_SIZE(m->channels5); i++) {
			m->channels5[i].band = NL80211_BAND_5GHZ;
			m->channels5[i].center_freq = 5180 + 20 * i;
			m->channels5[i].hw_value = 36 + 4 * i;
			m->channels5[i].max_power = 20;
			m->channels5[i].flags = IEEE80211_CHAN_NO_160MHZ |
				(i % 2 ? IEEE80211_CHAN_NO_HT40PLUS : IEEE80211_CHAN_NO_HT40MINUS);
			if (MT7932_MAX_5G_BW < 2)
				m->channels5[i].flags |= IEEE80211_CHAN_NO_80MHZ;
			if (!MT7932_MAX_5G_BW)
				m->channels5[i].flags |= IEEE80211_CHAN_NO_HT40;
		}
		m->band5.channels = m->channels5;
		m->band5.n_channels = ARRAY_SIZE(m->channels5);
		m->band5.bitrates = m->rates + 4;
		m->band5.n_bitrates = ARRAY_SIZE(m->rates) - 4;
		m->wiphy->bands[NL80211_BAND_5GHZ] = &m->band5;
	}
	if (mt7932_phy_profile(m->phy_cap)) {
		mt7932_band_capabilities(&m->band2, false);
		mt7932_band_capabilities(&m->band5, true);
	}
	ret = wiphy_register(m->wiphy);
	if (ret) {
		wiphy_free(m->wiphy);
		m->wiphy = NULL;
		return ret;
	}
	m->wiphy_registered = true;
	ret = mt_register_interface(m);
	if (ret)
		return ret;
	WRITE_ONCE(m->interface_registered, true);
	schedule_work(&m->startup_work);
	dev_info(&m->pdev->dev, "REGULATORY_GATE_REGISTERED: cfg80211 country policy required\n");
	return 0;
}
